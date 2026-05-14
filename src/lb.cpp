#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <map>

#define MAX_EVENTS 1024
#define BUFFER_SIZE 16384

static char shared_buffer[BUFFER_SIZE];

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

class BackendPool {
    std::map<std::string, int> pool;

public:
    int get_connection(const std::string& path) {
        if (pool.count(path)) {
            int fd = pool[path];
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
                return fd;
            }
            close(fd);
            pool.erase(path);
        }

        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        struct sockaddr_un addr = {};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd);
            return -1;
        }

        pool[path] = fd;
        return fd;
    }

    void invalidate(const std::string& path) {
        if (pool.count(path)) {
            close(pool[path]);
            pool.erase(path);
        }
    }
};

int main() {
    const char* env_port = std::getenv("LB_PORT");
    int port = env_port ? std::stoi(env_port) : 9999;

    const char* env_backends = std::getenv("LB_BACKENDS");
    std::vector<std::string> backends;
    if (env_backends) {
        std::stringstream ss(env_backends);
        std::string path;
        while (std::getline(ss, path, ',')) {
            backends.push_back(path);
        }
    }

    if (backends.empty()) return 1;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) return 1;
    listen(server_fd, SOMAXCONN);
    set_nonblocking(server_fd);

    int epoll_fd = epoll_create1(0);
    struct epoll_event ev, events[MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    BackendPool pool;
    int current_idx = 0;

    while (true) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        
        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == server_fd) {
                int client_fd = accept(server_fd, NULL, NULL);
                if (client_fd < 0) continue;

                std::string target_path = backends[current_idx];
                current_idx = (current_idx + 1) % backends.size();

                int backend_fd = pool.get_connection(target_path);
                
                if (backend_fd > 0) {
                    ssize_t n = recv(client_fd, shared_buffer, BUFFER_SIZE, 0);
                    if (n > 0) {
                        if (send(backend_fd, shared_buffer, n, 0) < 0) {
                            pool.invalidate(target_path);
                        }
                    }

                    n = recv(backend_fd, shared_buffer, BUFFER_SIZE, 0);
                    if (n > 0) {
                        send(client_fd, shared_buffer, n, 0);
                    }
                }

                close(client_fd);
            }
        }
    }
    return 0;
}