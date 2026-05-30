#pragma once

#include "mcp/transport/transport.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace mcp {

struct HttpServerConfig {
    std::string bind_address{"127.0.0.1"};
    uint16_t    port{8080};
    size_t      max_request_size{10 * 1024 * 1024};  // 10 MB
    int         backlog{64};
    int         read_timeout_ms{30000};
};

/// HTTP server transport — accepts JSON-RPC over HTTP POST.
///
/// Listens on a TCP port, accepts connections, reads HTTP POST
/// requests to the configured endpoint, dispatches JSON-RPC messages
/// via the message callback, and sends the response back over HTTP.
///
/// Platform: uses Winsock2 on Windows, POSIX sockets on Unix.
class HttpServerTransport : public Transport {
public:
    explicit HttpServerTransport(HttpServerConfig config = {});
    ~HttpServerTransport() override;

    HttpServerTransport(const HttpServerTransport&)            = delete;
    HttpServerTransport& operator=(const HttpServerTransport&) = delete;

    void start() override;
    void stop()  override;
    void send(const std::string& message) override;

    [[nodiscard]] bool is_running() const override {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint16_t port() const { return config_.port; }

    /// Set the endpoint path for JSON-RPC requests (default: "/mcp").
    void set_endpoint(const std::string& ep) { endpoint_ = ep; }

    /// Set a synchronous message handler that returns a response string.
    /// This is used instead of the async message callback for request/response.
    using SyncHandler = std::function<std::string(const std::string& body)>;
    void set_sync_handler(SyncHandler handler) {
        sync_handler_ = std::move(handler);
    }

private:
    void accept_loop();
    void handle_connection(intptr_t client_fd);
    std::string build_http_response(int status, const std::string& status_text,
                                    const std::string& body);

    HttpServerConfig  config_;
    std::string       endpoint_{"/mcp"};
    std::atomic<bool> running_{false};
    intptr_t          listen_fd_{-1};
    std::thread       accept_thread_;
    std::mutex        write_mutex_;
    SyncHandler       sync_handler_;

    // Last response for async send() compatibility
    std::string       pending_response_;
    std::mutex        response_mutex_;
};

} // namespace mcp
