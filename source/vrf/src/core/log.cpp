#include "vrf/core/log.hpp"

#include <cstdio>

namespace vrf
{
    namespace
    {
        void DefaultSink(LogLevel level, std::string_view message)
        {
            const char* tag = level == LogLevel::Error ? "ERROR" : level == LogLevel::Warning ? "WARN" : "INFO";
            std::fprintf(stderr, "[vrf][%s] %.*s\n", tag, static_cast<int>(message.size()), message.data());
            std::fflush(stderr);
        }

        LogSink& Sink()
        {
            static LogSink s_sink = &DefaultSink;
            return s_sink;
        }
    } // namespace

    void SetLogSink(LogSink sink) { Sink() = sink ? std::move(sink) : LogSink(&DefaultSink); }

    void Log(LogLevel level, std::string_view message)
    {
        if (Sink())
            Sink()(level, message);
    }
} // namespace vrf
