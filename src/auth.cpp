#include "auth.hpp"

#include <openssl/sha.h>

#include <iomanip>
#include <sstream>

namespace koshdb {

AuthManager::AuthManager() {
    // Development default account.
    // Change this before using KoshDB outside localhost.
    set_password("default", "koshdb");
}

std::string AuthManager::hash_password(
    const std::string& password
) {
    unsigned char digest[SHA256_DIGEST_LENGTH];

    SHA256(
        reinterpret_cast<const unsigned char*>(password.data()),
        password.size(),
        digest
    );

    std::ostringstream output;

    for (unsigned char byte : digest) {
        output
            << std::hex
            << std::setw(2)
            << std::setfill('0')
            << static_cast<int>(byte);
    }

    return output.str();
}

void AuthManager::set_password(
    const std::string& username,
    const std::string& password
) {
    users[username] = User {
        hash_password(password),
        true
    };
}

bool AuthManager::user_exists(
    const std::string& username
) const {
    return users.find(username) != users.end();
}

bool AuthManager::user_enabled(
    const std::string& username
) const {
    const auto iterator = users.find(username);

    if (iterator == users.end()) {
        return false;
    }

    return iterator->second.enabled;
}

bool AuthManager::authenticate(
    const std::string& username,
    const std::string& password
) const {
    const auto iterator = users.find(username);

    if (iterator == users.end()) {
        return false;
    }

    if (!iterator->second.enabled) {
        return false;
    }

    return iterator->second.password_hash == hash_password(password);
}

} // namespace koshdb