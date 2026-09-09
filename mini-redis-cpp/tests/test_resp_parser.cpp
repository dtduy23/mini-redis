#include "protocol/resp_parser.hpp"
#include "protocol/resp_serializer.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace mini_redis;

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "  FAILED: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            return false; \
        } \
    } while (0)

#define RUN_TEST(test_func) \
    do { \
        g_tests_run++; \
        std::cout << "[TEST] " << #test_func << "... "; \
        if (test_func()) { \
            g_tests_passed++; \
            std::cout << "PASSED\n"; \
        } else { \
            std::cout << "FAILED\n"; \
        } \
    } while (0)

// 1. Kiểm tra parse các lệnh đơn lẻ hoàn chỉnh
bool test_single_commands() {
    RespParser parser;
    std::vector<std::string> cmd;
    std::string err;

    // PING: *1\r\n$4\r\nPING\r\n
    std::string buf = "*1\r\n$4\r\nPING\r\n";
    ParseResult res = parser.parse(buf, cmd, err);
    TEST_ASSERT(res == ParseResult::Ok, "PING should parse Ok");
    TEST_ASSERT(cmd.size() == 1 && cmd[0] == "PING", "PING token matches");
    TEST_ASSERT(buf.empty(), "buffer should be fully consumed");

    // SET key value: *3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n
    buf = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
    cmd.clear();
    res = parser.parse(buf, cmd, err);
    TEST_ASSERT(res == ParseResult::Ok, "SET should parse Ok");
    TEST_ASSERT(cmd.size() == 3, "SET should have 3 tokens");
    TEST_ASSERT(cmd[0] == "SET" && cmd[1] == "foo" && cmd[2] == "bar", "SET tokens match");
    TEST_ASSERT(buf.empty(), "buffer should be fully consumed");

    // Empty array: *0\r\n
    buf = "*0\r\n";
    cmd.clear();
    res = parser.parse(buf, cmd, err);
    TEST_ASSERT(res == ParseResult::Ok, "*0 should parse Ok");
    TEST_ASSERT(cmd.empty(), "*0 should have 0 tokens");
    TEST_ASSERT(buf.empty(), "buffer should be fully consumed");

    return true;
}

// 2. Kiểm tra binary safety và các chuỗi đặc biệt
bool test_binary_safety_and_special_chars() {
    RespParser parser;
    std::vector<std::string> cmd;
    std::string err;

    // Payload chứa khoảng trắng, xuống dòng \r\n bên trong bulk string
    // "hello\r\nworld" có độ dài 12 bytes
    std::string payload = "hello\r\nworld";
    std::string buf = "*2\r\n$4\r\nECHO\r\n$12\r\nhello\r\nworld\r\n";
    ParseResult res = parser.parse(buf, cmd, err);
    TEST_ASSERT(res == ParseResult::Ok, "ECHO with embedded CRLF should parse Ok");
    TEST_ASSERT(cmd.size() == 2, "2 tokens");
    TEST_ASSERT(cmd[1] == payload, "payload with embedded CRLF matches exactly");
    TEST_ASSERT(buf.empty(), "buffer consumed");

    // Payload chứa ký tự null '\0'
    std::string null_payload("ab\0cd", 5);
    buf = "*2\r\n$4\r\nECHO\r\n$5\r\n" + null_payload + "\r\n";
    cmd.clear();
    res = parser.parse(buf, cmd, err);
    TEST_ASSERT(res == ParseResult::Ok, "payload with null byte should parse Ok");
    TEST_ASSERT(cmd.size() == 2, "2 tokens");
    TEST_ASSERT(cmd[1].size() == 5, "payload size is 5");
    TEST_ASSERT(cmd[1] == null_payload, "binary payload matches");
    TEST_ASSERT(buf.empty(), "buffer consumed");

    // Empty bulk string ($0\r\n\r\n)
    buf = "*2\r\n$3\r\nGET\r\n$0\r\n\r\n";
    cmd.clear();
    res = parser.parse(buf, cmd, err);
    TEST_ASSERT(res == ParseResult::Ok, "empty bulk string should parse Ok");
    TEST_ASSERT(cmd.size() == 2, "2 tokens");
    TEST_ASSERT(cmd[1] == "", "empty string token matches");
    TEST_ASSERT(buf.empty(), "buffer consumed");

    // Null bulk string ($-1\r\n)
    buf = "*2\r\n$3\r\nGET\r\n$-1\r\n";
    cmd.clear();
    res = parser.parse(buf, cmd, err);
    TEST_ASSERT(res == ParseResult::Ok, "null bulk string should parse Ok");
    TEST_ASSERT(cmd.size() == 2, "2 tokens");
    TEST_ASSERT(cmd[1] == "", "null bulk string mapped to empty string");
    TEST_ASSERT(buf.empty(), "buffer consumed");

    return true;
}

// 3. Kiểm tra phân mảnh dữ liệu (Fragmented Stream / Byte-by-byte feed)
bool test_fragmented_stream() {
    RespParser parser;
    std::string full_frame = "*3\r\n$3\r\nSET\r\n$5\r\nhello\r\n$5\r\nworld\r\n";
    std::string buffer;
    std::vector<std::string> cmd;
    std::string err;

    // Nạp từng byte một vào buffer và gọi parse()
    for (size_t i = 0; i < full_frame.size(); ++i) {
        buffer.push_back(full_frame[i]);
        ParseResult res = parser.parse(buffer, cmd, err);

        if (i < full_frame.size() - 1) {
            TEST_ASSERT(res == ParseResult::Incomplete, "intermediate bytes must return Incomplete");
            TEST_ASSERT(cmd.empty(), "no command should be emitted yet");
        } else {
            // Byte cuối cùng hoàn thiện frame
            TEST_ASSERT(res == ParseResult::Ok, "last byte should complete the frame");
            TEST_ASSERT(cmd.size() == 3, "should parse 3 tokens");
            TEST_ASSERT(cmd[0] == "SET", "token 0 matches");
            TEST_ASSERT(cmd[1] == "hello", "token 1 matches");
            TEST_ASSERT(cmd[2] == "world", "token 2 matches");
            TEST_ASSERT(buffer.empty(), "buffer consumed");
        }
    }

    return true;
}

// 4. Kiểm tra Pipelining (Nhiều lệnh nằm dính liền trong cùng 1 buffer)
bool test_pipelining() {
    RespParser parser;
    std::string buffer =
        "*1\r\n$4\r\nPING\r\n"
        "*2\r\n$4\r\nECHO\r\n$2\r\nhi\r\n"
        "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n";

    std::vector<std::string> cmd;
    std::string err;

    // Lệnh 1: PING
    ParseResult res1 = parser.parse(buffer, cmd, err);
    TEST_ASSERT(res1 == ParseResult::Ok, "cmd 1 parse Ok");
    TEST_ASSERT(cmd.size() == 1 && cmd[0] == "PING", "cmd 1 matches");

    // Lệnh 2: ECHO hi
    cmd.clear();
    ParseResult res2 = parser.parse(buffer, cmd, err);
    TEST_ASSERT(res2 == ParseResult::Ok, "cmd 2 parse Ok");
    TEST_ASSERT(cmd.size() == 2 && cmd[0] == "ECHO" && cmd[1] == "hi", "cmd 2 matches");

    // Lệnh 3: GET foo
    cmd.clear();
    ParseResult res3 = parser.parse(buffer, cmd, err);
    TEST_ASSERT(res3 == ParseResult::Ok, "cmd 3 parse Ok");
    TEST_ASSERT(cmd.size() == 2 && cmd[0] == "GET" && cmd[1] == "foo", "cmd 3 matches");

    // Hết buffer
    cmd.clear();
    ParseResult res4 = parser.parse(buffer, cmd, err);
    TEST_ASSERT(res4 == ParseResult::Incomplete, "empty buffer returns Incomplete");
    TEST_ASSERT(buffer.empty(), "buffer completely empty");

    return true;
}

// 5. Kiểm tra cú pháp lỗi (Malformed Input) & tự phục hồi
bool test_malformed_input() {
    RespParser parser;
    std::vector<std::string> cmd;
    std::string err;

    // Ký tự đầu không phải '*'
    std::string buf = "+OK\r\n";
    ParseResult res = parser.parse(buf, cmd, err);
    TEST_ASSERT(res == ParseResult::Error, "non-* array prefix must return Error");
    TEST_ASSERT(!err.empty(), "error message populated");

    // Độ dài array không hợp lệ
    buf = "*abc\r\n";
    res = parser.parse(buf, cmd, err);
    TEST_ASSERT(res == ParseResult::Error, "invalid array len must return Error");

    // Độ dài array âm
    buf = "*-5\r\n";
    res = parser.parse(buf, cmd, err);
    TEST_ASSERT(res == ParseResult::Error, "negative array count must return Error");

    // Bulk string thiếu ký tự '$'
    buf = "*1\r\n4\r\nPING\r\n";
    res = parser.parse(buf, cmd, err);
    TEST_ASSERT(res == ParseResult::Error, "missing $ must return Error");

    // Bulk length không hợp lệ
    buf = "*1\r\n$abc\r\nPING\r\n";
    res = parser.parse(buf, cmd, err);
    TEST_ASSERT(res == ParseResult::Error, "invalid bulk len must return Error");

    // Bulk length < -1
    buf = "*1\r\n$-2\r\n";
    res = parser.parse(buf, cmd, err);
    TEST_ASSERT(res == ParseResult::Error, "bulk len < -1 must return Error");

    // Bulk data không kết thúc bằng CRLF
    buf = "*1\r\n$4\r\nPINGXX";
    res = parser.parse(buf, cmd, err);
    TEST_ASSERT(res == ParseResult::Error, "missing CRLF after bulk data must return Error");
    TEST_ASSERT(buf.empty(), "buffer must be cleared on Error to prevent infinite loop");

    // Dòng array length quá dài mặc dù có CRLF (*000...001\r\n > 1024 bytes)
    buf = "*" + std::string(1500, '0') + "1\r\n";
    res = parser.parse(buf, cmd, err);
    TEST_ASSERT(res == ParseResult::Error, "array line > MAX_LINE_LEN must return Error even with CRLF");
    TEST_ASSERT(err.find("too long") != std::string::npos, "error message should mention line too long");
    TEST_ASSERT(buf.empty(), "buffer must be cleared on Error");

    // Dòng bulk length quá dài mặc dù có CRLF ($000...004\r\n > 1024 bytes)
    buf = "*1\r\n$" + std::string(1500, '0') + "4\r\nPING\r\n";
    res = parser.parse(buf, cmd, err);
    TEST_ASSERT(res == ParseResult::Error, "bulk line > MAX_LINE_LEN must return Error even with CRLF");
    TEST_ASSERT(err.find("too long") != std::string::npos, "error message should mention line too long");
    TEST_ASSERT(buf.empty(), "buffer must be cleared on Error");

    // Sau khi báo lỗi, parser phải tự reset và sẵn sàng parse lệnh chuẩn kế tiếp
    buf = "*1\r\n$4\r\nPING\r\n";
    cmd.clear();
    err.clear();
    res = parser.parse(buf, cmd, err);
    TEST_ASSERT(res == ParseResult::Ok, "parser must recover after error");
    TEST_ASSERT(cmd.size() == 1 && cmd[0] == "PING", "recovered parse matches");

    return true;
}

// 6. Kiểm tra RespSerializer cho cả 5 kiểu dữ liệu RESP2
bool test_serializer() {
    // Simple Strings
    TEST_ASSERT(RespSerializer::serialize_simple_string("PONG") == "+PONG\r\n", "simple string");
    TEST_ASSERT(RespSerializer::serialize_ok() == "+OK\r\n", "ok");
    TEST_ASSERT(RespSerializer::serialize_pong() == "+PONG\r\n", "pong");

    // Errors
    TEST_ASSERT(RespSerializer::serialize_error("unknown command") == "-ERR unknown command\r\n", "error with auto-ERR");
    TEST_ASSERT(RespSerializer::serialize_error("ERR unknown command") == "-ERR unknown command\r\n", "error without double-ERR");
    TEST_ASSERT(RespSerializer::serialize_custom_error("WRONGTYPE", "Operation against key") == "-WRONGTYPE Operation against key\r\n", "custom error");

    // Integers
    TEST_ASSERT(RespSerializer::serialize_integer(0) == ":0\r\n", "int 0");
    TEST_ASSERT(RespSerializer::serialize_integer(42) == ":42\r\n", "int 42");
    TEST_ASSERT(RespSerializer::serialize_integer(-1) == ":-1\r\n", "int -1");
    TEST_ASSERT(RespSerializer::serialize_integer(1000000000000LL) == ":1000000000000\r\n", "int 64-bit");

    // Bulk Strings
    TEST_ASSERT(RespSerializer::serialize_bulk_string("hello") == "$5\r\nhello\r\n", "bulk string");
    TEST_ASSERT(RespSerializer::serialize_bulk_string("") == "$0\r\n\r\n", "empty bulk string");
    TEST_ASSERT(RespSerializer::serialize_null_bulk_string() == "$-1\r\n", "null bulk string");
    TEST_ASSERT(RespSerializer::serialize_empty_bulk_string() == "$0\r\n\r\n", "explicit empty bulk string");

    // Arrays
    std::vector<std::string> arr = {"foo", "bar"};
    TEST_ASSERT(RespSerializer::serialize_array(arr) == "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", "array of bulk strings");
    TEST_ASSERT(RespSerializer::serialize_empty_array() == "*0\r\n", "empty array");
    TEST_ASSERT(RespSerializer::serialize_null_array() == "*-1\r\n", "null array");

    // Raw Arrays
    std::vector<std::string> raw_arr = {":1\r\n", "+OK\r\n", "$-1\r\n"};
    TEST_ASSERT(RespSerializer::serialize_raw_array(raw_arr) == "*3\r\n:1\r\n+OK\r\n$-1\r\n", "raw array");

    return true;
}

// 7. Kiểm tra payload lớn và dữ liệu UTF-8
bool test_large_payload_and_utf8() {
    RespParser parser;
    std::vector<std::string> cmd;
    std::string err;

    // UTF-8: Tiếng Việt có dấu
    std::string utf8_val = "Xin chào Redis từ C++20!";
    std::string buf = "*3\r\n$3\r\nSET\r\n$3\r\nmsg\r\n$" + std::to_string(utf8_val.size()) + "\r\n" + utf8_val + "\r\n";
    ParseResult res = parser.parse(buf, cmd, err);
    TEST_ASSERT(res == ParseResult::Ok, "UTF-8 SET should parse Ok");
    TEST_ASSERT(cmd.size() == 3, "3 tokens");
    TEST_ASSERT(cmd[2] == utf8_val, "UTF-8 payload matches");
    TEST_ASSERT(buf.empty(), "buffer consumed");

    // Large payload (100KB)
    std::string large_val(100 * 1024, 'X');
    buf = "*3\r\n$3\r\nSET\r\n$5\r\nlarge\r\n$" + std::to_string(large_val.size()) + "\r\n" + large_val + "\r\n";
    cmd.clear();
    res = parser.parse(buf, cmd, err);
    TEST_ASSERT(res == ParseResult::Ok, "100KB payload should parse Ok");
    TEST_ASSERT(cmd.size() == 3, "3 tokens");
    TEST_ASSERT(cmd[2].size() == 100 * 1024, "payload length matches");
    TEST_ASSERT(cmd[2] == large_val, "payload content matches");
    TEST_ASSERT(buf.empty(), "buffer consumed");

    return true;
}

// 8. Kiểm tra stream với các kích thước chunk ngẫu nhiên (Stress chunked stream)
bool test_variable_chunk_streaming() {
    std::string stream_data;
    // Tạo 50 lệnh pipelined
    for (int i = 0; i < 50; ++i) {
        stream_data += "*2\r\n$4\r\nECHO\r\n$" + std::to_string(std::to_string(i).size()) + "\r\n" + std::to_string(i) + "\r\n";
    }

    // Thử các bước chunk: 1, 3, 7, 13, 29 bytes
    std::vector<size_t> chunk_sizes = {1, 3, 7, 13, 29};
    for (size_t chunk_size : chunk_sizes) {
        RespParser parser;
        std::string buffer;
        std::vector<std::string> cmd;
        std::string err;
        int parsed_cmds = 0;

        size_t offset = 0;
        while (offset < stream_data.size()) {
            size_t take = std::min(chunk_size, stream_data.size() - offset);
            buffer.append(stream_data.data() + offset, take);
            offset += take;

            while (true) {
                ParseResult res = parser.parse(buffer, cmd, err);
                if (res == ParseResult::Ok) {
                    TEST_ASSERT(cmd.size() == 2, "ECHO has 2 tokens");
                    TEST_ASSERT(cmd[0] == "ECHO", "cmd is ECHO");
                    TEST_ASSERT(cmd[1] == std::to_string(parsed_cmds), "arg matches index");
                    parsed_cmds++;
                    cmd.clear();
                } else if (res == ParseResult::Incomplete) {
                    break;
                } else {
                    TEST_ASSERT(false, "unexpected error during streaming");
                }
            }
        }
        TEST_ASSERT(parsed_cmds == 50, "all 50 commands must be parsed");
        TEST_ASSERT(buffer.empty(), "buffer must be empty at the end");
    }

    return true;
}

int main() {
    std::cout << "========================================\n";
    std::cout << "  RUNNING RESP PARSER & SERIALIZER TESTS\n";
    std::cout << "========================================\n";

    RUN_TEST(test_single_commands);
    RUN_TEST(test_binary_safety_and_special_chars);
    RUN_TEST(test_fragmented_stream);
    RUN_TEST(test_pipelining);
    RUN_TEST(test_malformed_input);
    RUN_TEST(test_serializer);
    RUN_TEST(test_large_payload_and_utf8);
    RUN_TEST(test_variable_chunk_streaming);

    std::cout << "========================================\n";
    std::cout << "Results: " << g_tests_passed << "/" << g_tests_run << " tests passed.\n";
    std::cout << "========================================\n";

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
