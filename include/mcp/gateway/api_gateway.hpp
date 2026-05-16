#pragma once

/// Generic API Gateway — reads tool definitions from a JSON config file
/// at runtime, dynamically creates MCP tools that call any REST API.
///
/// No recompilation needed. Just edit the JSON config to add new APIs.

#include "mcp/core/types.hpp"
#include "mcp/core/json_utils.hpp"
#include "mcp/core/errors.hpp"

#include "rapidjson/document.h"

#include <cstdlib>
#include <functional>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace mcp {

// ── Config types ─────────────────────────────────────────────────────────────

enum class ParamLocation {
    Path,    // Substituted into URL path via {{name}}
    Query,   // Appended as ?key=value
    Body,    // Assembled into JSON request body
    Header,  // Sent as an HTTP header
};

struct ApiParam {
    std::string   name;
    std::string   type{"string"};     // JSON schema type
    std::string   description;
    bool          required{false};
    ParamLocation location{ParamLocation::Query};
    std::string   default_value;       // used if param not provided
};

enum class AuthType {
    None,
    Bearer,     // Authorization: Bearer <token>
    Basic,      // Authorization: Basic <base64(user:pass)>
    ApiKey,     // Custom header with key
};

struct AuthConfig {
    AuthType    type{AuthType::None};
    std::string token_env;       // env var name for bearer token
    std::string username_env;    // env var for basic auth user
    std::string password_env;    // env var for basic auth password
    std::string header_name;     // header name for api key auth
    std::string key_env;         // env var for api key
};

struct ApiEndpoint {
    std::string              name;
    std::string              description;
    std::string              method{"GET"};  // GET, POST, PUT, PATCH, DELETE
    std::string              path;           // e.g. "/users/{{username}}/repos"
    std::vector<ApiParam>    parameters;

    // Per-endpoint overrides (empty = use defaults)
    std::string              base_url_override;
    std::unordered_map<std::string, std::string> extra_headers;
};

struct ApiGatewayConfig {
    // Server identity
    std::string server_name{"api-gateway"};
    std::string server_version{"1.0.0"};

    // Default connection settings
    std::string base_url;  // e.g. "https://api.github.com"
    std::unordered_map<std::string, std::string> default_headers;
    AuthConfig  auth;

    // Tools
    std::vector<ApiEndpoint> endpoints;
};

// ── Config parser ────────────────────────────────────────────────────────────

inline ParamLocation parse_location(const std::string& s) {
    if (s == "path")   return ParamLocation::Path;
    if (s == "query")  return ParamLocation::Query;
    if (s == "body")   return ParamLocation::Body;
    if (s == "header") return ParamLocation::Header;
    return ParamLocation::Query; // default
}

inline AuthType parse_auth_type(const std::string& s) {
    if (s == "bearer")  return AuthType::Bearer;
    if (s == "basic")   return AuthType::Basic;
    if (s == "api_key") return AuthType::ApiKey;
    return AuthType::None;
}

/// Load and parse an API gateway config from a JSON file.
inline ApiGatewayConfig load_api_config(const std::string& filepath) {
    // Read file
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw McpError("Cannot open config file: " + filepath);
    }
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    file.close();

    auto doc = json::parse(content);
    ApiGatewayConfig config;

    // Server info
    if (doc.HasMember("server") && doc["server"].IsObject()) {
        const auto& s = doc["server"];
        if (s.HasMember("name"))    config.server_name    = s["name"].GetString();
        if (s.HasMember("version")) config.server_version = s["version"].GetString();
    }

    // Defaults
    if (doc.HasMember("defaults") && doc["defaults"].IsObject()) {
        const auto& d = doc["defaults"];

        if (d.HasMember("base_url"))
            config.base_url = d["base_url"].GetString();

        if (d.HasMember("headers") && d["headers"].IsObject()) {
            for (auto it = d["headers"].MemberBegin();
                 it != d["headers"].MemberEnd(); ++it) {
                config.default_headers[it->name.GetString()] =
                    it->value.GetString();
            }
        }

        if (d.HasMember("auth") && d["auth"].IsObject()) {
            const auto& a = d["auth"];
            if (a.HasMember("type"))
                config.auth.type = parse_auth_type(a["type"].GetString());
            if (a.HasMember("token_env"))
                config.auth.token_env = a["token_env"].GetString();
            if (a.HasMember("username_env"))
                config.auth.username_env = a["username_env"].GetString();
            if (a.HasMember("password_env"))
                config.auth.password_env = a["password_env"].GetString();
            if (a.HasMember("header_name"))
                config.auth.header_name = a["header_name"].GetString();
            if (a.HasMember("key_env"))
                config.auth.key_env = a["key_env"].GetString();
        }
    }

    // Tools / endpoints
    if (doc.HasMember("tools") && doc["tools"].IsArray()) {
        for (const auto& t : doc["tools"].GetArray()) {
            ApiEndpoint ep;

            if (t.HasMember("name"))        ep.name        = t["name"].GetString();
            if (t.HasMember("description")) ep.description = t["description"].GetString();
            if (t.HasMember("method"))      ep.method      = t["method"].GetString();
            if (t.HasMember("path"))        ep.path        = t["path"].GetString();
            if (t.HasMember("base_url"))    ep.base_url_override = t["base_url"].GetString();

            if (t.HasMember("headers") && t["headers"].IsObject()) {
                for (auto it = t["headers"].MemberBegin();
                     it != t["headers"].MemberEnd(); ++it) {
                    ep.extra_headers[it->name.GetString()] =
                        it->value.GetString();
                }
            }

            if (t.HasMember("parameters") && t["parameters"].IsArray()) {
                for (const auto& p : t["parameters"].GetArray()) {
                    ApiParam param;
                    if (p.HasMember("name"))        param.name        = p["name"].GetString();
                    if (p.HasMember("type"))         param.type        = p["type"].GetString();
                    if (p.HasMember("description"))  param.description = p["description"].GetString();
                    if (p.HasMember("required"))     param.required    = p["required"].GetBool();
                    if (p.HasMember("location"))     param.location    = parse_location(p["location"].GetString());
                    if (p.HasMember("default"))      param.default_value = p["default"].GetString();
                    ep.parameters.push_back(std::move(param));
                }
            }

            config.endpoints.push_back(std::move(ep));
        }
    }

    return config;
}

// ── Security utilities ───────────────────────────────────────────────────────

/// Sanitize a value destined for URL path substitution.
/// Blocks path traversal (CWE-918) by rejecting separators and control chars.
inline std::string sanitize_path_param(const std::string& value) {
    std::string safe;
    safe.reserve(value.size());
    for (unsigned char c : value) {
        if (c == '/' || c == '\\' || c == '?' || c == '#' ||
            c == '\0' || c == '\r' || c == '\n') {
            safe += '_';
        } else {
            safe += static_cast<char>(c);
        }
    }
    return safe;
}

/// Strip CRLF from header values to prevent header injection (CWE-113).
inline std::string sanitize_header_value(const std::string& val) {
    std::string safe;
    safe.reserve(val.size());
    for (char c : val) {
        if (c != '\r' && c != '\n' && c != '\0') safe += c;
    }
    return safe;
}

// ── Template engine ──────────────────────────────────────────────────────────

/// Replace all {{key}} placeholders in a template string.
/// Path-param values MUST be pre-sanitized before calling this.
inline std::string render_template(const std::string& tmpl,
                                    const std::unordered_map<std::string, std::string>& vars) {
    std::string result = tmpl;
    for (const auto& [key, value] : vars) {
        std::string placeholder = "{{" + key + "}}";
        size_t pos = 0;
        while ((pos = result.find(placeholder, pos)) != std::string::npos) {
            result.replace(pos, placeholder.length(), value);
            pos += value.length();
        }
    }
    return result;
}

/// RFC 3986 percent-encoding with correct 2-digit hex (fixes CWE-116).
inline std::string url_encode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            result += static_cast<char>(c);
        } else {
            result += '%';
            result += hex[(c >> 4) & 0x0F];
            result += hex[c & 0x0F];
        }
    }
    return result;
}

// ── Env var helper ───────────────────────────────────────────────────────────

/// Thread-safe environment variable lookup (fixes CWE-676 on MSVC).
inline std::string get_env(const std::string& name) {
#ifdef _MSC_VER
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name.c_str()) == 0 && buf) {
        std::string val(buf);
        free(buf);
        return val;
    }
    return {};
#else
    const char* val = std::getenv(name.c_str());
    return val ? std::string(val) : std::string();
#endif
}

// ── Build MCP ToolDefinition from ApiEndpoint ────────────────────────────────

inline ToolDefinition endpoint_to_tool_def(const ApiEndpoint& ep) {
    ToolDefinition def;
    def.name        = ep.name;
    def.description = ep.description;

    // Build properties JSON
    rapidjson::Document props(rapidjson::kObjectType);
    auto& a = props.GetAllocator();

    std::vector<std::string> required_params;

    for (const auto& p : ep.parameters) {
        rapidjson::Value prop(rapidjson::kObjectType);
        prop.AddMember("type", rapidjson::Value(p.type.c_str(), a), a);
        prop.AddMember("description", rapidjson::Value(p.description.c_str(), a), a);

        props.AddMember(rapidjson::Value(p.name.c_str(), a), prop, a);

        if (p.required) {
            required_params.push_back(p.name);
        }
    }

    def.input_schema.properties_json = json::stringify(props);
    def.input_schema.required        = required_params;

    return def;
}

} // namespace mcp
