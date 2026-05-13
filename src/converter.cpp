#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "simdjson.h"
#include "types.hpp"

using namespace simdjson;

int main(int argc, char* argv[]) {
  if (argc != 3) {
    std::cerr << "Uso: " << argv[0] << " <input_json> <output_bin>\n";
    return 1;
  }

  ondemand::parser parser;
  padded_string json;
  auto error = padded_string::load(argv[1]).get(json);
  if (error) return 1;

  std::vector<VectorRecord> temp_records;
  temp_records.reserve(3000000);

  ondemand::document doc;
  error = parser.iterate(json).get(doc);
  if (error) return 1;

  float g_min = 1e18f, g_max = -1e18f;
  for (ondemand::object obj : doc.get_array()) {
    VectorRecord rec;
    int i = 0;
    for (double val : obj["vector"].get_array()) {
      if (i < 14) {
        float fval = (float)val;
        rec.dimensions[i] = fval;
        g_min = std::min(g_min, fval);
        g_max = std::max(g_max, fval);
      }
      i++;
    }
    std::string_view label = obj["label"].get_string();
    rec.dimensions[15] = (label == "fraud") ? 1.0f : 0.0f;
    temp_records.push_back(rec);
  }

  QuantParams qp;
  std::memset(&qp, 0, sizeof(qp));
  qp.min_global = g_min;
  qp.range_global = g_max - g_min;
  if (qp.range_global < 1e-9f) qp.range_global = 1.0f;

  std::sort(temp_records.begin(), temp_records.end(),
            [](const VectorRecord& a, const VectorRecord& b) {
              float na = 0, nb = 0;
              for (int i = 0; i < 14; ++i) {
                na += a.dimensions[i] * a.dimensions[i];
                nb += b.dimensions[i] * b.dimensions[i];
              }
              return na < nb;
            });

  std::vector<Block8> blocks;
  for (size_t i = 0; i < temp_records.size(); i += 8) {
    Block8 block;
    std::memset(&block, 0, sizeof(block));
    block.min_norm = 1e18f;
    block.max_norm = -1e18f;

    for (int j = 0; j < 8; ++j) {
      size_t idx =
          (i + j < temp_records.size()) ? (i + j) : (temp_records.size() - 1);
      const auto& rec = temp_records[idx];

      float f_sq_sum = 0;
      for (int d = 0; d < 14; ++d) {
        double q = (double)(rec.dimensions[d] - qp.min_global) /
                   (double)qp.range_global * 65535.0;
        block.dims[d][j] = (uint16_t)std::clamp(std::round(q), 0.0, 65535.0);
        f_sq_sum += rec.dimensions[d] * rec.dimensions[d];
      }
      float norm = std::sqrt(f_sq_sum);
      block.min_norm = std::min(block.min_norm, norm);
      block.max_norm = std::max(block.max_norm, norm);
      block.is_fraud[j] = (uint8_t)(rec.dimensions[15] > 0.5f);
    }
    blocks.push_back(block);
  }

  std::ofstream out(argv[2], std::ios::binary);
  out.write(reinterpret_cast<const char*>(&qp), sizeof(QuantParams));
  out.write(reinterpret_cast<const char*>(blocks.data()),
            blocks.size() * sizeof(Block8));
  return 0;
}
