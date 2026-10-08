#include <doctest/doctest.h>

#include <vrf/gpu/gpu_profiler.hpp>
#include <vrf/gpu/render_device.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    std::vector<std::string> g_events;
    void VRI_CALL            Begin(VriCommandBuffer*, const char* name) { g_events.emplace_back(name); }
    void VRI_CALL            End(VriCommandBuffer*) { g_events.emplace_back("end"); }
} // namespace

TEST_CASE("GPU debug scopes remain balanced through nested exception unwinding")
{
    VriCoreInterface core {};
    core.CmdBeginDebugGroup = Begin;
    core.CmdEndDebugGroup   = End;
    auto* cmd               = reinterpret_cast<VriCommandBuffer*>(uintptr_t {1});
    g_events.clear();
    try
    {
        vrf::GpuDebugGroup outer {core, cmd, "outer"};
        vrf::GpuDebugGroup inner {core, cmd, "inner"};
        throw std::runtime_error("pass recording failed");
    }
    catch (const std::runtime_error&)
    {}
    REQUIRE(g_events.size() == 4);
    CHECK(g_events[0] == "outer");
    CHECK(g_events[1] == "inner");
    CHECK(g_events[2] == "end");
    CHECK(g_events[3] == "end");
    {
        vrf::GpuDebugGroup nullCommand {core, nullptr, "ignored"};
        core.CmdEndDebugGroup = nullptr;
        vrf::GpuDebugGroup unavailable {core, cmd, "ignored"};
    }
    CHECK(g_events.size() == 4);
}

TEST_CASE("GPU profiler keeps markers when timestamps are disabled and after moving")
{
    vrf::RenderDeviceDesc desc;
    desc.api        = vrf::GraphicsApi::Vulkan;
    desc.validation = false;
    auto device     = vrf::RenderDevice::Create(desc);
    if (!device)
    {
        MESSAGE("Vulkan unavailable - skipped");
        return;
    }
    auto& core = const_cast<VriCoreInterface&>(device->Core());
    struct Restore
    {
        VriCoreInterface& core;
        VriCoreInterface  saved;
        ~Restore() { core = saved; }
    } restore {core, core};
    core.CmdBeginDebugGroup = Begin;
    core.CmdEndDebugGroup   = End;
    auto result             = vrf::GpuProfiler::Create(*device, 1, 1, false);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->Enabled());
    auto* cmd = reinterpret_cast<VriCommandBuffer*>(uintptr_t {1});
    g_events.clear();
    result->BeginFrame(cmd, 0);
    result->BeginZone(cmd, "outer");
    result->BeginZone(cmd, "inner");
    auto moved = std::move(*result);
    result->EndZone(cmd); // moved-from profiler must not emit an unmatched end
    moved.EndZone(cmd);
    moved.EndZone(cmd);
    moved.EndZone(cmd); // unmatched end is ignored
    moved.EndFrame(cmd);
    REQUIRE(g_events.size() == 4);
    CHECK(g_events[0] == "outer");
    CHECK(g_events[1] == "inner");
    CHECK(g_events[2] == "end");
    CHECK(g_events[3] == "end");
    CHECK(moved.Results().empty());
}

TEST_CASE("GPU profiler reserves outer end timestamps when nested zones exhaust the pool")
{
    vrf::RenderDeviceDesc desc;
    desc.api        = vrf::GraphicsApi::Vulkan;
    desc.validation = true;
    auto device     = vrf::RenderDevice::Create(desc);
    if (!device)
    {
        MESSAGE("Vulkan unavailable - skipped");
        return;
    }
    auto profiler = vrf::GpuProfiler::Create(*device, 1, 1);
    REQUIRE(profiler.has_value());
    if (!profiler->Enabled())
    {
        MESSAGE("Timestamps unavailable - skipped");
        return;
    }
    auto& core = const_cast<VriCoreInterface&>(device->Core());
    struct Restore
    {
        VriCoreInterface& core;
        VriCoreInterface  saved;
        ~Restore() { core = saved; }
    } restore {core, core};
    core.CmdBeginDebugGroup        = Begin;
    core.CmdEndDebugGroup          = End;
    VriCommandAllocator* allocator = nullptr;
    VriCommandBuffer*    cmd       = nullptr;
    VriFence*            fence     = nullptr;
    REQUIRE(core.CreateCommandAllocator(device->Handle(), VriQueueType_Graphics, &allocator) == VriResult_Success);
    REQUIRE(core.CreateCommandBuffer(allocator, &cmd) == VriResult_Success);
    REQUIRE(core.CreateFence(device->Handle(), 0, &fence) == VriResult_Success);
    REQUIRE(core.BeginCommandBuffer(cmd) == VriResult_Success);
    profiler->BeginFrame(cmd, 0);
    g_events.clear();
    profiler->BeginZone(cmd, "outer");
    profiler->BeginZone(cmd, "overflow-inner");
    profiler->EndZone(cmd);
    profiler->EndZone(cmd);
    CHECK(g_events.size() == 4); // timing capacity must not suppress or unbalance markers
    profiler->EndFrame(cmd);
    REQUIRE(core.EndCommandBuffer(cmd) == VriResult_Success);
    const VriFenceSubmitDesc signal {fence, 1};
    VriQueueSubmitDesc       submit {};
    submit.commandBuffers   = &cmd;
    submit.commandBufferNum = 1;
    submit.signalFences     = &signal;
    submit.signalFenceNum   = 1;
    core.QueueSubmit(device->GraphicsQueue(), &submit);
    core.Wait(fence, 1);

    REQUIRE(core.BeginCommandBuffer(cmd) == VriResult_Success);
    profiler->BeginFrame(cmd, 0);
    REQUIRE(profiler->Results().size() == 1);
    CHECK(profiler->Results()[0].name == "outer");
    CHECK(profiler->Results()[0].milliseconds >= 0.0);
    profiler->EndFrame(cmd);
    REQUIRE(core.EndCommandBuffer(cmd) == VriResult_Success);
    core.DestroyCommandAllocator(allocator);
    core.DestroyFence(fence);
}
