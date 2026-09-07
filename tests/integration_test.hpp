#pragma once

#include <string>

namespace koshdb::test {

void run_all_tests();

void test_multiple_tcp_clients();

void test_authentication();

void test_tls_client();

void test_persistence_recovery();

void test_primary_replica_consistency();

} // namespace koshdb::test