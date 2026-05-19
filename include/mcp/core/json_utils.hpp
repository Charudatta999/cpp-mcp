#pragma once

#include "mcp/core/errors.hpp"
#include "mcp/core/types.hpp"

#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <limits>
#include <string>
#include <vector>

namespace mcp {
namespace json {

// ── Helpers ──────────────────────────────────────────────────────────────────

/// Serialize a RapidJSON value to a compact JSON string.
inline std::string stringify(const rapidjson::Value& v) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    v.Accept(w);
    return std::string(sb.GetString(), sb.GetSize());
}

/// Parse a JSON string into a document.  Throws on failure.
inline rapidjson::Document parse(const std::string& raw) {
    if (raw.find('\0') != std::string::npos) {
        throw JsonRpcError(ErrorCode::ParseError,
            "JSON parse error: embedded NUL byte");
    }

    rapidjson::Document doc;
    doc.Parse(raw.data(), raw.size());
    if (doc.HasParseError()) {
        throw JsonRpcError(ErrorCode::ParseError,
            std::string("JSON parse error: ") +
            rapidjson::GetParseError_En(doc.GetParseError()));
    }
    return doc;
}

inline rapidjson::Value json_string(const std::string& value,
                                    rapidjson::Document::AllocatorType& a) {
    if (value.size() > (std::numeric_limits<rapidjson::SizeType>::max)()) {
        throw JsonRpcError(ErrorCode::InvalidParams, "JSON string too large");
    }
    return rapidjson::Value(value.data(),
                            static_cast<rapidjson::SizeType>(value.size()),
                            a);
}

// ── JSON-RPC message builders ────────────────────────────────────────────────

/// Build a JSON-RPC request string.
inline std::string build_request(const RpcId& id, const std::string& method,
                                  const std::string& params_json = "") {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto& a = doc.GetAllocator();

    doc.AddMember("jsonrpc", rapidjson::Value("2.0", a), a);

    // id
    if (auto* s = std::get_if<std::string>(&id)) {
        doc.AddMember("id", rapidjson::Value(s->c_str(), a), a);
    } else if (auto* n = std::get_if<int64_t>(&id)) {
        doc.AddMember("id", rapidjson::Value(*n), a);
    }
    // NullId → we still include "id": null for requests
    else {
        doc.AddMember("id", rapidjson::Value(rapidjson::kNullType), a);
    }

    doc.AddMember("method", rapidjson::Value(method.c_str(), a), a);

    if (!params_json.empty()) {
        rapidjson::Document params;
        try {
            params = parse(params_json);
        } catch (const JsonRpcError&) {
            throw JsonRpcError(ErrorCode::InvalidParams, "Invalid JSON-RPC params JSON");
        }
        doc.AddMember("params", rapidjson::Value(params, a), a);
    }

    return stringify(doc);
}

/// Build a JSON-RPC notification string (no id field).
inline std::string build_notification(const std::string& method,
                                       const std::string& params_json = "") {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto& a = doc.GetAllocator();

    doc.AddMember("jsonrpc", rapidjson::Value("2.0", a), a);
    doc.AddMember("method", rapidjson::Value(method.c_str(), a), a);

    if (!params_json.empty()) {
        rapidjson::Document params;
        try {
            params = parse(params_json);
        } catch (const JsonRpcError&) {
            throw JsonRpcError(ErrorCode::InvalidParams, "Invalid JSON-RPC params JSON");
        }
        doc.AddMember("params", rapidjson::Value(params, a), a);
    }

    return stringify(doc);
}

/// Build a JSON-RPC success response string.
inline std::string build_response(const RpcId& id,
                                   const std::string& result_json) {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto& a = doc.GetAllocator();

    doc.AddMember("jsonrpc", rapidjson::Value("2.0", a), a);

    if (auto* s = std::get_if<std::string>(&id)) {
        doc.AddMember("id", rapidjson::Value(s->c_str(), a), a);
    } else if (auto* n = std::get_if<int64_t>(&id)) {
        doc.AddMember("id", rapidjson::Value(*n), a);
    } else {
        doc.AddMember("id", rapidjson::Value(rapidjson::kNullType), a);
    }

    try {
        rapidjson::Document result = parse(result_json);
        doc.AddMember("result", rapidjson::Value(result, a), a);
    } catch (const JsonRpcError&) {
        // Wrap raw string as result
        doc.AddMember("result", json_string(result_json, a), a);
    }

    return stringify(doc);
}

/// Build a JSON-RPC error response string.
inline std::string build_error_response(const RpcId& id, int code,
                                         const std::string& message,
                                         const std::string& data_json = "") {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto& a = doc.GetAllocator();

    doc.AddMember("jsonrpc", rapidjson::Value("2.0", a), a);

    if (auto* s = std::get_if<std::string>(&id)) {
        doc.AddMember("id", rapidjson::Value(s->c_str(), a), a);
    } else if (auto* n = std::get_if<int64_t>(&id)) {
        doc.AddMember("id", rapidjson::Value(*n), a);
    } else {
        doc.AddMember("id", rapidjson::Value(rapidjson::kNullType), a);
    }

    rapidjson::Value err(rapidjson::kObjectType);
    err.AddMember("code", code, a);
    err.AddMember("message", rapidjson::Value(message.c_str(), a), a);

    if (!data_json.empty()) {
        rapidjson::Document data;
        try {
            data = parse(data_json);
        } catch (const JsonRpcError&) {
            throw JsonRpcError(ErrorCode::InvalidParams, "Invalid JSON-RPC error data JSON");
        }
        err.AddMember("data", rapidjson::Value(data, a), a);
    }

    doc.AddMember("error", err, a);
    return stringify(doc);
}

// ── Incoming message parsing ─────────────────────────────────────────────────

/// Extract RpcId from a parsed JSON-RPC message.
inline RpcId extract_id(const rapidjson::Value& doc) {
    if (!doc.HasMember("id") || doc["id"].IsNull()) {
        return NullId{};
    }
    if (doc["id"].IsString()) {
        return std::string(doc["id"].GetString());
    }
    if (doc["id"].IsInt64()) {
        return doc["id"].GetInt64();
    }
    return NullId{};
}

/// Parse a raw JSON string into an RpcRequest.
inline RpcRequest parse_request(const std::string& raw) {
    auto doc = parse(raw);

    if (!doc.IsObject() || !doc.HasMember("method") || !doc["method"].IsString()) {
        throw JsonRpcError(ErrorCode::InvalidRequest, "Invalid JSON-RPC request");
    }

    RpcRequest req;
    req.id     = extract_id(doc);
    req.method = doc["method"].GetString();

    if (doc.HasMember("params") && !doc["params"].IsNull()) {
        req.params_json = stringify(doc["params"]);
    }

    return req;
}

/// Parse a raw JSON string into an RpcResponse.
inline RpcResponse parse_response(const std::string& raw) {
    auto doc = parse(raw);

    RpcResponse resp;
    resp.id = extract_id(doc);

    if (doc.HasMember("error") && doc["error"].IsObject()) {
        const auto& error = doc["error"];
        if (!error.HasMember("code") || !error["code"].IsInt() ||
            !error.HasMember("message") || !error["message"].IsString()) {
            throw JsonRpcError(ErrorCode::InvalidRequest, "Invalid JSON-RPC error response");
        }
        resp.is_error      = true;
        resp.error_code    = error["code"].GetInt();
        resp.error_message = error["message"].GetString();
        if (error.HasMember("data")) {
            resp.error_data_json = stringify(error["data"]);
        }
    } else if (doc.HasMember("result")) {
        resp.result_json = stringify(doc["result"]);
    }

    return resp;
}

// ── ParamMap extraction from params JSON ─────────────────────────────────────

/// Flatten a JSON object's top-level string/number/bool values into a ParamMap.
inline ParamMap extract_params(const std::string& params_json) {
    ParamMap map;
    if (params_json.empty()) return map;

    auto doc = parse(params_json);
    if (!doc.IsObject()) return map;

    for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
        const auto& key = it->name;
        const auto& val = it->value;

        if (val.IsString()) {
            map[key.GetString()] = val.GetString();
        } else if (val.IsInt()) {
            map[key.GetString()] = std::to_string(val.GetInt());
        } else if (val.IsDouble()) {
            map[key.GetString()] = std::to_string(val.GetDouble());
        } else if (val.IsBool()) {
            map[key.GetString()] = val.GetBool() ? "true" : "false";
        } else {
            // Nested objects/arrays → serialize as JSON string
            map[key.GetString()] = stringify(val);
        }
    }
    return map;
}

// ── MCP result serialization ─────────────────────────────────────────────────

/// Serialize a ContentItem to a rapidjson::Value.
inline rapidjson::Value serialize_content(const ContentItem& item,
                                           rapidjson::Document::AllocatorType& a) {
    rapidjson::Value obj(rapidjson::kObjectType);

    if (auto* t = std::get_if<TextContent>(&item)) {
        obj.AddMember("type", "text", a);
        obj.AddMember("text", rapidjson::Value(t->text.c_str(), a), a);
    } else if (auto* img = std::get_if<ImageContent>(&item)) {
        obj.AddMember("type", "image", a);
        obj.AddMember("data", rapidjson::Value(img->data.c_str(), a), a);
        obj.AddMember("mimeType", rapidjson::Value(img->mime_type.c_str(), a), a);
    } else if (auto* res = std::get_if<EmbeddedResource>(&item)) {
        obj.AddMember("type", "resource", a);
        rapidjson::Value resource(rapidjson::kObjectType);
        resource.AddMember("uri", rapidjson::Value(res->uri.c_str(), a), a);
        resource.AddMember("mimeType", rapidjson::Value(res->mime_type.c_str(), a), a);
        resource.AddMember("text", rapidjson::Value(res->text.c_str(), a), a);
        obj.AddMember("resource", resource, a);
    }

    return obj;
}

/// Serialize a ToolResult to a JSON string.
inline std::string serialize_tool_result(const ToolResult& result) {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto& a = doc.GetAllocator();

    rapidjson::Value content_arr(rapidjson::kArrayType);
    for (const auto& item : result.content) {
        content_arr.PushBack(serialize_content(item, a), a);
    }
    doc.AddMember("content", content_arr, a);

    if (result.is_error) {
        doc.AddMember("isError", true, a);
    }

    return stringify(doc);
}

/// Serialize a ToolDefinition to a rapidjson::Value.
inline rapidjson::Value serialize_tool_def(const ToolDefinition& tool,
                                            rapidjson::Document::AllocatorType& a) {
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("name", rapidjson::Value(tool.name.c_str(), a), a);
    obj.AddMember("description", rapidjson::Value(tool.description.c_str(), a), a);

    rapidjson::Value schema(rapidjson::kObjectType);
    schema.AddMember("type", rapidjson::Value(tool.input_schema.type.c_str(), a), a);

    // Parse properties JSON
    try {
        rapidjson::Document props = parse(tool.input_schema.properties_json);
        if (props.IsObject()) {
            schema.AddMember("properties", rapidjson::Value(props, a), a);
        }
    } catch (const JsonRpcError&) {
        // Invalid schema fragments are omitted to preserve legacy behavior.
    }

    if (!tool.input_schema.required.empty()) {
        rapidjson::Value req(rapidjson::kArrayType);
        for (const auto& r : tool.input_schema.required) {
            req.PushBack(rapidjson::Value(r.c_str(), a), a);
        }
        schema.AddMember("required", req, a);
    }

    obj.AddMember("inputSchema", schema, a);
    return obj;
}

/// Serialize a ResourceDefinition to a rapidjson::Value.
inline rapidjson::Value serialize_resource_def(const ResourceDefinition& res,
                                                rapidjson::Document::AllocatorType& a) {
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("uri", rapidjson::Value(res.uri.c_str(), a), a);
    obj.AddMember("name", rapidjson::Value(res.name.c_str(), a), a);
    obj.AddMember("description", rapidjson::Value(res.description.c_str(), a), a);
    if (!res.mime_type.empty()) {
        obj.AddMember("mimeType", rapidjson::Value(res.mime_type.c_str(), a), a);
    }
    return obj;
}

/// Serialize a PromptDefinition to a rapidjson::Value.
inline rapidjson::Value serialize_prompt_def(const PromptDefinition& prompt,
                                              rapidjson::Document::AllocatorType& a) {
    rapidjson::Value obj(rapidjson::kObjectType);
    obj.AddMember("name", rapidjson::Value(prompt.name.c_str(), a), a);
    obj.AddMember("description", rapidjson::Value(prompt.description.c_str(), a), a);

    rapidjson::Value args(rapidjson::kArrayType);
    for (const auto& arg : prompt.arguments) {
        rapidjson::Value argObj(rapidjson::kObjectType);
        argObj.AddMember("name", rapidjson::Value(arg.name.c_str(), a), a);
        argObj.AddMember("description", rapidjson::Value(arg.description.c_str(), a), a);
        argObj.AddMember("required", arg.required, a);
        args.PushBack(argObj, a);
    }
    obj.AddMember("arguments", args, a);
    return obj;
}

/// Serialize a ResourceContent to a JSON string (for resources/read result).
inline std::string serialize_resource_content(const ResourceContent& rc) {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto& a = doc.GetAllocator();

    rapidjson::Value contents(rapidjson::kArrayType);
    rapidjson::Value item(rapidjson::kObjectType);
    item.AddMember("uri", rapidjson::Value(rc.uri.c_str(), a), a);
    item.AddMember("mimeType", rapidjson::Value(rc.mime_type.c_str(), a), a);

    if (!rc.text.empty()) {
        item.AddMember("text", rapidjson::Value(rc.text.c_str(), a), a);
    } else if (!rc.blob.empty()) {
        item.AddMember("blob", rapidjson::Value(rc.blob.c_str(), a), a);
    }

    contents.PushBack(item, a);
    doc.AddMember("contents", contents, a);
    return stringify(doc);
}

/// Serialize a GetPromptResult to a JSON string.
inline std::string serialize_prompt_result(const GetPromptResult& pr) {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto& a = doc.GetAllocator();

    doc.AddMember("description", rapidjson::Value(pr.description.c_str(), a), a);

    rapidjson::Value msgs(rapidjson::kArrayType);
    for (const auto& msg : pr.messages) {
        rapidjson::Value m(rapidjson::kObjectType);
        m.AddMember("role", rapidjson::Value(msg.role.c_str(), a), a);

        rapidjson::Value content(rapidjson::kObjectType);
        // For simplicity, if single text content, flatten
        if (msg.content.size() == 1) {
            if (auto* t = std::get_if<TextContent>(&msg.content[0])) {
                content.AddMember("type", "text", a);
                content.AddMember("text", rapidjson::Value(t->text.c_str(), a), a);
            } else {
                content = serialize_content(msg.content[0], a);
            }
        } else {
            // Multi-content: use array
            rapidjson::Value arr(rapidjson::kArrayType);
            for (const auto& c : msg.content) {
                arr.PushBack(serialize_content(c, a), a);
            }
            content = arr;
        }
        m.AddMember("content", content, a);
        msgs.PushBack(m, a);
    }
    doc.AddMember("messages", msgs, a);
    return stringify(doc);
}

} // namespace json
} // namespace mcp
