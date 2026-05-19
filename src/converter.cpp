#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include "simdjson.h"
#include "types.hpp"

using namespace simdjson;

static constexpr int LEAF_SIZE = 256; // vectors per leaf (= 16 Block8 blocks)

// ─────────────────────────────────────────────────────────────────────────────
// Build state
// ─────────────────────────────────────────────────────────────────────────────
static QuantParams              qp;
static std::vector<BallNode>    nodes;
static std::vector<Block8>      blocks;

static Block8 make_block(const VectorRecord* recs, int count) {
    Block8 b;
    memset(&b, 0, sizeof(b));
    for (int j = 0; j < 8; j++) {
        int idx = (j < count) ? j : (count - 1);
        for (int d = 0; d < 14; d++) {
            double q = (double)(recs[idx].dimensions[d] - qp.min_global)
                       / (double)qp.range_global * 65535.0;
            b.dims[d][j] = (uint16_t)std::clamp(std::round(q), 0.0, 65535.0);
        }
        b.is_fraud[j] = (recs[idx].dimensions[15] > 0.5f) ? 1u : 0u;
    }
    b.n_valid = (uint8_t)count;
    return b;
}

// Recursive ball-tree construction.
// Returns the index of the newly created node in `nodes`.
static int build(std::vector<VectorRecord>& data, int lo, int hi) {
    int node_idx = (int)nodes.size();
    nodes.push_back({});

    int count = hi - lo;

    // ── Compute ball center (mean of all points) ──────────────────────────
    float center[14] = {};
    for (int i = lo; i < hi; i++)
        for (int d = 0; d < 14; d++)
            center[d] += data[i].dimensions[d];
    for (int d = 0; d < 14; d++) {
        center[d] /= (float)count;
        nodes[node_idx].center[d] = center[d];
    }

    // ── Compute bounding radius ───────────────────────────────────────────
    float max_r2 = 0.0f;
    for (int i = lo; i < hi; i++) {
        float r2 = 0;
        for (int d = 0; d < 14; d++) {
            float diff = data[i].dimensions[d] - center[d];
            r2 += diff * diff;
        }
        if (r2 > max_r2) max_r2 = r2;
    }
    nodes[node_idx].radius = std::sqrt(max_r2);

    // ── Leaf ──────────────────────────────────────────────────────────────
    if (count <= LEAF_SIZE) {
        nodes[node_idx].is_leaf    = 1;
        nodes[node_idx].left       = -1;
        nodes[node_idx].right      = -1;
        nodes[node_idx].leaf_start = (int32_t)blocks.size();
        nodes[node_idx].leaf_count = 0;

        for (int bi = 0; bi < count; bi += 8) {
            int n8 = std::min(8, count - bi);
            blocks.push_back(make_block(&data[lo + bi], n8));
            nodes[node_idx].leaf_count++;
        }
        return node_idx;
    }

    // ── Internal: split along the dimension with greatest spread ─────────
    int   best_dim    = 0;
    float best_spread = -1.0f;
    for (int d = 0; d < 14; d++) {
        float mn = 1e18f, mx = -1e18f;
        for (int i = lo; i < hi; i++) {
            float v = data[i].dimensions[d];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        float s = mx - mn;
        if (s > best_spread) { best_spread = s; best_dim = d; }
    }

    int mid = lo + count / 2;
    std::nth_element(data.begin() + lo, data.begin() + mid, data.begin() + hi,
        [best_dim](const VectorRecord& a, const VectorRecord& b) {
            return a.dimensions[best_dim] < b.dimensions[best_dim];
        });

    nodes[node_idx].is_leaf    = 0;
    nodes[node_idx].leaf_start = 0;
    nodes[node_idx].leaf_count = 0;

    // Recurse — access nodes[node_idx] by index after each push_back
    int left  = build(data, lo,  mid);
    int right = build(data, mid, hi);

    nodes[node_idx].left  = left;
    nodes[node_idx].right = right;

    return node_idx;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <input.json> <output.bin>\n";
        return 1;
    }

    // ── Load references JSON ──────────────────────────────────────────────
    ondemand::parser parser;
    padded_string    json;
    if (padded_string::load(argv[1]).get(json)) return 1;

    ondemand::document doc;
    if (parser.iterate(json).get(doc)) return 1;

    std::vector<VectorRecord> records;
    records.reserve(3'000'000);

    float g_min =  1e18f, g_max = -1e18f;

    for (ondemand::object obj : doc.get_array()) {
        VectorRecord rec;
        memset(&rec, 0, sizeof(rec));
        int i = 0;
        for (double v : obj["vector"].get_array()) {
            if (i < 14) {
                rec.dimensions[i] = (float)v;
                if ((float)v < g_min) g_min = (float)v;
                if ((float)v > g_max) g_max = (float)v;
            }
            i++;
        }
        std::string_view label;
        if (obj["label"].get_string().get(label)) continue;
        rec.dimensions[15] = (label == "fraud") ? 1.0f : 0.0f;
        records.push_back(rec);
    }

    std::cerr << "Loaded " << records.size() << " records\n";

    // ── Quantisation parameters ───────────────────────────────────────────
    qp.min_global   = g_min;
    qp.range_global = (g_max - g_min < 1e-9f) ? 1.0f : (g_max - g_min);
    float global_scale_sq = (qp.range_global / 65535.0f) * (qp.range_global / 65535.0f);

    // ── Build ball tree ───────────────────────────────────────────────────
    int estimated_leaves   = ((int)records.size() + LEAF_SIZE - 1) / LEAF_SIZE;
    int estimated_nodes    = estimated_leaves * 2 + 16;
    int estimated_blocks   = ((int)records.size() + 7) / 8;
    nodes.reserve(estimated_nodes);
    blocks.reserve(estimated_blocks);

    std::cerr << "Building ball tree (LEAF_SIZE=" << LEAF_SIZE << ")...\n";
    build(records, 0, (int)records.size());
    std::cerr << "  nodes=" << nodes.size() << "  blocks=" << blocks.size() << "\n";

    // ── Write binary ──────────────────────────────────────────────────────
    BallTreeHeader hdr   = {};
    hdr.magic            = 0xBA11BEEF;
    hdr.num_nodes        = (uint32_t)nodes.size();
    hdr.num_blocks       = (uint32_t)blocks.size();
    hdr.qp_min_global    = qp.min_global;
    hdr.qp_range_global  = qp.range_global;
    hdr.global_scale_sq  = global_scale_sq;

    std::ofstream out(argv[2], std::ios::binary);
    if (!out) { std::cerr << "Cannot write " << argv[2] << "\n"; return 1; }

    out.write((const char*)&hdr,          sizeof(hdr));
    out.write((const char*)nodes.data(),  nodes.size()  * sizeof(BallNode));
    out.write((const char*)blocks.data(), blocks.size() * sizeof(Block8));

    std::cerr << "Written " << argv[2] << "\n";
    return 0;
}
