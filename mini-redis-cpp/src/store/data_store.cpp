#include "data_store.hpp"

#include <charconv>
#include <limits>
#include <utility>

namespace mini_redis {

void DataStore::set(std::string key, std::string value) {
    store_.insert_or_assign(std::move(key), std::move(value));
}

const std::string* DataStore::get(std::string_view key) const {
    auto it = store_.find(key);
    if (it != store_.end()) {
        return &it->second;
    }
    return nullptr;
}

bool DataStore::del(std::string_view key) {
    auto it = store_.find(key);
    if (it != store_.end()) {
        store_.erase(it);
        return true;
    }
    return false;
}

int64_t DataStore::del(std::span<const std::string> keys) {
    int64_t count = 0;
    for (const auto& key : keys) {
        count += store_.erase(key);
    }
    return count;
}

bool DataStore::exists(std::string_view key) const {
    return store_.find(key) != store_.end();
}

int64_t DataStore::exists(std::span<const std::string> keys) const {
    int64_t count = 0;
    for (const auto& key : keys) {
        if (store_.find(key) != store_.end()) {
            count++;
        }
    }
    return count;
}

DataStore::IncrResult DataStore::incr(const std::string& key, int64_t& out_val) {
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

std::string DataStore::type(std::string_view key) const {
    if (store_.find(key) != store_.end()) {
        return "string";
    }
    return "none";
}

void DataStore::flushall() {
    store_.clear();
}

size_t DataStore::dbsize() const {
    return store_.size();
}

}  // namespace mini_redis
