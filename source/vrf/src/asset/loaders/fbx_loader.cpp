#include "vrf/asset/loaders/fbx_loader.hpp"

#ifdef VRF_ENABLE_FBX
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_map>

#include <glm/gtc/type_ptr.hpp>
#include <ofbx.h>

#include "vrf/asset/loaders/ktx_loader.hpp"

namespace vrf
{
    namespace
    {
        std::string Text(ofbx::DataView value)
        {
            return value.begin ? std::string(reinterpret_cast<const char*>(value.begin),
                                             reinterpret_cast<const char*>(value.end)) :
                                 std::string {};
        }

        struct VertexKey
        {
            int  position, normal, uv;
            bool operator==(const VertexKey&) const = default;
        };
        struct VertexHash
        {
            size_t operator()(const VertexKey& key) const
            {
                size_t result = static_cast<size_t>(key.position);
                result        = result * 16777619u ^ static_cast<size_t>(key.normal);
                return result * 16777619u ^ static_cast<size_t>(key.uv);
            }
        };

        glm::dmat4 Matrix(const ofbx::DMatrix& value) { return glm::make_mat4(value.m); }

        float Opacity(const ofbx::Material& material)
        {
            // OpenFBX v0.9 has no transparency accessor; retain the authored FBX property.
            for (auto* group = material.element.getFirstChild(); group; group = group->getSibling())
            {
                if (Text(group->getID()) != "Properties70")
                    continue;
                for (auto* entry = group->getFirstChild(); entry; entry = entry->getSibling())
                {
                    auto* value = entry->getFirstProperty();
                    if (Text(entry->getID()) != "P" || !value || Text(value->getValue()) != "TransparencyFactor")
                        continue;
                    for (int i = 0; i < 4 && value; ++i)
                        value = value->getNext();
                    if (value)
                        return std::clamp(1.0f - static_cast<float>(value->getValue().toDouble()), 0.0f, 1.0f);
                }
            }
            return 1.0f;
        }
    } // namespace

    Expected<void> LoadFbx(std::string_view path, Mesh& out, const FbxImportOptions& options)
    {
        namespace fs = std::filesystem;
        std::ifstream file(fs::path(path), std::ios::binary | std::ios::ate);
        if (!file || file.tellg() <= 0)
            return MakeError("LoadFbx: cannot read " + std::string(path));
        std::vector<ofbx::u8> bytes(static_cast<size_t>(file.tellg()));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
            return MakeError("LoadFbx: incomplete file " + std::string(path));
        const auto flags =
            ofbx::LoadFlags::IGNORE_ANIMATIONS | ofbx::LoadFlags::IGNORE_CAMERAS | ofbx::LoadFlags::IGNORE_LIGHTS;
        const auto                                       destroy = [](ofbx::IScene* scene) { scene->destroy(); };
        std::unique_ptr<ofbx::IScene, decltype(destroy)> scene(
            ofbx::load(bytes.data(), bytes.size(), static_cast<ofbx::u16>(flags)), destroy);
        if (!scene)
            return MakeError("LoadFbx: " + std::string(ofbx::getError()));

        Mesh mesh;
        mesh.name       = std::string(path);
        mesh.attributes = VertexAttribute::Position | VertexAttribute::Normal | VertexAttribute::TexCoord0;
        std::unordered_map<const ofbx::Material*, int> materials;
        std::unordered_map<std::string, int>           textures;
        const auto                                     base = fs::path(path).parent_path();
        const auto loadTexture = [&](const ofbx::Texture* texture) -> Expected<TextureRef> {
            if (!texture || !options.loadTextures)
                return TextureRef {};
            auto name = Text(texture->getRelativeFileName());
            if (name.empty())
                name = Text(texture->getFileName());
            std::replace(name.begin(), name.end(), '\\', '/');
            fs::path resolved = base / name;
            if (!fs::is_regular_file(resolved))
                resolved = base / fs::path(name).filename();
            if (!fs::is_regular_file(resolved))
                return MakeError("LoadFbx: missing texture " + name);
            const auto key = fs::weakly_canonical(resolved).generic_string();
            if (const auto found = textures.find(key); found != textures.end())
                return TextureRef {found->second};
            Texture loaded;
            if (auto result = LoadTexture(key, loaded); !result)
                return std::unexpected(result.error());
            const int index = static_cast<int>(mesh.textures.size());
            mesh.textures.push_back(std::move(loaded));
            textures.emplace(key, index);
            return TextureRef {index};
        };
        const auto loadMaterial = [&](const ofbx::Material* source) -> Expected<int> {
            if (!source)
                return -1;
            if (const auto found = materials.find(source); found != materials.end())
                return found->second;
            Material material;
            material.name = source->name;
            std::array<TextureRef, ofbx::Texture::COUNT> refs;
            for (int slot = 0; slot < ofbx::Texture::COUNT; ++slot)
            {
                auto texture = loadTexture(source->getTexture(static_cast<ofbx::Texture::TextureType>(slot)));
                if (!texture)
                    return std::unexpected(texture.error());
                refs[slot] = *texture;
            }
            const auto diffuse  = source->getDiffuseColor();
            const auto emissive = source->getEmissiveColor();
            if (options.materialConvention == FbxMaterialConvention::OrcaMetallicRoughness)
            {
                PbrMetallicRoughnessMaterial pbr;
                pbr.baseColorFactor          = {diffuse.r, diffuse.g, diffuse.b, 1.0f};
                pbr.baseColorTexture         = refs[ofbx::Texture::DIFFUSE];
                pbr.normalTexture            = refs[ofbx::Texture::NORMAL];
                pbr.metallicRoughnessTexture = refs[ofbx::Texture::SPECULAR];
                // Falcor does not consume this reserved channel. Bistro v5.2 has
                // zero R throughout: binding it as glTF AO would black out ambient light.
                pbr.metallicFactor  = pbr.metallicRoughnessTexture.Valid() ? 1.0f : 0.0f;
                pbr.emissiveTexture = refs[ofbx::Texture::EMISSIVE];
                pbr.emissiveFactor =
                    pbr.emissiveTexture.Valid() ? Vec3(1.0f) : Vec3(emissive.r, emissive.g, emissive.b);
                material.core = pbr;
                // ORCA stores opacity in base-color alpha, including leaf cutouts.
                material.alphaMode   = pbr.baseColorTexture.Valid() ? AlphaMode::Mask : AlphaMode::Opaque;
                material.doubleSided = material.name.ends_with(".DoubleSided");
            }
            else
            {
                PhongMaterial phong;
                phong.opacity       = Opacity(*source);
                const auto specular = source->getSpecularColor();
                phong.diffuse =
                    Vec4(Vec3(diffuse.r, diffuse.g, diffuse.b) * static_cast<float>(source->getDiffuseFactor()),
                         phong.opacity);
                phong.specular =
                    Vec3(specular.r, specular.g, specular.b) * static_cast<float>(source->getSpecularFactor());
                phong.shininess = static_cast<float>(source->getShininess());
                phong.emissive =
                    Vec3(emissive.r, emissive.g, emissive.b) * static_cast<float>(source->getEmissiveFactor());
                phong.diffuseTexture  = refs[ofbx::Texture::DIFFUSE];
                phong.specularTexture = refs[ofbx::Texture::SPECULAR];
                phong.normalTexture   = refs[ofbx::Texture::NORMAL];
                phong.emissiveTexture = refs[ofbx::Texture::EMISSIVE];
                material.core         = phong;
                if (phong.opacity < 1.0f)
                    material.alphaMode = AlphaMode::Blend;
            }
            const int index = static_cast<int>(mesh.materials.size());
            mesh.materials.push_back(std::move(material));
            materials.emplace(source, index);
            return index;
        };

        const double scale = options.convertToMeters ? scene->getGlobalSettings()->UnitScaleFactor * 0.01 : 1.0;
        if (!std::isfinite(scale) || scale <= 0)
            return MakeError("LoadFbx: invalid unit scale");
        for (int m = 0; m < scene->getMeshCount(); ++m)
        {
            const auto* source    = scene->getMesh(m);
            const auto& data      = source->getGeometryData();
            const auto  positions = data.getPositions();
            const auto  normals   = data.getNormals();
            const auto  uvs       = data.getUVs();
            if (!positions.count)
                continue;
            if (normals.count != positions.count || uvs.count != positions.count)
                return MakeError("LoadFbx: static import requires per-corner normals and UVs: " +
                                 std::string(source->name));
            const glm::dmat4 transform   = Matrix(source->getGlobalTransform()) * Matrix(source->getGeometricMatrix());
            const double     determinant = glm::determinant(glm::dmat3(transform));
            if (!std::isfinite(determinant) || std::abs(determinant) < 1e-15)
                return MakeError("LoadFbx: singular mesh transform");
            const auto normalTransform = glm::transpose(glm::inverse(glm::dmat3(transform)));
            std::unordered_map<VertexKey, uint32_t, VertexHash> vertices;
            for (int partitionIndex = 0; partitionIndex < data.getPartitionCount(); ++partitionIndex)
            {
                const auto partition = data.getPartition(partitionIndex);
                if (!partition.polygon_count)
                    continue;
                SubMesh sub;
                sub.name        = source->name;
                sub.indexOffset = static_cast<uint32_t>(mesh.indices.size());
                auto material   = loadMaterial(
                    partitionIndex < source->getMaterialCount() ? source->getMaterial(partitionIndex) : nullptr);
                if (!material)
                    return std::unexpected(material.error());
                sub.materialIndex = *material;
                for (int polygonIndex = 0; polygonIndex < partition.polygon_count; ++polygonIndex)
                {
                    const auto& polygon = partition.polygons[polygonIndex];
                    if (polygon.vertex_count < 3 || polygon.vertex_count > 4)
                        return MakeError("LoadFbx: only triangles and convex quads are supported");
                    int            corners[6];
                    const uint32_t count = ofbx::triangulate(data, polygon, corners);
                    if (determinant < 0)
                        for (uint32_t t = 0; t < count; t += 3)
                            std::swap(corners[t + 1], corners[t + 2]);
                    for (uint32_t c = 0; c < count; ++c)
                    {
                        const int corner = corners[c];
                        if (corner < 0 || corner >= positions.count)
                            return MakeError("LoadFbx: invalid polygon corner");
                        VertexKey key {positions.indices ? positions.indices[corner] : corner,
                                       normals.indices ? normals.indices[corner] : corner,
                                       uvs.indices ? uvs.indices[corner] : corner};
                        auto [found, inserted] =
                            vertices.try_emplace(key, static_cast<uint32_t>(mesh.positions.size()));
                        if (inserted)
                        {
                            const auto p      = positions.get(corner);
                            const auto n      = normals.get(corner);
                            const auto uv     = uvs.get(corner);
                            const auto world  = transform * glm::dvec4(p.x, p.y, p.z, 1.0);
                            const auto normal = normalTransform * glm::dvec3(n.x, n.y, n.z);
                            if (!std::isfinite(world.x + world.y + world.z) || glm::length(normal) < 1e-12)
                                return MakeError("LoadFbx: invalid position or normal");
                            mesh.positions.emplace_back(glm::dvec3(world) * scale);
                            mesh.normals.emplace_back(glm::normalize(normal));
                            mesh.texCoords0.emplace_back(uv.x, options.flipTexCoordY ? 1.0f - uv.y : uv.y);
                        }
                        mesh.indices.push_back(found->second);
                    }
                }
                sub.indexCount  = static_cast<uint32_t>(mesh.indices.size()) - sub.indexOffset;
                sub.vertexCount = mesh.VertexCount();
                mesh.subMeshes.push_back(std::move(sub));
            }
        }
        if (mesh.positions.empty())
            return MakeError("LoadFbx: no mesh geometry");
        mesh.tangents = GenerateTangents(mesh);
        // GenerateTangents uses the imported UVs. Flipping V already reverses
        // its bitangent, which compensates a DirectX map's negative green.
        if (options.directXNormalMaps != options.flipTexCoordY)
            for (auto& tangent : mesh.tangents)
                tangent.w = -tangent.w;
        mesh.attributes |= VertexAttribute::Tangent;
        mesh.ComputeBounds();
        out = std::move(mesh);
        return {};
    }
} // namespace vrf
#else
namespace vrf
{
    Expected<void> LoadFbx(std::string_view, Mesh&, const FbxImportOptions&)
    {
        return MakeError("LoadFbx: build with vrf_loader_fbx enabled");
    }
} // namespace vrf
#endif
