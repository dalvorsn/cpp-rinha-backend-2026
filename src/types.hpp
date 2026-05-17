#pragma once
#include <cstdint>

struct QuantParams {
    float   min_global;
    float   range_global;
    uint8_t padding[56]; // 64 bytes total
};

// 8 records packed column-major for AVX2 distance computation
struct alignas(64) Block8 {
    uint16_t dims[14][8]; // 224 bytes
    uint8_t  is_fraud[8]; //   8 bytes
    uint8_t  n_valid;     //   1 byte  (valid entries: 1-8)
    uint8_t  padding[23]; //  23 bytes → 256 bytes total
};
static_assert(sizeof(Block8) == 256);

// Ball tree node — 128 bytes (2 cache lines)
struct alignas(64) BallNode {
    float   center[14];   // 56 bytes: ball center in float space
    float   radius;       //  4 bytes: bounding ball radius
    int32_t left;         //  4 bytes: left child index  (-1 if leaf)
    int32_t right;        //  4 bytes: right child index (-1 if leaf)
    int32_t leaf_start;   //  4 bytes: first Block8 index (leaf only)
    int32_t leaf_count;   //  4 bytes: number of Block8 blocks (leaf only)
    uint8_t is_leaf;      //  1 byte
    uint8_t padding[51];  // 51 bytes → 128 bytes total
};
static_assert(sizeof(BallNode) == 128);

struct BallTreeHeader {
    uint32_t magic;          // 0xBA11BEEF
    uint32_t num_nodes;
    uint32_t num_blocks;
    float    qp_min_global;
    float    qp_range_global;
    float    global_scale_sq;
    uint8_t  padding[40];    // 64 bytes total
};
static_assert(sizeof(BallTreeHeader) == 64);

// Used only during index build
struct VectorRecord {
    float dimensions[16]; // [0..13] = feature dims, [15] = is_fraud flag
};
