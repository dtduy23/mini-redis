#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mini_redis {

/**
 * Kết quả trả về của mỗi lần gọi parse():
 * - Ok: Đã parse được 1 lệnh hoàn chỉnh
 * - Incomplete: Dữ liệu chưa đủ 1 frame, cần đợi đọc thêm từ mạng
 * - Error: Dữ liệu sai cú pháp giao thức RESP
 */
enum class ParseResult {
    Ok,
    Incomplete,
    Error
};

/**
 * Các trạng thái trong State Machine:
 * - ArrayLen: Đang chờ và đọc '*<count>\r\n' (số lượng tham số của lệnh)
 * - BulkLen:  Đang chờ và đọc '$<len>\r\n'   (độ dài của tham số tiếp theo)
 * - BulkData: Đang chờ và đọc '<data>\r\n'  (nội dung của tham số)
 */
enum class ParserState {
    ArrayLen,
    BulkLen,
    BulkData
};

class RespParser {
public:
    static constexpr size_t  MAX_LINE_LEN  = 1024;              // Độ dài tối đa của 1 dòng (*N\r\n hoặc $len\r\n)
    static constexpr int64_t MAX_ARGS      = 1024 * 1024;       // Số đối số tối đa cho 1 lệnh
    static constexpr int64_t MAX_BULK_LEN  = 512 * 1024 * 1024; // 512MB giới hạn tối đa cho 1 Bulk String

    RespParser() = default;

    // Parse 1 lệnh từ buffer.
    // - Nếu Ok: các token của lệnh được đưa vào out_command, các byte đã parse bị cắt khỏi buffer.
    // - Nếu Incomplete: buffer giữ nguyên phần byte chưa đọc, state machine bảo lưu trạng thái.
    // - Nếu Error: out_error chứa thông điệp lỗi giao thức, state machine được reset.
    ParseResult parse(std::string& buffer,
                      std::vector<std::string>& out_command,
                      std::string& out_error);

    // Reset parser về trạng thái ban đầu
    void reset();

    ParserState state() const { return state_; }
    bool is_idle() const { return state_ == ParserState::ArrayLen && current_args_.empty(); }

private:
    ParserState              state_{ParserState::ArrayLen};
    int64_t                  expected_args_{-1};
    int64_t                  expected_bulk_len_{-1};
    std::vector<std::string> current_args_;

    static size_t find_crlf(std::string_view sv, size_t start_pos = 0);
    static bool   parse_int64(std::string_view sv, int64_t& out_val);
};

}  // namespace mini_redis
