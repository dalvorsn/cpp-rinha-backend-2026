#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>

#include "flat_json.hpp"

struct StringHash {
  using is_transparent = void;
  size_t operator()(std::string_view sv) const {
    return std::hash<std::string_view>{}(sv);
  }
};

class Normalizer {
 public:
  float inv_max_amount;
  float inv_max_installments;
  float inv_amount_vs_avg_ratio;
  float inv_max_minutes;
  float inv_max_km;
  float inv_max_tx_count_24h;
  float inv_max_merchant_avg_amount;

  std::unordered_map<std::string, float, StringHash, std::equal_to<>> mcc_risk;

  Normalizer() {}

  bool load_config(const std::string& mcc_path, const std::string& norm_path) {
    if (!parse_flat_floats(mcc_path, [&](std::string_view k, float v) {
          mcc_risk.emplace(std::string(k), v);
        })) {
      std::cerr << "[ERR] failed to load mcc_risk.json\n";
      return false;
    }

    std::unordered_map<std::string, float, StringHash, std::equal_to<>> norm;
    if (!parse_flat_floats(norm_path, [&](std::string_view k, float v) {
          norm.emplace(std::string(k), v);
        })) {
      std::cerr << "[ERR] failed to load normalization.json\n";
      return false;
    }

    auto get = [&](const char* key) -> float {
      auto it = norm.find(std::string_view(key));
      return it != norm.end() ? it->second : 1.0f;
    };

    inv_max_amount = 1.0f / get("max_amount");
    inv_max_installments = 1.0f / get("max_installments");
    inv_amount_vs_avg_ratio = 1.0f / get("amount_vs_avg_ratio");
    inv_max_minutes = 1.0f / get("max_minutes");
    inv_max_km = 1.0f / get("max_km");
    inv_max_tx_count_24h = 1.0f / get("max_tx_count_24h");
    inv_max_merchant_avg_amount = 1.0f / get("max_merchant_avg_amount");

    return true;
  }

  static inline float clampf(float val) {
    if (val < 0.0f) return 0.0f;
    if (val > 1.0f) return 1.0f;
    return val;
  }

  static inline int64_t fast_epoch(int y, int m, int d, int hh, int mm,
                                   int ss) {
    if (m <= 2) {
      y--;
      m += 9;
    } else {
      m -= 3;
    }
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * m + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + (int64_t)doe - 719468;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
  }

  static inline int fast_weekday(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    int dow = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    return (dow + 6) % 7;
  }

  bool normalize_raw(const char* js, int jslen, int16_t* out) const noexcept {
    float vec[14];
    if (!parse_raw(js, jslen, vec)) return false;
    for (int i = 0; i < 14; ++i) {
      double v = (double)vec[i] * 10000.0;
      if (v < -10000.0) v = -10000.0;
      if (v > 10000.0) v = 10000.0;
      out[i] = (int16_t)std::llround(v);
    }
    return true;
  }

 private:
  static float fast_f32(const char* p) noexcept {
    uint32_t w = 0;
    while ((unsigned)(*p - '0') <= 9u) w = w * 10u + (uint32_t)(*p++ - '0');
    if (*p != '.') return (float)w;
    ++p;
    uint32_t f = 0, d = 1;
    while ((unsigned)(*p - '0') <= 9u && d < 100000000u) {
      f = f * 10u + (uint32_t)(*p++ - '0');
      d *= 10u;
    }
    return (float)w + (float)f / (float)d;
  }

  bool parse_raw(const char* js, int jslen, float* vec) const noexcept {
    const char *p = js, *end = js + jslen;
    enum Sec : uint8_t { ROOT, TX, CUST, MERCH, TERM, LAST };
    Sec sec = ROOT;
    int depth = 0;

    float tx_amount = 0, tx_inst = 0, cust_avg = 0, cnt24 = 0;
    float merch_avg = 0, km_home = 0, km_cur = 0;
    bool is_online = false, card_present = false, has_last = false;
    const char *rat = nullptr, *last_ts = nullptr;
    const char* mid = nullptr;
    int mid_len = 0;
    const char* mcc_p = nullptr;
    int mcc_len = 0;

    struct SV {
      const char* p;
      int n;
    };
    SV kms[32];
    int km_cnt = 0;

    while (p < end) {
      char c = *p++;
      if (c == '{') {
        ++depth;
        continue;
      }
      if (c == '}') {
        if (--depth == 1) sec = ROOT;
        continue;
      }
      if (c != '"') continue;

      const char* key = p;
      while (p < end && *p != '"') ++p;
      int klen = (int)(p - key);
      if (p < end) ++p;

      while (p < end && (*p == ' ' || *p == '\t')) ++p;
      if (p >= end || *p != ':') continue;
      ++p;
      while (p < end && (*p == ' ' || *p == '\t')) ++p;
      if (p >= end) return false;

#define KEY(lit) \
  (klen == (int)(sizeof(lit) - 1) && memcmp(key, lit, sizeof(lit) - 1) == 0)
      switch (sec) {
        case ROOT:
          if (KEY("transaction"))
            sec = TX;
          else if (KEY("customer"))
            sec = CUST;
          else if (KEY("merchant"))
            sec = MERCH;
          else if (KEY("terminal"))
            sec = TERM;
          else if (KEY("last_transaction") && *p != 'n') {
            has_last = true;
            sec = LAST;
          }
          break;
        case TX:
          if (KEY("amount"))
            tx_amount = fast_f32(p);
          else if (KEY("installments"))
            tx_inst = fast_f32(p);
          else if (KEY("requested_at"))
            rat = p + 1;
          break;
        case CUST:
          if (KEY("avg_amount"))
            cust_avg = fast_f32(p);
          else if (KEY("tx_count_24h"))
            cnt24 = fast_f32(p);
          else if (KEY("known_merchants") && *p == '[') {
            ++p;
            while (p < end && *p != ']') {
              while (p < end && *p != '"' && *p != ']') ++p;
              if (p >= end || *p == ']') break;
              const char* s = ++p;
              while (p < end && *p != '"') ++p;
              if (km_cnt < 32) kms[km_cnt++] = {s, (int)(p - s)};
              if (p < end) ++p;
            }
            if (p < end) ++p;
          }
          break;
        case MERCH:
          if (KEY("id")) {
            mid = p + 1;
            const char* q = mid;
            while (q < end && *q != '"') ++q;
            mid_len = (int)(q - mid);
          } else if (KEY("mcc")) {
            mcc_p = p + 1;
            const char* q = mcc_p;
            while (q < end && *q != '"') ++q;
            mcc_len = (int)(q - mcc_p);
          } else if (KEY("avg_amount")) {
            merch_avg = fast_f32(p);
          }
          break;
        case TERM:
          if (KEY("is_online"))
            is_online = (*p == 't');
          else if (KEY("card_present"))
            card_present = (*p == 't');
          else if (KEY("km_from_home"))
            km_home = fast_f32(p);
          break;
        case LAST:
          if (KEY("timestamp"))
            last_ts = p + 1;
          else if (KEY("km_from_current"))
            km_cur = fast_f32(p);
          break;
      }
#undef KEY
    }

    if (!rat || !mid) return false;

    int y = (rat[0] - '0') * 1000 + (rat[1] - '0') * 100 + (rat[2] - '0') * 10 +
            (rat[3] - '0');
    int mo = (rat[5] - '0') * 10 + (rat[6] - '0');
    int dy = (rat[8] - '0') * 10 + (rat[9] - '0');
    int hh = (rat[11] - '0') * 10 + (rat[12] - '0');
    int mi = (rat[14] - '0') * 10 + (rat[15] - '0');
    int ss = (rat[17] - '0') * 10 + (rat[18] - '0');

    vec[0] = clampf(tx_amount * inv_max_amount);
    vec[1] = clampf(tx_inst * inv_max_installments);
    vec[2] = clampf((tx_amount / (cust_avg > 0.0f ? cust_avg : 1.0f)) *
                    inv_amount_vs_avg_ratio);
    vec[3] = (float)hh * (1.0f / 23.0f);
    vec[4] = (float)fast_weekday(y, mo, dy) * (1.0f / 6.0f);

    if (!has_last || !last_ts) {
      vec[5] = -1.0f;
      vec[6] = -1.0f;
    } else {
      int ly = (last_ts[0] - '0') * 1000 + (last_ts[1] - '0') * 100 +
               (last_ts[2] - '0') * 10 + (last_ts[3] - '0');
      int lmo = (last_ts[5] - '0') * 10 + (last_ts[6] - '0');
      int ld = (last_ts[8] - '0') * 10 + (last_ts[9] - '0');
      int lhh = (last_ts[11] - '0') * 10 + (last_ts[12] - '0');
      int lmi = (last_ts[14] - '0') * 10 + (last_ts[15] - '0');
      int lss = (last_ts[17] - '0') * 10 + (last_ts[18] - '0');
      int64_t req_e = fast_epoch(y, mo, dy, hh, mi, ss);
      int64_t last_e = fast_epoch(ly, lmo, ld, lhh, lmi, lss);
      vec[5] = clampf((float)(req_e - last_e) / 60.0f * inv_max_minutes);
      vec[6] = clampf(km_cur * inv_max_km);
    }

    vec[7] = clampf(km_home * inv_max_km);
    vec[8] = clampf(cnt24 * inv_max_tx_count_24h);
    vec[9] = is_online ? 1.0f : 0.0f;
    vec[10] = card_present ? 1.0f : 0.0f;

    bool known = false;
    for (int i = 0; i < km_cnt && !known; ++i)
      known =
          (kms[i].n == mid_len && memcmp(kms[i].p, mid, (size_t)mid_len) == 0);
    vec[11] = known ? 0.0f : 1.0f;

    std::string_view sv(mcc_p ? mcc_p : "", (size_t)mcc_len);
    auto it = mcc_risk.find(sv);
    vec[12] = (it != mcc_risk.end()) ? it->second : 0.5f;

    vec[13] = clampf(merch_avg * inv_max_merchant_avg_amount);
    return true;
  }
};
