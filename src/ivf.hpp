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
    void*    file_data = nullptr;
    size_t   file_size = 0;

    uint32_t n            = 0;
    uint32_t k            = 0;
    uint32_t total_blocks = 0;

    const int16_t*  raw_centroids = nullptr;
    const int16_t*  bbox_min      = nullptr;
    const int16_t*  bbox_max      = nullptr;
    const uint32_t* blk_offsets   = nullptr;
    const uint32_t* counts        = nullptr;
    const uint8_t*  labels        = nullptr;
    const int16_t*  blocks        = nullptr;

    // Centroid pair-SoA: n_groups × IVF_PAIRS × 16 int16
    // Group g, pair p: cpsoa[(g*IVF_PAIRS+p)*16 .. +15]
    //   = [c0.2p, c0.2p+1, c1.2p, c1.2p+1, ..., c7.2p, c7.2p+1]
    int16_t* cpsoa    = nullptr;
    uint32_t n_groups = 0;

    explicit IVF(const char* filename) {
        int fd = open(filename, O_RDONLY);
        if (fd < 0) { perror("ivf open"); std::exit(1); }
        struct stat st; fstat(fd, &st);
        file_size = (size_t)st.st_size;
        file_data = malloc(file_size);
        if (!file_data) { perror("ivf malloc"); std::exit(1); }

        char* dst = (char*)file_data;
        size_t left = file_size;
        while (left > 0) {
            ssize_t r = read(fd, dst, left);
            if (r <= 0) { perror("ivf read"); std::exit(1); }
            dst += r; left -= (size_t)r;
        }
        close(fd);

        const auto* hdr = (const IndexHeader*)file_data;
        n            = hdr->n;
        k            = hdr->k;
        total_blocks = hdr->total_blocks;

        const IndexLayout lo = layout_for(k, total_blocks);
        raw_centroids = (const int16_t*)((char*)file_data + lo.centroids);
        bbox_min      = (const int16_t*)((char*)file_data + lo.bbox_min);
        bbox_max      = (const int16_t*)((char*)file_data + lo.bbox_max);
        blk_offsets   = (const uint32_t*)((char*)file_data + lo.offsets);
        counts        = (const uint32_t*)((char*)file_data + lo.counts);
        labels        = (const uint8_t*)((char*)file_data + lo.labels);
        blocks        = (const int16_t*)((char*)file_data + lo.blocks);

        build_cpsoa();
    }

    ~IVF() {
        free(file_data);
        free(cpsoa);
    }

    static constexpr int NPROBE = 4;

    int get_fraud_count(const int16_t* q) const {
        uint32_t probes[NPROBE];
        find_top_centroids(q, probes, NPROBE);

        float   top_dists[5]  = {1e18f, 1e18f, 1e18f, 1e18f, 1e18f};
        uint8_t top_labels[5] = {};
        float   max_top       = 1e18f;

        for (int i = 0; i < NPROBE; i++)
            scan_cluster(probes[i], q, top_dists, top_labels, max_top);

        int cnt = 0;
        for (int i = 0; i < 5; i++) cnt += top_labels[i];

        if (cnt >= 1 && cnt <= 4) {
            repair(q, probes, NPROBE, top_dists, top_labels, max_top);
            cnt = 0;
            for (int i = 0; i < 5; i++) cnt += top_labels[i];
        }

        return cnt;
    }

private:
    // Broadcast [q[2p], q[2p+1]] × 8 as int16 packed into int32 lanes.
    // Works because int16 subtraction is bitwise-equivalent for signed values
    // within [-32768, 32767] — our range [-10000, 10000] never overflows.
    static __attribute__((always_inline)) inline __m256i
    make_qpair(const int16_t* q, int p) {
        uint32_t lo = (uint32_t)(uint16_t)q[p * 2];
        uint32_t hi = (uint32_t)(uint16_t)q[p * 2 + 1];
        return _mm256_set1_epi32((int32_t)(lo | (hi << 16)));
    }

    void build_cpsoa() {
        n_groups = (k + 7) / 8;
        const size_t sz = size_t(n_groups) * IVF_PAIRS * 16 * sizeof(int16_t);
        cpsoa = (int16_t*)aligned_alloc(32, sz);
        if (!cpsoa) { perror("cpsoa alloc"); std::exit(1); }
        memset(cpsoa, 0, sz);
        for (uint32_t g = 0; g < n_groups; g++) {
            for (int p = 0; p < IVF_PAIRS; p++) {
                int16_t* dst = cpsoa + (size_t(g) * IVF_PAIRS + p) * 16;
                for (int lane = 0; lane < 8; lane++) {
                    const uint32_t c = g * 8 + (uint32_t)lane;
                    if (c < k) {
                        dst[lane * 2]     = raw_centroids[size_t(c) * IVF_DIMS + p * 2];
                        dst[lane * 2 + 1] = raw_centroids[size_t(c) * IVF_DIMS + p * 2 + 1];
                    }
                }
            }
        }
    }

    // Fill `out[0..n-1]` with the n nearest centroid indices, sorted by distance.
    // Uses a small float[n] heap; n is assumed small (≤ 16).
    void find_top_centroids(const int16_t* q, uint32_t* out, int n) const {
        __m256i vq[IVF_PAIRS];
        for (int p = 0; p < IVF_PAIRS; p++) vq[p] = make_qpair(q, p);

        // top_d[0] = worst distance in the heap (largest), out[0] = its index.
        // We keep a max-heap of size n so insertion is O(n) worst case,
        // acceptable since n ≤ 16 and we only call this once per request.
        float    top_d[NPROBE]; // worst-first
        uint32_t top_c[NPROBE];
        for (int i = 0; i < n; i++) { top_d[i] = 1e18f; top_c[i] = (uint32_t)i; }
        float worst = 1e18f;

        for (uint32_t g = 0; g < n_groups; g++) {
            const int16_t* src = cpsoa + size_t(g) * IVF_PAIRS * 16;
            __m256 acc = _mm256_setzero_ps();
            for (int p = 0; p < IVF_PAIRS; p++) {
                __m256i vc   = _mm256_loadu_si256((const __m256i*)(src + p * 16));
                __m256i diff = _mm256_sub_epi16(vq[p], vc);
                acc = _mm256_add_ps(acc, _mm256_cvtepi32_ps(_mm256_madd_epi16(diff, diff)));
            }
            float vals[8];
            _mm256_storeu_ps(vals, acc);
            const uint32_t base = g * 8;
            const uint32_t lim  = std::min(8u, k - base);
            for (uint32_t i = 0; i < lim; i++) {
                const float v = vals[i];
                if (v >= worst) continue;
                // Replace worst entry
                int wi = 0;
                for (int j = 1; j < n; j++) if (top_d[j] > top_d[wi]) wi = j;
                top_d[wi] = v; top_c[wi] = base + i;
                worst = 0;
                for (int j = 0; j < n; j++) if (top_d[j] > worst) worst = top_d[j];
            }
        }
        // Sort ascending so out[0] is the nearest cluster
        for (int i = 0; i < n - 1; i++)
            for (int j = i + 1; j < n; j++)
                if (top_d[j] < top_d[i]) {
                    std::swap(top_d[i], top_d[j]);
                    std::swap(top_c[i], top_c[j]);
                }
        for (int i = 0; i < n; i++) out[i] = top_c[i];
    }

    __attribute__((always_inline)) inline
    void update_top5(float dist, uint8_t label,
                     float* top_dists, uint8_t* top_labels,
                     float& max_top) const {
        if (dist >= max_top) return;
        if (dist < top_dists[0]) {
            top_dists[4]=top_dists[3]; top_labels[4]=top_labels[3];
            top_dists[3]=top_dists[2]; top_labels[3]=top_labels[2];
            top_dists[2]=top_dists[1]; top_labels[2]=top_labels[1];
            top_dists[1]=top_dists[0]; top_labels[1]=top_labels[0];
            top_dists[0]=dist;         top_labels[0]=label;
        } else if (dist < top_dists[1]) {
            top_dists[4]=top_dists[3]; top_labels[4]=top_labels[3];
            top_dists[3]=top_dists[2]; top_labels[3]=top_labels[2];
            top_dists[2]=top_dists[1]; top_labels[2]=top_labels[1];
            top_dists[1]=dist;         top_labels[1]=label;
        } else if (dist < top_dists[2]) {
            top_dists[4]=top_dists[3]; top_labels[4]=top_labels[3];
            top_dists[3]=top_dists[2]; top_labels[3]=top_labels[2];
            top_dists[2]=dist;         top_labels[2]=label;
        } else if (dist < top_dists[3]) {
            top_dists[4]=top_dists[3]; top_labels[4]=top_labels[3];
            top_dists[3]=dist;         top_labels[3]=label;
        } else {
            top_dists[4]=dist;         top_labels[4]=label;
        }
        max_top = top_dists[4];
    }

    void scan_cluster(uint32_t c, const int16_t* q,
                      float* top_dists, uint8_t* top_labels, float& max_top) const {
        if (counts[c] == 0) return;

        __m256i vq[IVF_PAIRS];
        for (int p = 0; p < IVF_PAIRS; p++) vq[p] = make_qpair(q, p);

        const uint32_t blk_start = blk_offsets[c];
        const uint32_t blk_end   = blk_offsets[c + 1];

#define SPAIR(p, acc) { \
    __m256i vc = _mm256_loadu_si256( \
        (const __m256i*)(blk + (p) * IVF_BLOCK * 2)); \
    __m256i df = _mm256_sub_epi16(vq[p], vc); \
    acc = _mm256_add_ps(acc, _mm256_cvtepi32_ps(_mm256_madd_epi16(df, df))); \
}
        for (uint32_t bi = blk_start; bi < blk_end; bi++) {
            const int16_t* blk = blocks + size_t(bi) * IVF_DIMS * IVF_BLOCK;

            __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
            SPAIR(0, acc0); SPAIR(1, acc1);
            SPAIR(2, acc0); SPAIR(3, acc1);
            SPAIR(4, acc0);

            // Partial prune: after 10/14 dims, skip if all 8 lanes already >= max_top
            if (_mm256_movemask_ps(_mm256_cmp_ps(
                    _mm256_add_ps(acc0, acc1),
                    _mm256_set1_ps(max_top), _CMP_GE_OQ)) == 0xFF) continue;

            SPAIR(5, acc1); SPAIR(6, acc0);

            float dists[8];
            _mm256_storeu_ps(dists, _mm256_add_ps(acc0, acc1));

            const uint32_t pos = (bi - blk_start) * IVF_BLOCK;
            const int n_valid  = (int)std::min((uint32_t)IVF_BLOCK, counts[c] - pos);
            const uint8_t* lbl = labels + size_t(bi) * IVF_BLOCK;
            for (int j = 0; j < n_valid; j++)
                update_top5(dists[j], lbl[j], top_dists, top_labels, max_top);
        }
#undef SPAIR
    }

    float bbox_lower_bound(uint32_t c, const int16_t* q) const {
        const int16_t* mn = bbox_min + size_t(c) * IVF_DIMS;
        const int16_t* mx = bbox_max + size_t(c) * IVF_DIMS;
        // Vectorize first 8 dims: load 8×int16 → 8×int32, compute gap², hsum
        __m256i vq  = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)q));
        __m256i vmn = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)mn));
        __m256i vmx = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)mx));
        __m256i gap = _mm256_max_epi32(_mm256_setzero_si256(),
                      _mm256_max_epi32(_mm256_sub_epi32(vmn, vq),
                                       _mm256_sub_epi32(vq, vmx)));
        __m256 gf = _mm256_cvtepi32_ps(gap);
        __m256 acc = _mm256_mul_ps(gf, gf);
        __m128 lo = _mm256_castps256_ps128(acc);
        __m128 hi = _mm256_extractf128_ps(acc, 1);
        __m128 s  = _mm_add_ps(lo, hi);
        s = _mm_add_ps(s, _mm_movehl_ps(s, s));
        s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
        float lb = _mm_cvtss_f32(s);
        // Scalar for remaining 6 dims (safe: no out-of-bounds read)
        for (int d = 8; d < IVF_DIMS; d++) {
            float v   = (float)q[d];
            float g   = v < (float)mn[d] ? (float)mn[d] - v
                      : v > (float)mx[d] ? v - (float)mx[d]
                      : 0.0f;
            lb += g * g;
        }
        return lb;
    }

    void repair(const int16_t* q, const uint32_t* skip, int nskip,
                float* top_dists, uint8_t* top_labels, float& max_top) const {
        // Collect candidates with bbox lb < max_top, then sort by lb so
        // nearest clusters shrink max_top fastest → more pruning downstream.
        struct Cand { float lb; uint32_t c; };
        static thread_local Cand cands[4096];
        int ncands = 0;
        for (uint32_t c = 0; c < k; c++) {
            if (counts[c] == 0) continue;
            bool skipped = false;
            for (int s = 0; s < nskip; s++) if (c == skip[s]) { skipped = true; break; }
            if (skipped) continue;
            float lb = bbox_lower_bound(c, q);
            if (lb < max_top) cands[ncands++] = {lb, c};
        }
        std::sort(cands, cands + ncands, [](const Cand& a, const Cand& b){ return a.lb < b.lb; });
        for (int i = 0; i < ncands; i++) {
            if (cands[i].lb >= max_top) break; // max_top may have tightened
            scan_cluster(cands[i].c, q, top_dists, top_labels, max_top);
        }
    }
};
