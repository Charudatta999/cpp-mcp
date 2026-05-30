#pragma once

#include <atomic>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>

namespace mcp {
namespace log {

enum class Level : int {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Fatal = 5,
    Off   = 6
};

inline const char* level_name(Level lvl) {
    switch (lvl) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
        case Level::Fatal: return "FATAL";
        default:           return "?";
    }
}

using Sink = std::function<void(Level, const char* file, int line,
                                const std::string& msg)>;

namespace detail {

inline std::atomic<Level>& min_level() {
    static std::atomic<Level> lvl{Level::Info};
    return lvl;
}

inline Sink& sink() {
    static Sink s = [](Level lvl, const char* /*file*/, int /*line*/,
                       const std::string& msg) {
        std::time_t now = std::time(nullptr);
        char ts[20];
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &now);
#else
        localtime_r(&now, &tm_buf);
#endif
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
        std::fprintf(stderr, "[%s] [%s] %s\n", ts, level_name(lvl),
                     msg.c_str());
    };
    return s;
}

} // namespace detail

inline void set_level(Level lvl) {
    detail::min_level().store(lvl, std::memory_order_relaxed);
}

inline Level get_level() {
    return detail::min_level().load(std::memory_order_relaxed);
}

inline void set_sink(Sink s) {
    detail::sink() = std::move(s);
}

inline void write(Level lvl, const char* file, int line,
                  const std::string& msg) {
    if (static_cast<int>(lvl) <
        static_cast<int>(detail::min_level().load(std::memory_order_relaxed)))
        return;
    detail::sink()(lvl, file, line, msg);
}

} // namespace log
} // namespace mcp

#define MCP_LOG(level, msg)                                        \
    do {                                                           \
        if (static_cast<int>(level) >=                             \
            static_cast<int>(::mcp::log::get_level()))             \
            ::mcp::log::write(level, __FILE__, __LINE__, msg);     \
    } while (0)

#define MCP_TRACE(msg) MCP_LOG(::mcp::log::Level::Trace, msg)
#define MCP_DEBUG(msg) MCP_LOG(::mcp::log::Level::Debug, msg)
#define MCP_INFO(msg)  MCP_LOG(::mcp::log::Level::Info,  msg)
#define MCP_WARN(msg)  MCP_LOG(::mcp::log::Level::Warn,  msg)
#define MCP_ERROR(msg) MCP_LOG(::mcp::log::Level::Error, msg)
#define MCP_FATAL(msg) MCP_LOG(::mcp::log::Level::Fatal, msg)
