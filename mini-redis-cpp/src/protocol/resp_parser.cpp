#include "resp_parser.hpp"

#include <charconv>
#include <string_view>

namespace mini_redis {

namespace {

size_t find_crlf(std::string_view sv, size_t start_pos = 0) {
    if (start_pos >= sv.size()) return std::string_view::npos;
    return sv.find("\r\n", start_pos);
}

bool parse_int64(std::string_view sv, int64_t& out_val) {
    if (sv.empty()) return false;
    const char* start = sv.data();
    const char* end   = sv.data() + sv.size();
    auto [ptr, ec] = std::from_chars(start, end, out_val);
    return (ec == std::errc{} && ptr == end);
}

}  // anonymous namespace

void RespParser::reset() {
    state_ = ParserState::ArrayLen;
    expected_args_ = -1;
    expected_bulk_len_ = -1;
    current_args_.clear();
}

ParseResult RespParser::parse(std::string& buffer,
                              std::vector<std::string>& out_command,
                              std::string& out_error) {
    while (!buffer.empty()) {
        switch (state_) {
        case ParserState::ArrayLen: {
            if (buffer.empty()) {
                return ParseResult::Incomplete;
            }

            // Client RESP phải bắt đầu bằng '*'
            if (buffer[0] != '*') {
                out_error = "Protocol error: expected '*'";
                reset();
                return ParseResult::Error;
            }

            size_t crlf = find_crlf(buffer, 1);
            if (crlf == std::string_view::npos) {
                if (buffer.size() > MAX_LINE_LEN) {
                    out_error = "Protocol error: array length line too long";
                    reset();
                    return ParseResult::Error;
                }
                return ParseResult::Incomplete;
            }

            std::string_view num_sv(buffer.data() + 1, crlf - 1);
            int64_t count = 0;
            if (!parse_int64(num_sv, count) || count < 0 || count > MAX_ARGS) {
                out_error = "Protocol error: invalid multibulk length";
                reset();
                return ParseResult::Error;
            }

            buffer.erase(0, crlf + 2);

            if (count == 0) {
                // Mảng rỗng (*0\r\n): không có đối số nào
                out_command.clear();
                reset();
                return ParseResult::Ok;
            }

            expected_args_ = count;
            current_args_.clear();
            current_args_.reserve(static_cast<size_t>(count));
            state_ = ParserState::BulkLen;
            break;
        }

        case ParserState::BulkLen: {
            if (buffer.empty()) {
                return ParseResult::Incomplete;
            }

            if (buffer[0] != '$') {
                out_error = "Protocol error: expected '$'";
                reset();
                return ParseResult::Error;
            }

            size_t crlf = find_crlf(buffer, 1);
            if (crlf == std::string_view::npos) {
                if (buffer.size() > MAX_LINE_LEN) {
                    out_error = "Protocol error: bulk length line too long";
                    reset();
                    return ParseResult::Error;
                }
                return ParseResult::Incomplete;
            }

            std::string_view len_sv(buffer.data() + 1, crlf - 1);
            int64_t bulk_len = 0;
            if (!parse_int64(len_sv, bulk_len) || bulk_len < -1 || bulk_len > MAX_BULK_LEN) {
                out_error = "Protocol error: invalid bulk length";
                reset();
                return ParseResult::Error;
            }

            buffer.erase(0, crlf + 2);

            if (bulk_len == -1) {
                // Null bulk string ($-1\r\n): không có payload tiếp theo
                current_args_.emplace_back("");
                if (current_args_.size() == static_cast<size_t>(expected_args_)) {
                    out_command = std::move(current_args_);
                    reset();
                    return ParseResult::Ok;
                }
                state_ = ParserState::BulkLen;
                break;
            }

            expected_bulk_len_ = bulk_len;
            state_ = ParserState::BulkData;
            break;
        }

        case ParserState::BulkData: {
            size_t required = static_cast<size_t>(expected_bulk_len_) + 2; // payload + \r\n
            if (buffer.size() < required) {
                return ParseResult::Incomplete;
            }

            // Kiểm tra CRLF ở cuối bulk string
            if (buffer[expected_bulk_len_] != '\r' || buffer[expected_bulk_len_ + 1] != '\n') {
                out_error = "Protocol error: bulk data does not end with CRLF";
                reset();
                return ParseResult::Error;
            }

            current_args_.emplace_back(buffer.data(), static_cast<size_t>(expected_bulk_len_));
            buffer.erase(0, required);

            if (current_args_.size() == static_cast<size_t>(expected_args_)) {
                out_command = std::move(current_args_);
                reset();
                return ParseResult::Ok;
            }

            state_ = ParserState::BulkLen;
            break;
        }
        }
    }

    return ParseResult::Incomplete;
}

}  // namespace mini_redis
