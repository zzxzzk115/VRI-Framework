#include "vrf/asset/loaders/gltf_loader.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "vrf/core/log.hpp"

// The single STB_IMAGE_IMPLEMENTATION lives in image_loader.cpp; here we tell
// tinygltf not to include stb_image itself but still use it (we provide the
// declarations), so there is exactly one stb implementation in the library.
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_INCLUDE_STB_IMAGE
#if defined(VRF_ENABLE_DRACO)
// tinygltf decodes KHR_draco_mesh_compression into normal accessors when this is set + draco linked.
#define TINYGLTF_ENABLE_DRACO
#endif
#include <stb_image.h>
#include <tiny_gltf.h>

namespace vrf
{
    namespace
    {
        // Read a vertex-attribute accessor into a flat float array; returns the component
        // count per element (2/3/4). Handles float + normalized integer component types and
        // interleaved byte strides. Sparse accessors are not supported (returns 0).
        int ReadAccessorFloats(const tinygltf::Model& model, int accessorIndex, std::vector<float>& out)
        {
            out.clear();
            if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size()))
                return 0;
            const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
            const int                 comps    = tinygltf::GetNumComponentsInType(static_cast<uint32_t>(accessor.type));
            // An accessor with no bufferView (or a sparse one we don't apply) is spec-legal and
            // reads as zeros - do that instead of indexing bufferViews[-1] (which would crash).
            if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size()))
            {
                out.assign(accessor.count * static_cast<size_t>(comps), 0.0f);
                return comps;
            }
            const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
            if (view.buffer < 0 || view.buffer >= static_cast<int>(model.buffers.size()))
                return 0;
            const tinygltf::Buffer& buffer = model.buffers[view.buffer];

            const int    compType = accessor.componentType;
            const size_t compSize =
                static_cast<size_t>(tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(compType)));
            const size_t stride     = static_cast<size_t>(accessor.ByteStride(view));
            const size_t baseOffset = view.byteOffset + accessor.byteOffset;
            // Bounds-check the last byte the loop will touch; bail if the accessor overruns the buffer.
            if (accessor.count > 0 && stride > 0)
            {
                const size_t lastByte =
                    baseOffset + (accessor.count - 1) * stride + static_cast<size_t>(comps - 1) * compSize + compSize;
                if (lastByte > buffer.data.size())
                {
                    out.assign(accessor.count * static_cast<size_t>(comps), 0.0f);
                    return comps;
                }
            }
            const unsigned char* base = buffer.data.data() + baseOffset;

            out.resize(accessor.count * static_cast<size_t>(comps));
            for (size_t i = 0; i < accessor.count; ++i)
            {
                const unsigned char* element = base + i * stride;
                for (int c = 0; c < comps; ++c)
                {
                    const unsigned char* comp  = element + static_cast<size_t>(c) * compSize;
                    float                value = 0.0f;
                    switch (compType)
                    {
                        case TINYGLTF_COMPONENT_TYPE_FLOAT:
                            std::memcpy(&value, comp, sizeof(float));
                            break;
                        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                            value = accessor.normalized ? (*comp) / 255.0f : static_cast<float>(*comp);
                            break;
                        case TINYGLTF_COMPONENT_TYPE_BYTE: {
                            const int8_t v = static_cast<int8_t>(*comp);
                            value          = accessor.normalized ? v / 127.0f : static_cast<float>(v);
                            break;
                        }
                        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                            uint16_t v = 0;
                            std::memcpy(&v, comp, sizeof(v));
                            value = accessor.normalized ? v / 65535.0f : static_cast<float>(v);
                            break;
                        }
                        case TINYGLTF_COMPONENT_TYPE_SHORT: {
                            int16_t v = 0;
                            std::memcpy(&v, comp, sizeof(v));
                            value = accessor.normalized ? v / 32767.0f : static_cast<float>(v);
                            break;
                        }
                        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
                            uint32_t v = 0;
                            std::memcpy(&v, comp, sizeof(v));
                            value = static_cast<float>(v);
                            break;
                        }
                        default:
                            break;
                    }
                    out[i * static_cast<size_t>(comps) + static_cast<size_t>(c)] = value;
                }
            }
            return comps;
        }

        void ReadIndices(const tinygltf::Model& model, int accessorIndex, std::vector<uint32_t>& out)
        {
            out.clear();
            if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size()))
                return;
            const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
            if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size()))
                return;
            const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
            if (view.buffer < 0 || view.buffer >= static_cast<int>(model.buffers.size()))
                return;
            const tinygltf::Buffer& buffer     = model.buffers[view.buffer];
            const size_t            stride     = static_cast<size_t>(accessor.ByteStride(view));
            const size_t            baseOffset = view.byteOffset + accessor.byteOffset;
            const size_t            compSize =
                static_cast<size_t>(tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.componentType)));
            if (accessor.count > 0 && stride > 0 &&
                baseOffset + (accessor.count - 1) * stride + compSize > buffer.data.size())
                return; // accessor overruns the buffer
            const unsigned char* base = buffer.data.data() + baseOffset;

            out.resize(accessor.count);
            for (size_t i = 0; i < accessor.count; ++i)
            {
                const unsigned char* comp = base + i * stride;
                switch (accessor.componentType)
                {
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                        out[i] = *comp;
                        break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                        uint16_t v = 0;
                        std::memcpy(&v, comp, sizeof(v));
                        out[i] = v;
                        break;
                    }
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
                        uint32_t v = 0;
                        std::memcpy(&v, comp, sizeof(v));
                        out[i] = v;
                        break;
                    }
                    default:
                        out[i] = 0;
                        break;
                }
            }
        }

        int TextureImageIndex(const tinygltf::Model& model, int textureIndex)
        {
            if (textureIndex < 0 || textureIndex >= static_cast<int>(model.textures.size()))
                return -1;
            return model.textures[textureIndex].source;
        }

        // A glTF node's LOCAL transform: either an explicit column-major 4x4 matrix, or the
        // T * R * S composition. (A node has one or the other, never both.)
        glm::mat4 NodeLocalMatrix(const tinygltf::Node& node)
        {
            if (node.matrix.size() == 16)
            {
                glm::mat4 m(1.0f);
                for (int col = 0; col < 4; ++col)
                    for (int row = 0; row < 4; ++row)
                        m[col][row] = static_cast<float>(node.matrix[static_cast<size_t>(col) * 4 + row]);
                return m;
            }
            glm::mat4 m(1.0f);
            if (node.translation.size() == 3)
                m = glm::translate(m,
                                   glm::vec3(static_cast<float>(node.translation[0]),
                                             static_cast<float>(node.translation[1]),
                                             static_cast<float>(node.translation[2])));
            if (node.rotation.size() == 4) // glTF stores the quaternion as (x, y, z, w); glm::quat is (w, x, y, z)
                m = m * glm::mat4_cast(glm::quat(static_cast<float>(node.rotation[3]),
                                                 static_cast<float>(node.rotation[0]),
                                                 static_cast<float>(node.rotation[1]),
                                                 static_cast<float>(node.rotation[2])));
            if (node.scale.size() == 3)
                m = glm::scale(m,
                               glm::vec3(static_cast<float>(node.scale[0]),
                                         static_cast<float>(node.scale[1]),
                                         static_cast<float>(node.scale[2])));
            return m;
        }

        // Flatten one glTF extension Value tree into a typed MaterialExtension (lossless for
        // scalars / vectors / strings / texture refs). A one-level object with an "index" is a
        // glTF textureInfo -> TextureRef; other nested objects are skipped (rare in materials).
        template<class TexRefFn>
        MaterialExtension ParseGltfExtension(const tinygltf::Value& value, TexRefFn&& texRef)
        {
            MaterialExtension ext;
            if (!value.IsObject())
                return ext;
            for (const std::string& key : value.Keys())
            {
                const tinygltf::Value& v = value.Get(key);
                if (v.IsBool())
                    ext.Set(key, v.Get<bool>());
                else if (v.IsNumber())
                    ext.Set(key, static_cast<float>(v.GetNumberAsDouble()));
                else if (v.IsString())
                    ext.Set(key, v.Get<std::string>());
                else if (v.IsArray())
                {
                    const int  n   = static_cast<int>(v.ArrayLen());
                    const auto num = [&](int i) { return static_cast<float>(v.Get(i).GetNumberAsDouble()); };
                    if (n == 2)
                        ext.Set(key, Vec2(num(0), num(1)));
                    else if (n == 3)
                        ext.Set(key, Vec3(num(0), num(1), num(2)));
                    else if (n == 4)
                        ext.Set(key, Vec4(num(0), num(1), num(2), num(3)));
                }
                else if (v.IsObject() && v.Has("index") && v.Get("index").IsNumber())
                {
                    ext.Set(key, texRef(v.Get("index").GetNumberAsInt()));
                }
            }
            return ext;
        }

        MaterialFeature FeatureForExtension(const std::string& name)
        {
            if (name == ext_name::Clearcoat)
                return MaterialFeature::Clearcoat;
            if (name == ext_name::Transmission)
                return MaterialFeature::Transmission;
            if (name == ext_name::Volume)
                return MaterialFeature::Volume;
            if (name == ext_name::Sheen)
                return MaterialFeature::Sheen;
            if (name == ext_name::Specular)
                return MaterialFeature::Specular;
            if (name == ext_name::Ior)
                return MaterialFeature::Ior;
            if (name == ext_name::Iridescence)
                return MaterialFeature::Iridescence;
            if (name == ext_name::Anisotropy)
                return MaterialFeature::Anisotropy;
            if (name == ext_name::EmissiveStrength)
                return MaterialFeature::EmissiveStrength;
            return MaterialFeature::None;
        }
    } // namespace

    Expected<void> LoadGltf(std::string_view path, Mesh& out, const GltfImportOptions& options)
    {
        const std::string filePath(path);

        tinygltf::Model    model;
        tinygltf::TinyGLTF loader;
        std::string        err;
        std::string        warn;

        const bool isBinary = filePath.size() >= 4 && filePath.compare(filePath.size() - 4, 4, ".glb") == 0;
        const bool ok       = isBinary ? loader.LoadBinaryFromFile(&model, &err, &warn, filePath) :
                                         loader.LoadASCIIFromFile(&model, &err, &warn, filePath);
        if (!warn.empty())
            LogWarning("LoadGltf: {}", warn);
        if (!ok)
            return MakeError("LoadGltf failed: " + (err.empty() ? std::string("unknown error") : err));

        out      = Mesh {};
        out.name = filePath;

#if !defined(VRF_ENABLE_DRACO)
        // Bail cleanly on required extensions we can't honor (rather than producing garbage).
        // KHR_draco_mesh_compression stores geometry Draco-encoded with no plain bufferView;
        // decoding needs the Draco library (enable the vrf_loader_draco option to pull it in,
        // which lets tinygltf decode it into normal accessors transparently).
        for (const std::string& required : model.extensionsRequired)
        {
            if (required == "KHR_draco_mesh_compression")
                return MakeError(VriResult_Unsupported,
                                 "LoadGltf",
                                 "Draco-compressed geometry (KHR_draco_mesh_compression) is not supported "
                                 "(enable the vrf_loader_draco option)");
        }
#endif

        // ---- textures (decoded by tinygltf into image.image) ----
        if (options.loadTextures)
        {
            out.textures.reserve(model.images.size());
            for (const tinygltf::Image& image : model.images)
            {
                Texture texture;
                texture.name        = image.name;
                texture.width       = static_cast<uint32_t>(image.width);
                texture.height      = static_cast<uint32_t>(image.height);
                texture.depth       = 1;
                texture.mipLevels   = 1;
                texture.arrayLayers = 1;
                texture.format      = VriFormat_RGBA8_UNORM;
                texture.fileFormat  = TextureFileFormat::Unknown;

                if (!image.image.empty() && image.width > 0 && image.height > 0)
                {
                    const size_t pixelCount = static_cast<size_t>(image.width) * image.height;
                    texture.data.resize(pixelCount * 4);
                    if (image.component == 4)
                    {
                        std::memcpy(texture.data.data(), image.image.data(), pixelCount * 4);
                    }
                    else
                    {
                        const int comp = image.component;
                        for (size_t i = 0; i < pixelCount; ++i)
                        {
                            const unsigned char r   = image.image[i * comp + 0];
                            const unsigned char g   = comp > 1 ? image.image[i * comp + 1] : r;
                            const unsigned char b   = comp > 2 ? image.image[i * comp + 2] : r;
                            texture.data[i * 4 + 0] = r;
                            texture.data[i * 4 + 1] = g;
                            texture.data[i * 4 + 2] = b;
                            texture.data[i * 4 + 3] = 255;
                        }
                    }
                }
                out.textures.push_back(std::move(texture));
            }
        }

        // ---- materials (routed to the shading model the glTF actually describes) ----
        out.materials.reserve(model.materials.size());
        for (const tinygltf::Material& gm : model.materials)
        {
            Material material;
            material.name        = gm.name;
            material.doubleSided = gm.doubleSided;
            material.alphaCutoff = static_cast<float>(gm.alphaCutoff);
            if (gm.alphaMode == "MASK")
                material.alphaMode = AlphaMode::Mask;
            else if (gm.alphaMode == "BLEND")
                material.alphaMode = AlphaMode::Blend;

            const auto texRef = [&](int textureIndex) -> TextureRef {
                return TextureRef {options.loadTextures ? TextureImageIndex(model, textureIndex) : -1};
            };

            const bool isUnlit   = gm.extensions.find("KHR_materials_unlit") != gm.extensions.end();
            const auto specGloss = gm.extensions.find("KHR_materials_pbrSpecularGlossiness");

            const tinygltf::PbrMetallicRoughness& pbr = gm.pbrMetallicRoughness;

            if (isUnlit)
            {
                UnlitMaterial unlit;
                unlit.color        = Vec4(static_cast<float>(pbr.baseColorFactor[0]),
                                   static_cast<float>(pbr.baseColorFactor[1]),
                                   static_cast<float>(pbr.baseColorFactor[2]),
                                   static_cast<float>(pbr.baseColorFactor[3]));
                unlit.colorTexture = texRef(pbr.baseColorTexture.index);
                material.core      = unlit;
            }
            else if (specGloss != gm.extensions.end())
            {
                const tinygltf::Value&        ext = specGloss->second;
                PbrSpecularGlossinessMaterial sg;

                const auto extVec = [](const tinygltf::Value& value, int count, glm::vec4 fallback) -> glm::vec4 {
                    if (!value.IsArray() || static_cast<int>(value.ArrayLen()) < count)
                        return fallback;
                    glm::vec4 out = fallback;
                    for (int i = 0; i < count; ++i)
                        out[i] = static_cast<float>(value.Get(i).GetNumberAsDouble());
                    return out;
                };
                const auto extTexture = [&](const char* key) -> TextureRef {
                    if (!ext.Has(key))
                        return {};
                    const tinygltf::Value& t = ext.Get(std::string(key));
                    if (!t.Has("index"))
                        return {};
                    return texRef(t.Get("index").GetNumberAsInt());
                };

                if (ext.Has("diffuseFactor"))
                    sg.diffuseFactor = extVec(ext.Get("diffuseFactor"), 4, glm::vec4(1.0f));
                if (ext.Has("specularFactor"))
                {
                    const glm::vec4 s = extVec(ext.Get("specularFactor"), 3, glm::vec4(1.0f));
                    sg.specularFactor = Vec3(s.x, s.y, s.z);
                }
                if (ext.Has("glossinessFactor"))
                    sg.glossinessFactor = static_cast<float>(ext.Get("glossinessFactor").GetNumberAsDouble());
                sg.diffuseTexture            = extTexture("diffuseTexture");
                sg.specularGlossinessTexture = extTexture("specularGlossinessTexture");
                sg.normalTexture             = texRef(gm.normalTexture.index);
                material.core                = sg;
            }
            else
            {
                PbrMetallicRoughnessMaterial mr;
                mr.baseColorFactor          = Vec4(static_cast<float>(pbr.baseColorFactor[0]),
                                          static_cast<float>(pbr.baseColorFactor[1]),
                                          static_cast<float>(pbr.baseColorFactor[2]),
                                          static_cast<float>(pbr.baseColorFactor[3]));
                mr.metallicFactor           = static_cast<float>(pbr.metallicFactor);
                mr.roughnessFactor          = static_cast<float>(pbr.roughnessFactor);
                mr.emissiveFactor           = Vec3(static_cast<float>(gm.emissiveFactor[0]),
                                         static_cast<float>(gm.emissiveFactor[1]),
                                         static_cast<float>(gm.emissiveFactor[2]));
                mr.baseColorTexture         = texRef(pbr.baseColorTexture.index);
                mr.metallicRoughnessTexture = texRef(pbr.metallicRoughnessTexture.index);
                mr.normalTexture            = texRef(gm.normalTexture.index);
                mr.occlusionTexture         = texRef(gm.occlusionTexture.index);
                mr.emissiveTexture          = texRef(gm.emissiveTexture.index);
                material.core               = mr;
            }

            // Open, lossless capture of every OTHER glTF extension (KHR clearcoat / transmission
            // / volume-SSS / sheen / specular / ior / iridescence / anisotropy, or anything the
            // fast-path core doesn't model). unlit + spec-gloss are already the core variant.
            for (const auto& [extName, extValue] : gm.extensions)
            {
                if (extName == "KHR_materials_unlit" || extName == "KHR_materials_pbrSpecularGlossiness")
                    continue;
                material.extensions.insert_or_assign(extName, ParseGltfExtension(extValue, texRef));
                const MaterialFeature feature = FeatureForExtension(extName);
                if (feature != MaterialFeature::None)
                    material.AddFeature(feature);
            }

            out.materials.push_back(std::move(material));
        }

        // ---- attribute union across all primitives (keeps SoA arrays aligned) ----
        bool useNormal = false, useTangent = false, useTexCoord0 = false, useColor = false;
        for (const tinygltf::Mesh& gmesh : model.meshes)
            for (const tinygltf::Primitive& prim : gmesh.primitives)
            {
                useNormal    = useNormal || prim.attributes.count("NORMAL") != 0;
                useTangent   = useTangent || prim.attributes.count("TANGENT") != 0;
                useTexCoord0 = useTexCoord0 || prim.attributes.count("TEXCOORD_0") != 0;
                useColor     = useColor || prim.attributes.count("COLOR_0") != 0;
            }
        out.attributes = VertexAttribute::Position;
        if (useNormal)
            out.attributes |= VertexAttribute::Normal;
        if (useTangent)
            out.attributes |= VertexAttribute::Tangent;
        if (useTexCoord0)
            out.attributes |= VertexAttribute::TexCoord0;
        if (useColor)
            out.attributes |= VertexAttribute::Color;

        // ---- primitives, baked by node world transform ----
        // glTF places meshes through the node graph (each node a TRS/matrix, hierarchical), so a
        // mesh's vertices are in LOCAL space. Bake the node's world transform into them, or every
        // submesh collapses to its own local origin - multi-node models come apart and a mesh placed
        // (or scaled) by a node transform, e.g. the Khronos Duck, ends up mislocated. Positions go
        // through the world matrix; normals/tangents through its normal (inverse-transpose) matrix.
        const auto processMesh = [&](int meshIndex, const glm::mat4& world) {
            if (meshIndex < 0 || meshIndex >= static_cast<int>(model.meshes.size()))
                return;
            const glm::mat3       normalMat = glm::mat3(glm::transpose(glm::inverse(world)));
            const glm::mat3       linear    = glm::mat3(world); // tangents transform by the plain 3x3
            const tinygltf::Mesh& gmesh     = model.meshes[meshIndex];
            for (const tinygltf::Primitive& prim : gmesh.primitives)
            {
                if (prim.mode != TINYGLTF_MODE_TRIANGLES)
                    continue;
                const auto posIt = prim.attributes.find("POSITION");
                if (posIt == prim.attributes.end())
                    continue;

                std::vector<float> positions;
                ReadAccessorFloats(model, posIt->second, positions);
                const size_t   vertexCount  = positions.size() / 3;
                const uint32_t vertexOffset = out.VertexCount();
                for (size_t i = 0; i < vertexCount; ++i)
                {
                    const glm::vec4 p =
                        world * glm::vec4(positions[i * 3 + 0], positions[i * 3 + 1], positions[i * 3 + 2], 1.0f);
                    out.positions.emplace_back(p.x, p.y, p.z);
                }

                if (useNormal)
                {
                    if (const auto it = prim.attributes.find("NORMAL"); it != prim.attributes.end())
                    {
                        std::vector<float> data;
                        ReadAccessorFloats(model, it->second, data);
                        for (size_t i = 0; i < vertexCount; ++i)
                        {
                            glm::vec3   n   = normalMat * glm::vec3(data[i * 3 + 0], data[i * 3 + 1], data[i * 3 + 2]);
                            const float len = glm::length(n);
                            n               = len > 0.0f ? n / len : glm::vec3(0.0f);
                            out.normals.emplace_back(n.x, n.y, n.z);
                        }
                    }
                    else
                        out.normals.resize(out.normals.size() + vertexCount, Normal(0.0f, 0.0f, 0.0f));
                }
                if (useTangent)
                {
                    if (const auto it = prim.attributes.find("TANGENT"); it != prim.attributes.end())
                    {
                        std::vector<float> data;
                        ReadAccessorFloats(model, it->second, data);
                        for (size_t i = 0; i < vertexCount; ++i)
                        {
                            glm::vec3   t   = linear * glm::vec3(data[i * 4 + 0], data[i * 4 + 1], data[i * 4 + 2]);
                            const float len = glm::length(t);
                            t               = len > 0.0f ? t / len : glm::vec3(0.0f);
                            out.tangents.emplace_back(t.x, t.y, t.z, data[i * 4 + 3]); // handedness (w) preserved
                        }
                    }
                    else
                        out.tangents.resize(out.tangents.size() + vertexCount, Tangent(0.0f, 0.0f, 0.0f, 1.0f));
                }
                if (useTexCoord0)
                {
                    if (const auto it = prim.attributes.find("TEXCOORD_0"); it != prim.attributes.end())
                    {
                        std::vector<float> data;
                        ReadAccessorFloats(model, it->second, data);
                        for (size_t i = 0; i < vertexCount; ++i)
                            out.texCoords0.emplace_back(data[i * 2 + 0], data[i * 2 + 1]);
                    }
                    else
                        out.texCoords0.resize(out.texCoords0.size() + vertexCount, TexCoord(0.0f, 0.0f));
                }
                if (useColor)
                {
                    if (const auto it = prim.attributes.find("COLOR_0"); it != prim.attributes.end())
                    {
                        std::vector<float> data;
                        const int          comps = ReadAccessorFloats(model, it->second, data);
                        for (size_t i = 0; i < vertexCount; ++i)
                        {
                            const float a = comps == 4 ? data[i * comps + 3] : 1.0f;
                            out.colors.emplace_back(data[i * comps + 0], data[i * comps + 1], data[i * comps + 2], a);
                        }
                    }
                    else
                        out.colors.resize(out.colors.size() + vertexCount, Color(1.0f, 1.0f, 1.0f, 1.0f));
                }

                const uint32_t indexOffset = out.IndexCount();
                if (prim.indices >= 0)
                {
                    std::vector<uint32_t> indices;
                    ReadIndices(model, prim.indices, indices);
                    for (uint32_t index : indices)
                        out.indices.push_back(vertexOffset + index);
                }
                else
                {
                    for (size_t i = 0; i < vertexCount; ++i)
                        out.indices.push_back(vertexOffset + static_cast<uint32_t>(i));
                }

                SubMesh sub;
                sub.name          = gmesh.name;
                sub.vertexOffset  = 0; // shared vertex array; indices are absolute
                sub.vertexCount   = out.VertexCount();
                sub.indexOffset   = indexOffset;
                sub.indexCount    = out.IndexCount() - indexOffset;
                sub.materialIndex = prim.material;
                out.subMeshes.push_back(std::move(sub));
            }
        };

        if (model.nodes.empty())
        {
            // No scene graph (rare): bake every mesh at identity.
            for (int m = 0; m < static_cast<int>(model.meshes.size()); ++m)
                processMesh(m, glm::mat4(1.0f));
        }
        else
        {
            std::vector<int> roots;
            if (!model.scenes.empty())
            {
                const int sceneIndex =
                    (model.defaultScene >= 0 && model.defaultScene < static_cast<int>(model.scenes.size())) ?
                        model.defaultScene :
                        0;
                roots = model.scenes[sceneIndex].nodes;
            }
            if (roots.empty()) // no scene declared: treat every node as a root
                for (int i = 0; i < static_cast<int>(model.nodes.size()); ++i)
                    roots.push_back(i);

            // Iterative DFS accumulating world = parent * local; guard against cycles.
            std::vector<std::pair<int, glm::mat4>> stack;
            for (int r : roots)
                stack.emplace_back(r, glm::mat4(1.0f));
            std::vector<bool> visited(model.nodes.size(), false);
            while (!stack.empty())
            {
                const int       nodeIndex   = stack.back().first;
                const glm::mat4 parentWorld = stack.back().second;
                stack.pop_back();
                if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size()) || visited[nodeIndex])
                    continue;
                visited[nodeIndex]          = true;
                const tinygltf::Node& node  = model.nodes[nodeIndex];
                const glm::mat4       world = parentWorld * NodeLocalMatrix(node);
                if (node.mesh >= 0)
                    // A skinned mesh ignores its node transform per spec (joints define it); we don't
                    // skin, so bake the bind pose at identity rather than double-applying the node.
                    processMesh(node.mesh, node.skin >= 0 ? glm::mat4(1.0f) : world);
                for (int child : node.children)
                    stack.emplace_back(child, world);
            }
        }

        out.ComputeBounds();
        return {};
    }

    Expected<Mesh> LoadGltf(std::string_view path, const GltfImportOptions& options)
    {
        Mesh mesh;
        auto result = LoadGltf(path, mesh, options);
        if (!result)
            return std::unexpected(result.error());
        return mesh;
    }
} // namespace vrf
