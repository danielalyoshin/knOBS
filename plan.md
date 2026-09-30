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

1. **Locates the OBS install** — installer registry key → `C:\Program Files\obs-studio` → manual path picker (Steam / portable installs).
2. **Copies the needed subset** into `%LocalAppData%\knOBS\runtime\<obs-version>\`, mirroring the install layout (`bin\64bit`, `obs-plugins\64bit`, `data\libobs`, `data\obs-plugins\<module>`) so relative data lookups still resolve. Loading from the copy keeps OBS's own DLLs unlocked, so OBS updates aren't blocked by a running knOBS.
3. **Loads `obs.dll` from the copy** (`AddDllDirectory` for its dependencies) and resolves the ~20–30 functions it needs via `GetProcAddress` into a function table. No import lib needed, and a missing export fails gracefully instead of at process load.
4. **Checks `obs_get_version()`** against a tested range. Outside it, refuse to start with a clear tray message.

When the installed OBS version changes, knOBS notices on startup and prompts a re-import, which refreshes the runtime copy.

**Modules loaded (only these):**
| Module | Provides |
|---|---|
| `win-wasapi` | `wasapi_input_capture` source |
| `obs-filters` | gate, expander, compressor, limiter, gain, EQ, noise suppression (Speex/RNNoise) |

`obs-vst` is not loaded in v1.

**Key libobs calls (happy path):**
1. `obs_startup("en-US", module_config_path, nullptr)` — `module_config_path` is knOBS's own `%AppData%\knOBS\module-config`, never OBS's. knOBS treats `%AppData%\obs-studio` as read-only.
2. `base_set_log_handler()` → knOBS log file (backs "open logs" in the tray).
3. `obs_reset_audio()` — sample rate + channels from the active profile's `basic.ini`.
4. Video: first try running with **no** `obs_reset_video()`. Only if something requires graphics, fall back to a minimal dummy config (8×8 @ 1 fps, never rendered) and measure its cost against G3.
5. `obs_add_module_path()` → runtime copy; open + init only `win-wasapi` and `obs-filters` (`obs_open_module` / `obs_init_module`) instead of loading everything.
6. `obs_load_source(source_data)` with the mic's saved source object from the scene collection. OBS's own loader recreates the `wasapi_input_capture` source, its filters in order, and source-level state (volume, balance, mute, Mono flag, sync offset).
7. Post-load fixups: `obs_source_set_monitoring_type(src, OBS_MONITORING_TYPE_MONITOR_ONLY)`; clear push-to-talk / push-to-mute (no hotkeys in v1, otherwise the mic stays muted).
8. `obs_set_audio_monitoring_device()` → virtual cable (from imported config or knOBS setting).

**Why `obs_load_source()`:** it's the same code path OBS uses to load the collection, and the scene JSON was written by the same OBS version whose loader reads it. That removes a hand-written settings replayer as a source of drift. Manual replay (`obs_source_create` + `obs_source_filter_add` + setters) is the fallback if it misbehaves.

### Config import

- **Active profile + scene collection:** `%AppData%\obs-studio\user.ini` (OBS 31+ split app/user settings out of `global.ini`), falling back to `global.ini` for OBS ≤ 30. `[Basic]` section.
- **Scene collection:** `%AppData%\obs-studio\basic\scenes\<SceneCollectionFile>.json`.
- **Candidate mic sources:** `wasapi_input_capture` objects in `sources[]` **and** global audio devices stored as top-level keys (`AuxAudioDevice1` … `AuxAudioDevice4`). If more than one, the user picks.
- **Profile settings:** `basic\profiles\<profile>\basic.ini` → `[Audio]` sample rate, channel setup, monitoring device.
- **Pre-flight checks** (before `obs_load_source`): strip `vst_filter` entries from the filters array (warn); flag unknown filter IDs (warn); detect push-to-talk / push-to-mute (clear + warn); show Mono on/off in the import summary.

Exact key names are verified against the installed OBS version in M2.

## 5. Milestones

**M0 — Runtime bootstrap**
- [ ] Locate OBS install (registry → default path → manual picker)
- [ ] Work out the exact file set to shadow-copy (`dumpbin /dependents` on `obs.dll` + the two modules); build the runtime copier
- [ ] Grab headers from the obs-studio tag matching the installed version; build the `GetProcAddress` function table + version-range check
- [ ] Smoke test: console app that loads `obs.dll` from the runtime copy, runs `obs_startup` → loads the two modules → `obs_shutdown` cleanly, with data files resolving
- [ ] Try audio-only init with no `obs_reset_video()`
- [ ] *(Optional, off critical path)* from-source libobs debug build for stepping through problems

**M1 — Audio pipeline + determinism harness**
- [ ] Hardcoded: create iD4 source → add one gain filter → monitor to VB-Cable
- [ ] Verify audio arrives in another app; measure added latency vs OBS
- [ ] Offline harness: custom source pushes a 48 kHz float WAV through the chain in fixed chunks with synthetic timestamps; capture via `obs_source_add_audio_capture_callback` → WAV
- [ ] **Go/no-go gate:** harness output is bit-identical across runs, and live audio reaches VB-Cable with latency comparable to OBS.

**M2 — Config import + OBS comparison**
- [ ] Resolve active profile + scene collection (`user.ini` / `global.ini`); verify key names against the installed version
- [ ] Enumerate candidate mic sources (`sources[]` + `AuxAudioDevice*` keys); picker if multiple
- [ ] Pre-flight checks (VST strip, unknown IDs, push-to-talk/mute) with warnings
- [ ] Load via `obs_load_source()`; apply post-load fixups
- [ ] Comparison vs OBS: same WAV (leading ~2 s of silence so envelopes settle identically) through an OBS Media Source with the chain pasted on and Mono/balance matched → record 32-bit float PCM. Run the same file through the harness with the imported chain. Align via cross-correlation; residual must fall below a set threshold (expect ~zero — investigate anything audible).
- [ ] Blind ABX on real voice: OBS vs knOBS

**M3 — Tray app**
- [ ] Win32 tray: start/stop, device + cable pickers, re-import, autostart toggle, log access
- [ ] OBS coexistence: watch for `obs64.exe`; auto-pause while it runs, resume when it exits (toggleable)
- [ ] Detect installed-OBS version change on startup → prompt re-import / runtime refresh
- [ ] Device disconnect/reconnect: audit win-wasapi's built-in reconnect logic first; knOBS surfaces state rather than reimplementing it
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
| Audio-only init has undocumented video dependencies | Try no video first; fall back to dummy config; measure its cost vs G3. |
| `data/` path resolution (`find_libobs_data_file`) | Mirror the install layout in the runtime copy; set module/data paths explicitly in M0. |
| Monitoring path latency differs from OBS | Same code path as OBS monitoring, should match; verify in M1. |
| Doubled audio when OBS and knOBS both monitor to VB-Cable | Auto-pause while `obs64.exe` runs. |
| Push-to-talk/mute on the imported source silences the mic | Clear on import with a warning; PTT support is a v2 idea. |
| OBS updates change scene JSON schema / filter IDs | `obs_load_source()` from the user's own OBS version; pre-flight validates and warns on unknown IDs. |
| Chain includes a VST filter | Not supported in v1: stripped with a loud warning. v2 feature. |
| NVIDIA noise suppression needs an external runtime | v1 promises Speex/RNNoise. The NVIDIA code is already in the user's `obs-filters.dll` and may just work if their runtime is installed — test, no promise. |
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
- NVIDIA noise suppression / Audio Effects as a supported feature (see §6)
- Push-to-talk / push-to-mute via global hotkeys
- Level meter / simple visualizer in a tray flyout
- Filter enable/disable toggles (no parameter editing — still tune in OBS)
- Auto re-import on scene collection file change (file watcher)
- Multiple sources (second mic / line input)
- macOS port (CoreAudio monitoring backend)
- "Profiles" — switch between chains (streaming voice vs. calls)

---

### Revision notes — Rev 2 (2026-09-27)

- Renamed to **knOBS** (trademark check + fallback kept as an M4 item).
- Runtime now loads the user's installed OBS (shadow-copied) instead of building and bundling libobs. M0 is much smaller; packaging is just an exe.
- Import uses `obs_load_source()` and now covers source-level state (Mono, balance, volume, mute, sync), global audio devices (`AuxAudioDevice*`), and OBS 31+ `user.ini`.
- M1 gate: live null test replaced by a deterministic offline harness. The OBS comparison moved to M2 with alignment, a residual threshold, and blind ABX.
- Added OBS coexistence (auto-pause while OBS runs) and push-to-talk/mute handling.
- VST deferred to v2.
- Added AI-use disclaimer to M4 per the OBS forum policy.
