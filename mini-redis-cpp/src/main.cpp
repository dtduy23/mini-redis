#include "config/config.hpp"
#include "server/listener.hpp"
#include "server/logging.hpp"

#include <algorithm>
#include <cctype>
#include <csignal>    // signal, SIGINT, SIGTERM
#include <cstdlib>    // atoi
#include <cstring>
#include <iostream>   // cerr
#include <stdexcept>  // exception
#include <string>

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

    std::string config_path = "mini-redis.conf";
    int override_port = -1;
    int override_timeout = -1;

    if (argc >= 2) {
        std::string_view arg1 = argv[1];
        bool is_num = !arg1.empty() && std::all_of(arg1.begin(), arg1.end(), [](unsigned char c) { return std::isdigit(c); });
        if (is_num) {
            // Trường hợp tương thích ngược: ./mini_redis_cpp <port> [timeout]
            override_port = std::atoi(argv[1]);
            if (argc >= 3) {
                std::string_view arg2 = argv[2];
                if (std::all_of(arg2.begin(), arg2.end(), [](unsigned char c) { return std::isdigit(c); })) {
                    override_timeout = std::atoi(argv[2]);
                }
            }
        } else if (arg1 == "--config" && argc >= 3) {
            config_path = argv[2];
        } else {
            config_path = argv[1];
        }
    }

    // Nạp cấu hình từ file với cơ chế tự phục hồi (Self-Healing)
    mini_redis::ConfigStatus status;
    std::string config_msg;
    mini_redis::ServerConfig config = mini_redis::ConfigManager::load_and_manage(config_path, status, config_msg);

    // Ghi log trạng thái nạp cấu hình
    if (status == mini_redis::ConfigStatus::CreatedDefault) {
        log.info("{}", config_msg);
    } else if (status == mini_redis::ConfigStatus::RepairedMissing) {
        log.warning("{}", config_msg);
    } else if (status == mini_redis::ConfigStatus::RecoveredFromCorrupted) {
        log.warning("{}", config_msg);
    } else {
        log.info("{}", config_msg);
    }

    // Áp dụng override từ CLI (nếu có)
    if (override_port > 0 && override_port <= 65535) {
        config.port = override_port;
        log.info("CLI override: port = {}", config.port);
    }
    if (override_timeout >= 0) {
        config.client_idle_timeout = std::chrono::seconds(override_timeout);
        log.info("CLI override: client_idle_timeout = {}s", override_timeout);
    }

    // Áp dụng mức lọc loglevel toàn cục
    Logger::set_global_level(Logger::parse_level(config.loglevel));

    // Khởi tạo server — throw nếu không bind được port
    try {
        mini_redis::Listener listener(config);

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
