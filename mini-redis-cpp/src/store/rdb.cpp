#include "rdb.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace mini_redis {

namespace {

// Hàm tính checksum FNV-1a 64-bit đơn giản và hiệu quả
uint64_t fnv1a_64(const void* data, size_t len, uint64_t hash = 14695981039346656037ULL) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

}  // namespace

bool RdbManager::is_bgsave_running() noexcept {
    return s_bgsave_pid.load() > 0;
}

pid_t RdbManager::bgsave_pid() noexcept {
    return s_bgsave_pid.load();
}

void RdbManager::set_bgsave_pid(pid_t pid) noexcept {
    s_bgsave_pid.store(pid);
}

void RdbManager::reset_bgsave_pid() noexcept {
    s_bgsave_pid.store(-1);
}

bool RdbManager::save(const std::string& filepath, const DataStore& store, std::string& out_err) {
    std::string tmp_path = filepath + ".tmp." + std::to_string(::getpid());
    std::ofstream ofs(tmp_path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        out_err = "Cannot open temp file for writing RDB: " + tmp_path;
        return false;
    }

    uint64_t checksum = 14695981039346656037ULL;

    auto write_bytes = [&](const void* data, size_t len) -> bool {
        ofs.write(static_cast<const char*>(data), len);
        if (!ofs.good()) return false;
        checksum = fnv1a_64(data, len, checksum);
        return true;
    };

    // 1. Ghi Magic Header
    if (!write_bytes(RDB_MAGIC.data(), RDB_MAGIC.size())) {
        out_err = "Failed to write RDB header";
        ofs.close();
        ::unlink(tmp_path.c_str());
        return false;
    }

    // 2. Duyệt ghi từng Key-Value trong DataStore
    for (const auto& [key, val] : store.raw_store()) {
        // Bỏ qua nếu key đã hết hạn
        if (store.expiry().is_expired(key)) {
            continue;
        }

        uint64_t expire_epoch_ms = store.expiry().get_expire_epoch_ms(key);
        if (expire_epoch_ms > 0) {
            uint8_t op_expire = OPCODE_EXPIRETIME_MS;
            if (!write_bytes(&op_expire, sizeof(op_expire)) ||
                !write_bytes(&expire_epoch_ms, sizeof(expire_epoch_ms))) {
                out_err = "Failed to write expire metadata";
                ofs.close();
                ::unlink(tmp_path.c_str());
                return false;
            }
        }

        uint8_t op_type = OPCODE_STRING;
        uint32_t k_len = static_cast<uint32_t>(key.size());
        uint32_t v_len = static_cast<uint32_t>(val.size());

        if (!write_bytes(&op_type, sizeof(op_type)) ||
            !write_bytes(&k_len, sizeof(k_len)) ||
            !write_bytes(key.data(), key.size()) ||
            !write_bytes(&v_len, sizeof(v_len)) ||
            !write_bytes(val.data(), val.size())) {
            out_err = "Failed to write key-value entry";
            ofs.close();
            ::unlink(tmp_path.c_str());
            return false;
        }
    }

    // 3. Ghi EOF opcode và Checksum
    uint8_t op_eof = OPCODE_EOF;
    if (!write_bytes(&op_eof, sizeof(op_eof)) ||
        !write_bytes(&checksum, sizeof(checksum))) {
        out_err = "Failed to write RDB EOF/checksum";
        ofs.close();
        ::unlink(tmp_path.c_str());
        return false;
    }

    ofs.flush();
    if (!ofs.good()) {
        out_err = "Failed to flush RDB to disk";
        ofs.close();
        ::unlink(tmp_path.c_str());
        return false;
    }
    ofs.close();

    // 4. Atomic Rename file tạm sang file chính thức
    std::error_code ec;
    std::filesystem::rename(tmp_path, filepath, ec);
    if (ec) {
        out_err = "Failed to atomic rename '" + tmp_path + "' to '" + filepath + "': " + ec.message();
        ::unlink(tmp_path.c_str());
        return false;
    }

    return true;
}

bool RdbManager::load(const std::string& filepath, DataStore& store, size_t& out_loaded, std::string& out_err) {
    out_loaded = 0;
    std::error_code ec;
    if (!std::filesystem::exists(filepath, ec)) {
        out_err = "RDB file not found: " + filepath;
        return false;
    }

    std::ifstream ifs(filepath, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) {
        out_err = "Cannot open RDB file for reading: " + filepath;
        return false;
    }

    uint64_t computed_checksum = 14695981039346656037ULL;
    auto read_exact = [&](void* dest, size_t len, bool update_ck = true) -> bool {
        ifs.read(static_cast<char*>(dest), len);
        bool ok = ifs.good() || (ifs.eof() && ifs.gcount() == static_cast<std::streamsize>(len));
        if (ok && update_ck) {
            computed_checksum = fnv1a_64(dest, len, computed_checksum);
        }
        return ok;
    };

    // 1. Kiểm tra Magic Header
    char magic_buf[9] = {};
    if (!read_exact(magic_buf, 9) || std::string_view(magic_buf, 9) != RDB_MAGIC) {
        out_err = "Invalid RDB magic header (expected " + std::string(RDB_MAGIC) + ")";
        return false;
    }

    bool saw_eof = false;

    // 2. Vòng lặp đọc các bản ghi
    while (true) {
        uint8_t opcode = 0;
        if (!read_exact(&opcode, 1)) {
            break; // Hết file
        }

        if (opcode == OPCODE_EOF) {
            // Đọc 8 bytes checksum còn lại
            uint64_t file_checksum = 0;
            if (!read_exact(&file_checksum, sizeof(file_checksum), false)) {
                out_err = "Unexpected EOF reading RDB checksum";
                return false;
            }
            if (file_checksum != computed_checksum) {
                out_err = "RDB checksum mismatch";
                return false;
            }
            saw_eof = true;
            break;
        }

        uint64_t expire_epoch_ms = 0;
        if (opcode == OPCODE_EXPIRETIME_MS) {
            if (!read_exact(&expire_epoch_ms, sizeof(expire_epoch_ms))) {
                out_err = "Unexpected EOF while reading expire timestamp";
                return false;
            }
            if (!read_exact(&opcode, 1)) {
                out_err = "Unexpected EOF after expire opcode";
                return false;
            }
        }

        if (opcode != OPCODE_STRING) {
            out_err = "Unsupported RDB opcode: " + std::to_string(static_cast<int>(opcode));
            return false;
        }

        uint32_t k_len = 0;
        if (!read_exact(&k_len, sizeof(k_len))) {
            out_err = "Unexpected EOF reading key length";
            return false;
        }

        std::string key(k_len, '\0');
        if (!read_exact(key.data(), k_len)) {
            out_err = "Unexpected EOF reading key data";
            return false;
        }

        uint32_t v_len = 0;
        if (!read_exact(&v_len, sizeof(v_len))) {
            out_err = "Unexpected EOF reading value length";
            return false;
        }

        std::string val(v_len, '\0');
        if (!read_exact(val.data(), v_len)) {
            out_err = "Unexpected EOF reading value data";
            return false;
        }

        store.restore_key(std::move(key), std::move(val), expire_epoch_ms);
        out_loaded++;
    }

    if (!saw_eof) {
        out_err = "Unexpected EOF without RDB EOF opcode";
        return false;
    }

    return true;
}

}  // namespace mini_redis
