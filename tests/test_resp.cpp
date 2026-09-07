#include "resp.hpp"

#include <cassert>
#include <iostream>
#include <string>

int main() {
    // 1. Parse SET command.
    {
        std::string request =
            "*3\r\n"
            "$3\r\nSET\r\n"
            "$4\r\nname\r\n"
            "$6\r\nPromit\r\n";

        koshdb::RespParser parser(request);

        assert(parser.parse());

        const auto& parts = parser.get_parts();

        assert(parts.size() == 3);
        assert(parts[0] == "SET");
        assert(parts[1] == "name");
        assert(parts[2] == "Promit");
    }

    // 2. Parse GET command.
    {
        std::string request =
            "*2\r\n"
            "$3\r\nGET\r\n"
            "$4\r\nname\r\n";

        koshdb::RespParser parser(request);

        assert(parser.parse());

        const auto& parts = parser.get_parts();

        assert(parts.size() == 2);
        assert(parts[0] == "GET");
        assert(parts[1] == "name");
    }

    // 3. Parse empty string.
    {
        std::string request =
            "*2\r\n"
            "$3\r\nSET\r\n"
            "$0\r\n\r\n";

        koshdb::RespParser parser(request);

        assert(parser.parse());

        const auto& parts = parser.get_parts();

        assert(parts[1].empty());
    }

    // 4. Reject invalid request.
    {
        std::string request = "SET name Promit\r\n";

        koshdb::RespParser parser(request);

        assert(!parser.parse());
        assert(parser.has_error());
    }

    // 5. Reject incomplete request.
    {
        std::string request =
            "*2\r\n"
            "$3\r\nGET\r\n"
            "$4\r\nnam";

        koshdb::RespParser parser(request);

        assert(!parser.parse());
        assert(parser.has_error());
    }

    // 6. Test response helpers.
    {
        assert(
            koshdb::resp_simple_string("OK")
            == "+OK\r\n"
        );

        assert(
            koshdb::resp_integer(42)
            == ":42\r\n"
        );

        assert(
            koshdb::resp_bulk_string("Promit")
            == "$6\r\nPromit\r\n"
        );

        assert(
            koshdb::resp_null()
            == "$-1\r\n"
        );
    }

    std::cout << "All RESP tests passed!\n";

    return 0;
}