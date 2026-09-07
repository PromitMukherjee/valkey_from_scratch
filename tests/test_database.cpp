#include "database.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    koshdb::Database db;

    // 1. SET / GET
    db.set("name", "Promit");

    assert(db.get("name").value() == "Promit");

    // 2. EXISTS
    assert(db.exists("name"));

    // 3. DEL
    assert(db.del("name"));
    assert(!db.exists("name"));

    // 4. Missing key
    assert(!db.get("missing").has_value());

    // 5. Overwrite
    db.set("project", "old");
    db.set("project", "KoshDB");

    assert(db.get("project").value() == "KoshDB");

    // 6. Expiration
    db.set("temporary", "value");
    db.expire("temporary", 1);

    assert(db.exists("temporary"));

    std::this_thread::sleep_for(
        std::chrono::milliseconds(1100)
    );

    assert(!db.exists("temporary"));

    // 7. TTL for missing key
    assert(db.ttl("missing") == -2);

    // 8. TTL for persistent key
    db.set("persistent", "value");

    assert(db.ttl("persistent") == -1);

    std::cout << "All database tests passed!\n";

    return 0;
}