#include "commands/dispatcher.hpp"
#include "protocol/resp_serializer.hpp"
#include "store/data_store.hpp"
#include "store/rdb.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

static const std::string TEST_RDB_DIR = "test_rdb_tmp";

void cleanup_rdb_dir() {
    std::error_code ec;
    std::filesystem::remove_all(TEST_RDB_DIR, ec);
    std::filesystem::remove("dump.rdb", ec);
}

// ─── 1. Save and Load Roundtrip ──────────────────────────────────────────────
bool test_rdb_save_and_load_roundtrip() {
    std::filesystem::create_directories(TEST_RDB_DIR);
    std::string path = TEST_RDB_DIR + "/roundtrip.rdb";

    DataStore store1;
    store1.set("name", "antigravity");
    store1.set("version", "2.0");
    store1.set("count", "100");

    std::string err;
    bool ok = RdbManager::save(path, store1, err);
    TEST_ASSERT(ok, "save failed: " + err);
    TEST_ASSERT(std::filesystem::exists(path), "RDB file exists on disk");

    DataStore store2;
    size_t loaded = 0;
    ok = RdbManager::load(path, store2, loaded, err);
    TEST_ASSERT(ok, "load failed: " + err);
    TEST_ASSERT(loaded == 3, "loaded count is 3");

    const std::string* v1 = store2.get("name");
    const std::string* v2 = store2.get("version");
    const std::string* v3 = store2.get("count");

    TEST_ASSERT(v1 && *v1 == "antigravity", "key 'name' restored correctly");
    TEST_ASSERT(v2 && *v2 == "2.0", "key 'version' restored correctly");
    TEST_ASSERT(v3 && *v3 == "100", "key 'count' restored correctly");

    return true;
}

// ─── 2. TTL Preservation for Non-Expired Keys ─────────────────────────────────
bool test_rdb_ttl_preservation() {
    std::filesystem::create_directories(TEST_RDB_DIR);
    std::string path = TEST_RDB_DIR + "/ttl_preserve.rdb";

    DataStore store1;
    store1.set("temp_key", "active_data");
    store1.expiry().set_expiry("temp_key", std::chrono::seconds(120));

    std::string err;
    bool ok = RdbManager::save(path, store1, err);
    TEST_ASSERT(ok, "save with TTL failed: " + err);

    DataStore store2;
    size_t loaded = 0;
    ok = RdbManager::load(path, store2, loaded, err);
    TEST_ASSERT(ok, "load with TTL failed: " + err);
    TEST_ASSERT(loaded == 1, "loaded 1 key with TTL");

    int64_t ttl = store2.expiry().get_ttl_seconds("temp_key", store2.exists("temp_key"));
    TEST_ASSERT(ttl > 100 && ttl <= 120, "TTL preserved in future range (100-120s)");

    return true;
}

// ─── 3. Discard Expired Keys during Load ──────────────────────────────────────
bool test_rdb_expired_keys_discarded() {
    std::filesystem::create_directories(TEST_RDB_DIR);
    std::string path = TEST_RDB_DIR + "/expired_discard.rdb";

    DataStore store1;
    // Khôi phục một key với mốc thời gian quá khứ (1000ms epoch)
    store1.restore_key("stale_key", "old_data", 1000);
    store1.set("fresh_key", "fresh_data");

    std::string err;
    bool ok = RdbManager::save(path, store1, err);
    TEST_ASSERT(ok, "save with stale key failed: " + err);

    DataStore store2;
    size_t loaded = 0;
    ok = RdbManager::load(path, store2, loaded, err);
    TEST_ASSERT(ok, "load failed: " + err);
    TEST_ASSERT(loaded == 1, "only fresh_key loaded (stale discarded)");

    TEST_ASSERT(!store2.exists("stale_key"), "stale_key does not exist");
    TEST_ASSERT(store2.exists("fresh_key"), "fresh_key exists");

    return true;
}

// ─── 4. Corrupted File - Bad Magic Header ─────────────────────────────────────
bool test_rdb_corrupted_magic() {
    std::filesystem::create_directories(TEST_RDB_DIR);
    std::string path = TEST_RDB_DIR + "/bad_magic.rdb";

    {
        std::ofstream ofs(path, std::ios::binary);
        ofs << "INVALID01";
        ofs.put('\xFF');
        uint64_t fake_cksum = 0;
        ofs.write(reinterpret_cast<const char*>(&fake_cksum), sizeof(fake_cksum));
    }

    DataStore store;
    size_t loaded = 0;
    std::string err;
    bool ok = RdbManager::load(path, store, loaded, err);
    TEST_ASSERT(!ok, "load should fail on invalid magic");
    TEST_ASSERT(err.find("magic header") != std::string::npos, "error reports bad magic");

    return true;
}

// ─── 5. Corrupted File - Checksum Mismatch ────────────────────────────────────
bool test_rdb_corrupted_checksum() {
    std::filesystem::create_directories(TEST_RDB_DIR);
    std::string path = TEST_RDB_DIR + "/bad_checksum.rdb";

    DataStore store1;
    store1.set("foo", "bar");
    std::string err;
    bool ok = RdbManager::save(path, store1, err);
    TEST_ASSERT(ok, "save succeeded");

    // Thay đổi 1 byte ở cuối file (checksum)
    {
        std::fstream fs(path, std::ios::in | std::ios::out | std::ios::binary);
        fs.seekp(-1, std::ios::end);
        char bad = 0x5A;
        fs.write(&bad, 1);
    }

    DataStore store2;
    size_t loaded = 0;
    ok = RdbManager::load(path, store2, loaded, err);
    TEST_ASSERT(!ok, "load should fail on checksum mismatch");
    TEST_ASSERT(err.find("checksum") != std::string::npos, "error reports checksum mismatch");

    return true;
}

// ─── 6. BGSAVE Tracking ───────────────────────────────────────────────────────
bool test_rdb_bgsave_state_tracking() {
    TEST_ASSERT(!RdbManager::is_bgsave_running(), "initially bgsave not running");
    TEST_ASSERT(RdbManager::bgsave_pid() == -1, "bgsave_pid is -1");

    RdbManager::set_bgsave_pid(4242);
    TEST_ASSERT(RdbManager::is_bgsave_running(), "bgsave running after set");
    TEST_ASSERT(RdbManager::bgsave_pid() == 4242, "bgsave_pid is 4242");

    RdbManager::reset_bgsave_pid();
    TEST_ASSERT(!RdbManager::is_bgsave_running(), "bgsave not running after reset");
    TEST_ASSERT(RdbManager::bgsave_pid() == -1, "bgsave_pid is -1 after reset");

    return true;
}

// ─── 7. SAVE and COMMAND via Dispatcher ───────────────────────────────────────
bool test_rdb_dispatcher_commands() {
    Dispatcher dispatcher;
    DataStore store;
    store.set("key1", "val1");

    // SAVE command
    std::string out;
    dispatcher.dispatch({"SAVE"}, store, out);
    TEST_ASSERT(out == "+OK\r\n", "SAVE returns +OK");
    TEST_ASSERT(std::filesystem::exists("dump.rdb"), "dump.rdb created in cwd");

    // COMMAND command (compatible with redis-cli)
    out.clear();
    dispatcher.dispatch({"COMMAND"}, store, out);
    TEST_ASSERT(out == "*0\r\n", "COMMAND returns empty array *0");

    // COMMAND DOCS
    out.clear();
    dispatcher.dispatch({"COMMAND", "DOCS"}, store, out);
    TEST_ASSERT(out == "*0\r\n", "COMMAND DOCS returns empty array *0");

    // COMMAND COUNT
    out.clear();
    dispatcher.dispatch({"COMMAND", "COUNT"}, store, out);
    TEST_ASSERT(out == ":16\r\n", "COMMAND COUNT returns integer count 16");

    return true;
}

int main() {
    cleanup_rdb_dir();

    std::cout << "========================================\n";
    std::cout << "  RUNNING RDB & COMMAND UNIT TESTS\n";
    std::cout << "========================================\n";

    RUN_TEST(test_rdb_save_and_load_roundtrip);
    RUN_TEST(test_rdb_ttl_preservation);
    RUN_TEST(test_rdb_expired_keys_discarded);
    RUN_TEST(test_rdb_corrupted_magic);
    RUN_TEST(test_rdb_corrupted_checksum);
    RUN_TEST(test_rdb_bgsave_state_tracking);
    RUN_TEST(test_rdb_dispatcher_commands);

    cleanup_rdb_dir();

    std::cout << "========================================\n";
    std::cout << "Results: " << g_tests_passed << "/" << g_tests_run << " passed.\n";
    std::cout << "========================================\n";

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}

