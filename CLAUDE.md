# knobs

Windows tray app that runs the user's OBS mic filter chain through their installed libobs and sends the result to a virtual audio cable, with no OBS process. Design, milestones and rationale are in [plan.md](plan.md). Read it before non-trivial work. When a decision changes, update it and tick milestone boxes as work lands.

Status: M0 (runtime bootstrap) and M1 (live path and offline harness) are done against OBS 32.2.2, and the mic-to-cable latency matches OBS. M2 (config import and the comparison against OBS) is done apart from OBS 33.0 support, which waits for 33.0's release: knobs's output is bit-identical to OBS's, on test signals and real voice. M3 (tray app) is under way: the always-on core (`src/core`, plan.md "The always-on core"), the tray shell (`src/tray`, `knobs.exe`, plan.md "Built: the tray shell") and the first run (plan.md "Built: the first run") are built; the tray glyph and notifications are next.

## Stack
- C++20, CMake, MSVC (VS 2022 or newer; the preset uses the newest installed Visual Studio), x64 only, static CRT.
- Plain Win32 for UI. No Qt or other UI frameworks.
- Windows-only in v1. Keep OS-specific code behind thin seams so a port stays possible.

## Build & test
Run these from a Developer PowerShell for VS, which puts `cmake` and `ctest` on PATH:

```powershell
cmake --preset x64            # configure
cmake --build --preset debug  # or: release
ctest --preset debug          # unit tests, then smoke, harness, import and core tests (these skip if OBS isn't installed)
```

The build puts the tray app, `knobs.exe`, in `build\x64\<config>\`. It runs the real core: unless OBS is open or something needs setting up, it opens the mic. Use `knobs-tray` to look at the tray and the first run instead.

The dev tools in `build\x64\<config>\` each take `--help`:
- `knobs-smoke`: the M0 bootstrap check. Options include `--video dummy`, `--list-files`, `--obs-dir`, and opt-in mic capture (`--capture-seconds`).
- `knobs-import`: imports the mic from OBS's active profile and scene collection and reports each step: settings found, mics, pre-flight warnings, the chain libobs loads. `--obs-config` points it at another OBS settings folder, `--pick` chooses among several mics, `--save` writes the source object. It opens no audio devices.
- `knobs-harness`: pushes a WAV (or a built-in test signal) through a filter chain offline and checks that runs are bit-identical. `--import` uses the imported mic's chain; `--source <json>` takes an OBS source object; `--out` writes the result. It opens no audio devices.
- `knobs-compare`: runs the same input through the imported chain in knobs and in OBS itself, and measures the difference. OBS runs from a portable copy of the install in `%LocalAppData%\knobs\compare`, minimized to the tray, with settings of its own. It opens no audio device and leaves `%AppData%\obs-studio` alone. `--obs-wav` compares with an existing OBS recording instead.
- `knobs-core`: the always-on core with no tray. It prints each change of state, and takes keys to pause, resume and re-import. By default it opens no audio device: the chain loads as a push source and isn't monitored (`--stall` fakes a stalled mic). `--live` opens the mic and the cable. `--ignore-obs` stops it pausing for OBS.
- `knobs-tray`: the tray app on a fake core: the real core over a made-up OBS and devices. `--state` picks the state (every one the menu can show), `--theme light|dark` the menu's theme, and `--screenshot` saves a picture of the menu (`--open` opens a submenu or a dialog for it). `--first-run <page>` opens any page of the first run on those fake findings, `--obs-config tests\fixtures\obs-config` imports a real OBS settings folder with the installed OBS's libobs instead, and `--press` scripts keys that open and close OBS, plug cables in and add filters, and click the first run's buttons. It opens no audio device, never starts OBS, and reads nothing of OBS's but the install and a settings folder it's given.
- `knobs-live`: the live path into a virtual cable. `--import` runs the imported mic, monitored to the profile's monitoring device unless that's `default` (then `--output` is required). `--list-devices`, `--measure-output` and `--measure-cable` don't use the mic. `--measure-output` and `--measure-cable` play test clicks into VB-Cable and refuse to run while another app is using the cable. `--run` and `--measure-mic` open the mic.

Don't open the mic without the user's go-ahead.

## Layout
- `src/runtime/`: finding OBS, the runtime copy, loading `obs.dll` and its function table, the libobs session, logging, and `ObsHost`, which runs all of those in order.
- `src/import/`: reading OBS's settings (an INI reader that matches libobs's parser, the active profile and scene collection), finding the mics in a collection, and the pre-flight checks.
- `src/audio/`: the live path. Device lists (libobs's, and Windows' own with `DeviceWatch` for devices coming and going), and `LiveChain`, which loads a source through OBS's loader and monitors it.
- `src/core/`: the always-on core the tray runs. `Controller` makes the decisions with no threads or clock of its own, on a `Backend` (`ObsBackend` is the real one). `Core` runs it on its own thread with libobs, and `ObsWatch` tells whether OBS is running. The tray hears state through `Observer`.
- `src/tray/`: the tray app. `TrayApp` is the icon and the menu; `BuildMenu` turns a snapshot into the menu as data, which the unit tests check for every state. `FirstRun` is the first run as data in the same way (pages, buttons, what each click does), and `FirstRunDialog` shows it in one task dialog. Also dark menus, the settings file, the Run entry, the single-instance lock and starting OBS. `src/main.cpp` is `knobs.exe`'s entry point.
- `src/util/`: `Result`, UTF-8, JSON, text file and Win32 helpers, knobs's app folders.
- `tools/common/`: code shared by the dev tools (console output, WAV files, the push source and offline runs, the test signal, the tools' start-up and import steps, aligning and diffing audio, energy envelopes and peaks, a seeded RNG). It isn't part of the app.
- `tools/smoke/`, `tools/import/`, `tools/harness/`, `tools/compare/`, `tools/live/`, `tools/core/`, `tools/tray/`: the dev tools above. `tools/tray/` also has the fake backend and the screenshots. `tools/vendor-libobs-headers.ps1` refreshes `third_party/libobs`.
- `tests/`: unit tests, which don't need OBS (`core_tests.cpp` runs the core on a fake backend, `tray_tests.cpp` checks the menu, the settings file, the Run entry on a scratch key, the lock, and starting OBS with a stand-in, and `first_run_tests.cpp` the first run's pages and moves), and the import and core tests, which run `knobs-import` and `knobs-core` on the made-up OBS settings in `tests/fixtures/obs-config`.
- `third_party/libobs/`: vendored libobs headers (declarations only). Don't edit them.
- `assets/`: the logo. The SVGs are the masters, and `knobs.ico` is the app icon, built into `knobs.exe`. `assets/README.md` has the colors and usage rules.
- `PRODUCT.md`: who knobs is for, its voice and brand commitments. Read it before UI or copy work.

## Invariants
- **Fidelity is the product.** Audio passes only through OBS's own filter code. No custom DSP and no "improvements".
- **`%AppData%\obs-studio` is read-only**, as is a portable OBS's `config` folder. knobs keeps its own state in `%AppData%\knobs` and `%LocalAppData%\knobs`. Some libobs helpers write: `obs_data_create_from_json_file_safe` renames a backup over a broken file, so read files yourself and parse them with `obs_data_create_from_json`.
- **Never load from the OBS install dir.** Load only from the shadow copy in `%LocalAppData%\knobs\runtime\<obs-version>\`.
- **No link-time libobs dependency.** Resolve exports into the function table (`KNOBS_OBS_API` in `src/runtime/obs_api.h`) with `GetProcAddress`. Add new libobs functions there. A missing export fails gracefully with a clear message.
- **Load only `win-wasapi` and `obs-filters`.** No `obs-vst` in v1.
- **Never ship libobs.** A from-source build is only for local debugging.
- Import uses `obs_load_source()`, in its private form `obs_load_private_source()`. Manual replay is the fallback, not the default.
- Check OBS behavior (config keys, scene JSON, libobs semantics) against the obs-studio source at the tag matching the installed version. Don't rely on memory.
- When the tested OBS version changes, re-vendor the headers for that tag and revisit the supported range in `src/runtime/obs_version.h`.

## Conventions
- Every source file starts with `// SPDX-License-Identifier: GPL-2.0-or-later`.
- The name is "knobs", all lowercase, even at the start of a sentence. It was "knOBS" until 2026-10-05. Keep it in one constant (`KNOBS_DISPLAY_NAME` in `src/app_info.h`), and don't write it out in user-facing strings; tests shouldn't depend on it either.

## Git
- Commit straight to `main`.
- Conventional Commits: `type(scope): subject`. Types: `feat`, `fix`, `refactor`, `perf`, `test`, `docs`, `build`, `chore`. Optional scopes: `runtime`, `import`, `audio`, `harness`, `tray`.
- Imperative, lowercase subject, no trailing period, 72 chars max. Add a body only when the why isn't obvious.
