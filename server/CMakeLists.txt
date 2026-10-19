cmake_minimum_required(VERSION 3.16)
project(rdr2_coop_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Сервер — отдельное консольное приложение. Кроссплатформенное: соберётся
# и на Windows (Winsock), и на Linux (POSIX) одним и тем же кодом.

add_executable(rdr2_coop_server
    server.cpp
    ../src/mission_barrier_rules.cpp
    ../net/socket.cpp
    ../net/frame.cpp
)

target_compile_definitions(rdr2_coop_server PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Русский текст в консоли: строковые литералы в UTF-8 (MSVC).
if(MSVC)
    target_compile_options(rdr2_coop_server PRIVATE /utf-8)
endif()

target_include_directories(rdr2_coop_server PRIVATE .. ../src)

if(WIN32)
    # Winsock
    target_link_libraries(rdr2_coop_server PRIVATE ws2_32)
else()
    target_link_libraries(rdr2_coop_server PRIVATE pthread)
endif()