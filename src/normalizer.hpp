#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "simdjson.h"

using namespace simdjson;

struct StringHash {
  using is_transparent = void;
  size_t operator()(std::string_view sv) const {
    return std::hash<std::string_view>{}(sv);
  }
};

class Normalizer {
 public:
  // Precomputed reciprocals: multiplying is faster than dividing
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
    dom::parser parser;

    // Load mcc_risk
    dom::element doc;
    auto error = parser.load(mcc_path).get(doc);
    if (error) {
      std::cerr << "[ERR] failed to load mcc_risk.json\n";
      return false;
    }

    for (auto field : doc.get_object()) {
      mcc_risk[std::string(field.key)] = (float)field.value.get_double();
    }

    // Load normalization constants and precompute reciprocals
    error = parser.load(norm_path).get(doc);
    if (error) {
      std::cerr << "[ERR] failed to load normalization.json\n";
      return false;
    }

    dom::object obj = doc.get_object();
    inv_max_amount = 1.0f / (float)obj["max_amount"].get_double();
    inv_max_installments = 1.0f / (float)obj["max_installments"].get_double();
    inv_amount_vs_avg_ratio =
        1.0f / (float)obj["amount_vs_avg_ratio"].get_double();
    inv_max_minutes = 1.0f / (float)obj["max_minutes"].get_double();
    inv_max_km = 1.0f / (float)obj["max_km"].get_double();
    inv_max_tx_count_24h = 1.0f / (float)obj["max_tx_count_24h"].get_double();
    inv_max_merchant_avg_amount =
        1.0f / (float)obj["max_merchant_avg_amount"].get_double();

    return true;
  }

  static inline float clampf(float val) {
    if (val < 0.0f) return 0.0f;
    if (val > 1.0f) return 1.0f;
    return val;
  }

  // Fast epoch without syscall: calculates seconds since 1970-01-01 using pure
  // arithmetic
  static inline int64_t fast_epoch(int y, int m, int d, int hh, int mm,
                                   int ss) {
    // Civil-to-epoch conversion algorithm (Howard Hinnant)
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

  // Weekday without syscall (0=Monday, 6=Sunday)
  static inline int fast_weekday(int y, int m, int d) {
    // Tomohiko Sakamoto's algorithm
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    int dow = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    // Convert from 0=Sunday to 0=Monday
    return (dow + 6) % 7;
  }

  void normalize(dom::element& req, int16_t* out) {
    float vec[14];
    normalize(req, vec);
    for (int i = 0; i < 14; i++) {
      // Use double + llround to match converter.cpp's quantize() exactly.
      double v = (double)vec[i] * 10000.0;
      if (v < -10000.0) v = -10000.0;
      if (v > 10000.0) v = 10000.0;
      out[i] = (int16_t)std::llround(v);
    }
  }

  void normalize(dom::element& req, float* vec) {
    dom::element transaction = req["transaction"];
    dom::element customer = req["customer"];
    dom::element merchant = req["merchant"];
    dom::element terminal = req["terminal"];
    dom::element last_tx_val = req["last_transaction"];

    float amount = (float)transaction["amount"].get_double();
    float installments = (float)transaction["installments"].get_double();
    std::string_view requested_at_str =
        transaction["requested_at"].get_string().value();

    float cust_avg_amount = (float)customer["avg_amount"].get_double();
    float tx_count_24h = (float)customer["tx_count_24h"].get_double();

    std::string_view merchant_id = merchant["id"].get_string().value();
    std::string_view mcc = merchant["mcc"].get_string().value();
    float merchant_avg_amount = (float)merchant["avg_amount"].get_double();

    bool is_online = terminal["is_online"].get_bool();
    bool card_present = terminal["card_present"].get_bool();
    float km_from_home = (float)terminal["km_from_home"].get_double();

    // 0. amount — no round4, ball tree doesn't need it
    vec[0] = clampf(amount * inv_max_amount);

    // 1. installments
    vec[1] = clampf(installments * inv_max_installments);

    // 2. amount_vs_avg
    float avg_safe = cust_avg_amount > 0.0f ? cust_avg_amount : 1.0f;
    vec[2] = clampf((amount / avg_safe) * inv_amount_vs_avg_ratio);

    // 3. hour_of_day & 4. day_of_week — parse inline without timegm
    int y = (requested_at_str[0] - '0') * 1000 +
            (requested_at_str[1] - '0') * 100 +
            (requested_at_str[2] - '0') * 10 + (requested_at_str[3] - '0');
    int mo = (requested_at_str[5] - '0') * 10 + (requested_at_str[6] - '0');
    int dy = (requested_at_str[8] - '0') * 10 + (requested_at_str[9] - '0');
    int hh = (requested_at_str[11] - '0') * 10 + (requested_at_str[12] - '0');
    int mi = (requested_at_str[14] - '0') * 10 + (requested_at_str[15] - '0');
    int ss = (requested_at_str[17] - '0') * 10 + (requested_at_str[18] - '0');

    vec[3] = (float)hh * (1.0f / 23.0f);
    vec[4] = (float)fast_weekday(y, mo, dy) * (1.0f / 6.0f);

    // 5. minutes_since_last_tx & 6. km_from_last_tx
    if (last_tx_val.is_null()) {
      vec[5] = -1.0f;
      vec[6] = -1.0f;
    } else {
      dom::object last_tx = last_tx_val.get_object();
      std::string_view last_ts_str = last_tx["timestamp"].get_string().value();
      float km_from_current = (float)last_tx["km_from_current"].get_double();

      int ly = (last_ts_str[0] - '0') * 1000 + (last_ts_str[1] - '0') * 100 +
               (last_ts_str[2] - '0') * 10 + (last_ts_str[3] - '0');
      int lmo = (last_ts_str[5] - '0') * 10 + (last_ts_str[6] - '0');
      int ld = (last_ts_str[8] - '0') * 10 + (last_ts_str[9] - '0');
      int lhh = (last_ts_str[11] - '0') * 10 + (last_ts_str[12] - '0');
      int lmi = (last_ts_str[14] - '0') * 10 + (last_ts_str[15] - '0');
      int lss = (last_ts_str[17] - '0') * 10 + (last_ts_str[18] - '0');

      int64_t req_epoch = fast_epoch(y, mo, dy, hh, mi, ss);
      int64_t last_epoch = fast_epoch(ly, lmo, ld, lhh, lmi, lss);
      float diff_minutes = (float)(req_epoch - last_epoch) / 60.0f;

      vec[5] = clampf(diff_minutes * inv_max_minutes);
      vec[6] = clampf(km_from_current * inv_max_km);
    }

    // 7. km_from_home
    vec[7] = clampf(km_from_home * inv_max_km);

    // 8. tx_count_24h
    vec[8] = clampf(tx_count_24h * inv_max_tx_count_24h);

    // 9. is_online & 10. card_present
    vec[9] = is_online ? 1.0f : 0.0f;
    vec[10] = card_present ? 1.0f : 0.0f;

    // 11. unknown_merchant
    bool known = false;
    for (auto km : customer["known_merchants"].get_array()) {
      if (km.get_string().value() == merchant_id) {
        known = true;
        break;
      }
    }
    vec[11] = known ? 0.0f : 1.0f;

    // 12. mcc_risk
    auto it = mcc_risk.find(mcc);
    vec[12] = it != mcc_risk.end() ? it->second : 0.5f;

    // 13. merchant_avg_amount
    vec[13] = clampf(merchant_avg_amount * inv_max_merchant_avg_amount);
  }

  // Hot path: specialized zero-copy parser for the rinha fraud-score payload.
  // Single forward pass, no tokenization stage, no DOM allocation.
  bool normalize_raw(const char *js, int jslen, int16_t *out) const noexcept {
    float vec[14];
    if (!parse_raw(js, jslen, vec)) return false;
    for (int i = 0; i < 14; ++i) {
      double v = (double)vec[i] * 10000.0;
      if (v < -10000.0) v = -10000.0;
      if (v > 10000.0)  v = 10000.0;
      out[i] = (int16_t)std::llround(v);
    }
    return true;
  }

 private:
  // Parse non-negative float without strtod.
  static float fast_f32(const char *p) noexcept {
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

  bool parse_raw(const char *js, int jslen, float *vec) const noexcept {
    const char *p = js, *end = js + jslen;
    enum Sec : uint8_t { ROOT, TX, CUST, MERCH, TERM, LAST };
    Sec sec = ROOT;
    int depth = 0;

    float tx_amount = 0, tx_inst = 0, cust_avg = 0, cnt24 = 0;
    float merch_avg = 0, km_home = 0, km_cur = 0;
    bool is_online = false, card_present = false, has_last = false;
    const char *rat = nullptr, *last_ts = nullptr;
    const char *mid = nullptr; int mid_len = 0;
    const char *mcc_p = nullptr; int mcc_len = 0;

    struct SV { const char *p; int n; };
    SV kms[32]; int km_cnt = 0;

    while (p < end) {
      char c = *p++;
      if (c == '{') { ++depth; continue; }
      if (c == '}') {
        if (--depth == 1) sec = ROOT;
        continue;
      }
      if (c != '"') continue;

      // Read key candidate up to next '"', then require ':'.
      const char *key = p;
      while (p < end && *p != '"') ++p;
      int klen = (int)(p - key);
      if (p < end) ++p;  // skip closing '"'

      while (p < end && (*p == ' ' || *p == '\t')) ++p;
      if (p >= end || *p != ':') continue;  // string value, not a key
      ++p;
      while (p < end && (*p == ' ' || *p == '\t')) ++p;
      if (p >= end) return false;

      // p points at the value. Dispatch by (section, key length, first char).
      switch (sec) {
      case ROOT:
        if      (klen == 11)                  sec = TX;    // "transaction"
        else if (klen ==  8 && key[0] == 'c') sec = CUST;  // "customer"
        else if (klen ==  8 && key[0] == 'm') sec = MERCH; // "merchant"
        else if (klen ==  8 && key[0] == 't') sec = TERM;  // "terminal"
        else if (klen == 16 && *p != 'n')   { has_last = true; sec = LAST; }
        break;                                              // "last_transaction"
      case TX:
        if      (klen ==  6)                  tx_amount = fast_f32(p);
        else if (klen == 12 && key[0] == 'i') tx_inst   = fast_f32(p);
        else if (klen == 12 && key[0] == 'r') rat       = p + 1;  // skip '"'
        break;
      case CUST:
        if      (klen == 10)                  cust_avg = fast_f32(p);
        else if (klen == 12)                  cnt24    = fast_f32(p);
        else if (klen == 15 && *p == '[') {   // "known_merchants"
          ++p;  // consume '['
          while (p < end && *p != ']') {
            while (p < end && *p != '"' && *p != ']') ++p;
            if (p >= end || *p == ']') break;
            const char *s = ++p;  // skip opening '"'
            while (p < end && *p != '"') ++p;
            if (km_cnt < 32) kms[km_cnt++] = {s, (int)(p - s)};
            if (p < end) ++p;  // skip closing '"'
          }
          if (p < end) ++p;  // consume ']'
        }
        break;
      case MERCH:
        if (klen == 2) {                      // "id"
          mid = p + 1;
          const char *q = mid;
          while (q < end && *q != '"') ++q;
          mid_len = (int)(q - mid);
        } else if (klen == 3) {               // "mcc"
          mcc_p = p + 1;
          const char *q = mcc_p;
          while (q < end && *q != '"') ++q;
          mcc_len = (int)(q - mcc_p);
        } else if (klen == 10) {              // "avg_amount"
          merch_avg = fast_f32(p);
        }
        break;
      case TERM:
        if      (klen ==  9)                  is_online    = (*p == 't');
        else if (klen == 12 && key[0] == 'c') card_present = (*p == 't');
        else if (klen == 12 && key[0] == 'k') km_home      = fast_f32(p);
        break;
      case LAST:
        if      (klen ==  9) last_ts = p + 1;  // "timestamp", skip '"'
        else if (klen == 15) km_cur  = fast_f32(p);
        break;
      }
    }

    if (!rat || !mid) return false;

    int y  = (rat[0]-'0')*1000+(rat[1]-'0')*100+(rat[2]-'0')*10+(rat[3]-'0');
    int mo = (rat[5]-'0')*10+(rat[6]-'0');
    int dy = (rat[8]-'0')*10+(rat[9]-'0');
    int hh = (rat[11]-'0')*10+(rat[12]-'0');
    int mi = (rat[14]-'0')*10+(rat[15]-'0');
    int ss = (rat[17]-'0')*10+(rat[18]-'0');

    vec[0] = clampf(tx_amount * inv_max_amount);
    vec[1] = clampf(tx_inst * inv_max_installments);
    vec[2] = clampf((tx_amount / (cust_avg > 0.0f ? cust_avg : 1.0f))
                    * inv_amount_vs_avg_ratio);
    vec[3] = (float)hh * (1.0f / 23.0f);
    vec[4] = (float)fast_weekday(y, mo, dy) * (1.0f / 6.0f);

    if (!has_last || !last_ts) {
      vec[5] = -1.0f;
      vec[6] = -1.0f;
    } else {
      int ly  = (last_ts[0]-'0')*1000+(last_ts[1]-'0')*100
               +(last_ts[2]-'0')*10+(last_ts[3]-'0');
      int lmo = (last_ts[5]-'0')*10+(last_ts[6]-'0');
      int ld  = (last_ts[8]-'0')*10+(last_ts[9]-'0');
      int lhh = (last_ts[11]-'0')*10+(last_ts[12]-'0');
      int lmi = (last_ts[14]-'0')*10+(last_ts[15]-'0');
      int lss = (last_ts[17]-'0')*10+(last_ts[18]-'0');
      int64_t req_e  = fast_epoch(y,  mo, dy,  hh, mi, ss);
      int64_t last_e = fast_epoch(ly, lmo, ld, lhh, lmi, lss);
      vec[5] = clampf((float)(req_e - last_e) / 60.0f * inv_max_minutes);
      vec[6] = clampf(km_cur * inv_max_km);
    }

    vec[7]  = clampf(km_home * inv_max_km);
    vec[8]  = clampf(cnt24 * inv_max_tx_count_24h);
    vec[9]  = is_online   ? 1.0f : 0.0f;
    vec[10] = card_present ? 1.0f : 0.0f;

    bool known = false;
    for (int i = 0; i < km_cnt && !known; ++i)
      known = (kms[i].n == mid_len &&
               memcmp(kms[i].p, mid, (size_t)mid_len) == 0);
    vec[11] = known ? 0.0f : 1.0f;

    std::string_view sv(mcc_p ? mcc_p : "", (size_t)mcc_len);
    auto it = mcc_risk.find(sv);
    vec[12] = (it != mcc_risk.end()) ? it->second : 0.5f;

    vec[13] = clampf(merch_avg * inv_max_merchant_avg_amount);
    return true;
  }
};
