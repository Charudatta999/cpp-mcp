#pragma once

#include <atomic>
#include <csignal>
#include <functional>

namespace mcp {

using ShutdownCallback = std::function<void()>;

namespace detail {

inline std::atomic<bool>& shutdown_requested() {
    static std::atomic<bool> flag{false};
    return flag;
}

inline ShutdownCallback& shutdown_cb() {
    static ShutdownCallback cb;
    return cb;
}

inline void signal_handler(int /*sig*/) {
    shutdown_requested().store(true, std::memory_order_release);
    auto& cb = shutdown_cb();
    if (cb) cb();
}

} // namespace detail

inline void install_signal_handlers(ShutdownCallback cb = {}) {
    detail::shutdown_cb() = std::move(cb);
    std::signal(SIGINT, detail::signal_handler);
    std::signal(SIGTERM, detail::signal_handler);
}

inline bool shutdown_requested() {
    return detail::shutdown_requested().load(std::memory_order_acquire);
}

} // namespace mcp
