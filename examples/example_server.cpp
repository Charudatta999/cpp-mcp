/// example_server.cpp — Minimal MCP server with a tool, resource, and prompt.
///
/// Run: pipe JSON-RPC messages via stdin, responses on stdout.
/// Usage with Claude Desktop or any MCP client that supports stdio transport.

#include "mcp/mcp.hpp"

#include <ctime>
#include <sstream>

int main() {
    // Create server with stdio transport
    auto transport = std::make_unique<mcp::StdioTransport>();
    mcp::McpServer server(
        mcp::Implementation{"example-server", "1.0.0"},
        std::move(transport)
    );

    // ── Register a tool: "get_time" ──────────────────────────────────────

    mcp::ToolDefinition time_tool;
    time_tool.name        = "get_time";
    time_tool.description = "Returns the current date and time";
    time_tool.input_schema.properties_json = R"json({
        "timezone": {
            "type": "string",
            "description": "Timezone (currently ignored, uses local)"
        }
    })json";

    server.add_tool(time_tool, [](const mcp::ParamMap& /*args*/) -> mcp::ToolResult {
        std::time_t now = std::time(nullptr);
        std::tm local_time{};
#ifdef _WIN32
        localtime_s(&local_time, &now);
#else
        localtime_r(&now, &local_time);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local_time);

        mcp::ToolResult result;
        result.content.push_back(mcp::TextContent{buf});
        result.is_error = false;
        return result;
    });

    // ── Register a resource: "info://server" ─────────────────────────────

    mcp::ResourceDefinition info_resource;
    info_resource.uri         = "info://server";
    info_resource.name        = "Server Info";
    info_resource.description = "Basic server information";
    info_resource.mime_type   = "text/plain";

    server.add_resource(info_resource, [](const std::string& /*uri*/) {
        mcp::ResourceContent rc;
        rc.uri       = "info://server";
        rc.mime_type = "text/plain";
        rc.text      = "cpp-mcp example server v1.0.0";
        return rc;
    });

    // ── Register a prompt: "greet" ───────────────────────────────────────

    mcp::PromptDefinition greet_prompt;
    greet_prompt.name        = "greet";
    greet_prompt.description = "Generate a greeting for a user";
    greet_prompt.arguments   = {
        mcp::PromptArgument{"name", "The name to greet", true},
        mcp::PromptArgument{"style", "Formal or casual", false},
    };

    server.add_prompt(greet_prompt, [](const mcp::ParamMap& args) {
        std::string name  = "World";
        std::string style = "casual";

        auto it_name = args.find("name");
        if (it_name != args.end()) name = it_name->second;

        auto it_style = args.find("style");
        if (it_style != args.end()) style = it_style->second;

        std::string greeting;
        if (style == "formal") {
            greeting = "Good day, " + name + ". How may I assist you?";
        } else {
            greeting = "Hey " + name + "! What's up?";
        }

        mcp::PromptMessage msg;
        msg.role = "assistant";
        msg.content.push_back(mcp::TextContent{greeting});

        mcp::GetPromptResult result;
        result.description = "A greeting for " + name;
        result.messages.push_back(std::move(msg));
        return result;
    });

    // ── Run ──────────────────────────────────────────────────────────────

    server.run();
    return 0;
}
