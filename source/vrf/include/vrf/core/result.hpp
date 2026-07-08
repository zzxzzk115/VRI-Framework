/*
 * result.hpp - error type and std::expected alias used across the framework.
 *
 * Every fallible framework call returns vrf::Expected<T> (std::expected<T, Error>),
 * matching VRI's own C++ wrapper (vri.hpp), whose Device::create already returns
 * std::expected.
 */
#pragma once

#include <expected>
#include <string>
#include <utility>

#include <vri/vri.h>

namespace vrf
{
    // A failure carries the originating VriResult (VriResult_Failure for framework-level
    // errors that don't map to a VRI code) plus a human-readable message.
    struct Error
    {
        VriResult   code = VriResult_Failure;
        std::string message;
    };

    template<class T>
    using Expected = std::expected<T, Error>;

    [[nodiscard]] constexpr bool Succeeded(VriResult r) noexcept { return r == VriResult_Success; }

    [[nodiscard]] inline const char* ToString(VriResult r) noexcept
    {
        switch (r)
        {
            case VriResult_Success:
                return "Success";
            case VriResult_Failure:
                return "Failure";
            case VriResult_InvalidArgument:
                return "InvalidArgument";
            case VriResult_OutOfMemory:
                return "OutOfMemory";
            case VriResult_Unsupported:
                return "Unsupported";
            case VriResult_DeviceLost:
                return "DeviceLost";
            case VriResult_OutOfDate:
                return "OutOfDate";
            default:
                return "Unknown";
        }
    }

    [[nodiscard]] inline std::unexpected<Error> MakeError(VriResult code, std::string message)
    {
        return std::unexpected(Error {code, std::move(message)});
    }

    [[nodiscard]] inline std::unexpected<Error> MakeError(std::string message)
    {
        return std::unexpected(Error {VriResult_Failure, std::move(message)});
    }

    // Turn a VriResult into an Error, tagging it with `what` (the failing operation).
    [[nodiscard]] inline std::unexpected<Error> MakeError(VriResult code, const char* what, const char* detail)
    {
        std::string message = what;
        message += ": ";
        message += detail;
        message += " (";
        message += ToString(code);
        message += ")";
        return std::unexpected(Error {code, std::move(message)});
    }
} // namespace vrf
