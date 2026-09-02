#include "listener.hpp"

namespace mini_redis {

// ─── Constructor ──────────────────────────────────────────────────────────────

Listener::Listener(int port)
    : port_(port),
      server_fd_(-1),
      address_{},
      running_(false)
{
    Logger logger;
    logger.debug("Initializing server on port {}", port_);
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
    logger.info("mini-redis listening on port {} (O_NONBLOCK + epoll)", port_);

    EventLoop loop(server_fd_);
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
    address_.sin_addr.s_addr = INADDR_ANY;
    address_.sin_port        = ::htons(port_);

    if (::bind(server_fd_,
               reinterpret_cast<const sockaddr*>(&address_),
               sizeof(address_)) < 0) {
        throw std::runtime_error(
            std::string("bind() failed: ") + std::strerror(errno));
    }

    logger.debug("bound to 0.0.0.0:{}", port_);
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