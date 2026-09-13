#include "expiry.hpp"
#include "data_store.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>
#include <vector>

namespace mini_redis {

void ExpiryManager::set_expiry(std::string key, std::chrono::milliseconds ttl, TimePoint now) {
    expires_.insert_or_assign(std::move(key), now + ttl);
}

bool ExpiryManager::is_expired(std::string_view key, TimePoint now) const {
    auto it = expires_.find(key);
    if (it == expires_.end()) {
        return false;
    }
    return now >= it->second;
}

int64_t ExpiryManager::get_ttl_seconds(std::string_view key, bool key_exists, TimePoint now) const {
    if (!key_exists) {
        return -2;
    }

    auto it = expires_.find(key);
    if (it == expires_.end()) {
        return -1;
    }

    if (now >= it->second) {
        return -2;
    }

    auto rem_ms = std::chrono::duration_cast<std::chrono::milliseconds>(it->second - now).count();
    // Làm tròn trần (ceiling) để số giây còn lại luôn >= 1 nếu chưa hết hạn
    int64_t rem_sec = (rem_ms + 999) / 1000;
    return rem_sec > 0 ? rem_sec : 1;
}

uint64_t ExpiryManager::get_expire_epoch_ms(std::string_view key, TimePoint now, std::chrono::system_clock::time_point sys_now) const {
    auto it = expires_.find(key);
    if (it == expires_.end()) {
        return 0;
    }
    if (now >= it->second) {
        return 0;
    }
    auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(it->second - now);
    auto sys_expire = sys_now + rem;
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(sys_expire.time_since_epoch()).count());
}

bool ExpiryManager::persist(std::string_view key) {
    auto it = expires_.find(key);
    if (it != expires_.end()) {
        expires_.erase(it);
        return true;
    }
    return false;
}

bool ExpiryManager::remove(std::string_view key) {
    auto it = expires_.find(key);
    if (it != expires_.end()) {
        expires_.erase(it);
        return true;
    }
    return false;
}

void ExpiryManager::clear() {
    expires_.clear();
}

size_t ExpiryManager::size() const {
    return expires_.size();
}

size_t ExpiryManager::active_expire_cycle(DataStore& store,
                                         size_t sample_size,
                                         double threshold,
                                         size_t max_duration_ms) {
    if (expires_.empty() || sample_size == 0) {
        return 0;
    }

    size_t total_deleted = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto max_duration = std::chrono::milliseconds(max_duration_ms);

    while (!expires_.empty()) {
        size_t to_sample = std::min(sample_size, expires_.size());
        auto now = std::chrono::steady_clock::now();

        std::vector<std::string> keys_to_delete;
        keys_to_delete.reserve(to_sample);

        size_t num_buckets = expires_.bucket_count();
        size_t start_bucket = num_buckets > 0 ? (static_cast<size_t>(std::rand()) % num_buckets) : 0;
        size_t checked = 0;

        for (size_t b_offset = 0; b_offset < num_buckets && checked < to_sample; ++b_offset) {
            size_t b = (start_bucket + b_offset) % num_buckets;
            for (auto it = expires_.begin(b); it != expires_.end(b) && checked < to_sample; ++it) {
                checked++;
                if (now >= it->second) {
                    keys_to_delete.push_back(it->first);
                }
            }
        }

        if (checked == 0) {
            break;
        }

        size_t expired_count = 0;
        for (const auto& k : keys_to_delete) {
            expires_.erase(k);
            store.del_raw(k);
            expired_count++;
            total_deleted++;
        }

        // Nếu tỷ lệ quá hạn <= threshold (mặc định 25%), dừng quét
        if ((static_cast<double>(expired_count) / static_cast<double>(checked)) <= threshold) {
            break;
        }

        // Giới hạn trần thời gian chạy (mặc định 25ms)
        if (std::chrono::steady_clock::now() - start_time >= max_duration) {
            break;
        }
    }

    return total_deleted;
}

}  // namespace mini_redis
