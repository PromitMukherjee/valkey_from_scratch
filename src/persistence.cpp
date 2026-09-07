#include "persistence.hpp"

#include "command.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace koshdb {

namespace {

constexpr char SNAPSHOT_MAGIC[] = "KOSHDBSNAP";
constexpr std::uint64_t SNAPSHOT_VERSION = 1;

constexpr std::size_t MAX_RECOVERY_RECORD_SIZE = 512 * 1024;
constexpr std::uint64_t MAX_SNAPSHOT_ENTRIES = 10'000'000;
constexpr std::uint64_t MAX_STRING_SIZE = 512 * 1024;

void throw_system_error(const std::string& operation) {
    throw std::runtime_error(
        operation + ": " + std::strerror(errno)
    );
}

void write_exact(
    int fd,
    const void* data,
    std::size_t size
) {
    const char* bytes =
        static_cast<const char*>(data);

    std::size_t written = 0;

    while (written < size) {
        const ssize_t result = ::write(
            fd,
            bytes + written,
            size - written
        );

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            throw_system_error("write");
        }

        if (result == 0) {
            throw std::runtime_error(
                "write returned zero bytes"
            );
        }

        written += static_cast<std::size_t>(result);
    }
}

void read_exact(
    int fd,
    void* data,
    std::size_t size
) {
    char* bytes =
        static_cast<char*>(data);

    std::size_t received = 0;

    while (received < size) {
        const ssize_t result = ::read(
            fd,
            bytes + received,
            size - received
        );

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            throw_system_error("read");
        }

        if (result == 0) {
            throw std::runtime_error(
                "unexpected end of file"
            );
        }

        received += static_cast<std::size_t>(result);
    }
}

void fsync_file(int fd) {
    if (::fsync(fd) == -1) {
        throw_system_error("fsync");
    }
}

void fsync_directory(
    const std::filesystem::path& directory
) {
    const int directory_fd = ::open(
        directory.c_str(),
        O_RDONLY | O_DIRECTORY
    );

    if (directory_fd == -1) {
        throw_system_error(
            "open persistence directory"
        );
    }

    const int result = ::fsync(directory_fd);
    const int saved_errno = errno;

    ::close(directory_fd);

    if (result == -1) {
        errno = saved_errno;

        throw_system_error(
            "fsync persistence directory"
        );
    }
}

/*
 * Binary snapshot helpers for POSIX file descriptors.
 *
 * These are different from the class member overloads that
 * operate on std::ofstream and std::ifstream.
 */
void write_uint64_to_fd(
    int fd,
    std::uint64_t value
) {
    write_exact(
        fd,
        &value,
        sizeof(value)
    );
}

void write_string_to_fd(
    int fd,
    const std::string& value
) {
    if (value.size() > MAX_STRING_SIZE) {
        throw std::runtime_error(
            "String exceeds snapshot size limit"
        );
    }

    write_uint64_to_fd(
        fd,
        static_cast<std::uint64_t>(
            value.size()
        )
    );

    if (!value.empty()) {
        write_exact(
            fd,
            value.data(),
            value.size()
        );
    }
}

std::vector<std::string> parse_resp_request(
    const std::string& request
) {
    std::vector<std::string> arguments;

    std::size_t cursor = 0;

    auto find_crlf = [&](std::size_t start) {
        return request.find("\r\n", start);
    };

    if (request.empty() || request[cursor] != '*') {
        throw std::runtime_error(
            "AOF record does not begin with RESP array"
        );
    }

    const std::size_t array_end =
        find_crlf(cursor);

    if (array_end == std::string::npos) {
        throw std::runtime_error(
            "Incomplete RESP array header"
        );
    }

    const long long argument_count =
        std::stoll(
            request.substr(
                cursor + 1,
                array_end - cursor - 1
            )
        );

    if (argument_count < 0 ||
        argument_count > 1024) {
        throw std::runtime_error(
            "Invalid RESP argument count"
        );
    }

    cursor = array_end + 2;

    for (long long index = 0;
         index < argument_count;
         ++index) {
        if (cursor >= request.size() ||
            request[cursor] != '$') {
            throw std::runtime_error(
                "Expected RESP bulk string"
            );
        }

        const std::size_t length_end =
            find_crlf(cursor);

        if (length_end == std::string::npos) {
            throw std::runtime_error(
                "Incomplete RESP bulk length"
            );
        }

        const long long length =
            std::stoll(
                request.substr(
                    cursor + 1,
                    length_end - cursor - 1
                )
            );

        if (length < 0 ||
            length > static_cast<long long>(
                MAX_RECOVERY_RECORD_SIZE
            )) {
            throw std::runtime_error(
                "Invalid RESP bulk string length"
            );
        }

        cursor = length_end + 2;

        const std::size_t string_length =
            static_cast<std::size_t>(length);

        if (cursor + string_length + 2 >
            request.size()) {
            throw std::runtime_error(
                "Incomplete RESP bulk string"
            );
        }

        if (request.compare(
                cursor + string_length,
                2,
                "\r\n"
            ) != 0) {
            throw std::runtime_error(
                "RESP bulk string is missing CRLF"
            );
        }

        arguments.emplace_back(
            request.substr(
                cursor,
                string_length
            )
        );

        cursor += string_length + 2;
    }

    if (cursor != request.size()) {
        throw std::runtime_error(
            "Trailing bytes in AOF record"
        );
    }

    return arguments;
}

} // namespace

PersistenceManager::PersistenceManager(
    std::string data_directory
)
    : data_directory(std::move(data_directory)),
      snapshot_file(
          this->data_directory + "/dump.rdb"
      ),
      temporary_snapshot_file(
          this->data_directory + "/dump.rdb.tmp"
      ),
      append_only_log_file(
          this->data_directory + "/appendonly.aof"
      ),
      initialized(false) {
}

PersistenceManager::~PersistenceManager() {
    close();
}

void PersistenceManager::initialize() {
    if (initialized) {
        return;
    }

    create_data_directory();
    open_append_only_log();

    initialized = true;
}

void PersistenceManager::create_data_directory() {
    std::error_code error;

    std::filesystem::create_directories(
        data_directory,
        error
    );

    if (error) {
        throw std::runtime_error(
            "Unable to create persistence directory: " +
            error.message()
        );
    }
}

void PersistenceManager::open_append_only_log() {
    append_only_log.open(
        append_only_log_file,
        std::ios::binary |
        std::ios::out |
        std::ios::app
    );

    if (!append_only_log.is_open()) {
        throw std::runtime_error(
            "Unable to open append-only log: " +
            append_only_log_file
        );
    }
}

void PersistenceManager::append_command(
    const std::string& resp_command
) {
    if (!initialized) {
        throw std::runtime_error(
            "PersistenceManager is not initialized"
        );
    }

    if (resp_command.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(log_mutex);

    append_only_log.write(
        resp_command.data(),
        static_cast<std::streamsize>(
            resp_command.size()
        )
    );

    if (!append_only_log.good()) {
        throw std::runtime_error(
            "Unable to append command to AOF"
        );
    }

    append_only_log.flush();

    if (!append_only_log.good()) {
        throw std::runtime_error(
            "Unable to flush append-only log"
        );
    }
}

void PersistenceManager::flush() {
    std::lock_guard<std::mutex> lock(log_mutex);

    if (!append_only_log.is_open()) {
        return;
    }

    append_only_log.flush();

    if (!append_only_log.good()) {
        throw std::runtime_error(
            "Unable to flush append-only log"
        );
    }
}

void PersistenceManager::recover(
    Database& database
) {
    if (!initialized) {
        initialize();
    }

    recover_snapshot(database);
    recover_append_only_log(database);
}

void PersistenceManager::recover_snapshot(
    Database& database
) {
    if (!std::filesystem::exists(snapshot_file)) {
        return;
    }

    std::ifstream input(
        snapshot_file,
        std::ios::binary
    );

    if (!input.is_open()) {
        throw std::runtime_error(
            "Unable to open snapshot file: " +
            snapshot_file
        );
    }

    char magic[sizeof(SNAPSHOT_MAGIC) - 1];

    input.read(
        magic,
        sizeof(magic)
    );

    if (!input.good()) {
        throw std::runtime_error(
            "Snapshot file is truncated"
        );
    }

    if (std::memcmp(
            magic,
            SNAPSHOT_MAGIC,
            sizeof(magic)
        ) != 0) {
        throw std::runtime_error(
            "Invalid snapshot magic"
        );
    }

    const std::uint64_t version =
        read_uint64(input);

    if (version != SNAPSHOT_VERSION) {
        throw std::runtime_error(
            "Unsupported snapshot version"
        );
    }

    const std::uint64_t entry_count =
        read_uint64(input);

    if (entry_count > MAX_SNAPSHOT_ENTRIES) {
        throw std::runtime_error(
            "Snapshot contains too many entries"
        );
    }

    for (std::uint64_t index = 0;
         index < entry_count;
         ++index) {
        const std::string key =
            read_string(input);

        const std::string value =
            read_string(input);

        database.restore_set(
            key,
            value
        );
    }
}

void PersistenceManager::recover_append_only_log(
    Database& database
) {
    std::ifstream input(
        append_only_log_file,
        std::ios::binary
    );

    if (!input.is_open()) {
        return;
    }

    std::string record;
    record.reserve(4096);

    char character;

    while (input.get(character)) {
        record.push_back(character);

        if (record.size() > MAX_RECOVERY_RECORD_SIZE) {
            throw std::runtime_error(
                "AOF record exceeds maximum size"
            );
        }

        if (record.size() >= 2 &&
            record[record.size() - 2] == '\r' &&
            record[record.size() - 1] == '\n') {
            const std::vector<std::string> arguments =
                parse_resp_request(record);

            if (arguments.empty()) {
                record.clear();
                continue;
            }

            const std::string& command =
                arguments[0];

            if (command == "SET" ||
                command == "set") {
                if (arguments.size() != 3) {
                    throw std::runtime_error(
                        "Invalid SET record in AOF"
                    );
                }

                database.restore_set(
                    arguments[1],
                    arguments[2]
                );
            }
            else if (
                command == "DEL" ||
                command == "del"
            ) {
                if (arguments.size() != 2) {
                    throw std::runtime_error(
                        "Invalid DEL record in AOF"
                    );
                }

                database.restore_delete(
                    arguments[1]
                );
            }

            record.clear();
        }
    }

    /*
     * A partial final record can remain after a crash.
     * It is intentionally ignored.
     */
}

void PersistenceManager::save_snapshot(
    const Database& database
) {
    if (!initialized) {
        initialize();
    }

    write_snapshot_file(
        database,
        temporary_snapshot_file
    );

    atomically_replace_snapshot();
}

void PersistenceManager::write_snapshot_file(
    const Database& database,
    const std::string& path
) {
    const int fd = ::open(
        path.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC,
        0644
    );

    if (fd == -1) {
        throw_system_error(
            "open temporary snapshot"
        );
    }

    try {
        write_exact(
            fd,
            SNAPSHOT_MAGIC,
            sizeof(SNAPSHOT_MAGIC) - 1
        );

        write_uint64_to_fd(
            fd,
            SNAPSHOT_VERSION
        );

        const std::vector<KeyValueEntry> entries =
            database.entries();

        write_uint64_to_fd(
            fd,
            static_cast<std::uint64_t>(
                entries.size()
            )
        );

        for (const KeyValueEntry& entry : entries) {
            write_string_to_fd(
                fd,
                entry.key
            );

            write_string_to_fd(
                fd,
                entry.value
            );
        }

        fsync_file(fd);
        ::close(fd);
    }
    catch (...) {
        ::close(fd);
        throw;
    }
}

void PersistenceManager::atomically_replace_snapshot() {
    if (::rename(
            temporary_snapshot_file.c_str(),
            snapshot_file.c_str()
        ) == -1) {
        throw_system_error(
            "rename snapshot"
        );
    }

    fsync_directory(
        std::filesystem::path(data_directory)
    );
}

void PersistenceManager::write_uint64(
    std::ofstream& output,
    std::uint64_t value
) {
    output.write(
        reinterpret_cast<const char*>(&value),
        sizeof(value)
    );

    if (!output.good()) {
        throw std::runtime_error(
            "Unable to write uint64 value"
        );
    }
}

std::uint64_t PersistenceManager::read_uint64(
    std::ifstream& input
) {
    std::uint64_t value = 0;

    input.read(
        reinterpret_cast<char*>(&value),
        sizeof(value)
    );

    if (!input.good()) {
        throw std::runtime_error(
            "Unable to read uint64 value"
        );
    }

    return value;
}

void PersistenceManager::write_string(
    std::ofstream& output,
    const std::string& value
) {
    if (value.size() > MAX_STRING_SIZE) {
        throw std::runtime_error(
            "String exceeds snapshot size limit"
        );
    }

    write_uint64(
        output,
        static_cast<std::uint64_t>(
            value.size()
        )
    );

    if (!value.empty()) {
        output.write(
            value.data(),
            static_cast<std::streamsize>(
                value.size()
            )
        );
    }

    if (!output.good()) {
        throw std::runtime_error(
            "Unable to write string to snapshot"
        );
    }
}

std::string PersistenceManager::read_string(
    std::ifstream& input
) {
    const std::uint64_t length =
        read_uint64(input);

    if (length > MAX_STRING_SIZE) {
        throw std::runtime_error(
            "Snapshot string exceeds size limit"
        );
    }

    std::string value(
        static_cast<std::size_t>(length),
        '\0'
    );

    if (length > 0) {
        input.read(
            value.data(),
            static_cast<std::streamsize>(length)
        );
    }

    if (!input.good()) {
        throw std::runtime_error(
            "Unable to read string from snapshot"
        );
    }

    return value;
}

void PersistenceManager::close() {
    std::lock_guard<std::mutex> lock(log_mutex);

    if (append_only_log.is_open()) {
        append_only_log.flush();
        append_only_log.close();
    }

    initialized = false;
}

std::string PersistenceManager::snapshot_path() const {
    return snapshot_file;
}

std::string PersistenceManager::append_only_log_path() const {
    return append_only_log_file;
}

} // namespace koshdb