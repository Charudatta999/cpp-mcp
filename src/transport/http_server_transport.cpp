#include "mcp/transport/http_server_transport.hpp"
#include "mcp/core/errors.hpp"
#include "mcp/core/log.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;

namespace {

struct WinsockInit {
    WinsockInit() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinsockInit() { WSACleanup(); }
};

static WinsockInit& ensure_winsock() {
    static WinsockInit init;
    return init;
}

void close_socket(socket_t fd) {
    if (fd != kInvalidSocket) closesocket(fd);
}

} // namespace

#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using socket_t = int;
constexpr socket_t kInvalidSocket = -1;

namespace {

void close_socket(socket_t fd) {
    if (fd != kInvalidSocket) ::close(fd);
}

} // namespace
#endif

// Platform-aware length type for recv/send
#ifdef _WIN32
using socklen_param_t = int;
#else
using socklen_param_t = size_t;
#endif

namespace {

void send_all(socket_t fd, const std::string& data) {
    ::send(fd, data.data(), static_cast<socklen_param_t>(data.size()), 0);
}

} // namespace

namespace mcp {

namespace {

constexpr size_t kMaxHeaderSize = 8192;

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
    size_t      content_length{0};
    bool        valid{false};
};

HttpRequest parse_http_request(const std::string& raw, size_t max_body) {
    HttpRequest req;

    auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return req;

    std::string header_section = raw.substr(0, header_end);

    // Parse request line
    auto first_line_end = header_section.find("\r\n");
    std::string request_line = header_section.substr(0, first_line_end);

    auto sp1 = request_line.find(' ');
    auto sp2 = request_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return req;

    req.method = request_line.substr(0, sp1);
    req.path   = request_line.substr(sp1 + 1, sp2 - sp1 - 1);

    // Find Content-Length
    std::string cl_header = "content-length:";
    std::string lower_headers = header_section;
    std::transform(lower_headers.begin(), lower_headers.end(),
                   lower_headers.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto cl_pos = lower_headers.find(cl_header);
    if (cl_pos != std::string::npos) {
        auto val_start = cl_pos + cl_header.size();
        auto val_end = lower_headers.find("\r\n", val_start);
        std::string val = header_section.substr(val_start,
                                                 val_end - val_start);
        // Trim whitespace
        auto nws = val.find_first_not_of(" \t");
        if (nws != std::string::npos) val = val.substr(nws);

        try {
            auto len = std::stoull(val);
            if (len > max_body) return req;
            req.content_length = static_cast<size_t>(len);
        } catch (...) {
            return req;
        }
    }

    size_t body_start = header_end + 4;
    if (body_start + req.content_length <= raw.size()) {
        req.body = raw.substr(body_start, req.content_length);
        req.valid = true;
    }

    return req;
}

bool set_recv_timeout(socket_t fd, int timeout_ms) {
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(timeout_ms);
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                      reinterpret_cast<const char*>(&tv), sizeof(tv)) == 0;
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

} // namespace

HttpServerTransport::HttpServerTransport(HttpServerConfig config)
    : config_(std::move(config)) {}

HttpServerTransport::~HttpServerTransport() {
    stop();
}

void HttpServerTransport::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;

#ifdef _WIN32
    ensure_winsock();
#endif

    auto fd = static_cast<socket_t>(
        socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (fd == kInvalidSocket) {
        running_ = false;
        throw TransportError("Failed to create server socket");
    }

    // Allow port reuse
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);

    if (inet_pton(AF_INET, config_.bind_address.c_str(),
                  &addr.sin_addr) != 1) {
        close_socket(fd);
        running_ = false;
        throw TransportError("Invalid bind address: " + config_.bind_address);
    }

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) != 0) {
        close_socket(fd);
        running_ = false;
        throw TransportError("Failed to bind on port " +
                             std::to_string(config_.port));
    }

    if (listen(fd, config_.backlog) != 0) {
        close_socket(fd);
        running_ = false;
        throw TransportError("Failed to listen on port " +
                             std::to_string(config_.port));
    }

    listen_fd_ = static_cast<intptr_t>(fd);

    MCP_INFO("HTTP server listening on " + config_.bind_address + ":" +
             std::to_string(config_.port));

    accept_thread_ = std::thread([this]() { accept_loop(); });
}

void HttpServerTransport::stop() {
    running_.store(false, std::memory_order_release);

    if (listen_fd_ != -1) {
        close_socket(static_cast<socket_t>(listen_fd_));
        listen_fd_ = -1;
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    MCP_DEBUG("HTTP server stopped");
}

void HttpServerTransport::send(const std::string& message) {
    std::lock_guard lock(response_mutex_);
    pending_response_ = message;
}

void HttpServerTransport::accept_loop() {
    while (running_.load(std::memory_order_acquire)) {
        struct sockaddr_in client_addr{};
#ifdef _WIN32
        int addr_len = sizeof(client_addr);
#else
        socklen_t addr_len = sizeof(client_addr);
#endif
        auto client_fd = accept(
            static_cast<socket_t>(listen_fd_),
            reinterpret_cast<struct sockaddr*>(&client_addr),
            &addr_len);

        if (client_fd == kInvalidSocket) {
            if (!running_.load(std::memory_order_acquire)) break;
            continue;
        }

        // Handle synchronously — one connection at a time for simplicity
        // and to match the JSON-RPC request/response model.
        handle_connection(static_cast<intptr_t>(client_fd));
    }
}

void HttpServerTransport::handle_connection(intptr_t client_fd) {
    auto fd = static_cast<socket_t>(client_fd);
    set_recv_timeout(fd, config_.read_timeout_ms);

    std::string raw_request;
    raw_request.reserve(4096);
    std::vector<char> buf(4096);

    // Read until we have full headers + body
    size_t max_total = kMaxHeaderSize + config_.max_request_size;
    bool headers_complete = false;
    size_t content_length = 0;
    size_t header_end_pos = 0;

    while (raw_request.size() < max_total) {
        auto n = recv(fd, buf.data(), static_cast<socklen_param_t>(buf.size()), 0);
        if (n <= 0) break;
        raw_request.append(buf.data(), static_cast<size_t>(n));

        if (!headers_complete) {
            auto pos = raw_request.find("\r\n\r\n");
            if (pos != std::string::npos) {
                headers_complete = true;
                header_end_pos = pos + 4;

                // Parse Content-Length from headers
                auto http_req = parse_http_request(raw_request,
                                                   config_.max_request_size);
                content_length = http_req.content_length;
            }
        }

        if (headers_complete &&
            raw_request.size() >= header_end_pos + content_length) {
            break;
        }
    }

    auto req = parse_http_request(raw_request, config_.max_request_size);

    if (!req.valid) {
        auto resp = build_http_response(400, "Bad Request",
                                        "{\"error\":\"Invalid HTTP request\"}");
        send_all(fd, resp);
        close_socket(fd);
        return;
    }

    if (req.method != "POST") {
        auto resp = build_http_response(405, "Method Not Allowed",
                                        "{\"error\":\"Only POST is supported\"}");
        send_all(fd, resp);
        close_socket(fd);
        return;
    }

    if (req.path != endpoint_) {
        auto resp = build_http_response(404, "Not Found",
                                        "{\"error\":\"Not found\"}");
        send_all(fd, resp);
        close_socket(fd);
        return;
    }

    std::string response_body;

    if (sync_handler_) {
        response_body = sync_handler_(req.body);
    } else if (on_message_) {
        // Use the async callback path — dispatch and wait for send()
        {
            std::lock_guard lock(response_mutex_);
            pending_response_.clear();
        }

        on_message_(req.body);

        {
            std::lock_guard lock(response_mutex_);
            response_body = pending_response_;
        }
    }

    auto resp = build_http_response(200, "OK", response_body);
    send_all(fd, resp);
    close_socket(fd);
}

std::string HttpServerTransport::build_http_response(
    int status, const std::string& status_text, const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " " << status_text << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;
    return oss.str();
}

} // namespace mcp
