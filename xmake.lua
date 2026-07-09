-- set project name
set_project("VRI-Framework")

-- set project version
set_version("0.1.0")

-- set language version: C++ 23
set_languages("cxx23")

-- root ?
local is_root = (os.projectdir() == os.scriptdir())
set_config("root", is_root)
set_config("project_dir", os.scriptdir())

-- global options
option("vrf_build_examples") -- build examples?
    set_default(true)
    set_showmenu(true)
    set_description("Enable VRI-Framework examples")
option_end()

option("vrf_build_tests") -- build tests?
    set_default(true)
    set_showmenu(true)
    set_description("Enable VRI-Framework tests")
option_end()

-- window backends (an abstract Window with SDL3 and/or GLFW implementations)
option("vrf_window_sdl3")
    set_default(true)
    set_showmenu(true)
    set_description("Build the SDL3 window backend")
option_end()

option("vrf_window_glfw")
    set_default(true)
    set_showmenu(true)
    set_description("Build the GLFW window backend")
option_end()

-- KTX2 texture loading via libktx (heavy: CMake + astc-encoder + zstd). Opt-in so the
-- default build stays minimal; DDS/KTX(v1) always work via the vendored dds-ktx header.
option("vrf_loader_ktx2")
    set_default(false)
    set_showmenu(true)
    set_description("Enable the KTX2 texture loader (libktx)")
option_end()

-- Optional Dear ImGui integration (Application onGui + RenderModule draw, via VRI's imgui
-- extension and the SDL3/GLFW platform backends for input).
option("vrf_with_imgui")
    set_default(true)
    set_showmenu(true)
    set_description("Enable the Dear ImGui integration")
option_end()

-- Optional Draco mesh decompression for the glTF loader (KHR_draco_mesh_compression),
-- via tinygltf's built-in draco integration. Opt-in so the default build stays minimal.
option("vrf_loader_draco")
    set_default(false)
    set_showmenu(true)
    set_description("Enable Draco-compressed glTF (KHR_draco_mesh_compression)")
option_end()


-- Re-cook the examples' shader variant libraries (.vshlib) from their .slang sources using
-- the vshadersystem `vshaderc` host tool, regenerating the committed byte headers. Opt-in so
-- the default build needs no Slang toolchain (the cooked .vshlib headers are committed).
option("vrf_cook_shaders")
    set_default(false)
    set_showmenu(true)
    set_description("Re-cook shader variant libraries (.vshlib) via vshaderc (needs vshadersystem~host)")
option_end()

-- VRI backend selection (forwarded to the vri package). Vulkan is the reference
-- backend (default on); the others are opt-in so only the used deps are pulled.
option("vrf_backend_vulkan")
    set_default(true)
    set_showmenu(true)
    set_description("Enable the Vulkan backend (vri package)")
option_end()

option("vrf_backend_gl")
    set_default(false)
    set_showmenu(true)
    set_description("Enable the OpenGL / OpenGL ES backend (vri package)")
option_end()

option("vrf_backend_wgpu")
    set_default(false)
    set_showmenu(true)
    set_description("Enable the WebGPU backend (vri package)")
option_end()

option("vrf_backend_d3d12")
    set_default(false)
    set_showmenu(true)
    set_description("Enable the Direct3D 12 backend (vri package, Windows only)")
option_end()

option("vrf_backend_metal")
    set_default(false)
    set_showmenu(true)
    set_description("Enable the native Metal backend (vri package, macOS only)")
option_end()

-- align package runtimes with the consuming targets (matches the VRI / libvultra
-- ecosystem: static runtime on Windows). Without this the prebuilt vri package
-- (MT/MTd) mismatches an MD consumer -> LNK2038.
if is_plat("windows") then
    add_requireconfs("**", {configs = {runtimes = is_mode("debug") and "MTd" or "MT"}})
end

-- if build on windows
if is_plat("windows") then
    add_cxxflags("/Zc:__cplusplus", {tools = {"msvc", "cl"}}) -- fix __cplusplus == 199711L error
    add_cxxflags("/bigobj") -- avoid big obj
    add_cxxflags("-D_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING")
    add_cxxflags("/EHsc")
    if is_mode("debug") then
        set_runtimes("MTd")
    else
        set_runtimes("MT")
    end
else
    add_cxxflags("-fexceptions")
end

-- add rules
rule("clangd.config")
    on_config(function (target)
        if is_host("windows") then
            os.cp(".clangd.win", ".clangd")
        else
            os.cp(".clangd.nowin", ".clangd")
        end
    end)
rule_end()

-- Link only the Vulkan loader (vulkan-1 / libvulkan). Ported verbatim from VRI:
-- when a downstream target consumes the `vri` *package*, VRI's own loader link
-- does not cross the package boundary, so the consumer must link the loader. The
-- vri headers come from the vulkan-headers package; this rule locates + links the
-- loader from the installed Vulkan SDK. https://github.com/xmake-io/xmake-repo/issues/3962
rule("vulkansdk")
    on_config(function (target)
        if target:is_plat("android") then
            target:add("syslinks", "vulkan", { public = true })
            return
        end

        import("lib.detect.find_library")
        import("detect.sdks.find_vulkansdk")

        local vulkansdk = find_vulkansdk()
        if vulkansdk then
            target:add("runenvs", "PATH", vulkansdk.bindir)

            local suffix
            if target:is_plat("windows") then
                suffix = ".lib"
            elseif target:is_plat("macosx") then
                suffix = ".dylib"
            else
                suffix = ".so"
            end

            local util = target:is_plat("windows") and "vulkan-1" or "vulkan"

            if target:is_plat("macosx") then
                target:add("rpathdirs", vulkansdk.linkdirs[1], { public = true })
                target:add("ldflags", "-Wl,-rpath," .. vulkansdk.linkdirs[1], { force = true, public = true })
            end

            if not find_library(util, vulkansdk.linkdirs) then
                wprint(format("The Vulkan loader %s for %s is not found!", util, target:arch()))
                return
            end

            -- Linux: the loader must resolve vk* symbols from the vri PACKAGE (libvri.a), which
            -- appear LATE in the link line. A full-path link placed before those package libs is
            -- dropped by ld's default --as-needed -> undefined vk* refs. Add it as a trailing
            -- syslink (+ linkdir) instead, which xmake emits after the package libs.
            if target:is_plat("linux") then
                target:add("linkdirs", vulkansdk.linkdirs[1], { public = true })
                target:add("syslinks", util, { public = true })
                return
            end

            local lib_name = target:is_plat("windows") and util or "lib" .. util
            local lib_path = path.join(vulkansdk.linkdirs[1], lib_name .. suffix)
            target:add("links", lib_path, { public = true })
        end
    end)
rule_end()

add_rules("mode.debug", "mode.release")
add_rules("plugin.vsxmake.autoupdate")
add_rules("plugin.compile_commands.autoupdate", {outputdir = ".vscode", lsp = "clangd"})
add_rules("clangd.config")

-- add repositories
add_repositories("my-xmake-repo https://github.com/zzxzzk115/xmake-repo.git backup")

-- include external libraries
includes("external")

-- include source
includes("source")

-- include tests
if has_config("vrf_build_tests") then
    includes("tests")
end

-- if build examples, then include examples
if has_config("vrf_build_examples") then
    includes("examples")
end
