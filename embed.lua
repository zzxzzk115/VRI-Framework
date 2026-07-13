-- embed.lua - include the framework from ANOTHER xmake project (sibling-repo
-- co-development), without its tests/examples:
--
--   -- consumer's root xmake.lua:
--   add_repositories("my-xmake-repo https://github.com/zzxzzk115/xmake-repo.git backup")
--   includes("../VRI-Framework/embed.lua")
--   target("app") ... add_deps("vrf")
--
-- The consumer configures the vrf_* options like any of its own (e.g.
-- `xmake f --vrf_with_openxr=y`). Windows consumers must also align package
-- runtimes to MT/MTd (see the add_requireconfs block in the root xmake.lua).
includes("xmake/options.lua")
includes("xmake/rules.lua")
includes("external")
includes("source")
