#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace mini_redis {

namespace {

std::string_view trim_sv(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string to_lower_str(std::string_view s) {
    std::string res;
    res.reserve(s.size());
    for (char c : s) {
        res.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return res;
}

}  // namespace

const std::set<std::string>& ConfigManager::all_supported_directives() {
    static const std::set<std::string> s_directives = {
        "bind",
        "port",
        "timeout",
        "loglevel",
        "max-buffer-size",
        "active-expire-interval-ms",
        "compact-threshold",
        "shrink-threshold"
    };
    return s_directives;
}

std::string ConfigManager::generate_default_template() {
    return R"(# ==============================================================================
# mini-redis configuration file
# ==============================================================================
#
# Note: You can customize this file to adjust the behavior of mini-redis.
# If this file is corrupted, mini-redis will automatically back it up to a .bak
# file and regenerate a fresh configuration. If directives are missing, they
# will automatically be appended with default values.

# ------------------------------------------------------------------------------
# 1. NETWORK & PORT
# ------------------------------------------------------------------------------
# By default, mini-redis listens for connections from 127.0.0.1.
# To listen on all network interfaces, use: bind 0.0.0.0
bind 127.0.0.1

# Accept connections on the specified port. Default is 6379 (Redis standard port).
port 6379

# ------------------------------------------------------------------------------
# 2. CLIENT CONNECTION & TIMEOUT
# ------------------------------------------------------------------------------
# Close client connection if idle for N seconds (0 to disable idle timeout).
# Default is 300 seconds (5 minutes).
timeout 300

# ------------------------------------------------------------------------------
# 3. LOGGING
# ------------------------------------------------------------------------------
# Specify the server verbosity level.
# Valid values: debug, info, warning, error
# Default: info
loglevel info

# ------------------------------------------------------------------------------
# 4. BUFFER & MEMORY MANAGEMENT
# ------------------------------------------------------------------------------
# Maximum buffer size allowed per client in bytes (protection against DoS).
# Default is 1048576 (1 Megabyte).
max-buffer-size 1048576

# Buffer compaction threshold in bytes. When unparsed offset drifts beyond this,
# pending bytes are shifted to the front. Default: 32768 (32KB).
compact-threshold 32768

# Buffer shrink threshold in bytes. When buffer capacity exceeds this and client
# is idle, RAM is released back to the operating system. Default: 65536 (64KB).
shrink-threshold 65536

# ------------------------------------------------------------------------------
# 5. ACTIVE EXPIRY & HEARTBEAT
# ------------------------------------------------------------------------------
# Heartbeat interval of the Linux timerfd in milliseconds for Active Expiry
# and idle timeout sweeps. Default: 100ms (10Hz).
active-expire-interval-ms 100
)";
}

bool ConfigManager::write_default_file(const std::string& filepath, std::string& out_err) {
    std::ofstream ofs(filepath, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) {
        out_err = "Cannot open file for writing: " + filepath;
        return false;
    }
    ofs << generate_default_template();
    if (!ofs.good()) {
        out_err = "Failed to write default config content to " + filepath;
        return false;
    }
    return true;
}

bool ConfigManager::parse_content(std::string_view content,
                                  ServerConfig& out_cfg,
                                  std::set<std::string>& out_seen_directives,
                                  std::string& out_err) {
    ServerConfig cfg;
    std::set<std::string> seen;

    size_t line_no = 0;
    size_t pos = 0;

    while (pos < content.size()) {
        line_no++;
        size_t next_newline = content.find('\n', pos);
        std::string_view line = (next_newline == std::string_view::npos)
                                    ? content.substr(pos)
                                    : content.substr(pos, next_newline - pos);
        pos = (next_newline == std::string_view::npos) ? content.size() : next_newline + 1;

        // Loại bỏ phần comment bắt đầu bằng '#' (kể cả inline comment)
        size_t hash_pos = line.find('#');
        if (hash_pos != std::string_view::npos) {
            line = line.substr(0, hash_pos);
        }

        line = trim_sv(line);
        if (line.empty()) {
            continue;
        }

        // Tách directive và value
        size_t sep_pos = line.find_first_of(" \t");
        if (sep_pos == std::string_view::npos) {
            out_err = "Line " + std::to_string(line_no) + ": Missing value for directive '" + std::string(line) + "'";
            return false;
        }

        std::string directive = to_lower_str(trim_sv(line.substr(0, sep_pos)));
        std::string_view val_sv = trim_sv(line.substr(sep_pos + 1));

        if (val_sv.empty()) {
            out_err = "Line " + std::to_string(line_no) + ": Missing value for directive '" + directive + "'";
            return false;
        }

        if (directive == "bind") {
            cfg.bind_address = std::string(val_sv);
            seen.insert("bind");
        } else if (directive == "port") {
            int p = 0;
            auto [ptr, ec] = std::from_chars(val_sv.data(), val_sv.data() + val_sv.size(), p);
            if (ec != std::errc{} || ptr != val_sv.data() + val_sv.size() || p <= 0 || p > 65535) {
                out_err = "Line " + std::to_string(line_no) + ": Invalid port '" + std::string(val_sv) + "' (must be 1-65535)";
                return false;
            }
            cfg.port = p;
            seen.insert("port");
        } else if (directive == "timeout") {
            int64_t t = 0;
            auto [ptr, ec] = std::from_chars(val_sv.data(), val_sv.data() + val_sv.size(), t);
            if (ec != std::errc{} || ptr != val_sv.data() + val_sv.size() || t < 0) {
                out_err = "Line " + std::to_string(line_no) + ": Invalid timeout '" + std::string(val_sv) + "' (must be >= 0)";
                return false;
            }
            cfg.client_idle_timeout = std::chrono::seconds(t);
            seen.insert("timeout");
        } else if (directive == "loglevel") {
            std::string level = to_lower_str(val_sv);
            if (level != "debug" && level != "info" && level != "warning" && level != "warn" && level != "error") {
                out_err = "Line " + std::to_string(line_no) + ": Invalid loglevel '" + std::string(val_sv) + "' (must be debug, info, warning, error)";
                return false;
            }
            cfg.loglevel = (level == "warn") ? "warning" : level;
            seen.insert("loglevel");
        } else if (directive == "max-buffer-size") {
            size_t sz = 0;
            auto [ptr, ec] = std::from_chars(val_sv.data(), val_sv.data() + val_sv.size(), sz);
            if (ec != std::errc{} || ptr != val_sv.data() + val_sv.size() || sz < 4096) {
                out_err = "Line " + std::to_string(line_no) + ": Invalid max-buffer-size '" + std::string(val_sv) + "' (must be >= 4096 bytes)";
                return false;
            }
            cfg.max_buffer_size = sz;
            seen.insert("max-buffer-size");
        } else if (directive == "active-expire-interval-ms") {
            int ms = 0;
            auto [ptr, ec] = std::from_chars(val_sv.data(), val_sv.data() + val_sv.size(), ms);
            if (ec != std::errc{} || ptr != val_sv.data() + val_sv.size() || ms < 10 || ms > 10000) {
                out_err = "Line " + std::to_string(line_no) + ": Invalid active-expire-interval-ms '" + std::string(val_sv) + "' (must be 10-10000 ms)";
                return false;
            }
            cfg.active_expire_interval_ms = ms;
            seen.insert("active-expire-interval-ms");
        } else if (directive == "compact-threshold") {
            size_t sz = 0;
            auto [ptr, ec] = std::from_chars(val_sv.data(), val_sv.data() + val_sv.size(), sz);
            if (ec != std::errc{} || ptr != val_sv.data() + val_sv.size() || sz < 1024) {
                out_err = "Line " + std::to_string(line_no) + ": Invalid compact-threshold '" + std::string(val_sv) + "' (must be >= 1024 bytes)";
                return false;
            }
            cfg.compact_threshold = sz;
            seen.insert("compact-threshold");
        } else if (directive == "shrink-threshold") {
            size_t sz = 0;
            auto [ptr, ec] = std::from_chars(val_sv.data(), val_sv.data() + val_sv.size(), sz);
            if (ec != std::errc{} || ptr != val_sv.data() + val_sv.size() || sz < 4096) {
                out_err = "Line " + std::to_string(line_no) + ": Invalid shrink-threshold '" + std::string(val_sv) + "' (must be >= 4096 bytes)";
                return false;
            }
            cfg.shrink_threshold = sz;
            seen.insert("shrink-threshold");
        } else {
            out_err = "Line " + std::to_string(line_no) + ": Unknown configuration directive '" + directive + "'";
            return false;
        }
    }

    out_cfg = cfg;
    out_seen_directives = std::move(seen);
    return true;
}

bool ConfigManager::append_missing_directives(const std::string& filepath,
                                             const std::set<std::string>& missing_directives,
                                             const ServerConfig& default_cfg,
                                             std::string& out_err) {
    std::ofstream ofs(filepath, std::ios::out | std::ios::app);
    if (!ofs.is_open()) {
        out_err = "Cannot open file for appending missing directives: " + filepath;
        return false;
    }

    ofs << "\n# ------------------------------------------------------------------------------\n";
    ofs << "# Automatically restored missing configuration directives\n";
    ofs << "# ------------------------------------------------------------------------------\n";

    for (const auto& d : missing_directives) {
        if (d == "bind") {
            ofs << "bind " << default_cfg.bind_address << "\n";
        } else if (d == "port") {
            ofs << "port " << default_cfg.port << "\n";
        } else if (d == "timeout") {
            ofs << "timeout " << default_cfg.client_idle_timeout.count() << "\n";
        } else if (d == "loglevel") {
            ofs << "loglevel " << default_cfg.loglevel << "\n";
        } else if (d == "max-buffer-size") {
            ofs << "max-buffer-size " << default_cfg.max_buffer_size << "\n";
        } else if (d == "active-expire-interval-ms") {
            ofs << "active-expire-interval-ms " << default_cfg.active_expire_interval_ms << "\n";
        } else if (d == "compact-threshold") {
            ofs << "compact-threshold " << default_cfg.compact_threshold << "\n";
        } else if (d == "shrink-threshold") {
            ofs << "shrink-threshold " << default_cfg.shrink_threshold << "\n";
        }
    }

    return true;
}

ServerConfig ConfigManager::load_and_manage(const std::string& filepath,
                                            ConfigStatus& out_status,
                                            std::string& out_msg) {
    std::error_code ec;
    bool exists = std::filesystem::exists(filepath, ec);

    // Kịch bản 1: Chưa có file -> Tạo file mặc định mới
    if (!exists) {
        std::string err;
        if (!write_default_file(filepath, err)) {
            out_status = ConfigStatus::CreatedDefault;
            out_msg = "Could not create default config file (" + err + "), using in-memory defaults";
            return ServerConfig{};
        }
        out_status = ConfigStatus::CreatedDefault;
        out_msg = "Config file not found. Generated default configuration file at '" + filepath + "'";
        return ServerConfig{};
    }

    // Đọc nội dung file
    std::ifstream ifs(filepath, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) {
        out_status = ConfigStatus::CreatedDefault;
        out_msg = "Failed to open '" + filepath + "' for reading, using in-memory defaults";
        return ServerConfig{};
    }

    std::ostringstream sstr;
    sstr << ifs.rdbuf();
    std::string content = sstr.str();
    ifs.close();

    ServerConfig parsed_cfg;
    std::set<std::string> seen;
    std::string parse_err;

    bool parse_ok = parse_content(content, parsed_cfg, seen, parse_err);

    if (parse_ok) {
        // Tìm các directive bị thiếu
        std::set<std::string> missing;
        const auto& all_directives = all_supported_directives();
        for (const auto& d : all_directives) {
            if (seen.find(d) == seen.end()) {
                missing.insert(d);
            }
        }

        // Kịch bản 3: Thiếu một số directive -> Tự động append bổ sung
        if (!missing.empty()) {
            std::string append_err;
            append_missing_directives(filepath, missing, ServerConfig{}, append_err);
            out_status = ConfigStatus::RepairedMissing;
            out_msg = "Loaded config and restored " + std::to_string(missing.size()) +
                      " missing directive(s) into '" + filepath + "'";
            return parsed_cfg;
        }

        // Kịch bản 2: Đầy đủ hợp lệ
        out_status = ConfigStatus::Ok;
        out_msg = "Successfully loaded configuration from '" + filepath + "'";
        return parsed_cfg;
    }

    // Kịch bản 4: File bị lỗi cú pháp / hỏng -> Backup sang .bak và tạo mới file chuẩn
    auto now_ns = std::chrono::system_clock::now().time_since_epoch().count();
    std::string backup_path = filepath + ".bak." + std::to_string(now_ns);

    std::error_code rename_ec;
    std::filesystem::rename(filepath, backup_path, rename_ec);

    std::string write_err;
    write_default_file(filepath, write_err);

    out_status = ConfigStatus::RecoveredFromCorrupted;
    out_msg = "Configuration file '" + filepath + "' was corrupted (" + parse_err +
              "). Backed up to '" + backup_path + "' and regenerated fresh default config";
    return ServerConfig{};
}

}  // namespace mini_redis
