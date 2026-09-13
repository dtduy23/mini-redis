#include "config/config.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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

static const std::string TEST_CONF_DIR = "test_conf_tmp";

void cleanup_test_dir() {
    std::error_code ec;
    std::filesystem::remove_all(TEST_CONF_DIR, ec);
}

// ─── 1. Kịch bản 1: Chạy lần đầu (file chưa có -> tự động sinh file) ─────────
bool test_config_first_run_generates_default() {
    std::string path = TEST_CONF_DIR + "/first_run.conf";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    ConfigStatus status;
    std::string msg;
    ServerConfig cfg = ConfigManager::load_and_manage(path, status, msg);

    TEST_ASSERT(status == ConfigStatus::CreatedDefault, "status is CreatedDefault");
    TEST_ASSERT(std::filesystem::exists(path), "default config file created on disk");

    // Kiểm tra các giá trị mặc định
    TEST_ASSERT(cfg.port == 6379, "default port is 6379");
    TEST_ASSERT(cfg.bind_address == "127.0.0.1", "default bind is 127.0.0.1");
    TEST_ASSERT(cfg.client_idle_timeout == std::chrono::seconds(300), "default timeout is 300s");
    TEST_ASSERT(cfg.loglevel == "info", "default loglevel is info");
    TEST_ASSERT(cfg.max_buffer_size == 1048576, "default max_buffer_size is 1MB");
    TEST_ASSERT(cfg.active_expire_interval_ms == 100, "default active_expire_interval_ms is 100ms");
    TEST_ASSERT(cfg.compact_threshold == 32768, "default compact_threshold is 32KB");
    TEST_ASSERT(cfg.shrink_threshold == 65536, "default shrink_threshold is 64KB");

    // Đọc lại file vừa sinh ra xem có nội dung hợp lệ không
    std::ifstream ifs(path);
    TEST_ASSERT(ifs.is_open(), "can open generated file");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    TEST_ASSERT(content.find("port 6379") != std::string::npos, "file contains 'port 6379'");
    TEST_ASSERT(content.find("bind 127.0.0.1") != std::string::npos, "file contains 'bind 127.0.0.1'");

    return true;
}

// ─── 2. Kịch bản 2: File đã có và hợp lệ (nạp cấu hình tùy chỉnh) ─────────────
bool test_config_valid_file_loads_custom() {
    std::string path = TEST_CONF_DIR + "/custom.conf";
    {
        std::ofstream ofs(path);
        ofs << "bind 0.0.0.0\n"
            << "port 9999\n"
            << "timeout 120\n"
            << "loglevel debug\n"
            << "max-buffer-size 2097152\n"
            << "active-expire-interval-ms 50\n"
            << "compact-threshold 16384\n"
            << "shrink-threshold 32768\n";
    }

    ConfigStatus status;
    std::string msg;
    ServerConfig cfg = ConfigManager::load_and_manage(path, status, msg);

    TEST_ASSERT(status == ConfigStatus::Ok, "status is Ok");
    TEST_ASSERT(cfg.bind_address == "0.0.0.0", "custom bind 0.0.0.0");
    TEST_ASSERT(cfg.port == 9999, "custom port 9999");
    TEST_ASSERT(cfg.client_idle_timeout == std::chrono::seconds(120), "custom timeout 120s");
    TEST_ASSERT(cfg.loglevel == "debug", "custom loglevel debug");
    TEST_ASSERT(cfg.max_buffer_size == 2097152, "custom max_buffer_size 2MB");
    TEST_ASSERT(cfg.active_expire_interval_ms == 50, "custom active_expire_interval_ms 50ms");
    TEST_ASSERT(cfg.compact_threshold == 16384, "custom compact_threshold 16KB");
    TEST_ASSERT(cfg.shrink_threshold == 32768, "custom shrink_threshold 32KB");

    return true;
}

// ─── 3. Kịch bản 3: File bị thiếu dòng (Self-Healing tự động append bổ sung) ──
bool test_config_missing_lines_auto_appends() {
    std::string path = TEST_CONF_DIR + "/missing.conf";
    {
        std::ofstream ofs(path);
        // Chỉ ghi 2 dòng, thiếu 6 directive còn lại
        ofs << "port 8080\n"
            << "loglevel warning\n";
    }

    ConfigStatus status;
    std::string msg;
    ServerConfig cfg = ConfigManager::load_and_manage(path, status, msg);

    TEST_ASSERT(status == ConfigStatus::RepairedMissing, "status is RepairedMissing");
    TEST_ASSERT(cfg.port == 8080, "specified port preserved (8080)");
    TEST_ASSERT(cfg.loglevel == "warning", "specified loglevel preserved (warning)");

    // Các trường bị thiếu phải tự động nhận giá trị mặc định
    TEST_ASSERT(cfg.bind_address == "127.0.0.1", "missing bind uses default");
    TEST_ASSERT(cfg.client_idle_timeout == std::chrono::seconds(300), "missing timeout uses default");

    // Kiểm tra file trên đĩa đã được append các directive bị thiếu
    std::ifstream ifs(path);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    TEST_ASSERT(content.find("bind 127.0.0.1") != std::string::npos, "file was appended with bind");
    TEST_ASSERT(content.find("timeout 300") != std::string::npos, "file was appended with timeout");
    TEST_ASSERT(content.find("max-buffer-size 1048576") != std::string::npos, "file was appended with max-buffer-size");

    // Nạp lại lần 2: lúc này file đã đầy đủ -> status phải là Ok
    ConfigStatus status2;
    std::string msg2;
    ServerConfig cfg2 = ConfigManager::load_and_manage(path, status2, msg2);
    TEST_ASSERT(status2 == ConfigStatus::Ok, "second load is Ok after self-healing");
    TEST_ASSERT(cfg2.port == 8080, "port still 8080");

    return true;
}

// ─── 4. Kịch bản 4: File bị lỗi cú pháp / hỏng -> backup và tạo file mới ──────
bool test_config_corrupted_file_backs_up_and_regenerates() {
    std::string path = TEST_CONF_DIR + "/corrupted.conf";
    {
        std::ofstream ofs(path);
        ofs << "port NOT_A_NUMBER\n"
            << "garbage_key_invalid @#$%^&*\n";
    }

    ConfigStatus status;
    std::string msg;
    ServerConfig cfg = ConfigManager::load_and_manage(path, status, msg);

    TEST_ASSERT(status == ConfigStatus::RecoveredFromCorrupted, "status is RecoveredFromCorrupted");
    TEST_ASSERT(cfg.port == 6379, "recovered config uses default port");

    // File chính phải được tạo lại mới chuẩn
    TEST_ASSERT(std::filesystem::exists(path), "fresh config recreated");

    // Phải có ít nhất 1 file backup .bak
    bool found_bak = false;
    for (const auto& entry : std::filesystem::directory_iterator(TEST_CONF_DIR)) {
        std::string filename = entry.path().filename().string();
        if (filename.find("corrupted.conf.bak.") != std::string::npos) {
            found_bak = true;
            // Kiểm tra file backup lưu giữ nội dung hỏng cũ
            std::ifstream bak_fs(entry.path());
            std::string bak_content((std::istreambuf_iterator<char>(bak_fs)), std::istreambuf_iterator<char>());
            TEST_ASSERT(bak_content.find("port NOT_A_NUMBER") != std::string::npos, "backup preserved original corrupted data");
            break;
        }
    }
    TEST_ASSERT(found_bak, "backup file .bak exists on disk");

    return true;
}

// ─── 5. Kiểm tra xử lý Comment #, dòng trống, thụt lề tab/space ───────────────
bool test_config_comments_and_whitespace() {
    std::string content =
        "# Header comment\n"
        "   \n"
        "   # Indented comment\n"
        "bind    \t 192.168.1.100   \n"
        "\n"
        "PORT    6380   # uppercase directive\n";

    // Port 6380
    ServerConfig cfg;
    std::set<std::string> seen;
    std::string err;

    std::string full_valid_content =
        content +
        "timeout 0\n"
        "loglevel info\n"
        "max-buffer-size 1048576\n"
        "active-expire-interval-ms 100\n"
        "compact-threshold 32768\n"
        "shrink-threshold 64128\n";

    bool ok = ConfigManager::parse_content(full_valid_content, cfg, seen, err);
    TEST_ASSERT(ok, "parse with whitespace and comments succeeds");
    TEST_ASSERT(cfg.bind_address == "192.168.1.100", "bind trimmed");
    TEST_ASSERT(cfg.port == 6380, "uppercase PORT parsed");
    TEST_ASSERT(cfg.client_idle_timeout == std::chrono::seconds(0), "timeout 0 supported");

    return true;
}

// ─── 6. Kiểm tra các lỗi tham số không hợp lệ ─────────────────────────────────
bool test_config_invalid_values_rejected() {
    ServerConfig cfg;
    std::set<std::string> seen;
    std::string err;

    // Port = 0 -> lỗi
    TEST_ASSERT(!ConfigManager::parse_content("port 0\n", cfg, seen, err), "port 0 rejected");

    // Port > 65535 -> lỗi
    TEST_ASSERT(!ConfigManager::parse_content("port 70000\n", cfg, seen, err), "port 70000 rejected");

    // Timeout < 0 -> lỗi
    TEST_ASSERT(!ConfigManager::parse_content("timeout -10\n", cfg, seen, err), "negative timeout rejected");

    // Loglevel lạ -> lỗi
    TEST_ASSERT(!ConfigManager::parse_content("loglevel super_verbose\n", cfg, seen, err), "invalid loglevel rejected");

    // Directive lạ -> lỗi
    TEST_ASSERT(!ConfigManager::parse_content("unknown_directive 123\n", cfg, seen, err), "unknown directive rejected");

    return true;
}

int main() {
    std::filesystem::create_directories(TEST_CONF_DIR);

    std::cout << "========================================\n";
    std::cout << "     RUNNING CONFIG MANAGER TESTS       \n";
    std::cout << "========================================\n";

    RUN_TEST(test_config_first_run_generates_default);
    RUN_TEST(test_config_valid_file_loads_custom);
    RUN_TEST(test_config_missing_lines_auto_appends);
    RUN_TEST(test_config_corrupted_file_backs_up_and_regenerates);
    RUN_TEST(test_config_comments_and_whitespace);
    RUN_TEST(test_config_invalid_values_rejected);

    cleanup_test_dir();

    std::cout << "========================================\n";
    std::cout << "Results: " << g_tests_passed << "/" << g_tests_run << " tests passed.\n";
    std::cout << "========================================\n";

    return (g_tests_passed == g_tests_run) ? 0 : 1;
}

