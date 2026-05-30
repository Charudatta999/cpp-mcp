cmake_minimum_required(VERSION 3.20)

cmake_policy(SET CMP0091 NEW)

project(cpp-mcp
    VERSION 1.0.0
    LANGUAGES CXX
)

option(MCP_BUILD_EXAMPLES "Build example programs" ON)
option(MCP_BUILD_GATEWAY "Build the config-driven API gateway executable" ON)
option(MCP_BUILD_TESTS "Build the test suite" ON)
option(MCP_BUILD_BENCHMARKS "Build benchmark smoke executables" ON)
option(MCP_USE_CURL "Enable libcurl HTTP backend on non-Windows platforms" OFF)
option(MCP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(MCP_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer where supported" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

function(mcp_apply_project_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive-)
        if(MCP_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
        )
        if(MCP_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE -Werror)
        endif()
    endif()
endfunction()

function(mcp_apply_sanitizers target_name)
    if(MCP_ENABLE_SANITIZERS AND NOT MSVC)
        target_compile_options(${target_name} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target_name} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

add_library(rapidjson INTERFACE)
target_include_directories(rapidjson INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rapidjson/include
)

# ── Third-party static libraries (Linux: OpenSSL + curl from third_party/) ───
set(MCP_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(MCP_CURL_LIBS "")

# OpenSSL (Linux only — Windows uses BCrypt)
if(NOT WIN32)
    set(MCP_OPENSSL_DIR "${MCP_THIRD_PARTY_DIR}/openssl")
    if(EXISTS "${MCP_OPENSSL_DIR}/lib/libcrypto.a")
        message(STATUS "Using vendored OpenSSL from ${MCP_OPENSSL_DIR}")
        add_library(mcp_openssl_crypto STATIC IMPORTED)
        set_target_properties(mcp_openssl_crypto PROPERTIES
            IMPORTED_LOCATION "${MCP_OPENSSL_DIR}/lib/libcrypto.a"
            INTERFACE_INCLUDE_DIRECTORIES "${MCP_OPENSSL_DIR}/include"
            INTERFACE_LINK_LIBRARIES "${CMAKE_DL_LIBS}"
        )
        add_library(mcp_openssl_ssl STATIC IMPORTED)
        set_target_properties(mcp_openssl_ssl PROPERTIES
            IMPORTED_LOCATION "${MCP_OPENSSL_DIR}/lib/libssl.a"
            INTERFACE_INCLUDE_DIRECTORIES "${MCP_OPENSSL_DIR}/include"
            INTERFACE_LINK_LIBRARIES "mcp_openssl_crypto"
        )
        set(MCP_OPENSSL_TARGETS mcp_openssl_ssl mcp_openssl_crypto)
    else()
        message(STATUS "Vendored OpenSSL not found, falling back to system")
        find_package(OpenSSL REQUIRED)
        set(MCP_OPENSSL_TARGETS OpenSSL::SSL OpenSSL::Crypto)
    endif()
endif()

# libcurl (Linux only — Windows uses WinHTTP)
if(NOT WIN32)
    set(MCP_CURL_DIR "${MCP_THIRD_PARTY_DIR}/curl")
    if(EXISTS "${MCP_CURL_DIR}/lib/libcurl.a")
        message(STATUS "Using vendored curl from ${MCP_CURL_DIR}")
        add_library(mcp_curl STATIC IMPORTED)
        set_target_properties(mcp_curl PROPERTIES
            IMPORTED_LOCATION "${MCP_CURL_DIR}/lib/libcurl.a"
            INTERFACE_INCLUDE_DIRECTORIES "${MCP_CURL_DIR}/include"
            INTERFACE_LINK_LIBRARIES "mcp_openssl_ssl;mcp_openssl_crypto;${CMAKE_DL_LIBS}"
        )
        set(MCP_CURL_TARGET mcp_curl)
        # Force MCP_USE_CURL since we have the vendored lib
        set(MCP_USE_CURL ON CACHE BOOL "" FORCE)
    elseif(MCP_USE_CURL)
        message(STATUS "Vendored curl not found, falling back to system")
        find_package(CURL REQUIRED)
        set(MCP_CURL_TARGET CURL::libcurl)
    endif()
endif()

# ── Core library ────────────────────────────────────────────────────────────

add_library(mcp STATIC
    src/transport/stdio_transport.cpp
    src/transport/http_transport.cpp
    src/transport/http_server_transport.cpp
    src/server/server.cpp
    src/client/client.cpp
    src/auth/auth.cpp
)

target_compile_features(mcp PUBLIC cxx_std_20)
target_include_directories(mcp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(mcp PUBLIC rapidjson)

if(WIN32)
    target_compile_definitions(mcp PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(mcp PRIVATE winhttp ws2_32 bcrypt)
else()
    # PUBLIC: static lib consumers need the full transitive chain
    # (curl → ssl → crypto → dl) on their link line.
    target_link_libraries(mcp PUBLIC ${MCP_OPENSSL_TARGETS})
endif()

if(MCP_USE_CURL)
    target_compile_definitions(mcp PUBLIC MCP_USE_CURL=1)
    # PUBLIC: mcp_curl's INTERFACE_LINK_LIBRARIES already pulls in
    # openssl_ssl → openssl_crypto → dl, so a single PUBLIC link suffices.
    target_link_libraries(mcp PUBLIC ${MCP_CURL_TARGET})
    list(APPEND MCP_CURL_LIBS ${MCP_CURL_TARGET})
endif()

find_package(Threads REQUIRED)
target_link_libraries(mcp PRIVATE Threads::Threads)

mcp_apply_project_warnings(mcp)
mcp_apply_sanitizers(mcp)

if(MCP_BUILD_EXAMPLES)
    add_executable(example_server examples/example_server.cpp)
    target_link_libraries(example_server PRIVATE mcp)
    mcp_apply_project_warnings(example_server)
    mcp_apply_sanitizers(example_server)

    add_executable(example_calculator examples/example_calculator.cpp)
    target_link_libraries(example_calculator PRIVATE mcp)
    mcp_apply_project_warnings(example_calculator)
    mcp_apply_sanitizers(example_calculator)

    if(WIN32)
        add_executable(github_server examples/github_server.cpp)
        target_link_libraries(github_server PRIVATE mcp)
        target_link_libraries(github_server PRIVATE winhttp)
        mcp_apply_project_warnings(github_server)
        mcp_apply_sanitizers(github_server)
    endif()
endif()

if(MCP_BUILD_GATEWAY)
    add_executable(api_gateway src/gateway/api_gateway.cpp)
    target_link_libraries(api_gateway PRIVATE mcp)
    if(WIN32)
        target_link_libraries(api_gateway PRIVATE winhttp)
    endif()
    mcp_apply_project_warnings(api_gateway)
    mcp_apply_sanitizers(api_gateway)
endif()

if(MCP_BUILD_TESTS)
    enable_testing()

    add_executable(cpp_mcp_tests tests/cpp_mcp_tests.cpp)
    target_link_libraries(cpp_mcp_tests PRIVATE mcp)
    mcp_apply_project_warnings(cpp_mcp_tests)
    mcp_apply_sanitizers(cpp_mcp_tests)
    add_test(NAME cpp_mcp_tests COMMAND cpp_mcp_tests)

    add_executable(auth_tests tests/auth_tests.cpp)
    target_link_libraries(auth_tests PRIVATE mcp)
    mcp_apply_project_warnings(auth_tests)
    mcp_apply_sanitizers(auth_tests)
    add_test(NAME auth_tests COMMAND auth_tests)
endif()

if(MCP_BUILD_BENCHMARKS)
    add_executable(benchmark_smoke benchmarks/benchmark_smoke.cpp)
    target_link_libraries(benchmark_smoke PRIVATE mcp)
    mcp_apply_project_warnings(benchmark_smoke)
    mcp_apply_sanitizers(benchmark_smoke)
endif()
