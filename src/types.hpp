#pragma once

#include <cstdint>

struct VectorRecord {
  float dimensions[16];

  inline bool get_is_fraud() const { return dimensions[15] > 0.5f; }
};

// Block8: 8 records per block = 256 bytes (4 cache lines)
struct alignas(64) Block8 {
  uint16_t dims[14][8];  // 224 bytes
  uint8_t is_fraud[8];   // 8 bytes
  float min_norm;        // 4 bytes
  float max_norm;        // 4 bytes
  uint8_t padding[16];   // Total: 256 bytes
};

struct QuantParams {
  float min_global;
  float range_global;
  uint8_t padding[56];  // 64 bytes total header
};
