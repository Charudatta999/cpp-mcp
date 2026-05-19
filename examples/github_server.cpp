/// github_server.cpp — MCP server for GitHub API access.
///
/// Exposes GitHub REST API operations as MCP tools.
/// Uses WinHTTP directly on Windows (no curl needed).
///
/// Set your GitHub token via environment variable:
///   set GITHUB_TOKEN=ghp_xxxxxxxxxxxx
///
/// Then run this server via stdio (Claude Desktop, Cursor, etc.)

#include "mcp/mcp.hpp"
#include "mcp/gateway/api_gateway.hpp"

#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

// ── GitHub API helper ────────────────────────────────────────────────────────

namespace {

constexpr size_t kMaxResponseSize = 50ULL * 1024ULL * 1024ULL;
constexpr int kHttpResolveTimeoutMs = 10000;
constexpr int kHttpConnectTimeoutMs = 10000;
constexpr int kHttpSendTimeoutMs = 30000;
constexpr int kHttpReceiveTimeoutMs = 30000;

std::string encode_path_preserving_slashes(const std::string& value) {
    std::string encoded;
    std::string segment;
    for (char ch : value) {
        if (ch == '/') {
            encoded += mcp::url_encode(segment);
            encoded += '/';
            segment.clear();
        } else {
            segment += ch;
        }
    }
    encoded += mcp::url_encode(segment);
    return encoded;
}

std::string repo_path(const std::string& owner, const std::string& repo) {
    return "/repos/" + mcp::sanitize_path_param(owner) + "/" + mcp::sanitize_path_param(repo);
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

/// Make an HTTPS request to api.github.com.
/// method: "GET", "POST", "PATCH", "DELETE"
/// path:   e.g. "/repos/owner/repo/issues"
/// body:   JSON string for POST/PATCH, empty for GET
/// token:  GitHub PAT
static std::string github_api(const std::string& method,
                               const std::string& path,
                               const std::string& body,
                               const std::string& token) {
    HINTERNET session = WinHttpOpen(
        L"cpp-mcp-github/1.0",
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

    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS,
                          &protocols, sizeof(protocols))) {
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpSetOption TLS policy failed");
    }

    HINTERNET connection = WinHttpConnect(
        session, L"api.github.com",
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpConnect failed");
    }

    auto wpath   = to_wide(path);
    auto wmethod = to_wide(method);

    HINTERNET request = WinHttpOpenRequest(
        connection, wmethod.c_str(), wpath.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpOpenRequest failed");
    }

    // Set headers
    std::wstring headers =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n"
        L"User-Agent: cpp-mcp-github/1.0\r\n";

    if (!token.empty()) {
        headers += L"Authorization: Bearer " + to_wide(mcp::sanitize_header_value(token)) + L"\r\n";
    }

    if (!WinHttpAddRequestHeaders(request, headers.c_str(),
                                  static_cast<DWORD>(headers.size()),
                                  WINHTTP_ADDREQ_FLAG_ADD)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpAddRequestHeaders failed");
    }

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
            const_cast<char*>(body.data()),
            static_cast<DWORD>(body.size()),
            static_cast<DWORD>(body.size()), 0);
    }

    if (!ok) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpSendRequest failed");
    }

    ok = WinHttpReceiveResponse(request, nullptr);
    if (!ok) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpReceiveResponse failed");
    }

    // Read response body
    std::string response;
    char buf[4096];
    DWORD bytes_read = 0;
    while (WinHttpReadData(request, buf, sizeof(buf), &bytes_read)) {
        if (bytes_read == 0) break;
        if (response.size() + bytes_read > kMaxResponseSize) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            throw std::runtime_error("Response too large");
        }
        response.append(buf, bytes_read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return response;
}

// ── Convenience ──────────────────────────────────────────────────────────────

static std::string get_token() {
#ifdef _MSC_VER
    char* tok = nullptr;
    size_t len = 0;
    if (_dupenv_s(&tok, &len, "GITHUB_TOKEN") != 0 || tok == nullptr || len == 0) {
        throw std::runtime_error(
            "GITHUB_TOKEN environment variable not set. "
            "Create a token at https://github.com/settings/tokens");
    }
    std::string token(tok);
    free(tok);
    return token;
#else
    const char* tok = std::getenv("GITHUB_TOKEN");
    if (!tok || std::string(tok).empty()) {
        throw std::runtime_error(
            "GITHUB_TOKEN environment variable not set. "
            "Create a token at https://github.com/settings/tokens");
    }
    return tok;
#endif
}

static mcp::ToolResult text_result(const std::string& text) {
    mcp::ToolResult r;
    r.content.push_back(mcp::TextContent{text});
    return r;
}

static mcp::ToolResult error_result(const std::string& msg) {
    mcp::ToolResult r;
    r.content.push_back(mcp::TextContent{msg});
    r.is_error = true;
    return r;
}

static std::string require_arg(const mcp::ParamMap& args,
                                const std::string& key) {
    auto it = args.find(key);
    if (it == args.end() || it->second.empty()) {
        throw std::runtime_error("Missing required argument: " + key);
    }
    return it->second;
}

static std::string opt_arg(const mcp::ParamMap& args,
                            const std::string& key,
                            const std::string& def = "") {
    auto it = args.find(key);
    return (it != args.end()) ? it->second : def;
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    auto transport = std::make_unique<mcp::StdioTransport>();
    mcp::McpServer server(
        mcp::Implementation{"github-mcp-server", "1.0.0"},
        std::move(transport)
    );

    // ═══════════════════════════════════════════════════════════════════
    //  TOOL: list_repos — List repositories for a user or org
    // ═══════════════════════════════════════════════════════════════════
    {
        mcp::ToolDefinition def;
        def.name        = "list_repos";
        def.description = "List GitHub repositories for a user or the authenticated user";
        def.input_schema.properties_json = R"json({
            "owner": {
                "type": "string",
                "description": "GitHub username or org. Leave empty for authenticated user's repos."
            },
            "per_page": {
                "type": "string",
                "description": "Results per page, default 10, max 100"
            }
        })json";

        server.add_tool(def, [](const mcp::ParamMap& args) -> mcp::ToolResult {
            try {
                auto token = get_token();
                auto owner = opt_arg(args, "owner");
                auto per_page = opt_arg(args, "per_page", "10");

                std::string path;
                if (owner.empty()) {
                    path = "/user/repos?per_page=" + mcp::url_encode(per_page) + "&sort=updated";
                } else {
                    path = "/users/" + mcp::sanitize_path_param(owner) +
                           "/repos?per_page=" + mcp::url_encode(per_page) + "&sort=updated";
                }

                auto resp = github_api("GET", path, "", token);
                return text_result(resp);
            } catch (const std::exception& e) {
                return error_result(e.what());
            }
        });
    }

    // ═══════════════════════════════════════════════════════════════════
    //  TOOL: get_repo — Get detailed info about a repository
    // ═══════════════════════════════════════════════════════════════════
    {
        mcp::ToolDefinition def;
        def.name        = "get_repo";
        def.description = "Get detailed information about a GitHub repository";
        def.input_schema.properties_json = R"json({
            "owner": {"type": "string", "description": "Repository owner"},
            "repo":  {"type": "string", "description": "Repository name"}
        })json";
        def.input_schema.required = {"owner", "repo"};

        server.add_tool(def, [](const mcp::ParamMap& args) -> mcp::ToolResult {
            try {
                auto token = get_token();
                auto owner = require_arg(args, "owner");
                auto repo  = require_arg(args, "repo");
                auto resp = github_api("GET", repo_path(owner, repo), "", token);
                return text_result(resp);
            } catch (const std::exception& e) {
                return error_result(e.what());
            }
        });
    }

    // ═══════════════════════════════════════════════════════════════════
    //  TOOL: list_issues — List issues in a repository
    // ═══════════════════════════════════════════════════════════════════
    {
        mcp::ToolDefinition def;
        def.name        = "list_issues";
        def.description = "List issues in a GitHub repository";
        def.input_schema.properties_json = R"json({
            "owner":    {"type": "string", "description": "Repository owner"},
            "repo":     {"type": "string", "description": "Repository name"},
            "state":    {"type": "string", "description": "Filter by state: open, closed, all. Default: open"},
            "per_page": {"type": "string", "description": "Results per page, default 10"}
        })json";
        def.input_schema.required = {"owner", "repo"};

        server.add_tool(def, [](const mcp::ParamMap& args) -> mcp::ToolResult {
            try {
                auto token = get_token();
                auto owner = require_arg(args, "owner");
                auto repo  = require_arg(args, "repo");
                auto state = opt_arg(args, "state", "open");
                auto per_page = opt_arg(args, "per_page", "10");

                auto path = repo_path(owner, repo) +
                            "/issues?state=" + mcp::url_encode(state) +
                            "&per_page=" + mcp::url_encode(per_page);
                auto resp = github_api("GET", path, "", token);
                return text_result(resp);
            } catch (const std::exception& e) {
                return error_result(e.what());
            }
        });
    }

    // ═══════════════════════════════════════════════════════════════════
    //  TOOL: create_issue — Create a new issue
    // ═══════════════════════════════════════════════════════════════════
    {
        mcp::ToolDefinition def;
        def.name        = "create_issue";
        def.description = "Create a new issue in a GitHub repository";
        def.input_schema.properties_json = R"json({
            "owner": {"type": "string", "description": "Repository owner"},
            "repo":  {"type": "string", "description": "Repository name"},
            "title": {"type": "string", "description": "Issue title"},
            "body":  {"type": "string", "description": "Issue body/description"},
            "labels": {"type": "string", "description": "Comma-separated labels"}
        })json";
        def.input_schema.required = {"owner", "repo", "title"};

        server.add_tool(def, [](const mcp::ParamMap& args) -> mcp::ToolResult {
            try {
                auto token = get_token();
                auto owner = require_arg(args, "owner");
                auto repo  = require_arg(args, "repo");
                auto title = require_arg(args, "title");
                auto body  = opt_arg(args, "body");
                auto labels_str = opt_arg(args, "labels");

                // Build JSON body
                rapidjson::Document doc(rapidjson::kObjectType);
                auto& a = doc.GetAllocator();
                doc.AddMember("title", rapidjson::Value(title.c_str(), a), a);
                if (!body.empty()) {
                    doc.AddMember("body", rapidjson::Value(body.c_str(), a), a);
                }
                if (!labels_str.empty()) {
                    rapidjson::Value labels(rapidjson::kArrayType);
                    std::istringstream ss(labels_str);
                    std::string label;
                    while (std::getline(ss, label, ',')) {
                        // trim whitespace
                        auto start = label.find_first_not_of(" ");
                        auto end = label.find_last_not_of(" ");
                        if (start != std::string::npos) {
                            label = label.substr(start, end - start + 1);
                            labels.PushBack(rapidjson::Value(label.c_str(), a), a);
                        }
                    }
                    doc.AddMember("labels", labels, a);
                }

                auto json_body = mcp::json::stringify(doc);
                auto path = repo_path(owner, repo) + "/issues";
                auto resp = github_api("POST", path, json_body, token);
                return text_result(resp);
            } catch (const std::exception& e) {
                return error_result(e.what());
            }
        });
    }

    // ═══════════════════════════════════════════════════════════════════
    //  TOOL: list_pull_requests — List PRs in a repository
    // ═══════════════════════════════════════════════════════════════════
    {
        mcp::ToolDefinition def;
        def.name        = "list_pull_requests";
        def.description = "List pull requests in a GitHub repository";
        def.input_schema.properties_json = R"json({
            "owner":    {"type": "string", "description": "Repository owner"},
            "repo":     {"type": "string", "description": "Repository name"},
            "state":    {"type": "string", "description": "Filter: open, closed, all. Default: open"},
            "per_page": {"type": "string", "description": "Results per page, default 10"}
        })json";
        def.input_schema.required = {"owner", "repo"};

        server.add_tool(def, [](const mcp::ParamMap& args) -> mcp::ToolResult {
            try {
                auto token = get_token();
                auto owner = require_arg(args, "owner");
                auto repo  = require_arg(args, "repo");
                auto state = opt_arg(args, "state", "open");
                auto per_page = opt_arg(args, "per_page", "10");

                auto path = repo_path(owner, repo) +
                            "/pulls?state=" + mcp::url_encode(state) +
                            "&per_page=" + mcp::url_encode(per_page);
                auto resp = github_api("GET", path, "", token);
                return text_result(resp);
            } catch (const std::exception& e) {
                return error_result(e.what());
            }
        });
    }

    // ═══════════════════════════════════════════════════════════════════
    //  TOOL: search_repos — Search GitHub repositories
    // ═══════════════════════════════════════════════════════════════════
    {
        mcp::ToolDefinition def;
        def.name        = "search_repos";
        def.description = "Search GitHub repositories by keyword";
        def.input_schema.properties_json = R"json({
            "query":    {"type": "string", "description": "Search query"},
            "per_page": {"type": "string", "description": "Results per page, default 10"}
        })json";
        def.input_schema.required = {"query"};

        server.add_tool(def, [](const mcp::ParamMap& args) -> mcp::ToolResult {
            try {
                auto token = get_token();
                auto query = require_arg(args, "query");
                auto per_page = opt_arg(args, "per_page", "10");

                auto path = "/search/repositories?q=" + mcp::url_encode(query) +
                            "&per_page=" + mcp::url_encode(per_page);
                auto resp = github_api("GET", path, "", token);
                return text_result(resp);
            } catch (const std::exception& e) {
                return error_result(e.what());
            }
        });
    }

    // ═══════════════════════════════════════════════════════════════════
    //  TOOL: get_file_contents — Read a file from a repo
    // ═══════════════════════════════════════════════════════════════════
    {
        mcp::ToolDefinition def;
        def.name        = "get_file_contents";
        def.description = "Get the contents of a file from a GitHub repository";
        def.input_schema.properties_json = R"json({
            "owner": {"type": "string", "description": "Repository owner"},
            "repo":  {"type": "string", "description": "Repository name"},
            "path":  {"type": "string", "description": "File path in the repo"},
            "ref":   {"type": "string", "description": "Branch/tag/SHA. Default: default branch"}
        })json";
        def.input_schema.required = {"owner", "repo", "path"};

        server.add_tool(def, [](const mcp::ParamMap& args) -> mcp::ToolResult {
            try {
                auto token = get_token();
                auto owner = require_arg(args, "owner");
                auto repo  = require_arg(args, "repo");
                auto fpath = require_arg(args, "path");
                auto ref   = opt_arg(args, "ref");

                auto api_path = repo_path(owner, repo) + "/contents/" +
                                encode_path_preserving_slashes(fpath);
                if (!ref.empty()) {
                    api_path += "?ref=" + mcp::url_encode(ref);
                }

                auto resp = github_api("GET", api_path, "", token);
                return text_result(resp);
            } catch (const std::exception& e) {
                return error_result(e.what());
            }
        });
    }

    // ═══════════════════════════════════════════════════════════════════
    //  TOOL: get_user — Get GitHub user profile
    // ═══════════════════════════════════════════════════════════════════
    {
        mcp::ToolDefinition def;
        def.name        = "get_user";
        def.description = "Get a GitHub user's profile info, or the authenticated user if no username given";
        def.input_schema.properties_json = R"json({
            "username": {"type": "string", "description": "GitHub username. Leave empty for authenticated user."}
        })json";

        server.add_tool(def, [](const mcp::ParamMap& args) -> mcp::ToolResult {
            try {
                auto token = get_token();
                auto user = opt_arg(args, "username");
                auto path = user.empty() ? "/user" : "/users/" + mcp::sanitize_path_param(user);
                auto resp = github_api("GET", path, "", token);
                return text_result(resp);
            } catch (const std::exception& e) {
                return error_result(e.what());
            }
        });
    }

    server.run();
    return 0;
}
