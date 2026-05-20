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
#include <filesystem>

#include "ivf.hpp"
#include "normalizer.hpp"
#include "simdjson.h"

namespace fs = std::filesystem;
using namespace simdjson;

#ifdef DEBUG
#define DLOG(...) fprintf(stderr, "[DBG] " __VA_ARGS__)
#else
#define DLOG(...) ((void)0)
#endif

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
  bool is_fraud = false;
  const char* send_ptr = nullptr;
  int send_len = 0;
  int send_off = 0;
};

static Conn* conns[MAX_FDS] = {};
static IVF* ivf = nullptr;
static Normalizer normalizer;
static int epfd = -1;
static int ctrl_fd = -1;

static Conn* get_conn(int fd) {
  if (fd < 0 || fd >= MAX_FDS) return nullptr;
  if (!conns[fd]) conns[fd] = new Conn;
  auto* c = conns[fd];
  c->buf_len = 0;
  c->headers_size = 0;
  c->content_len = 0;
  c->is_fraud = false;
  c->send_ptr = nullptr;
  c->send_len = 0;
  c->send_off = 0;
  return c;
}

static void drop_conn(int fd) {
  DLOG("drop fd=%d\n", fd);
  epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
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
  DLOG("recv_fd: got fd=%d\n", new_fd);
  return new_fd;
}

// ---------------------------------------------------------------------------
// Handle readable/writable client fds
// ---------------------------------------------------------------------------
static void handle_client_read(int fd) {
  Conn* c = (fd >= 0 && fd < MAX_FDS) ? conns[fd] : nullptr;
  if (!c) {
    drop_conn(fd);
    return;
  }

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
  DLOG("fd=%d recv %zd bytes (total=%d)\n", fd, n, c->buf_len);

  if (c->headers_size == 0) {
    const char* hdrend = (const char*)memmem(c->buf, c->buf_len, "\r\n\r\n", 4);
    if (!hdrend) return;
    c->headers_size = (int)(hdrend - c->buf) + 4;
    c->is_fraud = (c->buf[0] == 'P');
    if (c->is_fraud)
      c->content_len = parse_content_length(c->buf, c->headers_size);
    DLOG("fd=%d hdr=%d fraud=%d clen=%d\n", fd, c->headers_size, c->is_fraud,
         c->content_len);
  }

  int body_have = c->buf_len - c->headers_size;
  if (c->is_fraud && body_have < c->content_len) return;

  const char* resp = READY_RESP;
  int resp_len = READY_RESP_LEN;

  if (c->is_fraud) {
    thread_local dom::parser parser;
    const char* body = c->buf + c->headers_size;
    int blen = c->content_len;
    dom::element doc;
    bool ok;
    if (c->headers_size + blen + (int)SIMDJSON_PADDING <= BUF_CAP) {
      // Parse in-place: zero the 64-byte padding region after the body,
      // then parse without copying (saves one malloc/free per request).
      memset(c->buf + c->headers_size + blen, 0, SIMDJSON_PADDING);
      ok = !parser.parse(body, (size_t)blen, false).get(doc);
    } else {
      padded_string padded(body, (size_t)blen);
      ok = !parser.parse(padded).get(doc);
    }
    if (ok) {
      try {
        int16_t vec[14];
        normalizer.normalize(doc, vec);
        int cnt = ivf->get_fraud_count(vec);
        DLOG("fd=%d fraud_count=%d\n", fd, cnt);
        resp = full_resp[cnt];
        resp_len = full_resp_len[cnt];
      } catch (...) {
        fprintf(stderr, "[ERR] fd=%d ivf/normalize threw\n", fd);
        ok = false;
      }
    }
    if (!ok) {
      resp = ERR_RESP;
      resp_len = ERR_RESP_LEN;
    }
  }

  int consumed = c->headers_size + (c->is_fraud ? c->content_len : 0);
  int leftover = c->buf_len - consumed;
  if (leftover > 0) memmove(c->buf, c->buf + consumed, leftover);
  c->buf_len = leftover;
  c->headers_size = 0;
  c->content_len = 0;

  ssize_t sent = send(fd, resp, resp_len, MSG_NOSIGNAL);
  DLOG("fd=%d send %d → sent=%zd\n", fd, resp_len, sent);
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
// Drain all fds from LB via SCM_RIGHTS
// ---------------------------------------------------------------------------
static void accept_from_lb() {
  while (true) {
    int client_fd = recv_fd(ctrl_fd);
    if (client_fd < 0) return;

    if (client_fd >= MAX_FDS) {
      fprintf(stderr, "[ERR] fd=%d >= MAX_FDS\n", client_fd);
      close(client_fd);
      continue;
    }

    int flags = fcntl(client_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    int yes = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_QUICKACK, &yes, sizeof(yes));

    get_conn(client_fd);

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = client_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
      fprintf(stderr, "[ERR] epoll_ctl ADD fd=%d: %s\n", client_fd,
              strerror(errno));
      drop_conn(client_fd);
    } else {
      DLOG("new client fd=%d\n", client_fd);
    }
  }
}

// ---------------------------------------------------------------------------
// Control socket setup
// ---------------------------------------------------------------------------
static int create_ctrl_socket(const char* path) {
  fs::path p(path);
  if (p.has_parent_path()) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
  }
  unlink(path);

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  listen(fd, 8);
  chmod(path, 0777);
  return fd;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  if (argc < 5) {
    fprintf(stderr, "Usage: %s <ctrl_path> <data.bin> <norm.json> <mcc.json>\n",
            argv[0]);
    return 1;
  }

  for (int i = 0; i < 6; i++) {
    full_resp_len[i] = snprintf(full_resp[i], sizeof(full_resp[i]),
                                "%sContent-Length: %zu\r\n\r\n%s", RESP_HDRS,
                                strlen(BODIES[i]), BODIES[i]);
  }

  fprintf(stderr, "[INFO] loading IVF index: %s\n", argv[2]);
  ivf = new IVF(argv[2]);
  fprintf(stderr, "[INFO] IVF index loaded\n");
  mlockall(MCL_CURRENT);

  if (!normalizer.load_config(argv[4], argv[3])) {
    fprintf(stderr, "[ERR] failed to load normalizer config\n");
    return 1;
  }
  fprintf(stderr, "[INFO] normalizer loaded\n");

  int listen_ctrl = create_ctrl_socket(argv[1]);
  if (listen_ctrl < 0) {
    perror("ctrl socket");
    return 1;
  }
  fprintf(stderr, "[INFO] ctrl socket: %s\n", argv[1]);

  fprintf(stderr, "[INFO] waiting for LB...\n");
  ctrl_fd = accept(listen_ctrl, nullptr, nullptr);
  if (ctrl_fd < 0) {
    perror("accept ctrl");
    return 1;
  }
  close(listen_ctrl);
  fprintf(stderr, "[INFO] LB connected\n");

  {
    int fl = fcntl(ctrl_fd, F_GETFL, 0);
    fcntl(ctrl_fd, F_SETFL, fl | O_NONBLOCK);
  }

  epfd = epoll_create1(0);
  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = ctrl_fd;
  epoll_ctl(epfd, EPOLL_CTL_ADD, ctrl_fd, &ev);

  fprintf(stderr, "Server ready\n");

  struct epoll_event events[MAX_EVTS];
  while (true) {
    int nfds = epoll_wait(epfd, events, MAX_EVTS, -1);
    if (nfds < 0) {
      if (errno == EINTR) continue;
      perror("epoll_wait");
      break;
    }
    for (int i = 0; i < nfds; i++) {
      int fd = events[i].data.fd;
      uint32_t e = events[i].events;

      if (fd == ctrl_fd) {
        accept_from_lb();
        continue;
      }

      if (e & (EPOLLHUP | EPOLLERR)) {
        drop_conn(fd);
        continue;
      }
      if (e & EPOLLOUT) handle_client_write(fd);
      if (e & EPOLLIN) handle_client_read(fd);
      if (e & EPOLLRDHUP) drop_conn(fd);
    }
  }
  return 0;
}
