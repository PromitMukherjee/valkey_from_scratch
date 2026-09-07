#pragma once

#include <openssl/ssl.h>

#include <string>

namespace koshdb {

class TLSManager {
public:
    TLSManager();
    ~TLSManager();

    TLSManager(const TLSManager&) = delete;
    TLSManager& operator=(const TLSManager&) = delete;

    void initialize(
        const std::string& certificate_path,
        const std::string& private_key_path
    );

    SSL* create_session(int client_fd);

    bool accept(SSL* ssl);
    int read(SSL* ssl, void* buffer, int length);
    int write(SSL* ssl, const void* buffer, int length);

    int get_error(SSL* ssl, int result) const;

    void shutdown_session(SSL* ssl);
    void destroy_session(SSL* ssl);

    bool enabled() const;

private:
    SSL_CTX* context;
    bool tls_enabled;

    void cleanup();
};

} // namespace koshdb