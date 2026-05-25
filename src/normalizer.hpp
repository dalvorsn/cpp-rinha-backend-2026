#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "jsmn.h"

struct StringHash {
  using is_transparent = void;
  size_t operator()(std::string_view sv) const {
    return std::hash<std::string_view>{}(sv);
  }
};

// ---------------------------------------------------------------------------
// jsmn tree helpers
// ---------------------------------------------------------------------------

static inline int jsmn_subtree(const jsmntok_t *t, int ntoks, int idx) {
  if (idx >= ntoks) return 0;
  int n = 1;
  int pos = idx + 1;
  for (int i = 0; i < t[idx].size; i++) {
    int s = jsmn_subtree(t, ntoks, pos);
    pos += s;
    n += s;
  }
  return n;
}

// Returns token index of the value for 'key' inside OBJECT at toks[obj],
// or -1 if not found.
static inline int jsmn_find(const char *js, const jsmntok_t *t, int ntoks,
                             int obj, const char *key, int klen) {
  if (obj < 0 || obj >= ntoks || t[obj].type != JSMN_OBJECT) return -1;
  int pos = obj + 1;
  for (int i = 0; i < t[obj].size && pos < ntoks; i++) {
    if (t[pos].type == JSMN_STRING && t[pos].end - t[pos].start == klen &&
        memcmp(js + t[pos].start, key, klen) == 0)
      return pos + 1;
    pos += jsmn_subtree(t, ntoks, pos);
  }
  return -1;
}

static inline float jsmn_f32(const char *js, const jsmntok_t *t, int i) {
  return (float)strtod(js + t[i].start, nullptr);
}

static inline bool jsmn_b(const char *js, const jsmntok_t *t, int i) {
  return js[t[i].start] == 't';
}

// ---------------------------------------------------------------------------

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

  Normalizer() = default;

  bool load_config(const std::string &mcc_path,
                   const std::string &norm_path) noexcept {
    // ── mcc_risk.json ─────────────────────────────────────────────────────
    {
      std::vector<char> buf;
      if (!read_file(mcc_path, buf)) {
        fprintf(stderr, "[ERR] failed to open %s\n", mcc_path.c_str());
        return false;
      }
      // ≤10000 MCC codes → ≤20001 tokens (root obj + 2 per entry)
      std::vector<jsmntok_t> toks(20002);
      jsmn_parser p; jsmn_init(&p);
      int ntoks = jsmn_parse(&p, buf.data(), buf.size() - 1,
                             toks.data(), (unsigned)toks.size());
      if (ntoks < 1 || toks[0].type != JSMN_OBJECT) {
        fprintf(stderr, "[ERR] bad mcc_risk.json\n");
        return false;
      }
      int pos = 1;
      for (int i = 0; i < toks[0].size && pos + 1 < ntoks; i++) {
        const jsmntok_t &k = toks[pos], &v = toks[pos + 1];
        if (k.type == JSMN_STRING && v.type == JSMN_PRIMITIVE)
          mcc_risk[std::string(buf.data() + k.start, k.end - k.start)] =
              (float)strtod(buf.data() + v.start, nullptr);
        pos += jsmn_subtree(toks.data(), ntoks, pos);
      }
    }

    // ── normalization.json ────────────────────────────────────────────────
    {
      std::vector<char> buf;
      if (!read_file(norm_path, buf)) {
        fprintf(stderr, "[ERR] failed to open %s\n", norm_path.c_str());
        return false;
      }
      jsmntok_t toks[32]; jsmn_parser p; jsmn_init(&p);
      int ntoks = jsmn_parse(&p, buf.data(), buf.size() - 1, toks, 32);
      if (ntoks < 1 || toks[0].type != JSMN_OBJECT) {
        fprintf(stderr, "[ERR] bad normalization.json\n");
        return false;
      }
#define LI(field, key)                                                        \
  {                                                                            \
    int vi = jsmn_find(buf.data(), toks, ntoks, 0, key, (int)sizeof(key)-1); \
    if (vi < 0) { fprintf(stderr, "[ERR] missing " key "\n"); return false; } \
    field = 1.0f / jsmn_f32(buf.data(), toks, vi);                           \
  }
      LI(inv_max_amount,              "max_amount")
      LI(inv_max_installments,        "max_installments")
      LI(inv_amount_vs_avg_ratio,     "amount_vs_avg_ratio")
      LI(inv_max_minutes,             "max_minutes")
      LI(inv_max_km,                  "max_km")
      LI(inv_max_tx_count_24h,        "max_tx_count_24h")
      LI(inv_max_merchant_avg_amount, "max_merchant_avg_amount")
#undef LI
    }
    return true;
  }

  // Hot path: parse + normalize in one call. Returns false on malformed JSON.
  bool normalize(const char *js, int jslen, int16_t *out) const noexcept {
    float vec[14];
    if (!normalize_f(js, jslen, vec)) return false;
    for (int i = 0; i < 14; i++) {
      double v = (double)vec[i] * 10000.0;
      if (v < -10000.0) v = -10000.0;
      if (v > 10000.0) v = 10000.0;
      out[i] = (int16_t)std::llround(v);
    }
    return true;
  }

  bool normalize_f(const char *js, int jslen, float *vec) const noexcept {
    jsmntok_t toks[128]; jsmn_parser p; jsmn_init(&p);
    int ntoks = jsmn_parse(&p, js, (size_t)jslen, toks, 128);
    if (ntoks < 1 || toks[0].type != JSMN_OBJECT) return false;

    // transaction
    int tx = jsmn_find(js, toks, ntoks, 0, "transaction", 11);
    if (tx < 0) return false;
    int amt_i  = jsmn_find(js, toks, ntoks, tx, "amount",       6);
    int inst_i = jsmn_find(js, toks, ntoks, tx, "installments", 12);
    int rat_i  = jsmn_find(js, toks, ntoks, tx, "requested_at", 12);
    if (amt_i < 0 || inst_i < 0 || rat_i < 0) return false;

    float amount       = jsmn_f32(js, toks, amt_i);
    float installments = jsmn_f32(js, toks, inst_i);
    const char *rat    = js + toks[rat_i].start;

    // customer
    int cust = jsmn_find(js, toks, ntoks, 0, "customer", 8);
    if (cust < 0) return false;
    int cavg_i  = jsmn_find(js, toks, ntoks, cust, "avg_amount",   10);
    int cnt24_i = jsmn_find(js, toks, ntoks, cust, "tx_count_24h", 12);
    if (cavg_i < 0 || cnt24_i < 0) return false;

    float cust_avg = jsmn_f32(js, toks, cavg_i);
    float cnt24    = jsmn_f32(js, toks, cnt24_i);

    // merchant
    int merch = jsmn_find(js, toks, ntoks, 0, "merchant", 8);
    if (merch < 0) return false;
    int mid_i  = jsmn_find(js, toks, ntoks, merch, "id",         2);
    int mcc_i  = jsmn_find(js, toks, ntoks, merch, "mcc",        3);
    int mavg_i = jsmn_find(js, toks, ntoks, merch, "avg_amount", 10);
    if (mid_i < 0 || mcc_i < 0 || mavg_i < 0) return false;

    // terminal
    int term = jsmn_find(js, toks, ntoks, 0, "terminal", 8);
    if (term < 0) return false;
    int ionl_i  = jsmn_find(js, toks, ntoks, term, "is_online",    9);
    int cpres_i = jsmn_find(js, toks, ntoks, term, "card_present", 12);
    int kmh_i   = jsmn_find(js, toks, ntoks, term, "km_from_home", 12);
    if (ionl_i < 0 || cpres_i < 0 || kmh_i < 0) return false;

    // requested_at: "YYYY-MM-DDTHH:MM:SSZ"
    int y  = (rat[0]-'0')*1000+(rat[1]-'0')*100+(rat[2]-'0')*10+(rat[3]-'0');
    int mo = (rat[5]-'0')*10+(rat[6]-'0');
    int dy = (rat[8]-'0')*10+(rat[9]-'0');
    int hh = (rat[11]-'0')*10+(rat[12]-'0');
    int mi = (rat[14]-'0')*10+(rat[15]-'0');
    int ss = (rat[17]-'0')*10+(rat[18]-'0');

    vec[0] = clampf(amount * inv_max_amount);
    vec[1] = clampf(installments * inv_max_installments);
    {
      float safe = cust_avg > 0.0f ? cust_avg : 1.0f;
      vec[2] = clampf((amount / safe) * inv_amount_vs_avg_ratio);
    }
    vec[3] = (float)hh * (1.0f / 23.0f);
    vec[4] = (float)fast_weekday(y, mo, dy) * (1.0f / 6.0f);

    // last_transaction (nullable)
    int lt    = jsmn_find(js, toks, ntoks, 0, "last_transaction", 16);
    bool has_lt = (lt >= 0 && lt < ntoks && toks[lt].type == JSMN_OBJECT);
    if (!has_lt) {
      vec[5] = -1.0f; vec[6] = -1.0f;
    } else {
      int lts_i = jsmn_find(js, toks, ntoks, lt, "timestamp",       9);
      int lkm_i = jsmn_find(js, toks, ntoks, lt, "km_from_current", 15);
      if (lts_i < 0 || lkm_i < 0) {
        vec[5] = -1.0f; vec[6] = -1.0f;
      } else {
        const char *lts = js + toks[lts_i].start;
        int ly  = (lts[0]-'0')*1000+(lts[1]-'0')*100+(lts[2]-'0')*10+(lts[3]-'0');
        int lmo = (lts[5]-'0')*10+(lts[6]-'0');
        int ld  = (lts[8]-'0')*10+(lts[9]-'0');
        int lhh = (lts[11]-'0')*10+(lts[12]-'0');
        int lmi = (lts[14]-'0')*10+(lts[15]-'0');
        int lss = (lts[17]-'0')*10+(lts[18]-'0');

        int64_t req_e  = fast_epoch(y,  mo,  dy,  hh,  mi,  ss);
        int64_t last_e = fast_epoch(ly, lmo, ld,  lhh, lmi, lss);
        vec[5] = clampf((float)(req_e - last_e) / 60.0f * inv_max_minutes);
        vec[6] = clampf(jsmn_f32(js, toks, lkm_i) * inv_max_km);
      }
    }

    vec[7]  = clampf(jsmn_f32(js, toks, kmh_i) * inv_max_km);
    vec[8]  = clampf(cnt24 * inv_max_tx_count_24h);
    vec[9]  = jsmn_b(js, toks, ionl_i)  ? 1.0f : 0.0f;
    vec[10] = jsmn_b(js, toks, cpres_i) ? 1.0f : 0.0f;

    // known_merchants: linear scan
    bool known = false;
    int kma = jsmn_find(js, toks, ntoks, cust, "known_merchants", 15);
    if (kma >= 0 && kma < ntoks && toks[kma].type == JSMN_ARRAY) {
      int n_km = toks[kma].size;
      int pos  = kma + 1;
      int ms = toks[mid_i].start, ml = toks[mid_i].end - toks[mid_i].start;
      for (int i = 0; i < n_km && pos < ntoks; i++) {
        const jsmntok_t &m = toks[pos];
        if (m.type == JSMN_STRING && m.end - m.start == ml &&
            memcmp(js + m.start, js + ms, ml) == 0) {
          known = true; break;
        }
        pos += jsmn_subtree(toks, ntoks, pos);
      }
    }
    vec[11] = known ? 0.0f : 1.0f;

    std::string_view mcc_sv(js + toks[mcc_i].start,
                             toks[mcc_i].end - toks[mcc_i].start);
    auto it = mcc_risk.find(mcc_sv);
    vec[12] = (it != mcc_risk.end()) ? it->second : 0.5f;

    vec[13] = clampf(jsmn_f32(js, toks, mavg_i) * inv_max_merchant_avg_amount);
    return true;
  }

 private:
  static bool read_file(const std::string &path,
                        std::vector<char> &out) noexcept {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    out.resize((size_t)sz + 1);
    fread(out.data(), 1, (size_t)sz, f);
    fclose(f);
    out[(size_t)sz] = '\0';
    return true;
  }

  static float clampf(float v) noexcept {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
  }

  static int64_t fast_epoch(int y, int m, int d, int hh, int mm,
                             int ss) noexcept {
    if (m <= 2) { y--; m += 9; } else { m -= 3; }
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * m + 2) / 5 + (unsigned)d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + (int64_t)doe - 719468;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
  }

  static int fast_weekday(int y, int m, int d) noexcept {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    return ((y + y/4 - y/100 + y/400 + t[m-1] + d) % 7 + 6) % 7;
  }
};
