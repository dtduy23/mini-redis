#pragma once

#include "event_loop.hpp"

#include <atomic>
#include <chrono>
#include <netinet/in.h> // sockaddr_in

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

    explicit Listener(int port = DEFAULT_PORT,
                      std::chrono::seconds idle_timeout = EventLoop::DEFAULT_CLIENT_IDLE_TIMEOUT);
    ~Listener();

    Listener(const Listener&)            = delete;
    Listener& operator=(const Listener&) = delete;

    // Bắt đầu event loop — blocking
    void run();

    // Dừng server (thread-safe)
    void stop();

private:
    int                  port_;
    std::chrono::seconds idle_timeout_;
    int                  server_fd_;
    sockaddr_in          address_;
    std::atomic<bool>    running_;

    void create_socket();
    void bind_socket();
    void start_listen();
};

}  // namespace mini_redis
