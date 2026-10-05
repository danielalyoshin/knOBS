# knOBS

Windows tray app that runs the user's OBS mic filter chain through their installed libobs and sends the result to a virtual audio cable, with no OBS process. Design, milestones and rationale are in [plan.md](plan.md). Read it before non-trivial work. When a decision changes, update it and tick milestone boxes as work lands.

Status: M0 (runtime bootstrap) and M1 (live path and offline harness) are done against OBS 32.2.2, and the mic-to-cable latency matches OBS. In M2, config import and the comparison against OBS are done: knOBS's output is bit-identical to OBS's. The ABX item awaits a decision (plan.md, M2), and 33.0 support waits for its release. M3 (tray app) is next.

## Stack
- C++20, CMake, MSVC (VS 2022 or newer; the preset uses the newest installed Visual Studio), x64 only, static CRT.
- Plain Win32 for UI. No Qt or other UI frameworks.
- Windows-only in v1. Keep OS-specific code behind thin seams so a port stays possible.

## Build & test
Run these from a Developer PowerShell for VS, which puts `cmake` and `ctest` on PATH:

```powershell
cmake --preset x64            # configure
cmake --build --preset debug  # or: release
ctest --preset debug          # unit tests, then smoke, harness and import tests (these skip if OBS isn't installed)
```

The dev tools in `build\x64\<config>\` each take `--help`:
- `knobs-smoke`: the M0 bootstrap check. Options include `--video dummy`, `--list-files`, `--obs-dir`, and opt-in mic capture (`--capture-seconds`).
- `knobs-import`: imports the mic from OBS's active profile and scene collection and reports each step: settings found, mics, pre-flight warnings, the chain libobs loads. `--obs-config` points it at another OBS settings folder, `--pick` chooses among several mics, `--save` writes the source object. It opens no audio devices.
- `knobs-harness`: pushes a WAV (or a built-in test signal) through a filter chain offline and checks that runs are bit-identical. `--import` uses the imported mic's chain; `--source <json>` takes an OBS source object; `--out` writes the result. It opens no audio devices.
- `knobs-compare`: runs the same input through the imported chain in knOBS and in OBS itself, and measures the difference. OBS runs from a portable copy of the install in `%LocalAppData%\knOBS\compare`, minimized to the tray, with settings of its own. It opens no audio device and leaves `%AppData%\obs-studio` alone. `--obs-wav` compares with an existing OBS recording instead.
- `knobs-live`: the live path into a virtual cable. `--import` runs the imported mic, monitored to the profile's monitoring device. `--list-devices`, `--measure-output` and `--measure-cable` don't use the mic. `--measure-output` and `--measure-cable` play test clicks into VB-Cable and refuse to run while another app is using the cable. `--run` and `--measure-mic` open the mic.

Don't open the mic without the user's go-ahead.

## Layout
- `src/runtime/`: finding OBS, the runtime copy, loading `obs.dll` and its function table, the libobs session, logging, and `ObsHost`, which runs all of those in order.
- `src/import/`: reading OBS's settings (an INI reader that matches libobs's parser, the active profile and scene collection), finding the mics in a collection, and the pre-flight checks.
- `src/audio/`: the live path. Device lists, and `LiveChain`, which loads a source through OBS's loader and monitors it.
- `src/util/`: `Result`, UTF-8, JSON and Win32 helpers, knOBS's app folders.
- `tools/common/`: code shared by the dev tools (console output, WAV and text files, the push source and offline runs, the test signal, the tools' import steps, aligning and diffing audio, energy envelopes and peaks, a seeded RNG). It isn't part of the app.
- `tools/smoke/`, `tools/import/`, `tools/harness/`, `tools/compare/`, `tools/live/`: the dev tools above. `tools/vendor-libobs-headers.ps1` refreshes `third_party/libobs`.
- `tests/`: unit tests, which don't need OBS, and the import test, which runs `knobs-import` on the made-up OBS settings in `tests/fixtures/obs-config`.
- `third_party/libobs/`: vendored libobs headers (declarations only). Don't edit them.

## Invariants
- **Fidelity is the product.** Audio passes only through OBS's own filter code. No custom DSP and no "improvements".
- **`%AppData%\obs-studio` is read-only**, as is a portable OBS's `config` folder. knOBS keeps its own state in `%AppData%\knOBS` and `%LocalAppData%\knOBS`. Some libobs helpers write: `obs_data_create_from_json_file_safe` renames a backup over a broken file, so read files yourself and parse them with `obs_data_create_from_json`.
- **Never load from the OBS install dir.** Load only from the shadow copy in `%LocalAppData%\knOBS\runtime\<obs-version>\`.
- **No link-time libobs dependency.** Resolve exports into the function table (`KNOBS_OBS_API` in `src/runtime/obs_api.h`) with `GetProcAddress`. Add new libobs functions there. A missing export fails gracefully with a clear message.
- **Load only `win-wasapi` and `obs-filters`.** No `obs-vst` in v1.
- **Never ship libobs.** A from-source build is only for local debugging.
- Import uses `obs_load_source()`, in its private form `obs_load_private_source()`. Manual replay is the fallback, not the default.
- Check OBS behavior (config keys, scene JSON, libobs semantics) against the obs-studio source at the tag matching the installed version. Don't rely on memory.
- When the tested OBS version changes, re-vendor the headers for that tag and revisit the supported range in `src/runtime/obs_version.h`.

## Conventions
- Every source file starts with `// SPDX-License-Identifier: GPL-2.0-or-later`.
- Keep the display name "knOBS" in one constant. It becomes "Knobs" if the OBS team asks for a change.

## Git
- Commit straight to `main`.
- Conventional Commits: `type(scope): subject`. Types: `feat`, `fix`, `refactor`, `perf`, `test`, `docs`, `build`, `chore`. Optional scopes: `runtime`, `import`, `audio`, `harness`, `tray`.
- Imperative, lowercase subject, no trailing period, 72 chars max. Add a body only when the why isn't obvious.
