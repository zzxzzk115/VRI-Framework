#include "vrf/gpu/gpu_profiler.hpp"

#include <cstring>
#include <utility>

#include "vrf/gpu/render_device.hpp"

namespace vrf
{
    namespace
    {
        constexpr uint32_t kUnset = 0xFFFFFFFFu;
    } // namespace

    Expected<GpuProfiler> GpuProfiler::Create(RenderDevice& device, uint32_t framesInFlight, uint32_t maxZonesPerFrame)
    {
        GpuProfiler out;

        // Disabled-but-valid on backends without timestamp queries (MoltenVK often reports none):
        // callers use VRF_GPU_ZONE unconditionally and it compiles to no-ops at runtime.
        if (device.Desc()->hasTimestampQueries == VRI_FALSE || device.Desc()->timestampPeriodNanoseconds <= 0.0f)
        {
            return out;
        }

        if (const auto r = vriGetInterface(device.Handle(), VRI_INTERFACE_QUERY, sizeof(out.m_query), &out.m_query);
            !Succeeded(r))
        {
            return MakeError(r, "GpuProfiler::Create", "vriGetInterface(QUERY) failed");
        }

        const uint32_t framesN = framesInFlight == 0 ? 1 : framesInFlight;
        out.m_maxQueries       = (maxZonesPerFrame == 0 ? 1 : maxZonesPerFrame) * 2; // begin + end per zone
        out.m_periodNs         = device.Desc()->timestampPeriodNanoseconds;
        const auto& core       = device.Core();

        out.m_slots.resize(framesN);
        for (auto& slot : out.m_slots)
        {
            const VriQueryPoolDesc pd {.type = VriQueryType_Timestamp, .queryCount = out.m_maxQueries};
            if (const auto r = out.m_query.CreateQueryPool(device.Handle(), &pd, &slot.pool); !Succeeded(r))
            {
                out.m_device = &device; // so Destroy() cleans up the slots created so far
                return MakeError(r, "GpuProfiler::Create", "CreateQueryPool failed");
            }
            const VriBufferDesc bd {.size           = static_cast<uint64_t>(out.m_maxQueries) * sizeof(uint64_t),
                                    .usage          = VriBufferUsage_TransferDst,
                                    .memoryLocation = VriMemoryLocation_HostReadback};
            if (const auto r = core.CreateBuffer(device.Handle(), &bd, &slot.readback); r != VriResult_Success)
            {
                out.m_device = &device;
                return MakeError(r, "GpuProfiler::Create", "CreateBuffer(HostReadback) failed");
            }
        }
        out.m_device = &device;
        return out;
    }

    void GpuProfiler::Destroy() noexcept
    {
        if (!m_device)
        {
            return;
        }
        const auto& core = m_device->Core();
        for (auto& slot : m_slots)
        {
            if (slot.readback)
            {
                core.DestroyBuffer(slot.readback);
            }
            if (slot.pool)
            {
                m_query.DestroyQueryPool(slot.pool);
            }
        }
        m_slots.clear();
        m_device = nullptr;
    }

    GpuProfiler::~GpuProfiler() { Destroy(); }

    GpuProfiler::GpuProfiler(GpuProfiler&& other) noexcept { *this = std::move(other); }

    GpuProfiler& GpuProfiler::operator=(GpuProfiler&& other) noexcept
    {
        if (this != &other)
        {
            Destroy();
            m_device     = std::exchange(other.m_device, nullptr);
            m_query      = other.m_query;
            m_periodNs   = other.m_periodNs;
            m_maxQueries = other.m_maxQueries;
            m_slots      = std::move(other.m_slots);
            m_current    = other.m_current;
            m_stack      = std::move(other.m_stack);
            m_results    = std::move(other.m_results);
        }
        return *this;
    }

    void GpuProfiler::BeginFrame(VriCommandBuffer* cmd, uint32_t frameIndex)
    {
        if (!m_device)
        {
            return;
        }
        m_current   = frameIndex % static_cast<uint32_t>(m_slots.size());
        Slot& slot  = m_slots[m_current];

        // Resolve the results this slot copied on its previous submit (complete now: FrameStream::Begin
        // waited on the slot's fence before returning). Read back BEFORE resetting the pool.
        m_results.clear();
        if (slot.pending)
        {
            const auto bytes = static_cast<uint64_t>(m_maxQueries) * sizeof(uint64_t);
            if (const auto* ticks = static_cast<const uint64_t*>(m_device->Core().MapBuffer(slot.readback, 0, bytes)))
            {
                for (const auto& rec : slot.records)
                {
                    if (rec.end == kUnset)
                    {
                        continue; // unbalanced zone (missing EndZone)
                    }
                    const uint64_t delta = ticks[rec.end] - ticks[rec.begin];
                    m_results.push_back({rec.name, static_cast<double>(delta) * m_periodNs / 1.0e6, rec.depth});
                }
                m_device->Core().UnmapBuffer(slot.readback);
            }
            slot.pending = false;
        }

        // Fresh cycle: reset must precede any CmdWriteTimestamp (required on Vulkan, outside a pass).
        m_query.CmdResetQueries(cmd, slot.pool, 0, m_maxQueries);
        slot.records.clear();
        slot.next = 0;
        m_stack.clear();
    }

    void GpuProfiler::BeginZone(VriCommandBuffer* cmd, const char* name)
    {
        if (!m_device)
        {
            return;
        }
        Slot&          slot  = m_slots[m_current];
        const uint32_t depth = static_cast<uint32_t>(m_stack.size());
        if (slot.next + 2 > m_maxQueries) // pool full - skip but keep the stack balanced
        {
            m_stack.push_back(kUnset);
            return;
        }
        const uint32_t qBegin = slot.next++;
        m_query.CmdWriteTimestamp(cmd, slot.pool, qBegin);
        slot.records.push_back({name, qBegin, kUnset, depth});
        m_stack.push_back(static_cast<uint32_t>(slot.records.size() - 1));
    }

    void GpuProfiler::EndZone(VriCommandBuffer* cmd)
    {
        if (!m_device || m_stack.empty())
        {
            return;
        }
        const uint32_t recIndex = m_stack.back();
        m_stack.pop_back();
        if (recIndex == kUnset)
        {
            return; // matched a skipped BeginZone
        }
        Slot&          slot = m_slots[m_current];
        const uint32_t qEnd = slot.next++;
        m_query.CmdWriteTimestamp(cmd, slot.pool, qEnd);
        slot.records[recIndex].end = qEnd;
    }

    void GpuProfiler::EndFrame(VriCommandBuffer* cmd)
    {
        if (!m_device)
        {
            return;
        }
        Slot& slot = m_slots[m_current];
        if (slot.next > 0)
        {
            m_query.CmdCopyQueries(cmd, slot.pool, 0, slot.next, slot.readback, 0);
            slot.pending = true;
        }
    }

    double GpuProfiler::LastFrameMs() const noexcept
    {
        double total = 0.0;
        for (const auto& z : m_results)
        {
            if (z.depth == 0)
            {
                total += z.milliseconds;
            }
        }
        return total;
    }
} // namespace vrf
