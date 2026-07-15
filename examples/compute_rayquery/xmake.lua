-- compute_rayquery: inline ray tracing (RayQuery) validation. Headless: a
-- one-triangle TLAS traced by a compute shader, gated on hasRayQuery so it runs
-- on the Metal backend too. The inline counterpart to rt_triangle.

if has_config("vrf_cook_shaders") then
    rule("vrf.cook_compute_rayquery_shaders")
        before_build(function (target)
            import("core.project.project")
            local pkg = project.required_package("vshadersystem~host")
            assert(pkg, "vrf_cook_shaders is on but the vshadersystem~host tool is unavailable")
            local vshaderc = path.join(pkg:installdir(), "bin", "vshaderc")
            if is_host("windows") and not os.isfile(vshaderc) then vshaderc = vshaderc .. ".exe" end
            assert(os.isfile(vshaderc), "vshaderc host tool not found: " .. vshaderc)
            local shaderRoot = path.join(target:scriptdir(), "shaders")
            os.vrunv(vshaderc, {"build", "--shader_root", shaderRoot, "-o", path.join(shaderRoot, "rayquery.vshlib"), "--no-wgsl"})
            cprint("${green}[cook]${clear} regenerated shaders/rayquery.vshlib")
        end)
    rule_end()
end

rule("vrf.embed_compute_rayquery_shaders")
    on_config(function (target)
        target:add("includedirs", path.join(target:autogendir(), "shaders"))
    end)
    before_build(function (target)
        local src    = path.join(target:scriptdir(), "shaders", "rayquery.vshlib")
        local gendir = path.join(target:autogendir(), "shaders")
        os.mkdir(gendir)
        local data  = io.readfile(src, {encoding = "binary"})
        local parts = {}
        for i = 1, #data do
            parts[#parts + 1] = string.format("0x%02x,", data:byte(i))
            if i % 20 == 0 then parts[#parts + 1] = "\n" end
        end
        io.writefile(path.join(gendir, "rayquery.vshlib.h"), table.concat(parts))
    end)
rule_end()

target("vrf_example_compute_rayquery")
    set_kind("binary")
    set_languages("cxx23")
    add_deps("vrf")
    if has_config("vrf_cook_shaders") then
        add_rules("vrf.cook_compute_rayquery_shaders")
    end
    add_rules("vrf.embed_compute_rayquery_shaders")
    add_files("main.cpp")
    set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/vrf_example_compute_rayquery")
