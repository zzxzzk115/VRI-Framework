# Contributing to VRI-Framework

Thanks for helping out! A few conventions keep the tree consistent across platforms.

## One-time setup

Enable the git hooks and provision the pinned formatter (clang-format 20.1.0) into a
repo-local virtualenv:

```sh
scripts/setup-hooks.sh        # Linux / macOS
```
```powershell
scripts\setup-hooks.ps1       # Windows
```

This points `core.hooksPath` at `scripts/hooks/` and installs clang-format 20.1.0 under
`scripts/.cf-venv/` (git-ignored). If you already have clang-format 20.x on `PATH`, it
uses that and skips the download. No usable Python? The script prints how to get
clang-format another way — until it's available the hook just **skips** (it never blocks
a commit), and CI stays the backstop.

## Code style

House style mirrors [VRI](../VRI): namespace `vrf`; types/structs/enums `CamelCase`;
**methods `PascalCase`** (`CreateBuffer`, `Init`, `Run`); private members `m_camelBack`;
static class members `s_`; locals/params `camelBack`; public/struct data members bare
`camelBack`. Public headers are C++-only `.hpp` with `#pragma once`. Errors use
`vrf::Expected<T>` (`std::expected<T, vrf::Error>`).

C++ is formatted with **clang-format 20.1.0** (the root `.clang-format`, byte-identical to
VRI's). CI pins that exact version, so differences between clang-format majors matter — use
the provisioned one. The pre-commit hook checks the **staged** C/C++ files under
`source/ examples/ tests/` with the same command CI runs (`clang-format --dry-run --Werror`)
and blocks a commit that isn't formatted, printing the fix.

Check or fix the whole tree yourself:

```sh
scripts/check-format.sh          # check (exit 1 on violations)
scripts/check-format.sh --fix    # reformat in place
```
```powershell
scripts\check-format.ps1         # check
scripts\check-format.ps1 -Fix    # reformat in place
```

Bypass the hook for a single commit (e.g. a WIP checkpoint):

```sh
git commit --no-verify
```

Generated shader headers (`examples/*/shaders/*_spv.h`, `mesh_vshlib.h`) are exempt via
their own `DisableFormat` `.clang-format` files, and the vendored third-party under
`external/` (GaussForge, spz) is never formatted — CI only checks `source/ examples/ tests/`.

## Commits

Commit messages follow [Conventional Commits](https://www.conventionalcommits.org)
(`feat:`, `fix:`, `docs:`, `chore:`, `style:`, …), matching the existing history.

## Building & running

See the [README](README.md) for build/run instructions and the framework's options
(window backends, VRI backend selection, optional loaders).
