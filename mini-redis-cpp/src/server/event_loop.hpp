#pragma once

#include "connection.hpp"
#include "commands/dispatcher.hpp"
#include "store/data_store.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace mini_redis {

/**
 * EventLoop: vòng lặp sự kiện dùng epoll + O_NONBLOCK.
 *
 * Luồng hoạt động:
 *   1. Nhận server_fd đã bind/listen từ Listener
 *   2. Khởi tạo timerfd (chu kỳ 100ms) để xử lý Active Expiry và Client Idle Timeout
 *   3. epoll_wait() chờ sự kiện (block hiệu quả, không tốn CPU)
 *   4. Nếu sự kiện trên server_fd → accept client mới
 *   5. Nếu sự kiện trên timer_fd → quét active expiry & client idle timeouts
 *   6. Nếu sự kiện trên client_fd → đọc dữ liệu, xử lý, trả response
 *   7. Lặp lại — 1 thread duy nhất phục vụ N client
 */

class EventLoop {
public:
    static constexpr int    MAX_EVENTS      = 64;                // số event xử lý mỗi lần epoll_wait
    static constexpr int    RECV_BUF_SIZE   = 4096;
    static constexpr size_t MAX_BUFFER_SIZE = 1 * 1024 * 1024;   // 1MB giới hạn bộ đệm chống DoS tràn RAM
    static constexpr int    TIMER_INTERVAL_MS = 100;             // Chu kỳ nhịp đập 100ms (10Hz)
    static constexpr std::chrono::seconds DEFAULT_CLIENT_IDLE_TIMEOUT{300}; // 300s mặc định

    explicit EventLoop(int server_fd);
    ~EventLoop();

    // Không copy, không move — quản lý epoll_fd và timer_fd
    EventLoop(const EventLoop&)            = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // Bắt đầu vòng lặp — block cho đến khi running = false
    void run(std::atomic<bool>& running);

    // Truy cập DataStore (hữu ích cho testing và kiểm tra trạng thái)
    DataStore& store() noexcept { return store_; }
    const DataStore& store() const noexcept { return store_; }

    // Cấu hình Client Idle Timeout (<= 0 để tắt)
    void set_client_idle_timeout(std::chrono::seconds timeout) noexcept { client_idle_timeout_ = timeout; }
    std::chrono::seconds client_idle_timeout() const noexcept { return client_idle_timeout_; }

    // Số lượng client đang kết nối
    size_t client_count() const noexcept { return connections_.size(); }

    // Xử lý nhịp timer (public cho phép test trực tiếp nếu cần)
    void on_timer_tick();
    void check_client_timeouts();

private:
    int epoll_fd_;
    int server_fd_;
    int timer_fd_{-1};
    std::chrono::seconds client_idle_timeout_{DEFAULT_CLIENT_IDLE_TIMEOUT};

    // Lưu tất cả client đang kết nối, key = file descriptor
    std::unordered_map<int, Connection> connections_;

    DataStore store_;
    Dispatcher dispatcher_;

    // Thêm fd vào epoll để theo dõi
    void epoll_add(int fd);

    // Xóa fd khỏi epoll
    void epoll_del(int fd);

    // Thay đổi events đang theo dõi (EPOLLIN / EPOLLIN|EPOLLOUT)
    void epoll_mod(int fd, uint32_t events);

    // Đặt fd thành non-blocking
    static void set_nonblocking(int fd);

    // Xử lý khi server_fd có sự kiện: accept client mới
    void on_new_client();

    // Xử lý khi client_fd có sự kiện đọc
    void on_client_data(int client_fd);

    // Xử lý khi client_fd sẵn sàng ghi (EPOLLOUT)
    void on_client_writable(int client_fd);

    // Flush write_buf của client — đăng ký/hủy EPOLLOUT tùy kết quả
    void try_flush(int client_fd);

    // Đóng kết nối client và dọn dẹp
    void close_client(int client_fd);
};

}  // namespace mini_redis
