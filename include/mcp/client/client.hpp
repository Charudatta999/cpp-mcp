#pragma once

#include "mcp/core/types.hpp"
#include "mcp/transport/http_transport.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mcp {

/// MCP Client — connects to an MCP server over HTTP transport
/// and provides typed accessors for tools, resources, and prompts.
class McpClient {
public:
    explicit McpClient(std::unique_ptr<HttpClientTransport> transport,
                       Implementation info = {"cpp-mcp-client", "1.0.0"});
    ~McpClient();

    McpClient(const McpClient&)            = delete;
    McpClient& operator=(const McpClient&) = delete;

    // ── Lifecycle ────────────────────────────────────────────────
    /// Initialize the MCP session (sends initialize + initialized).
    void initialize();

    // ── Discovery ────────────────────────────────────────────────
    std::vector<ToolDefinition>     list_tools();
    std::vector<ResourceDefinition> list_resources();
    std::vector<PromptDefinition>   list_prompts();

    // ── Invocation ───────────────────────────────────────────────
    ToolResult      call_tool(const std::string& name, const ParamMap& args);
    ResourceContent read_resource(const std::string& uri);
    GetPromptResult get_prompt(const std::string& name, const ParamMap& args);

    /// Send a raw JSON-RPC request, return raw response.
    std::string send_request(const std::string& method,
                              const std::string& params_json = "");

    /// Ping the server.
    void ping();

private:
    std::unique_ptr<HttpClientTransport> transport_;
    Implementation                       info_;
    int64_t                              next_id_{1};
    bool                                 initialized_{false};
};

} // namespace mcp
