#pragma once

#include "store/data_store.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace mini_redis {

using CommandHandler = void (*)(const std::vector<std::string>& cmd, DataStore& store, std::string& out);

struct CommandDescriptor {
    int min_args;  // Số đối số tối thiểu (bao gồm tên lệnh)
    int max_args;  // Số đối số tối đa (bao gồm tên lệnh, -1 nếu không giới hạn)
    CommandHandler handler;
};

class Dispatcher {
public:
    Dispatcher();

    // Điều hướng lệnh đến handler tương ứng, ghi kết quả RESP vào out
    void dispatch(const std::vector<std::string>& cmd, DataStore& store, std::string& out) const;

    // Đăng ký lệnh mới vào bảng điều hướng
    void register_command(std::string name, int min_args, int max_args, CommandHandler handler);

private:
    std::unordered_map<std::string, CommandDescriptor> commands_;
};

}  // namespace mini_redis
