#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "types.hpp"

// False positive transaction feature vector (int16, ×10000)
// [458, 4167, 1439, 3478, 3333, 639, 1123, 1681, 2000, 0, 10000, 0, 1500, 289]
static const int16_t QUERY[IVF_DIMS] = {
    458, 4167, 1439, 3478, 3333, 639, 1123, 1681, 2000, 0, 10000, 0, 1500, 289
};

struct Hit { float dist; uint32_t idx; uint8_t label; };

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <references.bin>\n", argv[0]);
        return 1;
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    struct stat st; fstat(fd, &st);
    size_t fsz = (size_t)st.st_size;
    void* data = malloc(fsz);
    if (!data) { perror("malloc"); return 1; }

    char* dst = (char*)data;
    size_t left = fsz;
    while (left > 0) {
        ssize_t r = read(fd, dst, left);
        if (r <= 0) { perror("read"); return 1; }
        dst += r; left -= r;
    }
    close(fd);

    const auto* hdr = (const IndexHeader*)data;
    printf("Index: n=%u k=%u total_blocks=%u\n", hdr->n, hdr->k, hdr->total_blocks);

    const IndexLayout lo = layout_for(hdr->k, hdr->total_blocks);
    const int16_t*  blocks     = (const int16_t*)((char*)data + lo.blocks);
    const uint8_t*  labels_arr = (const uint8_t*)((char*)data + lo.labels);
    const uint32_t* counts_arr = (const uint32_t*)((char*)data + lo.counts);
    const uint32_t* offsets_arr= (const uint32_t*)((char*)data + lo.offsets);

    // Brute-force: scan every stored vector, compute exact int16 squared distance
    const int TOP = 10;
    Hit top[TOP];
    for (int i = 0; i < TOP; i++) { top[i].dist = 1e30f; top[i].idx = 0; top[i].label = 0; }
    float worst = 1e30f;

    uint32_t vec_idx = 0;
    for (uint32_t c = 0; c < hdr->k; c++) {
        uint32_t blk_start = offsets_arr[c];
        uint32_t blk_end   = offsets_arr[c + 1];
        uint32_t pos = 0;
        for (uint32_t bi = blk_start; bi < blk_end; bi++) {
            const int16_t* blk = blocks + size_t(bi) * IVF_DIMS * IVF_BLOCK;
            const uint8_t* lbl = labels_arr + size_t(bi) * IVF_BLOCK;
            int n_valid = (int)std::min((uint32_t)IVF_BLOCK, counts_arr[c] - pos);
            for (int lane = 0; lane < n_valid; lane++) {
                float d = 0;
                for (int dim = 0; dim < IVF_DIMS; dim++) {
                    int16_t v = blk[block_pair_offset(dim, lane)];
                    float diff = (float)(QUERY[dim] - v);
                    d += diff * diff;
                }
                if (d < worst) {
                    // Replace worst slot
                    int wi = 0;
                    for (int i = 1; i < TOP; i++) if (top[i].dist > top[wi].dist) wi = i;
                    top[wi] = {d, vec_idx, lbl[lane]};
                    worst = 0;
                    for (int i = 0; i < TOP; i++) if (top[i].dist > worst) worst = top[i].dist;
                }
                vec_idx++;
            }
            pos += n_valid;
        }
    }

    // Sort by distance ascending
    std::sort(top, top + TOP, [](const Hit& a, const Hit& b){ return a.dist < b.dist; });

    printf("\nExact brute-force top-%d neighbors for false positive tx:\n", TOP);
    printf("  Query int16: [");
    for (int i = 0; i < IVF_DIMS; i++) printf("%d%s", QUERY[i], i<IVF_DIMS-1?", ":"");
    printf("]\n\n");

    int fraud_count = 0;
    for (int i = 0; i < TOP; i++) {
        const char* tag = (i < 5) ? "<<< top5" : "";
        printf("  [%d] dist_int16²=%-12.0f  dist_float=%.6f  label=%s  vec=%u  %s\n",
               i+1, top[i].dist,
               sqrtf(top[i].dist) / 10000.0f,
               top[i].label ? "FRAUD" : "legit",
               top[i].idx, tag);
        if (i < 5 && top[i].label) fraud_count++;
    }
    printf("\nfinal fraud_count=%d → %s\n",
           fraud_count, fraud_count >= 3 ? "approved:false (fraud)" : "approved:true (legit)");

    free(data);
    return 0;
}
