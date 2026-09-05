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
      parser_(std::move(other.parser_)) {
    other.fd_ = -1;
}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        close();
        fd_        = other.fd_;
        peer_ip_   = std::move(other.peer_ip_);
        peer_port_ = other.peer_port_;
        read_buf_  = std::move(other.read_buf_);
        write_buf_ = std::move(other.write_buf_);
        parser_    = std::move(other.parser_);
        other.fd_  = -1;
    }
    return *this;
}

void Connection::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace mini_redis
