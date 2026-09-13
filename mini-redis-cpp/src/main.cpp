#include "server/listener.hpp"
#include "server/logging.hpp"

#include <csignal>    // signal, SIGINT, SIGTERM
#include <cstdlib>    // atoi
#include <iostream>   // cerr
#include <stdexcept>  // exception

// ─── Signal handling ──────────────────────────────────────────────────────────
//
// Khi nhận Ctrl+C (SIGINT) hoặc kill (SIGTERM), gọi listener.stop()
// để thoát vòng lặp run() một cách sạch sẽ.
//
// Lý do dùng global pointer: signal handler chỉ được nhận tham số `int sig`,
// không thể capture biến local — đây là giới hạn của C signal API.

static mini_redis::Listener* g_listener = nullptr;

static void on_shutdown_signal(int sig) {
    const char* name = (sig == SIGINT) ? "SIGINT" : "SIGTERM";
    // Chỉ dùng write() trong signal handler vì printf/cout không async-signal-safe
    const char msg[] = "\nShutting down...\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    (void)name;

    if (g_listener != nullptr) {
        g_listener->stop();
    }
}

// ─── main() ───────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    Logger log;

    // Đọc port từ argument dòng lệnh, ví dụ: ./mini_redis_cpp 7000
    // Nếu không truyền thì dùng port mặc định 6379 (port chuẩn của Redis)
    int port = mini_redis::Listener::DEFAULT_PORT;
    if (argc >= 2) {
        port = std::atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            log.error("Invalid port: {} (must be 1-65535)", argv[1]);
            return 1;
        }
    }

    std::chrono::seconds idle_timeout = mini_redis::EventLoop::DEFAULT_CLIENT_IDLE_TIMEOUT;
    if (argc >= 3) {
        idle_timeout = std::chrono::seconds(std::atoi(argv[2]));
    }

    // Khởi tạo server — throw nếu không bind được port
    try {
        mini_redis::Listener listener(port, idle_timeout);

        // Đăng ký signal handler sau khi listener sẵn sàng
        g_listener = &listener;
        std::signal(SIGINT,  on_shutdown_signal);
        std::signal(SIGTERM, on_shutdown_signal);

        // Bắt đầu accept loop — block ở đây cho đến khi stop() được gọi
        listener.run();

    } catch (const std::exception& e) {
        log.error("Server failed to start: {}", e.what());
        return 1;
    }

    log.info("Goodbye.");
    return 0;
}
