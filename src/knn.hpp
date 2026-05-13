#pragma once

#include <fcntl.h>
#include <immintrin.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "types.hpp"

class KNN {
 public:
  const Block8* blocks;
  int num_blocks;
  QuantParams qp;
  float global_scale_sq;
  void* mmap_ptr;
  size_t mmap_size;

  KNN(const char* filename) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) exit(1);
    struct stat st;
    fstat(fd, &st);
    mmap_size = st.st_size;
    mmap_ptr = mmap(NULL, mmap_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mmap_ptr == MAP_FAILED) exit(1);
    close(fd);

    std::memcpy(&qp, mmap_ptr, sizeof(QuantParams));
    float s = qp.range_global / 65535.0f;
    global_scale_sq = s * s;

    blocks = (const Block8*)((char*)mmap_ptr + sizeof(QuantParams));
    num_blocks = (int)(mmap_size - sizeof(QuantParams)) / sizeof(Block8);
  }

  ~KNN() {
    if (mmap_ptr && mmap_ptr != MAP_FAILED) munmap(mmap_ptr, mmap_size);
  }

  int get_fraud_count_exact(const float* target_f) {
    uint16_t tq[14];
    float target_norm_sq = 0;
    for (int d = 0; d < 14; ++d) {
      double q = (double)(target_f[d] - qp.min_global) /
                 (double)qp.range_global * 65535.0;
      tq[d] = (uint16_t)std::clamp(std::round(q), 0.0, 65535.0);
      target_norm_sq += target_f[d] * target_f[d];
    }
    float target_norm = std::sqrt(target_norm_sq);

    const __m256i vt0 = _mm256_set1_epi32(tq[0]);
    const __m256i vt1 = _mm256_set1_epi32(tq[1]);
    const __m256i vt2 = _mm256_set1_epi32(tq[2]);
    const __m256i vt3 = _mm256_set1_epi32(tq[3]);
    const __m256i vt4 = _mm256_set1_epi32(tq[4]);
    const __m256i vt5 = _mm256_set1_epi32(tq[5]);
    const __m256i vt6 = _mm256_set1_epi32(tq[6]);
    const __m256i vt7 = _mm256_set1_epi32(tq[7]);
    const __m256i vt8 = _mm256_set1_epi32(tq[8]);
    const __m256i vt9 = _mm256_set1_epi32(tq[9]);
    const __m256i vt10 = _mm256_set1_epi32(tq[10]);
    const __m256i vt11 = _mm256_set1_epi32(tq[11]);
    const __m256i vt12 = _mm256_set1_epi32(tq[12]);
    const __m256i vt13 = _mm256_set1_epi32(tq[13]);

    float top_dists[5] = {1e18f, 1e18f, 1e18f, 1e18f, 1e18f};
    bool top_frauds[5] = {false, false, false, false, false};
    float max_top_dist = 1e18f;

    auto update_top5 = [&](float dist, bool fraud)
                           __attribute__((always_inline)) {
                             if (dist >= max_top_dist) return;
                             if (dist < top_dists[0]) {
                               top_dists[4] = top_dists[3];
                               top_frauds[4] = top_frauds[3];
                               top_dists[3] = top_dists[2];
                               top_frauds[3] = top_frauds[2];
                               top_dists[2] = top_dists[1];
                               top_frauds[2] = top_frauds[1];
                               top_dists[1] = top_dists[0];
                               top_frauds[1] = top_frauds[0];
                               top_dists[0] = dist;
                               top_frauds[0] = fraud;
                             } else if (dist < top_dists[1]) {
                               top_dists[4] = top_dists[3];
                               top_frauds[4] = top_frauds[3];
                               top_dists[3] = top_dists[2];
                               top_frauds[3] = top_frauds[2];
                               top_dists[2] = top_dists[1];
                               top_frauds[2] = top_frauds[1];
                               top_dists[1] = dist;
                               top_frauds[1] = fraud;
                             } else if (dist < top_dists[2]) {
                               top_dists[4] = top_dists[3];
                               top_frauds[4] = top_frauds[3];
                               top_dists[3] = top_dists[2];
                               top_frauds[3] = top_frauds[2];
                               top_dists[2] = dist;
                               top_frauds[2] = fraud;
                             } else if (dist < top_dists[3]) {
                               top_dists[4] = top_dists[3];
                               top_frauds[4] = top_frauds[3];
                               top_dists[3] = dist;
                               top_frauds[3] = fraud;
                             } else {
                               top_dists[4] = dist;
                               top_frauds[4] = fraud;
                             }
                             max_top_dist = top_dists[4];
                           };

    int low = 0, high = num_blocks - 1;
    int start_idx = 0;
    while (low <= high) {
      int mid = low + (high - low) / 2;
      if (blocks[mid].max_norm < target_norm) {
        start_idx = mid;
        low = mid + 1;
      } else {
        high = mid - 1;
      }
    }

    // Early-exit threshold: max_top_dist / global_scale_sq converted to
    // quantized space
    int left = start_idx, right = start_idx + 1;
    while (left >= 0 || right < num_blocks) {
      int current = (left >= 0 && (right >= num_blocks ||
                                   (target_norm - blocks[left].max_norm) <
                                       (blocks[right].min_norm - target_norm)))
                        ? left--
                        : right++;

      const Block8& b = blocks[current];
      float norm_diff =
          (target_norm < b.min_norm)
              ? (b.min_norm - target_norm)
              : ((target_norm > b.max_norm) ? (target_norm - b.max_norm) : 0);
      if (norm_diff * norm_diff >= max_top_dist) {
        if (current < start_idx)
          left = -1;
        else
          right = num_blocks;
        if (left < 0 && right >= num_blocks) break;
        continue;
      }

#define COMPUTE_DIM(d, vt, acc)                                  \
  {                                                              \
    __m128i bv = _mm_loadu_si128((const __m128i*)&b.dims[d][0]); \
    __m256i bi = _mm256_cvtepu16_epi32(bv);                      \
    __m256 df = _mm256_cvtepi32_ps(_mm256_sub_epi32(vt, bi));    \
    acc = _mm256_fmadd_ps(df, df, acc);                          \
  }

      __m256 acc0 = _mm256_setzero_ps();
      __m256 acc1 = _mm256_setzero_ps();

      // First half: dimensions 0-6 (7 dims)
      COMPUTE_DIM(0, vt0, acc0);
      COMPUTE_DIM(1, vt1, acc1);
      COMPUTE_DIM(2, vt2, acc0);
      COMPUTE_DIM(3, vt3, acc1);
      COMPUTE_DIM(4, vt4, acc0);
      COMPUTE_DIM(5, vt5, acc1);
      COMPUTE_DIM(6, vt6, acc0);

      // Early exit: if partial distance (7/14 dims) already exceeds threshold,
      // skip Heuristic: if half the dimensions already exceeded, the total
      // certainly will
      __m256 partial = _mm256_mul_ps(_mm256_add_ps(acc0, acc1),
                                     _mm256_set1_ps(global_scale_sq));
      // Check if the MINIMUM of the 8 records already exceeds — if so, entire
      // block is useless
      float partial_dists[8];
      _mm256_storeu_ps(partial_dists, partial);
      float min_partial = partial_dists[0];
      for (int j = 1; j < 8; ++j)
        min_partial = std::min(min_partial, partial_dists[j]);
      if (min_partial >= max_top_dist) continue;

      // Second half: dimensions 7-13 (7 dims)
      COMPUTE_DIM(7, vt7, acc1);
      COMPUTE_DIM(8, vt8, acc0);
      COMPUTE_DIM(9, vt9, acc1);
      COMPUTE_DIM(10, vt10, acc0);
      COMPUTE_DIM(11, vt11, acc1);
      COMPUTE_DIM(12, vt12, acc0);
      COMPUTE_DIM(13, vt13, acc1);

      __m256 v_acc = _mm256_mul_ps(_mm256_add_ps(acc0, acc1),
                                   _mm256_set1_ps(global_scale_sq));
      float final_dists[8];
      _mm256_storeu_ps(final_dists, v_acc);

      for (int j = 0; j < 8; ++j)
        update_top5(final_dists[j], (bool)b.is_fraud[j]);
    }

    int count = 0;
    for (int i = 0; i < 5; ++i)
      if (top_frauds[i]) count++;
    return count;
  }
};
