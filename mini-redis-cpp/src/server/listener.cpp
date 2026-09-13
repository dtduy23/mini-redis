#include "listener.hpp"
#include "event_loop.hpp"
#include "logging.hpp"
#include "store/rdb.hpp"

#include <arpa/inet.h>   // inet_ntop
#include <filesystem>
#include <netinet/in.h>  // sockaddr_in, htons, INADDR_ANY
#include <sys/socket.h>  // socket, bind, listen, setsockopt
#include <unistd.h>      // close, shutdown

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace mini_redis {

// ─── Constructor ──────────────────────────────────────────────────────────────

Listener::Listener(const ServerConfig& config)
    : config_(config),
      server_fd_(-1),
      address_{},
      running_(false)
{
    Logger logger;
    logger.debug("Initializing server on {}:{}", config_.bind_address, config_.port);
    create_socket();
    bind_socket();
    start_listen();
}

Listener::Listener(int port, std::chrono::seconds idle_timeout)
    : server_fd_(-1),
      address_{},
      running_(false)
{
    config_.port = port;
    config_.client_idle_timeout = idle_timeout;
    Logger logger;
    logger.debug("Initializing server on port {}", config_.port);
    create_socket();
    bind_socket();
    start_listen();
}

// ─── Destructor ───────────────────────────────────────────────────────────────

Listener::~Listener() {
    stop();
    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
}

// ─── run() ────────────────────────────────────────────────────────────────────
//
// Tạo EventLoop và chạy — block ở đây cho đến khi stop() được gọi.
// Không còn thread: mọi client được phục vụ bởi vòng lặp epoll.

void Listener::run() {
    Logger logger;
    running_.store(true);
    logger.info("mini-redis listening on {}:{} (O_NONBLOCK + epoll, idle_timeout={}s)",
                config_.bind_address, config_.port, config_.client_idle_timeout.count());

    EventLoop loop(server_fd_);
    loop.set_client_idle_timeout(config_.client_idle_timeout);
    loop.set_max_buffer_size(config_.max_buffer_size);

    // Tự động khôi phục dữ liệu từ snapshot dump.rdb nếu có
    if (std::filesystem::exists("dump.rdb")) {
        size_t loaded = 0;
        std::string err;
        if (RdbManager::load("dump.rdb", loop.store(), loaded, err)) {
            logger.info("DB loaded from disk: {} keys loaded from dump.rdb", loaded);
        } else {
            logger.warning("Failed to load dump.rdb: {}", err);
        }
    }

    loop.run(running_);
}

// ─── stop() ───────────────────────────────────────────────────────────────────

void Listener::stop() {
    running_.store(false);
    // shutdown làm epoll_wait() trả về ngay với sự kiện lỗi trên server_fd
    if (server_fd_ >= 0) {
        ::shutdown(server_fd_, SHUT_RDWR);
    }
}

// ─── create_socket() ──────────────────────────────────────────────────────────

void Listener::create_socket() {
    Logger logger;

    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        throw std::runtime_error(
            std::string("socket() failed: ") + std::strerror(errno));
    }

    // SO_REUSEADDR: tránh "Address already in use" khi restart nhanh
    int enable = 1;
    if (::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {
        throw std::runtime_error(
            std::string("setsockopt(SO_REUSEADDR) failed: ") + std::strerror(errno));
    }

    logger.debug("socket created (fd={})", server_fd_);
}

// ─── bind_socket() ────────────────────────────────────────────────────────────

void Listener::bind_socket() {
    Logger logger;

    address_.sin_family      = AF_INET;
    address_.sin_port        = ::htons(config_.port);

    if (config_.bind_address.empty() || config_.bind_address == "0.0.0.0") {
        address_.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (::inet_pton(AF_INET, config_.bind_address.c_str(), &address_.sin_addr) <= 0) {
            logger.warning("Invalid bind address '{}', defaulting to INADDR_ANY", config_.bind_address);
            address_.sin_addr.s_addr = INADDR_ANY;
        }
    }

    if (::bind(server_fd_,
               reinterpret_cast<const sockaddr*>(&address_),
               sizeof(address_)) < 0) {
        throw std::runtime_error(
            std::string("bind() failed: ") + std::strerror(errno));
    }

    logger.debug("bound to {}:{}", config_.bind_address, config_.port);
}

// ─── start_listen() ───────────────────────────────────────────────────────────

void Listener::start_listen() {
    Logger logger;

    if (::listen(server_fd_, BACKLOG) < 0) {
        throw std::runtime_error(
            std::string("listen() failed: ") + std::strerror(errno));
    }

    logger.debug("socket in LISTEN state (backlog={})", BACKLOG);
}

}  // namespace mini_redis