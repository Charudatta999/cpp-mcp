#pragma once

#include "mcp/transport/transport.hpp"

#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace mcp {

/// Stdio transport for JSON-RPC over stdin/stdout.
///
/// This is the primary transport for MCP servers invoked by clients
/// as child processes (e.g. Claude Desktop, Cursor, etc.).
///
/// Protocol: newline-delimited JSON on stdin, writes to stdout.
class StdioTransport : public Transport {
public:
    StdioTransport() = default;
    ~StdioTransport() override;

    // Non-copyable, non-movable
    StdioTransport(const StdioTransport&)            = delete;
    StdioTransport& operator=(const StdioTransport&) = delete;

    void start() override;
    void stop()  override;
    void send(const std::string& message) override;

    [[nodiscard]] bool is_running() const override {
        return running_.load(std::memory_order_acquire);
    }

private:
    void read_loop();

    std::atomic<bool> running_{false};
    std::thread       reader_thread_;
    std::mutex        write_mutex_;
};

} // namespace mcp
