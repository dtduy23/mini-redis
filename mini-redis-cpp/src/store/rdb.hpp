#pragma once

#include "data_store.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace mini_redis {

class RdbManager {
public:
    static constexpr std::string_view RDB_MAGIC = "REDIS0009";
    static constexpr uint8_t OPCODE_EXPIRETIME_MS = 0xFC;
    static constexpr uint8_t OPCODE_STRING        = 0x00;
    static constexpr uint8_t OPCODE_EOF           = 0xFF;

    RdbManager() = delete;

    // Lưu snapshot đồng bộ ra file RDB (ghi file tạm trước rồi atomic rename)
    static bool save(const std::string& filepath, const DataStore& store, std::string& out_err);

    // Nạp dữ liệu từ file RDB vào DataStore (bỏ qua key đã hết hạn)
    static bool load(const std::string& filepath, DataStore& store, size_t& out_loaded, std::string& out_err);

    // Quản lý trạng thái Background Save (BGSAVE fork)
    static bool is_bgsave_running() noexcept;
    static pid_t bgsave_pid() noexcept;
    static void set_bgsave_pid(pid_t pid) noexcept;
    static void reset_bgsave_pid() noexcept;

private:
    inline static std::atomic<pid_t> s_bgsave_pid{-1};
};

}  // namespace mini_redis

