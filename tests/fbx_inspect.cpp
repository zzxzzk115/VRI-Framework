#include <iostream>
#include <vrf/asset/loaders/fbx_loader.hpp>

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    vrf::Mesh  mesh;
    const auto loaded = vrf::LoadFbx(
        argv[1],
        mesh,
        {.directXNormalMaps = true, .materialConvention = vrf::FbxMaterialConvention::OrcaMetallicRoughness});
    if (!loaded)
    {
        std::cerr << loaded.error().message << '\n';
        return 1;
    }
    uint64_t bytes = 0, compressed = 0, bc5 = 0, normalRefs = 0, mrRefs = 0;
    for (const auto& texture : mesh.textures)
    {
        bytes += texture.SizeBytes();
        compressed += texture.compressed;
        bc5 += texture.format == VriFormat_BC5_UNORM;
    }
    for (const auto& material : mesh.materials)
        if (const auto* pbr = std::get_if<vrf::PbrMetallicRoughnessMaterial>(&material.core))
        {
            normalRefs += pbr->normalTexture.Valid();
            mrRefs += pbr->metallicRoughnessTexture.Valid();
        }
    std::cout << "{\n\"vertices\":" << mesh.VertexCount() << ",\n\"triangles\":" << mesh.IndexCount() / 3
              << ",\n\"materials\":" << mesh.materials.size() << ",\n\"submeshes\":" << mesh.subMeshes.size()
              << ",\n\"textures\":" << mesh.textures.size() << ",\n\"compressed_textures\":" << compressed
              << ",\n\"bc5_textures\":" << bc5 << ",\n\"normal_references\":" << normalRefs
              << ",\n\"mr_references\":" << mrRefs << ",\n\"texture_bytes\":" << bytes << ",\n\"bounds_min\":["
              << mesh.boundsMin.x << ',' << mesh.boundsMin.y << ',' << mesh.boundsMin.z << "],\n\"bounds_max\":["
              << mesh.boundsMax.x << ',' << mesh.boundsMax.y << ',' << mesh.boundsMax.z << "]\n}\n";
}
