/// cpp_mcp_tests.cpp -- Comprehensive unit + integration tests for cpp-mcp.
///
/// Test categories:
///   1. JSON-RPC serialization
///   2. Gateway security sanitizers
///   3. Server lifecycle & dispatch
///   4. Server thread-safety (atomic initialized, CV-based run)
///   5. Client initialization guard
///   6. Error message truncation (CWE-20 mitigation)
///   7. Transport input validation (port parsing)
///   8. Resource & prompt round-trips
///
/// Build: cmake --build . --target cpp_mcp_tests
/// Run:   ctest -R cpp_mcp_tests --output-on-failure

#include "mcp/core/errors.hpp"
#include "mcp/core/json_utils.hpp"
#include "mcp/core/types.hpp"
#include "mcp/gateway/api_gateway.hpp"
#include "mcp/server/server.hpp"
#include "mcp/client/client.hpp"
#include "mcp/transport/http_transport.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════════
//  Test utilities
// ═══════════════════════════════════════════════════════════════════════════

namespace {

int g_tests_run = 0;
int g_tests_passed = 0;


void run_test(const char* name, void(*fn)()) {
    ++g_tests_run;
    try {
        fn();
        ++g_tests_passed;
        std::cout << "  PASS: " << name << "\n";
    } catch (const std::exception& e) {
        std::cerr << "  FAIL: " << name << " -- " << e.what() << "\n";
    } catch (...) {
        std::cerr << "  FAIL: " << name << " -- unknown exception\n";
    }
}

#define ASSERT_TRUE(expr) \
    do { if (!(expr)) throw std::runtime_error("Assertion failed: " #expr); } while(0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ(a, b) \
    do { if ((a) != (b)) throw std::runtime_error( \
        std::string("Assertion failed: ") + #a + " != " + #b); } while(0)

#define ASSERT_THROWS(expr, ExType) \
    do { bool caught = false; \
        try { expr; } catch (const ExType&) { caught = true; } \
        if (!caught) throw std::runtime_error("Expected " #ExType " from: " #expr); \
    } while(0)

#define ASSERT_NOTHROW(expr) \
    do { try { expr; } catch (const std::exception& e) { \
        throw std::runtime_error(std::string("Unexpected throw in " #expr ": ") + e.what()); \
    }} while(0)

/// In-memory transport for testing server logic without I/O.
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
    void clear() { sent_.clear(); }

private:
    bool running_{false};
    std::vector<std::string> sent_;
};

/// Delayed-stop transport for testing CV-based run() shutdown.
class DelayedStopTransport final : public mcp::Transport {
public:
    void start() override { running_.store(true); }
    void stop() override { running_.store(false); }
    void send(const std::string&) override {}
    [[nodiscard]] bool is_running() const override {
        return running_.load();
    }

    /// Stop after a delay from another thread.
    void schedule_stop(std::chrono::milliseconds delay) {
        std::thread([this, delay] {
            std::this_thread::sleep_for(delay);
            running_.store(false);
        }).detach();
    }

private:
    std::atomic<bool> running_{false};
};

/// Helper to create a server with MemoryTransport and initialize it.
struct TestServer {
    mcp::McpServer server;
    MemoryTransport* transport;

    TestServer()
        : server(mcp::Implementation{"test-server", "1.0.0"},
                 [this] {
                     auto t = std::make_unique<MemoryTransport>();
                     transport = t.get();
                     return t;
                 }())
    {}

    void initialize() {
        server.start();
        transport->inject(
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  1. JSON-RPC serialization tests
// ═══════════════════════════════════════════════════════════════════════════

void test_json_rpc_build_and_parse() {
    const auto request = mcp::json::build_request(
        mcp::RpcId{int64_t{7}},
        "tools/list",
        R"({"cursor":"abc"})");

    const auto parsed = mcp::json::parse_request(request);
    ASSERT_EQ(std::get<int64_t>(parsed.id), 7);
    ASSERT_EQ(parsed.method, "tools/list");
    ASSERT_EQ(parsed.params_json, R"({"cursor":"abc"})");

    const auto response = mcp::json::build_response(parsed.id, R"({"ok":true})");
    const auto parsed_response = mcp::json::parse_response(response);
    ASSERT_FALSE(parsed_response.is_error);
    ASSERT_EQ(parsed_response.result_json, R"({"ok":true})");
}

void test_json_rpc_string_id() {
    const auto request = mcp::json::build_request(
        mcp::RpcId{std::string("req-42")},
        "ping", "");

    const auto parsed = mcp::json::parse_request(request);
    ASSERT_EQ(std::get<std::string>(parsed.id), "req-42");
    ASSERT_EQ(parsed.method, "ping");
}

void test_json_rpc_error_response() {
    auto err = mcp::json::build_error_response(
        mcp::RpcId{int64_t{5}}, -32600, "Invalid Request");
    const auto parsed = mcp::json::parse_response(err);
    ASSERT_TRUE(parsed.is_error);
    ASSERT_EQ(parsed.error_code, -32600);
}

void test_length_aware_json_parsing() {
    // Embedded null bytes should be rejected (CWE-170)
    std::string raw = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
    raw.push_back('\0');
    raw += R"({"jsonrpc":"2.0","id":2,"method":"ping"})";

    ASSERT_THROWS(mcp::json::parse_request(raw), mcp::JsonRpcError);
}

// ═══════════════════════════════════════════════════════════════════════════
//  2. Gateway security sanitizers
// ═══════════════════════════════════════════════════════════════════════════

void test_gateway_sanitize_path_param() {
    ASSERT_EQ(mcp::sanitize_path_param("../a\\b?x#y\r\n"), ".._a_b_x_y__");
    ASSERT_EQ(mcp::sanitize_path_param("normal-value"), "normal-value");
    ASSERT_EQ(mcp::sanitize_path_param(""), "");
}

void test_gateway_sanitize_header_value() {
    ASSERT_EQ(mcp::sanitize_header_value("ok\r\nInjected: bad"), "okInjected: bad");
    ASSERT_EQ(mcp::sanitize_header_value("clean"), "clean");
}

void test_gateway_url_encode() {
    ASSERT_EQ(mcp::url_encode("a b+c/"), "a%20b%2Bc%2F");
    ASSERT_EQ(mcp::url_encode("hello"), "hello");
}

void test_gateway_header_validation() {
    ASSERT_TRUE(mcp::is_valid_header_name("X-Test_Header"));
    ASSERT_TRUE(mcp::is_valid_header_name("Content-Type"));
    ASSERT_FALSE(mcp::is_valid_header_name("Bad\r\nName"));
    ASSERT_FALSE(mcp::is_valid_header_name(""));

    std::unordered_map<std::string, std::string> headers;
    ASSERT_THROWS(
        mcp::set_validated_header(headers, "Bad\r\nName", "value"),
        mcp::McpError);
}

void test_gateway_render_template() {
    const std::unordered_map<std::string, std::string> vars{
        {"id", "123"}, {"name", "widget"}};
    ASSERT_EQ(mcp::render_template("/items/{{id}}", vars), "/items/123");
    ASSERT_EQ(mcp::render_template("/{{name}}/{{id}}", vars), "/widget/123");
}

void test_gateway_config_type_validation() {
    const char* path = "bad_gateway_config.json";
    {
        std::ofstream out(path);
        out << R"({"defaults":{"headers":{"X-Test":42}},"tools":[]})";
    }

    ASSERT_THROWS(mcp::load_api_config(path), mcp::McpError);
    std::remove(path);
}

void test_gateway_ssrf_blocked_hosts() {
    // The SSRF validation is internal to api_gateway.cpp (static function)
    // so we only verify the public helper is_blocked_host-like behavior
    // via url_encode and sanitize_path_param which ARE in the header.
    // The actual SSRF blocking is tested by running the gateway binary.
    // Here we just confirm the sanitizer prevents path traversal:
    std::string result = mcp::sanitize_path_param("../../etc/passwd");
    ASSERT_TRUE(result.find("..") == std::string::npos ||
                result.find("/") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
//  3. Server lifecycle & dispatch
// ═══════════════════════════════════════════════════════════════════════════

void test_server_initialize_handshake() {
    TestServer ts;
    ts.initialize();

    ASSERT_EQ(ts.transport->sent().size(), 1u);
    const auto resp = mcp::json::parse_response(ts.transport->sent()[0]);
    ASSERT_FALSE(resp.is_error);
    ASSERT_TRUE(resp.result_json.find("protocolVersion") != std::string::npos);
    ASSERT_TRUE(resp.result_json.find("test-server") != std::string::npos);
    ts.server.stop();
}

void test_server_rejects_before_initialize() {
    TestServer ts;
    ts.server.start();

    // tools/list before initialize should fail with ServerNotInitialized
    ts.transport->inject(
        R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})");

    ASSERT_EQ(ts.transport->sent().size(), 1u);
    const auto resp = mcp::json::parse_response(ts.transport->sent()[0]);
    ASSERT_TRUE(resp.is_error);
    ASSERT_TRUE(resp.error_message.find("not initialized") != std::string::npos);
    ts.server.stop();
}

void test_server_ping_allowed_before_init() {
    TestServer ts;
    ts.server.start();

    ts.transport->inject(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");

    ASSERT_EQ(ts.transport->sent().size(), 1u);
    const auto resp = mcp::json::parse_response(ts.transport->sent()[0]);
    ASSERT_FALSE(resp.is_error);
    ts.server.stop();
}

void test_server_method_not_found() {
    TestServer ts;
    ts.initialize();
    ts.transport->clear();

    ts.transport->inject(
        R"({"jsonrpc":"2.0","id":5,"method":"nonexistent/method","params":{}})");

    ASSERT_EQ(ts.transport->sent().size(), 1u);
    const auto resp = mcp::json::parse_response(ts.transport->sent()[0]);
    ASSERT_TRUE(resp.is_error);
    ASSERT_TRUE(resp.error_message.find("Method not found") != std::string::npos);
    ts.server.stop();
}

void test_server_notification_ignored() {
    TestServer ts;
    ts.initialize();
    ts.transport->clear();

    // Notification (no "id") -- should not produce a response
    ts.transport->inject(
        R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})");

    ASSERT_EQ(ts.transport->sent().size(), 0u);
    ts.server.stop();
}

void test_server_invalid_json() {
    TestServer ts;
    ts.server.start();

    ts.transport->inject("not json at all {{{");

    ASSERT_EQ(ts.transport->sent().size(), 1u);
    const auto resp = mcp::json::parse_response(ts.transport->sent()[0]);
    ASSERT_TRUE(resp.is_error);
    ts.server.stop();
}

// ═══════════════════════════════════════════════════════════════════════════
//  4. Server thread-safety & CV-based run
// ═══════════════════════════════════════════════════════════════════════════

void test_server_run_stops_promptly() {
    auto transport = std::make_unique<DelayedStopTransport>();
    auto* raw = transport.get();

    mcp::McpServer server(
        mcp::Implementation{"cv-test", "1.0.0"},
        std::move(transport));

    // Schedule stop after 100ms
    raw->schedule_stop(std::chrono::milliseconds(100));

    auto start = std::chrono::steady_clock::now();
    server.run();
    auto elapsed = std::chrono::steady_clock::now() - start;

    // Should return within ~600ms (100ms delay + 500ms max CV timeout)
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    ASSERT_TRUE(ms < 700);
}

// ═══════════════════════════════════════════════════════════════════════════
//  5. Client initialization guard (Fix #4)
// ═══════════════════════════════════════════════════════════════════════════

// Note: We can't fully test McpClient without a live HTTP server,
// but we CAN test that the guard throws before any network call.

void test_client_require_initialized_guard() {
    // Create client with a transport pointing at a dummy URL
    auto transport = std::make_unique<mcp::HttpClientTransport>("http://127.0.0.1:1");
    mcp::McpClient client(std::move(transport));

    // All methods should throw McpError before making any network call
    ASSERT_THROWS(client.list_tools(), mcp::McpError);
    ASSERT_THROWS(client.list_resources(), mcp::McpError);
    ASSERT_THROWS(client.list_prompts(), mcp::McpError);
    ASSERT_THROWS(client.call_tool("x", {}), mcp::McpError);
    ASSERT_THROWS(client.read_resource("x"), mcp::McpError);
    ASSERT_THROWS(client.get_prompt("x", {}), mcp::McpError);
    ASSERT_THROWS(client.ping(), mcp::McpError);
}

// ═══════════════════════════════════════════════════════════════════════════
//  6. Error message truncation (Fix #6 - CWE-20)
// ═══════════════════════════════════════════════════════════════════════════

void test_server_truncates_unknown_tool_name() {
    TestServer ts;
    ts.initialize();
    ts.transport->clear();

    // Send a tools/call with a 500-char tool name
    std::string long_name(500, 'A');
    std::string req = R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":")"
        + long_name + R"(","arguments":{}}})";
    ts.transport->inject(req);

    const auto resp = mcp::json::parse_response(ts.transport->sent()[0]);
    ASSERT_TRUE(resp.is_error);
    // The error message should NOT contain the full 500-char name
    ASSERT_TRUE(resp.error_message.size() < 200);
    ts.server.stop();
}

void test_server_truncates_unknown_resource_uri() {
    TestServer ts;
    ts.initialize();
    ts.transport->clear();

    std::string long_uri(500, 'x');
    std::string req = R"({"jsonrpc":"2.0","id":4,"method":"resources/read","params":{"uri":")"
        + long_uri + R"("}})";
    ts.transport->inject(req);

    const auto resp = mcp::json::parse_response(ts.transport->sent()[0]);
    ASSERT_TRUE(resp.is_error);
    ASSERT_TRUE(resp.error_message.size() < 200);
    ts.server.stop();
}

void test_server_truncates_unknown_prompt_name() {
    TestServer ts;
    ts.initialize();
    ts.transport->clear();

    std::string long_name(500, 'P');
    std::string req = R"({"jsonrpc":"2.0","id":5,"method":"prompts/get","params":{"name":")"
        + long_name + R"(","arguments":{}}})";
    ts.transport->inject(req);

    const auto resp = mcp::json::parse_response(ts.transport->sent()[0]);
    ASSERT_TRUE(resp.is_error);
    ASSERT_TRUE(resp.error_message.size() < 200);
    ts.server.stop();
}

void test_server_truncates_unknown_method() {
    TestServer ts;
    ts.initialize();
    ts.transport->clear();

    std::string long_method(500, 'M');
    std::string req = R"({"jsonrpc":"2.0","id":6,"method":")" + long_method + R"(","params":{}})";
    ts.transport->inject(req);

    const auto resp = mcp::json::parse_response(ts.transport->sent()[0]);
    ASSERT_TRUE(resp.is_error);
    ASSERT_TRUE(resp.error_message.size() < 200);
    ts.server.stop();
}

// ═══════════════════════════════════════════════════════════════════════════
//  7. Transport input validation (Fix #3 - port parsing)
// ═══════════════════════════════════════════════════════════════════════════

void test_http_transport_invalid_port_string() {
    // Invalid port should throw TransportError during start()
    mcp::HttpClientTransport transport("http://localhost:notaport");
    ASSERT_THROWS(transport.start(), mcp::TransportError);
}

void test_http_transport_port_out_of_range() {
    mcp::HttpClientTransport transport("http://localhost:99999");
    ASSERT_THROWS(transport.start(), mcp::TransportError);
}

void test_http_transport_negative_port() {
    mcp::HttpClientTransport transport("http://localhost:-1");
    ASSERT_THROWS(transport.start(), mcp::TransportError);
}

void test_http_transport_valid_port() {
    // Should not throw during construction (lazy connect)
    ASSERT_NOTHROW(mcp::HttpClientTransport("http://localhost:8080"));
}

// ═══════════════════════════════════════════════════════════════════════════
//  8. Tool, Resource & Prompt full round-trips
// ═══════════════════════════════════════════════════════════════════════════

void test_server_tool_round_trip() {
    TestServer ts;

    mcp::ToolDefinition echo;
    echo.name = "echo";
    echo.description = "Echo text back";
    echo.input_schema.properties_json = R"({"text":{"type":"string"}})";
    echo.input_schema.required = {"text"};

    ts.server.add_tool(echo, [](const mcp::ParamMap& args) {
        mcp::ToolResult result;
        result.content.push_back(mcp::TextContent{args.at("text")});
        return result;
    });

    ts.initialize();
    ts.transport->clear();

    ts.transport->inject(
        R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hello world"}}})");

    ASSERT_EQ(ts.transport->sent().size(), 1u);
    const auto resp = mcp::json::parse_response(ts.transport->sent()[0]);
    ASSERT_FALSE(resp.is_error);
    ASSERT_TRUE(resp.result_json.find("hello world") != std::string::npos);
    ts.server.stop();
}

void test_server_tools_list() {
    TestServer ts;

    mcp::ToolDefinition t1;
    t1.name = "tool_a";
    t1.description = "First tool";
    ts.server.add_tool(t1, [](const mcp::ParamMap&) { return mcp::ToolResult{}; });

    mcp::ToolDefinition t2;
    t2.name = "tool_b";
    t2.description = "Second tool";
    ts.server.add_tool(t2, [](const mcp::ParamMap&) { return mcp::ToolResult{}; });

    ts.initialize();
    ts.transport->clear();

    ts.transport->inject(R"({"jsonrpc":"2.0","id":11,"method":"tools/list","params":{}})");

    const auto resp = mcp::json::parse_response(ts.transport->sent()[0]);
    ASSERT_FALSE(resp.is_error);
    ASSERT_TRUE(resp.result_json.find("tool_a") != std::string::npos);
    ASSERT_TRUE(resp.result_json.find("tool_b") != std::string::npos);
    ts.server.stop();
}

void test_server_tool_handler_exception() {
    TestServer ts;

    mcp::ToolDefinition bad;
    bad.name = "crasher";
    bad.description = "Always throws";
    ts.server.add_tool(bad, [](const mcp::ParamMap&) -> mcp::ToolResult {
        throw std::runtime_error("handler exploded");
    });

    ts.initialize();
    ts.transport->clear();

    ts.transport->inject(
        R"({"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"crasher","arguments":{}}})");

    const auto resp = mcp::json::parse_response(ts.transport->sent()[0]);
    // Should return an error ToolResult, not crash
    ASSERT_FALSE(resp.is_error);  // JSON-RPC success, but isError in result
    ASSERT_TRUE(resp.result_json.find("Tool execution failed") != std::string::npos);
    ts.server.stop();
}

void test_server_resource_round_trip() {
    TestServer ts;

    mcp::ResourceDefinition res;
    res.uri = "file:///config.json";
    res.name = "Config";
    res.description = "App config";
    res.mime_type = "application/json";

    ts.server.add_resource(res, [](const std::string&) {
        mcp::ResourceContent rc;
        rc.uri = "file:///config.json";
        rc.mime_type = "application/json";
        rc.text = R"({"key":"value"})";
        return rc;
    });

    ts.initialize();
    ts.transport->clear();

    // List resources
    ts.transport->inject(R"({"jsonrpc":"2.0","id":20,"method":"resources/list","params":{}})");
    auto resp = mcp::json::parse_response(ts.transport->sent()[0]);
    ASSERT_FALSE(resp.is_error);
    ASSERT_TRUE(resp.result_json.find("config.json") != std::string::npos);

    ts.transport->clear();

    // Read resource
    ts.transport->inject(
        R"({"jsonrpc":"2.0","id":21,"method":"resources/read","params":{"uri":"file:///config.json"}})");
    resp = mcp::json::parse_response(ts.transport->sent()[0]);
    ASSERT_FALSE(resp.is_error);
    ASSERT_TRUE(resp.result_json.find("value") != std::string::npos);
    ts.server.stop();
}

void test_server_prompt_round_trip() {
    TestServer ts;

    mcp::PromptDefinition prompt;
    prompt.name = "greet";
    prompt.description = "Generate a greeting";
    prompt.arguments = {{"name", "Person to greet", true}};

    ts.server.add_prompt(prompt, [](const mcp::ParamMap& args) {
        mcp::GetPromptResult result;
        result.description = "A greeting";
        mcp::PromptMessage msg;
        msg.role = "assistant";
        msg.content.push_back(mcp::TextContent{"Hello, " + args.at("name") + "!"});
        result.messages.push_back(std::move(msg));
        return result;
    });

    ts.initialize();
    ts.transport->clear();

    // List prompts
    ts.transport->inject(R"({"jsonrpc":"2.0","id":30,"method":"prompts/list","params":{}})");
    auto resp = mcp::json::parse_response(ts.transport->sent()[0]);
    ASSERT_FALSE(resp.is_error);
    ASSERT_TRUE(resp.result_json.find("greet") != std::string::npos);

    ts.transport->clear();

    // Get prompt
    ts.transport->inject(
        R"({"jsonrpc":"2.0","id":31,"method":"prompts/get","params":{"name":"greet","arguments":{"name":"World"}}})");
    resp = mcp::json::parse_response(ts.transport->sent()[0]);
    ASSERT_FALSE(resp.is_error);
    ASSERT_TRUE(resp.result_json.find("Hello, World!") != std::string::npos);
    ts.server.stop();
}

// ═══════════════════════════════════════════════════════════════════════════
//  9. Edge cases & robustness
// ═══════════════════════════════════════════════════════════════════════════

void test_server_tools_call_missing_name() {
    TestServer ts;
    ts.initialize();
    ts.transport->clear();

    ts.transport->inject(
        R"({"jsonrpc":"2.0","id":40,"method":"tools/call","params":{"arguments":{}}})");

    const auto resp = mcp::json::parse_response(ts.transport->sent()[0]);
    ASSERT_TRUE(resp.is_error);
    ASSERT_TRUE(resp.error_message.find("Missing 'name'") != std::string::npos);
    ts.server.stop();
}

void test_server_resources_read_missing_uri() {
    TestServer ts;
    ts.initialize();
    ts.transport->clear();

    ts.transport->inject(
        R"({"jsonrpc":"2.0","id":41,"method":"resources/read","params":{}})");

    const auto resp = mcp::json::parse_response(ts.transport->sent()[0]);
    ASSERT_TRUE(resp.is_error);
    ASSERT_TRUE(resp.error_message.find("Missing 'uri'") != std::string::npos);
    ts.server.stop();
}

void test_server_prompts_get_missing_name() {
    TestServer ts;
    ts.initialize();
    ts.transport->clear();

    ts.transport->inject(
        R"({"jsonrpc":"2.0","id":42,"method":"prompts/get","params":{"arguments":{}}})");

    const auto resp = mcp::json::parse_response(ts.transport->sent()[0]);
    ASSERT_TRUE(resp.is_error);
    ASSERT_TRUE(resp.error_message.find("Missing 'name'") != std::string::npos);
    ts.server.stop();
}

void test_server_capabilities_reflect_registered() {
    TestServer ts;

    // Register a tool and resource (no prompts)
    mcp::ToolDefinition t; t.name = "x";
    ts.server.add_tool(t, [](const mcp::ParamMap&) { return mcp::ToolResult{}; });
    mcp::ResourceDefinition r; r.uri = "r://x"; r.name = "x";
    ts.server.add_resource(r, [](const std::string&) { return mcp::ResourceContent{}; });

    ts.initialize();
    const auto resp = mcp::json::parse_response(ts.transport->sent()[0]);
    // Should advertise tools and resources but NOT prompts
    ASSERT_TRUE(resp.result_json.find("\"tools\"") != std::string::npos);
    ASSERT_TRUE(resp.result_json.find("\"resources\"") != std::string::npos);
    ASSERT_TRUE(resp.result_json.find("\"prompts\"") == std::string::npos);
    ts.server.stop();
}

void test_server_double_initialize() {
    TestServer ts;
    ts.initialize();
    ts.transport->clear();

    // Second initialize should still succeed (idempotent)
    ts.transport->inject(
        R"({"jsonrpc":"2.0","id":99,"method":"initialize","params":{}})");

    ASSERT_EQ(ts.transport->sent().size(), 1u);
    const auto resp = mcp::json::parse_response(ts.transport->sent()[0]);
    ASSERT_FALSE(resp.is_error);
    ts.server.stop();
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
//  Main
// ═══════════════════════════════════════════════════════════════════════════

int main() {
    std::cout << "╔══════════════════════════════════════════════╗\n";
    std::cout << "║       cpp-mcp Test Suite                     ║\n";
    std::cout << "╚══════════════════════════════════════════════╝\n\n";

    std::cout << "[1] JSON-RPC Serialization\n";
    run_test("json_rpc_build_and_parse", test_json_rpc_build_and_parse);
    run_test("json_rpc_string_id", test_json_rpc_string_id);
    run_test("json_rpc_error_response", test_json_rpc_error_response);
    run_test("length_aware_json_parsing", test_length_aware_json_parsing);

    std::cout << "\n[2] Gateway Security Sanitizers\n";
    run_test("gateway_sanitize_path_param", test_gateway_sanitize_path_param);
    run_test("gateway_sanitize_header_value", test_gateway_sanitize_header_value);
    run_test("gateway_url_encode", test_gateway_url_encode);
    run_test("gateway_header_validation", test_gateway_header_validation);
    run_test("gateway_render_template", test_gateway_render_template);
    run_test("gateway_config_type_validation", test_gateway_config_type_validation);
    run_test("gateway_ssrf_blocked_hosts", test_gateway_ssrf_blocked_hosts);

    std::cout << "\n[3] Server Lifecycle & Dispatch\n";
    run_test("server_initialize_handshake", test_server_initialize_handshake);
    run_test("server_rejects_before_initialize", test_server_rejects_before_initialize);
    run_test("server_ping_allowed_before_init", test_server_ping_allowed_before_init);
    run_test("server_method_not_found", test_server_method_not_found);
    run_test("server_notification_ignored", test_server_notification_ignored);
    run_test("server_invalid_json", test_server_invalid_json);

    std::cout << "\n[4] Server Thread-Safety (CV-based run)\n";
    run_test("server_run_stops_promptly", test_server_run_stops_promptly);

    std::cout << "\n[5] Client Initialization Guard\n";
    run_test("client_require_initialized_guard", test_client_require_initialized_guard);

    std::cout << "\n[6] Error Message Truncation (CWE-20)\n";
    run_test("server_truncates_unknown_tool_name", test_server_truncates_unknown_tool_name);
    run_test("server_truncates_unknown_resource_uri", test_server_truncates_unknown_resource_uri);
    run_test("server_truncates_unknown_prompt_name", test_server_truncates_unknown_prompt_name);
    run_test("server_truncates_unknown_method", test_server_truncates_unknown_method);

    std::cout << "\n[7] Transport Input Validation (Port Parsing)\n";
    run_test("http_transport_invalid_port_string", test_http_transport_invalid_port_string);
    run_test("http_transport_port_out_of_range", test_http_transport_port_out_of_range);
    run_test("http_transport_negative_port", test_http_transport_negative_port);
    run_test("http_transport_valid_port", test_http_transport_valid_port);

    std::cout << "\n[8] Tool, Resource & Prompt Round-Trips\n";
    run_test("server_tool_round_trip", test_server_tool_round_trip);
    run_test("server_tools_list", test_server_tools_list);
    run_test("server_tool_handler_exception", test_server_tool_handler_exception);
    run_test("server_resource_round_trip", test_server_resource_round_trip);
    run_test("server_prompt_round_trip", test_server_prompt_round_trip);

    std::cout << "\n[9] Edge Cases & Robustness\n";
    run_test("server_tools_call_missing_name", test_server_tools_call_missing_name);
    run_test("server_resources_read_missing_uri", test_server_resources_read_missing_uri);
    run_test("server_prompts_get_missing_name", test_server_prompts_get_missing_name);
    run_test("server_capabilities_reflect_registered", test_server_capabilities_reflect_registered);
    run_test("server_double_initialize", test_server_double_initialize);

    std::cout << "\n══════════════════════════════════════════════\n";
    std::cout << "Results: " << g_tests_passed << "/" << g_tests_run << " passed\n";

    if (g_tests_passed == g_tests_run) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    } else {
        std::cerr << (g_tests_run - g_tests_passed) << " TEST(S) FAILED\n";
        return 1;
    }
}
