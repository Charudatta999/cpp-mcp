#pragma once

#include "mcp/core/types.hpp"
#include "mcp/transport/transport.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mcp {

/// MCP Server -- registers tools, resources, prompts, and handles
/// the full JSON-RPC lifecycle including initialization handshake.
class McpServer {
public:
    McpServer(Implementation info, std::unique_ptr<Transport> transport);
    ~McpServer();

    McpServer(const McpServer&)            = delete;
    McpServer& operator=(const McpServer&) = delete;

    // -- Registration -------------------------------------------------
    void add_tool(ToolDefinition def, ToolHandler handler);
    void add_resource(ResourceDefinition def, ResourceHandler handler);
    void add_prompt(PromptDefinition def, PromptHandler handler);

    // -- Lifecycle ----------------------------------------------------
    /// Start the server (begins listening on transport).
    void start();

    /// Run the server blocking until transport closes.
    void run();

    /// Stop the server.
    void stop();

private:
    void handle_message(const std::string& raw);
    std::string dispatch(const std::string& method,
                         const RpcId& id,
                         const std::string& params_json);

    // Built-in method handlers
    std::string handle_initialize(const RpcId& id, const std::string& params);
    std::string handle_ping(const RpcId& id);
    std::string handle_tools_list(const RpcId& id);
    std::string handle_tools_call(const RpcId& id, const std::string& params);
    std::string handle_resources_list(const RpcId& id);
    std::string handle_resources_read(const RpcId& id, const std::string& params);
    std::string handle_prompts_list(const RpcId& id);
    std::string handle_prompts_get(const RpcId& id, const std::string& params);

    Implementation                info_;
    std::unique_ptr<Transport>    transport_;
    // [FIX #8] Promote to atomic for thread-safety if a multi-threaded
    // transport ever dispatches to handle_initialize / dispatch concurrently.
    std::atomic<bool>             initialized_{false};

    // [FIX #7] Condition variable to replace the 50ms busy-wait in run().
    std::mutex                    run_mutex_;
    std::condition_variable       run_cv_;

    struct ToolEntry {
        ToolDefinition def;
        ToolHandler    handler;
    };
    struct ResourceEntry {
        ResourceDefinition def;
        ResourceHandler    handler;
    };
    struct PromptEntry {
        PromptDefinition def;
        PromptHandler    handler;
    };

    std::unordered_map<std::string, ToolEntry>     tools_;
    std::unordered_map<std::string, ResourceEntry> resources_;
    std::unordered_map<std::string, PromptEntry>   prompts_;
};

} // namespace mcp
