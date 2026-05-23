#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

#include "simdjson.h"
#include "types.hpp"

using namespace simdjson;

static constexpr uint32_t DEFAULT_K = 4096;
static constexpr uint32_t DEFAULT_SAMPLE = 50000;
static constexpr int DEFAULT_ITERS = 50;

static inline int16_t quantize(double v) {
  if (v < -1.0) v = -1.0;
  if (v > 1.0) v = 1.0;
  return static_cast<int16_t>(std::llround(v * 10000.0));
}

// ── Distance helpers
// ──────────────────────────────────────────────────────────

static float dist_to_centroid(const int16_t* p,
                              const std::array<float, IVF_DIMS>& c) {
  float s = 0;
  for (int d = 0; d < IVF_DIMS; ++d) {
    float diff = float(p[d]) - c[d];
    s += diff * diff;
  }
  return s;
}

static uint32_t nearest_centroid(
    const int16_t* p,
    const std::vector<std::array<float, IVF_DIMS>>& centroids) {
  uint32_t best = 0;
  float best_d = dist_to_centroid(p, centroids[0]);
  for (uint32_t c = 1; c < (uint32_t)centroids.size(); ++c) {
    float d = dist_to_centroid(p, centroids[c]);
    if (d < best_d) {
      best_d = d;
      best = c;
    }
  }
  return best;
}

// ── K-means++ init
// ────────────────────────────────────────────────────────────

static std::vector<std::array<float, IVF_DIMS>> init_kmeans_pp(
    const std::vector<int16_t>& vecs, const std::vector<uint32_t>& sample,
    uint32_t k, uint64_t seed) {
  std::vector<std::array<float, IVF_DIMS>> centroids(k);
  std::vector<float> dmin(sample.size(),
                          std::numeric_limits<float>::infinity());
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<size_t> rand_idx(0, sample.size() - 1);

  const int16_t* fp = vecs.data() + size_t(sample[rand_idx(rng)]) * IVF_DIMS;
  for (int d = 0; d < IVF_DIMS; ++d) centroids[0][d] = float(fp[d]);

  for (uint32_t c = 1; c < k; ++c) {
    const auto& prev = centroids[c - 1];
    double sum = 0;
    for (size_t i = 0; i < sample.size(); ++i) {
      float dist =
          dist_to_centroid(vecs.data() + size_t(sample[i]) * IVF_DIMS, prev);
      if (dist < dmin[i]) dmin[i] = dist;
      sum += dmin[i];
    }
    if (sum <= 0) {
      const int16_t* pp =
          vecs.data() + size_t(sample[rand_idx(rng)]) * IVF_DIMS;
      for (int d = 0; d < IVF_DIMS; ++d) centroids[c][d] = float(pp[d]);
      continue;
    }
    std::uniform_real_distribution<double> wheel(0, sum);
    double target = wheel(rng), acc = 0;
    size_t chosen = sample.size() - 1;
    for (size_t i = 0; i < sample.size(); ++i) {
      acc += dmin[i];
      if (acc >= target) {
        chosen = i;
        break;
      }
    }
    const int16_t* pp = vecs.data() + size_t(sample[chosen]) * IVF_DIMS;
    for (int d = 0; d < IVF_DIMS; ++d) centroids[c][d] = float(pp[d]);
    if ((c & 255u) == 0) std::cerr << "  init " << c << "/" << k << "\n";
  }
  return centroids;
}

// ── K-means training
// ──────────────────────────────────────────────────────────

static void train_kmeans(const std::vector<int16_t>& vecs,
                         const std::vector<uint32_t>& sample,
                         std::vector<std::array<float, IVF_DIMS>>& centroids,
                         int iters) {
  const uint32_t k = (uint32_t)centroids.size();
  std::vector<uint32_t> assign(sample.size(), 0);
  std::mt19937_64 rng(0xC0FFEE);

  for (int iter = 0; iter < iters; ++iter) {
    uint64_t changed = 0;
    for (size_t i = 0; i < sample.size(); ++i) {
      uint32_t c = nearest_centroid(vecs.data() + size_t(sample[i]) * IVF_DIMS,
                                    centroids);
      if (c != assign[i]) {
        ++changed;
        assign[i] = c;
      }
    }

    std::vector<double> sums(size_t(k) * IVF_DIMS, 0.0);
    std::vector<uint32_t> counts(k, 0);
    for (size_t i = 0; i < sample.size(); ++i) {
      uint32_t c = assign[i];
      const int16_t* p = vecs.data() + size_t(sample[i]) * IVF_DIMS;
      ++counts[c];
      double* row = sums.data() + size_t(c) * IVF_DIMS;
      for (int d = 0; d < IVF_DIMS; ++d) row[d] += p[d];
    }

    std::uniform_int_distribution<size_t> pick(0, sample.size() - 1);
    for (uint32_t c = 0; c < k; ++c) {
      if (counts[c] == 0) {
        const int16_t* p = vecs.data() + size_t(sample[pick(rng)]) * IVF_DIMS;
        for (int d = 0; d < IVF_DIMS; ++d) centroids[c][d] = float(p[d]);
      } else {
        double inv = 1.0 / counts[c];
        double* row = sums.data() + size_t(c) * IVF_DIMS;
        for (int d = 0; d < IVF_DIMS; ++d)
          centroids[c][d] = float(row[d] * inv);
      }
    }
    std::cerr << "  iter " << (iter + 1) << "/" << iters
              << " changed=" << changed << "\n";
    if (changed == 0) break;
  }
}

// ── IVF build helper
// ──────────────────────────────────────────────────────

static bool build_ivf(const std::vector<int16_t>& vecs,
                      const std::vector<uint8_t>& labels, uint32_t k,
                      uint32_t sample_sz, int iters, const char* output) {
  const uint32_t n = (uint32_t)labels.size();

  std::mt19937_64 rng(42);
  uint32_t actual_sample = std::min(sample_sz, n);
  std::vector<uint32_t> sample(actual_sample);
  std::uniform_int_distribution<uint32_t> pick_rec(0, n - 1);
  for (auto& s : sample) s = pick_rec(rng);

  std::cerr << "K-means++ init (k=" << k << ", n=" << n
            << ", sample=" << actual_sample << ")...\n";
  auto centroids = init_kmeans_pp(vecs, sample, k, 42);
  std::cerr << "Training...\n";
  train_kmeans(vecs, sample, centroids, iters);

  std::cerr << "Assigning " << n << " vectors...\n";
  std::vector<uint32_t> assignment(n);
  std::vector<uint32_t> counts(k, 0);
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t c =
        nearest_centroid(vecs.data() + size_t(i) * IVF_DIMS, centroids);
    assignment[i] = c;
    ++counts[c];
    if (i % 500000 == 0 && i > 0)
      std::cerr << "  assigned " << i << "/" << n << "\n";
  }

  std::vector<uint32_t> block_offsets(k + 1, 0);
  for (uint32_t c = 0; c < k; ++c)
    block_offsets[c + 1] =
        block_offsets[c] + (counts[c] + IVF_BLOCK - 1) / IVF_BLOCK;
  const uint32_t total_blocks = block_offsets[k];

  std::vector<uint32_t> starts(k + 1, 0);
  for (uint32_t c = 0; c < k; ++c) starts[c + 1] = starts[c] + counts[c];
  std::vector<uint32_t> cursor = starts;
  std::vector<uint32_t> order(n);
  for (uint32_t i = 0; i < n; ++i) order[cursor[assignment[i]]++] = i;

  std::vector<int16_t> qcentroids(size_t(k) * IVF_DIMS);
  for (uint32_t c = 0; c < k; ++c)
    for (int d = 0; d < IVF_DIMS; ++d)
      qcentroids[size_t(c) * IVF_DIMS + d] =
          static_cast<int16_t>(std::llround(centroids[c][d]));

  std::vector<int16_t> bbox_min(size_t(k) * IVF_DIMS,
                                std::numeric_limits<int16_t>::max());
  std::vector<int16_t> bbox_max(size_t(k) * IVF_DIMS,
                                std::numeric_limits<int16_t>::min());
  std::vector<uint8_t> out_labels(size_t(total_blocks) * IVF_BLOCK, 0);
  std::vector<int16_t> blocks_data(size_t(total_blocks) * IVF_DIMS * IVF_BLOCK,
                                   0);

  for (uint32_t c = 0; c < k; ++c) {
    if (counts[c] == 0) {
      for (int d = 0; d < IVF_DIMS; ++d) {
        bbox_min[size_t(c) * IVF_DIMS + d] = 0;
        bbox_max[size_t(c) * IVF_DIMS + d] = 0;
      }
      continue;
    }
    for (uint32_t pos = 0; pos < counts[c]; ++pos) {
      const uint32_t orig = order[starts[c] + pos];
      const uint32_t block = block_offsets[c] + pos / IVF_BLOCK;
      const uint32_t lane = pos % IVF_BLOCK;

      out_labels[size_t(block) * IVF_BLOCK + lane] = labels[orig];

      const int16_t* src = vecs.data() + size_t(orig) * IVF_DIMS;
      int16_t* dst = blocks_data.data() + size_t(block) * IVF_DIMS * IVF_BLOCK;
      for (int d = 0; d < IVF_DIMS; ++d) {
        int16_t v = src[d];
        dst[block_pair_offset(d, (int)lane)] = v;
        int16_t& mn = bbox_min[size_t(c) * IVF_DIMS + d];
        int16_t& mx = bbox_max[size_t(c) * IVF_DIMS + d];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
      }
    }
  }

  IndexHeader hdr{};
  hdr.magic = IVF_MAGIC;
  hdr.version = IVF_VERSION;
  hdr.n = n;
  hdr.k = k;
  hdr.total_blocks = total_blocks;
  hdr.block_size = IVF_BLOCK;
  hdr.dims = IVF_DIMS;

  const IndexLayout layout = layout_for(k, total_blocks);

  std::ofstream out(output, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::cerr << "Cannot write " << output << "\n";
    return false;
  }
  out.seekp((std::streamoff)(layout.total - 1));
  out.put('\0');

  auto write_at = [&](size_t off, const void* data, size_t len) {
    out.seekp((std::streamoff)off);
    out.write(static_cast<const char*>(data), (std::streamsize)len);
  };

  write_at(0, &hdr, sizeof(hdr));
  write_at(layout.centroids, qcentroids.data(),
           qcentroids.size() * sizeof(int16_t));
  write_at(layout.bbox_min, bbox_min.data(), bbox_min.size() * sizeof(int16_t));
  write_at(layout.bbox_max, bbox_max.data(), bbox_max.size() * sizeof(int16_t));
  write_at(layout.offsets, block_offsets.data(),
           block_offsets.size() * sizeof(uint32_t));
  write_at(layout.counts, counts.data(), counts.size() * sizeof(uint32_t));
  write_at(layout.labels, out_labels.data(), out_labels.size());
  write_at(layout.blocks, blocks_data.data(),
           blocks_data.size() * sizeof(int16_t));

  std::cerr << "Written " << output << " (" << (layout.total / (1024 * 1024))
            << " MB)\n";
  return true;
}

// ── main
// ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <input.json> <output.bin>"
              << " [k=" << DEFAULT_K << "]"
              << " [sample=" << DEFAULT_SAMPLE << "]"
              << " [iters=" << DEFAULT_ITERS << "]"
              << " [ec_output.bin] [ec_k=256]\n";
    return 1;
  }
  const char* input = argv[1];
  const char* output = argv[2];
  uint32_t k = argc > 3 ? (uint32_t)std::stoul(argv[3]) : DEFAULT_K;
  uint32_t sample_sz =
      argc > 4 ? (uint32_t)std::stoul(argv[4]) : DEFAULT_SAMPLE;
  int iters = argc > 5 ? std::stoi(argv[5]) : DEFAULT_ITERS;
  const char* ec_output = argc > 6 ? argv[6] : nullptr;
  uint32_t ec_k = argc > 7 ? (uint32_t)std::stoul(argv[7]) : 256;

  // ── Parse references ──────────────────────────────────────────────────────
  std::cerr << "Parsing " << input << "...\n";
  ondemand::parser parser;
  padded_string json;
  if (padded_string::load(input).get(json)) {
    std::cerr << "load failed\n";
    return 1;
  }
  ondemand::document doc;
  if (parser.iterate(json).get(doc)) {
    std::cerr << "parse failed\n";
    return 1;
  }

  std::vector<int16_t> vecs;
  std::vector<uint8_t> labels;
  vecs.reserve(size_t(3'000'000) * IVF_DIMS);
  labels.reserve(3'000'000);

  for (ondemand::object obj : doc.get_array()) {
    int i = 0;
    for (double v : obj["vector"].get_array()) {
      if (i < IVF_DIMS) vecs.push_back(quantize(v));
      ++i;
    }
    std::string_view label;
    if (obj["label"].get_string().get(label)) {
      vecs.resize(vecs.size() - IVF_DIMS);
      continue;
    }
    labels.push_back(label == "fraud" ? 1u : 0u);
  }
  const uint32_t n = (uint32_t)labels.size();
  std::cerr << "Loaded " << n << " records\n";

  // ── Build main index ──────────────────────────────────────────────────────
  if (!build_ivf(vecs, labels, k, sample_sz, iters, output)) return 1;

  // ── Build edge-case index (unknown merchant + high mcc_risk) ─────────────
  if (ec_output) {
    std::cerr << "Filtering edge cases (vec[11]==10000 && vec[12]>=7500)...\n";
    std::vector<int16_t> ec_vecs;
    std::vector<uint8_t> ec_labels;
    ec_vecs.reserve(size_t(1'000'000) * IVF_DIMS);
    ec_labels.reserve(1'000'000);
    for (uint32_t i = 0; i < n; ++i) {
      const int16_t* v = vecs.data() + size_t(i) * IVF_DIMS;
      if (v[11] == 10000 && v[12] >= 7500) {
        ec_vecs.insert(ec_vecs.end(), v, v + IVF_DIMS);
        ec_labels.push_back(labels[i]);
      }
    }
    std::cerr << "Edge cases: " << ec_labels.size() << "\n";
    if (!build_ivf(ec_vecs, ec_labels, ec_k, sample_sz, iters, ec_output))
      return 1;
  }

  return 0;
}
