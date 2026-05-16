cmake_minimum_required(VERSION 3.20)
project(cpp-mcp
    VERSION   1.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Static linking ───────────────────────────────────────────────────────────
# Use static CRT on MSVC / clang-cl (equivalent to /MT)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
cmake_policy(SET CMP0091 NEW)  # respect CMAKE_MSVC_RUNTIME_LIBRARY

# For Clang with GNU-like driver on Windows: force static libstdc++/CRT
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND WIN32)
    add_compile_options(-Xclang --dependent-lib=libcmt)
    add_link_options(-static)
endif()

# ── Options ──────────────────────────────────────────────────────────────────

option(MCP_USE_CURL      "Enable libcurl HTTP backend (provide your own build)" OFF)
option(MCP_BUILD_EXAMPLES "Build example programs"                               ON)

# ── RapidJSON (header-only) ──────────────────────────────────────────────────

add_library(rapidjson INTERFACE)
target_include_directories(rapidjson INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rapidjson/include
)

# ── Core library ─────────────────────────────────────────────────────────────

add_library(mcp STATIC
    src/transport/stdio_transport.cpp
    src/transport/http_transport.cpp
    src/server/server.cpp
    src/client/client.cpp
)

target_include_directories(mcp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(mcp PUBLIC rapidjson)

# Platform-specific linking
if(WIN32)
    target_link_libraries(mcp PRIVATE winhttp)
    target_compile_definitions(mcp PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

# Optional curl extension
if(MCP_USE_CURL)
    target_compile_definitions(mcp PUBLIC MCP_USE_CURL=1)

    # User is expected to provide curl include/lib paths via:
    #   -DCURL_INCLUDE_DIR=...  -DCURL_LIBRARY=...
    # or have it findable by CMake
    if(DEFINED CURL_INCLUDE_DIR AND DEFINED CURL_LIBRARY)
        target_include_directories(mcp PRIVATE ${CURL_INCLUDE_DIR})
        target_link_libraries(mcp PRIVATE ${CURL_LIBRARY})
    else()
        find_package(CURL REQUIRED)
        target_link_libraries(mcp PRIVATE CURL::libcurl)
    endif()
endif()

# Threads
find_package(Threads REQUIRED)
target_link_libraries(mcp PRIVATE Threads::Threads)

# ── Examples ─────────────────────────────────────────────────────────────────

if(MCP_BUILD_EXAMPLES)
    add_executable(example_server examples/example_server.cpp)
    target_link_libraries(example_server PRIVATE mcp)

    add_executable(example_calculator examples/example_calculator.cpp)
    target_link_libraries(example_calculator PRIVATE mcp)

    add_executable(github_server examples/github_server.cpp)
    target_link_libraries(github_server PRIVATE mcp)
    if(WIN32)
        target_link_libraries(github_server PRIVATE winhttp)
    endif()
endif()

# ── Generic API Gateway ──────────────────────────────────────────────────────
# One binary that reads any JSON config to create MCP tools dynamically.
# Usage: api_gateway.exe <config.json>

add_executable(api_gateway src/gateway/api_gateway.cpp)
target_link_libraries(api_gateway PRIVATE mcp)
if(WIN32)
    target_link_libraries(api_gateway PRIVATE winhttp)
endif()
