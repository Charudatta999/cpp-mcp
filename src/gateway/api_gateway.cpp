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
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

// ── WinHTTP helpers ──────────────────────────────────────────────────────────

static std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                   static_cast<int>(s.size()), nullptr, 0);
    std::wstring ws(len, L'\0');
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
        parts.port = static_cast<INTERNET_PORT>(
            std::stoi(host_port.substr(colon + 1)));
    } else {
        parts.host = to_wide(host_port);
    }

    return parts;
}

/// Maximum HTTP response size to prevent OOM (CWE-400).
static constexpr size_t kMaxResponseSize = 50 * 1024 * 1024; // 50 MB

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

    // [C3] Enforce TLS 1.2+ to prevent protocol downgrade (CWE-295)
    if (parts.use_ssl) {
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS,
                         &protocols, sizeof(protocols));
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
        WinHttpAddRequestHeaders(request, header_str.c_str(),
                                  static_cast<DWORD>(header_str.size()),
                                  WINHTTP_ADDREQ_FLAG_ADD);
    }

    // [L3] Use LPVOID cast instead of const_cast (CWE-704)
    BOOL ok;
    if (body.empty()) {
        ok = WinHttpSendRequest(request,
            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    } else {
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
    WinHttpQueryHeaders(request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status_code, &status_size, WINHTTP_NO_HEADER_INDEX);

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

// ── Auth header builder ──────────────────────────────────────────────────────

static void apply_auth(const mcp::AuthConfig& auth,
                        std::unordered_map<std::string, std::string>& headers) {
    switch (auth.type) {
    case mcp::AuthType::Bearer: {
        auto token = mcp::get_env(auth.token_env);
        if (!token.empty()) {
            headers["Authorization"] = "Bearer " + token;
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
                    c4[0] = (c3[0] & 0xfc) >> 2;
                    c4[1] = ((c3[0] & 0x03) << 4) | ((c3[1] & 0xf0) >> 4);
                    c4[2] = ((c3[1] & 0x0f) << 2) | ((c3[2] & 0xc0) >> 6);
                    c4[3] = c3[2] & 0x3f;
                    for (i = 0; i < 4; i++) encoded += b64[c4[i]];
                    i = 0;
                }
            }
            if (i) {
                for (int j = i; j < 3; j++) c3[j] = '\0';
                c4[0] = (c3[0] & 0xfc) >> 2;
                c4[1] = ((c3[0] & 0x03) << 4) | ((c3[1] & 0xf0) >> 4);
                c4[2] = ((c3[1] & 0x0f) << 2) | ((c3[2] & 0xc0) >> 6);
                for (int j = 0; j < i + 1; j++) encoded += b64[c4[j]];
                while (i++ < 3) encoded += '=';
            }
            headers["Authorization"] = "Basic " + encoded;
        }
        break;
    }
    case mcp::AuthType::ApiKey: {
        auto key = mcp::get_env(auth.key_env);
        if (!key.empty() && !auth.header_name.empty()) {
            headers[auth.header_name] = key;
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

    // Start with default headers
    req.headers = config.default_headers;

    // Merge endpoint-specific headers
    for (const auto& [k, v] : endpoint.extra_headers) {
        req.headers[k] = v;
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
            req.headers[param.name] = mcp::sanitize_header_value(value);
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
