-- Opt-in (vrf_cook_shaders): re-cook mesh.slang -> shaders/mesh.vshlib with the vshadersystem
-- `vshaderc` host tool, emitting ALL backends (SPIR-V + WGSL + DXBC + DXIL). Off by default: the
-- binary .vshlib is committed, so ordinary builds need no Slang toolchain.
if has_config("vrf_cook_shaders") then
    rule("vrf.cook_mesh_shaders")
        before_build(function (target)
            import("core.project.project")

            local pkg = project.required_package("vshadersystem~host")
            assert(pkg, "vrf_cook_shaders is on but the vshadersystem~host tool is unavailable")
            local vshaderc = path.join(pkg:installdir(), "bin", "vshaderc")
            if is_host("windows") and not os.isfile(vshaderc) then
                vshaderc = vshaderc .. ".exe"
            end
            assert(os.isfile(vshaderc), "vshaderc host tool not found: " .. vshaderc)

            local shaderRoot = path.join(target:scriptdir(), "shaders")
            local outlib     = path.join(shaderRoot, "mesh.vshlib")
            -- Dev-time "all platforms": DXBC/DXIL need the Windows compilers; on other hosts they
            -- come out empty (best-effort). Release builds strip to the shipping targets.
            os.vrunv(vshaderc, {"build", "--shader_root", shaderRoot, "-o", outlib, "--dxbc", "--dxil"})
            cprint("${green}[cook]${clear} regenerated shaders/mesh.vshlib (all backends)")
        end)
    rule_end()
end

-- Embed the committed binary shader library (shaders/mesh.vshlib) at build time as a byte-array
-- header (mesh.vshlib.h in the target's autogen dir), instead of committing a ~MB C header.
-- Release stripping: when the vshaderc host tool is available (vrf_cook_shaders), the lib is first
-- stripped to the bytecode for the compiled-in backends (vrf_backend_*) - dev keeps all backends,
-- a release with only the shipping backends enabled drops the rest. Without the tool the full lib
-- is embedded (dev default). main.cpp wraps mesh.vshlib.h in `const unsigned char g_meshVshlib[]`.
rule("vrf.embed_mesh_shaders")
    on_config(function (target)
        target:add("includedirs", path.join(target:autogendir(), "shaders"))
    end)
    before_build(function (target)
        import("core.project.project")
        local shaderdir = path.join(target:scriptdir(), "shaders")
        local src       = path.join(shaderdir, "mesh.vshlib")
        local gendir    = path.join(target:autogendir(), "shaders")
        os.mkdir(gendir)

        -- Enabled backends -> strip API list (spirv covers vulkan/gl/metal).
        local apis = {}
        if has_config("vrf_backend_vulkan") or has_config("vrf_backend_gl") or has_config("vrf_backend_metal") then
            table.insert(apis, "vulkan")
        end
        if has_config("vrf_backend_wgpu") then table.insert(apis, "webgpu") end
        if has_config("vrf_backend_d3d12") then table.insert(apis, "d3d12") end

        local libpath = src
        local host    = project.required_package("vshadersystem~host")
        if host and #apis > 0 and #apis < 3 then -- < all-of {vulkan, webgpu, d3d12} => worth stripping
            local vshaderc = path.join(host:installdir(), "bin", "vshaderc")
            if is_host("windows") and not os.isfile(vshaderc) then
                vshaderc = vshaderc .. ".exe"
            end
            if os.isfile(vshaderc) then
                libpath = path.join(gendir, "mesh.stripped.vshlib")
                os.vrunv(vshaderc, {"strip", "-i", src, "-o", libpath, "--api", table.concat(apis, ",")})
                cprint("${green}[embed]${clear} stripped mesh.vshlib to: %s", table.concat(apis, ","))
            end
        end

        -- bin2c: emit the array body (bytes only) for main.cpp to wrap.
        local data  = io.readfile(libpath, {encoding = "binary"})
        local parts = {}
        for i = 1, #data do
            parts[#parts + 1] = string.format("0x%02x,", data:byte(i))
            if i % 20 == 0 then
                parts[#parts + 1] = "\n"
            end
        end
        io.writefile(path.join(gendir, "mesh.vshlib.h"), table.concat(parts))
    end)
rule_end()

target("vrf_example_model_viewer")
    set_kind("binary")
    set_languages("cxx23")

    add_deps("vrf") -- inherits vrf's public headers + vri/glm/sdl3/glfw (+ imgui when enabled)

    if has_config("vrf_cook_shaders") then
        add_rules("vrf.cook_mesh_shaders")
    end
    add_rules("vrf.embed_mesh_shaders")

    -- native file-open dialog for the model picker (only when ImGui is enabled)
    if has_config("vrf_with_imgui") then
        add_packages("nativefiledialog-extended")
    end

    add_files("main.cpp")

    set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/vrf_example_model_viewer")
