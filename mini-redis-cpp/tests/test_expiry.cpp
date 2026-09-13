#include "commands/dispatcher.hpp"
#include "protocol/resp_serializer.hpp"
#include "server/connection.hpp"
#include "store/data_store.hpp"
#include "store/expiry.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
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

// ─── 1. Kiểm tra EXPIRE và TTL cơ bản với mô phỏng thời gian & sleep ─────────
bool test_expiry_basic_ttl() {
    DataStore store;

    // Key chưa tồn tại -> TTL trả về -2
    TEST_ASSERT(store.expiry().get_ttl_seconds("k1", store.exists("k1")) == -2, "missing key TTL is -2");

    // Key tồn tại nhưng chưa set EXPIRE -> TTL trả về -1
    store.set("k1", "v1");
    TEST_ASSERT(store.expiry().get_ttl_seconds("k1", store.exists("k1")) == -1, "key without TTL is -1");

    // Set TTL 50ms
    store.expiry().set_expiry("k1", std::chrono::milliseconds(50));
    TEST_ASSERT(store.expiry().size() == 1, "expiry size is 1");
    TEST_ASSERT(store.expiry().get_ttl_seconds("k1", store.exists("k1")) >= 1, "unexpired key TTL >= 1");

    // Ngủ 70ms để key hết hạn
    std::this_thread::sleep_for(std::chrono::milliseconds(70));

    // TTL trả về -2 sau khi hết hạn
    TEST_ASSERT(store.expiry().is_expired("k1"), "k1 is expired");
    TEST_ASSERT(store.expiry().get_ttl_seconds("k1", store.exists("k1")) == -2, "expired key TTL is -2");

    // Lazy expiry: get(k1) trả về nullptr và dọn dẹp khỏi store_
    TEST_ASSERT(store.get("k1") == nullptr, "get expired key returns nullptr");
    TEST_ASSERT(store.dbsize() == 0, "store is empty after lazy eviction");
    TEST_ASSERT(store.expiry().size() == 0, "expiry map is empty after eviction");

    return true;
}

// ─── 2. Kiểm tra lệnh PERSIST ────────────────────────────────────────────────
bool test_expiry_persist() {
    DataStore store;

    // PERSIST trên key không tồn tại -> false
    TEST_ASSERT(!store.expiry().persist("nonexistent"), "persist on missing key returns false");

    // Key có dữ liệu nhưng không có TTL -> false
    store.set("foo", "bar");
    TEST_ASSERT(!store.expiry().persist("foo"), "persist on key without TTL returns false");

    // Gán TTL 10 giây
    store.expiry().set_expiry("foo", std::chrono::seconds(10));
    TEST_ASSERT(store.expiry().size() == 1, "expiry size is 1");
    TEST_ASSERT(store.expiry().get_ttl_seconds("foo", store.exists("foo")) > 0, "TTL > 0");

    // PERSIST thành công -> true
    TEST_ASSERT(store.expiry().persist("foo"), "persist returns true");
    TEST_ASSERT(store.expiry().size() == 0, "expiry map empty after persist");
    TEST_ASSERT(store.expiry().get_ttl_seconds("foo", store.exists("foo")) == -1, "TTL becomes -1");

    // Dữ liệu vẫn còn nguyên vẹn
    const std::string* val = store.get("foo");
    TEST_ASSERT(val != nullptr && *val == "bar", "value preserved after persist");

    return true;
}

// ─── 3. Kiểm tra SET ghi đè xóa bỏ TTL cũ (Redis spec) ───────────────────────
bool test_expiry_set_overwrites_ttl() {
    DataStore store;

    store.set("key", "val1");
    store.expiry().set_expiry("key", std::chrono::seconds(60));
    TEST_ASSERT(store.expiry().get_ttl_seconds("key", store.exists("key")) > 0, "TTL active");

    // Ghi đè SET key val2 -> phải xóa bỏ TTL
    store.set("key", "val2");
    TEST_ASSERT(store.expiry().size() == 0, "TTL removed after SET overwrite");
    TEST_ASSERT(store.expiry().get_ttl_seconds("key", store.exists("key")) == -1, "TTL is -1 after overwrite");

    const std::string* val = store.get("key");
    TEST_ASSERT(val != nullptr && *val == "val2", "new value stored");

    return true;
}

// ─── 4. Kiểm tra EXPIRE với số 0 hoặc số âm (xóa ngay lập tức) ───────────────
bool test_expiry_zero_or_negative() {
    Dispatcher dispatcher;
    DataStore store;
    std::string out;

    // EXPIRE key 0 -> xóa ngay lập tức
    store.set("k_zero", "v");
    dispatcher.dispatch({"EXPIRE", "k_zero", "0"}, store, out);
    TEST_ASSERT(out == ":1\r\n", "EXPIRE 0 returns :1");
    TEST_ASSERT(store.get("k_zero") == nullptr, "key deleted immediately on EXPIRE 0");
    TEST_ASSERT(store.dbsize() == 0, "dbsize is 0");

    // EXPIRE key -10 -> xóa ngay lập tức
    out.clear();
    store.set("k_neg", "v");
    dispatcher.dispatch({"EXPIRE", "k_neg", "-10"}, store, out);
    TEST_ASSERT(out == ":1\r\n", "EXPIRE negative returns :1");
    TEST_ASSERT(store.get("k_neg") == nullptr, "key deleted immediately on EXPIRE negative");

    // EXPIRE trên key không tồn tại -> trả về :0
    out.clear();
    dispatcher.dispatch({"EXPIRE", "ghost", "10"}, store, out);
    TEST_ASSERT(out == ":0\r\n", "EXPIRE missing key returns :0");

    return true;
}

// ─── 5. Kiểm tra Lazy Expiry trên toàn bộ các lệnh đọc/ghi ────────────────────
bool test_lazy_expiry_all_commands() {
    // 5.1 GET
    {
        DataStore store;
        store.set("k", "v");
        store.expiry().set_expiry("k", std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        TEST_ASSERT(store.get("k") == nullptr, "GET evicts expired key");
        TEST_ASSERT(store.dbsize() == 0, "store empty after GET lazy eviction");
    }

    // 5.2 EXISTS
    {
        DataStore store;
        store.set("k", "v");
        store.expiry().set_expiry("k", std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        TEST_ASSERT(!store.exists("k"), "EXISTS returns false on expired key");
        TEST_ASSERT(store.dbsize() == 0, "store empty after EXISTS lazy eviction");
    }

    // 5.3 INCR (Khi key đã hết hạn -> xóa key và khởi tạo lại từ 1)
    {
        DataStore store;
        store.set("cnt", "50");
        store.expiry().set_expiry("cnt", std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        int64_t new_val = 0;
        auto res = store.incr("cnt", new_val);
        TEST_ASSERT(res == DataStore::IncrResult::Ok, "INCR on expired key succeeds");
        TEST_ASSERT(new_val == 1, "INCR on expired key resets and initializes to 1");
        TEST_ASSERT(store.expiry().get_ttl_seconds("cnt", true) == -1, "new key has no TTL");
    }

    // 5.4 TYPE
    {
        DataStore store;
        store.set("k", "v");
        store.expiry().set_expiry("k", std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        TEST_ASSERT(store.type("k") == "none", "TYPE on expired key returns 'none'");
        TEST_ASSERT(store.dbsize() == 0, "store empty after TYPE lazy eviction");
    }

    // 5.5 DEL (Key đã hết hạn -> không tính vào số key bị xóa, trả về 0)
    {
        DataStore store;
        store.set("k", "v");
        store.expiry().set_expiry("k", std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        TEST_ASSERT(store.del("k") == false, "DEL on expired key returns false");
        TEST_ASSERT(store.dbsize() == 0, "store empty");
    }

    return true;
}

// ─── 6. Kiểm tra Quét chủ động (Active Expiry Cycle) ──────────────────────────
bool test_active_expiry_cycle() {
    DataStore store;

    // Tạo 100 keys với TTL 20ms
    for (int i = 0; i < 100; ++i) {
        std::string key = "key_" + std::to_string(i);
        store.set(key, "value");
        store.expiry().set_expiry(key, std::chrono::milliseconds(20));
    }

    TEST_ASSERT(store.dbsize() == 100, "100 keys inserted");
    TEST_ASSERT(store.expiry().size() == 100, "100 TTLs tracked");

    // Đợi 40ms để toàn bộ 100 keys hết hạn
    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    // TUYỆT ĐỐI KHÔNG GỌI GET / EXISTS để kích hoạt lazy expiry!
    // Kích hoạt chu trình Active Expiry quét xác suất:
    size_t evicted = store.expiry().active_expire_cycle(store, 20, 0.25, 50);

    TEST_ASSERT(evicted == 100, "active_expire_cycle evicted all 100 expired keys");
    TEST_ASSERT(store.dbsize() == 0, "store is empty after active cycle");
    TEST_ASSERT(store.expiry().size() == 0, "expiry map is empty");

    return true;
}

// ─── 7. Kiểm tra Quét chủ động hỗn hợp (chỉ xóa key hết hạn, giữ key còn hạn) ─
bool test_active_expiry_mixed() {
    DataStore store;

    // 50 keys hết hạn (TTL 10ms)
    for (int i = 0; i < 50; ++i) {
        std::string key = "exp_" + std::to_string(i);
        store.set(key, "val");
        store.expiry().set_expiry(key, std::chrono::milliseconds(10));
    }

    // 50 keys sống lâu (TTL 100 giây)
    for (int i = 0; i < 50; ++i) {
        std::string key = "live_" + std::to_string(i);
        store.set(key, "val");
        store.expiry().set_expiry(key, std::chrono::seconds(100));
    }

    TEST_ASSERT(store.dbsize() == 100, "100 keys total");

    // Đợi 25ms để 50 keys đầu hết hạn
    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    // Chạy chu trình Active Expiry
    size_t evicted = store.expiry().active_expire_cycle(store);
    TEST_ASSERT(evicted > 0, "active_expire_cycle evicted expired keys");
    TEST_ASSERT(store.dbsize() < 100, "active expire reduced total key count");

    // Live keys phải được giữ nguyên vẹn 100%, không bị xóa nhầm
    for (int i = 0; i < 50; ++i) {
        std::string live_key = "live_" + std::to_string(i);
        const std::string* val = store.get(live_key);
        TEST_ASSERT(val != nullptr && *val == "val", "live key is untouched");
    }

    // Các key hết hạn còn sót lại (nếu có) khi được client truy vấn sẽ được Lazy Expiry dọn sạch
    for (int i = 0; i < 50; ++i) {
        std::string exp_key = "exp_" + std::to_string(i);
        TEST_ASSERT(store.get(exp_key) == nullptr, "expired key returns nullptr via lazy expiry");
    }

    // Cuối cùng: đúng 50 live keys còn lại trong DB
    TEST_ASSERT(store.dbsize() == 50, "exactly 50 live keys remain in store");
    TEST_ASSERT(store.expiry().size() == 50, "exactly 50 live keys remain in expiry");

    return true;
}

// ─── 8. Kiểm tra Dispatcher & Handlers cho lệnh EXPIRE, TTL, PERSIST ─────────
bool test_dispatcher_expiry_commands() {
    Dispatcher dispatcher;
    DataStore store;
    std::string out;

    // SET key value
    dispatcher.dispatch({"SET", "mykey", "hello"}, store, out);
    TEST_ASSERT(out == "+OK\r\n", "SET OK");

    // TTL khi chưa có expire -> :-1
    out.clear();
    dispatcher.dispatch({"TTL", "mykey"}, store, out);
    TEST_ASSERT(out == ":-1\r\n", "TTL without expiry is :-1");

    // EXPIRE mykey 10 -> :1
    out.clear();
    dispatcher.dispatch({"EXPIRE", "mykey", "10"}, store, out);
    TEST_ASSERT(out == ":1\r\n", "EXPIRE returns :1");

    // TTL sau khi đặt 10s -> :10 (hoặc :9)
    out.clear();
    dispatcher.dispatch({"TTL", "mykey"}, store, out);
    TEST_ASSERT(out == ":10\r\n" || out == ":9\r\n", "TTL is remaining seconds");

    // PERSIST mykey -> :1
    out.clear();
    dispatcher.dispatch({"PERSIST", "mykey"}, store, out);
    TEST_ASSERT(out == ":1\r\n", "PERSIST returns :1");

    // TTL sau khi PERSIST -> :-1
    out.clear();
    dispatcher.dispatch({"TTL", "mykey"}, store, out);
    TEST_ASSERT(out == ":-1\r\n", "TTL after PERSIST is :-1");

    // PERSIST lần nữa -> :0
    out.clear();
    dispatcher.dispatch({"PERSIST", "mykey"}, store, out);
    TEST_ASSERT(out == ":0\r\n", "PERSIST again returns :0");

    // TTL key không tồn tại -> :-2
    out.clear();
    dispatcher.dispatch({"TTL", "nokey"}, store, out);
    TEST_ASSERT(out == ":-2\r\n", "TTL nonexistent is :-2");

    // Lỗi tham số: EXPIRE không phải số
    out.clear();
    dispatcher.dispatch({"EXPIRE", "mykey", "not_a_number"}, store, out);
    TEST_ASSERT(out == "-ERR value is not an integer or out of range\r\n", "EXPIRE non-int error");

    // Lỗi số lượng tham số (arity)
    out.clear();
    dispatcher.dispatch({"EXPIRE", "mykey"}, store, out);
    TEST_ASSERT(out == "-ERR wrong number of arguments for 'expire' command\r\n", "EXPIRE arity error");

    out.clear();
    dispatcher.dispatch({"TTL"}, store, out);
    TEST_ASSERT(out == "-ERR wrong number of arguments for 'ttl' command\r\n", "TTL arity error");

    out.clear();
    dispatcher.dispatch({"PERSIST"}, store, out);
    TEST_ASSERT(out == "-ERR wrong number of arguments for 'persist' command\r\n", "PERSIST arity error");

    return true;
}

// ─── 9. Kiểm tra Client Connection last_active & Idle Timeout ────────────────
bool test_connection_last_active_tracking() {
    Connection conn(10, "127.0.0.1", 12345);

    auto t1 = conn.last_active();
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    conn.update_last_active();
    auto t2 = conn.last_active();

    TEST_ASSERT(t2 > t1, "last_active is updated to a later time");

    // Move constructor retains last_active
    Connection conn2(std::move(conn));
    TEST_ASSERT(conn2.last_active() == t2, "move constructor preserves last_active");

    return true;
}

int main() {
    std::cout << "========================================\n";
    std::cout << "    RUNNING EXPIRY & TIMEOUT TESTS      \n";
    std::cout << "========================================\n";

    RUN_TEST(test_expiry_basic_ttl);
    RUN_TEST(test_expiry_persist);
    RUN_TEST(test_expiry_set_overwrites_ttl);
    RUN_TEST(test_expiry_zero_or_negative);
    RUN_TEST(test_lazy_expiry_all_commands);
    RUN_TEST(test_active_expiry_cycle);
    RUN_TEST(test_active_expiry_mixed);
    RUN_TEST(test_dispatcher_expiry_commands);
    RUN_TEST(test_connection_last_active_tracking);

    std::cout << "========================================\n";
    std::cout << "Results: " << g_tests_passed << "/" << g_tests_run << " tests passed.\n";
    std::cout << "========================================\n";

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
