// vrf model viewer - loads an OBJ / glTF / GLB (or a procedural cube), uploads it through the
// framework's CPU->GPU bridge, and renders it with textured PBR (metallic-roughness).
//
// The mesh is uploaded into a COMPACT vertex buffer carrying only the attributes it has
// (vrf::GpuVertexLayout). That layout drives both the pipeline's vertex inputs and the shader
// VARIANT: the shader is a cooked vshadersystem library (mesh.vshlib, all VTX_HAS_* permutations
// baked in), and vrf::ShaderLibrary resolves the variant matching the mesh's attributes. So a
// mesh with no normals/tangents/uvs selects a shader with no such inputs - the framework does
// NOT assume a fixed vertex. Each material gets a descriptor set (material UBO + baseColor /
// metallic-roughness / emissive / occlusion / normal textures + a shared sampler).
//
// NOTE ON SHADING: the framework is a toolset and does not dictate a shading model - this
// example brings its OWN PBR shader (mesh.slang) and reads the multi-model Material.
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include <vrf/vrf.hpp>

#if defined(VRF_WITH_IMGUI)
#include <imgui.h>
#include <nfd.hpp>
#endif

// The cooked shader variant library (shaders/mesh.vshlib) is embedded at build time by xmake's
// utils.bin2c rule (see xmake.lua) - no committed byte-array header.
static const unsigned char g_meshVshlib[] = {
#include "mesh.vshlib.h"
};

namespace
{
    struct PushConstants
    {
        glm::mat4 mvp;    // clip-space transform
        glm::mat4 model;  // world transform
        glm::vec4 camera; // camera position (xyz) + debug-view mode (w)
    };
    static_assert(sizeof(PushConstants) == 144, "push constant layout must match the shader");

    // Matches the shader's MaterialUBO (std140: three vec4s).
    struct MaterialUBO
    {
        glm::vec4 baseColorFactor;
        glm::vec4 factors;        // x=metallic, y=roughness, z=occlusionStrength, w=alphaCutoff
        glm::vec4 emissiveFactor; // xyz
    };

    // Texture slots, matching the shader's binding order (b1..b5).
    enum TextureSlot
    {
        kTexBaseColor = 0,
        kTexMetallicRoughness,
        kTexEmissive,
        kTexOcclusion,
        kTexNormal,
        kTextureSlotCount,
    };

    glm::vec4 BaseColorOf(const vrf::Material& material)
    {
        if (const auto* m = material.As<vrf::PbrMetallicRoughnessMaterial>())
            return m->baseColorFactor;
        if (const auto* m = material.As<vrf::PhongMaterial>())
            return m->diffuse;
        if (const auto* m = material.As<vrf::UnlitMaterial>())
            return m->color;
        if (const auto* m = material.As<vrf::PbrSpecularGlossinessMaterial>())
            return m->diffuseFactor;
        return glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
    }

    // Extract PBR-MR UBO data + texture-slot indices from a Material; falls back to a plain
    // factor material for the other shading models.
    void FillMaterial(const vrf::Material& material, MaterialUBO& ubo, int texture[kTextureSlotCount])
    {
        for (int i = 0; i < kTextureSlotCount; ++i)
            texture[i] = -1;
        // factors.w carries the alpha cutoff for MASK materials, or a negative sentinel otherwise -
        // the shader discards below the cutoff only when it is >= 0 (so OPAQUE/BLEND never mask).
        const float maskCutoff = material.alphaMode == vrf::AlphaMode::Mask ? material.alphaCutoff : -1.0f;
        if (const auto* m = material.As<vrf::PbrMetallicRoughnessMaterial>())
        {
            ubo.baseColorFactor            = m->baseColorFactor;
            ubo.factors                    = {m->metallicFactor, m->roughnessFactor, 1.0f, maskCutoff};
            ubo.emissiveFactor             = glm::vec4(m->emissiveFactor, 0.0f);
            texture[kTexBaseColor]         = m->baseColorTexture.index;
            texture[kTexMetallicRoughness] = m->metallicRoughnessTexture.index;
            texture[kTexEmissive]          = m->emissiveTexture.index;
            texture[kTexOcclusion]         = m->occlusionTexture.index;
            texture[kTexNormal]            = m->normalTexture.index;
        }
        else if (const auto* m = material.As<vrf::UnlitMaterial>())
        {
            // Unlit (e.g. KHR_materials_unlit, common in FBX->glTF exports): only a base color +
            // its texture. Route it through the base-color slot as a matte surface so it shows.
            ubo.baseColorFactor    = m->color;
            ubo.factors            = {0.0f, 1.0f, 1.0f, maskCutoff};
            ubo.emissiveFactor     = glm::vec4(0.0f);
            texture[kTexBaseColor] = m->colorTexture.index;
        }
        else if (const auto* m = material.As<vrf::PbrSpecularGlossinessMaterial>())
        {
            ubo.baseColorFactor    = m->diffuseFactor;
            ubo.factors            = {0.0f, 1.0f - m->glossinessFactor, 1.0f, maskCutoff};
            ubo.emissiveFactor     = glm::vec4(0.0f);
            texture[kTexBaseColor] = m->diffuseTexture.index;
            texture[kTexNormal]    = m->normalTexture.index;
        }
        else if (const auto* m = material.As<vrf::PhongMaterial>())
        {
            ubo.baseColorFactor    = m->diffuse;
            ubo.factors            = {0.0f, 0.8f, 1.0f, maskCutoff};
            ubo.emissiveFactor     = glm::vec4(m->emissive, 0.0f);
            texture[kTexBaseColor] = m->diffuseTexture.index;
            texture[kTexNormal]    = m->normalTexture.index;
            texture[kTexEmissive]  = m->emissiveTexture.index;
        }
        else
        {
            ubo.baseColorFactor = BaseColorOf(material);
            ubo.factors         = {0.0f, 0.8f, 1.0f, maskCutoff};
            ubo.emissiveFactor  = glm::vec4(0.0f);
        }
    }

    bool EndsWith(std::string_view s, std::string_view suffix)
    {
        return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    vrf::Mesh MakeCube()
    {
        vrf::Mesh mesh;
        mesh.name                         = "cube";
        mesh.attributes                   = vrf::VertexAttribute::Position | vrf::VertexAttribute::Normal;
        const glm::vec3 faceNormals[6]    = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
        const glm::vec3 faceCorners[6][4] = {
            {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},
            {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},
            {{1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}},
            {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}},
            {{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}},
            {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}},
        };
        for (int f = 0; f < 6; ++f)
        {
            const uint32_t base = mesh.VertexCount();
            for (int v = 0; v < 4; ++v)
            {
                mesh.positions.push_back(faceCorners[f][v] * 0.5f);
                mesh.normals.push_back(faceNormals[f]);
            }
            mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
        vrf::SubMesh sub;
        sub.indexCount  = mesh.IndexCount();
        sub.vertexCount = mesh.VertexCount();
        mesh.subMeshes.push_back(sub);
        mesh.ComputeBounds();
        return mesh;
    }

    vrf::Expected<vrf::Mesh> LoadModel(std::string_view path)
    {
        if (EndsWith(path, ".obj"))
            return vrf::LoadObj(path);
        if (EndsWith(path, ".gltf") || EndsWith(path, ".glb"))
            return vrf::LoadGltf(path);
        return vrf::MakeError(std::string("unsupported model extension: ") + std::string(path));
    }

    // GPU resources for one loaded model.
    struct GpuMaterial
    {
        VriBuffer*        ubo     = nullptr;
        VriDescriptor*    uboView = nullptr;
        VriDescriptorSet* set     = nullptr;
    };
    struct Scene
    {
        vrf::GpuMesh                 mesh;
        VriPipeline*                 pipeline = nullptr; // keyed by the mesh's vertex layout variant
        std::vector<vrf::GpuTexture> textures;           // uploaded mesh.textures (index-aligned)
        std::vector<GpuMaterial>     materials;          // one per material + a trailing default
        VriDescriptorPool*           pool = nullptr;
        glm::vec3                    center {0.0f};
        float                        radius   = 1.0f;
        float                        distance = 3.0f;
    };
} // namespace

int main(int argc, char** argv)
{
    vrf::ApplicationDesc desc;
    desc.title         = "vrf model viewer";
    desc.extent        = {1024, 768};
    desc.depthFormat   = VriFormat_D32_SFLOAT;
    desc.clearColor[0] = 0.05f;
    desc.clearColor[1] = 0.06f;
    desc.clearColor[2] = 0.08f;
#if defined(VRF_WITH_IMGUI)
    desc.imgui = true;
#endif
    // Request every optional feature best-effort so the startup capability dump
    // (vrf::DescribeDevice) reports what the GPU/backend actually supports.
    desc.enabledFeatures = VriFeature_RayTracing | VriFeature_RayQuery | VriFeature_MeshShader | VriFeature_Bindless |
                           VriFeature_VariableShadingRate | VriFeature_OpacityMicromap | VriFeature_ExternalMemory |
                           VriFeature_LowLatency;
    // Backend selection (VRI_API=vulkan|d3d12|opengl|metal|webgpu, else Auto) is handled by
    // vrf::Application, matching VRI's own convention - no per-example parsing needed.
    auto appResult = vrf::Application::Create(desc);
    if (!appResult)
    {
        std::fprintf(stderr, "[vrf-model-viewer] %s\n", appResult.error().message.c_str());
        return 1;
    }
    vrf::Application&       app    = *appResult.value();
    vrf::RenderDevice&      device = app.Device();
    const VriCoreInterface& c      = device.Core();

    // Shader variant library (cooked offline; all VTX_HAS_* permutations baked in).
    auto libResult = vrf::ShaderLibrary::LoadFromMemory(g_meshVshlib, sizeof(g_meshVshlib));
    if (!libResult)
    {
        std::fprintf(stderr, "[vrf-model-viewer] shader library: %s\n", libResult.error().message.c_str());
        return 1;
    }
    vrf::ShaderLibrary shaderLib = std::move(*libResult);

#if defined(VRF_WITH_IMGUI)
    NFD::Guard nfdGuard;
#endif

    // ---- shared resources: sampler + default 1x1 textures for missing material slots ----
    VriSamplerDesc samplerDesc {};
    samplerDesc.magFilter     = VriFilter_Linear;
    samplerDesc.minFilter     = VriFilter_Linear;
    samplerDesc.mipmapMode    = VriMipmapMode_Linear;
    samplerDesc.addressModeU  = VriAddressMode_Repeat;
    samplerDesc.addressModeV  = VriAddressMode_Repeat;
    samplerDesc.addressModeW  = VriAddressMode_Repeat;
    samplerDesc.maxLod        = 16.0f;
    samplerDesc.maxAnisotropy = 1.0f;
    VriDescriptor* sampler    = nullptr;
    c.CreateSampler(device.Handle(), &samplerDesc, &sampler);

    const auto upload1x1 = [&](uint8_t r, uint8_t g, uint8_t b, uint8_t a) -> vrf::Expected<vrf::GpuTexture> {
        vrf::Texture cpu;
        cpu.width  = 1;
        cpu.height = 1;
        cpu.format = VriFormat_RGBA8_UNORM;
        cpu.data   = {r, g, b, a};
        return vrf::UploadTexture(device, cpu);
    };
    auto whiteResult = upload1x1(255, 255, 255, 255); // baseColor / MR / emissive / occlusion default
    auto flatResult  = upload1x1(128, 128, 255, 255); // flat tangent-space normal (0,0,1) default
    if (!whiteResult || !flatResult)
    {
        std::fprintf(stderr, "[vrf-model-viewer] default textures failed\n");
        return 1;
    }
    vrf::GpuTexture white      = *whiteResult;
    vrf::GpuTexture flatNormal = *flatResult;

    // ---- pipeline layout: push constants + descriptor set 0 (UBO + 5 textures + sampler) ----
    std::vector<VriDescriptorRangeDesc> ranges;
    const auto                          addRange = [&](uint32_t reg, VriDescriptorType type) {
        VriDescriptorRangeDesc r {};
        r.baseRegister   = reg;
        r.descriptorNum  = 1;
        r.descriptorType = type;
        r.shaderStages   = VriShaderStage_Fragment;
        r.viewType       = VriTextureViewType_2D;
        ranges.push_back(r);
    };
    addRange(0, VriDescriptorType_ConstantBuffer);
    addRange(1, VriDescriptorType_Texture); // baseColor
    addRange(2, VriDescriptorType_Texture); // metallic-roughness
    addRange(3, VriDescriptorType_Texture); // emissive
    addRange(4, VriDescriptorType_Texture); // occlusion
    addRange(5, VriDescriptorType_Texture); // normal
    addRange(6, VriDescriptorType_Sampler);

    auto layoutResult = vrf::PipelineLayoutBuilder {}
                            // register b1: the material CBV pins b0, so Slang puts the push
                            // constant at b1 on D3D12 (ignored for Vulkan, which has no push register).
                            .AddPushConstant(1, sizeof(PushConstants), VriShaderStage_Vertex | VriShaderStage_Fragment)
                            .AddDescriptorSet(0, ranges)
                            .Build(device);
    if (!layoutResult)
    {
        std::fprintf(stderr, "[vrf-model-viewer] %s\n", layoutResult.error().message.c_str());
        return 1;
    }
    VriPipelineLayout* layout = *layoutResult;

    // ---- reloadable scene ----
    vrf::Mesh   cpuMesh;
    Scene       scene;
    std::string status;

    // ---- orbit camera (yaw/pitch around a target + zoom distance) + debug view ----
    float     camYaw      = 0.0f;
    float     camPitch    = 0.3f;
    float     camDistance = 3.0f;
    glm::vec3 camTarget {0.0f};
    int       debugMode = 0; // 0=PBR, 1=BaseColor, 2=Metallic, 3=Roughness, 4=Normal, 5=AO, 6=Emissive, 7=UV

    const auto computeEye = [&]() {
        return camTarget + camDistance * glm::vec3(std::cos(camPitch) * std::sin(camYaw),
                                                   std::sin(camPitch),
                                                   std::cos(camPitch) * std::cos(camYaw));
    };
    const auto resetCamera = [&]() {
        camTarget   = scene.center;
        camDistance = scene.distance;
        camYaw      = 0.0f;
        camPitch    = 0.3f;
    };

    const auto destroyScene = [&]() {
        device.WaitIdle();
        if (scene.pipeline)
            c.DestroyPipeline(scene.pipeline);
        for (GpuMaterial& m : scene.materials)
        {
            if (m.uboView)
                c.DestroyDescriptor(m.uboView);
            if (m.ubo)
                c.DestroyBuffer(m.ubo);
        }
        if (scene.pool)
            c.DestroyDescriptorPool(scene.pool);
        for (vrf::GpuTexture& t : scene.textures)
            t.Destroy(device);
        scene.mesh.Destroy(device);
        scene = Scene {};
    };

    const auto buildScene = [&]() {
        auto uploaded = vrf::UploadMesh(device, cpuMesh);
        if (!uploaded)
        {
            status = "upload failed: " + uploaded.error().message;
            return;
        }
        scene.mesh = *uploaded;

        // Resolve the shader variant matching this mesh's vertex attributes and build a pipeline.
        const std::vector<vrf::ShaderKeyword> keywords = vrf::ShaderKeywordsForVertexLayout(scene.mesh.layout);
        auto                                  vs       = shaderLib.Resolve("mesh", vrf::ShaderStage::Vertex, keywords);
        auto                                  fs = shaderLib.Resolve("mesh", vrf::ShaderStage::Fragment, keywords);
        if (!vs || !fs)
        {
            status = "shader variant failed: " + (vs ? fs.error().message : vs.error().message);
            return;
        }
        // Feed every backend blob the variant carries; the builder picks per active API. D3D12
        // uses DXBC (SM5.1) - VRI reflects vertex input via D3DReflect, which reads DXBC.
        const auto toVariants = [](const vrf::ResolvedShader& r) {
            vrf::ShaderVariants v {};
            v.spirv     = r.spirv;
            v.spirvSize = r.spirvSize;
            v.wgsl      = r.wgsl;
            v.wgslSize  = r.wgslSize;
            v.d3d12     = r.dxbc;
            v.d3d12Size = r.dxbcSize;
            return v;
        };
        const vrf::ShaderVariants vsv = toVariants(*vs);
        const vrf::ShaderVariants fsv = toVariants(*fs);

        auto pipelineResult = vrf::GraphicsPipelineBuilder {}
                                  .SetPipelineLayout(layout)
                                  .AddShaderVariants(VriShaderStage_Vertex, vsv, vs->entryPoint)
                                  .AddShaderVariants(VriShaderStage_Fragment, fsv, fs->entryPoint)
                                  .AddVertexLayout(scene.mesh.layout)
                                  .SetCullMode(VriCullMode_None)
                                  .SetDepthTest(true, true, VriCompareOp_LessOrEqual)
                                  .SetDepthStencilFormat(app.DepthFormat())
                                  .AddColorAttachment(app.ColorFormat())
                                  .Build(device);
        if (!pipelineResult)
        {
            status = "pipeline failed: " + pipelineResult.error().message;
            return;
        }
        scene.pipeline = *pipelineResult;

        scene.textures.reserve(cpuMesh.textures.size());
        for (const vrf::Texture& tex : cpuMesh.textures)
        {
            auto gpu = vrf::UploadTexture(device, tex);
            scene.textures.push_back(gpu ? *gpu : vrf::GpuTexture {});
        }

        const uint32_t        materialCount = static_cast<uint32_t>(cpuMesh.materials.size()) + 1; // +1 default
        VriDescriptorPoolDesc poolDesc {};
        poolDesc.descriptorSetMaxNum  = materialCount;
        poolDesc.constantBufferMaxNum = materialCount;
        poolDesc.textureMaxNum        = materialCount * kTextureSlotCount;
        poolDesc.samplerMaxNum        = materialCount;
        c.CreateDescriptorPool(device.Handle(), &poolDesc, &scene.pool);

        std::vector<VriDescriptorSet*> sets(materialCount);
        c.AllocateDescriptorSets(scene.pool, layout, 0, sets.data(), materialCount);

        const auto viewFor = [&](int index, const vrf::GpuTexture& fallback) -> VriDescriptor* {
            if (index >= 0 && index < static_cast<int>(scene.textures.size()) && scene.textures[index].view)
                return scene.textures[index].view;
            return fallback.view;
        };

        scene.materials.resize(materialCount);
        for (uint32_t i = 0; i < materialCount; ++i)
        {
            MaterialUBO ubo {};
            int         texture[kTextureSlotCount];
            if (i < cpuMesh.materials.size())
                FillMaterial(cpuMesh.materials[i], ubo, texture);
            else
            {
                ubo.baseColorFactor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
                ubo.factors         = {0.0f, 0.7f, 1.0f, -1.0f}; // w<0: no alpha masking on the default material
                ubo.emissiveFactor  = glm::vec4(0.0f);
                for (int t = 0; t < kTextureSlotCount; ++t)
                    texture[t] = -1;
            }

            auto uboBuffer = vrf::BufferBuilder {}
                                 .SetSize(sizeof(MaterialUBO))
                                 .SetUsage(VriBufferUsage_ConstantBuffer)
                                 .SetMemoryLocation(VriMemoryLocation_HostUpload)
                                 .Build(device);
            VriBuffer* ub = *uboBuffer;
            std::memcpy(c.MapBuffer(ub, 0, sizeof(MaterialUBO)), &ubo, sizeof(MaterialUBO));
            c.UnmapBuffer(ub);

            VriBufferViewDesc bvd {};
            bvd.buffer             = ub;
            bvd.viewType           = VriDescriptorType_ConstantBuffer;
            bvd.size               = sizeof(MaterialUBO);
            VriDescriptor* uboView = nullptr;
            c.CreateBufferView(device.Handle(), &bvd, &uboView);

            VriDescriptor* descriptors[kTextureSlotCount + 2] = {
                uboView,
                viewFor(texture[kTexBaseColor], white),
                viewFor(texture[kTexMetallicRoughness], white),
                viewFor(texture[kTexEmissive], white),
                viewFor(texture[kTexOcclusion], white),
                viewFor(texture[kTexNormal], flatNormal),
                sampler,
            };
            constexpr int                kDescriptorNum = kTextureSlotCount + 2; // UBO + textures + sampler
            VriDescriptorRangeUpdateDesc updates[kDescriptorNum] {};
            for (int r = 0; r < kDescriptorNum; ++r)
            {
                updates[r].descriptors    = &descriptors[r];
                updates[r].descriptorNum  = 1;
                updates[r].baseDescriptor = 0;
            }
            c.UpdateDescriptorRanges(sets[i], 0, kDescriptorNum, updates);
            scene.materials[i] = {ub, uboView, sets[i]};
        }

        scene.center = (cpuMesh.boundsMin + cpuMesh.boundsMax) * 0.5f;
        scene.radius = glm::length(cpuMesh.boundsMax - cpuMesh.boundsMin) * 0.5f;
        if (scene.radius < 1e-4f)
            scene.radius = 1.0f;
        scene.distance = scene.radius * 3.0f;
        status         = std::to_string(cpuMesh.VertexCount()) + " verts, " + std::to_string(cpuMesh.subMeshes.size()) +
                 " submeshes, " + std::to_string(cpuMesh.materials.size()) + " materials, " +
                 std::to_string(cpuMesh.textures.size()) + " textures";
    };

    const auto setMesh = [&](vrf::Mesh newMesh) {
        destroyScene();
        cpuMesh = std::move(newMesh);
        buildScene();
        resetCamera();
    };
    const auto loadPath = [&](const std::string& path) {
        auto loaded = LoadModel(path);
        if (!loaded)
        {
            status = loaded.error().message;
            return;
        }
        setMesh(std::move(*loaded));
    };

    if (argc > 1)
        loadPath(argv[1]);
    else
        setMesh(MakeCube());
    std::printf("[vrf-model-viewer] '%s': %s\n", cpuMesh.name.c_str(), status.c_str());

    app.onRecord = [&](VriCommandBuffer* cmd) {
        if (!scene.mesh.vertexBuffer || !scene.pipeline)
            return;
        const vrf::Extent2D extent = app.GetSwapchain().Extent();
        const float         aspect =
            extent.height > 0 ? static_cast<float>(extent.width) / static_cast<float>(extent.height) : 1.0f;

        // Orbit camera: the model stays put (identity model matrix), the camera moves.
        const glm::vec3 eye   = computeEye();
        const glm::mat4 model = glm::mat4(1.0f);
        const glm::mat4 view  = glm::lookAt(eye, camTarget, glm::vec3(0, 1, 0));
        // VRI is Y-up (negative-height viewport in the backend), so NO proj[1][1] flip.
        const float     nearZ = glm::max(camDistance * 0.01f, scene.radius * 0.01f);
        const float     farZ  = camDistance + scene.radius * 4.0f;
        const glm::mat4 proj  = glm::perspective(glm::radians(45.0f), aspect, nearZ, farZ);

        PushConstants push {};
        push.mvp    = proj * view * model;
        push.model  = model;
        push.camera = glm::vec4(eye, static_cast<float>(debugMode)); // debug view mode in .w

        c.CmdSetPipelineLayout(cmd, layout);
        c.CmdSetPipeline(cmd, scene.pipeline);
        VriVertexBufferBinding binding {};
        binding.buffer = scene.mesh.vertexBuffer;
        c.CmdSetVertexBuffers(cmd, 0, &binding, 1);
        c.CmdSetIndexBuffer(cmd, scene.mesh.indexBuffer, 0, scene.mesh.indexType);
        c.CmdSetConstants(cmd, 0, &push, sizeof(push));

        const int defaultMaterial = static_cast<int>(cpuMesh.materials.size());
        for (const vrf::SubMesh& sub : cpuMesh.subMeshes)
        {
            int mat = sub.materialIndex;
            if (mat < 0 || mat >= static_cast<int>(cpuMesh.materials.size()))
                mat = defaultMaterial;
            c.CmdSetDescriptorSet(cmd, 0, scene.materials[static_cast<size_t>(mat)].set);

            VriDrawIndexedDesc draw {};
            draw.indexNum    = sub.indexCount;
            draw.instanceNum = 1;
            draw.baseIndex   = sub.indexOffset;
            c.CmdDrawIndexed(cmd, &draw);
        }
    };

#if defined(VRF_WITH_IMGUI)
    app.onGui = [&]() {
        // Orbit-camera input from the mouse (skipped while the cursor is over the UI).
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantCaptureMouse)
        {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
            {
                camYaw -= io.MouseDelta.x * 0.01f;
                camPitch = glm::clamp(camPitch + io.MouseDelta.y * 0.01f, -1.5f, 1.5f);
            }
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f))
            {
                const glm::vec3 eye   = computeEye();
                const glm::vec3 fwd   = glm::normalize(camTarget - eye);
                const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0, 1, 0)));
                const glm::vec3 up    = glm::cross(right, fwd);
                const float     pan   = camDistance * 0.0015f;
                camTarget += right * (-io.MouseDelta.x * pan) + up * (io.MouseDelta.y * pan);
            }
            if (io.MouseWheel != 0.0f)
                camDistance = glm::max(camDistance * std::pow(0.9f, io.MouseWheel), 1e-3f);
        }

        ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
        ImGui::Begin("Model Viewer (PBR)");
        ImGui::TextWrapped("%s", cpuMesh.name.empty() ? "(none)" : cpuMesh.name.c_str());
        if (!status.empty())
            ImGui::TextWrapped("%s", status.c_str());
        ImGui::Separator();

        if (ImGui::Button("Load model..."))
        {
            NFD::UniquePathU8       outPath;
            const nfdu8filteritem_t filters[1] = {{"3D models", "gltf,glb,obj"}};
            if (NFD::OpenDialog(outPath, filters, 1) == NFD_OKAY)
                loadPath(outPath.get());
        }
        ImGui::SameLine();
        if (ImGui::Button("Cube"))
            setMesh(MakeCube());

        ImGui::Separator();
        const char* views[] = {
            "PBR", "Base color", "Metallic", "Roughness", "Normal", "Occlusion", "Emissive", "TexCoord"};
        ImGui::Combo("Debug view", &debugMode, views, IM_ARRAYSIZE(views));
        if (ImGui::Button("Reset camera"))
            resetCamera();
        ImGui::TextDisabled("drag: rotate | right-drag: pan | wheel: zoom");
        ImGui::End();
    };
#endif

    std::printf("[vrf-model-viewer] %s\n", vrf::DescribeDevice(device).c_str());
    app.Run();

    device.WaitIdle();
    destroyScene();
    white.Destroy(device);
    flatNormal.Destroy(device);
    c.DestroyDescriptor(sampler);
    c.DestroyPipelineLayout(layout);
    return 0;
}
