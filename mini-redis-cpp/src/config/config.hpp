#pragma once

#include <chrono>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>

namespace mini_redis {

struct ServerConfig {
    std::string           bind_address{"127.0.0.1"};
    int                   port{6379};
    std::chrono::seconds  client_idle_timeout{300}; // 300s mặc định, 0 để tắt
    std::string           loglevel{"info"};          // debug, info, warning, error
    size_t                max_buffer_size{1024 * 1024}; // 1MB buffer tối đa mỗi client
    int                   active_expire_interval_ms{100}; // chu kỳ quét timerfd (100ms / 10Hz)
    size_t                compact_threshold{32 * 1024}; // 32KB
    size_t                shrink_threshold{64 * 1024};  // 64KB
};

enum class ConfigStatus {
    Ok,                      // File có sẵn, đọc thành công đầy đủ
    CreatedDefault,          // File chưa có, tự động tạo mới file mặc định
    RepairedMissing,         // File có sẵn nhưng thiếu dòng, đã tự động append bổ sung
    RecoveredFromCorrupted   // File bị lỗi/hỏng, đã backup sang .bak và tạo lại file mới
};

class ConfigManager {
public:
    ConfigManager() = delete;

    // Nạp cấu hình từ file với cơ chế tự phục hồi (Self-Healing)
    static ServerConfig load_and_manage(const std::string& filepath,
                                        ConfigStatus& out_status,
                                        std::string& out_msg);

    // Parse nội dung chuỗi cấu hình
    static bool parse_content(std::string_view content,
                              ServerConfig& out_cfg,
                              std::set<std::string>& out_seen_directives,
                              std::string& out_err);

    // Ghi file cấu hình mặc định có đầy đủ chú thích
    static bool write_default_file(const std::string& filepath, std::string& out_err);

    // Ghi bổ sung các directive bị thiếu vào cuối file
    static bool append_missing_directives(const std::string& filepath,
                                          const std::set<std::string>& missing_directives,
                                          const ServerConfig& default_cfg,
                                          std::string& out_err);

    // Tạo nội dung mẫu mặc định (self-documenting template)
    static std::string generate_default_template();

    // Danh sách tất cả các directive hợp lệ
    static const std::set<std::string>& all_supported_directives();
};

}  // namespace mini_redis

