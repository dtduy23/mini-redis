#pragma once

#include "store/data_store.hpp"

#include <string>
#include <vector>

namespace mini_redis {

class Handlers {
public:
    Handlers() = delete;

    static void handle_ping(const std::vector<std::string>& cmd, DataStore& store, std::string& out);
    static void handle_echo(const std::vector<std::string>& cmd, DataStore& store, std::string& out);
    static void handle_set(const std::vector<std::string>& cmd, DataStore& store, std::string& out);
    static void handle_get(const std::vector<std::string>& cmd, DataStore& store, std::string& out);
    static void handle_del(const std::vector<std::string>& cmd, DataStore& store, std::string& out);
    static void handle_unlink(const std::vector<std::string>& cmd, DataStore& store, std::string& out);
    static void handle_exists(const std::vector<std::string>& cmd, DataStore& store, std::string& out);
    static void handle_incr(const std::vector<std::string>& cmd, DataStore& store, std::string& out);
    static void handle_type(const std::vector<std::string>& cmd, DataStore& store, std::string& out);
    static void handle_flushall(const std::vector<std::string>& cmd, DataStore& store, std::string& out);
    static void handle_expire(const std::vector<std::string>& cmd, DataStore& store, std::string& out);
    static void handle_ttl(const std::vector<std::string>& cmd, DataStore& store, std::string& out);
    static void handle_persist(const std::vector<std::string>& cmd, DataStore& store, std::string& out);
    static void handle_save(const std::vector<std::string>& cmd, DataStore& store, std::string& out);
    static void handle_bgsave(const std::vector<std::string>& cmd, DataStore& store, std::string& out);
    static void handle_command(const std::vector<std::string>& cmd, DataStore& store, std::string& out);
};

}  // namespace mini_redis
