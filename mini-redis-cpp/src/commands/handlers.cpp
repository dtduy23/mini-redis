#include "handlers.hpp"
#include "protocol/resp_serializer.hpp"

#include <span>

namespace mini_redis {

void Handlers::handle_ping(const std::vector<std::string>& cmd, DataStore& /*store*/, std::string& out) {
    if (cmd.size() == 1) {
        out += RespSerializer::serialize_pong();
    } else {
        out += RespSerializer::serialize_bulk_string(cmd[1]);
    }
}

void Handlers::handle_echo(const std::vector<std::string>& cmd, DataStore& /*store*/, std::string& out) {
    out += RespSerializer::serialize_bulk_string(cmd[1]);
}

void Handlers::handle_set(const std::vector<std::string>& cmd, DataStore& store, std::string& out) {
    store.set(cmd[1], cmd[2]);
    out += RespSerializer::serialize_ok();
}

void Handlers::handle_get(const std::vector<std::string>& cmd, DataStore& store, std::string& out) {
    const std::string* val = store.get(cmd[1]);
    if (val != nullptr) {
        out += RespSerializer::serialize_bulk_string(*val);
    } else {
        out += RespSerializer::serialize_null_bulk_string();
    }
}

void Handlers::handle_del(const std::vector<std::string>& cmd, DataStore& store, std::string& out) {
    std::span<const std::string> keys(cmd.data() + 1, cmd.size() - 1);
    int64_t count = store.del(keys);
    out += RespSerializer::serialize_integer(count);
}

void Handlers::handle_exists(const std::vector<std::string>& cmd, DataStore& store, std::string& out) {
    std::span<const std::string> keys(cmd.data() + 1, cmd.size() - 1);
    int64_t count = store.exists(keys);
    out += RespSerializer::serialize_integer(count);
}

void Handlers::handle_incr(const std::vector<std::string>& cmd, DataStore& store, std::string& out) {
    int64_t new_val = 0;
    auto res = store.incr(cmd[1], new_val);
    switch (res) {
        case DataStore::IncrResult::Ok:
            out += RespSerializer::serialize_integer(new_val);
            break;
        case DataStore::IncrResult::NotAnInteger:
            out += RespSerializer::serialize_error("value is not an integer or out of range");
            break;
        case DataStore::IncrResult::Overflow:
            out += RespSerializer::serialize_error("increment or decrement would overflow");
            break;
    }
}

void Handlers::handle_type(const std::vector<std::string>& cmd, DataStore& store, std::string& out) {
    out += RespSerializer::serialize_simple_string(store.type(cmd[1]));
}

void Handlers::handle_flushall(const std::vector<std::string>& /*cmd*/, DataStore& store, std::string& out) {
    store.flushall();
    out += RespSerializer::serialize_ok();
}

}  // namespace mini_redis
