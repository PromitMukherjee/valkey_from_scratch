#include "server.hpp"
#include "replication.hpp"
#include <exception>
#include <iostream>

int main() {
    try {
        koshdb::Server server(6379);
        server.run();
    } catch (const std::exception& error) {
        std::cerr
            << "KoshDB server error: "
            << error.what()
            << '\n';

        return 1;
    }

    return 0;
}