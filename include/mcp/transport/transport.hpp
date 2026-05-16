#pragma once

#include <functional>
#include <string>

namespace mcp {

/// Callback invoked when a complete JSON-RPC message is received.
using MessageCallback = std::function<void(const std::string& message)>;

/// Abstract transport interface.
///
/// Transports carry JSON-RPC messages between client and server.
/// They are completely transport-agnostic — stdio, HTTP, SSE, etc.
class Transport {
public:
    virtual ~Transport() = default;

    /// Start the transport (e.g. begin reading stdin, start listening).
    virtual void start() = 0;

    /// Stop / close the transport gracefully.
    virtual void stop() = 0;

    /// Send a complete JSON-RPC message string.
    virtual void send(const std::string& message) = 0;

    /// Set the callback for incoming messages.
    void set_message_callback(MessageCallback cb) {
        on_message_ = std::move(cb);
    }

    /// Returns true if the transport is currently running.
    [[nodiscard]] virtual bool is_running() const = 0;

protected:
    MessageCallback on_message_;
};

} // namespace mcp
