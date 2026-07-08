target("vrf-tests")
    set_kind("binary")
    set_languages("cxx23")
    set_default(false)

    add_deps("vrf")
    add_packages("doctest")

    add_files("test_*.cpp")
    add_tests("default")

    -- Point the tests at the committed fixture assets (tests/assets/*). Forward slashes so
    -- the path is a valid C string literal on Windows too.
    local asset_dir = path.join(os.scriptdir(), "assets"):gsub("\\", "/")
    add_defines("VRF_TEST_ASSET_DIR=\"" .. asset_dir .. "\"")

    set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/tests")
