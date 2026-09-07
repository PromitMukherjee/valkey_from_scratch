#pragma once

#include "database.hpp"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

namespace koshdb {

class PersistenceManager {
public:
    explicit PersistenceManager(
        std::string data_directory = "data"
    );

    ~PersistenceManager();

    PersistenceManager(const PersistenceManager&) = delete;
    PersistenceManager& operator=(
        const PersistenceManager&
    ) = delete;

    void initialize();

    /*
     * Replay the snapshot first, then replay the AOF.
     */
    void recover(Database& database);

    /*
     * Append a mutating command to the AOF.
     */
    void append_command(
        const std::string& resp_command
    );

    /*
     * Write a complete snapshot to a temporary file and
     * atomically replace the active snapshot.
     */
    void save_snapshot(
        const Database& database
    );

    /*
     * Flush the append-only log to disk.
     */
    void flush();

    void close();

    std::string snapshot_path() const;
    std::string append_only_log_path() const;

private:
    std::string data_directory;
    std::string snapshot_file;
    std::string temporary_snapshot_file;
    std::string append_only_log_file;

    std::ofstream append_only_log;

    mutable std::mutex log_mutex;

    std::atomic<bool> initialized;

    void create_data_directory();

    void open_append_only_log();

    void recover_snapshot(
        Database& database
    );

    void recover_append_only_log(
        Database& database
    );

    void write_snapshot_file(
        const Database& database,
        const std::string& path
    );

    void atomically_replace_snapshot();

    void write_uint64(
        std::ofstream& output,
        std::uint64_t value
    );

    std::uint64_t read_uint64(
        std::ifstream& input
    );

    void write_string(
        std::ofstream& output,
        const std::string& value
    );

    std::string read_string(
        std::ifstream& input
    );
};

} // namespace koshdb