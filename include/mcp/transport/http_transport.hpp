#pragma once

#include "mcp/transport/transport.hpp"
#include "mcp/auth/auth.hpp"
#include "mcp/core/errors.hpp"

#include <atomic>
#include <memory>
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

#if MCP_USE_CURL
/// RAII guard for libcurl global lifecycle.
/// Create exactly ONE instance on the stack in main(), before any
/// HttpClientTransport is constructed.  Destruction calls
/// curl_global_cleanup after all transports are gone.
///
///   int main() {
///       mcp::CurlInit curl_guard;   // first line
///       // ... create transports ...
///   }
///
/// Non-copyable, non-movable -- the caller owns the entire lifetime.
class CurlInit {
public:
    CurlInit();
    ~CurlInit();
    CurlInit(const CurlInit&)            = delete;
    CurlInit& operator=(const CurlInit&) = delete;
    CurlInit(CurlInit&&)                 = delete;
    CurlInit& operator=(CurlInit&&)      = delete;

    /// Returns true if a CurlInit instance is alive in the current process.
    [[nodiscard]] static bool is_initialized() noexcept;
};
#endif

/// HTTP client transport -- sends JSON-RPC via HTTP POST.
/// Windows: WinHTTP. Unix: requires MCP_USE_CURL=1 + user libcurl.
///
/// On Unix (curl backend) the caller MUST hold a live CurlInit instance
/// for the entire lifetime of every HttpClientTransport.
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

    // -- Authentication ---------------------------------------------------
    /// Attach an auth provider for encrypted token authentication.
    void set_auth(auth::KeyConfig config);

    /// Set the plaintext token to encrypt for outgoing requests (client-side).
    void set_auth_token(const std::string& token);

    /// Check whether auth is configured and enabled.
    [[nodiscard]] bool has_auth() const noexcept;

    /// Get the decrypted auth token from the most recent validated request.
    [[nodiscard]] std::string last_validated_token() const;

private:
    std::string       base_url_;
    std::string       endpoint_{"/mcp"};
    std::string       session_id_;
    std::atomic<bool> running_{false};
    std::mutex        mutex_;

    // Auth state
    std::unique_ptr<auth::AuthProvider> auth_provider_;
    std::string                         auth_token_;
    std::string                         auth_header_cache_;
    std::string                         last_validated_;

#if MCP_HTTP_WINHTTP
    void* session_{nullptr};
    void* connection_{nullptr};

    struct CachedUrlParts {
        std::wstring   host;
        unsigned short port{80};
        bool           use_ssl{false};
    } url_parts_;
#endif
};

} // namespace mcp
