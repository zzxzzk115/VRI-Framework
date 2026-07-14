-- compute_saxpy: validation example for the vrf ComputePipelineBuilder. Headless:
-- dispatches a SAXPY compute shader over two storage buffers and verifies the
-- readback. The compute counterpart to rt_triangle.

if has_config("vrf_cook_shaders") then
    rule("vrf.cook_compute_saxpy_shaders")
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
            local outlib     = path.join(shaderRoot, "compute_saxpy.vshlib")
            os.vrunv(vshaderc, {"build", "--shader_root", shaderRoot, "-o", outlib, "--no-wgsl"})
            cprint("${green}[cook]${clear} regenerated shaders/compute_saxpy.vshlib")
        end)
    rule_end()
end

rule("vrf.embed_compute_saxpy_shaders")
    on_config(function (target)
        target:add("includedirs", path.join(target:autogendir(), "shaders"))
    end)
    before_build(function (target)
        local src    = path.join(target:scriptdir(), "shaders", "compute_saxpy.vshlib")
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
        io.writefile(path.join(gendir, "saxpy.vshlib.h"), table.concat(parts))
    end)
rule_end()

target("vrf_example_compute_saxpy")
    set_kind("binary")
    set_languages("cxx23")

    add_deps("vrf")

    if has_config("vrf_cook_shaders") then
        add_rules("vrf.cook_compute_saxpy_shaders")
    end
    add_rules("vrf.embed_compute_saxpy_shaders")

    add_files("main.cpp")

    set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/vrf_example_compute_saxpy")
