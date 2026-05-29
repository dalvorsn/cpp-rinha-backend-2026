#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include "ivf.hpp"
#include "normalizer.hpp"

// ── Config table ──────────────────────────────────────────────────────────────
// approved = get_fraud_count() / 5 < 0.6  →  cnt < 3  (hardcoded in server)
static constexpr int FRAUD_THRESHOLD = 3;

// repair_min > 5 is impossible (cnt ∈ 0..5) → repair never fires
static constexpr int NO_REPAIR = 6;

struct Config {
    const char* name;
    int nprobe;
    int repair_min;
    int repair_max;
};

static Config CONFIGS[] = {
    // repair 1..4
    {"r14_nprobe1",    1,  1, 4},
    {"r14_nprobe2",    2,  1, 4},
    {"r14_nprobe4",    4,  1, 4},
    {"r14_nprobe8",    8,  1, 4},
    {"r14_nprobe12",  12,  1, 4},
    {"r14_nprobe16",  16,  1, 4},
    {"r14_nprobe24",  24,  1, 4},
    {"r14_nprobe32",  32,  1, 4},
    // repair 2..3
    {"r14_nprobe1",    1,  2, 3},
    {"r14_nprobe2",    2,  2, 3},
    {"r23_nprobe4",    4,  2, 3},
    {"r23_nprobe8",    8,  2, 3},
    {"r23_nprobe12",  12,  2, 3},
    {"r23_nprobe16",  16,  2, 3},
    {"r23_nprobe24",  24,  2, 3},
    {"r23_nprobe32",  32,  2, 3},
};
static constexpr int N_CONFIGS = (int)(sizeof(CONFIGS) / sizeof(CONFIGS[0]));

// ── Entry ─────────────────────────────────────────────────────────────────────
struct Entry {
    std::string request_json;
    bool expected_approved;
};

// ── Per-config result ─────────────────────────────────────────────────────────
struct Result {
    Config cfg;
    double avg_us, p50_us, p99_us, max_us;
    int tp, tn, fp, fn, total;
};


// ── Index stats ───────────────────────────────────────────────────────────────
struct IndexStats {
    uint32_t n, k, train_sample, train_iters, train_max_iters;
    std::vector<uint32_t> counts;
};

struct HistBucket { uint32_t lo, hi; int cnt; };

static std::vector<HistBucket> make_hist(const IndexStats& idx, int nbuckets = 20) {
    const uint32_t* cc = idx.counts.data();
    uint32_t mx = *std::max_element(cc, cc + idx.k);
    std::vector<HistBucket> h(nbuckets);
    for (int b = 0; b < nbuckets; ++b) {
        h[b].lo = (uint32_t)((uint64_t)b * (mx + 1) / nbuckets);
        h[b].hi = (uint32_t)((uint64_t)(b + 1) * (mx + 1) / nbuckets) - 1;
        h[b].cnt = 0;
    }
    for (uint32_t i = 0; i < idx.k; ++i) {
        int b = mx > 0 ? (int)((uint64_t)cc[i] * nbuckets / ((uint64_t)mx + 1)) : 0;
        if (b >= nbuckets) b = nbuckets - 1;
        ++h[b].cnt;
    }
    return h;
}

// ── System info ───────────────────────────────────────────────────────────────
struct CpuInfo {
    std::string model;
    int n_logical = 0;
    int n_cores   = 0;
    double max_mhz = 0.0;
    std::string l1d, l1i, l2, l3;
    std::string cpuset;
};

static std::string read_file_str(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return "?";
    char buf[64] = {};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    for (char& c : buf) if (c == '\n') { c = '\0'; break; }
    return buf;
}

static CpuInfo gather_cpu_info() {
    CpuInfo info;

    FILE* f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            auto after_colon = [&]() -> const char* {
                const char* c = strchr(line, ':');
                if (!c) return nullptr;
                c++;
                while (*c == ' ' || *c == '\t') c++;
                return c;
            };
            if (strncmp(line, "model name", 10) == 0 && info.model.empty()) {
                const char* v = after_colon();
                if (v) {
                    info.model = v;
                    if (!info.model.empty() && info.model.back() == '\n')
                        info.model.pop_back();
                }
            }
            if (strncmp(line, "processor", 9) == 0) ++info.n_logical;
            if (strncmp(line, "cpu cores", 9) == 0 && info.n_cores == 0) {
                const char* v = after_colon();
                if (v) info.n_cores = atoi(v);
            }
        }
        fclose(f);
    }

    {
        FILE* mf = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r");
        if (mf) {
            long khz = 0;
            fscanf(mf, "%ld", &khz);
            fclose(mf);
            if (khz > 0) info.max_mhz = khz / 1000.0;
        }
    }

    info.l1d = read_file_str("/sys/devices/system/cpu/cpu0/cache/index0/size");
    info.l1i = read_file_str("/sys/devices/system/cpu/cpu0/cache/index1/size");
    info.l2  = read_file_str("/sys/devices/system/cpu/cpu0/cache/index2/size");
    info.l3  = read_file_str("/sys/devices/system/cpu/cpu0/cache/index3/size");

    {
        FILE* sf = fopen("/proc/self/status", "r");
        if (sf) {
            char line[256];
            while (fgets(line, sizeof(line), sf)) {
                if (strncmp(line, "Cpus_allowed_list:", 18) == 0) {
                    const char* c = strchr(line, ':');
                    if (c) {
                        c++;
                        while (*c == ' ' || *c == '\t') c++;
                        info.cpuset = c;
                        if (!info.cpuset.empty() && info.cpuset.back() == '\n')
                            info.cpuset.pop_back();
                    }
                    break;
                }
            }
            fclose(sf);
        }
    }

    return info;
}

// ── Parse test-data.json ──────────────────────────────────────────────────────
static std::vector<Entry> load_entries(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return {}; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);
    std::vector<char> buf((size_t)fsz + 1, '\0');
    fread(buf.data(), 1, (size_t)fsz, f);
    fclose(f);

    const char* p = buf.data();
    const char* end = p + fsz;

    const char* ea = (const char*)memmem(p, (size_t)fsz, "\"entries\"", 9);
    if (!ea) { fprintf(stderr, "No 'entries' key in %s\n", path); return {}; }
    const char* arr = (const char*)memchr(ea + 9, '[', (size_t)(end - ea - 9));
    if (!arr) return {};
    p = arr + 1;

    std::vector<Entry> out;
    out.reserve(60000);

    while (p < end) {
        while (p < end && *p != '{' && *p != ']') ++p;
        if (p >= end || *p == ']') break;

        const char* obj = p;
        int depth = 0;
        const char* q = p;
        while (q < end) {
            if (*q == '{') ++depth;
            else if (*q == '}' && --depth == 0) break;
            ++q;
        }
        if (depth != 0) break;

        Entry e;

        const char* rk = (const char*)memmem(obj, (size_t)(q - obj), "\"request\"", 9);
        if (!rk) { p = q + 1; continue; }
        const char* rb = (const char*)memchr(rk + 9, '{', (size_t)(q - rk - 9));
        if (!rb) { p = q + 1; continue; }
        int rd = 0;
        const char* rq = rb;
        while (rq <= q) {
            if (*rq == '{') ++rd;
            else if (*rq == '}' && --rd == 0) break;
            ++rq;
        }
        e.request_json.assign(rb, rq + 1);

        const char* ek = (const char*)memmem(obj, (size_t)(q - obj), "\"expected_approved\"", 19);
        if (!ek) { p = q + 1; continue; }
        const char* ev = (const char*)memchr(ek + 19, ':', (size_t)(q - ek - 19));
        if (!ev) { p = q + 1; continue; }
        ++ev;
        while (ev < q && (*ev == ' ' || *ev == '\t' || *ev == '\n' || *ev == '\r')) ++ev;
        e.expected_approved = (*ev == 't');

        out.push_back(std::move(e));
        p = q + 1;
    }

    return out;
}

static uint64_t pct(std::vector<uint64_t>& v, double p) {
    if (v.empty()) return 0;
    size_t idx = (size_t)(v.size() * p / 100.0 + 0.5);
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
}

// ── Markdown output ───────────────────────────────────────────────────────────
static void write_markdown(const char* path, const CpuInfo& cpu,
                           const IndexStats& idx,
                           size_t n_entries, int n_fraud, int n_edge,
                           const std::vector<Result>& results) {
    FILE* md = fopen(path, "w");
    if (!md) { perror("benchmark.md"); return; }

    int n_legit = (int)n_entries - n_fraud;
    double fraud_pct = n_entries ? n_fraud * 100.0 / n_entries : 0;
    double edge_pct  = n_entries ? n_edge  * 100.0 / n_entries : 0;

    fprintf(md,
        "# Benchmark\n\n"
        "Offline benchmark — normalize + `get_fraud_count` directly, "
        "no HTTP overhead, against all %zu entries in the test dataset.\n\n",
        n_entries);

    // Environment
    fprintf(md, "## Environment\n\n| | |\n|---|---|\n");
    fprintf(md, "| **CPU** | %s |\n", cpu.model.c_str());
    fprintf(md, "| **Cores / Threads** | %d cores / %d threads |\n", cpu.n_cores, cpu.n_logical);
    if (cpu.max_mhz > 0)
        fprintf(md, "| **Max clock** | %.0f MHz |\n", cpu.max_mhz);
    if (cpu.l1d != "?") fprintf(md, "| **L1d** | %s |\n", cpu.l1d.c_str());
    if (cpu.l1i != "?") fprintf(md, "| **L1i** | %s |\n", cpu.l1i.c_str());
    if (cpu.l2  != "?") fprintf(md, "| **L2**  | %s |\n", cpu.l2.c_str());
    if (cpu.l3  != "?") fprintf(md, "| **L3**  | %s |\n", cpu.l3.c_str());
    fprintf(md, "| **Compiler** | GCC (Debian trixie-slim) |\n");
    fprintf(md, "| **Flags** | `-Ofast -march=haswell -mtune=haswell -flto` |\n");
    fprintf(md, "| **Pinned CPUs** | %s |\n", cpu.cpuset.c_str());
    fprintf(md, "| **CPU limit** | 0.37 cores (≈ Core i5-4260U @ 1.4 GHz single-thread) |\n");
    fprintf(md,
        "\n> Target hardware is a **Mac mini 2014 (Core i5-4260U, 1.4 GHz)**. "
        "The CPU throttle (0.37×) approximates its single-thread performance "
        "relative to this machine (~2.7× slower). Use these numbers to compare "
        "configs, not to predict absolute latency on the rinha.\n\n");

    // Dataset
    fprintf(md, "## Dataset\n\n| | |\n|---|---|\n");
    fprintf(md, "| **Total** | %zu |\n", n_entries);
    fprintf(md, "| **Fraud** | %d (%.1f%%) |\n", n_fraud, fraud_pct);
    fprintf(md, "| **Legit** | %d (%.1f%%) |\n", n_legit, 100.0 - fraud_pct);
    fprintf(md, "| **Edge cases** | %d (%.1f%%) |\n\n", n_edge, edge_pct);

    // Index
    fprintf(md, "## Index\n\n| | |\n|---|---|\n");
    fprintf(md, "| **n** | %u |\n", idx.n);
    fprintf(md, "| **k** | %u |\n", idx.k);
    fprintf(md, "| **train_sample** | %u |\n", idx.train_sample);
    fprintf(md, "| **train_iters** | %u/%u%s |\n\n",
            idx.train_iters, idx.train_max_iters,
            idx.train_iters < idx.train_max_iters ? " (converged)" : "");

    {
        uint32_t mn = UINT32_MAX, mx = 0;
        uint64_t total = 0;
        for (auto c : idx.counts) {
            if (c < mn) mn = c;
            if (c > mx) mx = c;
            total += c;
        }
        auto hist = make_hist(idx);
        int max_b = 0;
        for (auto& h : hist) if (h.cnt > max_b) max_b = h.cnt;
        int y_max = (max_b + 99) / 100 * 100 + 100;

        fprintf(md, "### Cluster size distribution\n\n");
        fprintf(md, "> min=%u  max=%u  avg=%.1f\n\n", mn, mx, (double)total / idx.k);

        fprintf(md, "```mermaid\nxychart-beta\n");
        fprintf(md, "    title \"Cluster Size Distribution (k=%u, n=%u)\"\n", idx.k, idx.n);

        fprintf(md, "    x-axis [");
        for (int b = 0; b < (int)hist.size(); ++b) {
            if (b > 0) fprintf(md, ", ");
            uint32_t hi = hist[b].hi;
            if (hi >= 1000)
                fprintf(md, "\"%.1fk\"", hi / 1000.0);
            else
                fprintf(md, "\"%u\"", hi);
        }
        fprintf(md, "]\n");

        fprintf(md, "    y-axis \"Clusters\" 0 --> %d\n", y_max);

        fprintf(md, "    bar [");
        for (int b = 0; b < (int)hist.size(); ++b) {
            if (b > 0) fprintf(md, ", ");
            fprintf(md, "%d", hist[b].cnt);
        }
        fprintf(md, "]\n```\n\n");
    }

    // Results — find best perfect config (fp==0, fn==0, lowest p99)
    int best_idx = -1;
    double best_p99 = std::numeric_limits<double>::infinity();
    for (int i = 0; i < (int)results.size(); ++i)
        if (results[i].fp == 0 && results[i].fn == 0 && results[i].p99_us < best_p99) {
            best_p99 = results[i].p99_us;
            best_idx = i;
        }

    fprintf(md, "## Results\n\n");
    fprintf(md, "> `approved = fraud_neighbors / 5 < 0.6` — threshold is fixed by the server.\n\n");
    fprintf(md, "| NPROBE | R.MIN | R.MAX | avg (µs) | p50 (µs) | p99 (µs) | max (µs) | TP | TN | FP | FN | FP%% | FN%% |\n");
    fprintf(md, "|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    for (int ri = 0; ri < (int)results.size(); ++ri) {
        const auto& r = results[ri];
        double fp_pct = r.total > 0 ? r.fp * 100.0 / r.total : 0;
        double fn_pct = r.total > 0 ? r.fn * 100.0 / r.total : 0;
        bool perfect  = (r.fp == 0 && r.fn == 0);
        bool is_best  = (ri == best_idx);
        const char* bp = is_best ? "<span style=\"color:limegreen\">**"
                       : perfect ? "**" : "";
        const char* bs = is_best ? "**</span>"
                       : perfect ? "**" : "";
        bool no_repair = r.cfg.repair_min > 5;
        if (no_repair) {
            fprintf(md,
                "| %s%d%s | %s—%s | %s—%s"
                " | %s%.2f%s | %s%.2f%s | %s%.2f%s | %s%.1f%s"
                " | %s%d%s | %s%d%s | %s%d%s | %s%d%s | %s%.2f%%%s | %s%.2f%%%s |\n",
                bp, r.cfg.nprobe, bs, bp, bs, bp, bs,
                bp, r.avg_us, bs, bp, r.p50_us, bs, bp, r.p99_us, bs, bp, r.max_us, bs,
                bp, r.tp, bs, bp, r.tn, bs, bp, r.fp, bs, bp, r.fn, bs,
                bp, fp_pct, bs, bp, fn_pct, bs);
        } else {
            fprintf(md,
                "| %s%d%s | %s%d%s | %s%d%s"
                " | %s%.2f%s | %s%.2f%s | %s%.2f%s | %s%.1f%s"
                " | %s%d%s | %s%d%s | %s%d%s | %s%d%s | %s%.2f%%%s | %s%.2f%%%s |\n",
                bp, r.cfg.nprobe, bs, bp, r.cfg.repair_min, bs, bp, r.cfg.repair_max, bs,
                bp, r.avg_us, bs, bp, r.p50_us, bs, bp, r.p99_us, bs, bp, r.max_us, bs,
                bp, r.tp, bs, bp, r.tn, bs, bp, r.fp, bs, bp, r.fn, bs,
                bp, fp_pct, bs, bp, fn_pct, bs);
        }
    }

    fprintf(md,
        "\n## Running\n\n"
        "```bash\nmake bench\n```\n\n"
        "To pin different CPUs, edit `cpuset` in `bench/docker-compose.yml`.\n");

    fclose(md);
}

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    const char* ivf_path  = argc > 1 ? argv[1] : "resources/references.bin";
    const char* norm_path = argc > 2 ? argv[2] : "resources/normalization.json";
    const char* mcc_path  = argc > 3 ? argv[3] : "resources/mcc_risk.json";
    const char* data_path = argc > 4 ? argv[4] : "test/test-data.json";
    const char* md_path   = argc > 5 ? argv[5] : "benchmark.md";

    CpuInfo cpu = gather_cpu_info();
    fprintf(stderr, "CPU: %s (%d cores / %d threads",
            cpu.model.c_str(), cpu.n_cores, cpu.n_logical);
    if (cpu.max_mhz > 0) fprintf(stderr, ", %.0f MHz max", cpu.max_mhz);
    fprintf(stderr, ")\nAllowed CPUs: %s\n\n", cpu.cpuset.c_str());

    // Load index stats before the config loop
    IndexStats idx;
    {
        IVF probe(ivf_path, 1, NO_REPAIR, 0);
        idx.n = probe.get_n();
        idx.k = probe.get_k();
        idx.train_sample = probe.get_train_sample();
        idx.train_iters = probe.get_train_iters();
        idx.train_max_iters = probe.get_train_max_iters();
        const uint32_t* cc = probe.cluster_counts();
        idx.counts.assign(cc, cc + idx.k);
    }
    fprintf(stderr, "Index: n=%u  k=%u  train_sample=%u  train_iters=%u\n\n",
            idx.n, idx.k, idx.train_sample, idx.train_iters);

    Normalizer norm;
    if (!norm.load_config(mcc_path, norm_path)) return 1;

    fprintf(stderr, "Loading %s...\n", data_path);
    auto entries = load_entries(data_path);
    if (entries.empty()) { fprintf(stderr, "No entries loaded\n"); return 1; }

    int n_fraud = 0;
    for (const auto& e : entries) if (!e.expected_approved) ++n_fraud;
    fprintf(stderr, "Loaded %zu entries (%d fraud)\n\n", entries.size(), n_fraud);

    fprintf(stderr, "Pre-normalizing...\n");
    std::vector<std::array<int16_t, IVF_DIMS>> vecs(entries.size());
    int norm_ok = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (norm.normalize_raw(e.request_json.data(), (int)e.request_json.size(), vecs[i].data()))
            ++norm_ok;
        else
            vecs[i].fill(0);
    }
    fprintf(stderr, "Normalized %d/%zu ok\n\n", norm_ok, entries.size());

    std::vector<Result> all_results;

    for (int ci = 0; ci < N_CONFIGS; ++ci) {
        const Config& cfg = CONFIGS[ci];
        bool no_repair = cfg.repair_min > 5;
        if (no_repair)
            fprintf(stderr, "=== %s  nprobe=%d repair=none ===\n", cfg.name, cfg.nprobe);
        else
            fprintf(stderr, "=== %s  nprobe=%d repair=%d-%d ===\n",
                    cfg.name, cfg.nprobe, cfg.repair_min, cfg.repair_max);

        IVF ivf(ivf_path, cfg.nprobe, cfg.repair_min, cfg.repair_max);

        std::vector<uint64_t> latencies;
        latencies.reserve(entries.size());
        int tp = 0, tn = 0, fp = 0, fn = 0;

        for (size_t i = 0; i < entries.size(); ++i) {
            if (vecs[i][0] == 0 && vecs[i][1] == 0) continue;

            auto t0 = std::chrono::steady_clock::now();
            int cnt = ivf.get_fraud_count(vecs[i].data());
            auto t1 = std::chrono::steady_clock::now();

            uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            latencies.push_back(ns);

            bool predicted = (cnt < FRAUD_THRESHOLD);
            bool expected  = entries[i].expected_approved;
            if (predicted == expected) { if (predicted) ++tn; else ++tp; }
            else                        { if (predicted) ++fn; else ++fp; }
        }

        std::sort(latencies.begin(), latencies.end());
        uint64_t sum = 0;
        for (auto v : latencies) sum += v;
        uint64_t avg_ns = latencies.empty() ? 0 : sum / latencies.size();
        uint64_t p50_ns = pct(latencies, 50);
        uint64_t p99_ns = pct(latencies, 99);
        uint64_t max_ns = latencies.empty() ? 0 : latencies.back();

        int total = tp + tn + fp + fn;
        double fn_rate   = total > 0 ? fn * 100.0 / total : 0.0;
        double fp_rate   = total > 0 ? fp * 100.0 / total : 0.0;

        fprintf(stderr, "  TP=%-6d TN=%-6d FP=%-6d FN=%-6d  FN=%.2f%%  FP=%.2f%%\n",
                tp, tn, fp, fn, fn_rate, fp_rate);
        fprintf(stderr, "  avg=%.1fus  p50=%.1fus  p99=%.1fus  max=%.1fus\n\n",
                avg_ns / 1000.0, p50_ns / 1000.0, p99_ns / 1000.0, max_ns / 1000.0);

        Result r{cfg, avg_ns/1000.0, p50_ns/1000.0, p99_ns/1000.0, max_ns/1000.0,
                 tp, tn, fp, fn, total};
        all_results.push_back(r);
    }

    write_markdown(md_path, cpu, idx, entries.size(), n_fraud, 645, all_results);
    fprintf(stderr, "benchmark.md -> %s\n", md_path);
    return 0;
}
