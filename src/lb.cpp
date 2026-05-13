#include <netdb.h>
#include <uv.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

struct Backend {
  std::string host;
  int port;
  struct sockaddr_in addr;
};

// Backend configuration for Rinha
std::vector<Backend> backends = {{"api1", 9999, {}}, {"api2", 9999, {}}};

static int current_backend = 0;
static int listen_port = 9999;

struct write_req_t {
  uv_write_t req;
  uv_buf_t buf;
};

struct proxy_ctx {
  uv_tcp_t client;
  uv_tcp_t backend;
  uv_connect_t connect_req;
  bool backend_connected = false;
  bool closed = false;
  int closed_handles = 0;
  std::vector<uv_buf_t> pending_client_data;
};

void on_write(uv_write_t* req, int status) {
  write_req_t* wr = (write_req_t*)req;
  if (wr->buf.base) free(wr->buf.base);
  free(wr);
}

void alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
  buf->base = (char*)malloc(suggested_size);
  buf->len = suggested_size;
}

void on_close(uv_handle_t* handle) {
  proxy_ctx* ctx = (proxy_ctx*)handle->data;
  ctx->closed_handles++;

  if (ctx->closed_handles == 2) {
    for (auto& buf : ctx->pending_client_data) {
      if (buf.base) free(buf.base);
    }
    delete ctx;
  }
}

void close_proxy(proxy_ctx* ctx) {
  if (ctx->closed) return;
  ctx->closed = true;

  uv_close((uv_handle_t*)&ctx->client, on_close);
  uv_close((uv_handle_t*)&ctx->backend, on_close);
}

void write_to_stream(uv_stream_t* dest, ssize_t nread, const uv_buf_t* buf) {
  if (uv_is_closing((uv_handle_t*)dest)) return;

  write_req_t* wr = (write_req_t*)malloc(sizeof(write_req_t));
  wr->buf.base = (char*)malloc(nread);
  wr->buf.len = nread;
  memcpy(wr->buf.base, buf->base, nread);

  int r = uv_write(&wr->req, dest, &wr->buf, 1, on_write);
  if (r < 0) {
    free(wr->buf.base);
    free(wr);
  }
}

void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  proxy_ctx* ctx = (proxy_ctx*)stream->data;

  if (nread > 0) {
    if (stream == (uv_stream_t*)&ctx->client) {
      if (!ctx->backend_connected) {
        uv_buf_t pbuf = uv_buf_init((char*)malloc(nread), nread);
        memcpy(pbuf.base, buf->base, nread);
        ctx->pending_client_data.push_back(pbuf);
      } else {
        write_to_stream((uv_stream_t*)&ctx->backend, nread, buf);
      }
    } else {
      write_to_stream((uv_stream_t*)&ctx->client, nread, buf);
    }
  } else if (nread < 0) {
    close_proxy(ctx);
  }

  if (buf->base) free(buf->base);
}

void on_connect(uv_connect_t* req, int status) {
  proxy_ctx* ctx = (proxy_ctx*)req->data;

  if (status == 0) {
    ctx->backend_connected = true;
    uv_read_start((uv_stream_t*)&ctx->backend, alloc_buffer, on_read);

    for (auto& buf : ctx->pending_client_data) {
      write_to_stream((uv_stream_t*)&ctx->backend, buf.len, &buf);
      free(buf.base);
    }
    ctx->pending_client_data.clear();
  } else {
    close_proxy(ctx);
  }
}

void on_new_connection(uv_stream_t* server, int status) {
  if (status < 0) return;

  proxy_ctx* ctx = new proxy_ctx();
  uv_tcp_init(uv_default_loop(), &ctx->client);
  ctx->client.data = ctx;

  if (uv_accept(server, (uv_stream_t*)&ctx->client) == 0) {
    uv_tcp_init(uv_default_loop(), &ctx->backend);
    ctx->backend.data = ctx;

    uv_tcp_nodelay(&ctx->client, 1);
    uv_tcp_nodelay(&ctx->backend, 1);

    Backend& b = backends[current_backend];
    current_backend = (current_backend + 1) % backends.size();

    ctx->connect_req.data = ctx;
    uv_tcp_connect(&ctx->connect_req, &ctx->backend,
                   (const struct sockaddr*)&b.addr, on_connect);
    uv_read_start((uv_stream_t*)&ctx->client, alloc_buffer, on_read);
  } else {
    uv_close((uv_handle_t*)&ctx->client, NULL);
    delete ctx;
  }
}

void resolve_backends() {
  for (auto& b : backends) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(b.host.c_str(), std::to_string(b.port).c_str(), &hints,
                    &res) == 0) {
      b.addr = *(struct sockaddr_in*)res->ai_addr;
      freeaddrinfo(res);
    } else {
      std::cerr << "ERROR: Failed to resolve " << b.host << std::endl;
    }
  }
}

void load_config_from_env() {
  const char* port_env = std::getenv("LB_PORT");
  int listen_port = port_env ? std::stoi(port_env) : 9999;

  const char* backends_env = std::getenv("LB_BACKENDS");
  if (backends_env) {
    backends.clear();
    std::string env_str(backends_env);
    std::stringstream ss(env_str);
    std::string item;

    while (std::getline(ss, item, ',')) {
      size_t colon_pos = item.find(':');
      if (colon_pos != std::string::npos) {
        Backend b;
        b.host = item.substr(0, colon_pos);
        b.port = std::stoi(item.substr(colon_pos + 1));
        backends.push_back(b);
      }
    }
  }
}

int main() {
  load_config_from_env();
  resolve_backends();

  uv_tcp_t server;
  uv_tcp_init(uv_default_loop(), &server);

  struct sockaddr_in addr;
  uv_ip4_addr("0.0.0.0", listen_port, &addr);

  uv_tcp_nodelay(&server, 1);
  uv_tcp_bind(&server, (const struct sockaddr*)&addr, 0);

  int r = uv_listen((uv_stream_t*)&server, 4096, on_new_connection);
  if (r) {
    std::cerr << "Error on uv_listen: " << uv_strerror(r) << std::endl;
    return 1;
  }

  std::cout << "Load Balancer running on port " << listen_port << "..."
            << std::endl;
  return uv_run(uv_default_loop(), UV_RUN_DEFAULT);
}