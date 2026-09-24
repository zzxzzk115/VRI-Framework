target("vrf-tests")
    set_kind("binary")
    set_languages("cxx23")
    set_default(false)

    add_deps("vrf")
    add_packages("doctest")

    add_files("test_*.cpp")
    add_files("../external/bc7enc/bc7decomp.cpp")
    add_includedirs("../source/vrf/src", "../external/bc7enc")
    if has_config("vrf_bake_bc7") then
        add_defines("VRF_TEST_BC7")
    end
    add_tests("default")
    if has_config("vrf_loader_fbx") then
        add_defines("VRF_TEST_FBX")
    end

    -- Point the tests at the committed fixture assets (tests/assets/*). Forward slashes so
    -- the path is a valid C string literal on Windows too.
    local asset_dir = path.join(os.scriptdir(), "assets"):gsub("\\", "/")
    add_defines("VRF_TEST_ASSET_DIR=\"" .. asset_dir .. "\"")

    set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/tests")

if has_config("vrf_loader_fbx") then
    target("vrf-inspect-fbx")
        set_kind("binary")
        set_languages("cxx23")
        set_default(false)
        add_deps("vrf")
        add_files("fbx_inspect.cpp")
        set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/tests")
end
