#pragma once

#include <cstdint>
#include <string>

namespace mini_redis {

/**
 * Connection: đại diện một kết nối TCP với client.
 *
 * Với mô hình O_NONBLOCK + epoll, Connection cần có read_buf
 * vì một lần recv() có thể chỉ nhận được 1 phần của lệnh —
 * ta phải gom đủ dữ liệu rồi mới parse.
 */
class Connection {
public:
    static constexpr size_t DEFAULT_BUF_CAPACITY = 4096;

    Connection(int fd, std::string peer_ip, uint16_t peer_port);
    ~Connection();

    // Không copy — sở hữu fd
    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;

    // Move để lưu vào unordered_map
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    int               fd()        const { return fd_; }
    const std::string& peer_ip()  const { return peer_ip_; }
    uint16_t          peer_port() const { return peer_port_; }
    bool              is_valid()  const { return fd_ >= 0; }

    // Buffer tích lũy dữ liệu nhận từ client
    // (có thể nhiều recv() mới đủ 1 lệnh Redis)
    // Buffer tích lũy dữ liệu nhận từ client
    std::string& read_buf()  { return read_buf_; }

    // Buffer chứa dữ liệu chờ gửi đi (partial write)
    std::string& write_buf() { return write_buf_; }

    void close();

private:
    int         fd_;
    std::string peer_ip_;
    uint16_t    peer_port_;
    std::string read_buf_;   // dữ liệu nhận chưa xử lý
    std::string write_buf_;  // dữ liệu chờ gửi đi
};

}  // namespace mini_redis
