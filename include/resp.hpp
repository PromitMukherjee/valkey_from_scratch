#pragma once

#include <string>
#include <vector>

namespace koshdb {

class RespParser {
public:
    explicit RespParser(const std::string& data);

    // Parse one complete RESP command.
    bool parse();

    const std::vector<std::string>& get_parts() const;

    bool has_error() const;

    const std::string& get_error() const;

private:
    std::string data;
    std::vector<std::string> parts;

    bool error = false;
    std::string error_message;

    size_t position = 0;

    bool parse_array();

    bool parse_bulk_string(std::string& output);

    bool read_line(std::string& line);

    void set_error(const std::string& message);
};

// RESP response helpers.

std::string resp_simple_string(
    const std::string& value
);

std::string resp_error(
    const std::string& value
);

std::string resp_integer(
    long long value
);

std::string resp_bulk_string(
    const std::string& value
);

std::string resp_null();

} // namespace koshdb