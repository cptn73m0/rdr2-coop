cmake_minimum_required(VERSION 3.16)
project(rdr2_coop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT so the .asi runs without a matching VCRedist preset.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_library(rdr2_coop SHARED
    src/main.cpp
    src/script_hook.cpp
    src/coop.cpp
    src/log.cpp
    src/persist.cpp
    src/config.cpp
    src/world_scan.cpp
    src/world_sync.cpp
    src/actor_sync.cpp
    src/world_snapshot_parse.cpp
    src/world_entity_budget.cpp
    src/mission_barrier_rules.cpp
    net/client.cpp
    net/socket.cpp
    net/frame.cpp
)

target_include_directories(rdr2_coop PRIVATE .)

if(WIN32)
    # Winsock для сетевого клиента
    target_link_libraries(rdr2_coop PRIVATE ws2_32)
endif()

target_compile_definitions(rdr2_coop PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Русскоязычные строки в конфиге/логах: строковые литералы в UTF-8 (MSVC).
if(MSVC)
    target_compile_options(rdr2_coop PRIVATE /utf-8)
endif()

# The plugin resolves ScriptHookRDR2 exports at runtime, so no import lib
# is required. Set output extension to .asi for the game's ASI loader.
set_target_properties(rdr2_coop PROPERTIES
    SUFFIX ".asi"
    PREFIX ""
)