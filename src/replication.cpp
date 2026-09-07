#include "replication.hpp"

namespace koshdb {

ReplicationState::ReplicationState()
    : current_role(ReplicationRole::Primary),
      primary_host_value(""),
      primary_port_value(0),
      offset(0) {
}

void ReplicationState::set_role(
    ReplicationRole new_role
) {
    current_role.store(
        new_role,
        std::memory_order_release
    );
}

ReplicationRole ReplicationState::role() const {
    return current_role.load(
        std::memory_order_acquire
    );
}

bool ReplicationState::is_primary() const {
    return role() == ReplicationRole::Primary;
}

bool ReplicationState::is_replica() const {
    return role() == ReplicationRole::Replica;
}

void ReplicationState::set_primary(
    const std::string& host,
    std::uint16_t port
) {
    std::lock_guard<std::mutex> lock(state_mutex);

    primary_host_value = host;
    primary_port_value = port;
}

std::string ReplicationState::primary_host() const {
    std::lock_guard<std::mutex> lock(state_mutex);

    return primary_host_value;
}

std::uint16_t ReplicationState::primary_port() const {
    std::lock_guard<std::mutex> lock(state_mutex);

    return primary_port_value;
}

std::uint64_t ReplicationState::replication_offset() const {
    return offset.load(
        std::memory_order_acquire
    );
}

std::uint64_t ReplicationState::advance_offset(
    std::uint64_t bytes
) {
    return offset.fetch_add(
        bytes,
        std::memory_order_acq_rel
    ) + bytes;
}

void ReplicationState::set_replication_offset(
    std::uint64_t new_offset
) {
    offset.store(
        new_offset,
        std::memory_order_release
    );
}

} // namespace koshdb