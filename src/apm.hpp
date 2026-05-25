#pragma once

#ifdef ENABLE_APM
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "metrics_server.hpp"

class APM {
 public:
  static constexpr int MAX_METRICS = 16;
  static constexpr int MAX_GAUGES = 8;
  static constexpr int BUCKETS = 600;
  static constexpr int BUCKET_US = 1;
  static constexpr uint32_t FLUSH_EVERY = 10000;

  double cpu_limit_cores = -1.0;

  static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
  }

  void register_metric(const char* name, const char* help = "") {
    if (metric_count_ >= MAX_METRICS) return;
    auto& m = metrics_[metric_count_++];
    m.name = name;
    m.help = help;
  }

  void register_gauge(const char* name, const char* help = "") {
    if (gauge_count_ >= MAX_GAUGES) return;
    auto& g = gauges_[gauge_count_++];
    g.name = name;
    g.help = help;
    g.value.store(0, std::memory_order_relaxed);
  }

  void add_gauge(const char* name, int64_t delta) {
    for (int i = 0; i < gauge_count_; i++)
      if (gauges_[i].name == name || strcmp(gauges_[i].name, name) == 0) {
        gauges_[i].value.fetch_add(delta, std::memory_order_relaxed);
        return;
      }
  }

  void init() {
    window_start_ns_ = now_ns();
    refresh_buf();
  }

  void register_api_metrics() {
    register_metric("rinha_latency_ms",
                    "End-to-end request latency (parse+norm+ivf)");
    register_metric(
        "rinha_queue_ms",
        "Within-batch wait: epoll_wait returned to score_transaction called");
    register_metric(
        "rinha_arrival_wait_ms",
        "True wait: LB handoff (accept_from_lb) to score_transaction start");
    register_metric("rinha_normalize_ms", "JSON parse + normalize latency");
    register_metric("rinha_ivf_ms",
                    "IVF nearest-neighbor search latency (includes repair)");
    register_metric(
        "rinha_epoll_batch_size",
        "Events per epoll_wait return (encoded: push N*1000 => bucket[N])");
    register_gauge("rinha_active_conns", "Active client connections");
    read_cpu_limit_();
    init();
  }

  void register_lb_metrics(int n_backends, std::atomic<unsigned long>* conns,
                           std::atomic<unsigned long>* errors) {
    lb_backend_count_ = n_backends;
    lb_total_conns_ = conns;
    lb_connect_errors_ = errors;
    read_cpu_limit_();
    init();
  }

  void start_metrics_server(int port) {
    bool ok = ::start_metrics_server(
        port, [this](char* buf, int& len) { scrape(buf, len); });
    if (ok)
      fprintf(stderr, "[INFO] metrics on :%d\n", port);
    else
      fprintf(stderr, "[WARN] metrics socket on port %d failed\n", port);
  }

  void start_record(const char* name) {
    int idx = find(name);
    if (idx >= 0) tl_starts()[idx] = now_ns();
  }

  void stop_record(const char* name) {
    int idx = find(name);
    if (idx < 0) return;
    uint64_t t = tl_starts()[idx];
    if (t == 0) return;
    push(idx, now_ns() - t);
    tl_starts()[idx] = 0;
  }

  void push(const char* name, uint64_t lat_ns) {
    int idx = find(name);
    if (idx >= 0) push(idx, lat_ns);
  }

  void record_request() {
    ++total_reqs_;
    if (++window_reqs_ >= FLUSH_EVERY) log_flush();
  }

  void record_repair() { ++repair_reqs_; }

  void scrape(char* out, int& len) {
    std::lock_guard<std::mutex> lg(mutex_);
    refresh_buf();
    len = buf_len_;
    memcpy(out, buf_, len);
  }

 private:
  struct Metric {
    const char* name = nullptr;
    const char* help = nullptr;
    uint32_t buckets[BUCKETS]{};
    uint64_t sum_us = 0;
    uint64_t count = 0;
  };

  struct Gauge {
    const char* name = nullptr;
    const char* help = nullptr;
    std::atomic<int64_t> value{0};
  };

  Metric metrics_[MAX_METRICS]{};
  int metric_count_ = 0;
  Gauge gauges_[MAX_GAUGES]{};
  int gauge_count_ = 0;
  uint64_t window_start_ns_ = 0;
  uint32_t window_reqs_ = 0;
  uint64_t total_reqs_ = 0;
  uint64_t repair_reqs_ = 0;
  int lb_backend_count_ = 0;
  std::atomic<unsigned long>* lb_total_conns_ = nullptr;
  std::atomic<unsigned long>* lb_connect_errors_ = nullptr;
  char buf_[262144];
  int buf_len_ = 0;
  std::mutex mutex_;

  static uint64_t* tl_starts() {
    thread_local uint64_t s[MAX_METRICS]{};
    return s;
  }

  int find(const char* name) const {
    for (int i = 0; i < metric_count_; i++)
      if (metrics_[i].name == name) return i;
    for (int i = 0; i < metric_count_; i++)
      if (strcmp(metrics_[i].name, name) == 0) return i;
    return -1;
  }

  void push(int idx, uint64_t lat_ns) {
    auto& m = metrics_[idx];
    int b = (int)(lat_ns / ((uint64_t)BUCKET_US * 1000ULL));
    if (b >= BUCKETS) b = BUCKETS - 1;
    ++m.buckets[b];
    m.sum_us += lat_ns / 1000;
    ++m.count;
  }

  double pct(const Metric& m, double p) const {
    if (m.count == 0) return 0.0;
    uint64_t target = (uint64_t)((double)m.count * p);
    if (target == 0) target = 1;
    uint64_t cum = 0;
    for (int i = 0; i < BUCKETS; i++) {
      cum += m.buckets[i];
      if (cum >= target) return (double)(i + 1) * BUCKET_US / 1000.0;
    }
    return (double)BUCKETS * BUCKET_US / 1000.0;
  }

  void log_flush() {
    uint64_t now = now_ns();
    double elapsed = (now - window_start_ns_) / 1e9;
    window_start_ns_ = now;
    window_reqs_ = 0;

    char line[512];
    int n = snprintf(line, sizeof(line), "[APM] reqs=%lu rps=%.0f",
                     (unsigned long)total_reqs_,
                     elapsed > 0 ? (double)FLUSH_EVERY / elapsed : 0.0);
    for (int i = 0; i < metric_count_; i++)
      n += snprintf(line + n, (int)sizeof(line) - n, " | %s p99=%.3fms",
                    metrics_[i].name, pct(metrics_[i], 0.99));
    fprintf(stderr, "%s\n", line);
  }

  void emit_histogram(int& n, const Metric& m) {
    const int cap = (int)sizeof(buf_);
#define W(...) \
  if (n < cap) n += snprintf(buf_ + n, (size_t)(cap - n), __VA_ARGS__)
    W("# HELP %s %s\n# TYPE %s histogram\n", m.name, m.help, m.name);
    uint64_t cum = 0;
    for (int i = 0; i < BUCKETS; i++) {
      cum += m.buckets[i];
      W("%s_bucket{le=\"%.3f\"} %lu\n", m.name,
        (double)(i + 1) * BUCKET_US / 1000.0, (unsigned long)cum);
    }
    W("%s_bucket{le=\"+Inf\"} %lu\n%s_sum %.6f\n%s_count %lu\n", m.name,
      (unsigned long)m.count, m.name, m.sum_us / 1000.0, m.name,
      (unsigned long)m.count);
#undef W
  }

  void emit_lb_metrics(int& n) {
    if (!lb_total_conns_) return;
    const int cap = (int)sizeof(buf_);
#define W(...) \
  if (n < cap) n += snprintf(buf_ + n, (size_t)(cap - n), __VA_ARGS__)
    W("# HELP rinha_lb_connections_total Connections routed per backend\n"
      "# TYPE rinha_lb_connections_total counter\n");
    for (int i = 0; i < lb_backend_count_; i++)
      W("rinha_lb_connections_total{backend=\"%d\"} %lu\n", i,
        lb_total_conns_[i].load(std::memory_order_relaxed));
    W("# HELP rinha_lb_errors_total Failed handoff calls per backend\n"
      "# TYPE rinha_lb_errors_total counter\n");
    for (int i = 0; i < lb_backend_count_; i++)
      W("rinha_lb_errors_total{backend=\"%d\"} %lu\n", i,
        lb_connect_errors_[i].load(std::memory_order_relaxed));
#undef W
  }

  void append_cpu(int& n) {
    const int cap = (int)sizeof(buf_);
#define W(...) \
  if (n < cap) n += snprintf(buf_ + n, (size_t)(cap - n), __VA_ARGS__)
    {
      FILE* f = fopen("/sys/fs/cgroup/cpu.stat", "r");
      if (f) {
        uint64_t usage_usec = 0, user_usec = 0, system_usec = 0,
                 throttled_usec = 0;
        char key[64];
        uint64_t val;
        while (fscanf(f, "%63s %lu", key, &val) == 2) {
          if (!strcmp(key, "usage_usec"))
            usage_usec = val;
          else if (!strcmp(key, "user_usec"))
            user_usec = val;
          else if (!strcmp(key, "system_usec"))
            system_usec = val;
          else if (!strcmp(key, "throttled_usec"))
            throttled_usec = val;
        }
        fclose(f);
        W("# HELP rinha_cgroup_cpu_usage_seconds_total cgroup v2 CPU usage\n"
          "# TYPE rinha_cgroup_cpu_usage_seconds_total counter\n"
          "rinha_cgroup_cpu_usage_seconds_total{mode=\"user\"} %.6f\n"
          "rinha_cgroup_cpu_usage_seconds_total{mode=\"system\"} %.6f\n"
          "rinha_cgroup_cpu_usage_seconds_total{mode=\"total\"} %.6f\n",
          user_usec / 1e6, system_usec / 1e6, usage_usec / 1e6);
        W("# HELP rinha_cgroup_cpu_throttled_seconds_total cgroup v2 CPU "
          "throttle\n"
          "# TYPE rinha_cgroup_cpu_throttled_seconds_total counter\n"
          "rinha_cgroup_cpu_throttled_seconds_total %.6f\n",
          throttled_usec / 1e6);
      }
    }
    {
      FILE* f = fopen("/sys/fs/cgroup/memory.current", "r");
      if (f) {
        unsigned long mem = 0;
        fscanf(f, "%lu", &mem);
        fclose(f);
        W("# HELP rinha_memory_bytes_current cgroup memory usage\n"
          "# TYPE rinha_memory_bytes_current gauge\n"
          "rinha_memory_bytes_current %lu\n",
          mem);
      }
    }
    if (cpu_limit_cores > 0)
      W("# HELP rinha_cgroup_cpu_limit_cores cgroup CPU limit in cores\n"
        "# TYPE rinha_cgroup_cpu_limit_cores gauge\n"
        "rinha_cgroup_cpu_limit_cores %.6f\n",
        cpu_limit_cores);
#undef W
  }

  void refresh_buf() {
    int n = 0;
    for (int i = 0; i < metric_count_; i++) emit_histogram(n, metrics_[i]);
    const int cap = (int)sizeof(buf_);
#define W(...) \
  if (n < cap) n += snprintf(buf_ + n, (size_t)(cap - n), __VA_ARGS__)
    W("# HELP rinha_requests_total Total processed requests\n"
      "# TYPE rinha_requests_total counter\n"
      "rinha_requests_total %lu\n",
      (unsigned long)total_reqs_);
    W("# HELP rinha_repair_total Requests that triggered IVF repair\n"
      "# TYPE rinha_repair_total counter\n"
      "rinha_repair_total %lu\n",
      (unsigned long)repair_reqs_);
    for (int i = 0; i < gauge_count_; i++) {
      auto& g = gauges_[i];
      W("# HELP %s %s\n# TYPE %s gauge\n%s %ld\n", g.name, g.help ? g.help : "",
        g.name, g.name, (long)g.value.load(std::memory_order_relaxed));
    }
#undef W
    emit_lb_metrics(n);
    append_cpu(n);
    buf_len_ = n < cap ? n : cap - 1;
  }

  void read_cpu_limit_() {
    FILE* f = fopen("/sys/fs/cgroup/cpu.max", "r");
    if (!f) return;
    char quota_str[32];
    unsigned long period = 100000;
    if (fscanf(f, "%31s %lu", quota_str, &period) == 2 &&
        strcmp(quota_str, "max") != 0 && period > 0) {
      unsigned long quota = strtoul(quota_str, nullptr, 10);
      // NOLINTNEXTLINE(clang-analyzer-core.DivideZero)
      cpu_limit_cores = (double)quota / (double)period;
      fprintf(stderr, "[INFO] cgroup CPU limit: %.3f cores\n", cpu_limit_cores);
    }
    fclose(f);
  }
};

extern APM apm;

#else  // !ENABLE_APM

class APM {
 public:
  static uint64_t now_ns() { return 0; }
  void init() {}
  void register_api_metrics() {}
  void start_metrics_server(int) {}
  void register_metric(const char*, const char* = "") {}
  void register_gauge(const char*, const char* = "") {}
  void add_gauge(const char*, int64_t) {}
  void start_record(const char*) {}
  void stop_record(const char*) {}
  void push(const char*, uint64_t) {}
  void record_request() {}
  void record_repair() {}
  void scrape(char*, int& len) { len = 0; }
};

extern APM apm;

#endif  // ENABLE_APM
