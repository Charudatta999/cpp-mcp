/// api_gateway.cpp — Generic, config-driven MCP server.
///
/// Reads a JSON config file that defines REST API endpoints,
/// dynamically creates MCP tools, and proxies requests.
///
/// Usage:
///   api_gateway.exe <config.json>
///   api_gateway.exe github.json
///   api_gateway.exe jira.json
///
/// The SAME binary works for any REST API — GitHub, Jira, Slack,
/// Notion, GitLab, etc. Just write a JSON config file.

#include "mcp/mcp.hpp"
#include "mcp/gateway/api_gateway.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define MCP_GATEWAY_WINHTTP 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#define MCP_GATEWAY_WINHTTP 0
#endif

// ── WinHTTP helpers ──────────────────────────────────────────────────────────

/// Maximum HTTP response size to prevent OOM (CWE-400).
static constexpr size_t kMaxResponseSize = 50 * 1024 * 1024; // 50 MB
static constexpr int kHttpResolveTimeoutMs = 10000;
static constexpr int kHttpConnectTimeoutMs = 10000;
static constexpr int kHttpSendTimeoutMs = 30000;
static constexpr int kHttpReceiveTimeoutMs = 30000;

static std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

static bool is_blocked_ipv4_literal(const std::string& host) {
    int octets[4] = {0, 0, 0, 0};
    size_t start = 0;
    for (int i = 0; i < 4; ++i) {
        const size_t end = (i == 3) ? host.size() : host.find('.', start);
        if (end == std::string::npos || end == start) {
            return false;
        }
        int value = 0;
        for (size_t j = start; j < end; ++j) {
            const unsigned char ch = static_cast<unsigned char>(host[j]);
            if (!std::isdigit(ch)) {
                return false;
            }
            value = (value * 10) + (host[j] - '0');
            if (value > 255) {
                return false;
            }
        }
        octets[i] = value;
        start = end + 1;
    }
    if (start <= host.size()) return false;

    return octets[0] == 0 ||
           octets[0] == 10 ||
           octets[0] == 127 ||
           (octets[0] == 169 && octets[1] == 254) ||
           (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) ||
           (octets[0] == 192 && octets[1] == 168);
}

static bool is_blocked_host(const std::string& raw_host) {
    std::string host = lower_ascii(raw_host);
    if (!host.empty() && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }

    return host == "localhost" ||
           host == "::1" ||
           host.rfind("127.", 0) == 0 ||
           host.rfind("fc", 0) == 0 ||
           host.rfind("fd", 0) == 0 ||
           host.rfind("fe80", 0) == 0 ||
           is_blocked_ipv4_literal(host);
}

static void validate_gateway_base_url(const std::string& base_url) {
    if (base_url.rfind("https://", 0) != 0) {
        throw std::runtime_error("API gateway base_url must use https://");
    }

    std::string rest = base_url.substr(8);
    const auto slash = rest.find('/');
    std::string host_port = slash == std::string::npos ? rest : rest.substr(0, slash);
    if (host_port.empty()) {
        throw std::runtime_error("API gateway base_url is missing a host");
    }

    std::string host = host_port;
    if (!host.empty() && host.front() == '[') {
        const auto bracket = host.find(']');
        if (bracket == std::string::npos) {
            throw std::runtime_error("Invalid IPv6 host in base_url");
        }
        host = host.substr(0, bracket + 1);
    } else {
        const auto colon = host.find(':');
        if (colon != std::string::npos) {
            host = host.substr(0, colon);
        }
    }

    if (is_blocked_host(host)) {
        throw std::runtime_error("API gateway base_url targets a local or private host");
    }
}

#if MCP_GATEWAY_WINHTTP

static std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                   static_cast<int>(s.size()), nullptr, 0);
    std::wstring ws(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(),
                        static_cast<int>(s.size()), ws.data(), len);
    return ws;
}

struct UrlParts {
    std::wstring host;
    INTERNET_PORT port{INTERNET_DEFAULT_HTTPS_PORT};
    bool use_ssl{true};
    std::string path_prefix; // any path from base_url
};

static UrlParts parse_base_url(const std::string& url) {
    validate_gateway_base_url(url);

    UrlParts parts;
    std::string rest = url;

    if (rest.rfind("https://", 0) == 0) {
        parts.use_ssl = true;
        rest = rest.substr(8);
        parts.port = INTERNET_DEFAULT_HTTPS_PORT;
    } else if (rest.rfind("http://", 0) == 0) {
        parts.use_ssl = false;
        rest = rest.substr(7);
        parts.port = INTERNET_DEFAULT_HTTP_PORT;
    }

    // Split host[:port]/path
    auto slash = rest.find('/');
    std::string host_port;
    if (slash != std::string::npos) {
        host_port = rest.substr(0, slash);
        parts.path_prefix = rest.substr(slash);
    } else {
        host_port = rest;
    }

    // Remove trailing slash from path_prefix
    if (!parts.path_prefix.empty() && parts.path_prefix.back() == '/') {
        parts.path_prefix.pop_back();
    }

    auto colon = host_port.find(':');
    if (colon != std::string::npos) {
        parts.host = to_wide(host_port.substr(0, colon));
        // [FIX #3] Wrap std::stoi in try/catch, validate port range.
        try {
            int port_val = std::stoi(host_port.substr(colon + 1));
            if (port_val < 0 || port_val > 65535) {
                throw std::runtime_error("Port number out of range (0-65535)");
            }
            parts.port = static_cast<INTERNET_PORT>(port_val);
        } catch (const std::invalid_argument&) {
            throw std::runtime_error("Invalid port number in base_url");
        } catch (const std::out_of_range&) {
            throw std::runtime_error("Port number out of range in base_url");
        }
    } else {
        parts.host = to_wide(host_port);
    }

    return parts;
}

/// Generic HTTPS request with security hardening.
static std::string http_request(const std::string& base_url,
                                 const std::string& method,
                                 const std::string& path,
                                 const std::string& body,
                                 const std::unordered_map<std::string, std::string>& headers) {
    auto parts = parse_base_url(base_url);
    std::string full_path = parts.path_prefix + path;

    HINTERNET session = WinHttpOpen(
        L"cpp-mcp-gateway/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) throw std::runtime_error("WinHttpOpen failed");

    if (!WinHttpSetTimeouts(session,
                            kHttpResolveTimeoutMs,
                            kHttpConnectTimeoutMs,
                            kHttpSendTimeoutMs,
                            kHttpReceiveTimeoutMs)) {
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpSetTimeouts failed");
    }

    // [C3] Enforce TLS 1.2+ to prevent protocol downgrade (CWE-295)
    if (parts.use_ssl) {
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS,
                              &protocols, sizeof(protocols))) {
            WinHttpCloseHandle(session);
            throw std::runtime_error("WinHttpSetOption TLS policy failed");
        }
    }

    HINTERNET connection = WinHttpConnect(
        session, parts.host.c_str(), parts.port, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpConnect failed");
    }

    auto wmethod = to_wide(method);
    auto wpath   = to_wide(full_path);
    DWORD flags  = parts.use_ssl ? WINHTTP_FLAG_SECURE : 0;

    HINTERNET request = WinHttpOpenRequest(
        connection, wmethod.c_str(), wpath.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpOpenRequest failed");
    }

    // Assemble headers
    std::wstring header_str;
    for (const auto& [key, value] : headers) {
        header_str += to_wide(key) + L": " + to_wide(value) + L"\r\n";
    }

    if (!header_str.empty()) {
        if (!WinHttpAddRequestHeaders(request, header_str.c_str(),
                                      static_cast<DWORD>(header_str.size()),
                                      WINHTTP_ADDREQ_FLAG_ADD)) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            throw std::runtime_error("WinHttpAddRequestHeaders failed");
        }
    }

    // [L3] Use LPVOID cast instead of const_cast (CWE-704)
    BOOL ok;
    if (body.empty()) {
        ok = WinHttpSendRequest(request,
            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    } else {
        if (body.size() > (std::numeric_limits<DWORD>::max)()) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            throw std::runtime_error("Request body too large");
        }
        std::wstring ct = L"Content-Type: application/json\r\n";
        ok = WinHttpSendRequest(request,
            ct.c_str(), static_cast<DWORD>(ct.size()),
            reinterpret_cast<LPVOID>(const_cast<char*>(body.data())),
            static_cast<DWORD>(body.size()),
            static_cast<DWORD>(body.size()), 0);
    }

    if (!ok) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        throw std::runtime_error("HTTP request failed (error " +
                                  std::to_string(err) + ")");
    }

    ok = WinHttpReceiveResponse(request, nullptr);
    if (!ok) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        throw std::runtime_error("HTTP response failed");
    }

    // [C3] Check HTTP status code
    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status_code, &status_size, WINHTTP_NO_HEADER_INDEX)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpQueryHeaders failed");
    }

    // [M3] Read response with size limit (CWE-400)
    std::string response;
    char buf[4096];
    DWORD bytes_read = 0;
    while (WinHttpReadData(request, buf, sizeof(buf), &bytes_read)) {
        if (bytes_read == 0) break;
        if (response.size() + bytes_read > kMaxResponseSize) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            throw std::runtime_error("Response too large (>50MB)");
        }
        response.append(buf, bytes_read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    // Include status code in error responses for debugging
    if (status_code >= 400) {
        std::string prefix = "HTTP " + std::to_string(status_code) + ": ";
        response.insert(0, prefix);
    }

    return response;
}

#elif MCP_USE_CURL

#include <curl/curl.h>

static size_t curl_write_cb(char* data, size_t size, size_t nmemb, void* user_data) {
    auto* out = static_cast<std::string*>(user_data);
    const size_t total = size * nmemb;
    if (out->size() + total > kMaxResponseSize) {
        return 0;
    }
    out->append(data, total);
    return total;
}

static std::string http_request(const std::string& base_url,
                                 const std::string& method,
                                 const std::string& path,
                                 const std::string& body,
                                 const std::unordered_map<std::string, std::string>& headers) {
    validate_gateway_base_url(base_url);

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        throw std::runtime_error("curl_easy_init failed");
    }

    std::string response;
    const std::string url = base_url + path;

    struct curl_slist* header_list = nullptr;
    for (const auto& [key, value] : headers) {
        const std::string header = key + ": " + value;
        header_list = curl_slist_append(header_list, header.c_str());
    }
    header_list = curl_slist_append(header_list, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(kHttpConnectTimeoutMs));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(kHttpReceiveTimeoutMs));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif

    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }

    const CURLcode rc = curl_easy_perform(curl);
    long status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("curl error: ") + curl_easy_strerror(rc));
    }

    if (status_code >= 400) {
        response.insert(0, "HTTP " + std::to_string(status_code) + ": ");
    }

    return response;
}

#else

static std::string http_request(const std::string&,
                                 const std::string&,
                                 const std::string&,
                                 const std::string&,
                                 const std::unordered_map<std::string, std::string>&) {
    throw std::runtime_error(
        "API gateway HTTP backend is unavailable on this platform. "
        "Configure with -DMCP_USE_CURL=ON on Linux.");
}

#endif

// ── Auth header builder ──────────────────────────────────────────────────────

static void apply_auth(const mcp::AuthConfig& auth,
                        std::unordered_map<std::string, std::string>& headers) {
    switch (auth.type) {
    case mcp::AuthType::Bearer: {
        auto token = mcp::get_env(auth.token_env);
        if (!token.empty()) {
            mcp::set_validated_header(headers, "Authorization", "Bearer " + token);
        }
        break;
    }
    case mcp::AuthType::Basic: {
        auto user = mcp::get_env(auth.username_env);
        auto pass = mcp::get_env(auth.password_env);
        if (!user.empty()) {
            // Simple base64 — enough for auth
            // (We'll use a minimal inline encoder)
            std::string credentials = user + ":" + pass;
            static const char b64[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string encoded;
            int i = 0;
            unsigned char c3[3];
            unsigned char c4[4];
            int len = static_cast<int>(credentials.size());
            const unsigned char* bytes =
                reinterpret_cast<const unsigned char*>(credentials.data());

            while (len--) {
                c3[i++] = *(bytes++);
                if (i == 3) {
                    c4[0] = static_cast<unsigned char>((c3[0] & 0xfc) >> 2);
                    c4[1] = static_cast<unsigned char>(((c3[0] & 0x03) << 4) | ((c3[1] & 0xf0) >> 4));
                    c4[2] = static_cast<unsigned char>(((c3[1] & 0x0f) << 2) | ((c3[2] & 0xc0) >> 6));
                    c4[3] = static_cast<unsigned char>(c3[2] & 0x3f);
                    for (i = 0; i < 4; i++) encoded += b64[c4[i]];
                    i = 0;
                }
            }
            if (i) {
                for (int j = i; j < 3; j++) c3[j] = '\0';
                c4[0] = static_cast<unsigned char>((c3[0] & 0xfc) >> 2);
                c4[1] = static_cast<unsigned char>(((c3[0] & 0x03) << 4) | ((c3[1] & 0xf0) >> 4));
                c4[2] = static_cast<unsigned char>(((c3[1] & 0x0f) << 2) | ((c3[2] & 0xc0) >> 6));
                for (int j = 0; j < i + 1; j++) encoded += b64[c4[j]];
                while (i++ < 3) encoded += '=';
            }
            mcp::set_validated_header(headers, "Authorization", "Basic " + encoded);
        }
        break;
    }
    case mcp::AuthType::ApiKey: {
        auto key = mcp::get_env(auth.key_env);
        if (!key.empty() && !auth.header_name.empty()) {
            mcp::set_validated_header(headers, auth.header_name, key);
        }
        break;
    }
    case mcp::AuthType::None:
        break;
    }
}

// ── Request builder ──────────────────────────────────────────────────────────

struct PreparedRequest {
    std::string method;
    std::string path;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
};

static PreparedRequest build_request(
    const mcp::ApiGatewayConfig& config,
    const mcp::ApiEndpoint& endpoint,
    const mcp::ParamMap& args) {

    PreparedRequest req;
    req.method = endpoint.method;

    for (const auto& [k, v] : config.default_headers) {
        mcp::set_validated_header(req.headers, k, v);
    }

    // Merge endpoint-specific headers
    for (const auto& [k, v] : endpoint.extra_headers) {
        mcp::set_validated_header(req.headers, k, v);
    }

    // Apply auth
    apply_auth(config.auth, req.headers);

    // Collect params by location
    std::unordered_map<std::string, std::string> path_vars;
    std::vector<std::pair<std::string, std::string>> query_params;
    rapidjson::Document body_doc(rapidjson::kObjectType);
    auto& alloc = body_doc.GetAllocator();

    for (const auto& param : endpoint.parameters) {
        // Get value from args, or default
        std::string value;
        auto it = args.find(param.name);
        if (it != args.end() && !it->second.empty()) {
            value = it->second;
        } else if (!param.default_value.empty()) {
            value = param.default_value;
        } else if (param.required) {
            throw std::runtime_error("Missing required parameter: " + param.name);
        } else {
            continue; // optional, not provided, no default
        }

        switch (param.location) {
        case mcp::ParamLocation::Path:
            // [C1] Sanitize path params to prevent SSRF (CWE-918)
            path_vars[param.name] = mcp::sanitize_path_param(value);
            break;
        case mcp::ParamLocation::Query:
            query_params.emplace_back(param.name, value);
            break;
        case mcp::ParamLocation::Body:
            body_doc.AddMember(
                rapidjson::Value(param.name.c_str(), alloc),
                rapidjson::Value(value.c_str(), alloc), alloc);
            break;
        case mcp::ParamLocation::Header:
            // [C2] Sanitize header values to prevent CRLF injection (CWE-113)
            mcp::set_validated_header(req.headers, param.name, value);
            break;
        }
    }

    // Render path template
    req.path = mcp::render_template(endpoint.path, path_vars);

    // Append query params
    if (!query_params.empty()) {
        req.path += "?";
        bool first = true;
        for (const auto& [k, v] : query_params) {
            if (!first) req.path += "&";
            req.path += mcp::url_encode(k) + "=" + mcp::url_encode(v);
            first = false;
        }
    }

    // Serialize body (only for methods that take a body)
    if (body_doc.MemberCount() > 0) {
        req.body = mcp::json::stringify(body_doc);
    }

    return req;
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: api_gateway.exe <config.json>\n";
        std::cerr << "  Reads API definitions from JSON and creates MCP tools.\n";
        return 1;
    }

    std::string config_path = argv[1];

    // Load config
    mcp::ApiGatewayConfig config;
    try {
        config = mcp::load_api_config(config_path);
    } catch (const std::exception& e) {
        std::cerr << "Error loading config: " << e.what() << "\n";
        return 1;
    }

    // Create server
    auto transport = std::make_unique<mcp::StdioTransport>();
    mcp::McpServer server(
        mcp::Implementation{config.server_name, config.server_version},
        std::move(transport)
    );

    // Register each endpoint as an MCP tool
    for (const auto& endpoint : config.endpoints) {
        auto tool_def = mcp::endpoint_to_tool_def(endpoint);

        // Capture endpoint and config by value for the lambda
        auto ep_copy = endpoint;
        auto cfg_copy_ptr = std::make_shared<mcp::ApiGatewayConfig>(config);

        server.add_tool(tool_def,
            [ep_copy, cfg_copy_ptr](const mcp::ParamMap& args) -> mcp::ToolResult {
                try {
                    auto req = build_request(*cfg_copy_ptr, ep_copy, args);

                    std::string base = ep_copy.base_url_override.empty()
                        ? cfg_copy_ptr->base_url
                        : ep_copy.base_url_override;

                    auto response = http_request(
                        base, req.method, req.path, req.body, req.headers);

                    mcp::ToolResult result;
                    result.content.push_back(mcp::TextContent{response});
                    return result;
                } catch (const std::exception& e) {
                    // [H3] Don't leak internal error details to client (CWE-209)
                    std::cerr << "[api_gateway] Request error: "
                              << e.what() << "\n";
                    mcp::ToolResult result;
                    result.content.push_back(mcp::TextContent{
                        "API request failed. Check server logs for details."});
                    result.is_error = true;
                    return result;
                }
            });
    }

    std::cerr << "[api_gateway] Loaded " << config.endpoints.size()
              << " tools from: " << config_path << "\n";

    server.run();
    return 0;
}
