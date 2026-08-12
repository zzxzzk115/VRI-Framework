-- All vrf_* option declarations, shared by the standalone build (root xmake.lua)
-- and by consumers that embed the framework via `includes("<vrf>/embed.lua")`.

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

-- Block-compress baked textures (asset_cache). Uses libktx's UASTC encoder, transcoded to
-- BC7 at bake time so runtime stays a plain read. Same libktx dependency as the KTX2 loader;
-- without it the bake still works, storing textures uncompressed (~4x the disk).
option("vrf_bake_bc7")
    set_default(false)
    set_showmenu(true)
    set_description("Block-compress baked cache textures to BC7 (libktx UASTC encoder)")
option_end()

-- Optional Dear ImGui integration (Application onGui + RenderModule draw, via VRI's imgui
-- extension and the SDL3/GLFW platform backends for input).
option("vrf_with_imgui")
    set_default(true)
    set_showmenu(true)
    set_description("Enable the Dear ImGui integration")
option_end()

-- Optional OpenXR support (vrf::xr): XrSystem device-creation probe + XrSession stereo rig
-- over VRI's interop extension (XR_KHR_vulkan_enable2 + wrapped swapchain images). The
-- OpenXR-free parts of vrf::xr (StereoRig, SimStereoRig - desktop stereo emulation) are
-- always built. Vulkan backend only.
option("vrf_with_openxr")
    set_default(false)
    set_showmenu(true)
    set_description("Enable OpenXR support (vrf::xr XrSystem/XrSession; needs the Vulkan backend)")
option_end()

-- Optional Tracy profiler integration (zones in the frame stream, framegraph
-- services, and XR session; VRF_ZONE for app code).
option("vrf_with_tracy")
    set_default(false)
    set_showmenu(true)
    set_description("Enable Tracy profiler zones")
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
