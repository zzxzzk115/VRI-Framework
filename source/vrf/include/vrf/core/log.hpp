/*
 * log.hpp - minimal logging with a default internal sink and an external override.
 *
 * By default the framework logs to stderr (the built-in sink). An embedding app /
 * library can take over all framework + VRI backend/validation messages by
 * installing its own sink with SetLogSink - e.g. forwarding into spdlog:
 *
 *     vrf::SetLogSink([logger](vrf::LogLevel level, std::string_view msg) {
 *         switch (level) {
 *             case vrf::LogLevel::Error:   logger->error("{}", msg);  break;
 *             case vrf::LogLevel::Warning: logger->warn("{}", msg);   break;
 *             case vrf::LogLevel::Info:    logger->info("{}", msg);   break;
 *         }
 *     });
 *
 * Passing an empty sink to SetLogSink restores the built-in stderr default.
 */
#pragma once

#include <format>
#include <functional>
#include <string_view>
#include <utility>

namespace vrf
{
    enum class LogLevel
    {
        Info,
        Warning,
        Error
    };

    using LogSink = std::function<void(LogLevel level, std::string_view message)>;

    // Replace the global sink. An empty function restores the default (stderr).
    void SetLogSink(LogSink sink);

    // The core sink entry point (also fed by the VRI message callback). Takes an
    // already-formatted message.
    void Log(LogLevel level, std::string_view message);

    // std::format convenience. `LogInfo("hello")` works (no args); for a runtime
    // string use the base Log() or LogInfo("{}", runtimeString).
    template<class... Args>
    void LogInfo(std::format_string<Args...> fmt, Args&&... args)
    {
        Log(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
    }

    template<class... Args>
    void LogWarning(std::format_string<Args...> fmt, Args&&... args)
    {
        Log(LogLevel::Warning, std::format(fmt, std::forward<Args>(args)...));
    }

    template<class... Args>
    void LogError(std::format_string<Args...> fmt, Args&&... args)
    {
        Log(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
    }
} // namespace vrf
