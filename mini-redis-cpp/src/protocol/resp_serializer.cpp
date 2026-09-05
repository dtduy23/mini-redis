#include "resp_serializer.hpp"

namespace mini_redis {

std::string RespSerializer::serialize_simple_string(std::string_view str) {
    std::string res;
    res.reserve(1 + str.size() + 2);
    res.push_back('+');
    res.append(str);
    res.append("\r\n");
    return res;
}

std::string RespSerializer::serialize_ok() {
    return "+OK\r\n";
}

std::string RespSerializer::serialize_pong() {
    return "+PONG\r\n";
}

std::string RespSerializer::serialize_error(std::string_view msg) {
    std::string res;
    if (msg.rfind("ERR", 0) == 0) {
        res.reserve(1 + msg.size() + 2);
        res.push_back('-');
        res.append(msg);
        res.append("\r\n");
    } else {
        res.reserve(5 + msg.size() + 2);
        res.append("-ERR ");
        res.append(msg);
        res.append("\r\n");
    }
    return res;
}

std::string RespSerializer::serialize_custom_error(std::string_view code, std::string_view msg) {
    std::string res;
    res.reserve(1 + code.size() + 1 + msg.size() + 2);
    res.push_back('-');
    res.append(code);
    res.push_back(' ');
    res.append(msg);
    res.append("\r\n");
    return res;
}

std::string RespSerializer::serialize_integer(int64_t val) {
    std::string s = std::to_string(val);
    std::string res;
    res.reserve(1 + s.size() + 2);
    res.push_back(':');
    res.append(s);
    res.append("\r\n");
    return res;
}

std::string RespSerializer::serialize_bulk_string(std::string_view str) {
    std::string len_str = std::to_string(str.size());
    std::string res;
    res.reserve(1 + len_str.size() + 2 + str.size() + 2);
    res.push_back('$');
    res.append(len_str);
    res.append("\r\n");
    res.append(str);
    res.append("\r\n");
    return res;
}

std::string RespSerializer::serialize_null_bulk_string() {
    return "$-1\r\n";
}

std::string RespSerializer::serialize_empty_bulk_string() {
    return "$0\r\n\r\n";
}

std::string RespSerializer::serialize_array(const std::vector<std::string>& elements) {
    std::string res;
    std::string count_str = std::to_string(elements.size());
    res.append("*");
    res.append(count_str);
    res.append("\r\n");
    for (const auto& elem : elements) {
        res.append(serialize_bulk_string(elem));
    }
    return res;
}

std::string RespSerializer::serialize_raw_array(const std::vector<std::string>& raw_elements) {
    std::string res;
    std::string count_str = std::to_string(raw_elements.size());
    res.append("*");
    res.append(count_str);
    res.append("\r\n");
    for (const auto& elem : raw_elements) {
        res.append(elem);
    }
    return res;
}

std::string RespSerializer::serialize_null_array() {
    return "*-1\r\n";
}

std::string RespSerializer::serialize_empty_array() {
    return "*0\r\n";
}

}  // namespace mini_redis
