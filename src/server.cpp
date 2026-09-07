#include "server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace koshdb {

namespace {

bool set_non_blocking_fd(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);

    if (flags == -1) {
        return false;
    }

    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

bool has_crlf(
    const std::string& value,
    std::size_t position
) {
    return position + 1 < value.size() &&
           value[position] == '\r' &&
           value[position + 1] == '\n';
}

bool parse_decimal(
    const std::string& value,
    std::size_t start,
    std::size_t end,
    std::size_t& result
) {
    if (start >= end) {
        return false;
    }

    std::size_t number = 0;

    for (std::size_t i = start; i < end; ++i) {
        const unsigned char character =
            static_cast<unsigned char>(value[i]);

        if (character < '0' || character > '9') {
            return false;
        }

        number = number * 10 +
                 static_cast<std::size_t>(character - '0');
    }

    result = number;
    return true;
}

} // namespace

Server::Server(int port)
    : port(port),
      server_fd(-1),
      running(false),
      database(),
      persistence_manager("data"),
      dispatcher(database, persistence_manager),
      auth_manager(),
      thread_pool(),
      tls_manager(),
      replication_state() {
}

Server::~Server() {
    stop();

    try {
        persistence_manager.flush();
        persistence_manager.close();
    } catch (const std::exception& error) {
        std::cerr << "Persistence shutdown failed: "
                  << error.what()
                  << '\n';
    }

    for (auto& entry : clients) {
        if (entry.second.fd >= 0) {
            ::close(entry.second.fd);
        }
    }

    clients.clear();

    if (server_fd >= 0) {
        ::close(server_fd);
        server_fd = -1;
    }
}

void Server::setup_listener() {
    server_fd = ::socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        throw std::runtime_error(
            std::string("Failed to create socket: ") +
            std::strerror(errno)
        );
    }

    int reuse_address = 1;

    if (::setsockopt(
            server_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            sizeof(reuse_address)
        ) < 0) {
        ::close(server_fd);
        server_fd = -1;

        throw std::runtime_error(
            std::string("Failed to set SO_REUSEADDR: ") +
            std::strerror(errno)
        );
    }

    if (!set_non_blocking_fd(server_fd)) {
        ::close(server_fd);
        server_fd = -1;

        throw std::runtime_error(
            std::string("Failed to set server socket non-blocking: ") +
            std::strerror(errno)
        );
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(
        static_cast<uint16_t>(port)
    );

    if (::bind(
            server_fd,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)
        ) < 0) {
        ::close(server_fd);
        server_fd = -1;

        throw std::runtime_error(
            std::string("Failed to bind server socket: ") +
            std::strerror(errno)
        );
    }

    if (::listen(server_fd, SOMAXCONN) < 0) {
        ::close(server_fd);
        server_fd = -1;

        throw std::runtime_error(
            std::string("Failed to listen on server socket: ") +
            std::strerror(errno)
        );
    }

    std::cout << "KoshDB server listening on 127.0.0.1:"
              << port
              << '\n';
}

void Server::accept_clients() {
    while (running) {
        sockaddr_in client_address{};
        socklen_t client_address_length =
            sizeof(client_address);

        const int client_fd = ::accept(
            server_fd,
            reinterpret_cast<sockaddr*>(&client_address),
            &client_address_length
        );

        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            if (errno == EINTR) {
                continue;
            }

            std::cerr << "Failed to accept client: "
                      << std::strerror(errno)
                      << '\n';

            break;
        }

        if (!set_non_blocking_fd(client_fd)) {
            std::cerr
                << "Failed to set client socket non-blocking: "
                << std::strerror(errno)
                << '\n';

            ::close(client_fd);
            continue;
        }

        Client client{};
        client.fd = client_fd;
        client.input_buffer.reserve(4096);
        client.output_buffer.reserve(4096);
        client.authenticated = true;
        client.username.clear();
        client.tls_enabled = false;
        client.tls_handshake_completed = false;

        clients.emplace(
            client_fd,
            std::move(client)
        );

        std::cout << "Client connected: fd="
                  << client_fd
                  << '\n';
    }
}

void Server::close_client(int client_fd) {
    const auto iterator = clients.find(client_fd);

    if (iterator == clients.end()) {
        return;
    }

    ::close(client_fd);
    clients.erase(iterator);

    std::cout << "Client disconnected: fd="
              << client_fd
              << '\n';
}

void Server::handle_read(int client_fd) {
    const auto iterator = clients.find(client_fd);

    if (iterator == clients.end()) {
        return;
    }

    Client& client = iterator->second;

    char buffer[4096];

    while (true) {
        const ssize_t bytes_read = ::recv(
            client_fd,
            buffer,
            sizeof(buffer),
            0
        );

        if (bytes_read > 0) {
            client.input_buffer.append(
                buffer,
                static_cast<std::size_t>(bytes_read)
            );

            constexpr std::size_t max_request_size =
                1024 * 1024;

            if (client.input_buffer.size() >
                max_request_size) {
                client.output_buffer =
                    "-ERR request too large\r\n";

                close_client(client_fd);
                return;
            }

            process_client_buffer(client);
            continue;
        }

        if (bytes_read == 0) {
            close_client(client_fd);
            return;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        std::cerr << "Read error on fd="
                  << client_fd
                  << ": "
                  << std::strerror(errno)
                  << '\n';

        close_client(client_fd);
        return;
    }
}

void Server::handle_write(int client_fd) {
    const auto iterator = clients.find(client_fd);

    if (iterator == clients.end()) {
        return;
    }

    Client& client = iterator->second;

    while (!client.output_buffer.empty()) {
        const ssize_t bytes_written = ::send(
            client_fd,
            client.output_buffer.data(),
            client.output_buffer.size(),
            MSG_NOSIGNAL
        );

        if (bytes_written > 0) {
            client.output_buffer.erase(
                0,
                static_cast<std::size_t>(bytes_written)
            );

            continue;
        }

        if (bytes_written < 0 && errno == EINTR) {
            continue;
        }

        if (bytes_written < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }

        std::cerr << "Write error on fd="
                  << client_fd
                  << ": "
                  << std::strerror(errno)
                  << '\n';

        close_client(client_fd);
        return;
    }
}

void Server::handle_plain_read(Client& client) {
    handle_read(client.fd);
}

void Server::handle_plain_write(Client& client) {
    handle_write(client.fd);
}

void Server::handle_tls_read(Client& client) {
    /*
     * TLS support will be connected here once TLSManager
     * exposes the required SSL_read-based interface.
     *
     * For now, use the normal socket read path.
     */
    handle_plain_read(client);
}

void Server::handle_tls_write(Client& client) {
    /*
     * TLS support will be connected here once TLSManager
     * exposes the required SSL_write-based interface.
     *
     * For now, use the normal socket write path.
     */
    handle_plain_write(client);
}

std::vector<std::string> Server::extract_requests(
    std::string& buffer
) {
    std::vector<std::string> requests;

    while (true) {
        if (buffer.empty()) {
            break;
        }

        /*
         * KoshDB currently accepts RESP array requests.
         *
         * Example:
         * *2\r\n$3\r\nGET\r\n$3\r\nkey\r\n
         */

        if (buffer[0] != '*') {
            const std::size_t line_end =
                buffer.find("\r\n");

            if (line_end == std::string::npos) {
                break;
            }

            requests.push_back(
                buffer.substr(0, line_end + 2)
            );

            buffer.erase(0, line_end + 2);
            continue;
        }

        const std::size_t first_line_end =
            buffer.find("\r\n");

        if (first_line_end == std::string::npos) {
            break;
        }

        std::size_t argument_count = 0;

        if (!parse_decimal(
                buffer,
                1,
                first_line_end,
                argument_count
            )) {
            requests.push_back(
                "-ERR invalid RESP array\r\n"
            );

            buffer.erase(0, first_line_end + 2);
            continue;
        }

        std::size_t position = first_line_end + 2;
        bool complete_request = true;

        for (std::size_t argument = 0;
             argument < argument_count;
             ++argument) {
            if (position >= buffer.size()) {
                complete_request = false;
                break;
            }

            if (buffer[position] != '$') {
                requests.push_back(
                    "-ERR invalid RESP bulk string\r\n"
                );

                const std::size_t line_end =
                    buffer.find("\r\n", position);

                if (line_end == std::string::npos) {
                    buffer.clear();
                } else {
                    buffer.erase(0, line_end + 2);
                }

                complete_request = false;
                break;
            }

            const std::size_t length_line_end =
                buffer.find("\r\n", position);

            if (length_line_end == std::string::npos) {
                complete_request = false;
                break;
            }

            std::size_t bulk_length = 0;

            if (!parse_decimal(
                    buffer,
                    position + 1,
                    length_line_end,
                    bulk_length
                )) {
                requests.push_back(
                    "-ERR invalid bulk length\r\n"
                );

                buffer.erase(0, length_line_end + 2);
                complete_request = false;
                break;
            }

            position = length_line_end + 2;

            if (position + bulk_length + 2 >
                buffer.size()) {
                complete_request = false;
                break;
            }

            if (!has_crlf(
                    buffer,
                    position + bulk_length
                )) {
                requests.push_back(
                    "-ERR invalid bulk terminator\r\n"
                );

                buffer.erase(
                    0,
                    position + bulk_length
                );

                complete_request = false;
                break;
            }

            position += bulk_length + 2;
        }

        if (!complete_request) {
            break;
        }

        requests.push_back(
            buffer.substr(0, position)
        );

        buffer.erase(0, position);
    }

    return requests;
}

void Server::process_client_buffer(Client& client) {
    const std::vector<std::string> requests =
        extract_requests(client.input_buffer);

    for (const std::string& request : requests) {
        const std::string response =
            execute_command(client, request);

        client.output_buffer.append(response);
    }
}

std::string Server::execute_command(
    Client& client,
    const std::string& request
) {
    /*
     * Authentication-specific command handling can be added
     * here. At the current stage, all commands are allowed.
     */
    (void)client;

    return dispatcher.execute(request);
}

std::string Server::execute_auth_command(
    Client& client,
    const std::vector<std::string>& command
) {
    (void)client;
    (void)command;

    return "-ERR authentication is not implemented\r\n";
}

bool Server::command_allowed_without_auth(
    const std::vector<std::string>& command
) const {
    if (command.empty()) {
        return false;
    }

    return command[0] == "AUTH" ||
           command[0] == "PING" ||
           command[0] == "QUIT";
}

void Server::update_poll_events() {
    /*
     * Poll events are generated dynamically inside run().
     * This function is intentionally kept as a placeholder
     * for future event-management improvements.
     */
}

void Server::initialize_persistence() {
    try {
        persistence_manager.initialize();
        persistence_manager.recover(database);

        std::cout << "Persistence initialized"
                  << '\n';
    } catch (const std::exception& error) {
        std::cerr
            << "Persistence initialization failed: "
            << error.what()
            << '\n';

        throw;
    }
}

void Server::initialize_replication() {
    /*
     * Replication initialization will be connected here when
     * ReplicationState exposes its initialization API.
     */
}

void Server::publish_write_command(
    const std::string& request
) {
    /*
     * Replication publishing will be connected here when
     * ReplicationState exposes its publish API.
     */
    (void)request;
}

void Server::run() {
    if (running) {
        return;
    }

    initialize_persistence();
    initialize_replication();
    setup_listener();

    running = true;

    std::cout << "KoshDB server started"
              << '\n';

    while (running) {
        std::vector<pollfd> poll_fds;
        poll_fds.reserve(clients.size() + 1);

        pollfd listener_poll_fd{};
        listener_poll_fd.fd = server_fd;
        listener_poll_fd.events = POLLIN;
        listener_poll_fd.revents = 0;

        poll_fds.push_back(listener_poll_fd);

        for (const auto& entry : clients) {
            pollfd client_poll_fd{};
            client_poll_fd.fd = entry.first;
            client_poll_fd.events = POLLIN;
            client_poll_fd.revents = 0;

            if (!entry.second.output_buffer.empty()) {
                client_poll_fd.events |= POLLOUT;
            }

            poll_fds.push_back(client_poll_fd);
        }

        const int poll_result = ::poll(
            poll_fds.data(),
            poll_fds.size(),
            1000
        );

        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            std::cerr << "poll() failed: "
                      << std::strerror(errno)
                      << '\n';

            break;
        }

        if (poll_result == 0) {
            continue;
        }

        if (poll_fds[0].revents & POLLIN) {
            accept_clients();
        }

        if (poll_fds[0].revents &
            (POLLERR | POLLHUP | POLLNVAL)) {
            std::cerr << "Server listener error"
                      << '\n';

            break;
        }

        for (std::size_t index = 1;
             index < poll_fds.size();
             ++index) {
            const pollfd& current_poll_fd =
                poll_fds[index];

            if (clients.find(current_poll_fd.fd) ==
                clients.end()) {
                continue;
            }

            if (current_poll_fd.revents &
                (POLLERR | POLLHUP | POLLNVAL)) {
                close_client(current_poll_fd.fd);
                continue;
            }

            if (current_poll_fd.revents & POLLIN) {
                handle_read(current_poll_fd.fd);
            }

            if (clients.find(current_poll_fd.fd) ==
                clients.end()) {
                continue;
            }

            if (current_poll_fd.revents & POLLOUT) {
                handle_write(current_poll_fd.fd);
            }
        }
    }

    running = false;

    try {
        persistence_manager.flush();
        persistence_manager.close();
    } catch (const std::exception& error) {
        std::cerr << "Persistence shutdown failed: "
                  << error.what()
                  << '\n';
    }

    std::cout << "KoshDB server stopped"
              << '\n';
}

void Server::stop() {
    running = false;
}

} // namespace koshdb