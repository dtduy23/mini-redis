#include "data_store.hpp"

#include <charconv>
#include <limits>
#include <utility>

namespace mini_redis {

void DataStore::set(std::string key, std::string value) {
    expiry_.remove(key);
    store_.insert_or_assign(std::move(key), std::move(value));
}

bool DataStore::del_raw(std::string_view key) {
    auto it = store_.find(key);
    if (it != store_.end()) {
        store_.erase(it);
        return true;
    }
    return false;
}

const std::string* DataStore::get(std::string_view key) {
    if (expiry_.is_expired(key)) {
        del_raw(key);
        expiry_.remove(key);
        return nullptr;
    }
    auto it = store_.find(key);
    if (it != store_.end()) {
        return &it->second;
    }
    return nullptr;
}

const std::string* DataStore::get(std::string_view key) const {
    if (expiry_.is_expired(key)) {
        return nullptr;
    }
    auto it = store_.find(key);
    if (it != store_.end()) {
        return &it->second;
    }
    return nullptr;
}

bool DataStore::del(std::string_view key) {
    bool expired = expiry_.is_expired(key);
    expiry_.remove(key);
    bool erased = del_raw(key);
    return !expired && erased;
}

int64_t DataStore::del(std::span<const std::string> keys) {
    int64_t count = 0;
    for (const auto& key : keys) {
        if (del(key)) {
            count++;
        }
    }
    return count;
}

bool DataStore::exists(std::string_view key) {
    if (expiry_.is_expired(key)) {
        del_raw(key);
        expiry_.remove(key);
        return false;
    }
    return store_.find(key) != store_.end();
}

bool DataStore::exists(std::string_view key) const {
    if (expiry_.is_expired(key)) {
        return false;
    }
    return store_.find(key) != store_.end();
}

int64_t DataStore::exists(std::span<const std::string> keys) {
    int64_t count = 0;
    for (const auto& key : keys) {
        if (exists(key)) {
            count++;
        }
    }
    return count;
}

int64_t DataStore::exists(std::span<const std::string> keys) const {
    int64_t count = 0;
    for (const auto& key : keys) {
        if (exists(key)) {
            count++;
        }
    }
    return count;
}

DataStore::IncrResult DataStore::incr(const std::string& key, int64_t& out_val) {
    if (expiry_.is_expired(key)) {
        del_raw(key);
        expiry_.remove(key);
    }

    auto it = store_.find(key);
    if (it == store_.end()) {
        out_val = 1;
        store_.insert_or_assign(key, "1");
        return IncrResult::Ok;
    }

    const std::string& val_str = it->second;
    int64_t val = 0;
    auto [ptr, ec] = std::from_chars(val_str.data(), val_str.data() + val_str.size(), val);
    if (ec != std::errc{} || ptr != val_str.data() + val_str.size()) {
        return IncrResult::NotAnInteger;
    }

    if (val == std::numeric_limits<int64_t>::max()) {
        return IncrResult::Overflow;
    }

    val += 1;
    out_val = val;
    it->second = std::to_string(val);
    return IncrResult::Ok;
}

std::string DataStore::type(std::string_view key) {
    if (expiry_.is_expired(key)) {
        del_raw(key);
        expiry_.remove(key);
        return "none";
    }
    return store_.find(key) != store_.end() ? "string" : "none";
}

std::string DataStore::type(std::string_view key) const {
    if (expiry_.is_expired(key)) {
        return "none";
    }
    return store_.find(key) != store_.end() ? "string" : "none";
}

void DataStore::flushall() {
    expiry_.clear();
    store_.clear();
}

size_t DataStore::dbsize() const {
    return store_.size();
}

void DataStore::restore_key(std::string key, std::string value, uint64_t expire_epoch_ms) {
    if (expire_epoch_ms > 0) {
        auto now_epoch_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );
        if (expire_epoch_ms <= now_epoch_ms) {
            return; // Đã hết hạn trong thời gian tắt server, bỏ qua
        }
        uint64_t rem_ms = expire_epoch_ms - now_epoch_ms;
        expiry_.set_expiry(key, std::chrono::milliseconds(rem_ms));
    } else {
        expiry_.remove(key);
    }
    store_.insert_or_assign(std::move(key), std::move(value));
}

}  // namespace mini_redis
