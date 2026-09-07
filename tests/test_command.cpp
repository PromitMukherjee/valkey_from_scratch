#include "command.hpp"
#include "persistence.hpp"

#include <iostream>
#include <string>

int main() {
    koshdb::Database database;
    koshdb::PersistenceManager persistence_manager("test_data");

    persistence_manager.initialize();

    koshdb::CommandDispatcher dispatcher(
        database,
        persistence_manager
    );

    auto check = [](
        const std::string& test_name,
        const std::string& actual,
        const std::string& expected
    ) {
        if (actual != expected) {
            std::cerr << "[FAIL] "
                      << test_name
                      << "\nExpected: "
                      << expected
                      << "\nActual: "
                      << actual
                      << "\n";

            return false;
        }

        std::cout << "[PASS] "
                  << test_name
                  << '\n';

        return true;
    };

    bool success = true;

    success &= check(
        "PING",
        dispatcher.execute("*1\r\n$4\r\nPING\r\n"),
        "+PONG\r\n"
    );

    success &= check(
        "PING with message",
        dispatcher.execute(
            "*2\r\n$4\r\nPING\r\n$5\r\nhello\r\n"
        ),
        "$5\r\nhello\r\n"
    );

    success &= check(
        "SET",
        dispatcher.execute(
            "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n"
        ),
        "+OK\r\n"
    );

    success &= check(
        "GET",
        dispatcher.execute(
            "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n"
        ),
        "$3\r\nbar\r\n"
    );

    success &= check(
        "EXISTS",
        dispatcher.execute(
            "*2\r\n$6\r\nEXISTS\r\n$3\r\nfoo\r\n"
        ),
        ":1\r\n"
    );

    success &= check(
        "TTL without expiration",
        dispatcher.execute(
            "*2\r\n$3\r\nTTL\r\n$3\r\nfoo\r\n"
        ),
        ":-1\r\n"
    );

    success &= check(
        "EXPIRE",
        dispatcher.execute(
            "*3\r\n$6\r\nEXPIRE\r\n$3\r\nfoo\r\n$2\r\n60\r\n"
        ),
        ":1\r\n"
    );

    success &= check(
        "TTL after expiration",
        dispatcher.execute(
            "*2\r\n$3\r\nTTL\r\n$3\r\nfoo\r\n"
        ),
        ":60\r\n"
    );

    success &= check(
        "DEL",
        dispatcher.execute(
            "*2\r\n$3\r\nDEL\r\n$3\r\nfoo\r\n"
        ),
        ":1\r\n"
    );

    success &= check(
        "GET missing key",
        dispatcher.execute(
            "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n"
        ),
        "$-1\r\n"
    );

    success &= check(
        "DEL missing key",
        dispatcher.execute(
            "*2\r\n$3\r\nDEL\r\n$3\r\nfoo\r\n"
        ),
        ":0\r\n"
    );

    success &= check(
        "Unknown command",
        dispatcher.execute(
            "*1\r\n$7\r\nUNKNOWN\r\n"
        ),
        "-ERR unknown command 'UNKNOWN'\r\n"
    );

    success &= check(
        "SAVE",
        dispatcher.execute(
            "*1\r\n$4\r\nSAVE\r\n"
        ),
        "+OK\r\n"
    );

    persistence_manager.flush();
    persistence_manager.close();

    if (!success) {
        return 1;
    }

    std::cout << "All command tests passed"
              << '\n';

    return 0;
}