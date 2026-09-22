#include "commands/dispatcher.hpp"
#include "protocol/resp_serializer.hpp"
#include "server/bio.hpp"
#include "store/data_store.hpp"

#include <atomic>
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

// ─── 1. Kiểm tra BioManager chạy tác vụ bất đồng bộ trên luồng riêng ─────────
bool test_bio_async_execution() {
    auto& bio = BioManager::instance();
    auto main_tid = std::this_thread::get_id();

    std::atomic<bool> job_done{false};
    std::thread::id worker_tid{};

    bio.submit(BioType::LazyFree, [&]() {
        worker_tid = std::this_thread::get_id();
        job_done.store(true);
    });

    bio.wait_empty(BioType::LazyFree);

    TEST_ASSERT(job_done.load(), "job was executed");
    TEST_ASSERT(worker_tid != main_tid, "job executed on separate worker thread");
    TEST_ASSERT(bio.pending(BioType::LazyFree) == 0, "pending count is 0 after completion");
    return true;
}

// ─── 2. Kiểm tra BioManager xử lý nhiều tác vụ đồng thời ─────────────────────
bool test_bio_multiple_jobs() {
    auto& bio = BioManager::instance();
    constexpr int NUM_JOBS = 50;
    std::atomic<int> counter{0};

    for (int i = 0; i < NUM_JOBS; ++i) {
        bio.submit(BioType::LazyFree, [&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    bio.wait_empty(BioType::LazyFree);

    TEST_ASSERT(counter.load() == NUM_JOBS, "all 50 jobs completed");
    TEST_ASSERT(bio.pending(BioType::LazyFree) == 0, "pending count is 0");
    return true;
}

// ─── 3. Kiểm tra DataStore::unlink một key ───────────────────────────────────
bool test_datastore_unlink_single_key() {
    DataStore store;
    store.set("user:100", "Alice");

    TEST_ASSERT(store.exists("user:100"), "key exists initially");
    TEST_ASSERT(store.dbsize() == 1, "dbsize is 1");

    bool unlinked = store.unlink("user:100");
    TEST_ASSERT(unlinked, "unlink returns true for existing key");
    TEST_ASSERT(!store.exists("user:100"), "key no longer exists in store");
    TEST_ASSERT(store.get("user:100") == nullptr, "get returns nullptr");
    TEST_ASSERT(store.dbsize() == 0, "dbsize is 0");

    // Chờ luồng BIO giải phóng hoàn tất
    BioManager::instance().wait_empty(BioType::LazyFree);

    // Unlink lại lần nữa (không tồn tại) phải trả về false
    TEST_ASSERT(!store.unlink("user:100"), "unlink non-existent key returns false");
    return true;
}

// ─── 4. Kiểm tra DataStore::unlink nhiều keys cùng lúc ───────────────────────
bool test_datastore_unlink_multi_keys() {
    DataStore store;
    store.set("k1", "v1");
    store.set("k2", "v2");
    store.set("k3", "v3");
    store.set("k4", "v4");

    std::vector<std::string> keys_to_unlink = {"k1", "k2", "k999", "k3"};
    int64_t count = store.unlink(keys_to_unlink);

    TEST_ASSERT(count == 3, "unlinked exactly 3 existing keys");
    TEST_ASSERT(!store.exists("k1"), "k1 removed");
    TEST_ASSERT(!store.exists("k2"), "k2 removed");
    TEST_ASSERT(!store.exists("k3"), "k3 removed");
    TEST_ASSERT(store.exists("k4"), "k4 remains intact");
    TEST_ASSERT(store.dbsize() == 1, "dbsize is 1");

    BioManager::instance().wait_empty(BioType::LazyFree);
    return true;
}

// ─── 5. Kiểm tra DataStore::unlink key đã hết hạn (TTL) ──────────────────────
bool test_datastore_unlink_expired_key() {
    DataStore store;
    store.set("temp", "temp_value");
    store.expiry().set_expiry("temp", std::chrono::milliseconds(1)); // 1 millisecond

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    bool unlinked = store.unlink("temp");
    TEST_ASSERT(!unlinked, "unlink expired key returns false");
    TEST_ASSERT(!store.exists("temp"), "key not in store");
    return true;
}

// ─── 6. Kiểm tra Dispatcher lệnh UNLINK qua RESP protocol ───────────────────
bool test_dispatcher_unlink_command() {
    DataStore store;
    Dispatcher dispatcher;
    std::string out;

    store.set("alpha", "1");
    store.set("beta", "2");

    // UNLINK alpha beta gamma -> returns :2\r\n
    std::vector<std::string> cmd = {"UNLINK", "alpha", "beta", "gamma"};
    dispatcher.dispatch(cmd, store, out);

    TEST_ASSERT(out == ":2\r\n", "UNLINK returns :2\\r\\n");
    TEST_ASSERT(!store.exists("alpha"), "alpha unlinked");
    TEST_ASSERT(!store.exists("beta"), "beta unlinked");

    out.clear();
    // UNLINK missing_key -> returns :0\r\n
    cmd = {"UNLINK", "missing_key"};
    dispatcher.dispatch(cmd, store, out);
    TEST_ASSERT(out == ":0\r\n", "UNLINK missing key returns :0\\r\\n");

    BioManager::instance().wait_empty(BioType::LazyFree);
    return true;
}

int main() {
    std::cout << "========================================\n";
    std::cout << " RUNNING MINI-REDIS BIO & UNLINK TESTS  \n";
    std::cout << "========================================\n";

    RUN_TEST(test_bio_async_execution);
    RUN_TEST(test_bio_multiple_jobs);
    RUN_TEST(test_datastore_unlink_single_key);
    RUN_TEST(test_datastore_unlink_multi_keys);
    RUN_TEST(test_datastore_unlink_expired_key);
    RUN_TEST(test_dispatcher_unlink_command);

    std::cout << "========================================\n";
    std::cout << " Summary: " << g_tests_passed << "/" << g_tests_run << " tests passed.\n";
    std::cout << "========================================\n";

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
