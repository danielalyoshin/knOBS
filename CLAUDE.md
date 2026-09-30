# knOBS

Windows tray app that runs the user's OBS mic filter chain through their installed libobs and sends the result to a virtual audio cable, with no OBS process. Design, milestones and rationale are in [plan.md](plan.md). Read it before non-trivial work. When a decision changes, update it and tick milestone boxes as work lands.

Status: no code yet. Starting M0.

## Stack
- C++20, CMake, MSVC (VS 2022), x64 only.
- Plain Win32 for UI. No Qt or other UI frameworks.
- Windows-only in v1. Keep OS-specific code behind thin seams so a port stays possible.

## Build & test
Not set up yet. Add the configure, build and test commands here when the first `CMakeLists.txt` lands.

## Invariants
- **Fidelity is the product.** Audio passes only through OBS's own filter code. No custom DSP and no "improvements".
- **`%AppData%\obs-studio` is read-only.** knOBS keeps its own state in `%AppData%\knOBS` and `%LocalAppData%\knOBS`.
- **Never load from the OBS install dir.** Load only from the shadow copy in `%LocalAppData%\knOBS\runtime\<obs-version>\`.
- **No link-time libobs dependency.** Resolve exports into the function table with `GetProcAddress`. A missing export fails gracefully with a clear message.
- **Load only `win-wasapi` and `obs-filters`.** No `obs-vst` in v1.
- **Never ship libobs.** A from-source build is only for local debugging.
- Import uses `obs_load_source()`. Manual replay is the fallback, not the default.
- Check OBS behavior (config keys, scene JSON, libobs semantics) against the obs-studio source at the tag matching the installed version. Don't rely on memory.

## Conventions
- Every source file starts with `// SPDX-License-Identifier: GPL-2.0-or-later`.
- Keep the display name "knOBS" in one constant. It may become "Knobs" before release.

## Git
- Commit straight to `main`.
- Conventional Commits: `type(scope): subject`. Types: `feat`, `fix`, `refactor`, `perf`, `test`, `docs`, `build`, `chore`. Optional scopes: `runtime`, `import`, `audio`, `harness`, `tray`.
- Imperative, lowercase subject, no trailing period, 72 chars max. Add a body only when the why isn't obvious.
