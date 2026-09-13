#pragma once

#include "expiry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mini_redis {

class DataStore {
public:
    enum class IncrResult {
        Ok,
        NotAnInteger,
        Overflow
    };

    DataStore() = default;

    // Set a key-value pair. Overwrites if key already exists, clearing any previous TTL.
    void set(std::string key, std::string value);

    // Get value by key. Triggers lazy expiry if expired.
    const std::string* get(std::string_view key);
    const std::string* get(std::string_view key) const;

    // Delete a single key. Returns true if deleted, false if key did not exist or was expired.
    bool del(std::string_view key);

    // Delete one or more keys. Returns number of keys removed.
    int64_t del(std::span<const std::string> keys);

    // Check if a single key exists. Triggers lazy expiry if expired.
    bool exists(std::string_view key);
    bool exists(std::string_view key) const;

    // Check how many of the specified keys exist. Duplicates in input are counted multiple times.
    int64_t exists(std::span<const std::string> keys);
    int64_t exists(std::span<const std::string> keys) const;

    // Increment integer value of key by 1. If key doesn't exist (or expired), initializes to 1.
    IncrResult incr(const std::string& key, int64_t& out_val);

    // Return the type of key ("string" or "none"). Triggers lazy expiry.
    std::string type(std::string_view key);
    std::string type(std::string_view key) const;

    // Remove all keys and TTLs from the database.
    void flushall();

    // Return the number of keys in the database.
    size_t dbsize() const;

    // Direct deletion from store_ without triggering expiry logic (used by Active Expiry)
    bool del_raw(std::string_view key);

    // Truy cập trực tiếp store (cho RDB serialization)
    const auto& raw_store() const noexcept { return store_; }

    // Khôi phục key-value từ RDB (kèm epoch timestamp)
    void restore_key(std::string key, std::string value, uint64_t expire_epoch_ms);

    // Expiry manager accessors
    ExpiryManager& expiry() noexcept { return expiry_; }
    const ExpiryManager& expiry() const noexcept { return expiry_; }

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

    std::unordered_map<std::string, std::string, StringHash, std::equal_to<>> store_;
    ExpiryManager expiry_;
};

}  // namespace mini_redis
