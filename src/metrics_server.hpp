#pragma once

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <thread>

// Starts a background thread serving Prometheus metrics over HTTP.
// fn(char* body_buf, int& body_len) must write the response body into buf
// (capacity 262144) and set body_len.
template <typename Fn>
inline bool start_metrics_server(int port, Fn&& fn) {
  int sfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sfd < 0) return false;
  int yes = 1;
  setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons((uint16_t)port);
  if (bind(sfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(sfd);
    return false;
  }
  listen(sfd, 8);
  std::thread([sfd, fn = std::forward<Fn>(fn)]() mutable {
    static char body[262144];
    char rbuf[512], hdr[128];
    for (;;) {
      int cfd = accept(sfd, nullptr, nullptr);
      if (cfd < 0) continue;
      int n = 0;
      for (;;) {
        ssize_t r = recv(cfd, rbuf + n, sizeof(rbuf) - 1 - (size_t)n, 0);
        if (r <= 0) break;
        n += (int)r;
        const char* s = rbuf + (n > 4 ? n - 4 : 0);
        bool done = false;
        for (const char* p = s; p + 3 < rbuf + n && !done; ++p)
          done = p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n';
        if (done || n >= (int)sizeof(rbuf) - 1) break;
      }
      int blen = 0;
      fn(body, blen);
      int hlen = snprintf(hdr, sizeof(hdr),
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/plain; version=0.0.4\r\n"
                          "Content-Length: %d\r\n\r\n",
                          blen);
      send(cfd, hdr, hlen, MSG_NOSIGNAL);
      send(cfd, body, blen, MSG_NOSIGNAL);
      close(cfd);
    }
  }).detach();
  return true;
}
