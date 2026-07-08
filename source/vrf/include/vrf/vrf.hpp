/*
 * vrf.hpp - umbrella include for the VRI-Framework public API.
 */
#pragma once

#include "vrf/core/log.hpp"
#include "vrf/core/math.hpp"
#include "vrf/core/result.hpp"

#include "vrf/platform/window.hpp"

#include "vrf/gpu/builders/buffer_builder.hpp"
#include "vrf/gpu/builders/graphics_pipeline_builder.hpp"
#include "vrf/gpu/builders/pipeline_layout_builder.hpp"
#include "vrf/gpu/builders/texture_builder.hpp"
#include "vrf/gpu/frame.hpp"
#include "vrf/gpu/render_device.hpp"
#include "vrf/gpu/render_module.hpp"
#include "vrf/gpu/shader_library.hpp"
#include "vrf/gpu/swapchain.hpp"
#include "vrf/gpu/upload.hpp"
#include "vrf/gpu/vertex_layout.hpp"

#include "vrf/asset/gaussian_splat.hpp"
#include "vrf/asset/light.hpp"
#include "vrf/asset/loaders/gaussian_splat_loader.hpp"
#include "vrf/asset/loaders/gltf_loader.hpp"
#include "vrf/asset/loaders/image_loader.hpp"
#include "vrf/asset/loaders/ktx_loader.hpp"
#include "vrf/asset/loaders/obj_loader.hpp"
#include "vrf/asset/material.hpp"
#include "vrf/asset/mesh.hpp"
#include "vrf/asset/texture.hpp"
#include "vrf/asset/vertex.hpp"

#include "vrf/app/application.hpp"
