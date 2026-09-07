#include "command.hpp"

#include "resp.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <exception>
#include <string>
#include <system_error>
#include <vector>

namespace koshdb {

namespace {

std::string to_upper(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        }
    );

    return value;
}

bool parse_integer(
    const std::string& value,
    long long& result
) {
    if (value.empty()) {
        return false;
    }

    auto parsed = std::from_chars(
        value.data(),
        value.data() + value.size(),
        result
    );

    return parsed.ec == std::errc() &&
           parsed.ptr == value.data() + value.size();
}

} // namespace

CommandDispatcher::CommandDispatcher(
    Database& database,
    PersistenceManager& persistence
)
    : database(database),
      persistence(persistence) {
}

std::string CommandDispatcher::execute(
    const std::string& request
) {
    RespParser parser(request);

    if (!parser.parse()) {
        return resp_error("ERR " + parser.get_error());
    }

    const auto& parts = parser.get_parts();

    if (parts.empty()) {
        return resp_error("ERR empty command");
    }

    std::string command = to_upper(parts[0]);

    std::vector<std::string> args(
        parts.begin() + 1,
        parts.end()
    );

    return execute_command(command, args);
}

std::string CommandDispatcher::execute_command(
    const std::string& command,
    const std::vector<std::string>& args
) {
    // PING
    if (command == "PING") {
        if (args.empty()) {
            return resp_simple_string("PONG");
        }

        if (args.size() == 1) {
            return resp_bulk_string(args[0]);
        }

        return resp_error(
            "ERR wrong number of arguments for 'ping'"
        );
    }

    // SET key value
    if (command == "SET") {
        if (args.size() != 2) {
            return resp_error(
                "ERR wrong number of arguments for 'set'"
            );
        }

        database.set(args[0], args[1]);

        return resp_simple_string("OK");
    }

    // GET key
    if (command == "GET") {
        if (args.size() != 1) {
            return resp_error(
                "ERR wrong number of arguments for 'get'"
            );
        }

        auto value = database.get(args[0]);

        if (!value.has_value()) {
            return resp_null();
        }

        return resp_bulk_string(value.value());
    }

    // DEL key
    if (command == "DEL") {
        if (args.size() != 1) {
            return resp_error(
                "ERR wrong number of arguments for 'del'"
            );
        }

        return resp_integer(
            database.del(args[0]) ? 1 : 0
        );
    }

    // EXISTS key
    if (command == "EXISTS") {
        if (args.size() != 1) {
            return resp_error(
                "ERR wrong number of arguments for 'exists'"
            );
        }

        return resp_integer(
            database.exists(args[0]) ? 1 : 0
        );
    }

    // EXPIRE key seconds
    if (command == "EXPIRE") {
        if (args.size() != 2) {
            return resp_error(
                "ERR wrong number of arguments for 'expire'"
            );
        }

        long long seconds = 0;

        if (!parse_integer(args[1], seconds)) {
            return resp_error(
                "ERR invalid expire time"
            );
        }

        database.expire(args[0], seconds);

        return resp_integer(1);
    }

    // TTL key
    if (command == "TTL") {
        if (args.size() != 1) {
            return resp_error(
                "ERR wrong number of arguments for 'ttl'"
            );
        }

        return resp_integer(database.ttl(args[0]));
    }

    // SAVE
    if (command == "SAVE") {
        if (!args.empty()) {
            return resp_error(
                "ERR wrong number of arguments for 'save'"
            );
        }

        try {
            /*
             * Flush pending append-only-log data first,
             * then create a consistent snapshot.
             */
            persistence.flush();
            persistence.save_snapshot(database);

            return resp_simple_string("OK");
        } catch (const std::exception& error) {
            return resp_error(
                std::string("ERR persistence failure: ") +
                error.what()
            );
        }
    }

    return resp_error(
        "ERR unknown command '" + command + "'"
    );
}

} // namespace koshdb