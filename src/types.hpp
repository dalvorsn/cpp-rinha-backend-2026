#pragma once
#include <cstddef>
#include <cstdint>

static constexpr uint64_t IVF_MAGIC = 0x524832364956460AULL;  // "RH26IVF\n"
static constexpr uint32_t IVF_VERSION = 1;
static constexpr int IVF_DIMS = 14;
static constexpr int IVF_BLOCK = 8;
static constexpr int IVF_PAIRS = IVF_DIMS / 2;  // 7

struct alignas(64) IndexHeader {
  uint64_t magic;
  uint32_t version;
  uint32_t n;                // total vectors
  uint32_t k;                // number of clusters
  uint32_t total_blocks;     // total Block8s across all clusters
  uint32_t block_size;       // always IVF_BLOCK
  uint32_t dims;             // always IVF_DIMS
  uint32_t train_sample;     // k-means training sample size
  uint32_t train_iters;      // actual k-means iterations that ran
  uint32_t train_max_iters;  // configured max iterations
  uint32_t reserved[5];
};
static_assert(sizeof(IndexHeader) == 64);

struct IndexLayout {
  size_t centroids;  // k × DIMS × int16
  size_t bbox_min;   // k × DIMS × int16
  size_t bbox_max;   // k × DIMS × int16
  size_t offsets;    // (k+1) × uint32  — first block index per cluster
  size_t counts;     // k × uint32       — vector count per cluster
  size_t labels;     // total_blocks × BLOCK × uint8
  size_t blocks;     // total_blocks × DIMS × BLOCK × int16  (pair-SoA)
  size_t total;
};

inline size_t ivf_align(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

inline IndexLayout layout_for(uint32_t k, uint32_t total_blocks) {
  IndexLayout l{};
  size_t off = sizeof(IndexHeader);
  l.centroids = off;
  off += size_t(k) * IVF_DIMS * sizeof(int16_t);
  l.bbox_min = off;
  off += size_t(k) * IVF_DIMS * sizeof(int16_t);
  l.bbox_max = off;
  off += size_t(k) * IVF_DIMS * sizeof(int16_t);
  off = ivf_align(off, alignof(uint32_t));
  l.offsets = off;
  off += size_t(k + 1) * sizeof(uint32_t);
  l.counts = off;
  off += size_t(k) * sizeof(uint32_t);
  l.labels = off;
  off += size_t(total_blocks) * IVF_BLOCK;
  off = ivf_align(off, alignof(int16_t));
  l.blocks = off;
  off += size_t(total_blocks) * IVF_DIMS * IVF_BLOCK * sizeof(int16_t);
  l.total = off;
  return l;
}

// Offset within a block's int16 array for (dimension d, lane lane).
// Layout (pair-SoA): pair × BLOCK × 2 + lane × 2 + (d & 1)
inline size_t block_pair_offset(int d, int lane) {
  return size_t(d / 2) * IVF_BLOCK * 2 + size_t(lane) * 2 + size_t(d & 1);
}

// Label encoding:
//   bit 0 = fraud   (0=legit, 1=fraud)
//   bit 1 = borderline (0=clear, 1=borderline region)
//
// int16 scale: value = round(normalized × 10000)
//   q[0]=amount/10000  q[1]=installments/12  q[2]=(amount/avg)/10
//   q[7]=km_home/1000  q[8]=tx_count_24h/20  q[11]=unknown_merchant
//   q[12]=mcc_risk×10000  (safe: 1500/2000/2500/3000  risky: 7500/8000/8500)
inline bool is_borderline(const int16_t* q) {
  const bool safe_mcc =
      (q[12] == 1500 || q[12] == 3000 || q[12] == 2000 || q[12] == 2500);
  const bool obvious_legit = q[0] <= 500 && q[2] <= 500 && q[1] <= 2500 &&
                             q[8] <= 2500 && q[7] <= 500 &&
                             safe_mcc && q[11] == 0;
  if (obvious_legit) return false;

  const bool risky_mcc = (q[12] == 8500 || q[12] == 8000 || q[12] == 7500);
  const bool obvious_fraud = q[0] >= 5000 && q[1] >= 4167 && q[8] >= 3000 &&
                              q[7] >= 1500 && risky_mcc && q[11] == 10000;
  if (obvious_fraud) return false;

  return true;
}
