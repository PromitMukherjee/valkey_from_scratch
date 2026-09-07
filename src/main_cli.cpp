#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <charconv>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::size_t MAX_RESPONSE_SIZE = 1024 * 1024;

void throw_socket_error(const std::string& operation) {
    throw std::runtime_error(
        operation + ": " + std::strerror(errno)
    );
}

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current;
    bool quoted = false;
    char quote_character = '\0';
    bool escaping = false;

    for (char character : line) {
        if (escaping) {
            current.push_back(character);
            escaping = false;
            continue;
        }

        if (character == '\\') {
            escaping = true;
            continue;
        }

        if (quoted) {
            if (character == quote_character) {
                quoted = false;
            } else {
                current.push_back(character);
            }
            continue;
        }

        if (character == '"' || character == '\'') {
            quoted = true;
            quote_character = character;
        } else if (std::isspace(
                       static_cast<unsigned char>(character))) {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(character);
        }
    }

    if (escaping) {
        current.push_back('\\');
    }

    if (quoted) {
        throw std::runtime_error("unterminated quoted argument");
    }

    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }

    return tokens;
}

std::string encode_resp(const std::vector<std::string>& arguments) {
    std::string request = "*" +
        std::to_string(arguments.size()) + "\r\n";

    for (const std::string& argument : arguments) {
        request += "$" +
            std::to_string(argument.size()) +
            "\r\n" +
            argument +
            "\r\n";
    }

    return request;
}

void send_all(int fd, const std::string& data) {
    std::size_t offset = 0;

    while (offset < data.size()) {
        const ssize_t written = ::send(
            fd,
            data.data() + offset,
            data.size() - offset,
            MSG_NOSIGNAL
        );

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_socket_error("send");
        }

        if (written == 0) {
            throw std::runtime_error("send returned zero bytes");
        }

        offset += static_cast<std::size_t>(written);
    }
}

std::string receive_resp(int fd) {
    std::string response;
    response.reserve(256);

    char buffer[4096];

    while (true) {
        const ssize_t received = ::recv(
            fd,
            buffer,
            sizeof(buffer),
            0
        );

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_socket_error("recv");
        }

        if (received == 0) {
            throw std::runtime_error(
                "server closed the connection"
            );
        }

        response.append(
            buffer,
            static_cast<std::size_t>(received)
        );

        if (response.size() > MAX_RESPONSE_SIZE) {
            throw std::runtime_error(
                "server response exceeds maximum size"
            );
        }

        /*
         * KoshDB responses currently contain one RESP value.
         * Detect the end of simple strings, errors, integers,
         * bulk strings, and arrays.
         */
        if (response[0] == '+' ||
            response[0] == '-' ||
            response[0] == ':') {
            if (response.size() >= 2 &&
                response.compare(
                    response.size() - 2,
                    2,
                    "\r\n"
                ) == 0) {
                return response;
            }
        } else if (response[0] == '$') {
            const std::size_t line_end =
                response.find("\r\n");

            if (line_end != std::string::npos) {
                long long length = 0;
                const auto result = std::from_chars(
                    response.data() + 1,
                    response.data() + line_end,
                    length
                );

                if (result.ec == std::errc() &&
                    result.ptr == response.data() + line_end) {
                    if (length == -1) {
                        return response;
                    }

                    if (length >= 0) {
                        const std::size_t expected =
                            line_end + 2 +
                            static_cast<std::size_t>(length) +
                            2;

                        if (response.size() >= expected) {
                            return response.substr(0, expected);
                        }
                    }
                }
            }
        } else if (response[0] == '*') {
            /*
             * Arrays are not currently returned by the command
             * dispatcher, but preserve a useful fallback parser.
             */
            const std::size_t line_end =
                response.find("\r\n");

            if (line_end != std::string::npos) {
                long long count = 0;
                const auto result = std::from_chars(
                    response.data() + 1,
                    response.data() + line_end,
                    count
                );

                if (result.ec == std::errc() &&
                    result.ptr == response.data() + line_end &&
                    count >= 0) {
                    std::size_t position = line_end + 2;
                    bool complete = true;

                    for (long long index = 0;
                         index < count;
                         ++index) {
                        const std::size_t item_end =
                            response.find("\r\n", position);

                        if (item_end == std::string::npos ||
                            position >= response.size() ||
                            response[position] != '$') {
                            complete = false;
                            break;
                        }

                        long long length = 0;
                        const auto item_result = std::from_chars(
                            response.data() + position + 1,
                            response.data() + item_end,
                            length
                        );

                        if (item_result.ec != std::errc() ||
                            item_result.ptr !=
                                response.data() + item_end ||
                            length < 0) {
                            complete = false;
                            break;
                        }

                        position = item_end + 2 +
                            static_cast<std::size_t>(length) + 2;

                        if (position > response.size()) {
                            complete = false;
                            break;
                        }
                    }

                    if (complete && position <= response.size()) {
                        return response.substr(0, position);
                    }
                }
            }
        }
    }
}

std::string decode_resp(const std::string& response) {
    if (response.empty()) {
        return "(empty response)";
    }

    const char type = response[0];

    if (type == '+') {
        return response.substr(1, response.size() - 3);
    }

    if (type == '-') {
        return "(error) " + response.substr(1, response.size() - 3);
    }

    if (type == ':') {
        return "(integer) " + response.substr(1, response.size() - 3);
    }

    if (type == '$') {
        const std::size_t line_end = response.find("\r\n");

        if (line_end == std::string::npos) {
            return "(protocol error)";
        }

        if (response.compare(1, line_end - 1, "-1") == 0) {
            return "(nil)";
        }

        const std::size_t value_start = line_end + 2;
        const std::size_t value_end =
            response.size() >= 2 &&
            response.compare(response.size() - 2, 2, "\r\n") == 0
                ? response.size() - 2
                : response.size();

        return "\"" +
            response.substr(value_start, value_end - value_start) +
            "\"";
    }

    return response;
}

int connect_to_server(
    const std::string& host,
    int port
) {
    const int fd = ::socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (fd < 0) {
        throw_socket_error("socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(
        static_cast<std::uint16_t>(port)
    );

    if (::inet_pton(
            AF_INET,
            host.c_str(),
            &address.sin_addr
        ) != 1) {
        ::close(fd);
        throw std::runtime_error(
            "invalid IPv4 address: " + host
        );
    }

    if (::connect(
            fd,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)
        ) < 0) {
        const std::string message =
            "connect: " + std::string(std::strerror(errno));
        ::close(fd);
        throw std::runtime_error(message);
    }

    return fd;
}

void print_help() {
    std::cout
        << "KoshDB interactive CLI\n"
        << "Commands:\n"
        << "  PING\n"
        << "  SET <key> <value>\n"
        << "  GET <key>\n"
        << "  DEL <key>\n"
        << "  EXISTS <key>\n"
        << "  EXPIRE <key> <seconds>\n"
        << "  TTL <key>\n"
        << "  HELP\n"
        << "  EXIT or QUIT\n"
        << "\n"
        << "Arguments may be enclosed in single or double quotes.\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 6379;

    try {
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];

            if (argument == "--host") {
                if (index + 1 >= argc) {
                    throw std::runtime_error(
                        "--host requires a value"
                    );
                }
                host = argv[++index];
            } else if (argument == "--port") {
                if (index + 1 >= argc) {
                    throw std::runtime_error(
                        "--port requires a value"
                    );
                }

                port = std::stoi(argv[++index]);

                if (port < 1 || port > 65535) {
                    throw std::runtime_error(
                        "port must be between 1 and 65535"
                    );
                }
            } else if (argument == "--help" ||
                       argument == "-h") {
                std::cout
                    << "Usage: koshdb-cli "
                    << "[--host <address>] "
                    << "[--port <port>]\n";
                return 0;
            } else {
                throw std::runtime_error(
                    "unknown option: " + argument
                );
            }
        }

        const int fd = connect_to_server(host, port);

        std::cout
            << "Connected to KoshDB at "
            << host << ":" << port << "\n"
            << "Type HELP for commands.\n";

        std::string line;

        while (true) {
            std::cout << "koshdb> " << std::flush;

            if (!std::getline(std::cin, line)) {
                std::cout << "\n";
                break;
            }

            if (line.empty()) {
                continue;
            }

            const std::vector<std::string> arguments =
                tokenize(line);

            if (arguments.empty()) {
                continue;
            }

            const std::string command = arguments[0];

            if (command == "EXIT" ||
                command == "exit" ||
                command == "QUIT" ||
                command == "quit") {
                break;
            }

            if (command == "HELP" ||
                command == "help") {
                print_help();
                continue;
            }

            send_all(fd, encode_resp(arguments));
            std::cout << decode_resp(receive_resp(fd))
                      << "\n";
        }

        ::close(fd);
        std::cout << "Disconnected from KoshDB.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "KoshDB CLI error: "
                  << error.what() << "\n";
        return 1;
    }
}