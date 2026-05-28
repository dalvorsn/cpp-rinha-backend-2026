#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

#include "types.hpp"

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

static float dist_to_centroid(const int16_t *p,
                              const std::array<float, IVF_DIMS> &c) {
  float s = 0;
  for (int d = 0; d < IVF_DIMS; ++d) {
    float diff = float(p[d]) - c[d];
    s += diff * diff;
  }
  return s;
}

static uint32_t nearest_centroid(
    const int16_t *p,
    const std::vector<std::array<float, IVF_DIMS>> &centroids) {
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

static std::pair<uint32_t, uint32_t> nearest2_centroid(
    const int16_t *p,
    const std::vector<std::array<float, IVF_DIMS>> &centroids) {
  const uint32_t k = (uint32_t)centroids.size();
  if (k == 1) return {0, 0};
  uint32_t best = 0, second = 1;
  float best_d = dist_to_centroid(p, centroids[0]);
  float second_d = dist_to_centroid(p, centroids[1]);
  if (second_d < best_d) {
    std::swap(best, second);
    std::swap(best_d, second_d);
  }
  for (uint32_t c = 2; c < k; ++c) {
    float d = dist_to_centroid(p, centroids[c]);
    if (d < best_d) {
      second = best;
      second_d = best_d;
      best = c;
      best_d = d;
    } else if (d < second_d) {
      second = c;
      second_d = d;
    }
  }
  return {best, second};
}

static void print_histogram(const std::vector<uint32_t> &counts) {
  const uint32_t k = (uint32_t)counts.size();
  uint32_t mn = UINT32_MAX, mx = 0;
  uint64_t total = 0;
  for (auto c : counts) {
    if (c < mn) mn = c;
    if (c > mx) mx = c;
    total += c;
  }
  double avg = (double)total / k;
  fprintf(stderr, "Cluster size: min=%u max=%u avg=%.1f (k=%u n=%llu)\n",
          mn, mx, avg, k, (unsigned long long)total);

  static constexpr int NBUCKETS = 20;
  static constexpr int BAR_W = 40;
  int buckets[NBUCKETS] = {};
  for (auto c : counts) {
    int b = mx > 0 ? (int)((uint64_t)c * NBUCKETS / ((uint64_t)mx + 1)) : 0;
    if (b >= NBUCKETS) b = NBUCKETS - 1;
    ++buckets[b];
  }
  int max_b = *std::max_element(buckets, buckets + NBUCKETS);
  for (int b = 0; b < NBUCKETS; ++b) {
    uint32_t lo = (uint32_t)((uint64_t)b * (mx + 1) / NBUCKETS);
    uint32_t hi = (uint32_t)((uint64_t)(b + 1) * (mx + 1) / NBUCKETS) - 1;
    int bar = max_b > 0 ? buckets[b] * BAR_W / max_b : 0;
    char bar_str[BAR_W + 1];
    memset(bar_str, '#', bar);
    memset(bar_str + bar, ' ', BAR_W - bar);
    bar_str[BAR_W] = '\0';
    fprintf(stderr, "  %5u-%5u | %s %d\n", lo, hi, bar_str, buckets[b]);
  }
  fprintf(stderr, "\n");
}

// ── K-means++ init
// ────────────────────────────────────────────────────────────

static std::vector<std::array<float, IVF_DIMS>> init_kmeans_pp(
    const std::vector<int16_t> &vecs, const std::vector<uint32_t> &sample,
    uint32_t k, uint64_t seed) {
  std::vector<std::array<float, IVF_DIMS>> centroids(k);
  std::vector<float> dmin(sample.size(),
                          std::numeric_limits<float>::infinity());
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<size_t> rand_idx(0, sample.size() - 1);

  const int16_t *fp = vecs.data() + size_t(sample[rand_idx(rng)]) * IVF_DIMS;
  for (int d = 0; d < IVF_DIMS; ++d) centroids[0][d] = float(fp[d]);

  for (uint32_t c = 1; c < k; ++c) {
    const auto &prev = centroids[c - 1];
    double sum = 0;
    for (size_t i = 0; i < sample.size(); ++i) {
      float dist =
          dist_to_centroid(vecs.data() + size_t(sample[i]) * IVF_DIMS, prev);
      if (dist < dmin[i]) dmin[i] = dist;
      sum += dmin[i];
    }
    if (sum <= 0) {
      const int16_t *pp =
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
    const int16_t *pp = vecs.data() + size_t(sample[chosen]) * IVF_DIMS;
    for (int d = 0; d < IVF_DIMS; ++d) centroids[c][d] = float(pp[d]);
    if ((c & 255u) == 0) std::cerr << "  init " << c << "/" << k << "\n";
  }
  return centroids;
}

// ── K-means training
// ──────────────────────────────────────────────────────────

static int train_kmeans(const std::vector<int16_t> &vecs,
                        const std::vector<uint32_t> &sample,
                        std::vector<std::array<float, IVF_DIMS>> &centroids,
                        int iters) {
  const uint32_t k = (uint32_t)centroids.size();
  std::vector<uint32_t> assign(sample.size(), 0);
  std::mt19937_64 rng(0xC0FFEE);
  int actual_iters = iters;

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
      const int16_t *p = vecs.data() + size_t(sample[i]) * IVF_DIMS;
      ++counts[c];
      double *row = sums.data() + size_t(c) * IVF_DIMS;
      for (int d = 0; d < IVF_DIMS; ++d) row[d] += p[d];
    }

    std::uniform_int_distribution<size_t> pick(0, sample.size() - 1);
    for (uint32_t c = 0; c < k; ++c) {
      if (counts[c] == 0) {
        const int16_t *p = vecs.data() + size_t(sample[pick(rng)]) * IVF_DIMS;
        for (int d = 0; d < IVF_DIMS; ++d) centroids[c][d] = float(p[d]);
      } else {
        double inv = 1.0 / counts[c];
        double *row = sums.data() + size_t(c) * IVF_DIMS;
        for (int d = 0; d < IVF_DIMS; ++d)
          centroids[c][d] = float(row[d] * inv);
      }
    }
    std::cerr << "  iter " << (iter + 1) << "/" << iters
              << " changed=" << changed << "\n";
    if (changed == 0) { actual_iters = iter + 1; break; }
  }
  return actual_iters;
}

// ── IVF build helper
// ──────────────────────────────────────────────────────────

static bool build_ivf(const std::vector<int16_t> &vecs,
                      const std::vector<uint8_t> &labels, uint32_t k,
                      uint32_t sample_sz, int iters, const char *output) {
  const uint32_t n = (uint32_t)labels.size();

  std::mt19937_64 rng(42);
  uint32_t actual_sample = std::min(sample_sz, n);
  std::vector<uint32_t> sample(actual_sample);
  std::uniform_int_distribution<uint32_t> pick_rec(0, n - 1);
  for (auto &s : sample) s = pick_rec(rng);

  std::cerr << "K-means++ init (k=" << k << ", n=" << n
            << ", sample=" << actual_sample << ")...\n";
  auto centroids = init_kmeans_pp(vecs, sample, k, 42);
  std::cerr << "Training...\n";
  int actual_iters = train_kmeans(vecs, sample, centroids, iters);

  std::cerr << "Assigning " << n << " vectors (top-2)...\n";
  std::vector<uint32_t> assignment(n);
  std::vector<uint32_t> alt(n);
  std::vector<uint32_t> counts(k, 0);
  for (uint32_t i = 0; i < n; ++i) {
    auto [c1, c2] = nearest2_centroid(vecs.data() + size_t(i) * IVF_DIMS, centroids);
    assignment[i] = c1;
    alt[i] = c2;
    ++counts[c1];
    if (i % 500000 == 0 && i > 0)
      std::cerr << "  assigned " << i << "/" << n << "\n";
  }

  // Balance pass: move excess vectors to their 2nd nearest centroid.
  // max_cap = 3× average, minimum avg+10 for small k.
  {
    const uint32_t avg_cnt = n / std::max(k, 1u);
    const uint32_t max_cap = std::max(avg_cnt * 3 + 1, avg_cnt + 10);
    std::cerr << "Balance pass (avg=" << avg_cnt << " max_cap=" << max_cap << ")...\n";
    uint64_t moved = 0;
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t c1 = assignment[i];
      uint32_t c2 = alt[i];
      if (counts[c1] > max_cap && counts[c2] < max_cap) {
        --counts[c1];
        ++counts[c2];
        assignment[i] = c2;
        ++moved;
      }
    }
    std::cerr << "  moved " << moved << " vectors\n";
  }

  print_histogram(counts);

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

  for (uint32_t c = 0; c < k; ++c) {
    if (counts[c] < 2) continue;
    const auto &cent = centroids[c];
    std::sort(
        order.begin() + starts[c], order.begin() + starts[c] + counts[c],
        [&](uint32_t a, uint32_t b) {
          return dist_to_centroid(vecs.data() + size_t(a) * IVF_DIMS, cent) <
                 dist_to_centroid(vecs.data() + size_t(b) * IVF_DIMS, cent);
        });
  }

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

      const int16_t *src = vecs.data() + size_t(orig) * IVF_DIMS;
      int16_t *dst = blocks_data.data() + size_t(block) * IVF_DIMS * IVF_BLOCK;
      for (int d = 0; d < IVF_DIMS; ++d) {
        int16_t v = src[d];
        dst[block_pair_offset(d, (int)lane)] = v;
        int16_t &mn = bbox_min[size_t(c) * IVF_DIMS + d];
        int16_t &mx = bbox_max[size_t(c) * IVF_DIMS + d];
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
  hdr.train_sample = actual_sample;
  hdr.train_iters = (uint32_t)actual_iters;

  const IndexLayout layout = layout_for(k, total_blocks);

  std::ofstream out(output, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::cerr << "Cannot write " << output << "\n";
    return false;
  }
  out.seekp((std::streamoff)(layout.total - 1));
  out.put('\0');

  auto write_at = [&](size_t off, const void *data, size_t len) {
    out.seekp((std::streamoff)off);
    out.write(static_cast<const char *>(data), (std::streamsize)len);
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

// ── Streaming JSON parser
// ───────────────────────────────────────────────────── Reads [{...}, ...]
// where each object has "vector":[14 doubles] and "label":"...". Avoids loading
// the whole file into a jsmn token tree.

static bool parse_references(const char *input, std::vector<int16_t> &vecs,
                             std::vector<uint8_t> &labels) {
  FILE *f = fopen(input, "rb");
  if (!f) {
    std::cerr << "Cannot open " << input << "\n";
    return false;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz <= 0) {
    fclose(f);
    return false;
  }
  rewind(f);

  std::vector<char> buf((size_t)sz + 1, '\0');
  fread(buf.data(), 1, (size_t)sz, f);
  fclose(f);

  const char *p = buf.data();
  const char *end = p + sz;

  while (p < end) {
    // Advance to next object
    while (p < end && *p != '{' && *p != ']') p++;
    if (p >= end || *p == ']') break;
    const char *obj_start = ++p;  // just past '{'

    // Objects are flat (vector array contains only numbers); find closing '}'
    // by counting brace depth.
    int depth = 1;
    const char *obj_end = obj_start;
    while (obj_end < end && depth > 0) {
      if (*obj_end == '{')
        depth++;
      else if (*obj_end == '}')
        depth--;
      obj_end++;
    }
    if (depth != 0) break;
    obj_end--;  // points at '}'

    // ── parse "vector" ────────────────────────────────────────────────────
    const char *vk =
        (const char *)memmem(obj_start, obj_end - obj_start, "\"vector\"", 8);
    if (!vk) {
      p = obj_end + 1;
      continue;
    }
    const char *arr = (const char *)memchr(vk + 8, '[', obj_end - vk - 8);
    if (!arr) {
      p = obj_end + 1;
      continue;
    }
    arr++;

    int n_read = 0;
    const char *np = arr;
    while (n_read < IVF_DIMS && np < obj_end) {
      while (np < obj_end && (*np == ' ' || *np == '\t' || *np == '\r' ||
                              *np == '\n' || *np == ','))
        np++;
      if (np >= obj_end || *np == ']') break;
      char *endp;
      double v = strtod(np, &endp);
      if (endp == np) break;
      vecs.push_back(quantize(v));
      np = endp;
      n_read++;
    }
    if (n_read != IVF_DIMS) {
      vecs.resize(vecs.size() - n_read);
      p = obj_end + 1;
      continue;
    }

    // ── parse "label" ─────────────────────────────────────────────────────
    const char *lk =
        (const char *)memmem(obj_start, obj_end - obj_start, "\"label\"", 7);
    if (!lk) {
      vecs.resize(vecs.size() - IVF_DIMS);
      p = obj_end + 1;
      continue;
    }
    const char *colon = (const char *)memchr(lk + 7, ':', obj_end - lk - 7);
    if (!colon) {
      vecs.resize(vecs.size() - IVF_DIMS);
      p = obj_end + 1;
      continue;
    }
    const char *qopen =
        (const char *)memchr(colon + 1, '"', obj_end - colon - 1);
    if (!qopen) {
      vecs.resize(vecs.size() - IVF_DIMS);
      p = obj_end + 1;
      continue;
    }
    const char *lval = qopen + 1;
    bool is_fraud = (lval + 5 <= obj_end && memcmp(lval, "fraud", 5) == 0);
    labels.push_back(is_fraud ? 1u : 0u);

    p = obj_end + 1;
  }
  return true;
}

// ── main
// ──────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <input.json> <output.bin>"
              << " [k=" << DEFAULT_K << "]"
              << " [sample=" << DEFAULT_SAMPLE << "]"
              << " [iters=" << DEFAULT_ITERS << "]"
              << " [ec_output.bin] [ec_k=256]\n";
    return 1;
  }
  const char *input = argv[1];
  const char *output = argv[2];
  uint32_t k = argc > 3 ? (uint32_t)std::stoul(argv[3]) : DEFAULT_K;
  uint32_t sample_sz =
      argc > 4 ? (uint32_t)std::stoul(argv[4]) : DEFAULT_SAMPLE;
  int iters = argc > 5 ? std::stoi(argv[5]) : DEFAULT_ITERS;
  const char *ec_output = argc > 6 ? argv[6] : nullptr;
  uint32_t ec_k = argc > 7 ? (uint32_t)std::stoul(argv[7]) : 256;

  std::cerr << "Parsing " << input << "...\n";
  std::vector<int16_t> vecs;
  std::vector<uint8_t> labels;
  vecs.reserve(size_t(3'000'000) * IVF_DIMS);
  labels.reserve(3'000'000);

  if (!parse_references(input, vecs, labels)) return 1;
  const uint32_t n = (uint32_t)labels.size();
  std::cerr << "Loaded " << n << " records\n";

  if (!build_ivf(vecs, labels, k, sample_sz, iters, output)) return 1;

  if (ec_output) {
    std::cerr << "Filtering edge cases (vec[11]==10000 && vec[12]>=7500)...\n";
    std::vector<int16_t> ec_vecs;
    std::vector<uint8_t> ec_labels;
    ec_vecs.reserve(size_t(1'000'000) * IVF_DIMS);
    ec_labels.reserve(1'000'000);
    for (uint32_t i = 0; i < n; ++i) {
      const int16_t *v = vecs.data() + size_t(i) * IVF_DIMS;
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
