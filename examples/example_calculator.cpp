/// example_calculator.cpp — MCP calculator server.
///
/// Demonstrates a more practical tool-only MCP server with
/// multiple tools and input validation.

#include "mcp/mcp.hpp"

#include <cmath>
#include <sstream>
#include <stdexcept>

static double parse_number(const mcp::ParamMap& args, const std::string& key) {
    auto it = args.find(key);
    if (it == args.end()) {
        throw std::runtime_error("Missing required argument: " + key);
    }
    try {
        return std::stod(it->second);
    } catch (...) {
        throw std::runtime_error("Invalid number for '" + key + "': " + it->second);
    }
}

static mcp::ToolResult make_result(double value) {
    std::ostringstream oss;
    oss << value;
    mcp::ToolResult r;
    r.content.push_back(mcp::TextContent{oss.str()});
    r.is_error = false;
    return r;
}

static mcp::ToolResult make_error(const std::string& msg) {
    mcp::ToolResult r;
    r.content.push_back(mcp::TextContent{msg});
    r.is_error = true;
    return r;
}

int main() {
    auto transport = std::make_unique<mcp::StdioTransport>();
    mcp::McpServer server(
        mcp::Implementation{"calculator-server", "1.0.0"},
        std::move(transport)
    );

    // ── Add ─────────────────────────────────────────────────────────────
    {
        mcp::ToolDefinition def;
        def.name        = "add";
        def.description = "Add two numbers";
        def.input_schema.properties_json = R"json({
            "a": {"type": "number", "description": "First operand"},
            "b": {"type": "number", "description": "Second operand"}
        })json";
        def.input_schema.required = {"a", "b"};

        server.add_tool(def, [](const mcp::ParamMap& args) -> mcp::ToolResult {
            return make_result(parse_number(args, "a") + parse_number(args, "b"));
        });
    }

    // ── Subtract ────────────────────────────────────────────────────────
    {
        mcp::ToolDefinition def;
        def.name        = "subtract";
        def.description = "Subtract b from a";
        def.input_schema.properties_json = R"json({
            "a": {"type": "number", "description": "First operand"},
            "b": {"type": "number", "description": "Second operand"}
        })json";
        def.input_schema.required = {"a", "b"};

        server.add_tool(def, [](const mcp::ParamMap& args) -> mcp::ToolResult {
            return make_result(parse_number(args, "a") - parse_number(args, "b"));
        });
    }

    // ── Multiply ────────────────────────────────────────────────────────
    {
        mcp::ToolDefinition def;
        def.name        = "multiply";
        def.description = "Multiply two numbers";
        def.input_schema.properties_json = R"json({
            "a": {"type": "number", "description": "First operand"},
            "b": {"type": "number", "description": "Second operand"}
        })json";
        def.input_schema.required = {"a", "b"};

        server.add_tool(def, [](const mcp::ParamMap& args) -> mcp::ToolResult {
            return make_result(parse_number(args, "a") * parse_number(args, "b"));
        });
    }

    // ── Divide ──────────────────────────────────────────────────────────
    {
        mcp::ToolDefinition def;
        def.name        = "divide";
        def.description = "Divide a by b";
        def.input_schema.properties_json = R"json({
            "a": {"type": "number", "description": "Dividend"},
            "b": {"type": "number", "description": "Divisor, must be non-zero"}
        })json";
        def.input_schema.required = {"a", "b"};

        server.add_tool(def, [](const mcp::ParamMap& args) -> mcp::ToolResult {
            double b = parse_number(args, "b");
            if (b == 0.0) return make_error("Division by zero");
            return make_result(parse_number(args, "a") / b);
        });
    }

    // ── Square root ─────────────────────────────────────────────────────
    {
        mcp::ToolDefinition def;
        def.name        = "sqrt";
        def.description = "Square root of a number";
        def.input_schema.properties_json = R"json({
            "value": {"type": "number", "description": "Non-negative number"}
        })json";
        def.input_schema.required = {"value"};

        server.add_tool(def, [](const mcp::ParamMap& args) -> mcp::ToolResult {
            double v = parse_number(args, "value");
            if (v < 0) return make_error("Cannot take square root of negative number");
            return make_result(std::sqrt(v));
        });
    }

    // ── Power ───────────────────────────────────────────────────────────
    {
        mcp::ToolDefinition def;
        def.name        = "power";
        def.description = "Raise base to exponent";
        def.input_schema.properties_json = R"json({
            "base":     {"type": "number", "description": "Base"},
            "exponent": {"type": "number", "description": "Exponent"}
        })json";
        def.input_schema.required = {"base", "exponent"};

        server.add_tool(def, [](const mcp::ParamMap& args) -> mcp::ToolResult {
            return make_result(
                std::pow(parse_number(args, "base"),
                         parse_number(args, "exponent")));
        });
    }

    server.run();
    return 0;
}
