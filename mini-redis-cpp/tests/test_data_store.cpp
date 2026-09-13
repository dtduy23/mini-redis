#include "commands/dispatcher.hpp"
#include "protocol/resp_parser.hpp"
#include "protocol/resp_serializer.hpp"
#include "store/data_store.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
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

// ─── 1. Kiểm tra SET và GET cơ bản ───────────────────────────────────────────
bool test_datastore_basic_set_get() {
    DataStore store;

    // Ban đầu key không tồn tại
    TEST_ASSERT(store.get("foo") == nullptr, "get non-existent key returns nullptr");
    TEST_ASSERT(!store.exists("foo"), "exists returns false for non-existent key");
    TEST_ASSERT(store.dbsize() == 0, "dbsize is 0 initially");

    // SET key value
    store.set("foo", "bar");
    const std::string* val = store.get("foo");
    TEST_ASSERT(val != nullptr, "get existing key returns non-null");
    TEST_ASSERT(*val == "bar", "value matches 'bar'");
    TEST_ASSERT(store.exists("foo"), "exists returns true");
    TEST_ASSERT(store.dbsize() == 1, "dbsize is 1");

    // Ghi đè key (overwrite)
    store.set("foo", "baz");
    val = store.get("foo");
    TEST_ASSERT(val != nullptr && *val == "baz", "overwritten value matches 'baz'");
    TEST_ASSERT(store.dbsize() == 1, "dbsize remains 1 after overwrite");

    return true;
}

// ─── 2. Kiểm tra Binary Safety (chứa ký tự \0, emoji, chuỗi nhị phân) ────────
bool test_datastore_binary_safety() {
    DataStore store;

    // Key và Value chứa null byte ở giữa
    std::string bin_key("user\0id\0key", 11);
    std::string bin_val("\x00\x01\x02\xFF\xFE\xFD", 6);

    store.set(bin_key, bin_val);
    const std::string* res = store.get(bin_key);
    TEST_ASSERT(res != nullptr, "binary key found");
    TEST_ASSERT(*res == bin_val, "binary value preserved exactly with null bytes");
    TEST_ASSERT(res->size() == 6, "binary value size is 6");

    // UTF-8 tiếng Việt
    std::string vn_key = "xin_chào";
    std::string vn_val = "Cộng hòa Xã hội Chủ nghĩa Việt Nam";
    store.set(vn_key, vn_val);
    const std::string* vn_res = store.get(vn_key);
    TEST_ASSERT(vn_res != nullptr && *vn_res == vn_val, "UTF-8 preserved");

    return true;
}

// ─── 3. Kiểm tra DEL và EXISTS (hỗ trợ nhiều key và trùng lặp) ───────────────
bool test_datastore_del_and_exists() {
    DataStore store;

    store.set("k1", "v1");
    store.set("k2", "v2");
    store.set("k3", "v3");
    TEST_ASSERT(store.dbsize() == 3, "initial size is 3");

    // EXISTS với nhiều key
    std::vector<std::string> check_keys = {"k1", "k2", "k_missing"};
    TEST_ASSERT(store.exists(check_keys) == 2, "exists counts 2 existing keys");

    // EXISTS với key trùng nhau (theo chuẩn Redis: đếm mỗi lần xuất hiện)
    std::vector<std::string> dup_keys = {"k1", "k1", "k2"};
    TEST_ASSERT(store.exists(dup_keys) == 3, "exists counts duplicated existing keys");

    // DEL 1 key đơn lẻ
    TEST_ASSERT(store.del("k1") == true, "del single existing key returns true");
    TEST_ASSERT(store.del("k1") == false, "del non-existent key returns false");
    TEST_ASSERT(store.dbsize() == 2, "size is 2 after del");

    // DEL nhiều key (k2 tồn tại, k3 tồn tại, k_missing không tồn tại)
    std::vector<std::string> del_keys = {"k2", "k3", "k_missing"};
    TEST_ASSERT(store.del(del_keys) == 2, "del returns count of deleted keys (2)");
    TEST_ASSERT(store.dbsize() == 0, "store is empty now");

    // DEL danh sách có key trùng lặp: {"a", "a"}
    store.set("a", "1");
    std::vector<std::string> dup_del = {"a", "a"};
    TEST_ASSERT(store.del(dup_del) == 1, "del with duplicates erases key only once");
    TEST_ASSERT(store.dbsize() == 0, "store is empty");

    return true;
}

// ─── 4. Kiểm tra INCR (các trường hợp thành công, lỗi không phải số, tràn số) ─
bool test_datastore_incr_semantics() {
    DataStore store;
    int64_t out_val = 0;

    // Key chưa tồn tại -> khởi tạo bằng 1
    auto res = store.incr("cnt", out_val);
    TEST_ASSERT(res == DataStore::IncrResult::Ok, "incr on missing key succeeds");
    TEST_ASSERT(out_val == 1, "incr on missing key initializes to 1");
    const std::string* stored = store.get("cnt");
    TEST_ASSERT(stored != nullptr && *stored == "1", "stored string is '1'");

    // Key đang là 1 -> tăng lên 2
    res = store.incr("cnt", out_val);
    TEST_ASSERT(res == DataStore::IncrResult::Ok && out_val == 2, "incr increments to 2");

    // Số âm: -10 -> tăng lên -9
    store.set("neg", "-10");
    res = store.incr("neg", out_val);
    TEST_ASSERT(res == DataStore::IncrResult::Ok && out_val == -9, "incr on negative integer");
    TEST_ASSERT(*store.get("neg") == "-9", "stored is '-9'");

    // Giá trị không phải số nguyên: "abc"
    store.set("str", "abc");
    res = store.incr("str", out_val);
    TEST_ASSERT(res == DataStore::IncrResult::NotAnInteger, "incr on string returns NotAnInteger");
    TEST_ASSERT(*store.get("str") == "abc", "original value unchanged");

    // Chuỗi có ký tự thừa: "123a" hoặc khoảng trắng
    store.set("trailing", "123a");
    TEST_ASSERT(store.incr("trailing", out_val) == DataStore::IncrResult::NotAnInteger, "trailing char rejected");

    store.set("spaces", " 42 ");
    TEST_ASSERT(store.incr("spaces", out_val) == DataStore::IncrResult::NotAnInteger, "spaces rejected");

    // Số thực: "3.14"
    store.set("float", "3.14");
    TEST_ASSERT(store.incr("float", out_val) == DataStore::IncrResult::NotAnInteger, "float rejected");

    // Cận biên 64-bit: INT64_MAX - 1 -> INT64_MAX
    int64_t max_val = std::numeric_limits<int64_t>::max();
    store.set("max_minus_1", std::to_string(max_val - 1));
    res = store.incr("max_minus_1", out_val);
    TEST_ASSERT(res == DataStore::IncrResult::Ok && out_val == max_val, "incr to INT64_MAX succeeds");

    // Tràn số: INT64_MAX -> Overflow
    store.set("max_val", std::to_string(max_val));
    res = store.incr("max_val", out_val);
    TEST_ASSERT(res == DataStore::IncrResult::Overflow, "incr on INT64_MAX returns Overflow");

    // Cận biên âm INT64_MIN -> INT64_MIN + 1
    int64_t min_val = std::numeric_limits<int64_t>::min();
    store.set("min_val", std::to_string(min_val));
    res = store.incr("min_val", out_val);
    TEST_ASSERT(res == DataStore::IncrResult::Ok && out_val == min_val + 1, "incr on INT64_MIN succeeds");

    return true;
}

// ─── 5. Kiểm tra TYPE, FLUSHALL, DBSIZE ───────────────────────────────────────
bool test_datastore_type_and_flushall() {
    DataStore store;

    TEST_ASSERT(store.type("nonexistent") == "none", "type of nonexistent key is 'none'");

    store.set("mykey", "hello");
    TEST_ASSERT(store.type("mykey") == "string", "type of existing key is 'string'");

    store.set("k2", "world");
    TEST_ASSERT(store.dbsize() == 2, "dbsize is 2");

    store.flushall();
    TEST_ASSERT(store.dbsize() == 0, "dbsize after flushall is 0");
    TEST_ASSERT(store.get("mykey") == nullptr, "key cleared after flushall");
    TEST_ASSERT(store.type("mykey") == "none", "type is 'none' after flushall");

    return true;
}

// ─── 6. Kiểm tra Dispatcher & Handlers thực thi 9 lệnh cốt lõi ───────────────
bool test_dispatcher_core_commands() {
    Dispatcher dispatcher;
    DataStore store;
    std::string out;

    // 1. PING không tham số -> +PONG\r\n
    out.clear();
    dispatcher.dispatch({"PING"}, store, out);
    TEST_ASSERT(out == "+PONG\r\n", "PING replies +PONG");

    // 2. PING có message -> $5\r\nhello\r\n
    out.clear();
    dispatcher.dispatch({"PING", "hello"}, store, out);
    TEST_ASSERT(out == "$5\r\nhello\r\n", "PING msg replies bulk string");

    // 3. ECHO message -> $11\r\nhello world\r\n
    out.clear();
    dispatcher.dispatch({"ECHO", "hello world"}, store, out);
    TEST_ASSERT(out == "$11\r\nhello world\r\n", "ECHO replies bulk string");

    // 4. SET key value -> +OK\r\n
    out.clear();
    dispatcher.dispatch({"SET", "mykey", "myvalue"}, store, out);
    TEST_ASSERT(out == "+OK\r\n", "SET replies +OK");

    // 5. GET key tồn tại -> $7\r\nmyvalue\r\n
    out.clear();
    dispatcher.dispatch({"GET", "mykey"}, store, out);
    TEST_ASSERT(out == "$7\r\nmyvalue\r\n", "GET replies bulk string");

    // 6. GET key không tồn tại -> $-1\r\n
    out.clear();
    dispatcher.dispatch({"GET", "nokey"}, store, out);
    TEST_ASSERT(out == "$-1\r\n", "GET missing key replies null bulk string");

    // 7. EXISTS
    out.clear();
    dispatcher.dispatch({"EXISTS", "mykey", "nokey", "mykey"}, store, out);
    TEST_ASSERT(out == ":2\r\n", "EXISTS returns :2");

    // 8. INCR
    out.clear();
    dispatcher.dispatch({"INCR", "counter"}, store, out);
    TEST_ASSERT(out == ":1\r\n", "INCR new key returns :1");
    out.clear();
    dispatcher.dispatch({"INCR", "counter"}, store, out);
    TEST_ASSERT(out == ":2\r\n", "INCR returns :2");

    // INCR trên chuỗi không phải số
    out.clear();
    dispatcher.dispatch({"INCR", "mykey"}, store, out);
    TEST_ASSERT(out == "-ERR value is not an integer or out of range\r\n", "INCR bad value error");

    // 9. TYPE
    out.clear();
    dispatcher.dispatch({"TYPE", "mykey"}, store, out);
    TEST_ASSERT(out == "+string\r\n", "TYPE existing key replies +string");
    out.clear();
    dispatcher.dispatch({"TYPE", "ghost"}, store, out);
    TEST_ASSERT(out == "+none\r\n", "TYPE missing key replies +none");

    // 10. DEL
    out.clear();
    dispatcher.dispatch({"DEL", "mykey", "counter", "nokey"}, store, out);
    TEST_ASSERT(out == ":2\r\n", "DEL deleted 2 keys");
    TEST_ASSERT(store.get("mykey") == nullptr, "mykey is deleted");

    // 11. FLUSHALL
    store.set("a", "1");
    out.clear();
    dispatcher.dispatch({"FLUSHALL"}, store, out);
    TEST_ASSERT(out == "+OK\r\n", "FLUSHALL replies +OK");
    TEST_ASSERT(store.dbsize() == 0, "FLUSHALL cleared db");

    return true;
}

// ─── 7. Kiểm tra Dispatcher Case-Insensitive (không phân biệt hoa thường tên lệnh)
bool test_dispatcher_case_insensitivity() {
    Dispatcher dispatcher;
    DataStore store;
    std::string out;

    dispatcher.dispatch({"set", "k", "v"}, store, out);
    TEST_ASSERT(out == "+OK\r\n", "'set' lowercase works");

    out.clear();
    dispatcher.dispatch({"GeT", "k"}, store, out);
    TEST_ASSERT(out == "$1\r\nv\r\n", "'GeT' mixed case works");

    out.clear();
    dispatcher.dispatch({"pInG"}, store, out);
    TEST_ASSERT(out == "+PONG\r\n", "'pInG' mixed case works");

    // Chú ý: Key và Value vẫn giữ nguyên phân biệt hoa thường
    const std::string* v = store.get("k");
    TEST_ASSERT(v != nullptr && *v == "v", "lowercase key is stored");
    TEST_ASSERT(store.get("K") == nullptr, "uppercase key does not match");

    return true;
}

// ─── 8. Kiểm tra Dispatcher Arity Errors (sai số lượng tham số) ───────────────
bool test_dispatcher_arity_errors() {
    Dispatcher dispatcher;
    DataStore store;
    std::string out;

    // SET thiếu arg: SET key
    out.clear();
    dispatcher.dispatch({"SET", "k"}, store, out);
    TEST_ASSERT(out == "-ERR wrong number of arguments for 'set' command\r\n", "SET 1 arg error");

    // SET thừa arg: SET k v extra
    out.clear();
    dispatcher.dispatch({"SET", "k", "v", "extra"}, store, out);
    TEST_ASSERT(out == "-ERR wrong number of arguments for 'set' command\r\n", "SET 3 args error");

    // GET không có arg
    out.clear();
    dispatcher.dispatch({"GET"}, store, out);
    TEST_ASSERT(out == "-ERR wrong number of arguments for 'get' command\r\n", "GET 0 arg error");

    // DEL không có arg
    out.clear();
    dispatcher.dispatch({"DEL"}, store, out);
    TEST_ASSERT(out == "-ERR wrong number of arguments for 'del' command\r\n", "DEL 0 arg error");

    // PING quá 2 arg
    out.clear();
    dispatcher.dispatch({"PING", "a", "b"}, store, out);
    TEST_ASSERT(out == "-ERR wrong number of arguments for 'ping' command\r\n", "PING 2 args error");

    // FLUSHALL có arg
    out.clear();
    dispatcher.dispatch({"FLUSHALL", "extra"}, store, out);
    TEST_ASSERT(out == "-ERR wrong number of arguments for 'flushall' command\r\n", "FLUSHALL with arg error");

    return true;
}

// ─── 9. Kiểm tra Unknown Command ─────────────────────────────────────────────
bool test_dispatcher_unknown_command() {
    Dispatcher dispatcher;
    DataStore store;
    std::string out;

    dispatcher.dispatch({"FOOBAR", "arg1"}, store, out);
    TEST_ASSERT(out == "-ERR unknown command 'FOOBAR'\r\n", "unknown command error format");

    return true;
}

// ─── 10. Pipeline RESP parser + Dispatcher integration ───────────────────────
bool test_resp_pipeline_integration() {
    RespParser parser;
    Dispatcher dispatcher;
    DataStore store;
    std::string response_buf;

    // 3 lệnh gửi liên tiếp trong 1 buffer (pipelining):
    // 1. SET num 100
    // 2. INCR num
    // 3. GET num
    std::string wire_data =
        "*3\r\n$3\r\nSET\r\n$3\r\nnum\r\n$3\r\n100\r\n"
        "*2\r\n$4\r\nINCR\r\n$3\r\nnum\r\n"
        "*2\r\n$3\r\nGET\r\n$3\r\nnum\r\n";

    std::string_view stream(wire_data);
    while (!stream.empty()) {
        size_t consumed = 0;
        std::vector<std::string> cmd;
        std::string err;

        ParseResult res = parser.parse(stream, consumed, cmd, err);
        TEST_ASSERT(res == ParseResult::Ok, "command parses Ok");
        TEST_ASSERT(consumed > 0, "consumed > 0");

        stream.remove_prefix(consumed);
        dispatcher.dispatch(cmd, store, response_buf);
    }

    // Kết quả mong đợi trong response_buf:
    // +OK\r\n
    // :101\r\n
    // $3\r\n101\r\n
    std::string expected = "+OK\r\n:101\r\n$3\r\n101\r\n";
    TEST_ASSERT(response_buf == expected, "pipelined responses match exactly");

    return true;
}

int main() {
    std::cout << "========================================\n";
    std::cout << "  RUNNING DATA STORE & COMMAND TESTS    \n";
    std::cout << "========================================\n";

    RUN_TEST(test_datastore_basic_set_get);
    RUN_TEST(test_datastore_binary_safety);
    RUN_TEST(test_datastore_del_and_exists);
    RUN_TEST(test_datastore_incr_semantics);
    RUN_TEST(test_datastore_type_and_flushall);
    RUN_TEST(test_dispatcher_core_commands);
    RUN_TEST(test_dispatcher_case_insensitivity);
    RUN_TEST(test_dispatcher_arity_errors);
    RUN_TEST(test_dispatcher_unknown_command);
    RUN_TEST(test_resp_pipeline_integration);

    std::cout << "========================================\n";
    std::cout << "Results: " << g_tests_passed << "/" << g_tests_run << " tests passed.\n";
    std::cout << "========================================\n";

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}

