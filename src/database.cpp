#include "database.hpp"

namespace koshdb {

bool Database::is_expired(const Entry& entry) const {
    if (!entry.expires_at.has_value()) {
        return false;
    }

    return std::chrono::system_clock::now() >=
           entry.expires_at.value();
}

void Database::remove_if_expired(const std::string& key) {
    auto it = store.find(key);

    if (it != store.end() && is_expired(it->second)) {
        store.erase(it);
    }
}

void Database::set(
    const std::string& key,
    const std::string& value
) {
    store[key] = Entry{
        value,
        std::nullopt
    };
}

std::optional<std::string> Database::get(
    const std::string& key
) {
    remove_if_expired(key);

    auto it = store.find(key);

    if (it == store.end()) {
        return std::nullopt;
    }

    return it->second.value;
}

bool Database::del(const std::string& key) {
    remove_if_expired(key);

    return store.erase(key) > 0;
}

bool Database::exists(const std::string& key) {
    remove_if_expired(key);

    return store.find(key) != store.end();
}

void Database::expire(
    const std::string& key,
    long long seconds
) {
    auto it = store.find(key);

    if (it == store.end()) {
        return;
    }

    it->second.expires_at =
        std::chrono::system_clock::now() +
        std::chrono::seconds(seconds);
}

long long Database::ttl(const std::string& key) {
    remove_if_expired(key);

    auto it = store.find(key);

    if (it == store.end()) {
        return -2;
    }

    if (!it->second.expires_at.has_value()) {
        return -1;
    }

    auto remaining =
        std::chrono::duration_cast<std::chrono::seconds>(
            it->second.expires_at.value() -
            std::chrono::system_clock::now()
        ).count();

    return remaining < 0 ? 0 : remaining;
}

std::vector<KeyValueEntry> Database::entries() const {
    std::vector<KeyValueEntry> result;

    result.reserve(store.size());

    for (const auto& [key, entry] : store) {
        if (is_expired(entry)) {
            continue;
        }

        result.push_back(
            KeyValueEntry{
                key,
                entry.value
            }
        );
    }

    return result;
}

void Database::restore_set(
    const std::string& key,
    const std::string& value
) {
    store[key] = Entry{
        value,
        std::nullopt
    };
}

void Database::restore_delete(
    const std::string& key
) {
    store.erase(key);
}

} // namespace koshdb