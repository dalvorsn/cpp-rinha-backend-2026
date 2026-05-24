#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/ioctl.h>

#include "apm.hpp"
#include "ivf.hpp"
#include "normalizer.hpp"
#include "simdjson.h"

using namespace simdjson;

APM apm;

// ---------------------------------------------------------------------------
// Pre-rendered HTTP responses
// ---------------------------------------------------------------------------
static const char RESP_HDRS[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Connection: keep-alive\r\n";

static const char* const BODIES[6] = {
    "{\"approved\": true, \"fraud_score\": 0.00}",
    "{\"approved\": true, \"fraud_score\": 0.20}",
    "{\"approved\": true, \"fraud_score\": 0.40}",
    "{\"approved\": false, \"fraud_score\": 0.60}",
    "{\"approved\": false, \"fraud_score\": 0.80}",
    "{\"approved\": false, \"fraud_score\": 1.00}",
};

static char full_resp[6][256];
static int full_resp_len[6];

static const char READY_RESP[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nOK";
static const int READY_RESP_LEN = sizeof(READY_RESP) - 1;

static const char ERR_RESP[] =
    "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: "
    "close\r\n\r\n";
static const int ERR_RESP_LEN = sizeof(ERR_RESP) - 1;

// ---------------------------------------------------------------------------
// Per-connection state
// ---------------------------------------------------------------------------
static constexpr int BUF_CAP = 8192;
static constexpr int MAX_FDS = 65536;
static constexpr int MAX_EVTS = 512;

struct Conn {
  char buf[BUF_CAP];
  int buf_len = 0;
  int headers_size = 0;
  int content_len = 0;
  bool is_metrics = false;
  const char* send_ptr = nullptr;
  int send_len = 0;
  int send_off = 0;
  uint64_t req_start_ns = 0;  // set when first byte of current request arrives
  uint64_t ready_ns =
      0;  // set when epoll_wait returns with EPOLLIN for this fd
};

static Conn* conns[MAX_FDS] = {};
static IVF* ivf = nullptr;
static Normalizer normalizer;
static int g_repair_min = 2;
static int g_repair_max = 3;
static int g_epoll_timeout_ms = 1;

// epoll busy-poll params (Linux 6.0+, EPIOCSPARAMS ioctl)
#ifndef EPIOCSPARAMS
struct epoll_params {
  uint32_t busy_poll_usecs;
  uint16_t busy_poll_budget;
  uint8_t  prefer_busy_poll;
  uint8_t  __pad;
};
#define EPIOCSPARAMS _IOW('p', 0x01, struct epoll_params)
#endif
static struct epoll_params g_epoll_params = {};
static int epfd = -1;
static int ctrl_listen_fd = -1;
#ifdef ENABLE_APM
static int metrics_listen_fd = -1;
#endif
static bool is_ctrl_fd[MAX_FDS];

static Conn* get_conn(int fd) {
  if (fd < 0 || fd >= MAX_FDS) return nullptr;
  if (!conns[fd]) conns[fd] = new Conn;
  auto* c = conns[fd];
  c->buf_len = 0;
  c->headers_size = 0;
  c->content_len = 0;
  c->is_metrics = false;
  c->send_ptr = nullptr;
  c->send_len = 0;
  c->send_off = 0;
  c->req_start_ns = 0;
  return c;
}

static void drop_conn(int fd) {
  epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
#ifdef ENABLE_APM
  if (fd >= 0 && fd < MAX_FDS && conns[fd] && !conns[fd]->is_metrics)
    apm.addGauge("rinha_active_conns", -1);
#endif
  close(fd);
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------
static int parse_content_length(const char* hdr, int hdr_len) {
  const char* p = hdr;
  const char* end = hdr + hdr_len;
  while (p < end - 16) {
    if ((*p == 'C' || *p == 'c') &&
        strncasecmp(p, "Content-Length:", 15) == 0) {
      p += 15;
      while (p < end && *p == ' ') p++;
      return (int)strtol(p, nullptr, 10);
    }
    while (p < end && *p != '\n') p++;
    p++;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Receive one fd via SCM_RIGHTS
// ---------------------------------------------------------------------------
static int recv_fd(int sock) {
  char buf[1];
  struct iovec iov = {buf, 1};
  char cmsg_buf[CMSG_SPACE(sizeof(int))];
  struct msghdr msg = {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsg_buf;
  msg.msg_controllen = sizeof(cmsg_buf);

  ssize_t r = recvmsg(sock, &msg, 0);
  if (r < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      fprintf(stderr, "[ERR] recvmsg: %s\n", strerror(errno));
    return -1;
  }
  if (r == 0) {
    fprintf(stderr, "[ERR] ctrl closed by LB\n");
    return -1;
  }

  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) {
    fprintf(stderr, "[ERR] recv_fd: bad cmsg\n");
    return -1;
  }

  int new_fd;
  memcpy(&new_fd, CMSG_DATA(cmsg), sizeof(int));
  return new_fd;
}

// ---------------------------------------------------------------------------
// Accept new ctrl connection from an lb thread
// ---------------------------------------------------------------------------
static void accept_ctrl_conn() {
  for (;;) {
    int cfd = accept4(ctrl_listen_fd, nullptr, nullptr, SOCK_NONBLOCK);
    if (cfd < 0) return;
    if (cfd >= MAX_FDS) {
      close(cfd);
      continue;
    }
    is_ctrl_fd[cfd] = true;
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = cfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
  }
}

// ---------------------------------------------------------------------------
// Drain all fds from LB via SCM_RIGHTS
// ---------------------------------------------------------------------------
static void accept_from_lb(int ctrl) {
  while (true) {
    int cfd = recv_fd(ctrl);
    if (cfd < 0) return;

    if (cfd >= MAX_FDS) {
      fprintf(stderr, "[ERR] fd=%d >= MAX_FDS\n", cfd);
      close(cfd);
      continue;
    }

    int flags = fcntl(cfd, F_GETFL, 0);
    if (flags >= 0) fcntl(cfd, F_SETFL, flags | O_NONBLOCK);

    int yes = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    setsockopt(cfd, IPPROTO_TCP, TCP_QUICKACK, &yes, sizeof(yes));

    get_conn(cfd);
#ifdef ENABLE_APM
    if (conns[cfd]) {
      conns[cfd]->req_start_ns = APM::now_ns();
      apm.addGauge("rinha_active_conns", 1);
    }
#endif

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = cfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev) < 0) {
      fprintf(stderr, "[ERR] epoll_ctl ADD fd=%d: %s\n", cfd, strerror(errno));
      drop_conn(cfd);
    }
  }
}

#ifdef ENABLE_APM
// ---------------------------------------------------------------------------
// Metrics connections — accept and serve Prometheus text format on scrape
// ---------------------------------------------------------------------------
static void accept_metrics_conn() {
  int cfd = accept4(metrics_listen_fd, nullptr, nullptr, SOCK_NONBLOCK);
  if (cfd < 0) return;
  if (cfd >= MAX_FDS) {
    close(cfd);
    return;
  }
  Conn* c = get_conn(cfd);
  c->is_metrics = true;
  struct epoll_event ev;
  ev.events = EPOLLIN | EPOLLRDHUP;
  ev.data.fd = cfd;
  epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
}

static void handle_metrics_read(int fd, Conn* c) {
  int room = BUF_CAP - c->buf_len;
  if (room <= 0) {
    drop_conn(fd);
    return;
  }
  ssize_t n = recv(fd, c->buf + c->buf_len, room, 0);
  if (n <= 0) {
    drop_conn(fd);
    return;
  }
  c->buf_len += (int)n;
  if (!memmem(c->buf, c->buf_len, "\r\n\r\n", 4)) return;

  static char snap[262144];
  int snap_len;
  apm.scrape(snap, snap_len);
  char hdr[128];
  int hdr_len = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 200 OK\r\n"
                         "Content-Type: text/plain; version=0.0.4\r\n"
                         "Content-Length: %d\r\n\r\n",
                         snap_len);
  // Switch to blocking so the full response is sent before close
  int fl = fcntl(fd, F_GETFL);
  fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
  send(fd, hdr, hdr_len, MSG_NOSIGNAL);
  send(fd, snap, snap_len, MSG_NOSIGNAL);
  drop_conn(fd);
}
#endif  // ENABLE_APM

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Handle readable/writable client fds
// ---------------------------------------------------------------------------
// Returns 0–5 (response index) or 6 (error).
static int score_transaction(Conn* c) {
  thread_local dom::parser parser;
  const char* body = c->buf + c->headers_size;
  int blen = c->content_len;
  dom::element doc;

#ifdef ENABLE_APM
  {
    uint64_t t0 = APM::now_ns();
    if (c->ready_ns && t0 > c->ready_ns)
      apm.push("rinha_queue_ms", t0 - c->ready_ns);
    if (c->req_start_ns && t0 > c->req_start_ns) {
      apm.push("rinha_arrival_wait_ms", t0 - c->req_start_ns);
      c->req_start_ns = 0;
    }
  }
  apm.startRecord("rinha_latency_ms");
  apm.startRecord("rinha_parse_ms");
#endif

  bool ok;
  if (c->headers_size + blen + (int)SIMDJSON_PADDING <= BUF_CAP) {
    memset(c->buf + c->headers_size + blen, 0, SIMDJSON_PADDING);
    ok = !parser.parse(body, (size_t)blen, false).get(doc);
  } else {
    padded_string padded(body, (size_t)blen);
    ok = !parser.parse(padded).get(doc);
  }

#ifdef ENABLE_APM
  apm.stopRecord("rinha_parse_ms");
#endif

  if (!ok) return 6;

  try {
    int16_t vec[14];
#ifdef ENABLE_APM
    apm.startRecord("rinha_normalize_ms");
#endif
    normalizer.normalize(doc, vec);
#ifdef ENABLE_APM
    apm.stopRecord("rinha_normalize_ms");
    apm.startRecord("rinha_ivf_ms");
#endif
    bool repaired = false;
    int cnt = ivf->get_fraud_count(vec, &repaired);
#ifdef ENABLE_APM
    if (repaired) apm.recordRepair();
#endif
#ifdef ENABLE_APM
    apm.stopRecord("rinha_ivf_ms");
    apm.stopRecord("rinha_latency_ms");
    apm.recordRequest();
#endif
    return cnt;
  } catch (...) {
    fprintf(stderr, "[ERR] ivf/normalize threw\n");
    return 6;
  }
}

static void send_response(int fd, Conn* c, int score) {
  const char* resp;
  int resp_len;
  if (score <= 5) {
    resp = full_resp[score];
    resp_len = full_resp_len[score];
  } else {
    resp = ERR_RESP;
    resp_len = ERR_RESP_LEN;
  }

  ssize_t sent = send(fd, resp, resp_len, MSG_NOSIGNAL);
  if (sent < 0) {
    if (errno != EAGAIN) drop_conn(fd);
    return;
  }
  if (sent < resp_len) {
    c->send_ptr = resp;
    c->send_len = resp_len;
    c->send_off = (int)sent;
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
  }
}

static void handle_client_read(int fd) {
  Conn* c = (fd >= 0 && fd < MAX_FDS) ? conns[fd] : nullptr;
  if (!c) {
    drop_conn(fd);
    return;
  }

#ifdef ENABLE_APM
  if (c->is_metrics) {
    handle_metrics_read(fd, c);
    return;
  }
#endif

  int room = BUF_CAP - c->buf_len;
  if (room <= 0) {
    drop_conn(fd);
    return;
  }

  ssize_t n = recv(fd, c->buf + c->buf_len, room, 0);
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    drop_conn(fd);
    return;
  }
  if (n == 0) {
    drop_conn(fd);
    return;
  }

  c->buf_len += (int)n;

  if (c->headers_size == 0) {
    const char* hdrend = (const char*)memmem(c->buf, c->buf_len, "\r\n\r\n", 4);
    if (!hdrend) return;
    c->headers_size = (int)(hdrend - c->buf) + 4;
    if (c->buf[0] == 'P')
      c->content_len = parse_content_length(c->buf, c->headers_size);
  }

  const bool is_post = c->buf[0] == 'P';
  if (is_post && c->buf_len - c->headers_size < c->content_len) return;

  int score = is_post ? score_transaction(c) : -1;

  int consumed = c->headers_size + (is_post ? c->content_len : 0);
  int leftover = c->buf_len - consumed;
  if (leftover > 0) memmove(c->buf, c->buf + consumed, leftover);
  c->buf_len = leftover;
  c->headers_size = 0;
  c->content_len = 0;

  if (score < 0) {
    send(fd, READY_RESP, READY_RESP_LEN, MSG_NOSIGNAL);
  } else {
    send_response(fd, c, score);
  }
}

static void handle_client_write(int fd) {
  Conn* c = (fd >= 0 && fd < MAX_FDS) ? conns[fd] : nullptr;
  if (!c || !c->send_ptr) {
    drop_conn(fd);
    return;
  }

  ssize_t n = send(fd, c->send_ptr + c->send_off, c->send_len - c->send_off,
                   MSG_NOSIGNAL);
  if (n <= 0) {
    drop_conn(fd);
    return;
  }
  c->send_off += (int)n;
  if (c->send_off >= c->send_len) {
    c->send_ptr = nullptr;
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
  }
}

// ---------------------------------------------------------------------------
// Control socket setup (stays open, accepts one connection per lb thread)
// ---------------------------------------------------------------------------
static int create_ctrl_socket(const char* path) {
  char dir[256];
  strncpy(dir, path, sizeof(dir) - 1);
  char* slash = strrchr(dir, '/');
  if (slash && slash > dir) {
    *slash = '\0';
    mkdir(dir, 0755);
  }
  unlink(path);

  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (fd < 0) return -1;

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  listen(fd, 32);
  chmod(path, 0777);
  return fd;
}

#ifdef ENABLE_APM
// ---------------------------------------------------------------------------
// Metrics TCP socket (Prometheus scrape target, optional)
// ---------------------------------------------------------------------------
static int create_metrics_socket(int port) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (fd < 0) return -1;
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons((uint16_t)port);
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  listen(fd, 8);
  return fd;
}
#endif  // ENABLE_APM

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  (void)argc; (void)argv;
  const char* ctrl_path   = getenv("API_CTRL");
  const char* refs_path   = getenv("API_REFERENCES");
  const char* norm_path   = getenv("API_NORMALIZATION");
  const char* mcc_path    = getenv("API_MCC_RISK");
  const char* mport_str   = getenv("API_METRICS_PORT");
  const char* nprobe_str      = getenv("API_NPROBE");
  const char* rmin_str        = getenv("API_REPAIR_MIN");
  const char* rmax_str        = getenv("API_REPAIR_MAX");
  const char* busy_poll_str   = getenv("API_BUSY_POLL_US");
  const char* busy_budget_str = getenv("API_BUSY_POLL_BUDGET");
  const char* prefer_bp_str   = getenv("API_PREFER_BUSY_POLL");
  const char* epoll_to_str    = getenv("API_EPOLL_TIMEOUT_MS");

  if (!ctrl_path || !refs_path || !norm_path || !mcc_path) {
    fprintf(stderr,
            "Required env vars: API_CTRL API_REFERENCES "
            "API_NORMALIZATION API_MCC_RISK\n"
            "Optional: API_METRICS_PORT API_NPROBE API_REPAIR_MIN API_REPAIR_MAX\n"
            "         API_BUSY_POLL_US API_BUSY_POLL_BUDGET\n"
            "         API_PREFER_BUSY_POLL API_EPOLL_TIMEOUT_MS\n");
    return 1;
  }

  int g_nprobe = nprobe_str ? atoi(nprobe_str) : 4;
  if (rmin_str)        g_repair_min                    = atoi(rmin_str);
  if (rmax_str)        g_repair_max                    = atoi(rmax_str);
  if (busy_poll_str)   g_epoll_params.busy_poll_usecs  = (uint32_t)atoi(busy_poll_str);
  if (busy_budget_str) g_epoll_params.busy_poll_budget = (uint16_t)atoi(busy_budget_str);
  if (prefer_bp_str)   g_epoll_params.prefer_busy_poll = (uint8_t)atoi(prefer_bp_str);
  if (epoll_to_str)    g_epoll_timeout_ms              = atoi(epoll_to_str);

  for (int i = 0; i < 6; i++) {
    full_resp_len[i] = snprintf(full_resp[i], sizeof(full_resp[i]),
                                "%sContent-Length: %zu\r\n\r\n%s", RESP_HDRS,
                                strlen(BODIES[i]), BODIES[i]);
  }

#ifdef ENABLE_APM
  apm.registerMetric("rinha_latency_ms",
                     "End-to-end request latency (parse+norm+ivf)");
  apm.registerMetric("rinha_queue_ms",
                     "Within-batch wait: epoll_wait returned to score_transaction called");
  apm.registerMetric("rinha_arrival_wait_ms",
                     "True wait: LB handoff (accept_from_lb) to score_transaction start");
  apm.registerMetric("rinha_parse_ms", "JSON parsing stage latency");
  apm.registerMetric("rinha_normalize_ms", "Feature normalization stage latency");
  apm.registerMetric("rinha_ivf_ms",
                     "IVF nearest-neighbor search latency (includes repair)");
  apm.registerMetric("rinha_epoll_batch_size",
                     "Events per epoll_wait return (encoded: push N*1000 => bucket[N])");
  apm.registerGauge("rinha_active_conns", "Active client connections");
  {
    FILE* f = fopen("/sys/fs/cgroup/cpu.max", "r");
    if (f) {
      char quota_str[32];
      unsigned long period = 100000;
      if (fscanf(f, "%31s %lu", quota_str, &period) == 2 &&
          strcmp(quota_str, "max") != 0) {
        unsigned long quota = strtoul(quota_str, nullptr, 10);
        apm.cpu_limit_cores = (double)quota / (double)period;
        fprintf(stderr, "[INFO] cgroup CPU limit: %.3f cores\n",
                apm.cpu_limit_cores);
      }
      fclose(f);
    }
  }
#endif

  fprintf(stderr, "[INFO] loading IVF index: %s (nprobe=%d repair=[%d,%d])\n",
          refs_path, g_nprobe, g_repair_min, g_repair_max);
  ivf = new IVF(refs_path, g_nprobe, g_repair_min, g_repair_max);
  fprintf(stderr, "[INFO] IVF index loaded\n");

  mlockall(MCL_CURRENT);

  if (!normalizer.load_config(mcc_path, norm_path)) {
    fprintf(stderr, "[ERR] failed to load normalizer config\n");
    return 1;
  }
  fprintf(stderr, "[INFO] normalizer loaded\n");

#ifdef ENABLE_APM
  apm.init();
#endif

  ctrl_listen_fd = create_ctrl_socket(ctrl_path);
  if (ctrl_listen_fd < 0) {
    perror("ctrl socket");
    return 1;
  }
  fprintf(stderr, "[INFO] ctrl socket: %s\n", ctrl_path);

#ifdef ENABLE_APM
  if (mport_str) {
    int mport = atoi(mport_str);
    metrics_listen_fd = create_metrics_socket(mport);
    if (metrics_listen_fd < 0)
      fprintf(stderr, "[WARN] metrics socket on port %d failed\n", mport);
    else
      fprintf(stderr, "[INFO] metrics on :%d\n", mport);
  }
#endif

  epfd = epoll_create1(0);
  if (g_epoll_params.busy_poll_usecs || g_epoll_params.prefer_busy_poll) {
    if (ioctl(epfd, EPIOCSPARAMS, &g_epoll_params) < 0)
      fprintf(stderr, "[WARN] EPIOCSPARAMS: %s\n", strerror(errno));
    else
      fprintf(stderr, "[INFO] epoll busy_poll=%uus budget=%u prefer=%u\n",
              g_epoll_params.busy_poll_usecs, g_epoll_params.busy_poll_budget,
              g_epoll_params.prefer_busy_poll);
  }
  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = ctrl_listen_fd;
  epoll_ctl(epfd, EPOLL_CTL_ADD, ctrl_listen_fd, &ev);

#ifdef ENABLE_APM
  if (metrics_listen_fd >= 0) {
    ev.events = EPOLLIN;
    ev.data.fd = metrics_listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, metrics_listen_fd, &ev);
  }
#endif

  fprintf(stderr, "Server ready\n");

  struct epoll_event events[MAX_EVTS];
  while (true) {
    int nfds = epoll_wait(epfd, events, MAX_EVTS, g_epoll_timeout_ms);
    if (nfds < 0) {
      if (errno == EINTR) continue;
      perror("epoll_wait");
      break;
    }
    if (nfds == 0) continue;
#ifdef ENABLE_APM
    apm.push("rinha_epoll_batch_size", (uint64_t)nfds * 1000);
#endif
    for (int i = 0; i < nfds; i++) {
      int fd = events[i].data.fd;
      uint32_t e = events[i].events;

      if (fd == ctrl_listen_fd) {
        accept_ctrl_conn();
        continue;
      }
      if (fd < MAX_FDS && is_ctrl_fd[fd]) {
        if (e & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
          epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
          is_ctrl_fd[fd] = false;
          close(fd);
        } else if (e & EPOLLIN) {
          accept_from_lb(fd);
        }
        continue;
      }
#ifdef ENABLE_APM
      if (fd == metrics_listen_fd) {
        accept_metrics_conn();
        continue;
      }
#endif

      if (e & (EPOLLHUP | EPOLLERR)) {
        drop_conn(fd);
        continue;
      }
      if (e & EPOLLIN) {
#ifdef ENABLE_APM
        if (fd >= 0 && fd < MAX_FDS && conns[fd]) conns[fd]->ready_ns = APM::now_ns();
#endif
        handle_client_read(fd);
      }
      if (e & EPOLLOUT) handle_client_write(fd);
      if (e & EPOLLRDHUP) drop_conn(fd);
    }
  }
  return 0;
}
