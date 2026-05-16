#include "mcp/transport/stdio_transport.hpp"
#include "mcp/core/errors.hpp"

#include <iostream>
#include <sstream>
#include <string>

namespace mcp {

StdioTransport::~StdioTransport() {
    stop();
}

void StdioTransport::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return; // already running
    }

    reader_thread_ = std::thread([this]() { read_loop(); });
}

void StdioTransport::stop() {
    running_.store(false, std::memory_order_release);

    // Close stdin to unblock getline — platform-specific nudge.
    // On most platforms the thread will exit when the parent closes the pipe.
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
}

void StdioTransport::send(const std::string& message) {
    std::lock_guard lock(write_mutex_);
    // MCP stdio protocol: one JSON message per line
    std::cout << message << "\n" << std::flush;
}

void StdioTransport::read_loop() {
    // [H1] Maximum line length to prevent OOM DoS (CWE-400)
    constexpr size_t kMaxLineLength = 10 * 1024 * 1024; // 10 MB

    std::string line;
    while (running_.load(std::memory_order_acquire)) {
        if (!std::getline(std::cin, line)) {
            // EOF — client closed the pipe
            running_.store(false, std::memory_order_release);
            break;
        }

        // Reject oversized messages
        if (line.size() > kMaxLineLength) {
            // Discard — too large to process safely
            line.clear();
            continue;
        }

        // Skip empty lines
        if (line.empty() || line.find_first_not_of(" \t\r\n") == std::string::npos) {
            continue;
        }

        // Strip trailing CR (Windows line endings)
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (on_message_) {
            on_message_(line);
        }
    }
}

} // namespace mcp
