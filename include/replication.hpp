#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace koshdb {

enum class ReplicationRole {
    Primary,
    Replica
};

class ReplicationState {
public:
    ReplicationState();

    void set_role(ReplicationRole new_role);

    ReplicationRole role() const;

    bool is_primary() const;

    bool is_replica() const;

    void set_primary(
        const std::string& host,
        std::uint16_t port
    );

    std::string primary_host() const;

    std::uint16_t primary_port() const;

    std::uint64_t replication_offset() const;

    std::uint64_t advance_offset(
        std::uint64_t bytes
    );

    void set_replication_offset(
        std::uint64_t offset
    );

private:
    std::atomic<ReplicationRole> current_role;

    mutable std::mutex state_mutex;

    std::string primary_host_value;
    std::uint16_t primary_port_value;

    std::atomic<std::uint64_t> offset;
};

} // namespace koshdb