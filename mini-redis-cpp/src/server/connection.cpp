#include "connection.hpp"
#include <unistd.h>
#include <utility>

namespace mini_redis {

Connection::Connection(int fd, std::string peer_ip, uint16_t peer_port)
    : fd_(fd), peer_ip_(std::move(peer_ip)), peer_port_(peer_port) {
    read_buf_.reserve(DEFAULT_BUF_CAPACITY);
    write_buf_.reserve(DEFAULT_BUF_CAPACITY);
}

Connection::~Connection() {
    close();
}

Connection::Connection(Connection&& other) noexcept
    : fd_(other.fd_),
      peer_ip_(std::move(other.peer_ip_)),
      peer_port_(other.peer_port_),
      read_buf_(std::move(other.read_buf_)),
      write_buf_(std::move(other.write_buf_)),
      read_offset_(other.read_offset_),
      parser_(std::move(other.parser_)) {
    other.fd_          = -1;
    other.read_offset_ = 0;
}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        close();
        fd_          = other.fd_;
        peer_ip_     = std::move(other.peer_ip_);
        peer_port_   = other.peer_port_;
        read_buf_    = std::move(other.read_buf_);
        write_buf_   = std::move(other.write_buf_);
        read_offset_ = other.read_offset_;
        parser_      = std::move(other.parser_);
        other.fd_          = -1;
        other.read_offset_ = 0;
    }
    return *this;
}

void Connection::maybe_compact() {
    if (read_offset_ >= read_buf_.size()) {
        // Toàn bộ dữ liệu đã được tiêu thụ (O(1) reset)
        read_buf_.clear();
        read_offset_ = 0;

        // Nếu dung lượng phình to bất thường do payload lớn, giải phóng bớt về cho OS
        if (read_buf_.capacity() > SHRINK_THRESHOLD) {
            read_buf_.shrink_to_fit();
            read_buf_.reserve(DEFAULT_BUF_CAPACITY);
        }
    } else if (read_offset_ > COMPACT_THRESHOLD || read_offset_ > read_buf_.capacity() / 2) {
        // Con trỏ trôi quá xa, dời phần dở dang về đầu buffer (O(1) amortized)
        read_buf_.erase(0, read_offset_);
        read_offset_ = 0;
    }
}

void Connection::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace mini_redis
