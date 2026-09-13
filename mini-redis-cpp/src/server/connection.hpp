#pragma once

#include "protocol/resp_parser.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

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
    static constexpr size_t COMPACT_THRESHOLD    = 32 * 1024; // 32KB: dọn dẹp phần dở dang
    static constexpr size_t SHRINK_THRESHOLD     = 64 * 1024; // 64KB: thu hồi RAM khi rảnh

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
    std::string& read_buf()  { return read_buf_; }

    // Buffer chứa dữ liệu chờ gửi đi (partial write)
    std::string& write_buf() { return write_buf_; }

    // Trạng thái RESP parser riêng của từng kết nối
    RespParser& parser() { return parser_; }

    // Dữ liệu chưa đọc dưới dạng string_view (Zero-copy)
    std::string_view unparsed_view() const;

    // Dịch chuyển con trỏ đọc sau khi parse thành công n bytes (O(1))
    void consume(size_t n);

    size_t read_offset() const;

    // Dọn dẹp buffer định kỳ và thu hồi RAM khi vượt ngưỡng (Elastic Buffer)
    void maybe_compact();

    // Cập nhật thời điểm hoạt động cuối (chống idle timeout)
    void update_last_active() noexcept;
    std::chrono::steady_clock::time_point last_active() const noexcept;

    void close();

private:
    int         fd_;
    std::string peer_ip_;
    uint16_t    peer_port_;
    std::string read_buf_;   // dữ liệu nhận chưa xử lý
    std::string write_buf_;  // dữ liệu chờ gửi đi
    size_t      read_offset_{0}; // Con trỏ trỏ vào byte tiếp theo cần parse
    RespParser  parser_;     // parser state machine
    std::chrono::steady_clock::time_point last_active_{std::chrono::steady_clock::now()};
};

}  // namespace mini_redis
