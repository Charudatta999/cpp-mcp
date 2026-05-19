#include "mcp/transport/stdio_transport.hpp"

#include <cstdio>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace mcp {

namespace {

constexpr size_t kMaxLineLength = 10ULL * 1024ULL * 1024ULL;

void close_stdin_for_stop() noexcept {
#ifdef _WIN32
    const int fd = _fileno(stdin);
    if (fd >= 0) {
        (void)_close(fd);
    }
#else
    (void)::close(STDIN_FILENO);
#endif
}

} // namespace

StdioTransport::~StdioTransport() {
    stop();
}

void StdioTransport::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    reader_thread_ = std::thread([this]() { read_loop(); });
}

void StdioTransport::stop() {
    running_.store(false, std::memory_order_release);

    if (reader_thread_.joinable()) {
        close_stdin_for_stop();
        reader_thread_.join();
    }
}

void StdioTransport::send(const std::string& message) {
    std::lock_guard lock(write_mutex_);
    std::cout << message << "\n" << std::flush;
}

void StdioTransport::read_loop() {
    std::string line;
    line.reserve(4096);
    bool discarding_oversized_line = false;

    while (running_.load(std::memory_order_acquire)) {
        const int next = std::cin.get();
        if (next == EOF) {
            running_.store(false, std::memory_order_release);
            break;
        }

        const char ch = static_cast<char>(next);
        if (ch == '\n') {
            if (discarding_oversized_line) {
                discarding_oversized_line = false;
                line.clear();
                continue;
            }

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (line.empty() || line.find_first_not_of(" \t\r\n") == std::string::npos) {
                line.clear();
                continue;
            }

            if (on_message_) {
                on_message_(line);
            }
            line.clear();
            continue;
        }

        if (discarding_oversized_line) {
            continue;
        }

        if (line.size() >= kMaxLineLength) {
            discarding_oversized_line = true;
            line.clear();
            continue;
        }

        line.push_back(ch);
    }
}

} // namespace mcp
