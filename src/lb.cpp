#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <atomic>

#define MAX_BACKENDS 8
#define MAX_FDS 65536
#define BACKLOG 65535

// ---------------------------------------------------------------------------
// Upstream (backend) — pre-allocated msghdr to avoid per-call setup
// ---------------------------------------------------------------------------
typedef struct {
  char path[256];
  int fd;
  char byte;
  struct iovec iov;
  alignas(struct cmsghdr) char control_buf[CMSG_SPACE(sizeof(int))];
  struct msghdr msg;
  struct cmsghdr* cmsg;
} upstream_t;

static upstream_t upstreams[MAX_BACKENDS];
static int upstream_count = 0;
static int lb_port = 9999;
static uint32_t rr_next = 0;

static double cgroup_cpu_limit_cores = -1.0;
#ifdef ENABLE_APM
static std::atomic<unsigned long> total_conns[MAX_BACKENDS];
static std::atomic<unsigned long> connect_errors[MAX_BACKENDS];

// Metrics (background thread)
static int metrics_srv_fd = -1;
static int metrics_epfd = -1;
static bool is_metrics_conn[MAX_FDS];
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void sleep_ms(long ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
  }
}

static void set_nonblock_cloexec(int fd) {
  int f = fcntl(fd, F_GETFL, 0);
  if (f >= 0) fcntl(fd, F_SETFL, f | O_NONBLOCK);
  f = fcntl(fd, F_GETFD, 0);
  if (f >= 0) fcntl(fd, F_SETFD, f | FD_CLOEXEC);
}

static void init_upstream_msg(upstream_t* u) {
  memset(u->control_buf, 0, sizeof(u->control_buf));
  memset(&u->msg, 0, sizeof(u->msg));
  u->byte = 1;
  u->iov.iov_base = &u->byte;
  u->iov.iov_len = 1;
  u->msg.msg_iov = &u->iov;
  u->msg.msg_iovlen = 1;
  u->msg.msg_control = u->control_buf;
  u->msg.msg_controllen = sizeof(u->control_buf);
  u->cmsg = CMSG_FIRSTHDR(&u->msg);
  u->cmsg->cmsg_level = SOL_SOCKET;
  u->cmsg->cmsg_type = SCM_RIGHTS;
  u->cmsg->cmsg_len = CMSG_LEN(sizeof(int));
}

// ---------------------------------------------------------------------------
// Connect to backend ctrl socket
// ---------------------------------------------------------------------------
static int connect_ctrl_once(const char* path) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;

  int sndbuf = 256 * 1024;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  set_nonblock_cloexec(fd);
  return fd;
}

static int connect_ctrl_wait(const char* path) {
  for (;;) {
    int fd = connect_ctrl_once(path);
    if (fd >= 0) return fd;
    sleep_ms(10);
  }
}

static int reconnect_upstream(int idx) {
  if (upstreams[idx].fd >= 0) close(upstreams[idx].fd);
  upstreams[idx].fd = -1;
  for (int i = 0; i < 10; i++) {
    int fd = connect_ctrl_once(upstreams[idx].path);
    if (fd >= 0) {
      upstreams[idx].fd = fd;
      return 0;
    }
    sleep_ms(2);
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Send fd via SCM_RIGHTS (non-blocking)
// ---------------------------------------------------------------------------
static int send_fd_once(upstream_t* u, int client_fd) {
  u->msg.msg_controllen = sizeof(u->control_buf);
  memcpy(CMSG_DATA(u->cmsg), &client_fd, sizeof(client_fd));
  for (;;) {
    ssize_t n = sendmsg(u->fd, &u->msg, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n == 1) return 0;
    if (n < 0 && errno == EINTR) continue;
    return errno ? errno : EIO;
  }
}

static int is_broken_errno(int e) {
  return e == EPIPE || e == ECONNRESET || e == ENOTCONN || e == EBADF;
}

static int handoff(int idx, int client_fd) {
  if (upstreams[idx].fd < 0 && reconnect_upstream(idx) != 0)
    return ECONNREFUSED;
  int e = send_fd_once(&upstreams[idx], client_fd);
  if (e == 0) return 0;
  if (is_broken_errno(e)) {
    if (reconnect_upstream(idx) != 0) return e;
    e = send_fd_once(&upstreams[idx], client_fd);
  }
  return e;
}

static int handoff_any(int first, int client_fd) {
  if (upstream_count == 2) {
    if (handoff(first, client_fd) == 0) return 0;
    return handoff(first ^ 1, client_fd);
  }
  for (int off = 0; off < upstream_count; off++) {
    int idx = (first + off) % upstream_count;
    if (handoff(idx, client_fd) == 0) return 0;
  }
  return -1;
}

// ---------------------------------------------------------------------------
// TCP listen socket
// ---------------------------------------------------------------------------
static int make_listen_socket(int port) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons((uint16_t)port);
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  listen(fd, BACKLOG);
  return fd;
}

// ---------------------------------------------------------------------------
// Metrics endpoint (background thread)
// ---------------------------------------------------------------------------
#ifdef ENABLE_APM
static void handle_metrics_accept() {
  for (;;) {
    int cfd = accept4(metrics_srv_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (cfd < 0) return;
    if (cfd >= MAX_FDS) {
      close(cfd);
      continue;
    }
    is_metrics_conn[cfd] = true;
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = cfd;
    epoll_ctl(metrics_epfd, EPOLL_CTL_ADD, cfd, &ev);
  }
}

static void handle_metrics_conn(int fd) {
  char req[256];
  recv(fd, req, sizeof(req), 0);

  static char body[4096];
  int n = 0;
  const int cap = (int)sizeof(body);
#define W(...) \
  if (n < cap) n += snprintf(body + n, (size_t)(cap - n), __VA_ARGS__)

  W("# HELP rinha_lb_connections_accepted_total Connections routed per "
    "backend\n"
    "# TYPE rinha_lb_connections_accepted_total counter\n");
  for (int i = 0; i < upstream_count; i++)
    W("rinha_lb_connections_accepted_total{backend=\"%d\"} %lu\n", i,
      total_conns[i].load(std::memory_order_relaxed));

  W("# HELP rinha_lb_connect_errors_total Failed send_fd calls\n"
    "# TYPE rinha_lb_connect_errors_total counter\n");
  for (int i = 0; i < upstream_count; i++)
    W("rinha_lb_connect_errors_total{backend=\"%d\"} %lu\n", i,
      connect_errors[i].load(std::memory_order_relaxed));

  // cgroup cpu.stat
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
      W("# HELP rinha_lb_cgroup_cpu_usage_seconds_total LB cgroup CPU usage\n"
        "# TYPE rinha_lb_cgroup_cpu_usage_seconds_total counter\n"
        "rinha_lb_cgroup_cpu_usage_seconds_total{mode=\"user\"} %.6f\n"
        "rinha_lb_cgroup_cpu_usage_seconds_total{mode=\"system\"} %.6f\n"
        "rinha_lb_cgroup_cpu_usage_seconds_total{mode=\"total\"} %.6f\n",
        user_usec / 1e6, system_usec / 1e6, usage_usec / 1e6);
      W("# HELP rinha_lb_cgroup_cpu_throttled_seconds_total LB cgroup "
        "throttle\n"
        "# TYPE rinha_lb_cgroup_cpu_throttled_seconds_total counter\n"
        "rinha_lb_cgroup_cpu_throttled_seconds_total %.6f\n",
        throttled_usec / 1e6);
    }
  }
  if (cgroup_cpu_limit_cores > 0)
    W("# HELP rinha_lb_cgroup_cpu_limit_cores LB CPU limit\n"
      "# TYPE rinha_lb_cgroup_cpu_limit_cores gauge\n"
      "rinha_lb_cgroup_cpu_limit_cores %.6f\n",
      cgroup_cpu_limit_cores);
#undef W

  char hdr[128];
  int hlen = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                      "Content-Length: %d\r\n\r\n",
                      n);
  send(fd, hdr, hlen, MSG_NOSIGNAL);
  send(fd, body, n, MSG_NOSIGNAL);
  epoll_ctl(metrics_epfd, EPOLL_CTL_DEL, fd, NULL);
  is_metrics_conn[fd] = false;
  close(fd);
}

static void* metrics_thread_main(void*) {
  struct epoll_event events[64];
  for (;;) {
    int n = epoll_wait(metrics_epfd, events, 64, -1);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (int i = 0; i < n; i++) {
      int fd = events[i].data.fd;
      uint32_t e = events[i].events;
      if (fd == metrics_srv_fd) {
        handle_metrics_accept();
      } else if (fd >= 0 && fd < MAX_FDS && is_metrics_conn[fd]) {
        if (e & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
          epoll_ctl(metrics_epfd, EPOLL_CTL_DEL, fd, NULL);
          is_metrics_conn[fd] = false;
          close(fd);
        } else if (e & EPOLLIN) {
          handle_metrics_conn(fd);
        }
      }
    }
  }
  return NULL;
}
#endif  // ENABLE_APM

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(void) {
  signal(SIGPIPE, SIG_IGN);

  // Parse config
  const char* port_env = getenv("LB_PORT");
  lb_port = port_env ? atoi(port_env) : 9999;

  const char* be_env = getenv("LB_BACKENDS");
  if (!be_env) {
    fprintf(stderr, "LB_BACKENDS not set\n");
    return 1;
  }

  // Parse comma-separated backend paths
  char tmp[1024];
  strncpy(tmp, be_env, sizeof(tmp) - 1);
  char* tok = strtok(tmp, ",");
  while (tok && upstream_count < MAX_BACKENDS) {
    upstream_t* u = &upstreams[upstream_count++];
    memset(u, 0, sizeof(*u));
    strncpy(u->path, tok, sizeof(u->path) - 1);
    u->fd = -1;
    init_upstream_msg(u);
    tok = strtok(NULL, ",");
  }
  if (upstream_count == 0) return 1;

  // cgroup CPU limit
  {
    FILE* f = fopen("/sys/fs/cgroup/cpu.max", "r");
    if (f) {
      char quota_str[32];
      unsigned long period = 100000;
      if (fscanf(f, "%31s %lu", quota_str, &period) == 2 &&
          strcmp(quota_str, "max") != 0) {
        unsigned long quota = strtoul(quota_str, NULL, 10);
        cgroup_cpu_limit_cores = (double)quota / (double)period;
        fprintf(stderr, "[INFO/LB] cgroup CPU limit: %.3f cores\n",
                cgroup_cpu_limit_cores);
      }
      fclose(f);
    }
  }

  // Connect to all backends (blocking until ready)
  for (int i = 0; i < upstream_count; i++) {
    upstreams[i].fd = connect_ctrl_wait(upstreams[i].path);
    fprintf(stderr, "[INFO/LB] connected to backend %d: %s\n", i,
            upstreams[i].path);
  }

  // Listen socket
  int server_fd = make_listen_socket(lb_port);
  if (server_fd < 0) {
    perror("listen");
    return 1;
  }
  fprintf(stderr, "[INFO/LB] listening on port %d\n", lb_port);

#ifdef ENABLE_APM
  // Metrics (optional background thread)
  const char* mport_env = getenv("LB_METRICS_PORT");
  if (mport_env) {
    int mport = atoi(mport_env);
    metrics_srv_fd =
        socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int opt = 1;
    setsockopt(metrics_srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in maddr = {};
    maddr.sin_family = AF_INET;
    maddr.sin_addr.s_addr = INADDR_ANY;
    maddr.sin_port = htons((uint16_t)mport);
    if (bind(metrics_srv_fd, (struct sockaddr*)&maddr, sizeof(maddr)) < 0) {
      perror("metrics bind");
      close(metrics_srv_fd);
      metrics_srv_fd = -1;
    } else {
      listen(metrics_srv_fd, 8);
      metrics_epfd = epoll_create1(0);
      struct epoll_event ev;
      ev.events = EPOLLIN;
      ev.data.fd = metrics_srv_fd;
      epoll_ctl(metrics_epfd, EPOLL_CTL_ADD, metrics_srv_fd, &ev);
      pthread_t tid;
      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
      pthread_create(&tid, &attr, metrics_thread_main, NULL);
      pthread_attr_destroy(&attr);
      fprintf(stderr, "[INFO/LB] metrics on :%d\n", mport);
    }
  }
#endif

  // Main accept loop — simple poll-based, no epoll overhead
  for (;;) {
    int cfd = accept4(server_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (cfd < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        struct pollfd pfd = {server_fd, POLLIN, 0};
        poll(&pfd, 1, -1);
        continue;
      }
      continue;
    }

    int yes = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    setsockopt(cfd, IPPROTO_TCP, TCP_QUICKACK, &yes, sizeof(yes));

    uint32_t rr = rr_next++;
    int first = upstream_count == 2 ? (int)(rr & 1u)
                                    : (int)(rr % (uint32_t)upstream_count);
    int e = handoff_any(first, cfd);
#ifdef ENABLE_APM
    if (e == 0)
      total_conns[first].fetch_add(1, std::memory_order_relaxed);
    else
      connect_errors[first].fetch_add(1, std::memory_order_relaxed);
#endif

    close(cfd);
  }
}
