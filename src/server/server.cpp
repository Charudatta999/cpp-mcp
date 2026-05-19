#include "mcp/server/server.hpp"
#include "mcp/core/errors.hpp"
#include "mcp/core/json_utils.hpp"
#include "mcp/core/types.hpp"

#include "rapidjson/document.h"

#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

namespace mcp {

McpServer::McpServer(Implementation info, std::unique_ptr<Transport> transport)
    : info_(std::move(info)), transport_(std::move(transport)) {}

McpServer::~McpServer() {
    stop();
}

// ── Registration ─────────────────────────────────────────────────────────────

void McpServer::add_tool(ToolDefinition def, ToolHandler handler) {
    auto name = def.name;
    tools_[name] = ToolEntry{std::move(def), std::move(handler)};
}

void McpServer::add_resource(ResourceDefinition def, ResourceHandler handler) {
    auto uri = def.uri;
    resources_[uri] = ResourceEntry{std::move(def), std::move(handler)};
}

void McpServer::add_prompt(PromptDefinition def, PromptHandler handler) {
    auto name = def.name;
    prompts_[name] = PromptEntry{std::move(def), std::move(handler)};
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

void McpServer::start() {
    transport_->set_message_callback(
        [this](const std::string& msg) { handle_message(msg); });
    transport_->start();
}

void McpServer::run() {
    start();
    // Block until transport closes
    while (transport_->is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void McpServer::stop() {
    if (transport_) {
        transport_->stop();
    }
}

// ── Message handling ─────────────────────────────────────────────────────────

void McpServer::handle_message(const std::string& raw) {
    try {
        // [H2] Parse once — eliminates double-parse (CWE-407)
        auto doc = json::parse(raw);

        if (!doc.IsObject() || !doc.HasMember("method") || !doc["method"].IsString()) {
            throw JsonRpcError(ErrorCode::InvalidRequest, "Invalid JSON-RPC request");
        }

        // Notifications have no "id" field
        bool is_notification = !doc.HasMember("id");

        if (is_notification) {
            // Handle known notifications silently
            // ("notifications/initialized", etc.)
            // Unknown notifications are ignored per spec
            return;
        }

        // Extract request fields from the already-parsed document
        RpcRequest req;
        req.id     = json::extract_id(doc);
        req.method = doc["method"].GetString();

        if (doc.HasMember("params") && !doc["params"].IsNull()) {
            req.params_json = json::stringify(doc["params"]);
        }

        std::string response = dispatch(req.method, req.id, req.params_json);
        if (!response.empty()) {
            transport_->send(response);
        }

    } catch (const JsonRpcError& e) {
        auto err = json::build_error_response(
            NullId{}, static_cast<int>(e.code()), e.what());
        transport_->send(err);
    } catch (const std::exception& e) {
        std::cerr << "[cpp-mcp] Internal server error: " << e.what() << "\n";
        auto err = json::build_error_response(
            NullId{}, static_cast<int>(ErrorCode::InternalError), "Internal server error");
        transport_->send(err);
    }
}

std::string McpServer::dispatch(const std::string& method,
                                 const RpcId& id,
                                 const std::string& params_json) {
    if (method == "initialize") {
        return handle_initialize(id, params_json);
    }

    // All other methods require initialization
    if (!initialized_ && method != "ping") {
        return json::build_error_response(
            id, static_cast<int>(ErrorCode::ServerNotInitialized),
            "Server not initialized");
    }

    if (method == "ping") {
        return handle_ping(id);
    } else if (method == "tools/list") {
        return handle_tools_list(id);
    } else if (method == "tools/call") {
        return handle_tools_call(id, params_json);
    } else if (method == "resources/list") {
        return handle_resources_list(id);
    } else if (method == "resources/read") {
        return handle_resources_read(id, params_json);
    } else if (method == "prompts/list") {
        return handle_prompts_list(id);
    } else if (method == "prompts/get") {
        return handle_prompts_get(id, params_json);
    }

    // [M1] Truncate method name in errors (CWE-20)
    std::string safe_method = method.substr(0, 128);
    return json::build_error_response(
        id, static_cast<int>(ErrorCode::MethodNotFound),
        "Method not found: " + safe_method);
}

// ── Built-in method handlers ─────────────────────────────────────────────────

std::string McpServer::handle_initialize(const RpcId& id,
                                          const std::string& /*params*/) {
    initialized_ = true;

    rapidjson::Document doc(rapidjson::kObjectType);
    auto& a = doc.GetAllocator();

    doc.AddMember("protocolVersion",
                  rapidjson::Value(kProtocolVersion, a), a);

    // Server capabilities
    rapidjson::Value caps(rapidjson::kObjectType);
    if (!tools_.empty()) {
        rapidjson::Value tc(rapidjson::kObjectType);
        caps.AddMember("tools", tc, a);
    }
    if (!resources_.empty()) {
        rapidjson::Value rc(rapidjson::kObjectType);
        caps.AddMember("resources", rc, a);
    }
    if (!prompts_.empty()) {
        rapidjson::Value pc(rapidjson::kObjectType);
        caps.AddMember("prompts", pc, a);
    }
    doc.AddMember("capabilities", caps, a);

    // Server info
    rapidjson::Value info(rapidjson::kObjectType);
    info.AddMember("name", rapidjson::Value(info_.name.c_str(), a), a);
    info.AddMember("version", rapidjson::Value(info_.version.c_str(), a), a);
    doc.AddMember("serverInfo", info, a);

    return json::build_response(id, json::stringify(doc));
}

std::string McpServer::handle_ping(const RpcId& id) {
    return json::build_response(id, "{}");
}

std::string McpServer::handle_tools_list(const RpcId& id) {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto& a = doc.GetAllocator();

    rapidjson::Value arr(rapidjson::kArrayType);
    for (const auto& [name, entry] : tools_) {
        arr.PushBack(json::serialize_tool_def(entry.def, a), a);
    }
    doc.AddMember("tools", arr, a);

    return json::build_response(id, json::stringify(doc));
}

std::string McpServer::handle_tools_call(const RpcId& id,
                                          const std::string& params) {
    auto doc = json::parse(params);

    if (!doc.HasMember("name") || !doc["name"].IsString()) {
        return json::build_error_response(
            id, static_cast<int>(ErrorCode::InvalidParams),
            "Missing 'name' in tools/call");
    }

    std::string tool_name = doc["name"].GetString();
    auto it = tools_.find(tool_name);
    if (it == tools_.end()) {
        return json::build_error_response(
            id, static_cast<int>(ErrorCode::InvalidParams),
            "Unknown tool: " + tool_name);
    }

    // Extract arguments
    ParamMap args;
    if (doc.HasMember("arguments") && doc["arguments"].IsObject()) {
        args = json::extract_params(json::stringify(doc["arguments"]));
    }

    try {
        auto result = it->second.handler(args);
        return json::build_response(id, json::serialize_tool_result(result));
    } catch (const std::exception& e) {
        std::cerr << "[cpp-mcp] Tool handler failed: " << e.what() << "\n";
        ToolResult err_result;
        err_result.is_error = true;
        err_result.content.push_back(TextContent{"Tool execution failed"});
        return json::build_response(id,
            json::serialize_tool_result(err_result));
    }
}

std::string McpServer::handle_resources_list(const RpcId& id) {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto& a = doc.GetAllocator();

    rapidjson::Value arr(rapidjson::kArrayType);
    for (const auto& [uri, entry] : resources_) {
        arr.PushBack(json::serialize_resource_def(entry.def, a), a);
    }
    doc.AddMember("resources", arr, a);

    return json::build_response(id, json::stringify(doc));
}

std::string McpServer::handle_resources_read(const RpcId& id,
                                              const std::string& params) {
    auto doc = json::parse(params);

    if (!doc.HasMember("uri") || !doc["uri"].IsString()) {
        return json::build_error_response(
            id, static_cast<int>(ErrorCode::InvalidParams),
            "Missing 'uri' in resources/read");
    }

    std::string uri = doc["uri"].GetString();
    auto it = resources_.find(uri);
    if (it == resources_.end()) {
        return json::build_error_response(
            id, static_cast<int>(ErrorCode::InvalidParams),
            "Unknown resource: " + uri);
    }

    try {
        auto content = it->second.handler(uri);
        return json::build_response(id,
            json::serialize_resource_content(content));
    } catch (const std::exception& e) {
        std::cerr << "[cpp-mcp] Resource handler failed: " << e.what() << "\n";
        return json::build_error_response(
            id, static_cast<int>(ErrorCode::InternalError), "Resource read failed");
    }
}

std::string McpServer::handle_prompts_list(const RpcId& id) {
    rapidjson::Document doc(rapidjson::kObjectType);
    auto& a = doc.GetAllocator();

    rapidjson::Value arr(rapidjson::kArrayType);
    for (const auto& [name, entry] : prompts_) {
        arr.PushBack(json::serialize_prompt_def(entry.def, a), a);
    }
    doc.AddMember("prompts", arr, a);

    return json::build_response(id, json::stringify(doc));
}

std::string McpServer::handle_prompts_get(const RpcId& id,
                                           const std::string& params) {
    auto doc = json::parse(params);

    if (!doc.HasMember("name") || !doc["name"].IsString()) {
        return json::build_error_response(
            id, static_cast<int>(ErrorCode::InvalidParams),
            "Missing 'name' in prompts/get");
    }

    std::string prompt_name = doc["name"].GetString();
    auto it = prompts_.find(prompt_name);
    if (it == prompts_.end()) {
        return json::build_error_response(
            id, static_cast<int>(ErrorCode::InvalidParams),
            "Unknown prompt: " + prompt_name);
    }

    // Extract arguments
    ParamMap args;
    if (doc.HasMember("arguments") && doc["arguments"].IsObject()) {
        args = json::extract_params(json::stringify(doc["arguments"]));
    }

    try {
        auto result = it->second.handler(args);
        return json::build_response(id,
            json::serialize_prompt_result(result));
    } catch (const std::exception& e) {
        std::cerr << "[cpp-mcp] Prompt handler failed: " << e.what() << "\n";
        return json::build_error_response(
            id, static_cast<int>(ErrorCode::InternalError), "Prompt execution failed");
    }
}

} // namespace mcp
