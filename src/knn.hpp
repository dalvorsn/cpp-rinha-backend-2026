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
    void*    mmap_ptr  = nullptr;
    size_t   mmap_size = 0;

    const BallNode*      nodes  = nullptr;
    const Block8*        blocks = nullptr;
    QuantParams          qp     = {};
    float                global_scale_sq = 0;
    float                inv_range_scale = 0; // 65535 / range, precomputed

    explicit KNN(const char* filename) {
        int fd = open(filename, O_RDONLY);
        if (fd < 0) { perror("knn open"); std::exit(1); }
        struct stat st;
        fstat(fd, &st);
        mmap_size = (size_t)st.st_size;
        mmap_ptr  = mmap(nullptr, mmap_size, PROT_READ,
                         MAP_PRIVATE | MAP_POPULATE, fd, 0);
        if (mmap_ptr == MAP_FAILED) { perror("knn mmap"); std::exit(1); }
        close(fd);
        mlock(mmap_ptr, mmap_size);

        const auto* hdr  = (const BallTreeHeader*)mmap_ptr;
        qp.min_global    = hdr->qp_min_global;
        qp.range_global  = hdr->qp_range_global;
        global_scale_sq  = hdr->global_scale_sq;
        inv_range_scale  = 65535.0f / qp.range_global;

        nodes  = (const BallNode*)((char*)mmap_ptr + sizeof(BallTreeHeader));
        blocks = (const Block8*) ((char*)mmap_ptr + sizeof(BallTreeHeader)
                                   + hdr->num_nodes * sizeof(BallNode));

        // Blocks are accessed in ball-tree traversal order (random); disable OS read-ahead
        madvise((void*)blocks, hdr->num_blocks * sizeof(Block8), MADV_RANDOM);
    }

    ~KNN() {
        if (mmap_ptr && mmap_ptr != MAP_FAILED) munmap(mmap_ptr, mmap_size);
    }

    int get_fraud_count(const float* target_f) const {
        // Quantize query to uint16 using float arithmetic (no double needed)
        uint16_t tq[14];
        for (int d = 0; d < 14; ++d) {
            float q = (target_f[d] - qp.min_global) * inv_range_scale + 0.5f;
            if (q < 0.5f)       q = 0.5f;
            if (q > 65535.5f)   q = 65535.5f;
            tq[d] = (uint16_t)q;
        }

        float top_dists[5]     = {1e18f, 1e18f, 1e18f, 1e18f, 1e18f};
        bool  top_frauds[5]    = {};
        float max_top          = 1e18f;
        float sqrt_max_top     = 1e9f;  // sqrt(1e18)

        search(0, target_f, tq, top_dists, top_frauds, max_top, sqrt_max_top);

        int count = 0;
        for (int i = 0; i < 5; i++) if (top_frauds[i]) count++;
        return count;
    }

 private:
    // Maintain sqrt_max_top in sync: called only when a new neighbor is found.
    // Much cheaper than computing sqrt() at every node visit.
    inline void update_top5(float dist, bool fraud,
                             float* top_dists, bool* top_frauds,
                             float& max_top, float& sqrt_max_top) const
                             __attribute__((always_inline)) {
        if (dist >= max_top) return;
        if (dist < top_dists[0]) {
            top_dists[4]=top_dists[3]; top_frauds[4]=top_frauds[3];
            top_dists[3]=top_dists[2]; top_frauds[3]=top_frauds[2];
            top_dists[2]=top_dists[1]; top_frauds[2]=top_frauds[1];
            top_dists[1]=top_dists[0]; top_frauds[1]=top_frauds[0];
            top_dists[0]=dist;         top_frauds[0]=fraud;
        } else if (dist < top_dists[1]) {
            top_dists[4]=top_dists[3]; top_frauds[4]=top_frauds[3];
            top_dists[3]=top_dists[2]; top_frauds[3]=top_frauds[2];
            top_dists[2]=top_dists[1]; top_frauds[2]=top_frauds[1];
            top_dists[1]=dist;         top_frauds[1]=fraud;
        } else if (dist < top_dists[2]) {
            top_dists[4]=top_dists[3]; top_frauds[4]=top_frauds[3];
            top_dists[3]=top_dists[2]; top_frauds[3]=top_frauds[2];
            top_dists[2]=dist;         top_frauds[2]=fraud;
        } else if (dist < top_dists[3]) {
            top_dists[4]=top_dists[3]; top_frauds[4]=top_frauds[3];
            top_dists[3]=dist;         top_frauds[3]=fraud;
        } else {
            top_dists[4]=dist;         top_frauds[4]=fraud;
        }
        max_top      = top_dists[4];
        sqrt_max_top = std::sqrt(max_top);
    }

    void search_leaf(int leaf_start, int leaf_count,
                     const uint16_t* tq,
                     float* top_dists, bool* top_frauds,
                     float& max_top, float& sqrt_max_top) const {
        const __m256i vt0  = _mm256_set1_epi32(tq[0]);
        const __m256i vt1  = _mm256_set1_epi32(tq[1]);
        const __m256i vt2  = _mm256_set1_epi32(tq[2]);
        const __m256i vt3  = _mm256_set1_epi32(tq[3]);
        const __m256i vt4  = _mm256_set1_epi32(tq[4]);
        const __m256i vt5  = _mm256_set1_epi32(tq[5]);
        const __m256i vt6  = _mm256_set1_epi32(tq[6]);
        const __m256i vt7  = _mm256_set1_epi32(tq[7]);
        const __m256i vt8  = _mm256_set1_epi32(tq[8]);
        const __m256i vt9  = _mm256_set1_epi32(tq[9]);
        const __m256i vt10 = _mm256_set1_epi32(tq[10]);
        const __m256i vt11 = _mm256_set1_epi32(tq[11]);
        const __m256i vt12 = _mm256_set1_epi32(tq[12]);
        const __m256i vt13 = _mm256_set1_epi32(tq[13]);
        const float   gssq = global_scale_sq;

#define KDIM(d, vt, acc) { \
    __m128i bv = _mm_loadu_si128((const __m128i*)&b.dims[d][0]); \
    __m256i bi = _mm256_cvtepu16_epi32(bv); \
    __m256  df = _mm256_cvtepi32_ps(_mm256_sub_epi32(vt, bi)); \
    acc = _mm256_fmadd_ps(df, df, acc); \
}
        for (int bi = 0; bi < leaf_count; bi++) {
            const Block8& b = blocks[leaf_start + bi];

            __m256 acc0 = _mm256_setzero_ps();
            __m256 acc1 = _mm256_setzero_ps();
            KDIM(0,  vt0,  acc0); KDIM(1,  vt1,  acc1);
            KDIM(2,  vt2,  acc0); KDIM(3,  vt3,  acc1);
            KDIM(4,  vt4,  acc0); KDIM(5,  vt5,  acc1);
            KDIM(6,  vt6,  acc0);

            // After 7/14 dims: partial_dist <= full_dist, so if all 8 records
            // are already >= max_top, the complete distances can only be worse.
            __m256 partial = _mm256_mul_ps(_mm256_add_ps(acc0, acc1),
                                           _mm256_set1_ps(gssq));
            if (_mm256_movemask_ps(
                    _mm256_cmp_ps(partial, _mm256_set1_ps(max_top), _CMP_GE_OQ)
                ) == 0xFF) continue;

            KDIM(7,  vt7,  acc1);
            KDIM(8,  vt8,  acc0); KDIM(9,  vt9,  acc1);
            KDIM(10, vt10, acc0); KDIM(11, vt11, acc1);
            KDIM(12, vt12, acc0); KDIM(13, vt13, acc1);

            __m256 v = _mm256_mul_ps(_mm256_add_ps(acc0, acc1),
                                     _mm256_set1_ps(gssq));
            float dists[8];
            _mm256_storeu_ps(dists, v);
            for (int j = 0; j < b.n_valid; j++)
                update_top5(dists[j], (bool)b.is_fraud[j],
                            top_dists, top_frauds, max_top, sqrt_max_top);
        }
#undef KDIM
    }

    void search(int idx, const float* query, const uint16_t* tq,
                float* top_dists, bool* top_frauds,
                float& max_top, float& sqrt_max_top) const {
        const BallNode& n = nodes[idx];

        // Squared distance from query to ball center
        float dsq = 0;
        for (int d = 0; d < 14; d++) {
            float diff = query[d] - n.center[d];
            dsq += diff * diff;
        }
        // Prune without sqrt: (dist - radius)^2 >= max_top when dist > radius
        // is equivalent to: dsq >= (radius + sqrt_max_top)^2
        // sqrt_max_top is kept current by update_top5; conservative when stale
        // (never prunes incorrectly, just prunes less aggressively).
        float thr = n.radius + sqrt_max_top;
        if (dsq >= thr * thr) return;

        if (n.is_leaf) {
            search_leaf(n.leaf_start, n.leaf_count, tq,
                        top_dists, top_frauds, max_top, sqrt_max_top);
            return;
        }

        // Visit the child whose center is closest to query first
        // so max_top shrinks sooner → tighter pruning for the second child
        const BallNode& ln = nodes[n.left];
        const BallNode& rn = nodes[n.right];
        float ld = 0, rd = 0;
        for (int d = 0; d < 14; d++) {
            float ldf = query[d] - ln.center[d];
            float rdf = query[d] - rn.center[d];
            ld += ldf * ldf;
            rd += rdf * rdf;
        }
        if (ld <= rd) {
            search(n.left,  query, tq, top_dists, top_frauds, max_top, sqrt_max_top);
            search(n.right, query, tq, top_dists, top_frauds, max_top, sqrt_max_top);
        } else {
            search(n.right, query, tq, top_dists, top_frauds, max_top, sqrt_max_top);
            search(n.left,  query, tq, top_dists, top_frauds, max_top, sqrt_max_top);
        }
    }
};
