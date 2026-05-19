#include "mcp/core/json_utils.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

template <typename Fn>
std::chrono::nanoseconds measure(Fn&& fn) {
    const auto start = std::chrono::steady_clock::now();
    fn();
    const auto stop = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start);
}

} // namespace

int main() {
    constexpr int iterations = 10000;
    size_t checksum = 0;

    const auto request_time = measure([&] {
        for (int i = 0; i < iterations; ++i) {
            const auto raw = mcp::json::build_request(
                mcp::RpcId{static_cast<int64_t>(i)},
                "tools/call",
                R"({"name":"echo","arguments":{"text":"hello"}})");
            const auto parsed = mcp::json::parse_request(raw);
            checksum += parsed.method.size();
            checksum += parsed.params_json.size();
        }
    });

    const auto response_time = measure([&] {
        for (int i = 0; i < iterations; ++i) {
            const auto raw = mcp::json::build_response(
                mcp::RpcId{static_cast<int64_t>(i)},
                R"({"content":[{"type":"text","text":"hello"}]})");
            const auto parsed = mcp::json::parse_response(raw);
            checksum += parsed.result_json.size();
        }
    });

    if (checksum == 0) {
        return 1;
    }

    std::cout << "json_rpc_request_round_trip_ns_per_op="
              << (request_time.count() / iterations) << "\n";
    std::cout << "json_rpc_response_round_trip_ns_per_op="
              << (response_time.count() / iterations) << "\n";
    std::cout << "checksum=" << checksum << "\n";
    return 0;
}
