#pragma once

#include <fcntl.h>
#include <immintrin.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "types.hpp"

class IVF {
 public:
  explicit IVF(const char* filename, int nprobe = 4, int repair_min = 2,
               int repair_max = 3) {
    nprobe_ = nprobe;
    repair_min_ = repair_min;
    repair_max_ = repair_max;
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
      perror("ivf open");
      std::exit(1);
    }
    struct stat st;
    fstat(fd, &st);
    file_size = (size_t)st.st_size;
    file_data = malloc(file_size);
    if (!file_data) {
      perror("ivf malloc");
      std::exit(1);
    }

    char* dst = (char*)file_data;
    size_t left = file_size;
    while (left > 0) {
      ssize_t r = read(fd, dst, left);
      if (r <= 0) {
        perror("ivf read");
        std::exit(1);
      }
      dst += r;
      left -= (size_t)r;
    }
    close(fd);

    const auto* hdr = (const IndexHeader*)file_data;
    n = hdr->n;
    k = hdr->k;
    total_blocks = hdr->total_blocks;

    const IndexLayout lo = layout_for(k, total_blocks);
    raw_centroids = (const int16_t*)((char*)file_data + lo.centroids);
    bbox_min = (const int16_t*)((char*)file_data + lo.bbox_min);
    bbox_max = (const int16_t*)((char*)file_data + lo.bbox_max);
    blk_offsets = (const uint32_t*)((char*)file_data + lo.offsets);
    counts = (const uint32_t*)((char*)file_data + lo.counts);
    labels = (const uint8_t*)((char*)file_data + lo.labels);
    blocks = (const int16_t*)((char*)file_data + lo.blocks);

    build_cpsoa();
    build_bpsoa();
  }

  ~IVF() {
    free(file_data);
    free(cpsoa);
    free(bpsoa_min);
    free(bpsoa_max);
  }

  static constexpr int MAX_NPROBE = 16;

  int get_fraud_count(const int16_t* q, bool* did_repair = nullptr) const {
    // Compute vq pairs once — shared by find_top_centroids, scan_cluster, repair
    __m256i vq[IVF_PAIRS];
    for (int p = 0; p < IVF_PAIRS; p++) vq[p] = make_qpair(q, p);

    uint32_t probes[MAX_NPROBE];
    find_top_centroids_vq(vq, probes, nprobe_);

    uint32_t top_dists[5] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
                             UINT32_MAX};
    uint8_t top_labels[5] = {};
    uint32_t max_top = UINT32_MAX;

    for (int i = 0; i < nprobe_; i++)
      scan_cluster_vq(vq, probes[i], top_dists, top_labels, max_top);

    int cnt = 0;
    for (int i = 0; i < 5; i++) cnt += top_labels[i];

    if (cnt >= repair_min_ && cnt <= repair_max_) {
      repair(vq, probes, nprobe_, top_dists, top_labels, max_top);
      cnt = 0;
      for (int i = 0; i < 5; i++) cnt += top_labels[i];
      if (did_repair) *did_repair = true;
    }

    return cnt;
  }

 private:
  void* file_data = nullptr;
  size_t file_size = 0;

  int nprobe_ = 4;
  int repair_min_ = 2;
  int repair_max_ = 3;

  uint32_t n = 0;
  uint32_t k = 0;
  uint32_t total_blocks = 0;

  const int16_t* raw_centroids = nullptr;
  const int16_t* bbox_min = nullptr;
  const int16_t* bbox_max = nullptr;
  const uint32_t* blk_offsets = nullptr;
  const uint32_t* counts = nullptr;
  const uint8_t* labels = nullptr;
  const int16_t* blocks = nullptr;

  // Centroid pair-SoA: n_groups × IVF_PAIRS × 16 int16
  // Group g, pair p: cpsoa[(g*IVF_PAIRS+p)*16 .. +15]
  //   = [c0.2p, c0.2p+1, c1.2p, c1.2p+1, ..., c7.2p, c7.2p+1]
  int16_t* cpsoa = nullptr;
  // Bbox pair-SoA: same layout as cpsoa, for bbox_min and bbox_max.
  // Used to compute 8 bbox lower bounds simultaneously in repair().
  int16_t* bpsoa_min = nullptr;
  int16_t* bpsoa_max = nullptr;
  uint32_t n_groups = 0;

  // Broadcast [q[2p], q[2p+1]] × 8 as int16 packed into int32 lanes.
  // Works because int16 subtraction is bitwise-equivalent for signed values
  // within [-32768, 32767] — our range [-10000, 10000] never overflows.
  static __attribute__((always_inline)) inline __m256i make_qpair(
      const int16_t* q, int p) {
    uint32_t lo = (uint32_t)(uint16_t)q[p * 2];
    uint32_t hi = (uint32_t)(uint16_t)q[p * 2 + 1];
    return _mm256_set1_epi32((int32_t)(lo | (hi << 16)));
  }

  void build_cpsoa() {
    n_groups = (k + 7) / 8;
    const size_t sz = size_t(n_groups) * IVF_PAIRS * 16 * sizeof(int16_t);
    cpsoa = (int16_t*)aligned_alloc(32, sz);
    if (!cpsoa) {
      perror("cpsoa alloc");
      std::exit(1);
    }
    memset(cpsoa, 0, sz);
    for (uint32_t g = 0; g < n_groups; g++) {
      for (int p = 0; p < IVF_PAIRS; p++) {
        int16_t* dst = cpsoa + (size_t(g) * IVF_PAIRS + p) * 16;
        for (int lane = 0; lane < 8; lane++) {
          const uint32_t c = g * 8 + (uint32_t)lane;
          if (c < k) {
            dst[lane * 2] = raw_centroids[size_t(c) * IVF_DIMS + p * 2];
            dst[lane * 2 + 1] = raw_centroids[size_t(c) * IVF_DIMS + p * 2 + 1];
          }
        }
      }
    }
  }

  void build_bpsoa() {
    const size_t sz = size_t(n_groups) * IVF_PAIRS * 16 * sizeof(int16_t);
    bpsoa_min = (int16_t*)aligned_alloc(32, sz);
    bpsoa_max = (int16_t*)aligned_alloc(32, sz);
    if (!bpsoa_min || !bpsoa_max) {
      perror("bpsoa alloc");
      std::exit(1);
    }
    memset(bpsoa_min, 0, sz);
    memset(bpsoa_max, 0, sz);
    for (uint32_t g = 0; g < n_groups; g++) {
      for (int p = 0; p < IVF_PAIRS; p++) {
        int16_t* dmin = bpsoa_min + (size_t(g) * IVF_PAIRS + p) * 16;
        int16_t* dmax = bpsoa_max + (size_t(g) * IVF_PAIRS + p) * 16;
        for (int lane = 0; lane < 8; lane++) {
          const uint32_t c = g * 8 + (uint32_t)lane;
          if (c < k) {
            dmin[lane * 2]     = bbox_min[size_t(c) * IVF_DIMS + p * 2];
            dmin[lane * 2 + 1] = bbox_min[size_t(c) * IVF_DIMS + p * 2 + 1];
            dmax[lane * 2]     = bbox_max[size_t(c) * IVF_DIMS + p * 2];
            dmax[lane * 2 + 1] = bbox_max[size_t(c) * IVF_DIMS + p * 2 + 1];
          }
        }
      }
    }
  }

  // Compute bbox lower bounds for 8 clusters in group g simultaneously.
  // vq must be precomputed via make_qpair for each pair.
  // lbs[8] receives the 8 lower bounds (may include phantom clusters if k%8 != 0).
  __attribute__((always_inline)) inline void bbox_lower_bound_8(
      uint32_t g, const __m256i* vq, uint32_t* lbs) const {
    const int16_t* smin = bpsoa_min + size_t(g) * IVF_PAIRS * 16;
    const int16_t* smax = bpsoa_max + size_t(g) * IVF_PAIRS * 16;
    __m256i acc = _mm256_setzero_si256();
    for (int p = 0; p < IVF_PAIRS; p++) {
      __m256i vmn = _mm256_loadu_si256((const __m256i*)(smin + p * 16));
      __m256i vmx = _mm256_loadu_si256((const __m256i*)(smax + p * 16));
      __m256i gap_lo = _mm256_max_epi16(_mm256_setzero_si256(),
                                        _mm256_sub_epi16(vmn, vq[p]));
      __m256i gap_hi = _mm256_max_epi16(_mm256_setzero_si256(),
                                        _mm256_sub_epi16(vq[p], vmx));
      __m256i gap = _mm256_max_epi16(gap_lo, gap_hi);
      acc = _mm256_add_epi32(acc, _mm256_madd_epi16(gap, gap));
    }
    _mm256_storeu_si256((__m256i*)lbs, acc);
  }

  // Fill `out[0..n-1]` with the n nearest centroid indices, sorted ascending.
  void find_top_centroids_vq(const __m256i* vq, uint32_t* out, int n) const {
    uint32_t top_d[MAX_NPROBE];
    uint32_t top_c[MAX_NPROBE];
    for (int i = 0; i < n; i++) {
      top_d[i] = UINT32_MAX;
      top_c[i] = (uint32_t)i;
    }
    uint32_t worst = UINT32_MAX;

    for (uint32_t g = 0; g < n_groups; g++) {
      const int16_t* src = cpsoa + size_t(g) * IVF_PAIRS * 16;
      __m256i acc = _mm256_setzero_si256();
      for (int p = 0; p < IVF_PAIRS; p++) {
        __m256i vc = _mm256_loadu_si256((const __m256i*)(src + p * 16));
        __m256i diff = _mm256_sub_epi16(vq[p], vc);
        acc = _mm256_add_epi32(acc, _mm256_madd_epi16(diff, diff));
      }
      uint32_t vals[8];
      _mm256_storeu_si256((__m256i*)vals, acc);
      const uint32_t base = g * 8;
      const uint32_t lim = std::min(8u, k - base);
      for (uint32_t i = 0; i < lim; i++) {
        const uint32_t v = vals[i];
        if (v >= worst) continue;
        int wi = 0;
        for (int j = 1; j < n; j++)
          if (top_d[j] > top_d[wi]) wi = j;
        top_d[wi] = v;
        top_c[wi] = base + i;
        worst = 0;
        for (int j = 0; j < n; j++)
          if (top_d[j] > worst) worst = top_d[j];
      }
    }
    for (int i = 0; i < n - 1; i++)
      for (int j = i + 1; j < n; j++)
        if (top_d[j] < top_d[i]) {
          std::swap(top_d[i], top_d[j]);
          std::swap(top_c[i], top_c[j]);
        }

    for (int i = 0; i < n; i++) out[i] = top_c[i];
  }

  __attribute__((always_inline)) inline void update_top5(
      uint32_t dist, uint8_t label, uint32_t* top_dists, uint8_t* top_labels,
      uint32_t& max_top) const {
    if (dist >= max_top) return;
    int i = 3;
    while (i >= 0 && dist < top_dists[i]) {
      top_dists[i + 1] = top_dists[i];
      top_labels[i + 1] = top_labels[i];
      i--;
    }
    top_dists[i + 1] = dist;
    top_labels[i + 1] = label;
    max_top = top_dists[4];
  }

  void scan_cluster_vq(const __m256i* vq, uint32_t c, uint32_t* top_dists,
                       uint8_t* top_labels, uint32_t& max_top) const {
    if (counts[c] == 0) return;

    const uint32_t blk_start = blk_offsets[c];
    const uint32_t blk_end = blk_offsets[c + 1];

#define SPAIR(p, acc)                                                    \
  {                                                                      \
    __m256i vc =                                                         \
        _mm256_loadu_si256((const __m256i*)(blk + (p) * IVF_BLOCK * 2)); \
    __m256i df = _mm256_sub_epi16(vq[p], vc);                            \
    acc = _mm256_add_epi32(acc, _mm256_madd_epi16(df, df));              \
  }
    for (uint32_t bi = blk_start; bi < blk_end; bi++) {
      if (bi + 2 < blk_end)
        __builtin_prefetch(blocks + size_t(bi + 2) * IVF_DIMS * IVF_BLOCK, 0, 1);
      const int16_t* blk = blocks + size_t(bi) * IVF_DIMS * IVF_BLOCK;

      // Capture max_top at block start — uses the tightest available value
      __m256i vmax =
          _mm256_set1_epi32((int32_t)std::min(max_top, (uint32_t)INT32_MAX));

      __m256i acc0 = _mm256_setzero_si256(), acc1 = _mm256_setzero_si256();
      SPAIR(0, acc0);
      SPAIR(1, acc1);
      SPAIR(2, acc0);

      // Early prune at 6/14 dims: skip if all 8 lanes already >= max_top
      if (_mm256_movemask_epi8(
              _mm256_cmpgt_epi32(vmax, _mm256_add_epi32(acc0, acc1))) == 0)
        continue;

      SPAIR(3, acc1);
      SPAIR(4, acc0);

      // Prune at 10/14 dims
      if (_mm256_movemask_epi8(
              _mm256_cmpgt_epi32(vmax, _mm256_add_epi32(acc0, acc1))) == 0)
        continue;

      SPAIR(5, acc1);
      SPAIR(6, acc0);

      uint32_t dists[8];
      _mm256_storeu_si256((__m256i*)dists, _mm256_add_epi32(acc0, acc1));

      const uint32_t pos = (bi - blk_start) * IVF_BLOCK;
      const int n_valid = (int)std::min((uint32_t)IVF_BLOCK, counts[c] - pos);
      const uint8_t* lbl = labels + size_t(bi) * IVF_BLOCK;
      for (int j = 0; j < n_valid; j++)
        update_top5(dists[j], lbl[j], top_dists, top_labels, max_top);
    }
#undef SPAIR
  }


  void repair(const __m256i* vq, const uint32_t* skip, int nskip,
              uint32_t* top_dists, uint8_t* top_labels,
              uint32_t& max_top) const {
    struct Cand { uint32_t lb; uint32_t c; };
    static thread_local Cand cands[1024];
    int ncands = 0;

    // O(1) skip check per cluster via bitset (replaces O(nskip) linear scan)
    uint64_t skip_set[64] = {};
    for (int s = 0; s < nskip; s++)
      skip_set[skip[s] >> 6] |= 1ULL << (skip[s] & 63);

    // Signed comparison: clip max_top to INT32_MAX to avoid sign issues
    const __m256i vmt =
        _mm256_set1_epi32((int32_t)std::min(max_top, (uint32_t)INT32_MAX));

    uint32_t lbs[8];
    for (uint32_t g = 0; g < n_groups; g++) {
      // Compute bbox lower bounds for all 8 clusters in this group at once
      bbox_lower_bound_8(g, vq, lbs);

      // SIMD filter: which of the 8 clusters have lb < max_top?
      // _mm256_cmpgt_epi32 sets lane to 0xFFFFFFFF if vmt > vlbs (i.e. lb < max_top)
      __m256i vlbs = _mm256_loadu_si256((const __m256i*)lbs);
      int pass_mask =
          _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(vmt, vlbs)));
      if (!pass_mask) continue;

      const uint32_t base = g * 8;
      int m = pass_mask;
      while (m) {
        int i = __builtin_ctz(m);
        m &= m - 1;
        const uint32_t c = base + (uint32_t)i;
        if (c >= k) continue;
        if (skip_set[c >> 6] & (1ULL << (c & 63))) continue;
        if (counts[c] == 0) continue;
        if (ncands < 1024) cands[ncands++] = {lbs[i], c};
      }
    }

    std::sort(cands, cands + ncands,
              [](const Cand& a, const Cand& b) { return a.lb < b.lb; });
    for (int i = 0; i < ncands; i++) {
      if (cands[i].lb >= max_top) break;
      scan_cluster_vq(vq, cands[i].c, top_dists, top_labels, max_top);
      // Early stop: if result is now unambiguous, scanning more clusters
      // cannot change the approved/denied decision.
      int cnt_now = top_labels[0] + top_labels[1] + top_labels[2]
                  + top_labels[3] + top_labels[4];
      if (cnt_now < repair_min_ || cnt_now > repair_max_) break;
    }
  }
};
