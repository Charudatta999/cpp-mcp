#include "mcp/transport/http_transport.hpp"
#include "mcp/auth/auth.hpp"
#include "mcp/core/errors.hpp"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

// ═══════════════════════════════════════════════════════════════════════════
//  Platform-independent auth method implementations
//  (included once, before the first #if block)
// ═══════════════════════════════════════════════════════════════════════════

namespace mcp {

void HttpClientTransport::set_auth(auth::KeyConfig config) {
    std::lock_guard lock(mutex_);
    auth_provider_ = std::make_unique<auth::AuthProvider>(std::move(config));
}

void HttpClientTransport::set_auth_token(const std::string& token) {
    std::lock_guard lock(mutex_);
    auth_token_ = token;
    // Pre-encrypt so we don't re-encrypt on every request
    if (auth_provider_ && auth_provider_->is_enabled()) {
        auth_header_cache_ = auth_provider_->encrypt_for_header(token);
    }
}

bool HttpClientTransport::has_auth() const noexcept {
    // auth_provider_ read is safe: set_auth is called before start()
    return auth_provider_ && auth_provider_->is_enabled();
}

std::string HttpClientTransport::last_validated_token() const {
    std::lock_guard lock(const_cast<std::mutex&>(mutex_));
    return last_validated_;
}

} // namespace mcp

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

// [FIX #3] Wrap std::stoi in try/catch, validate port range 0-65535.
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
        try {
            int port_val = std::stoi(url.substr(colon + 1));
            if (port_val < 0 || port_val > 65535) {
                throw TransportError("Port number out of range (0-65535)");
            }
            parts.port = static_cast<INTERNET_PORT>(port_val);
        } catch (const std::invalid_argument&) {
            throw TransportError("Invalid port number in URL");
        } catch (const std::out_of_range&) {
            throw TransportError("Port number out of range in URL");
        }
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

// [FIX #5] Cache parsed URL parts during start(), reuse in post().
void HttpClientTransport::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;

    auto parts = parse_url(base_url_);

    // Cache for reuse in post()
    url_parts_.host    = parts.host;
    url_parts_.port    = parts.port;
    url_parts_.use_ssl = parts.use_ssl;

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

// [FIX #2] Guard connection_/session_ with mutex_ to prevent data race
// between concurrent stop() and post() calls.
void HttpClientTransport::stop() {
    running_.store(false, std::memory_order_release);

    void* conn = nullptr;
    void* sess = nullptr;
    {
        std::lock_guard lock(mutex_);
        conn = connection_;
        sess = session_;
        connection_ = nullptr;
        session_ = nullptr;
    }
    if (conn) WinHttpCloseHandle(static_cast<HINTERNET>(conn));
    if (sess) WinHttpCloseHandle(static_cast<HINTERNET>(sess));
}

void HttpClientTransport::send(const std::string& message) {
    // Fire-and-forget for notifications; response ignored
    (void)post(endpoint_, message);
}

// [FIX #2] Snapshot connection_ under lock before using it.
// [FIX #5] Use cached url_parts_ instead of re-parsing base_url_.
std::string HttpClientTransport::post(const std::string& endpoint,
                                       const std::string& body) {
    void* conn_snapshot = nullptr;
    {
        std::lock_guard lock(mutex_);
        conn_snapshot = connection_;
    }
    if (!conn_snapshot) {
        throw TransportError("HTTP transport not started");
    }

    auto wpath = to_wide(endpoint);
    DWORD flags = url_parts_.use_ssl ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET request = WinHttpOpenRequest(
        static_cast<HINTERNET>(conn_snapshot),
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
        if (!auth_header_cache_.empty()) {
            headers += L"Mcp-Auth-Token: " + to_wide(sanitize_header_value(auth_header_cache_)) + L"\r\n";
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

// [FIX #1] CurlInit RAII guard -- owns curl_global_init/cleanup lifecycle.
// Caller creates exactly one instance on the stack in main().
static bool g_curl_initialized = false;

CurlInit::CurlInit() {
    if (g_curl_initialized) {
        throw TransportError(
            "Only one mcp::CurlInit instance may exist at a time");
    }
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK) {
        throw TransportError(
            std::string("curl_global_init failed: ") +
            curl_easy_strerror(rc));
    }
    g_curl_initialized = true;
}

CurlInit::~CurlInit() {
    curl_global_cleanup();
    g_curl_initialized = false;
}

bool CurlInit::is_initialized() noexcept {
    return g_curl_initialized;
}

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

// [FIX #1] start() checks CurlInit guard instead of calling curl_global_init.
void HttpClientTransport::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;

    if (!CurlInit::is_initialized()) {
        running_ = false;
        throw TransportError(
            "No mcp::CurlInit instance is alive. "
            "Create one on the stack in main() before starting transports.");
    }
}

// [FIX #1] stop() no longer calls curl_global_cleanup -- CurlInit owns that.
void HttpClientTransport::stop() {
    running_.store(false, std::memory_order_release);
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
        if (!auth_header_cache_.empty()) {
            std::string h2 = "Mcp-Auth-Token: " + sanitize_header_value(auth_header_cache_);
            headers = curl_slist_append(headers, h2.c_str());
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

// No HTTP backend available -- stub that always throws
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
