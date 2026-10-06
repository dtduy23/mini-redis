#include "dispatcher.hpp"
#include "handlers.hpp"
#include "protocol/resp_serializer.hpp"

#include <cctype>

namespace mini_redis {

Dispatcher::Dispatcher() {
    register_command("PING", 1, 2, Handlers::handle_ping);
    register_command("ECHO", 2, 2, Handlers::handle_echo);
    register_command("SET", 3, 3, Handlers::handle_set);
    register_command("GET", 2, 2, Handlers::handle_get);
    register_command("DEL", 2, -1, Handlers::handle_del);
    register_command("UNLINK", 2, -1, Handlers::handle_unlink);
    register_command("EXISTS", 2, -1, Handlers::handle_exists);
    register_command("INCR", 2, 2, Handlers::handle_incr);
    register_command("TYPE", 2, 2, Handlers::handle_type);
    register_command("FLUSHALL", 1, 1, Handlers::handle_flushall);
    register_command("EXPIRE", 3, 3, Handlers::handle_expire);
    register_command("TTL", 2, 2, Handlers::handle_ttl);
    register_command("PERSIST", 2, 2, Handlers::handle_persist);
    register_command("SAVE", 1, 1, Handlers::handle_save);
    register_command("BGSAVE", 1, 1, Handlers::handle_bgsave);
    register_command("COMMAND", 1, -1, Handlers::handle_command);
}

void Dispatcher::register_command(std::string name, int min_args, int max_args, CommandHandler handler) {
    commands_[std::move(name)] = CommandDescriptor{min_args, max_args, handler};
}

void Dispatcher::dispatch(const std::vector<std::string>& cmd, DataStore& store, std::string& out,
                          size_t max_response_size) const {
    if (cmd.empty()) {
        return;
    }

    std::string cmd_upper = cmd[0];
    for (char& c : cmd_upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    auto it = commands_.find(cmd_upper);
    if (it == commands_.end()) {
        out += RespSerializer::serialize_error("unknown command '" + cmd[0] + "'");
        return;
    }

    const CommandDescriptor& desc = it->second;
    int argc = static_cast<int>(cmd.size());
    if (argc < desc.min_args || (desc.max_args != -1 && argc > desc.max_args)) {
        std::string cmd_lower = cmd[0];
        for (char& c : cmd_lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        out += RespSerializer::serialize_error("wrong number of arguments for '" + cmd_lower + "' command");
        return;
    }

    // Snapshot values can be larger than the current network buffer limit.
    // Reject an oversized GET before allocating its serialized response.
    if (desc.handler == Handlers::handle_get) {
        const auto* value = store.get(cmd[1]);
        if (value != nullptr) {
            const size_t overhead = std::to_string(value->size()).size() + 5;
            if (overhead > max_response_size || value->size() > max_response_size - overhead) {
                out += RespSerializer::serialize_error("response exceeds output buffer limit");
                return;
            }
        }
    }
    desc.handler(cmd, store, out);
}

}  // namespace mini_redis
