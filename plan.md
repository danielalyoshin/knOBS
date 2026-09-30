# knOBS — Project Plan

> **Your OBS mic chain, without OBS.** A lightweight Windows tray app that runs your exact OBS audio filter chain (gate, expander, compressor, noise suppression) against your microphone and outputs to a virtual audio cable — no OBS process required.

*Name: **knOBS**, pronounced "knobs" — the audio kind, with OBS inside it. Open item before public release: the OBS Project's [Forum Resource and IP Policy](https://obsproject.com/forum/threads/forum-resource-and-ip-policy.178569/) (Jan 2026) asks third-party tools to avoid the "OBS" acronym in names, and its branding rules apply to any app, not just forum submissions. Ask the OBS team before launch; fallback styling if needed: **Knobs**.*

---

## 1. Problem

XLR mic users who tuned a filter chain in OBS (noise suppression → gate → expander → compressor → limiter) and route it through a virtual cable get consistent, processed audio in every app — Discord, Zoom, games, browser. The cost: OBS must stay open forever, even when not recording or streaming.

This got worse in OBS 32.0: the `--disable-shutdown-check` flag is ignored, so the Safe Mode popup appears on every boot and blocks unattended background use ([obs-studio#12650](https://github.com/obsproject/obs-studio/issues/12650)). The reporter's use case is exactly this one — OBS as a background mic processor feeding VB-Cable.

Existing alternatives (Equalizer APO, VoiceMeeter, NVIDIA Broadcast, NoiseTorch) use **different DSP implementations**. OBS's gate/compressor/expander are custom code inside `obs-filters`; nothing else reproduces the sound users spent hours tuning. The only way to get identical processing is to run OBS's own filter code — and the most faithful way to do that is to load the user's own installed OBS binaries.

**No existing project does this.** Nearest neighbors: obs-headless (Linux RTMP compositing), obs-express-node (recording control server), atkAudio / Audio Monitor (OBS plugins — still require OBS open), NoiseTorch (Linux, RNNoise only). General-purpose libobs hosts exist — [libobs-rs](https://github.com/djmango/libobs-rs) (Rust; its bootstrapper uses official OBS binaries at runtime) and [pylibobs](https://pypi.org/project/pylibobs/) (Python) — but neither targets mic-chain replay.

## 2. Goals

- **G1 — Same audio as OBS.** Same filter binaries (loaded from the user's own OBS install), same settings, same order, same source-level processing (Mono, balance, volume). This is the entire reason the project exists. Verified by a deterministic harness, a comparison against OBS, and blind ABX (§7).
- **G2 — Zero-config import.** Read the user's active OBS profile + scene collection and recreate their mic source, source state, and filter chain automatically using OBS's own loader. No re-tuning.
- **G3 — Invisible footprint.** Tray icon, no window by default, no GPU render loop, target < 1% CPU idle-adjacent usage and small memory footprint.
- **G4 — Set-and-forget.** Launch on startup, survive device disconnect/reconnect (interface power cycles), coexist with OBS (auto-pause while OBS runs), fail loudly (tray notification) rather than silently.

## 3. Non-goals (v1)

- No video, no recording, no streaming, no encoder.
- No filter *editing* UI. Users tune in OBS; knOBS imports. (Editing is a possible v2.)
- **No VST filters.** Deferred to v2 (see §8). A chain containing a VST filter imports with that filter stripped and a loud warning.
- No macOS/Linux. Windows-only; audio monitoring backends are per-OS and Windows is the target user base. Architecture should not preclude ports.
- No custom DSP. If it's not an OBS filter module, it's out of scope.
- No building or bundling libobs for end users. knOBS runs on the user's installed OBS; a from-source build is a dev/debug tool only.

## 4. Architecture

```
┌──────────────────────────────────────────────────────────────┐
│ knOBS tray app (C++ / Win32)                                 │
│                                                              │
│  runtime loader ───► user's installed OBS, shadow-copied     │
│        │             to %LocalAppData%\knOBS\runtime\        │
│        │                                                     │
│  config importer ──► %AppData%\obs-studio\ (read-only)       │
│        │             user.ini → active profile + collection  │
│        ▼                                                     │
│  ┌───────────────── libobs (audio-only) ──────────────────┐  │
│  │ wasapi_input_capture (Audient iD4)                     │  │
│  │   loaded via obs_load_source():                        │  │
│  │   source state: Mono · balance · volume · sync         │  │
│  │   └─ filters[]: noise_suppress → gate →                │  │
│  │      expander → compressor → limiter                   │  │
│  │ monitoring: MONITOR_ONLY ─► VB-Cable input             │  │
│  └────────────────────────────────────────────────────────┘  │
│                                                              │
│  tray UI: start/stop · device + cable pickers ·              │
│           re-import · auto-pause while OBS runs · logs       │
└──────────────────────────────────────────────────────────────┘
```

### Runtime: the user's OBS, shadow-copied

knOBS does not ship libobs. At import time it:

1. **Locates the OBS install** — installer registry key (`HKLM\SOFTWARE\OBS Studio`, default value) → `C:\Program Files\obs-studio` → manual path picker (Steam / portable installs). The version comes from `obs.dll`'s version resource, so nothing is loaded from the install.
2. **Copies the needed subset** into `%LocalAppData%\knOBS\runtime\<obs-version>\`, mirroring the install layout (`bin\64bit`, `obs-plugins\64bit`, `data\libobs`, `data\obs-plugins\<module>`) so relative data lookups still resolve. Loading from the copy keeps OBS's own DLLs unlocked, so OBS updates aren't blocked by a running knOBS. The subset is computed from the PE import tables, not hardcoded: the import closure of `obs.dll`, `libobs-d3d11.dll` and the two modules, limited to DLLs the install ships, plus their PDBs, `data\libobs` and the two modules' data folders. For 32.2.2 that's 200 files, 53.6 MB, mostly FFmpeg: `obs.dll` imports avcodec, avformat, avutil, swscale and swresample directly, and those pull in libx264, librist, srt and zlib. The VC++ runtime and Windows DLLs come from the system. The copy happens in a staging folder with a manifest written last, so an interrupted copy is never used. Files are flushed to disk before the manifest. The manifest records each file's size and the install file's timestamp. The copy is reused only while the install still matches, because OBS drops beta and RC suffixes from `obs.dll`'s version resource, so the version alone can't tell a beta from the final release. Copies are serialized across processes with a named mutex. A replaced copy is renamed aside rather than deleted, so replacing fails as a whole while a running knOBS has it loaded.
3. **Loads `obs.dll` from the copy** and resolves the functions it needs (31 in M0) via `GetProcAddress` into a function table (`KNOBS_OBS_API` in `src/runtime/obs_api.h`, typed from the vendored headers in `third_party/libobs`). No import lib needed, and a missing export fails gracefully instead of at process load. `obs.dll`'s own dependencies resolve from its folder and the system, never PATH or the install. A successful load changes two process-wide settings; a failed load undoes both. `AddDllDirectory(bin\64bit)` lets libobs find the graphics module by bare name. The working directory becomes the copy's `bin\64bit`, because libobs resolves `../../data/libobs/` against the working directory (`obs-windows.c`, `find_libobs_data_file`), so nothing may change it while libobs runs (the folder picker uses `FOS_NOCHANGEDIR`). Separately, the host process calls `SetDefaultDllDirectories` at startup, so plain `LoadLibrary` calls skip PATH and the working directory.
4. **Checks `obs_get_version()`** against the supported range. Outside it, refuse to start with a clear tray message. The range is 32.2.0 up to, but not including, 33.0.0. The floor is the minor version of the vendored headers (32.2.2); patch releases don't change the libobs API, so 32.2.0 and 32.2.1 have the same declarations. The ceiling is the next major, which is where libobs makes breaking API changes (`obs-config.h`). Only 32.2.2 has been tested. The install's version is checked before anything is copied, and again from `obs_get_version()` after loading.

When the installed OBS version changes, knOBS notices on startup and prompts a re-import, which refreshes the runtime copy. The refresh needs a process restart: libobs never unloads module DLLs (`os_dlclose` is commented out in `free_module`), and they keep `obs.dll` loaded too.

**Modules loaded (only these):**
| Module | Provides |
|---|---|
| `win-wasapi` | `wasapi_input_capture` source |
| `obs-filters` | gate, expander, compressor, limiter, gain, EQ, noise suppression (Speex/RNNoise) |

`obs-vst` is not loaded in v1. Neither is `nv-filters`. In OBS 32 the NVIDIA noise removal is a separate filter (`nvidia_audiofx_filter`) in that module; `obs-filters` doesn't build its NVAFX path.

**Key libobs calls (happy path):**
1. `obs_startup("en-US", module_config_path, nullptr)` — `module_config_path` is knOBS's own `%AppData%\knOBS\module-config`, never OBS's. knOBS treats `%AppData%\obs-studio` as read-only. `obs_startup()` initializes COM as a single-threaded apartment on its calling thread, and `obs_shutdown()` uninitializes it, so both must run on the same thread, and not one already in a multithreaded apartment.
2. `base_set_log_handler()` → knOBS log file in `%LocalAppData%\knOBS\logs` (backs "open logs" in the tray). Set before `obs_startup` so startup is logged.
3. `obs_reset_audio()` — sample rate + channels from the active profile's `basic.ini`.
4. Video: **no** `obs_reset_video()` through M2. Audio flows without it. Device-loss recovery is the open question; see M0 findings.
5. Open + init only `win-wasapi` and `obs-filters` (`obs_open_module(bin, data)` with absolute paths into the runtime copy, then `obs_init_module`), then `obs_post_load_modules()`. No `obs_add_module_path()`: that only feeds `obs_load_all_modules()`, which knOBS doesn't use. As a side effect, OBS's safe-mode and disabled-module lists don't apply.
6. `obs_load_source(source_data)` with the mic's saved source object from the scene collection. OBS's own loader recreates the `wasapi_input_capture` source, its filters in order, and source-level state (volume, balance, mute, Mono flag, sync offset).
7. Post-load fixups: `obs_source_set_monitoring_type(src, OBS_MONITORING_TYPE_MONITOR_ONLY)`; clear push-to-talk / push-to-mute (no hotkeys in v1, otherwise the mic stays muted).
8. `obs_set_audio_monitoring_device()` → virtual cable (from imported config or knOBS setting).
9. Shutdown: release sources, drain, then `obs_shutdown()`. libobs destroys sources on a background queue, and `obs_shutdown()` only waits for it when a video thread exists (`obs_wait_for_destroy_queue`). The audio and graphics threads hold source references during each tick, so a released source can reach its last release on them. The drain therefore mirrors `obs_wait_for_destroy_queue`: a no-op task with `wait=true` on each running thread (graphics, then audio), then one on `OBS_TASK_DESTROY`. Skipping the audio round trip risks a source being destroyed after its module has unloaded.

### M0 findings (OBS 32.2.2)

Measured with `knobs-smoke` (`tools/smoke`), which runs the whole bootstrap and reports each step.

- **Audio works with no video.** Capture callbacks fire on the source's own thread after the filter chain (`obs_source_output_audio`), independent of the graphics thread. The smoke test captured the Audient iD4 through `gain_filter` with no `obs_reset_video()`.
- **The video tick drives more than video.** Only the graphics thread calls `obs_source_video_tick`, and it isn't exported. Without video:
  - Sources never get `activate`/`show` callbacks. win-wasapi creates its device-reconnect thread in `activate`, so a mic that disconnects, or is missing at startup, never recovers (G4). `obs_source_active()` only reports the activation ref count, so use the source's `activate` signal to see whether activation happened.
  - `video_tick` callbacks never run. The compressor looks up its sidechain source there. That only matters if a sidechain is set, and knOBS doesn't load other sources anyway, so it's an M2 pre-flight warning.
- **Cost** (Release build, 10 s capturing the iD4 through a gain filter):

  | Video mode | CPU (one core) | Working set | Private | Threads |
  |---|---|---|---|---|
  | None | 0.16% | 22 MB | 17 MB | 10 |
  | Dummy (8×8 @ 1 fps, D3D11) | 0.3–0.6% | 64 MB | 86 MB | 79 |

  A full load → startup → modules → shutdown cycle takes about 55 ms, with 0 leaked libobs allocations and no libobs warnings (the dummy canvas adds the usual GPU-scheduling warning).
- **Decision:** M1 and M2 run with no video. M3 decides how to survive device loss (G4):
  - (a) Use the dummy canvas so win-wasapi's own reconnect runs. It costs about 70 MB of private memory, about 70 threads, and a live D3D11 device on adapter 0, which can wake a discrete GPU on hybrid laptops.
  - (b) Stay video-less and recreate the source with `obs_load_source()` when the device comes back.

  Recommendation: (b). It re-runs OBS's own loader, so fidelity is unaffected, and it keeps the G3 footprint.

**Why `obs_load_source()`:** it's the same code path OBS uses to load the collection, and the scene JSON was written by the same OBS version whose loader reads it. That removes a hand-written settings replayer as a source of drift. Manual replay (`obs_source_create` + `obs_source_filter_add` + setters) is the fallback if it misbehaves.

### Config import

- **Active profile + scene collection:** `%AppData%\obs-studio\user.ini` (OBS 31+ split app/user settings out of `global.ini`), falling back to `global.ini` for OBS ≤ 30. `[Basic]` section.
- **Scene collection:** `%AppData%\obs-studio\basic\scenes\<SceneCollectionFile>.json`.
- **Candidate mic sources:** `wasapi_input_capture` objects in `sources[]` **and** global audio devices stored as top-level keys (`AuxAudioDevice1` … `AuxAudioDevice4`). If more than one, the user picks.
- **Profile settings:** `basic\profiles\<profile>\basic.ini` → `[Audio]` sample rate, channel setup, monitoring device.
- **Pre-flight checks** (before `obs_load_source`): strip `vst_filter` entries from the filters array (warn); flag unknown filter IDs, including `nvidia_audiofx_filter` (warn); warn if a compressor has a sidechain source set (knOBS doesn't load other sources, and without video the compressor never looks one up); detect push-to-talk / push-to-mute (clear + warn); show Mono on/off in the import summary.

Exact key names are verified against the installed OBS version in M2.

## 5. Milestones

**M0 — Runtime bootstrap** (done 2026-09-30 against OBS 32.2.2; see M0 findings in §4)
- [x] Locate OBS install (registry → default path → manual picker). The picker is `PickObsInstallFolder()`; the smoke test exposes it as `--pick-obs-dir`, and the tray wires it up in M3.
- [x] Work out the exact file set to shadow-copy (`dumpbin /dependents` on `obs.dll` + the two modules); build the runtime copier. The copier derives the set from the import tables itself (§4 step 2).
- [x] Grab headers from the obs-studio tag matching the installed version; build the `GetProcAddress` function table + version-range check. Headers are vendored by `tools/vendor-libobs-headers.ps1`.
- [x] Smoke test: console app that loads `obs.dll` from the runtime copy, runs `obs_startup` → loads the two modules → `obs_shutdown` cleanly, with data files resolving (`knobs-smoke`, also run by CTest)
- [x] Try audio-only init with no `obs_reset_video()`. Audio works, but activation doesn't (M0 findings).
- [ ] *(Optional, off critical path)* from-source libobs debug build for stepping through problems. Not needed yet; OBS's PDBs are copied with the runtime.

**M1 — Audio pipeline + determinism harness**
- [ ] Hardcoded: create iD4 source → add one gain filter → monitor to VB-Cable
- [ ] Verify audio arrives in another app; measure added latency vs OBS
- [ ] Offline harness: custom source pushes a 48 kHz float WAV through the chain in fixed chunks with synthetic timestamps; capture via `obs_source_add_audio_capture_callback` → WAV
- [ ] **Go/no-go gate:** harness output is bit-identical across runs, and live audio reaches VB-Cable with latency comparable to OBS.

**M2 — Config import + OBS comparison**
- [ ] Resolve active profile + scene collection (`user.ini` / `global.ini`); verify key names against the installed version
- [ ] Enumerate candidate mic sources (`sources[]` + `AuxAudioDevice*` keys); picker if multiple
- [ ] Pre-flight checks (VST strip, unknown IDs, compressor sidechain, push-to-talk/mute) with warnings
- [ ] Load via `obs_load_source()`; apply post-load fixups
- [ ] Comparison vs OBS: same WAV (leading ~2 s of silence so envelopes settle identically) through an OBS Media Source with the chain pasted on and Mono/balance matched → record 32-bit float PCM. Run the same file through the harness with the imported chain. Align via cross-correlation; residual must fall below a set threshold (expect ~zero — investigate anything audible).
- [ ] Blind ABX on real voice: OBS vs knOBS

**M3 — Tray app**
- [ ] Win32 tray: start/stop, device + cable pickers, re-import, autostart toggle, log access
- [ ] OBS folder picker when auto-detection fails; remember the choice in `%AppData%\knOBS`
- [ ] OBS coexistence: watch for `obs64.exe`; auto-pause while it runs, resume when it exits (toggleable)
- [ ] Detect installed-OBS version change on startup → prompt re-import / runtime refresh (restart the process to load the new copy; prune old copies)
- [ ] Device disconnect/reconnect. win-wasapi's reconnect thread only runs after `activate`, which needs the video tick (M0 findings). Choose between the dummy canvas and re-creating the source via `obs_load_source()`; either way, knOBS surfaces state rather than reimplementing capture.
- [ ] Error surfacing via tray notifications

**M4 — Ship**
- [ ] Packaging: small exe (no bundled libobs); simple installer or portable zip
- [ ] README with the pitch ("close OBS, keep your mic"), setup guide, VB-Cable pointer, "requires OBS Studio installed" + supported version range
- [ ] GPL-2.0-or-later compliance (links libobs): publish source, include license texts
- [ ] Name check: ask the OBS team about "knOBS" before public launch (fallback: Knobs)
- [ ] AI-use disclaimer in the forum post (required by the OBS Forum Resource and IP Policy)
- [ ] Post to OBS forums / r/obs — this is where the users who asked for this live

## 6. Risks & open questions

| Risk | Notes / mitigation |
|---|---|
| libobs ABI drift across OBS versions | `GetProcAddress` function table; tested version range; refuse to start outside it with a clear message. |
| OBS not installed / non-standard install (Steam, portable) | Registry → default path → manual picker. OBS installed is a stated requirement. |
| Loaded DLLs block OBS updates | Always load from the shadow copy in `%LocalAppData%\knOBS\runtime\`, never from the install dir. |
| Audio-only init has undocumented video dependencies | Confirmed in M0: activation and `video_tick` need the graphics thread. Audio is unaffected; device reconnect is decided in M3 (dummy canvas vs. source re-creation), with costs measured. |
| `data/` path resolution (`find_libobs_data_file`) | Resolved in M0: the copy mirrors the install, the working directory is the copy's `bin\64bit`, and module paths are passed explicitly. The smoke test verifies both. |
| Monitoring path latency differs from OBS | Same code path as OBS monitoring, should match; verify in M1. |
| Doubled audio when OBS and knOBS both monitor to VB-Cable | Auto-pause while `obs64.exe` runs. |
| Push-to-talk/mute on the imported source silences the mic | Clear on import with a warning; PTT support is a v2 idea. |
| OBS updates change scene JSON schema / filter IDs | `obs_load_source()` from the user's own OBS version; pre-flight validates and warns on unknown IDs. |
| Chain includes a VST filter | Not supported in v1: stripped with a loud warning. v2 feature. |
| NVIDIA noise suppression needs an external runtime | v1 promises Speex/RNNoise. In OBS 32 NVIDIA's filter lives in the separate `nv-filters` module, which v1 doesn't load, so a chain using it imports without it (warned). Loading `nv-filters` is a v2 idea. |
| Trademark ("OBS" in name) | OBS Forum Resource and IP Policy (Jan 2026) asks tools to avoid the OBS acronym in names. Ask the OBS team before launch; fallback: Knobs. |
| OBS forum rules on AI-assisted code | Policy requires an AI-use disclaimer and discourages listing resources mostly written by AI. Include the disclaimer; be ready to explain authorship. |
| GPL obligations | Fine: knOBS is GPL-2.0-or-later (same as libobs) and open source. |

## 7. Success criteria

1. Harness output is bit-identical run-to-run; the OBS comparison residual is below threshold; blind ABX can't tell OBS and knOBS apart.
2. OBS closed, knOBS in tray: mic sounds identical in Discord/Zoom/games.
3. Cold boot → working filtered mic with zero clicks.
4. Opening OBS while knOBS runs never produces doubled audio.
5. Idle resource usage meaningfully below OBS-minimized.

## 8. Future ideas (v2+)

- **VST 2.x filters** (deferred from v1). Known constraints: `obs-vst` links Qt Widgets and drives its plugin object through Qt, while knOBS has no QApplication. Needs Qt DLLs in the runtime copy, the "open interface when active" setting forced off (opening the editor would construct a QWidget with no QApplication, which aborts), and testing against plugins that expect a message pump on their thread. Give it its own milestone.
- NVIDIA noise suppression / Audio Effects as a supported feature by loading `nv-filters` (see §6)
- Push-to-talk / push-to-mute via global hotkeys
- Level meter / simple visualizer in a tray flyout
- Filter enable/disable toggles (no parameter editing — still tune in OBS)
- Auto re-import on scene collection file change (file watcher)
- Multiple sources (second mic / line input)
- macOS port (CoreAudio monitoring backend)
- "Profiles" — switch between chains (streaming voice vs. calls)

---

### Revision notes — Rev 3 (2026-09-30)

- M0 done against OBS 32.2.2. Added the M0 findings (§4): audio works without video, but activation doesn't, so win-wasapi's reconnect needs a decision in M3. Costs of both video modes are measured.
- Runtime copy set is derived from import tables. Supported range is 32.2.0 up to, but not including, 33.0.0. Recorded the loader's process-wide settings, the explicit destroy-queue drain before shutdown, and that a runtime refresh needs a restart.
- NVIDIA noise removal lives in `nv-filters` in OBS 32, not `obs-filters`; corrected §4, §6 and §8. Added compressor-sidechain and NVIDIA-filter warnings to M2 pre-flight.
- M3 gained an OBS folder picker item and the device-reconnect decision.
- After the M0 code review: the shutdown drain also waits on the audio and graphics threads; the runtime copy is checked against the install, flushed, locked and swapped by renaming; unsupported versions are refused before copying; a failed load restores the working directory; `ObsSession` must start and stop on one thread (COM).

### Revision notes — Rev 2 (2026-09-27)

- Renamed to **knOBS** (trademark check + fallback kept as an M4 item).
- Runtime now loads the user's installed OBS (shadow-copied) instead of building and bundling libobs. M0 is much smaller; packaging is just an exe.
- Import uses `obs_load_source()` and now covers source-level state (Mono, balance, volume, mute, sync), global audio devices (`AuxAudioDevice*`), and OBS 31+ `user.ini`.
- M1 gate: live null test replaced by a deterministic offline harness. The OBS comparison moved to M2 with alignment, a residual threshold, and blind ABX.
- Added OBS coexistence (auto-pause while OBS runs) and push-to-talk/mute handling.
- VST deferred to v2.
- Added AI-use disclaimer to M4 per the OBS forum policy.
