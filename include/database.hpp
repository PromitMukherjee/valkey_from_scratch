#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace koshdb {

struct Entry {
    std::string value;

    std::optional<std::chrono::system_clock::time_point> expires_at;
};

struct KeyValueEntry {
    std::string key;
    std::string value;
};

class Database {
public:
    void set(
        const std::string& key,
        const std::string& value
    );

    std::optional<std::string> get(
        const std::string& key
    );

    bool del(const std::string& key);

    bool exists(const std::string& key);

    void expire(
        const std::string& key,
        long long seconds
    );

    long long ttl(const std::string& key);

    // Persistence support
    std::vector<KeyValueEntry> entries() const;

    void restore_set(
        const std::string& key,
        const std::string& value
    );

    void restore_delete(
        const std::string& key
    );

private:
    std::unordered_map<std::string, Entry> store;

    bool is_expired(const Entry& entry) const;

    void remove_if_expired(
        const std::string& key
    );
};

} // namespace koshdb