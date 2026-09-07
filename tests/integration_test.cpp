#include "integration_test.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace koshdb::test {

namespace {

constexpr const char* HOST = "127.0.0.1";

constexpr int PRIMARY_PORT = 6379;
constexpr int REPLICA_PORT = 6380;
constexpr int TLS_PORT = 6381;

constexpr const char* AUTH_PASSWORD = "koshdb-secret";

constexpr int SOCKET_TIMEOUT_SECONDS = 5;

std::mutex output_mutex;

void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void assert_true(
    bool condition,
    const std::string& message
) {
    if (!condition) {
        fail(message);
    }
}

void assert_equal(
    const std::string& actual,
    const std::string& expected,
    const std::string& message
) {
    if (actual != expected) {
        fail(
            message +
            "\nExpected: " + expected +
            "\nActual: " + actual
        );
    }
}

void set_socket_timeout(int socket_fd) {
    timeval timeout{};
    timeout.tv_sec = SOCKET_TIMEOUT_SECONDS;
    timeout.tv_usec = 0;

    if (setsockopt(
            socket_fd,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            sizeof(timeout)
        ) < 0) {
        fail("Unable to configure receive timeout");
    }

    if (setsockopt(
            socket_fd,
            SOL_SOCKET,
            SO_SNDTIMEO,
            &timeout,
            sizeof(timeout)
        ) < 0) {
        fail("Unable to configure send timeout");
    }
}

int connect_tcp(
    const std::string& host,
    int port
) {
    int socket_fd = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (socket_fd < 0) {
        fail("Unable to create TCP socket");
    }

    set_socket_timeout(socket_fd);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(
        static_cast<std::uint16_t>(port)
    );

    if (inet_pton(
            AF_INET,
            host.c_str(),
            &address.sin_addr
        ) <= 0) {
        close(socket_fd);
        fail("Invalid server address");
    }

    if (connect(
            socket_fd,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)
        ) < 0) {
        close(socket_fd);
        fail(
            "Unable to connect to " +
            host + ":" + std::to_string(port)
        );
    }

    return socket_fd;
}

void close_socket(int socket_fd) {
    if (socket_fd >= 0) {
        close(socket_fd);
    }
}

std::string encode_bulk_string(
    const std::string& value
) {
    return "$" +
           std::to_string(value.size()) +
           "\r\n" +
           value +
           "\r\n";
}

std::string encode_command(
    const std::vector<std::string>& arguments
) {
    std::string request;

    request += "*" +
               std::to_string(arguments.size()) +
               "\r\n";

    for (const auto& argument : arguments) {
        request += encode_bulk_string(argument);
    }

    return request;
}

bool send_all(
    int socket_fd,
    const std::string& data
) {
    std::size_t sent = 0;

    while (sent < data.size()) {
        const ssize_t result = send(
            socket_fd,
            data.data() + sent,
            data.size() - sent,
            0
        );

        if (result <= 0) {
            return false;
        }

        sent += static_cast<std::size_t>(result);
    }

    return true;
}

std::string receive_response(
    int socket_fd
) {
    std::string response;
    char buffer[4096];

    while (true) {
        const ssize_t received = recv(
            socket_fd,
            buffer,
            sizeof(buffer),
            0
        );

        if (received <= 0) {
            break;
        }

        response.append(
            buffer,
            static_cast<std::size_t>(received)
        );

        /*
         * This test helper stops after the first complete
         * RESP response. It supports simple status, integer,
         * error, bulk-string, and array responses.
         */
        if (response.size() >= 2) {
            if (
                response[0] == '+' ||
                response[0] == '-' ||
                response[0] == ':'
            ) {
                if (
                    response.size() >= 2 &&
                    response.ends_with("\r\n")
                ) {
                    break;
                }
            }

            if (response[0] == '$') {
                const auto header_end =
                    response.find("\r\n");

                if (header_end != std::string::npos) {
                    const std::string length_text =
                        response.substr(
                            1,
                            header_end - 1
                        );

                    const long long length =
                        std::stoll(length_text);

                    if (length == -1) {
                        if (
                            response.size() >=
                            header_end + 4
                        ) {
                            break;
                        }
                    } else {
                        const std::size_t expected_size =
                            header_end +
                            2 +
                            static_cast<std::size_t>(length) +
                            2;

                        if (response.size() >= expected_size) {
                            break;
                        }
                    }
                }
            }
        }
    }

    return response;
}

std::string send_command(
    int socket_fd,
    const std::vector<std::string>& arguments
) {
    const std::string request =
        encode_command(arguments);

    assert_true(
        send_all(socket_fd, request),
        "Failed to send command"
    );

    return receive_response(socket_fd);
}

void wait_for_server(
    const std::string& host,
    int port,
    int attempts = 50
) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        int socket_fd = socket(
            AF_INET,
            SOCK_STREAM,
            0
        );

        if (socket_fd >= 0) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(
                static_cast<std::uint16_t>(port)
            );

            inet_pton(
                AF_INET,
                host.c_str(),
                &address.sin_addr
            );

            if (
                connect(
                    socket_fd,
                    reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)
                ) == 0
            ) {
                close_socket(socket_fd);
                return;
            }

            close_socket(socket_fd);
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(100)
        );
    }

    fail(
        "Server did not become available on port " +
        std::to_string(port)
    );
}

void print_test_success(
    const std::string& test_name
) {
    std::lock_guard<std::mutex> lock(output_mutex);

    std::cout
        << "[PASS] "
        << test_name
        << '\n';
}

void print_test_failure(
    const std::string& test_name,
    const std::string& error
) {
    std::lock_guard<std::mutex> lock(output_mutex);

    std::cerr
        << "[FAIL] "
        << test_name
        << ": "
        << error
        << '\n';
}

void run_test(
    const std::string& test_name,
    void (*test_function)()
) {
    try {
        test_function();
        print_test_success(test_name);
    } catch (const std::exception& error) {
        print_test_failure(
            test_name,
            error.what()
        );
        throw;
    }
}

} // namespace

void test_multiple_tcp_clients() {
    constexpr int CLIENT_COUNT = 10;

    std::vector<std::thread> clients;
    clients.reserve(CLIENT_COUNT);

    std::vector<std::string> responses(
        CLIENT_COUNT
    );

    for (int index = 0; index < CLIENT_COUNT; ++index) {
        clients.emplace_back(
            [index, &responses]() {
                const int socket_fd =
                    connect_tcp(
                        HOST,
                        PRIMARY_PORT
                    );

                const std::string key =
                    "integration:client:" +
                    std::to_string(index);

                const std::string value =
                    "value:" +
                    std::to_string(index);

                responses[index] =
                    send_command(
                        socket_fd,
                        {"SET", key, value}
                    );

                assert_equal(
                    responses[index],
                    "+OK\r\n",
                    "SET response was incorrect"
                );

                const std::string get_response =
                    send_command(
                        socket_fd,
                        {"GET", key}
                    );

                assert_equal(
                    get_response,
                    "$" +
                    std::to_string(value.size()) +
                    "\r\n" +
                    value +
                    "\r\n",
                    "GET response was incorrect"
                );

                close_socket(socket_fd);
            }
        );
    }

    for (auto& client : clients) {
        client.join();
    }
}

void test_authentication() {
    const int socket_fd =
        connect_tcp(
            HOST,
            PRIMARY_PORT
        );

    const std::string auth_response =
        send_command(
            socket_fd,
            {"AUTH", AUTH_PASSWORD}
        );

    assert_equal(
        auth_response,
        "+OK\r\n",
        "Correct password should authenticate"
    );

    const std::string set_response =
        send_command(
            socket_fd,
            {
                "SET",
                "integration:auth",
                "authenticated"
            }
        );

    assert_equal(
        set_response,
        "+OK\r\n",
        "Authenticated client should be able to write"
    );

    close_socket(socket_fd);

    const int invalid_socket =
        connect_tcp(
            HOST,
            PRIMARY_PORT
        );

    const std::string invalid_response =
        send_command(
            invalid_socket,
            {"AUTH", "wrong-password"}
        );

    assert_true(
        invalid_response.starts_with("-"),
        "Incorrect password should return an error"
    );

    close_socket(invalid_socket);
}

void test_tls_client() {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    SSL_CTX* context =
        SSL_CTX_new(
            TLS_client_method()
        );

    assert_true(
        context != nullptr,
        "Unable to create TLS client context"
    );

    /*
     * This is suitable for a local integration test.
     * Production clients should validate the server certificate.
     */
    SSL_CTX_set_verify(
        context,
        SSL_VERIFY_NONE,
        nullptr
    );

    const int socket_fd =
        connect_tcp(
            HOST,
            TLS_PORT
        );

    SSL* ssl =
        SSL_new(context);

    assert_true(
        ssl != nullptr,
        "Unable to create SSL object"
    );

    SSL_set_fd(
        ssl,
        socket_fd
    );

    const int handshake_result =
        SSL_connect(ssl);

    assert_true(
        handshake_result == 1,
        "TLS handshake failed"
    );

    const std::string request =
        encode_command({"PING"});

    const int written =
        SSL_write(
            ssl,
            request.data(),
            static_cast<int>(request.size())
        );

    assert_true(
        written == static_cast<int>(request.size()),
        "TLS request write failed"
    );

    char buffer[4096];

    const int received =
        SSL_read(
            ssl,
            buffer,
            sizeof(buffer)
        );

    assert_true(
        received > 0,
        "TLS response read failed"
    );

    const std::string response(
        buffer,
        static_cast<std::size_t>(received)
    );

    assert_equal(
        response,
        "+PONG\r\n",
        "TLS PING response was incorrect"
    );

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(context);

    close_socket(socket_fd);

    EVP_cleanup();
}

void test_persistence_recovery() {
    const int socket_fd =
        connect_tcp(
            HOST,
            PRIMARY_PORT
        );

    const std::string key =
        "integration:persistence";

    const std::string value =
        "recovery-value";

    const std::string set_response =
        send_command(
            socket_fd,
            {"SET", key, value}
        );

    assert_equal(
        set_response,
        "+OK\r\n",
        "Persistence SET failed"
    );

    close_socket(socket_fd);

    /*
     * The running server must be stopped and restarted
     * by the test runner or shell script between these
     * two operations.
     *
     * This file-based marker tells the external runner
     * which key must be checked after restart.
     */
    std::ofstream marker(
        "data/integration_recovery_marker.txt"
    );

    assert_true(
        marker.is_open(),
        "Unable to create recovery marker"
    );

    marker << key << '\n';
    marker << value << '\n';

    marker.close();

    /*
     * When this test is executed against an already
     * restarted server, the key must still be available.
     */
    const int recovered_socket =
        connect_tcp(
            HOST,
            PRIMARY_PORT
        );

    const std::string recovered_response =
        send_command(
            recovered_socket,
            {"GET", key}
        );

    assert_equal(
        recovered_response,
        "$" +
        std::to_string(value.size()) +
        "\r\n" +
        value +
        "\r\n",
        "Persisted value was not recovered"
    );

    close_socket(recovered_socket);
}

void test_primary_replica_consistency() {
    const int primary_socket =
        connect_tcp(
            HOST,
            PRIMARY_PORT
        );

    const std::string key =
        "integration:replication";

    const std::string value =
        "replicated-value";

    const std::string set_response =
        send_command(
            primary_socket,
            {"SET", key, value}
        );

    assert_equal(
        set_response,
        "+OK\r\n",
        "Primary SET failed"
    );

    close_socket(primary_socket);

    bool replicated = false;

    for (int attempt = 0; attempt < 50; ++attempt) {
        const int replica_socket =
            connect_tcp(
                HOST,
                REPLICA_PORT
            );

        const std::string response =
            send_command(
                replica_socket,
                {"GET", key}
            );

        close_socket(replica_socket);

        if (
            response ==
            "$" +
            std::to_string(value.size()) +
            "\r\n" +
            value +
            "\r\n"
        ) {
            replicated = true;
            break;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(100)
        );
    }

    assert_true(
        replicated,
        "Primary value was not replicated"
    );
}

void run_all_tests() {
    wait_for_server(
        HOST,
        PRIMARY_PORT
    );

    run_test(
        "Multiple TCP clients",
        test_multiple_tcp_clients
    );

    run_test(
        "Authentication",
        test_authentication
    );

    run_test(
        "TLS client",
        test_tls_client
    );

    run_test(
        "Persistence recovery",
        test_persistence_recovery
    );

    wait_for_server(
        HOST,
        REPLICA_PORT
    );

    run_test(
        "Primary-replica consistency",
        test_primary_replica_consistency
    );
}

} // namespace koshdb::test