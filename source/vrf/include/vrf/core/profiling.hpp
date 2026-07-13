/*
 * profiling.hpp - Tracy zone macros, compiled to nothing without
 * vrf_with_tracy (VRF_WITH_TRACY). Use VRF_ZONE("name") for a scoped CPU zone
 * and VRF_FRAME_MARK() once per presented/submitted frame.
 */
#pragma once

#if defined(VRF_WITH_TRACY)

#include <tracy/Tracy.hpp>

#define VRF_ZONE(name) ZoneScopedN(name)
#define VRF_FRAME_MARK() FrameMark

#else

#define VRF_ZONE(name)
#define VRF_FRAME_MARK()

#endif
