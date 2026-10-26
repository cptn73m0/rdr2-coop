cmake_minimum_required(VERSION 3.15)
project(rdr2_coop_tests CXX)

# Headless C++ tests for network layer.
# Build: cmake -S tests -B tests/build -A x64
#        cmake --build tests/build --config Release
add_executable(client_reconnect_test
    client_reconnect_test.cpp
    ../net/client.cpp
    ../net/frame.cpp
    ../net/socket.cpp
)
target_include_directories(client_reconnect_test PRIVATE ..)
target_compile_features(client_reconnect_test PRIVATE cxx_std_17)

add_executable(frame_reader_fuzz
    frame_reader_fuzz.cpp
    ../net/frame.cpp
    ../net/socket.cpp
)
target_include_directories(frame_reader_fuzz PRIVATE ..)
target_compile_features(frame_reader_fuzz PRIVATE cxx_std_17)

add_executable(config_test
    config_test.cpp
    ../src/config.cpp
    ../src/log.cpp
)
target_include_directories(config_test PRIVATE .. ../src)
target_compile_features(config_test PRIVATE cxx_std_17)

add_executable(world_snapshot_parse_test
    world_snapshot_parse_test.cpp
    ../src/world_snapshot_parse.cpp
)
target_include_directories(world_snapshot_parse_test PRIVATE .. ../src)
target_compile_features(world_snapshot_parse_test PRIVATE cxx_std_17)

add_executable(mission_catalog_test
    mission_catalog_test.cpp
)
target_include_directories(mission_catalog_test PRIVATE .. ../src)
target_compile_features(mission_catalog_test PRIVATE cxx_std_17)

add_executable(world_entity_budget_test
    world_entity_budget_test.cpp
    ../src/world_entity_budget.cpp
)
target_include_directories(world_entity_budget_test PRIVATE .. ../src)
target_compile_features(world_entity_budget_test PRIVATE cxx_std_17)

add_executable(mission_barrier_rules_fuzz
    mission_barrier_rules_fuzz.cpp
    ../src/mission_barrier_rules.cpp
)
target_include_directories(mission_barrier_rules_fuzz PRIVATE .. ../src)
target_compile_features(mission_barrier_rules_fuzz PRIVATE cxx_std_17)

if (WIN32)
    target_link_libraries(client_reconnect_test PRIVATE ws2_32)
    target_link_libraries(frame_reader_fuzz PRIVATE ws2_32)
endif()
