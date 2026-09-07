cmake_minimum_required(VERSION 3.16)

project(KoshDB VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(koshdb_core
    src/database.cpp
    src/resp.cpp
    src/command.cpp
    src/server.cpp
    src/auth.cpp
    src/persistence.cpp
    src/replication.cpp
    src/thread_pool.cpp
    src/tls_manager.cpp
)

target_include_directories(koshdb_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

target_link_libraries(koshdb_core
    PUBLIC
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
)

add_executable(koshdb-server
    src/main_server.cpp
)

target_link_libraries(koshdb-server
    PRIVATE
        koshdb_core
)

add_executable(koshdb-tests
    tests/test_database.cpp
)

target_link_libraries(koshdb-tests
    PRIVATE
        koshdb_core
)

add_executable(koshdb-resp-tests
    tests/test_resp.cpp
)

target_link_libraries(koshdb-resp-tests
    PRIVATE
        koshdb_core
)

add_executable(koshdb-command-tests
    tests/test_command.cpp
)

target_link_libraries(koshdb-command-tests
    PRIVATE
        koshdb_core
)

enable_testing()

add_test(
    NAME DatabaseTests
    COMMAND koshdb-tests
)

add_test(
    NAME RespTests
    COMMAND koshdb-resp-tests
)

add_test(
    NAME CommandTests
    COMMAND koshdb-command-tests
)
add_executable(koshdb-cli
    src/main_cli.cpp
)

target_link_libraries(koshdb-cli
    PRIVATE
    koshdb_core
)