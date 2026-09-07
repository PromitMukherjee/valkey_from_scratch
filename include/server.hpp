#pragma once

#include "auth.hpp"
#include "command.hpp"
#include "persistence.hpp"
#include "replication.hpp"
#include "thread_pool.hpp"
#include "tls_manager.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace koshdb {

class Server {
public:
    explicit Server(int port = 6379);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void run();
    void stop();

private:
    struct Client {
        int fd = -1;

        std::string input_buffer;
        std::string output_buffer;

        bool authenticated = true;
        std::string username;

        bool tls_enabled = false;
        bool tls_handshake_completed = false;
    };

    int port;
    int server_fd;
    bool running;

    Database database;
    CommandDispatcher dispatcher;

    AuthManager auth_manager;
    ThreadPool thread_pool;
    TLSManager tls_manager;
    PersistenceManager persistence_manager;
    ReplicationState replication_state;

    std::unordered_map<int, Client> clients;

    void setup_listener();
    void accept_clients();

    void handle_read(int client_fd);
    void handle_write(int client_fd);

    void handle_plain_read(Client& client);
    void handle_plain_write(Client& client);

    void handle_tls_read(Client& client);
    void handle_tls_write(Client& client);

    void process_client_buffer(Client& client);

    std::string execute_command(
        Client& client,
        const std::string& request
    );

    std::string execute_auth_command(
        Client& client,
        const std::vector<std::string>& command
    );

    bool command_allowed_without_auth(
        const std::vector<std::string>& command
    ) const;

    void close_client(int client_fd);

    std::vector<std::string> extract_requests(
        std::string& buffer
    );

    void update_poll_events();

    void initialize_persistence();
    void initialize_replication();

    void publish_write_command(
        const std::string& request
    );
};

} // namespace koshdb