-- Build rules shared by the standalone build and embedding consumers.

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

            -- macOS: an absolute path in `links` is emitted as `-L<sdk>/lib -lvulkan`, and that
            -- linkdir lands BEFORE the package linkdirs. The macOS Vulkan SDK also ships
            -- libglslang.a / libSPIRV.a, so the SDK's copies then win `-lglslang` / `-lSPIRV`
            -- over the glslang package's - and the SDK's SPIRV was built with ENABLE_OPT, so the
            -- link dies on ~20 undefined spvtools:: symbols the package never asked for. Pass the
            -- dylib's absolute path straight to the linker instead; no -L, nothing shadowed.
            if target:is_plat("macosx") then
                target:add("ldflags", lib_path, { force = true, public = true })
                return
            end

            target:add("links", lib_path, { public = true })
        end
    end)
rule_end()
-- Multi-target ISPC emits a dispatcher plus one object per ISA. All three must
-- enter vrf's archive; the generic utils.ispc rule only adds the dispatcher.
rule("vrf.bc7-ispc")
    set_extensions(".ispc")
    on_config(function (target)
        target:add("includedirs", path.join(target:autogendir(), "bc7-ispc"))
    end)
    before_buildcmd_file(function (target, batchcmds, sourcefile, opt)
        import("core.project.project")
        import("lib.detect.find_tool")
        local pkg = project.required_package("ispc")
        local ispc = assert(find_tool("ispc", {paths = pkg and {pkg:installdir("bin")}}), "ispc not found")
        local object = target:objectfile(sourcefile)
        local headerdir = path.join(target:autogendir(), "bc7-ispc")
        local header = path.join(headerdir, "bc7e_ispc.h")
        local objects = {object}
        for _, isa in ipairs({"sse2", "avx2"}) do
            table.insert(objects, path.join(path.directory(object), path.basename(object) .. "_" .. isa .. path.extension(object)))
        end
        for _, file in ipairs(objects) do
            table.insert(target:objectfiles(), file)
        end
        local flags = {"--target=sse2-i32x4,avx2-i32x8", "--arch=x86-64", "-O2", "--opt=disable-assertions"}
        if target:is_plat("windows") then
            table.insert(flags, "--target-os=windows")
        elseif target:is_plat("macosx") then
            table.insert(flags, "--target-os=macos")
        else
            table.insert(flags, "--target-os=linux")
        end
        if not target:is_plat("windows") then
            table.insert(flags, "--pic")
        end
        table.join2(flags, {"-h", header, "-o", object, sourcefile})
        batchcmds:show_progress(opt.progress, "${color.build.object}compiling.bc7-ispc %s", sourcefile)
        batchcmds:mkdir(path.directory(object))
        batchcmds:mkdir(headerdir)
        batchcmds:vrunv(ispc.program, flags)
        batchcmds:add_depfiles(sourcefile)
        batchcmds:set_depmtime(os.mtime(object))
        batchcmds:set_depcache(target:dependfile(object))
    end)
rule_end()
