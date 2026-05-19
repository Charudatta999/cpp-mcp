#include "mcp/core/json_utils.hpp"
#include "mcp/gateway/api_gateway.hpp"
#include "mcp/server/server.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

class MemoryTransport final : public mcp::Transport {
public:
    void start() override { running_ = true; }
    void stop() override { running_ = false; }
    void send(const std::string& message) override { sent_.push_back(message); }
    [[nodiscard]] bool is_running() const override { return running_; }

    void inject(const std::string& message) {
        assert(on_message_);
        on_message_(message);
    }

    [[nodiscard]] const std::vector<std::string>& sent() const { return sent_; }

private:
    bool running_{false};
    std::vector<std::string> sent_;
};

void test_json_rpc_build_and_parse() {
    const auto request = mcp::json::build_request(
        mcp::RpcId{int64_t{7}},
        "tools/list",
        R"({"cursor":"abc"})");

    const auto parsed = mcp::json::parse_request(request);
    assert(std::get<int64_t>(parsed.id) == 7);
    assert(parsed.method == "tools/list");
    assert(parsed.params_json == R"({"cursor":"abc"})");

    const auto response = mcp::json::build_response(parsed.id, R"({"ok":true})");
    const auto parsed_response = mcp::json::parse_response(response);
    assert(!parsed_response.is_error);
    assert(parsed_response.result_json == R"({"ok":true})");
}

void test_gateway_sanitizers() {
    assert(mcp::sanitize_path_param("../a\\b?x#y\r\n") == ".._a_b_x_y__");
    assert(mcp::sanitize_header_value("ok\r\nInjected: bad") == "okInjected: bad");
    assert(mcp::url_encode("a b+c/") == "a%20b%2Bc%2F");
    assert(mcp::is_valid_header_name("X-Test_Header"));
    assert(!mcp::is_valid_header_name("Bad\r\nName"));

    bool invalid_header_rejected = false;
    try {
        std::unordered_map<std::string, std::string> headers;
        mcp::set_validated_header(headers, "Bad\r\nName", "value");
    } catch (const mcp::McpError&) {
        invalid_header_rejected = true;
    }
    assert(invalid_header_rejected);

    const std::unordered_map<std::string, std::string> vars{{"id", "123"}};
    assert(mcp::render_template("/items/{{id}}", vars) == "/items/123");
}

void test_length_aware_json_parsing() {
    std::string raw = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
    raw.push_back('\0');
    raw += R"({"jsonrpc":"2.0","id":2,"method":"ping"})";

    bool rejected = false;
    try {
        (void)mcp::json::parse_request(raw);
    } catch (const mcp::JsonRpcError&) {
        rejected = true;
    }
    assert(rejected);
}

void test_gateway_config_type_validation() {
    const char* path = "bad_gateway_config.json";
    {
        std::ofstream out(path);
        out << R"({"defaults":{"headers":{"X-Test":42}},"tools":[]})";
    }

    bool rejected = false;
    try {
        (void)mcp::load_api_config(path);
    } catch (const mcp::McpError&) {
        rejected = true;
    }
    std::remove(path);
    assert(rejected);
}

void test_server_tool_round_trip() {
    auto transport = std::make_unique<MemoryTransport>();
    auto* raw_transport = transport.get();

    mcp::McpServer server(
        mcp::Implementation{"test-server", "1.0.0"},
        std::move(transport));

    mcp::ToolDefinition echo;
    echo.name = "echo";
    echo.description = "Echo text";
    echo.input_schema.properties_json = R"({"text":{"type":"string"}})";
    echo.input_schema.required = {"text"};

    server.add_tool(echo, [](const mcp::ParamMap& args) {
        mcp::ToolResult result;
        result.content.push_back(mcp::TextContent{args.at("text")});
        return result;
    });

    server.start();
    raw_transport->inject(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
    raw_transport->inject(
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hello"}}})");

    assert(raw_transport->sent().size() == 2);

    const auto init_response = mcp::json::parse_response(raw_transport->sent()[0]);
    assert(!init_response.is_error);
    assert(init_response.result_json.find("test-server") != std::string::npos);

    const auto tool_response = mcp::json::parse_response(raw_transport->sent()[1]);
    assert(!tool_response.is_error);
    assert(tool_response.result_json.find("hello") != std::string::npos);

    server.stop();
}

} // namespace

int main() {
    test_json_rpc_build_and_parse();
    test_gateway_sanitizers();
    test_length_aware_json_parsing();
    test_gateway_config_type_validation();
    test_server_tool_round_trip();
    std::cout << "cpp-mcp tests passed\n";
    return 0;
}
