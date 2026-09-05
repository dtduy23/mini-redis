#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mini_redis {

class RespSerializer {
public:
    RespSerializer() = delete;

    // Simple Strings
    static std::string serialize_simple_string(std::string_view str);
    static std::string serialize_ok();
    static std::string serialize_pong();

    // Errors
    static std::string serialize_error(std::string_view msg);
    static std::string serialize_custom_error(std::string_view code, std::string_view msg);

    // Integers
    static std::string serialize_integer(int64_t val);

    // Bulk Strings
    static std::string serialize_bulk_string(std::string_view str);
    static std::string serialize_null_bulk_string();
    static std::string serialize_empty_bulk_string();

    // Arrays (elements serialized as bulk strings)
    static std::string serialize_array(const std::vector<std::string>& elements);

    // Arrays where elements are already pre-serialized RESP strings
    static std::string serialize_raw_array(const std::vector<std::string>& raw_elements);
    static std::string serialize_null_array();
    static std::string serialize_empty_array();
};

}  // namespace mini_redis
