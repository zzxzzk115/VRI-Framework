target("vrf")
    set_kind("static")
    set_languages("cxx23")

    -- public headers (the parenthesized group preserves the vrf/... layout on install)
    add_headerfiles("include/(vrf/**.hpp)")
    add_includedirs("include", {public = true})

    -- private include dir for the internal backend hooks (src/platform/window_backends.hpp)
    add_includedirs("src")

    add_files("src/**.cpp")

    -- vri + glm appear in the public API, so re-export them to dependents. Marking the
    -- window packages public too ensures a consumer's final binary links them (the window
    -- backends are compiled into this static lib and reference SDL3 / GLFW symbols).
    add_packages("vri", "glm", {public = true})

    -- The framegraph module's graph core; public because pass code includes
    -- <fg/FrameGraph.hpp> / <fg/Blackboard.hpp> directly.
    add_packages("fg", {public = true})

    -- Loader implementations: single-header libs compiled into this static lib, so
    -- they are private (consumers don't need them). dds-ktx is vendored in src/.
    add_packages("stb", "tinyobjloader", "tinygltf")

    -- vshadersystem runtime loader: used only inside gpu/shader_library.cpp (PImpl), so its
    -- headers don't reach the public API - but it is a real static lib (unlike the header-only
    -- loaders above), so the package must be public for its symbols (+ spirv-cross/glslang/xxhash)
    -- to resolve when a consumer links this static lib.
    add_packages("vshadersystem", {public = true})

    -- KTX2 loader (opt-in) via libktx.
    if has_config("vrf_loader_ktx2") then
        add_defines("VRF_ENABLE_KTX2")
    end

    -- Asset-cache BC7 bake (opt-in): libktx's UASTC encoder + BC7 transcoder, run offline.
    if has_config("vrf_bake_bc7") then
        add_defines("VRF_ENABLE_BAKE_BC7")
    end

    -- Both of the above are libktx.
    if has_config("vrf_loader_ktx2") or has_config("vrf_bake_bc7") then
        add_packages("ktx")
    end

    -- Draco glTF decompression (opt-in) via tinygltf's built-in integration.
    if has_config("vrf_loader_draco") then
        add_defines("VRF_ENABLE_DRACO")
        add_packages("draco")
    end

    -- Gaussian Splatting: link the vendored GaussForge (+ spz + zlib) stack, the single
    -- gaussian backend. GaussForge headers are confined to gaussian_splat_loader.cpp.
    add_deps("GaussForge")

    -- Optional Dear ImGui. Public: dependents (examples) build their onGui with the same
    -- VRF_WITH_IMGUI view of the headers and get imgui + its platform backends to link.
    if has_config("vrf_with_imgui") then
        add_defines("VRF_WITH_IMGUI", {public = true})
        add_packages("imgui", {public = true})
    end

    -- Optional OpenXR (vrf::xr XrSystem/XrSession). The xr_system/xr_session TUs compile
    -- empty without VRF_WITH_OPENXR; SimStereoRig/stereo math are always in. Public so
    -- consumers see the same VRF_WITH_OPENXR headers and link the loader.
    if has_config("vrf_with_openxr") then
        add_defines("VRF_WITH_OPENXR", {public = true})
        add_packages("openxr", "vulkan-headers", {public = true})
    end

    -- Optional Tracy zones (vrf/core/profiling.hpp). Public so app code can use
    -- VRF_ZONE and connect with the same client.
    if has_config("vrf_with_tracy") then
        add_defines("VRF_WITH_TRACY", "TRACY_ENABLE", {public = true})
        add_packages("tracy", {public = true})
    end

    -- Link the Vulkan loader from the installed SDK (see the vulkansdk rule in the
    -- top-level xmake.lua). Public so the loader is linked into dependent binaries.
    if has_config("vrf_backend_vulkan") then
        add_rules("vulkansdk")
    end

    if has_config("vrf_window_sdl3") then
        add_defines("VRF_WINDOW_SDL3")
        add_packages("libsdl3", {public = true})
    end
    if has_config("vrf_window_glfw") then
        add_defines("VRF_WINDOW_GLFW")
        add_packages("glfw", {public = true})
    end

    set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/vrf")
