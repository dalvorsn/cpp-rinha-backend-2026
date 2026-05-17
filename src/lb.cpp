#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef DEBUG
#define DLOG(...) fprintf(stderr, "[DBG/LB] " __VA_ARGS__)
#else
#define DLOG(...) ((void)0)
#endif

static int    ctrl_fds[2]      = {-1, -1};
static char   ctrl_paths[2][256];
static int    num_backends     = 0;

static int connect_ctrl(int i) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ctrl_paths[i], sizeof(addr.sun_path) - 1);

    // Retry until the backend creates its control socket
    int attempts = 0;
    while (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errno != ENOENT && errno != ECONNREFUSED) {
            fprintf(stderr, "[ERR/LB] connect backend %s: %s\n", addr.sun_path, strerror(errno));
            close(fd);
            return -1;
        }
        if (attempts++ % 50 == 0)
            fprintf(stderr, "[INFO/LB] waiting for backend %s (attempt %d)...\n", addr.sun_path, attempts);
        usleep(20000);
    }
    DLOG("connect_ctrl: connected to %s after %d attempts\n", addr.sun_path, attempts);
    return fd;
}

static int send_fd(int ctrl, int client_fd) {
    char         buf[1]  = {0};
    struct iovec iov     = {buf, 1};
    char         cmsg_buf[CMSG_SPACE(sizeof(int))];

    struct msghdr msg    = {};
    msg.msg_iov          = &iov;
    msg.msg_iovlen       = 1;
    msg.msg_control      = cmsg_buf;
    msg.msg_controllen   = sizeof(cmsg_buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level     = SOL_SOCKET;
    cmsg->cmsg_type      = SCM_RIGHTS;
    cmsg->cmsg_len       = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &client_fd, sizeof(int));

    ssize_t r = sendmsg(ctrl, &msg, MSG_NOSIGNAL);
    if (r <= 0) {
        fprintf(stderr, "[ERR/LB] send_fd: sendmsg ctrl=%d client_fd=%d: %s\n",
                ctrl, client_fd, strerror(errno));
        return -1;
    }
    DLOG("send_fd: ctrl=%d client_fd=%d OK\n", ctrl, client_fd);
    return 0;
}

int main(void) {
    const char *port_env = getenv("LB_PORT");
    int port = port_env ? atoi(port_env) : 9999;

    const char *be_env = getenv("LB_BACKENDS");
    if (!be_env) { fprintf(stderr, "LB_BACKENDS not set\n"); return 1; }

    char tmp[1024];
    strncpy(tmp, be_env, sizeof(tmp) - 1);
    char *tok = strtok(tmp, ",");
    while (tok && num_backends < 2) {
        strncpy(ctrl_paths[num_backends++], tok, 255);
        tok = strtok(NULL, ",");
    }
    if (num_backends == 0) return 1;

    // Connect to all backend control sockets (retrying until available)
    for (int i = 0; i < num_backends; i++) {
        ctrl_fds[i] = connect_ctrl(i);
        if (ctrl_fds[i] < 0) {
            fprintf(stderr, "Cannot connect to backend %d: %s\n", i, ctrl_paths[i]);
            return 1;
        }
        fprintf(stderr, "LB: connected to backend %d (%s)\n", i, ctrl_paths[i]);
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt  = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family         = AF_INET;
    addr.sin_addr.s_addr    = INADDR_ANY;
    addr.sin_port           = htons(port);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    listen(srv, SOMAXCONN);
    fprintf(stderr, "LB: listening on port %d\n", port);

    int cur      = 0;
    long total   = 0;
    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = accept(srv, (struct sockaddr*)&cli_addr, &cli_len);
        if (client_fd < 0) {
            fprintf(stderr, "[ERR/LB] accept: %s\n", strerror(errno));
            continue;
        }

        total++;
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        int idx = cur;
        cur = (cur + 1) % num_backends;

        DLOG("accept fd=%d total=%ld → backend[%d] ctrl=%d\n",
             client_fd, total, idx, ctrl_fds[idx]);

        if (send_fd(ctrl_fds[idx], client_fd) < 0) {
            fprintf(stderr, "[WARN/LB] send_fd failed for backend[%d], reconnecting...\n", idx);
            close(ctrl_fds[idx]);
            ctrl_fds[idx] = connect_ctrl(idx);
            if (ctrl_fds[idx] >= 0) {
                if (send_fd(ctrl_fds[idx], client_fd) < 0)
                    fprintf(stderr, "[ERR/LB] retry send_fd also failed for backend[%d]\n", idx);
            } else {
                fprintf(stderr, "[ERR/LB] reconnect backend[%d] failed\n", idx);
            }
        }

        close(client_fd); // LB releases its copy; backend owns the socket now
        DLOG("closed lb copy of fd=%d\n", client_fd);
    }
    return 0;
}
