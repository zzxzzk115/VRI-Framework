#include "vrf/asset/loaders/obj_loader.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

namespace vrf
{
    namespace
    {
        // Pack a (position, normal, texcoord) index triple into a dedup key. Indices are
        // offset by +1 so -1 (absent) maps to 0; 21 bits each (up to ~2M of each) is ample
        // for example assets.
        uint64_t VertexKey(const tinyobj::index_t& index)
        {
            const uint64_t v = static_cast<uint64_t>(index.vertex_index + 1) & 0x1FFFFF;
            const uint64_t n = static_cast<uint64_t>(index.normal_index + 1) & 0x1FFFFF;
            const uint64_t t = static_cast<uint64_t>(index.texcoord_index + 1) & 0x1FFFFF;
            return (v << 42) | (n << 21) | t;
        }
    } // namespace

    Expected<void> LoadObj(std::string_view path, Mesh& out, const ObjImportOptions& options)
    {
        tinyobj::ObjReaderConfig config;
        config.triangulate = options.triangulate;

        tinyobj::ObjReader reader;
        if (!reader.ParseFromFile(std::string(path), config))
            return MakeError("LoadObj failed: " + reader.Error());

        const tinyobj::attrib_t&                attrib    = reader.GetAttrib();
        const std::vector<tinyobj::shape_t>&    shapes    = reader.GetShapes();
        const std::vector<tinyobj::material_t>& materials = reader.GetMaterials();

        out                     = Mesh {};
        out.name                = std::string(path);
        out.attributes          = VertexAttribute::Position;
        const bool hasNormals   = !attrib.normals.empty();
        const bool hasTexCoords = !attrib.texcoords.empty();
        const bool hasColors    = attrib.colors.size() == attrib.vertices.size() && !attrib.colors.empty();
        if (hasNormals)
            out.attributes |= VertexAttribute::Normal;
        if (hasTexCoords)
            out.attributes |= VertexAttribute::TexCoord0;
        if (hasColors)
            out.attributes |= VertexAttribute::Color;

        std::unordered_map<uint64_t, uint32_t> uniqueVertices;

        for (const tinyobj::shape_t& shape : shapes)
        {
            SubMesh sub;
            sub.name         = shape.name;
            sub.vertexOffset = 0; // single shared vertex array; indices are absolute
            sub.indexOffset  = static_cast<uint32_t>(out.indices.size());
            if (!shape.mesh.material_ids.empty())
                sub.materialIndex = shape.mesh.material_ids[0];

            for (const tinyobj::index_t& idx : shape.mesh.indices)
            {
                const uint64_t key = VertexKey(idx);
                auto           it  = uniqueVertices.find(key);
                if (it == uniqueVertices.end())
                {
                    const uint32_t newIndex = static_cast<uint32_t>(out.positions.size());

                    out.positions.emplace_back(attrib.vertices[3 * idx.vertex_index + 0],
                                               attrib.vertices[3 * idx.vertex_index + 1],
                                               attrib.vertices[3 * idx.vertex_index + 2]);
                    if (hasNormals)
                    {
                        if (idx.normal_index >= 0)
                            out.normals.emplace_back(attrib.normals[3 * idx.normal_index + 0],
                                                     attrib.normals[3 * idx.normal_index + 1],
                                                     attrib.normals[3 * idx.normal_index + 2]);
                        else
                            out.normals.emplace_back(0.0f, 0.0f, 0.0f);
                    }
                    if (hasTexCoords)
                    {
                        if (idx.texcoord_index >= 0)
                        {
                            const float u = attrib.texcoords[2 * idx.texcoord_index + 0];
                            float       v = attrib.texcoords[2 * idx.texcoord_index + 1];
                            if (options.flipTexCoordY)
                                v = 1.0f - v;
                            out.texCoords0.emplace_back(u, v);
                        }
                        else
                        {
                            out.texCoords0.emplace_back(0.0f, 0.0f);
                        }
                    }
                    if (hasColors)
                        out.colors.emplace_back(attrib.colors[3 * idx.vertex_index + 0],
                                                attrib.colors[3 * idx.vertex_index + 1],
                                                attrib.colors[3 * idx.vertex_index + 2],
                                                1.0f);

                    uniqueVertices.emplace(key, newIndex);
                    out.indices.push_back(newIndex);
                }
                else
                {
                    out.indices.push_back(it->second);
                }
            }

            sub.indexCount  = static_cast<uint32_t>(out.indices.size()) - sub.indexOffset;
            sub.vertexCount = out.VertexCount();
            out.subMeshes.push_back(std::move(sub));
        }

        out.materials.reserve(materials.size());
        for (const tinyobj::material_t& m : materials)
        {
            // OBJ/MTL is a Phong model (diffuse/specular/shininess/emission/dissolve).
            PhongMaterial phong;
            phong.diffuse   = {m.diffuse[0], m.diffuse[1], m.diffuse[2], m.dissolve};
            phong.specular  = {m.specular[0], m.specular[1], m.specular[2]};
            phong.shininess = m.shininess;
            phong.emissive  = {m.emission[0], m.emission[1], m.emission[2]};
            phong.opacity   = m.dissolve;
            phong.ior       = m.ior;

            Material material;
            material.name = m.name;
            material.core = phong;
            if (m.dissolve < 1.0f)
                material.alphaMode = AlphaMode::Blend;
            out.materials.push_back(std::move(material));
        }

        out.ComputeBounds();
        return {};
    }

    Expected<Mesh> LoadObj(std::string_view path, const ObjImportOptions& options)
    {
        Mesh mesh;
        auto result = LoadObj(path, mesh, options);
        if (!result)
            return std::unexpected(result.error());
        return mesh;
    }
} // namespace vrf
