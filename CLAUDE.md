# knOBS

Windows tray app that runs the user's OBS mic filter chain through their installed libobs and sends the result to a virtual audio cable, with no OBS process. Design, milestones and rationale are in [plan.md](plan.md). Read it before non-trivial work. When a decision changes, update it and tick milestone boxes as work lands.

Status: M0 (runtime bootstrap) is done against OBS 32.2.2. In M1, the offline harness passes its gate and the live path is verified up to VB-Cable without a mic. The mic runs (the iD4 into VB-Cable, latency vs OBS) are still to do.

## Stack
- C++20, CMake, MSVC (VS 2022 or newer; the preset uses the newest installed Visual Studio), x64 only, static CRT.
- Plain Win32 for UI. No Qt or other UI frameworks.
- Windows-only in v1. Keep OS-specific code behind thin seams so a port stays possible.

## Build & test
Run these from a Developer PowerShell for VS, which puts `cmake` and `ctest` on PATH:

```powershell
cmake --preset x64            # configure
cmake --build --preset debug  # or: release
ctest --preset debug          # unit tests, smoke test, harness (the last two skip if OBS isn't installed)
```

The dev tools in `build\x64\<config>\` each take `--help`:
- `knobs-smoke`: the M0 bootstrap check. Options include `--video dummy`, `--list-files`, `--obs-dir`, and opt-in mic capture (`--capture-seconds`).
- `knobs-harness`: pushes a WAV (or a built-in test signal) through a filter chain offline and checks that runs are bit-identical. `--source <json>` takes an OBS source object; `--out` writes the result. It opens no audio devices.
- `knobs-live`: the live path into a virtual cable. `--list-devices`, `--measure-output` and `--measure-cable` don't use the mic. `--measure-output` and `--measure-cable` play test clicks into VB-Cable and refuse to run while another app is using the cable. `--run` and `--measure-mic` open the mic.

Don't open the mic without the user's go-ahead.

## Layout
- `src/runtime/`: finding OBS, the runtime copy, loading `obs.dll` and its function table, the libobs session, logging, and `ObsHost`, which runs all of those in order.
- `src/audio/`: the live path. Device lists, and `LiveChain`, which loads a source through OBS's loader and monitors it.
- `src/util/`: `Result`, UTF-8, JSON and Win32 helpers, knOBS's app folders.
- `tools/common/`: code shared by the dev tools (console output, WAV I/O, the push source, energy envelopes). It isn't part of the app.
- `tools/smoke/`, `tools/harness/`, `tools/live/`: the dev tools above. `tools/vendor-libobs-headers.ps1` refreshes `third_party/libobs`.
- `tests/`: unit tests. These don't need OBS.
- `third_party/libobs/`: vendored libobs headers (declarations only). Don't edit them.

## Invariants
- **Fidelity is the product.** Audio passes only through OBS's own filter code. No custom DSP and no "improvements".
- **`%AppData%\obs-studio` is read-only.** knOBS keeps its own state in `%AppData%\knOBS` and `%LocalAppData%\knOBS`.
- **Never load from the OBS install dir.** Load only from the shadow copy in `%LocalAppData%\knOBS\runtime\<obs-version>\`.
- **No link-time libobs dependency.** Resolve exports into the function table (`KNOBS_OBS_API` in `src/runtime/obs_api.h`) with `GetProcAddress`. Add new libobs functions there. A missing export fails gracefully with a clear message.
- **Load only `win-wasapi` and `obs-filters`.** No `obs-vst` in v1.
- **Never ship libobs.** A from-source build is only for local debugging.
- Import uses `obs_load_source()`, in its private form `obs_load_private_source()`. Manual replay is the fallback, not the default.
- Check OBS behavior (config keys, scene JSON, libobs semantics) against the obs-studio source at the tag matching the installed version. Don't rely on memory.
- When the tested OBS version changes, re-vendor the headers for that tag and revisit the supported range in `src/runtime/obs_version.h`.

## Conventions
- Every source file starts with `// SPDX-License-Identifier: GPL-2.0-or-later`.
- Keep the display name "knOBS" in one constant. It may become "Knobs" before release.

## Git
- Commit straight to `main`.
- Conventional Commits: `type(scope): subject`. Types: `feat`, `fix`, `refactor`, `perf`, `test`, `docs`, `build`, `chore`. Optional scopes: `runtime`, `import`, `audio`, `harness`, `tray`.
- Imperative, lowercase subject, no trailing period, 72 chars max. Add a body only when the why isn't obvious.
