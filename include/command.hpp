#pragma once

#include "database.hpp"
#include "persistence.hpp"

#include <string>
#include <vector>

namespace koshdb {

class CommandDispatcher {
public:
    CommandDispatcher(
        Database& database,
        PersistenceManager& persistence
    );

    // Execute one complete RESP request.
    std::string execute(const std::string& request);

private:
    Database& database;
    PersistenceManager& persistence;

    std::string execute_command(
        const std::string& command,
        const std::vector<std::string>& args
    );
};

} // namespace koshdb