#pragma once

#include <atomic>
#include <netinet/in.h>
#include "event_loop.hpp"
#include "logging.hpp"

#include <arpa/inet.h>   // inet_ntop
#include <netinet/in.h>  // sockaddr_in, htons, INADDR_ANY
#include <sys/socket.h>  // socket, bind, listen, setsockopt
#include <unistd.h>       // close, shutdown

#include <cerrno>
#include <cstring>
#include <stdexcept>
namespace mini_redis {

/**
 * Listener: tạo TCP socket, bind port, listen.
 * Giao toàn bộ việc xử lý client cho EventLoop.
 *
 * Luồng hoạt động:
 *   Constructor → create_socket() → bind_socket() → start_listen()
 *   run()       → tạo EventLoop → epoll loop (single thread, O_NONBLOCK)
 *   stop()      → báo dừng, shutdown server socket
 */
class Listener {
public:
    static constexpr int DEFAULT_PORT = 6379;
    static constexpr int BACKLOG      = 128;

    explicit Listener(int port = DEFAULT_PORT);
    ~Listener();

    Listener(const Listener&)            = delete;
    Listener& operator=(const Listener&) = delete;

    // Bắt đầu event loop — blocking
    void run();

    // Dừng server (thread-safe)
    void stop();

private:
    int               port_;
    int               server_fd_;
    sockaddr_in       address_;
    std::atomic<bool> running_;

    void create_socket();
    void bind_socket();
    void start_listen();
};

}  // namespace mini_redis
