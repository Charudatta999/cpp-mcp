#pragma once

#include "mcp/transport/transport.hpp"
#include "mcp/core/errors.hpp"

#include <atomic>
#include <mutex>
#include <string>

#ifdef _WIN32
    #define MCP_HTTP_WINHTTP 1
#else
    #define MCP_HTTP_WINHTTP 0
#endif

#ifndef MCP_USE_CURL
    #define MCP_USE_CURL 0
#endif

namespace mcp {

/// HTTP client transport — sends JSON-RPC via HTTP POST.
/// Windows: WinHTTP. Unix: requires MCP_USE_CURL=1 + user libcurl.
class HttpClientTransport : public Transport {
public:
    explicit HttpClientTransport(std::string base_url);
    ~HttpClientTransport() override;

    HttpClientTransport(const HttpClientTransport&)            = delete;
    HttpClientTransport& operator=(const HttpClientTransport&) = delete;

    void start() override;
    void stop()  override;
    void send(const std::string& message) override;

    [[nodiscard]] bool is_running() const override {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::string post(const std::string& endpoint,
                                    const std::string& body);

    void set_session_id(const std::string& id) {
        std::lock_guard lock(mutex_);
        session_id_ = id;
    }

    [[nodiscard]] const std::string& endpoint() const { return endpoint_; }
    void set_endpoint(const std::string& ep) { endpoint_ = ep; }

private:
    std::string       base_url_;
    std::string       endpoint_{"/mcp"};
    std::string       session_id_;
    std::atomic<bool> running_{false};
    std::mutex        mutex_;

#if MCP_HTTP_WINHTTP
    void* session_{nullptr};
    void* connection_{nullptr};
#endif
};

} // namespace mcp
