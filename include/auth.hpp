#pragma once

#include <string>
#include <unordered_map>

namespace koshdb {

class AuthManager {
public:
    AuthManager();

    void set_password(
        const std::string& username,
        const std::string& password
    );

    bool authenticate(
        const std::string& username,
        const std::string& password
    ) const;

    bool user_exists(const std::string& username) const;

    bool user_enabled(const std::string& username) const;

private:
    struct User {
        std::string password_hash;
        bool enabled;
    };

    std::unordered_map<std::string, User> users;

    static std::string hash_password(
        const std::string& password
    );
};

} // namespace koshdb