#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <vrf/core/math.hpp>
#include <vrf/core/result.hpp>
#include <vrf/platform/window.hpp>

// CPU-only smoke tests: they don't create a device (no GPU/loader required here),
// only exercise the framework's value types and error handling.

TEST_CASE("Extent2D basics")
{
    const vrf::Extent2D extent {1280, 720};
    CHECK(extent.width == 1280);
    CHECK(extent.height == 720);
    CHECK_FALSE(extent.IsZero());
    CHECK(vrf::Extent2D {0, 100}.IsZero());
    CHECK(extent == vrf::Extent2D {1280, 720});
}

TEST_CASE("WindowDesc defaults")
{
    const vrf::WindowDesc desc;
    CHECK(desc.extent.width == 1280);
    CHECK(desc.extent.height == 720);
    CHECK(desc.backend == vrf::WindowBackend::Auto);
    CHECK(desc.resizable);
}

TEST_CASE("Expected success carries a value")
{
    const vrf::Expected<int> ok = 42;
    REQUIRE(ok.has_value());
    CHECK(*ok == 42);
}

TEST_CASE("MakeError produces a tagged failure")
{
    const vrf::Expected<int> bad = vrf::MakeError(VriResult_Unsupported, "SomeOp", "not available");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == VriResult_Unsupported);
    CHECK(bad.error().message.find("SomeOp") != std::string::npos);
    CHECK(bad.error().message.find("Unsupported") != std::string::npos);
}

TEST_CASE("Succeeded / ToString")
{
    CHECK(vrf::Succeeded(VriResult_Success));
    CHECK_FALSE(vrf::Succeeded(VriResult_DeviceLost));
    CHECK(std::string(vrf::ToString(VriResult_OutOfDate)) == "OutOfDate");
}
