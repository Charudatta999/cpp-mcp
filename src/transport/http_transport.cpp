#include "mcp/transport/http_transport.hpp"
#include "mcp/core/errors.hpp"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

// ═══════════════════════════════════════════════════════════════════════════
//  WinHTTP backend (Windows-only, no external deps)
// ═══════════════════════════════════════════════════════════════════════════
#if MCP_HTTP_WINHTTP

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <algorithm>
#include <vector>

namespace mcp {

// ── Helpers ──────────────────────────────────────────────────────────────────

namespace {

constexpr size_t kMaxHttpResponseSize = 50 * 1024 * 1024;
constexpr int kHttpResolveTimeoutMs = 10000;
constexpr int kHttpConnectTimeoutMs = 10000;
constexpr int kHttpSendTimeoutMs = 30000;
constexpr int kHttpReceiveTimeoutMs = 30000;

std::string sanitize_header_value(const std::string& val) {
    std::string safe;
    safe.reserve(val.size());
    for (char c : val) {
        if (c != '\r' && c != '\n' && c != '\0') {
            safe += c;
        }
    }
    return safe;
}

} // namespace

static std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                   static_cast<int>(s.size()), nullptr, 0);
    std::wstring ws(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(),
                        static_cast<int>(s.size()), ws.data(), len);
    return ws;
}

/// Parse "http://host:port" into components.
struct UrlParts {
    std::wstring host;
    INTERNET_PORT port{INTERNET_DEFAULT_HTTP_PORT};
    bool          use_ssl{false};
};

static UrlParts parse_url(const std::string& base_url) {
    UrlParts parts;

    std::string url = base_url;
    if (url.rfind("https://", 0) == 0) {
        parts.use_ssl = true;
        url = url.substr(8);
        parts.port = INTERNET_DEFAULT_HTTPS_PORT;
    } else if (url.rfind("http://", 0) == 0) {
        url = url.substr(7);
    }

    // Remove trailing slash
    if (!url.empty() && url.back() == '/') url.pop_back();

    // Split host:port
    auto colon = url.find(':');
    if (colon != std::string::npos) {
        parts.host = to_wide(url.substr(0, colon));
        parts.port = static_cast<INTERNET_PORT>(
            std::stoi(url.substr(colon + 1)));
    } else {
        parts.host = to_wide(url);
    }

    return parts;
}

// ── HttpClientTransport implementation ────────────────────────────────────

HttpClientTransport::HttpClientTransport(std::string base_url)
    : base_url_(std::move(base_url)) {}

HttpClientTransport::~HttpClientTransport() {
    stop();
}

void HttpClientTransport::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;

    auto parts = parse_url(base_url_);

    session_ = WinHttpOpen(L"cpp-mcp/1.0",
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_) {
        running_ = false;
        throw TransportError("WinHttpOpen failed");
    }

    if (!WinHttpSetTimeouts(static_cast<HINTERNET>(session_),
                            kHttpResolveTimeoutMs,
                            kHttpConnectTimeoutMs,
                            kHttpSendTimeoutMs,
                            kHttpReceiveTimeoutMs)) {
        WinHttpCloseHandle(static_cast<HINTERNET>(session_));
        session_ = nullptr;
        running_ = false;
        throw TransportError("WinHttpSetTimeouts failed");
    }

    if (parts.use_ssl) {
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        if (!WinHttpSetOption(static_cast<HINTERNET>(session_),
                              WINHTTP_OPTION_SECURE_PROTOCOLS,
                              &protocols,
                              sizeof(protocols))) {
            WinHttpCloseHandle(static_cast<HINTERNET>(session_));
            session_ = nullptr;
            running_ = false;
            throw TransportError("WinHttpSetOption TLS policy failed");
        }
    }

    connection_ = WinHttpConnect(
        static_cast<HINTERNET>(session_),
        parts.host.c_str(), parts.port, 0);
    if (!connection_) {
        WinHttpCloseHandle(static_cast<HINTERNET>(session_));
        session_ = nullptr;
        running_ = false;
        throw TransportError("WinHttpConnect failed");
    }
}

void HttpClientTransport::stop() {
    running_ = false;
    if (connection_) {
        WinHttpCloseHandle(static_cast<HINTERNET>(connection_));
        connection_ = nullptr;
    }
    if (session_) {
        WinHttpCloseHandle(static_cast<HINTERNET>(session_));
        session_ = nullptr;
    }
}

void HttpClientTransport::send(const std::string& message) {
    // Fire-and-forget for notifications; response ignored
    (void)post(endpoint_, message);
}

std::string HttpClientTransport::post(const std::string& endpoint,
                                       const std::string& body) {
    if (!connection_) {
        throw TransportError("HTTP transport not started");
    }

    auto parts = parse_url(base_url_);
    auto wpath = to_wide(endpoint);

    DWORD flags = parts.use_ssl ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET request = WinHttpOpenRequest(
        static_cast<HINTERNET>(connection_),
        L"POST", wpath.c_str(), nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);
    if (!request) {
        throw TransportError("WinHttpOpenRequest failed");
    }

    // Set headers
    std::wstring headers = L"Content-Type: application/json\r\n";
    {
        std::lock_guard lock(mutex_);
        if (!session_id_.empty()) {
            headers += L"Mcp-Session-Id: " + to_wide(sanitize_header_value(session_id_)) + L"\r\n";
        }
    }

    if (!WinHttpAddRequestHeaders(request, headers.c_str(),
                                  static_cast<DWORD>(headers.size()),
                                  WINHTTP_ADDREQ_FLAG_ADD)) {
        WinHttpCloseHandle(request);
        throw TransportError("WinHttpAddRequestHeaders failed");
    }

    if (body.size() > (std::numeric_limits<DWORD>::max)()) {
        WinHttpCloseHandle(request);
        throw TransportError("HTTP request body too large");
    }

    BOOL ok = WinHttpSendRequest(
        request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()), 0);

    if (!ok) {
        WinHttpCloseHandle(request);
        throw TransportError("WinHttpSendRequest failed");
    }

    ok = WinHttpReceiveResponse(request, nullptr);
    if (!ok) {
        WinHttpCloseHandle(request);
        throw TransportError("WinHttpReceiveResponse failed");
    }

    // Read response
    std::string response;
    std::vector<char> buf(4096);
    DWORD bytes_read = 0;
    while (WinHttpReadData(request, buf.data(),
                            static_cast<DWORD>(buf.size()), &bytes_read)) {
        if (bytes_read == 0) break;
        if (response.size() + bytes_read > kMaxHttpResponseSize) {
            WinHttpCloseHandle(request);
            throw TransportError("HTTP response too large");
        }
        response.append(buf.data(), bytes_read);
    }

    WinHttpCloseHandle(request);
    return response;
}

} // namespace mcp

// ═══════════════════════════════════════════════════════════════════════════
//  libcurl backend (opt-in via MCP_USE_CURL=1)
// ═══════════════════════════════════════════════════════════════════════════
#elif MCP_USE_CURL

#include <curl/curl.h>

namespace mcp {

namespace {

constexpr size_t kMaxHttpResponseSize = 50 * 1024 * 1024;
constexpr long kHttpConnectTimeoutMs = 10000;
constexpr long kHttpReceiveTimeoutMs = 30000;

std::string sanitize_header_value(const std::string& val) {
    std::string safe;
    safe.reserve(val.size());
    for (char c : val) {
        if (c != '\r' && c != '\n' && c != '\0') {
            safe += c;
        }
    }
    return safe;
}

} // namespace

static size_t curl_write_cb(char* data, size_t size, size_t nmemb, void* ud) {
    auto* out = static_cast<std::string*>(ud);
    size_t total = size * nmemb;
    if (out->size() + total > kMaxHttpResponseSize) {
        return 0;
    }
    out->append(data, total);
    return total;
}

HttpClientTransport::HttpClientTransport(std::string base_url)
    : base_url_(std::move(base_url)) {}

HttpClientTransport::~HttpClientTransport() { stop(); }

void HttpClientTransport::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void HttpClientTransport::stop() {
    if (running_.exchange(false)) {
        curl_global_cleanup();
    }
}

void HttpClientTransport::send(const std::string& message) {
    (void)post(endpoint_, message);
}

std::string HttpClientTransport::post(const std::string& endpoint,
                                       const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) throw TransportError("curl_easy_init failed");

    std::string url = base_url_ + endpoint;
    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kHttpConnectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kHttpReceiveTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    {
        std::lock_guard lock(mutex_);
        if (!session_id_.empty()) {
            std::string h = "Mcp-Session-Id: " + sanitize_header_value(session_id_);
            headers = curl_slist_append(headers, h.c_str());
        }
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw TransportError(std::string("curl error: ") +
                             curl_easy_strerror(res));
    }
    return response;
}

} // namespace mcp

// ═══════════════════════════════════════════════════════════════════════════
//  No HTTP backend available — stub that always throws
// ═══════════════════════════════════════════════════════════════════════════
#else

namespace mcp {

HttpClientTransport::HttpClientTransport(std::string base_url)
    : base_url_(std::move(base_url)) {}
HttpClientTransport::~HttpClientTransport() = default;

void HttpClientTransport::start() {
    throw TransportError(
        "No HTTP backend available. "
        "On Windows, WinHTTP is used automatically. "
        "On Unix, define MCP_USE_CURL=1 and link libcurl.");
}
void HttpClientTransport::stop() {}
void HttpClientTransport::send(const std::string&) {
    throw TransportError("HTTP transport not available");
}
std::string HttpClientTransport::post(const std::string&,
                                       const std::string&) {
    throw TransportError("HTTP transport not available");
}

} // namespace mcp

#endif
