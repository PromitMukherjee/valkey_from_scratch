#include "tls_manager.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <stdexcept>
#include <string>

namespace koshdb {

namespace {

std::string openssl_error_message() {
    unsigned long error_code = ERR_get_error();

    if (error_code == 0) {
        return "unknown OpenSSL error";
    }

    char message[256];

    ERR_error_string_n(
        error_code,
        message,
        sizeof(message)
    );

    return std::string(message);
}

} // namespace

TLSManager::TLSManager()
    : context(nullptr),
      tls_enabled(false) {
}

TLSManager::~TLSManager() {
    cleanup();
}

void TLSManager::initialize(
    const std::string& certificate_path,
    const std::string& private_key_path
) {
    cleanup();

    /*
     * TLS_server_method() allows OpenSSL to negotiate
     * the highest supported TLS version.
     */
    const SSL_METHOD* method = TLS_server_method();

    context = SSL_CTX_new(method);

    if (context == nullptr) {
        throw std::runtime_error(
            "Unable to create TLS context: " +
            openssl_error_message()
        );
    }

    /*
     * Disable obsolete SSL/TLS versions.
     */
    SSL_CTX_set_min_proto_version(
        context,
        TLS1_2_VERSION
    );

    /*
     * Load the server certificate chain.
     *
     * The certificate file should normally contain:
     * server certificate followed by intermediate certificates.
     */
    if (SSL_CTX_use_certificate_chain_file(
            context,
            certificate_path.c_str()
        ) != 1) {
        const std::string error =
            openssl_error_message();

        cleanup();

        throw std::runtime_error(
            "Unable to load TLS certificate: " + error
        );
    }

    /*
     * Load the private key.
     */
    if (SSL_CTX_use_PrivateKey_file(
            context,
            private_key_path.c_str(),
            SSL_FILETYPE_PEM
        ) != 1) {
        const std::string error =
            openssl_error_message();

        cleanup();

        throw std::runtime_error(
            "Unable to load TLS private key: " + error
        );
    }

    /*
     * Confirm that the private key matches the
     * public key contained in the certificate.
     */
    if (SSL_CTX_check_private_key(context) != 1) {
        const std::string error =
            openssl_error_message();

        cleanup();

        throw std::runtime_error(
            "TLS certificate and private key do not match: " +
            error
        );
    }

    tls_enabled = true;
}

SSL* TLSManager::create_session(int client_fd) {
    if (!tls_enabled || context == nullptr) {
        return nullptr;
    }

    SSL* ssl = SSL_new(context);

    if (ssl == nullptr) {
        throw std::runtime_error(
            "Unable to create TLS session: " +
            openssl_error_message()
        );
    }

    if (SSL_set_fd(ssl, client_fd) != 1) {
        SSL_free(ssl);

        throw std::runtime_error(
            "Unable to associate TLS session with socket: " +
            openssl_error_message()
        );
    }

    SSL_set_accept_state(ssl);

    return ssl;
}

bool TLSManager::accept(SSL* ssl) {
    if (ssl == nullptr) {
        return false;
    }

    const int result = SSL_accept(ssl);

    if (result == 1) {
        return true;
    }

    return false;
}

int TLSManager::read(
    SSL* ssl,
    void* buffer,
    int length
) {
    if (ssl == nullptr) {
        return -1;
    }

    return SSL_read(
        ssl,
        buffer,
        length
    );
}

int TLSManager::write(
    SSL* ssl,
    const void* buffer,
    int length
) {
    if (ssl == nullptr) {
        return -1;
    }

    return SSL_write(
        ssl,
        buffer,
        length
    );
}

int TLSManager::get_error(
    SSL* ssl,
    int result
) const {
    if (ssl == nullptr) {
        return SSL_ERROR_SSL;
    }

    return SSL_get_error(
        ssl,
        result
    );
}

void TLSManager::shutdown_session(SSL* ssl) {
    if (ssl != nullptr) {
        SSL_shutdown(ssl);
    }
}

void TLSManager::destroy_session(SSL* ssl) {
    if (ssl != nullptr) {
        shutdown_session(ssl);
        SSL_free(ssl);
    }
}

bool TLSManager::enabled() const {
    return tls_enabled;
}

void TLSManager::cleanup() {
    if (context != nullptr) {
        SSL_CTX_free(context);
        context = nullptr;
    }

    tls_enabled = false;
}

} // namespace koshdb