#pragma once

#include <stdexcept>
#include <string>

namespace mcp {

// ── Standard JSON-RPC 2.0 error codes ────────────────────────────────────────

enum class ErrorCode : int {
    // JSON-RPC standard
    ParseError     = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams  = -32602,
    InternalError  = -32603,

    // MCP-specific range: -32000 to -32099
    ServerNotInitialized = -32002,
    RequestCancelled     = -32800,
};

// ── Exception hierarchy ──────────────────────────────────────────────────────

class McpError : public std::runtime_error {
public:
    explicit McpError(const std::string& msg)
        : std::runtime_error(msg) {}
};

class JsonRpcError : public McpError {
public:
    JsonRpcError(ErrorCode code, const std::string& msg)
        : McpError(msg), code_(code) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

class TransportError : public McpError {
public:
    explicit TransportError(const std::string& msg)
        : McpError(msg) {}
};

class ProtocolError : public McpError {
public:
    explicit ProtocolError(const std::string& msg)
        : McpError(msg) {}
};

} // namespace mcp
