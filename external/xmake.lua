-- Dependency declarations. Packages resolve from the author's xmake-repo
-- (added in the top-level xmake.lua). Backend selection forwards the framework's
-- vrf_backend_* options to the vri package.

local vri_configs = {
    vulkan = has_config("vrf_backend_vulkan"),
    gl     = has_config("vrf_backend_gl"),
    wgpu   = has_config("vrf_backend_wgpu"),
    d3d12  = has_config("vrf_backend_d3d12"),
    metal  = has_config("vrf_backend_metal"),
}
add_requires("vri v0.1.5", {configs = vri_configs})

-- glm: math types used across the framework's public API.
add_requires("glm")

-- fg (skaarj1989/FrameGraph): the graph core under the vrf::fg framegraph module.
-- Passes declare create/read/write; fg culls, orders, and drives the transient
-- pool + barrier hooks (vrf/fg/*). Public - pass code includes <fg/FrameGraph.hpp>.
add_requires("fg")

-- vshadersystem: runtime shader-variant loader (a cooked .vshlib -> SPIR-V/WGSL per variant),
-- so shaders can be keyed by mesh vertex attributes (VTX_HAS_*). Prebuilt per platform; the
-- runtime loader links spirv-cross/glslang/xxhash and is confined to the framework's .cpp.
-- v1.1.0 adds DXBC/DXIL bytecode targets + `vshaderc strip`, so a cooked .vshlib can carry
-- Vulkan/OpenGL/Metal (SPIR-V), WebGPU (WGSL) and Direct3D 12 (DXBC/DXIL) and be stripped to the
-- shipping targets. (v1.0.1 already defaulted the matrix layout to column-major for glm.)
add_requires("vshadersystem v1.1.0", {configs = {debug = is_mode("debug")}})

-- Offline cook tool (the vshaderc CLI) - opt-in, to regenerate the committed .vshlib and to strip
-- it to the compiled-in backends at build time.
if has_config("vrf_cook_shaders") then
    add_requires("vshadersystem~host v1.1.0", {kind = "binary"})
end

-- Minimal single-header asset loaders (used only inside the loader .cpp files).
-- (dds-ktx is vendored as a single header under source/vrf/src/asset/loaders/.)
add_requires("stb")
add_requires("tinyobjloader")
add_requires("tinygltf")

-- KTX2 loader (opt-in): libktx pulls a heavier CMake/astc-encoder/zstd chain.
if has_config("vrf_loader_ktx2") then
    add_requires("ktx", {configs = {ktx1 = true, ktx2 = true, shared = false}})
end

-- Draco decompression (opt-in): tinygltf decodes KHR_draco_mesh_compression when linked with draco.
if has_config("vrf_loader_draco") then
    add_requires("draco")
end

-- OpenXR loader for vrf::xr (opt-in). vulkan-headers only for the Vulkan types in the
-- OpenXR<->Vulkan binding structs (no Vulkan functions are called from vrf).
if has_config("vrf_with_openxr") then
    add_requires("openxr")
    add_requires("vulkan-headers 1.4.335")
end

-- Tracy profiler (opt-in): CPU zones via vrf/core/profiling.hpp (VRF_ZONE).
if has_config("vrf_with_tracy") then
    add_requires("tracy")
end

-- Window backends (an abstract Window with SDL3 and/or GLFW implementations).
if has_config("vrf_window_sdl3") then
    add_requires("libsdl3") -- NOTE: the package is named "libsdl3", not "sdl3"
end
if has_config("vrf_window_glfw") then
    add_requires("glfw")
end

-- Optional Dear ImGui (VRI's imgui renderer + the SDL3/GLFW platform backends for input).
if has_config("vrf_with_imgui") then
    add_requires("imgui v1.92.5-docking", {configs = {
        sdl3 = has_config("vrf_window_sdl3"),
        glfw = has_config("vrf_window_glfw"),
    }})
end

-- Native file-open dialog for the examples' model picker.
if has_config("vrf_build_examples") and has_config("vrf_with_imgui") then
    add_requires("nativefiledialog-extended")
end

-- Tests use doctest.
if has_config("vrf_build_tests") then
    add_requires("doctest")
end

-- Gaussian Splatting stack (always built - small): GaussForge (a trimmed fork - no SOG/CLI/WASM)
-- plus Niantic's spz, both vendored as source under external/. Declared as their own static libs
-- so the third-party sources stay out of the vrf lib's own compilation. Mirrors vasset's wiring.
add_requires("zlib")

target("spz")
    set_kind("static")
    set_default(false)
    add_headerfiles("spz/**.h")
    add_includedirs("spz/src/cc", {public = true})
    add_files("spz/**.cc")
    add_packages("zlib", {public = true})
    set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/spz")

target("GaussForge")
    set_kind("static")
    set_default(false)
    add_headerfiles("GaussForge/include/(gf/**.h)")
    add_includedirs("GaussForge/include", {public = true})
    add_files("GaussForge/src/core/**.cpp", "GaussForge/src/io/**.cpp")
    remove_files("GaussForge/src/io/sog_*.cpp") -- SOG format is intentionally dropped (see vasset)
    add_deps("spz", {public = true})
    set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/GaussForge")
