#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace mcp {

// ── Protocol version ────────────────────────────────────────────────────────
inline constexpr const char* kProtocolVersion = "2024-11-05";
inline constexpr const char* kJsonRpcVersion  = "2.0";

// ── JSON-RPC id (string | int | null) ───────────────────────────────────────
struct NullId {};

using RpcId = std::variant<NullId, std::string, int64_t>;

// ── Raw JSON-RPC message types ───────────────────────────────────────────────

struct RpcRequest {
    RpcId       id;
    std::string method;
    // params stored as serialized JSON string for lazy parsing
    std::string params_json; // empty string == no params
};

struct RpcResponse {
    RpcId       id;
    std::string result_json; // populated on success
    // error fields
    bool        is_error{false};
    int         error_code{0};
    std::string error_message;
    std::string error_data_json; // optional
};

struct RpcNotification {
    std::string method;
    std::string params_json;
};

// ── MCP capability sets ──────────────────────────────────────────────────────

struct ToolInputSchema {
    std::string type{"object"};
    // JSON string of "properties" object
    std::string properties_json{"{}"}; 
    std::vector<std::string> required;
};

struct ToolDefinition {
    std::string     name;
    std::string     description;
    ToolInputSchema input_schema;
};

struct ResourceDefinition {
    std::string uri;
    std::string name;
    std::string description;
    std::string mime_type; // optional
};

struct PromptArgument {
    std::string name;
    std::string description;
    bool        required{false};
};

struct PromptDefinition {
    std::string                  name;
    std::string                  description;
    std::vector<PromptArgument>  arguments;
};

// ── Tool call result content ─────────────────────────────────────────────────

struct TextContent {
    std::string text;
};

struct ImageContent {
    std::string data;      // base64
    std::string mime_type;
};

struct EmbeddedResource {
    std::string uri;
    std::string mime_type;
    std::string text; // or base64 blob
};

using ContentItem = std::variant<TextContent, ImageContent, EmbeddedResource>;

struct ToolResult {
    std::vector<ContentItem> content;
    bool                     is_error{false};
};

// ── Resource read result ─────────────────────────────────────────────────────

struct ResourceContent {
    std::string uri;
    std::string mime_type;
    std::string text;  // text content (or empty if blob)
    std::string blob;  // base64 blob content (or empty if text)
};

// ── Prompt message ────────────────────────────────────────────────────────────

struct PromptMessage {
    std::string              role; // "user" | "assistant"
    std::vector<ContentItem> content;
};

struct GetPromptResult {
    std::string                  description;
    std::vector<PromptMessage>   messages;
};

// ── Callback signatures ──────────────────────────────────────────────────────

using ParamMap = std::unordered_map<std::string, std::string>;

using ToolHandler     = std::function<ToolResult(const ParamMap& args)>;
using ResourceHandler = std::function<ResourceContent(const std::string& uri)>;
using PromptHandler   = std::function<GetPromptResult(const ParamMap& args)>;

// ── Client/server info ────────────────────────────────────────────────────────

struct Implementation {
    std::string name;
    std::string version;
};

struct ServerCapabilities {
    bool tools{false};
    bool resources{false};
    bool prompts{false};
    bool logging{false};
};

struct ClientCapabilities {
    bool roots{false};
    bool sampling{false};
};

} // namespace mcp
