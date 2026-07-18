#include "vrf/gpu/sampler_cache.hpp"

#include "vrf/gpu/render_device.hpp"

namespace vrf
{
    VriDescriptor* SamplerCache::Get(const VriSamplerDesc& desc)
    {
        return m_cache.GetOrCreate(detail::SamplerKey {desc}, [&]() -> Expected<VriDescriptor*> {
            VriDescriptor* sampler = nullptr;
            if (const auto r = m_device->Core().CreateSampler(m_device->Handle(), &desc, &sampler); !Succeeded(r))
            {
                return MakeError(r, "SamplerCache::Get", "CreateSampler failed");
            }
            return sampler;
        });
    }
} // namespace vrf
