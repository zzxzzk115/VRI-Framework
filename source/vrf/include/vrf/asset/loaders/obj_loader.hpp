/*
 * obj_loader.hpp - load a Wavefront OBJ into a vrf::Mesh via tinyobjloader.
 */
#pragma once

#include <string_view>

#include "vrf/asset/mesh.hpp"
#include "vrf/core/result.hpp"

namespace vrf
{
    struct ObjImportOptions
    {
        bool triangulate   = true;
        bool flipTexCoordY = false;
    };

    [[nodiscard]] Expected<void> LoadObj(std::string_view path, Mesh& out, const ObjImportOptions& options = {});

    [[nodiscard]] Expected<Mesh> LoadObj(std::string_view path, const ObjImportOptions& options = {});
} // namespace vrf
