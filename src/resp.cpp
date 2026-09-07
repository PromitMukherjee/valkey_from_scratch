#include "resp.hpp"

#include <charconv>
#include <system_error>

namespace koshdb {

RespParser::RespParser(const std::string& data)
    : data(data) {}

void RespParser::set_error(const std::string& message) {
    error = true;
    error_message = message;
}

bool RespParser::read_line(std::string& line) {
    size_t end = data.find("\r\n", position);

    if (end == std::string::npos) {
        set_error("Incomplete RESP line");
        return false;
    }

    line = data.substr(position, end - position);

    position = end + 2;

    return true;
}

bool RespParser::parse_bulk_string(std::string& output) {
    std::string line;

    if (!read_line(line)) {
        return false;
    }

    if (line.empty() || line[0] != '$') {
        set_error("Expected bulk string");
        return false;
    }

    long long length = 0;

    auto result = std::from_chars(
        line.data() + 1,
        line.data() + line.size(),
        length
    );

    if (result.ec != std::errc() ||
        result.ptr != line.data() + line.size()) {
        set_error("Invalid bulk string length");
        return false;
    }

    if (length < 0) {
        set_error("Null bulk strings are not supported");
        return false;
    }

    if (position + static_cast<size_t>(length) + 2 >
        data.size()) {
        set_error("Incomplete bulk string");
        return false;
    }

    output = data.substr(
        position,
        static_cast<size_t>(length)
    );

    position += static_cast<size_t>(length);

    if (data.substr(position, 2) != "\r\n") {
        set_error("Bulk string missing CRLF");
        return false;
    }

    position += 2;

    return true;
}

bool RespParser::parse_array() {
    std::string line;

    if (!read_line(line)) {
        return false;
    }

    if (line.empty() || line[0] != '*') {
        set_error("Expected array");
        return false;
    }

    long long count = 0;

    auto result = std::from_chars(
        line.data() + 1,
        line.data() + line.size(),
        count
    );

    if (result.ec != std::errc() ||
        result.ptr != line.data() + line.size()) {
        set_error("Invalid array length");
        return false;
    }

    if (count <= 0) {
        set_error("Empty command");
        return false;
    }

    // Reasonable limit for our university project.
    if (count > 1024) {
        set_error("Command too large");
        return false;
    }

    for (long long i = 0; i < count; ++i) {
        std::string part;

        if (!parse_bulk_string(part)) {
            return false;
        }

        parts.push_back(std::move(part));
    }

    return true;
}

bool RespParser::parse() {
    parts.clear();
    position = 0;
    error = false;
    error_message.clear();

    if (data.empty()) {
        set_error("Empty request");
        return false;
    }

    if (data[0] != '*') {
        set_error("Only RESP arrays are supported");
        return false;
    }

    if (!parse_array()) {
        return false;
    }

    // A Phase 2 request must contain exactly one command.
    if (position != data.size()) {
        set_error("Extra data after RESP command");
        return false;
    }

    return true;
}

const std::vector<std::string>& RespParser::get_parts() const {
    return parts;
}

bool RespParser::has_error() const {
    return error;
}

const std::string& RespParser::get_error() const {
    return error_message;
}

std::string resp_simple_string(const std::string& value) {
    return "+" + value + "\r\n";
}

std::string resp_error(const std::string& value) {
    return "-" + value + "\r\n";
}

std::string resp_integer(long long value) {
    return ":" + std::to_string(value) + "\r\n";
}

std::string resp_bulk_string(const std::string& value) {
    return "$" + std::to_string(value.size()) +
           "\r\n" + value + "\r\n";
}

std::string resp_null() {
    return "$-1\r\n";
}

} // namespace koshdb