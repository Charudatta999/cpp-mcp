#include "mcp/client/client.hpp"
#include "mcp/core/errors.hpp"
#include "mcp/core/json_utils.hpp"
#include "mcp/core/types.hpp"

#include "rapidjson/document.h"

#include <utility>

namespace mcp {

McpClient::McpClient(std::unique_ptr<HttpClientTransport> transport,
                     Implementation info)
    : transport_(std::move(transport)), info_(std::move(info)) {}

McpClient::~McpClient() = default;

// ── Raw request ──────────────────────────────────────────────────────────────

std::string McpClient::send_request(const std::string& method,
                                     const std::string& params_json) {
    RpcId id = next_id_++;
    auto req = json::build_request(id, method, params_json);
    auto raw_resp = transport_->post(transport_->endpoint(), req);

    auto resp = json::parse_response(raw_resp);
    if (resp.is_error) {
        throw JsonRpcError(
            static_cast<ErrorCode>(resp.error_code),
            resp.error_message);
    }
    return resp.result_json;
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

void McpClient::initialize() {
    transport_->start();

    // Build initialize params
    rapidjson::Document doc(rapidjson::kObjectType);
    auto& a = doc.GetAllocator();

    doc.AddMember("protocolVersion",
                  rapidjson::Value(kProtocolVersion, a), a);

    rapidjson::Value caps(rapidjson::kObjectType);
    doc.AddMember("capabilities", caps, a);

    rapidjson::Value info(rapidjson::kObjectType);
    info.AddMember("name", rapidjson::Value(info_.name.c_str(), a), a);
    info.AddMember("version", rapidjson::Value(info_.version.c_str(), a), a);
    doc.AddMember("clientInfo", info, a);

    auto result = send_request("initialize", json::stringify(doc));

    // Send initialized notification
    auto notif = json::build_notification("notifications/initialized");
    transport_->send(notif);

    initialized_ = true;
}

// ── Discovery ────────────────────────────────────────────────────────────────

std::vector<ToolDefinition> McpClient::list_tools() {
    auto result_json = send_request("tools/list");
    auto doc = json::parse(result_json);

    std::vector<ToolDefinition> tools;
    if (!doc.HasMember("tools") || !doc["tools"].IsArray()) return tools;

    for (const auto& t : doc["tools"].GetArray()) {
        ToolDefinition def;
        if (t.HasMember("name")) def.name = t["name"].GetString();
        if (t.HasMember("description"))
            def.description = t["description"].GetString();

        if (t.HasMember("inputSchema") && t["inputSchema"].IsObject()) {
            const auto& schema = t["inputSchema"];
            if (schema.HasMember("type"))
                def.input_schema.type = schema["type"].GetString();
            if (schema.HasMember("properties"))
                def.input_schema.properties_json =
                    json::stringify(schema["properties"]);
            if (schema.HasMember("required") &&
                schema["required"].IsArray()) {
                for (const auto& r : schema["required"].GetArray()) {
                    def.input_schema.required.push_back(r.GetString());
                }
            }
        }
        tools.push_back(std::move(def));
    }
    return tools;
}

std::vector<ResourceDefinition> McpClient::list_resources() {
    auto result_json = send_request("resources/list");
    auto doc = json::parse(result_json);

    std::vector<ResourceDefinition> resources;
    if (!doc.HasMember("resources") || !doc["resources"].IsArray())
        return resources;

    for (const auto& r : doc["resources"].GetArray()) {
        ResourceDefinition def;
        if (r.HasMember("uri")) def.uri = r["uri"].GetString();
        if (r.HasMember("name")) def.name = r["name"].GetString();
        if (r.HasMember("description"))
            def.description = r["description"].GetString();
        if (r.HasMember("mimeType"))
            def.mime_type = r["mimeType"].GetString();
        resources.push_back(std::move(def));
    }
    return resources;
}

std::vector<PromptDefinition> McpClient::list_prompts() {
    auto result_json = send_request("prompts/list");
    auto doc = json::parse(result_json);

    std::vector<PromptDefinition> prompts;
    if (!doc.HasMember("prompts") || !doc["prompts"].IsArray())
        return prompts;

    for (const auto& p : doc["prompts"].GetArray()) {
        PromptDefinition def;
        if (p.HasMember("name")) def.name = p["name"].GetString();
        if (p.HasMember("description"))
            def.description = p["description"].GetString();
        if (p.HasMember("arguments") && p["arguments"].IsArray()) {
            for (const auto& arg : p["arguments"].GetArray()) {
                PromptArgument pa;
                if (arg.HasMember("name"))
                    pa.name = arg["name"].GetString();
                if (arg.HasMember("description"))
                    pa.description = arg["description"].GetString();
                if (arg.HasMember("required"))
                    pa.required = arg["required"].GetBool();
                def.arguments.push_back(std::move(pa));
            }
        }
        prompts.push_back(std::move(def));
    }
    return prompts;
}

// ── Invocation ───────────────────────────────────────────────────────────────

ToolResult McpClient::call_tool(const std::string& name,
                                 const ParamMap& args) {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto& a = doc.GetAllocator();

    doc.AddMember("name", rapidjson::Value(name.c_str(), a), a);

    rapidjson::Value argsObj(rapidjson::kObjectType);
    for (const auto& [k, v] : args) {
        argsObj.AddMember(
            rapidjson::Value(k.c_str(), a),
            rapidjson::Value(v.c_str(), a), a);
    }
    doc.AddMember("arguments", argsObj, a);

    auto result_json = send_request("tools/call", json::stringify(doc));
    auto result_doc = json::parse(result_json);

    ToolResult result;
    if (result_doc.HasMember("isError") && result_doc["isError"].IsBool()) {
        result.is_error = result_doc["isError"].GetBool();
    }

    if (result_doc.HasMember("content") && result_doc["content"].IsArray()) {
        for (const auto& c : result_doc["content"].GetArray()) {
            if (!c.HasMember("type")) continue;
            std::string type = c["type"].GetString();

            if (type == "text" && c.HasMember("text")) {
                result.content.push_back(
                    TextContent{c["text"].GetString()});
            } else if (type == "image") {
                ImageContent img;
                if (c.HasMember("data")) img.data = c["data"].GetString();
                if (c.HasMember("mimeType"))
                    img.mime_type = c["mimeType"].GetString();
                result.content.push_back(std::move(img));
            }
        }
    }
    return result;
}

ResourceContent McpClient::read_resource(const std::string& uri) {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto& a = doc.GetAllocator();
    doc.AddMember("uri", rapidjson::Value(uri.c_str(), a), a);

    auto result_json = send_request("resources/read", json::stringify(doc));
    auto result_doc = json::parse(result_json);

    ResourceContent rc;
    if (result_doc.HasMember("contents") &&
        result_doc["contents"].IsArray() &&
        result_doc["contents"].Size() > 0) {

        const auto& item = result_doc["contents"][0];
        if (item.HasMember("uri")) rc.uri = item["uri"].GetString();
        if (item.HasMember("mimeType"))
            rc.mime_type = item["mimeType"].GetString();
        if (item.HasMember("text")) rc.text = item["text"].GetString();
        if (item.HasMember("blob")) rc.blob = item["blob"].GetString();
    }
    return rc;
}

GetPromptResult McpClient::get_prompt(const std::string& name,
                                       const ParamMap& args) {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto& a = doc.GetAllocator();
    doc.AddMember("name", rapidjson::Value(name.c_str(), a), a);

    rapidjson::Value argsObj(rapidjson::kObjectType);
    for (const auto& [k, v] : args) {
        argsObj.AddMember(
            rapidjson::Value(k.c_str(), a),
            rapidjson::Value(v.c_str(), a), a);
    }
    doc.AddMember("arguments", argsObj, a);

    auto result_json = send_request("prompts/get", json::stringify(doc));
    auto result_doc = json::parse(result_json);

    GetPromptResult result;
    if (result_doc.HasMember("description"))
        result.description = result_doc["description"].GetString();

    if (result_doc.HasMember("messages") &&
        result_doc["messages"].IsArray()) {
        for (const auto& m : result_doc["messages"].GetArray()) {
            PromptMessage msg;
            if (m.HasMember("role")) msg.role = m["role"].GetString();

            if (m.HasMember("content")) {
                const auto& c = m["content"];
                if (c.IsObject() && c.HasMember("type")) {
                    std::string type = c["type"].GetString();
                    if (type == "text" && c.HasMember("text")) {
                        msg.content.push_back(
                            TextContent{c["text"].GetString()});
                    }
                }
            }
            result.messages.push_back(std::move(msg));
        }
    }
    return result;
}

void McpClient::ping() {
    send_request("ping");
}

} // namespace mcp
