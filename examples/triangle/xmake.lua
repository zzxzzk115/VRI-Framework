target("vrf_example_triangle")
    set_kind("binary")
    set_languages("cxx23")

    add_deps("vrf") -- inherits vrf's public headers + vri/glm/sdl3/glfw

    -- the embedded shader blob is included as "shaders/triangle_spv.h" relative to main.cpp
    add_includedirs(os.scriptdir())
    add_files("main.cpp")

    set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)/vrf_example_triangle")
