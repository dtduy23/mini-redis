#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mini_redis {

class DataStore;

class ExpiryManager {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    ExpiryManager() = default;

    // Đặt hạn sử dụng cho key (tính bằng milliseconds kể từ now)
    void set_expiry(std::string key, std::chrono::milliseconds ttl, TimePoint now = std::chrono::steady_clock::now());

    // Kiểm tra key đã hết hạn chưa tại thời điểm now
    bool is_expired(std::string_view key, TimePoint now = std::chrono::steady_clock::now()) const;

    // Lấy thời gian sống còn lại tính bằng giây (chuẩn Redis TTL):
    // - Trả về -2 nếu key không tồn tại hoặc đã hết hạn
    // - Trả về -1 nếu key tồn tại nhưng không có TTL
    // - Trả về số giây còn lại (>= 0) nếu có TTL
    int64_t get_ttl_seconds(std::string_view key, bool key_exists, TimePoint now = std::chrono::steady_clock::now()) const;

    // Lấy thời điểm hết hạn tính bằng epoch ms (Unix timestamp). Trả về 0 nếu không có TTL hoặc đã hết hạn
    uint64_t get_expire_epoch_ms(std::string_view key,
                                 TimePoint now = std::chrono::steady_clock::now(),
                                 std::chrono::system_clock::time_point sys_now = std::chrono::system_clock::now()) const;

    // Xóa TTL của key (lệnh PERSIST): trả về true nếu xóa thành công, false nếu key không có TTL
    bool persist(std::string_view key);

    // Xóa theo dõi TTL của key (dùng khi key bị xóa bởi DEL hoặc ghi đè bởi SET)
    bool remove(std::string_view key);

    // Xóa toàn bộ TTL (FLUSHALL)
    void clear();

    // Số lượng key đang có TTL
    size_t size() const;

    // Quét chủ động (Active Expiry): lấy mẫu tối đa sample_size keys,
    // xóa các key đã quá hạn khỏi DataStore và ExpiryManager.
    // Nếu tỷ lệ quá hạn > threshold (25%), lặp lại cho đến khi <= 25% hoặc chạm max_duration_ms.
    size_t active_expire_cycle(DataStore& store,
                               size_t sample_size = 20,
                               double threshold = 0.25,
                               size_t max_duration_ms = 25);

private:
    struct StringHash {
        using is_transparent = void;
        size_t operator()(std::string_view sv) const noexcept {
            return std::hash<std::string_view>{}(sv);
        }
        size_t operator()(const std::string& s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };

    std::unordered_map<std::string, TimePoint, StringHash, std::equal_to<>> expires_;
};

using Expiry = ExpiryManager;

}  // namespace mini_redis
