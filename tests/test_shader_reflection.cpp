#include <doctest/doctest.h>

#include <string>

#include <vrf/gpu/shader_library.hpp>

// A .vshlib has always carried each variant's descriptor table; ShaderLibrary deserialized it and
// Resolve() dropped it, so consumers re-declared every binding by hand next to the shader and a
// disagreement between the two spellings surfaced as a first-frame device hang with no validation
// message. These tests pin the round trip that makes the hand-written copy checkable.
//
// The fixture is tests/assets/reflection_fixture.{slang,vshlib} - one compute shader with one
// descriptor of each kind, a [numthreads(8,4,1)], and a keyword that REMOVES the sixth binding, so
// both the kind mapping and the cooker's base-variant-only reflection are observable.
namespace
{
    const std::string kFixture = std::string(VRF_TEST_ASSET_DIR) + "/reflection_fixture.vshlib";

    vrf::ResolvedShader ResolveFixture(const vrf::ShaderLibrary& lib, uint32_t extraTexture)
    {
        auto shader = lib.Resolve("reflection_fixture", vrf::ShaderStage::Compute,
                                  {{"EXTRA_TEXTURE", extraTexture}});
        REQUIRE_MESSAGE(shader.has_value(), (shader ? "" : shader.error().message));
        return *shader;
    }
} // namespace

TEST_CASE("shader reflection: the cooked descriptor table round-trips")
{
    auto lib = vrf::ShaderLibrary::LoadFromFile(kFixture);
    REQUIRE_MESSAGE(lib.has_value(), (lib ? "" : lib.error().message));

    const vrf::ResolvedShader shader = ResolveFixture(*lib, 0);
    REQUIRE(shader.reflection != nullptr);
    const vrf::ShaderReflection& r = *shader.reflection;

    SUBCASE("every descriptor kind maps")
    {
        const vrf::ReflectedDescriptor* params = r.Find("u_Params");
        REQUIRE(params != nullptr);
        CHECK(params->set == 0);
        CHECK(params->binding == 0);
        CHECK(params->count == 1);
        CHECK(params->kind == vrf::DescriptorKind::UniformBuffer);

        const vrf::ReflectedDescriptor* source = r.Find("t_Source");
        REQUIRE(source != nullptr);
        CHECK(source->binding == 1);
        CHECK(source->kind == vrf::DescriptorKind::SampledImage);
        CHECK(source->textureType == vrf::ReflectedTextureType::Tex2D);

        const vrf::ReflectedDescriptor* sampler = r.Find("s_Linear");
        REQUIRE(sampler != nullptr);
        CHECK(sampler->binding == 2);
        CHECK(sampler->kind == vrf::DescriptorKind::Sampler);

        const vrf::ReflectedDescriptor* indices = r.Find("b_Indices");
        REQUIRE(indices != nullptr);
        CHECK(indices->binding == 3);
        CHECK(indices->kind == vrf::DescriptorKind::StorageBuffer);

        const vrf::ReflectedDescriptor* dst = r.Find("u_Dst");
        REQUIRE(dst != nullptr);
        CHECK(dst->binding == 4);
        CHECK(dst->kind == vrf::DescriptorKind::StorageImage);
    }

    SUBCASE("bindings are addressable by slot as well as by name")
    {
        const vrf::ReflectedDescriptor* bySlot = r.FindAt(0, 1);
        REQUIRE(bySlot != nullptr);
        CHECK(bySlot->name == "t_Source");
        CHECK(r.FindAt(0, 99) == nullptr);
        CHECK(r.Find("t_NoSuchThing") == nullptr);
    }

    SUBCASE("every binding is visible to the compute stage")
    {
        REQUIRE(!r.descriptors.empty());
        for (const vrf::ReflectedDescriptor& d : r.descriptors)
            CHECK(vrf::HasStage(d.stageFlags, vrf::ShaderStageMask::Compute));
    }

    SUBCASE("[numthreads] comes through as the local size")
    {
        CHECK(r.hasLocalSize);
        CHECK(r.localSize[0] == 8);
        CHECK(r.localSize[1] == 4);
        CHECK(r.localSize[2] == 1);
    }
}

TEST_CASE("shader reflection: the table is the BASE variant's, shared by every variant")
{
    // The limitation every consumer has to design around, pinned here because it is invisible
    // otherwise and fatal to build on. vshadersystem v1.2.0 specializes the BYTECODE per variant
    // but reflects only the base variant (all permute keywords 0) and copies that one table onto
    // every variant. The fixture gates t_Extra on !EXTRA_TEXTURE, so the EXTRA_TEXTURE=1 variant's
    // SPIR-V provably lacks the binding (it is the smaller blob) while its reflection still lists
    // it. The reverse case - a keyword that ADDS a binding, which is how pvw's shaders are written
    // - is the dangerous direction: the reflection then UNDER-reports, and a layout derived from it
    // is short, which is a first-frame device hang with no validation message.
    //
    // When the cooker starts emitting per-variant reflection this test fails, which is exactly
    // when every consumer wants to hear about it.
    auto lib = vrf::ShaderLibrary::LoadFromFile(kFixture);
    REQUIRE(lib.has_value());

    const vrf::ResolvedShader base = ResolveFixture(*lib, 0); // declares t_Extra
    const vrf::ResolvedShader gated = ResolveFixture(*lib, 1); // does not
    REQUIRE(base.reflection != nullptr);
    REQUIRE(gated.reflection != nullptr);

    // The bytecode really is specialized: the variant without the extra sampler is smaller.
    CHECK(gated.spirvSize < base.spirvSize);

    // ... and yet both report the same table, including the binding one of them cannot use.
    REQUIRE(base.reflection->descriptors.size() == gated.reflection->descriptors.size());
    for (size_t i = 0; i < base.reflection->descriptors.size(); ++i)
    {
        CHECK(base.reflection->descriptors[i].name == gated.reflection->descriptors[i].name);
        CHECK(base.reflection->descriptors[i].binding == gated.reflection->descriptors[i].binding);
    }

    const vrf::ReflectedDescriptor* extra = gated.reflection->Find("t_Extra");
    REQUIRE(extra != nullptr);
    CHECK(extra->binding == 5);
    CHECK(extra->kind == vrf::DescriptorKind::SampledImage);
}
