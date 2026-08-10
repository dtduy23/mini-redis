#include "event_loop.hpp"
#include "logging.hpp"

#include <arpa/inet.h>   // inet_ntop
#include <fcntl.h>       // fcntl, O_NONBLOCK
#include <sys/epoll.h>   // epoll_create1, epoll_ctl, epoll_wait
#include <sys/socket.h>  // accept
#include <unistd.h>      // close

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace mini_redis {

// ─── Constructor / Destructor ─────────────────────────────────────────────────

EventLoop::EventLoop(int server_fd)
    : epoll_fd_(-1), server_fd_(server_fd) {
    Logger logger;

    // Tạo epoll instance
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0) {
        throw std::runtime_error(
            std::string("epoll_create1() failed: ") + std::strerror(errno));
    }

    // Đặt server socket thành non-blocking
    set_nonblocking(server_fd_);

    // Đăng ký server_fd vào epoll để theo dõi kết nối mới
    epoll_add(server_fd_);

    logger.debug("epoll created (fd={}), watching server_fd={}", epoll_fd_, server_fd_);
}

EventLoop::~EventLoop() {
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
    }
}

// ─── run() ────────────────────────────────────────────────────────────────────
//
// Vòng lặp chính: chờ sự kiện từ epoll, phân loại và xử lý.
// Không tốn CPU khi không có client nào gửi data (epoll_wait ngủ).

void EventLoop::run(std::atomic<bool>& running) {
    Logger logger;
    logger.info("Event loop started (epoll, single-thread, O_NONBLOCK)");

    epoll_event events[MAX_EVENTS];

    while (running.load()) {
        // epoll_wait: ngủ cho đến khi có sự kiện (timeout -1 = vô hạn)
        int num_events = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);

        if (num_events < 0) {
            if (errno == EINTR) continue;  // bị interrupt bởi signal, thử lại
            if (!running.load()) break;    // stop() đã được gọi
            logger.warning("epoll_wait() failed: {}", std::strerror(errno));
            break;
        }

        // Xử lý từng sự kiện
        for (int i = 0; i < num_events; ++i) {
            int fd = events[i].data.fd;

            if (fd == server_fd_) {
                // Server socket sẵn sàng → có client mới muốn kết nối
                on_new_client();
            } else {
                // Client socket sẵn sàng → client gửi data đến
                on_client_data(fd);
            }
        }
    }

    logger.info("Event loop stopped.");
}

// ─── on_new_client() ──────────────────────────────────────────────────────────
//
// Được gọi khi server_fd có sự kiện đọc.
// Dùng vòng lặp để accept tất cả client đang xếp hàng (vì non-blocking).

void EventLoop::on_new_client() {
    Logger logger;

    while (true) {
        sockaddr_in client_addr{};
        socklen_t   client_addr_len = sizeof(client_addr);

        int client_fd = ::accept(server_fd_,
                                 reinterpret_cast<sockaddr*>(&client_addr),
                                 &client_addr_len);

        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Không còn client nào xếp hàng, thoát vòng lặp
                break;
            }
            logger.warning("accept() failed: {}", std::strerror(errno));
            break;
        }

        // Đặt client socket thành non-blocking
        set_nonblocking(client_fd);

        // Lấy IP client để log
        char ip_buf[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &client_addr.sin_addr, ip_buf, sizeof(ip_buf));
        std::string peer_ip   = ip_buf;
        uint16_t    peer_port = ::ntohs(client_addr.sin_port);

        logger.info("+ client connected   {}:{} (fd={})", peer_ip, peer_port, client_fd);

        // Lưu vào map và đăng ký vào epoll
        connections_.emplace(client_fd,
                             Connection(client_fd, std::move(peer_ip), peer_port));
        epoll_add(client_fd);
    }
}

// ─── on_client_data() ─────────────────────────────────────────────────────────
//
// Được gọi khi một client_fd có sự kiện đọc.
// Dùng vòng lặp recv() cho đến khi hết data (EAGAIN) vì non-blocking.

void EventLoop::on_client_data(int client_fd) {
    Logger logger;

    auto it = connections_.find(client_fd);
    if (it == connections_.end()) return;  // fd lạ, bỏ qua

    Connection& conn = it->second;
    char buf[RECV_BUF_SIZE];

    while (true) {
        ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);

        if (n > 0) {
            // Nhận được data — gom vào read_buf
            conn.read_buf().append(buf, static_cast<size_t>(n));

            // TODO: Kiểm tra read_buf có đủ 1 lệnh RESP chưa?
            //       Nếu đủ → parse + dispatch + send response
            //       Hiện tại: echo lại toàn bộ
            ::send(client_fd, conn.read_buf().data(), conn.read_buf().size(), 0);
            conn.read_buf().clear();

        } else if (n == 0) {
            // Client đóng kết nối bình thường (gửi FIN)
            close_client(client_fd);
            return;

        } else {
            // n < 0
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Hết data tạm thời — đợi epoll báo lần sau
                break;
            }
            // Lỗi mạng thật sự
            logger.warning("recv() error on fd={}: {}", client_fd, std::strerror(errno));
            close_client(client_fd);
            return;
        }
    }
}

// ─── close_client() ───────────────────────────────────────────────────────────

void EventLoop::close_client(int client_fd) {
    Logger logger;

    auto it = connections_.find(client_fd);
    if (it == connections_.end()) return;

    logger.info("- client disconnected {}:{} (fd={})",
                it->second.peer_ip(), it->second.peer_port(), client_fd);

    epoll_del(client_fd);
    connections_.erase(it);  // destructor của Connection gọi close(fd)
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

void EventLoop::epoll_add(int fd) {
    epoll_event ev{};
    ev.events  = EPOLLIN;   // theo dõi sự kiện có data để đọc
    ev.data.fd = fd;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        throw std::runtime_error(
            std::string("epoll_ctl(ADD) failed: ") + std::strerror(errno));
    }
}

void EventLoop::epoll_del(int fd) {
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
}

void EventLoop::set_nonblocking(int fd) {
    // Lấy flags hiện tại
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        throw std::runtime_error(
            std::string("fcntl(F_GETFL) failed: ") + std::strerror(errno));
    }

    // Thêm O_NONBLOCK vào flags
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error(
            std::string("fcntl(F_SETFL, O_NONBLOCK) failed: ") + std::strerror(errno));
    }
}

}  // namespace mini_redis
