-- gaussian_splat: a 3D Gaussian Splatting renderer on the vrf framegraph
-- (instanced covariance-projected quads, premultiplied over-blend, CPU depth
-- sort, tonemap). Windowed; loads a .ply/.spz/.splat from argv[1] or generates
-- a synthetic sphere. Set VRF_EXAMPLE_AUTO_EXIT for a timed headless run.

if has_config("vrf_cook_shaders") then
    rule("vrf.cook_gaussian_splat_shaders")
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
            local outlib     = path.join(shaderRoot, "splat.vshlib")
            os.vrunv(vshaderc, {"build", "--shader_root", shaderRoot, "-o", outlib, "--no-wgsl"})
            cprint("${green}[cook]${clear} regenerated shaders/splat.vshlib")
        end)
    rule_end()
end

rule("vrf.embed_gaussian_splat_shaders")
    on_config(function (target)
        target:add("includedirs", path.join(target:autogendir(), "shaders"))
    end)
    before_build(function (target)
        local src    = path.join(target:scriptdir(), "shaders", "splat.vshlib")
        local gendir = path.join(target:autogendir(), "shaders")
        os.mkdir(gendir)

        local data  = io.readfile(src, {encoding = "binary"})
        local parts = {}
        for i = 1, #data do
            parts[#parts + 1] = string.format("0x%02x,", data:byte(i))
            if i % 20 == 0 then
                parts[#parts + 1] = "\n"
            end
        end
        io.writefile(path.join(gendir, "splat.vshlib.h"), table.concat(parts))
    end)
rule_end()

target("vrf_example_gaussian_splat")
    set_kind("binary")
    set_languages("cxx23")

    add_deps("vrf")

    if has_config("vrf_cook_shaders") then
        add_rules("vrf.cook_gaussian_splat_shaders")
    end
    add_rules("vrf.embed_gaussian_splat_shaders")

    add_files("main.cpp")

    set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/vrf_example_gaussian_splat")
