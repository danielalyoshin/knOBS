# knobs — Project Plan

> **Your OBS mic chain, without OBS.** A lightweight Windows tray app that runs your exact OBS audio filter chain (gate, expander, compressor, noise suppression) against your microphone and outputs to a virtual audio cable — no OBS process required.

*Name: **knobs**, all lowercase, as in the audio kind. It was styled knOBS until 2026-10-05. The OBS Project's [Forum Resource and IP Policy](https://obsproject.com/forum/threads/forum-resource-and-ip-policy.178569/) (Jan 2026) asks third-party tools to avoid the "OBS" acronym in names, and its branding rules apply to any app, not just forum submissions. If the OBS team asks for a change, reconsider then.*

---

## 1. Problem

XLR mic users who tuned a filter chain in OBS (noise suppression → gate → expander → compressor → limiter) and route it through a virtual cable get consistent, processed audio in every app — Discord, Zoom, games, browser. The cost: OBS must stay open forever, even when not recording or streaming.

This got worse in OBS 32.0: the `--disable-shutdown-check` flag is ignored, so the Safe Mode popup appears on every boot and blocks unattended background use ([obs-studio#12650](https://github.com/obsproject/obs-studio/issues/12650)). The reporter's use case is exactly this one — OBS as a background mic processor feeding VB-Cable.

Existing alternatives (Equalizer APO, VoiceMeeter, NVIDIA Broadcast, NoiseTorch) use **different DSP implementations**. OBS's gate/compressor/expander are custom code inside `obs-filters`; nothing else reproduces the sound users spent hours tuning. The only way to get identical processing is to run OBS's own filter code — and the most faithful way to do that is to load the user's own installed OBS binaries.

**No existing project does this.** Nearest neighbors: obs-headless (Linux RTMP compositing), obs-express-node (recording control server), atkAudio / Audio Monitor (OBS plugins — still require OBS open), NoiseTorch (Linux, RNNoise only). General-purpose libobs hosts exist — [libobs-rs](https://github.com/djmango/libobs-rs) (Rust; its bootstrapper uses official OBS binaries at runtime) and [pylibobs](https://pypi.org/project/pylibobs/) (Python) — but neither targets mic-chain replay.

## 2. Goals

- **G1 — Same audio as OBS.** Same filter binaries (loaded from the user's own OBS install), same settings, same order, same source-level processing (Mono, balance, volume). This is the entire reason the project exists. Verified by a deterministic harness and a comparison against OBS itself, on test signals and real voice (§7).
- **G2 — Zero-config import.** Read the user's active OBS profile + scene collection and recreate their mic source, source state, and filter chain automatically using OBS's own loader. No re-tuning.
- **G3 — Invisible footprint.** Tray icon, no window by default, no GPU render loop, target < 1% CPU idle-adjacent usage and small memory footprint.
- **G4 — Set-and-forget.** Launch on startup, survive device disconnect/reconnect (interface power cycles), coexist with OBS (auto-pause while OBS runs), fail loudly (tray notification) rather than silently.

## 3. Non-goals (v1)

- No video, no recording, no streaming, no encoder.
- No filter *editing* UI. Users tune in OBS; knobs imports. (Editing is a possible v2.)
- **No VST filters.** Deferred to v2 (see §8). A chain containing a VST filter imports with that filter stripped and a loud warning.
- No macOS/Linux. Windows-only; audio monitoring backends are per-OS and Windows is the target user base. Architecture should not preclude ports.
- No custom DSP. If it's not an OBS filter module, it's out of scope.
- No building or bundling libobs for end users. knobs runs on the user's installed OBS; a from-source build is a dev/debug tool only.

## 4. Architecture

```
┌──────────────────────────────────────────────────────────────┐
│ knobs tray app (C++ / Win32)                                 │
│                                                              │
│  runtime loader ───► user's installed OBS, shadow-copied     │
│        │             to %LocalAppData%\knobs\runtime\        │
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

knobs does not ship libobs. At import time it:

1. **Locates the OBS install** — installer registry key (`HKLM\SOFTWARE\OBS Studio`, default value) → `C:\Program Files\obs-studio` → manual path picker (Steam / portable installs). The version comes from `obs.dll`'s version resource, so nothing is loaded from the install.
2. **Copies the needed subset** into `%LocalAppData%\knobs\runtime\<obs-version>\`, mirroring the install layout (`bin\64bit`, `obs-plugins\64bit`, `data\libobs`, `data\obs-plugins\<module>`) so relative data lookups still resolve. Loading from the copy keeps OBS's own DLLs unlocked, so OBS updates aren't blocked by a running knobs. The subset is computed from the PE import tables, not hardcoded: the import closure of `obs.dll`, `libobs-d3d11.dll` and the two modules, limited to DLLs the install ships, plus their PDBs, `data\libobs` and the two modules' data folders. For 32.2.2 that's 200 files, 53.6 MB, mostly FFmpeg: `obs.dll` imports avcodec, avformat, avutil, swscale and swresample directly, and those pull in libx264, librist, srt and zlib. The VC++ runtime and Windows DLLs come from the system. The copy happens in a staging folder with a manifest written last, so an interrupted copy is never used. Files are flushed to disk before the manifest. The manifest records each file's size and the install file's timestamp. The copy is reused only while the install still matches, because OBS drops beta and RC suffixes from `obs.dll`'s version resource, so the version alone can't tell a beta from the final release. Copies are serialized across processes with a named mutex. A replaced copy is renamed aside rather than deleted, so replacing fails as a whole while a running knobs uses it. The rename fails while a process has a file open in the copy or works in it, and libobs works in the copy's `bin\64bit` (step 3). A loaded DLL alone doesn't stop it (measured 2026-10-05), but Windows won't open a loaded DLL for writing. Making a copy and loading `obs.dll` from it hold the mutex together. Once libobs has started, knobs.exe prunes, on a thread of its own, what it no longer needs: copies of other OBS versions, copies set aside (`.old-<n>`) and interrupted staging folders (`.partial`). Each one's DLLs are checked, then it's renamed aside, and only then deleted. One in use, or anything while another process holds the mutex, is left for next time with a line in the log. Setting a folder aside is retried for about a second, as the copier does, since antivirus and the indexer hold files briefly. Pruning stops when knobs quits, between folders and between files: a copy is deleted only once renamed aside, so a stop leaves at most a part-deleted `.old-<n>` folder, which the next prune finishes. The mutex is `Local\knobs-runtime-copy`; the unit tests use one of their own. Only those names in `runtime\` are touched, and links aren't followed. The tools and tests don't prune; `knobs-core --prune-runtime` does, for trying it.
3. **Loads `obs.dll` from the copy** and resolves the functions it needs (31 in M0, 54 after M1, 72 after the M2 import, 74 in M3) via `GetProcAddress` into a function table (`KNOBS_OBS_API` in `src/runtime/obs_api.h`, typed from the vendored headers in `third_party/libobs`). No import lib needed, and a missing export fails gracefully instead of at process load. `obs.dll`'s own dependencies resolve from its folder and the system, never PATH or the install. A successful load changes two process-wide settings; a failed load undoes both. `AddDllDirectory(bin\64bit)` lets libobs find the graphics module by bare name. The working directory becomes the copy's `bin\64bit`, because libobs resolves `../../data/libobs/` against the working directory (`obs-windows.c`, `find_libobs_data_file`), so nothing may change it while libobs runs (the folder picker uses `FOS_NOCHANGEDIR`). Separately, the host process calls `SetDefaultDllDirectories` at startup, so plain `LoadLibrary` calls skip PATH and the working directory.
4. **Checks `obs_get_version()`** against the supported range. Outside it, refuse to start with a clear tray message. The range is 32.2.0 up to, but not including, 33.0.0. The floor is the minor version of the vendored headers (32.2.2); patch releases don't change the libobs API, so 32.2.0 and 32.2.1 have the same declarations. The ceiling is the next major, which is where libobs makes breaking API changes (`obs-config.h`). Only 32.2.2 has been tested. 33.0 is in beta and changes the monitoring API (OBS 33.0 notes). The install's version is checked before anything is copied, and again from `obs_get_version()` after loading.

When the installed OBS version changes while knobs runs, the re-import when OBS exits sees it, and knobs restarts by itself to load a runtime copy of the new version (Built: notifications). The refresh needs a process restart: libobs never unloads module DLLs (`os_dlclose` is commented out in `free_module`), and they keep `obs.dll` loaded too. Started after an update, knobs makes the new copy as it starts. The restarted knobs prunes the old version's copy once its own has loaded.

**Modules loaded (only these):**
| Module | Provides |
|---|---|
| `win-wasapi` | `wasapi_input_capture` source |
| `obs-filters` | gate, expander, compressor, limiter, gain, EQ, noise suppression (Speex/RNNoise) |

`obs-vst` is not loaded in v1. Neither is `nv-filters`. In OBS 32 the NVIDIA noise removal is a separate filter (`nvidia_audiofx_filter`) in that module; `obs-filters` doesn't build its NVAFX path.

**Key libobs calls (happy path):**
1. `obs_startup("en-US", module_config_path, nullptr)` — `module_config_path` is knobs's own `%AppData%\knobs\module-config`, never OBS's. knobs treats `%AppData%\obs-studio` as read-only. `obs_startup()` initializes COM as a single-threaded apartment on its calling thread, and `obs_shutdown()` uninitializes it, so both must run on the same thread, and not one already in a multithreaded apartment.
2. `base_set_log_handler()` → knobs log file in `%LocalAppData%\knobs\logs` (backs "open logs" in the tray). Set before `obs_startup` so startup is logged.
3. `obs_reset_audio()` — sample rate + channels from the active profile's `basic.ini`. Monitoring bypasses the audio thread, so the buffering settings don't affect what reaches the cable (M1 findings).
4. Video: **no** `obs_reset_video()`. Audio flows without it. Without the video tick, a mic never restarts by itself after losing its device, so knobs rebuilds it (M3 findings).
5. Open + init only `win-wasapi` and `obs-filters` (`obs_open_module(bin, data)` with absolute paths into the runtime copy, then `obs_init_module`), then `obs_post_load_modules()`. No `obs_add_module_path()`: that only feeds `obs_load_all_modules()`, which knobs doesn't use. As a side effect, OBS's safe-mode and disabled-module lists don't apply.
6. `obs_load_private_source(source_data)` with the mic's saved source object from the scene collection. It's the private form of `obs_load_source()`: the same loader, but the source stays out of the global source list and registers no hotkeys. OBS's own loader recreates the `wasapi_input_capture` source, its filters in order, and source-level state (volume, balance, mute, Mono flag, sync offset). knobs forces the saved monitoring off during the load so no device opens early (`LoadSourceJson` in `src/audio/live_chain.h`), under both the 32.2 key and the one 33.0 reads (OBS 33.0 notes). OBS itself loads a mic two ways. `obs_load_sources` loads each of a collection's `sources` and then calls `obs_source_load2` on it, which runs the `load` callbacks of the source and its filters. The frontend loads global audio devices (Mic/Aux) with `obs_load_source` alone (`LoadAudioDevice`). Import matches whichever applies to the chosen mic (`LoadOptions::load_callbacks`). None of the types in the two modules has a `load` callback in 32.2.2, so this is about staying aligned, not about today's audio.
7. Post-load fixups: `obs_source_set_monitoring_type(src, OBS_MONITORING_TYPE_MONITOR_ONLY)`, then `obs_source_inc_active(src)`, because the monitor only plays while the source is active. Mute, push-to-talk and push-to-mute need no fixup: the monitor ignores them, in OBS as in knobs (M1 findings). 33.0 deprecates `obs_source_set_monitoring_type` (OBS 33.0 notes).
8. `obs_set_audio_monitoring_device()` → virtual cable (from imported config or knobs setting).
9. Shutdown: release sources, drain, then `obs_shutdown()`. libobs destroys sources on a background queue, and `obs_shutdown()` only waits for it when a video thread exists (`obs_wait_for_destroy_queue`). The audio and graphics threads hold source references during each tick, so a released source can reach its last release on them. The drain therefore mirrors `obs_wait_for_destroy_queue`: a no-op task with `wait=true` on each running thread (graphics, then audio), then one on `OBS_TASK_DESTROY`. Skipping the audio round trip risks a source being destroyed after its module has unloaded.

### M0 findings (OBS 32.2.2)

Measured with `knobs-smoke` (`tools/smoke`), which runs the whole bootstrap and reports each step.

- **Audio works with no video.** Capture callbacks fire on the source's own thread after the filter chain (`obs_source_output_audio`), independent of the graphics thread. The smoke test captured the Audient iD4 through `gain_filter` with no `obs_reset_video()`.
- **The video tick drives more than video.** Only the graphics thread calls `obs_source_video_tick`, and it isn't exported. Without video:
  - Sources never get `activate`/`show` callbacks. win-wasapi creates its device-reconnect thread in `activate`, so a mic that disconnects, or is missing at startup, never recovers (G4). `obs_source_active()` only reports the activation ref count, so use the source's `activate` signal to see whether activation happened.
  - `video_tick` callbacks never run. The compressor looks up its sidechain source there. That only matters if a sidechain is set, and knobs doesn't load other sources anyway, so pre-flight loads such a compressor as it is while its sidechain is silent, with a warning (Config import).
- **Cost** (Release build, 10 s capturing the iD4 through a gain filter):

  | Video mode | CPU (one core) | Working set | Private | Threads |
  |---|---|---|---|---|
  | None | 0.16% | 22 MB | 17 MB | 10 |
  | Dummy (8×8 @ 1 fps, D3D11) | 0.3–0.6% | 64 MB | 86 MB | 79 |

  A full load → startup → modules → shutdown cycle takes about 55 ms, with 0 leaked libobs allocations and no libobs warnings (the dummy canvas adds the usual GPU-scheduling warning).
- **Decision:** M1 and M2 run with no video. M3 decides how to survive device loss (G4):
  - (a) Use the dummy canvas so win-wasapi's own reconnect runs. It costs about 70 MB of private memory, about 70 threads, and a live D3D11 device on adapter 0, which can wake a discrete GPU on hybrid laptops.
  - (b) Stay video-less and recreate the source with `obs_load_source()` when the device comes back.

  Recommendation: (b). It re-runs OBS's own loader, so fidelity is unaffected, and it keeps the G3 footprint. Decided in M3: (b) (M3 findings).

### M1 findings (OBS 32.2.2)

Measured with `knobs-harness` and `knobs-live` (`tools/`). The mic runs (2026-10-04) used the Audient iD4. Everything else ran without the mic.

- **The monitor is a capture callback, not part of the mix.** libobs's monitor (`audio-monitoring/win32/wasapi-output.c`) registers an audio capture callback on the source. It takes the post-filter audio, applies the source's volume, resamples it to the device's mix format, and writes it straight into a shared-mode WASAPI buffer (1 s, no event callback). It never passes through libobs's audio thread, and `MONITOR_ONLY` keeps the source out of the output mix entirely. So the audio buffering settings (OBS's "low latency audio buffering") don't affect what reaches the cable. The monitor plays only while the source's activation count is above zero. `obs_source_inc_active()` is enough, with no video.
- **Mute doesn't reach the monitor.** The callback ignores its `muted` argument, so mute, push-to-talk and push-to-mute don't gate what OBS sends to VB-Cable. knobs behaves the same by running the same code. This changes how import treats push-to-talk (§4 step 7, pre-flight). It's a deliberate, recent change ([e8a8ac5](https://github.com/obsproject/obs-studio/commit/e8a8ac5451), [PR #13378](https://github.com/obsproject/obs-studio/pull/13378)), so it depends on the version:
  - 32.0 and earlier: mute and push-to-talk both silence the monitor.
  - 32.1: the mixer's mute button on a monitored source switches it to Monitor Only instead of muting it, so the monitor keeps playing. Push-to-talk still silences the monitor.
  - 32.2.0 on: libobs ignores mute for the monitor, so mute stops only the stream and recording. The PR's goal was hearing a monitored source while it's muted. It doesn't mention push-to-talk, which changed along with mute. A bug report about this ([#13725](https://github.com/obsproject/obs-studio/issues/13725)) was closed as intended: turn monitoring off instead.

  The supported range starts at 32.2.0, so all of it behaves the same way. 33.0 keeps this behavior (OBS 33.0 notes).
- **The output latency is VB-Cable's.** The time from a packet's timestamp until VB-Cable's recording side has it:
  - 84–94 ms over five runs through libobs (push source → gain filter → monitor → CABLE Input).
  - 81–98 ms over four runs of a bare WASAPI stream set up like the monitor.

  Each run holds within ±0.2 ms. The spread between runs comes from how the cable's buffers line up at start. libobs adds nothing measurable, and OBS runs the same monitor code from the same `obs.dll`. The capture side (win-wasapi) is the same code in both too.
- **Mic to cable, knobs matches OBS.** `knobs-live --measure-mic` compares the mic with what arrives on CABLE Output. Both ran into the same cable input with the same mic, and OBS had its filters turned off:
  - knobs: 88.1 ms;
  - OBS: 87.7 ms.

  A knobs run into CABLE Input measured 96.9 ms. Runs into different cable inputs, or started at different times, vary about that much (see above), so only runs into the same input are compared. OBS here monitors to CABLE In 16ch, a second playback input of the same VB-Cable. So import has to use the saved monitoring device rather than assume CABLE Input. That was already the plan (§4 step 8).
- **Latency can step up after a hiccup and stays there.** In the knobs run above, the last quarter measured 98.7 ms, 10.6 ms above the rest of the run. That's about one audio period, so it looks like a hiccup that dropped or delayed about 10 ms of audio. Every other run held within ±0.2 ms. libobs's monitor only corrects its delay for sources with video (`process_audio_delay` in `wasapi-output.c`). For a mic, audio queued by a hiccup stays in the monitor's 1 s buffer, so the latency stays higher until the stream restarts. OBS runs the same code. Open questions for G4: how often this happens over hours, whether knobs hits it more often than OBS, and whether the iD4 and the cable drift apart. A 5-minute run without the mic (`--measure-output 300` into CABLE In 16ch) held at 102.0 ms in every quarter. So the monitor and cable alone didn't step there. The capture side, or the tool's own recording of the mic, is the likelier source. Since then (Long-run latency, in M3): the iD4's clock runs 31.5 ppm slower than VB-Cable's, so the monitor's stream runs dry every 5 min or so, which steps the delay up by a period, and drift takes it back down. On this PC a step doesn't stay.
- **Mono matters for this setup.** The OBS mic source has Mono on, and the mic is almost certainly only on input 1. So OBS's cable level sat a constant 6 dB under the mic, because Mono averages the two channels. knobs's M1 test chain has Mono off. Import carries Mono through OBS's loader (§4 step 6).
- **The harness is deterministic.** A push source (`obs_register_source_s`) feeds audio through `obs_source_output_audio`, which runs the filters and capture callbacks on the caller's thread before returning. So the harness needs no clock and no devices. The chain loads through `obs_load_private_source` with the source type swapped, the same loader import uses. The output was bit-identical in every case:
  - repeated runs in one process;
  - separate processes;
  - Debug and Release builds;
  - a mono input.

  The M1 chain (one gain filter at 0 dB) passes input through bit-exactly. A chain with every obs-filters audio filter runs at 29× real time.
- **Packet size matters only at rounding level.** Pushing 256-, 441- or 1024-frame packets instead of 480 changes the full chain's output by at most 2e-5. The residual is 121–130 dB below the signal, with no time shift, and about 98% of frames are identical. OBS's Media Source delivers different packet sizes from WASAPI, so the M2 comparison should expect a residual around −120 dB, not zero.
- **win-wasapi's "default" input is the default communications device** (`InitDevice` asks for `eCommunications`), not the default recording device. The M3 mic picker should say so.

### M2 findings (OBS 32.2.2)

Measured with `knobs-compare` (`tools/compare`) on 2026-10-05. It runs the same input through the imported chain twice: in knobs, offline, as `knobs-harness` does, and in OBS itself. The OBS side is a scripted run of a private copy of the install in portable mode (`%LocalAppData%\knobs\compare\obs-<version>`, about 125 MB without the browser source and debug symbols):
- a Media Source carrying the mic's filters and source settings, loaded where the mic was: as the Mic/Aux device, or in the scene for a mic from `sources`, so OBS takes the same load path for it as knobs (§4 step 6);
- Custom Output (FFmpeg) recording track 1 to a 32-bit float WAV;
- the Output Timer stopping the recording.

OBS starts minimized to the tray. No audio device is opened, and the user's OBS settings aren't touched.

- **knobs's output is bit-identical to OBS's** when both filter the same packets. That held on the built-in test signal and on synthetic speech (Windows TTS over a −60 dBFS noise floor), for two chains:
  - the real collection's mic: 3-band EQ, expander, compressor and limiter, with Mono on;
  - every obs-filters audio filter in one chain, RNNoise and Speex included.

  Seven OBS recordings, all bit-identical. Since the code review, "identical" means bit for bit, and runs since then also cover the full chain on a mic from `sources`, a chain that clips, and a 6.5 s clip.
- **Real voice too.** A 20.9 s recording of Daniel's mic, taken the way the Audient iD4 delivers it (voice on the left channel only, peaking at 0 dBFS), came out bit-identical through the real chain, over 26.8 s.
- **Packet size is the only difference.** OBS's Media Source plays a WAV in 4096-frame packets. FFmpeg's PCM demuxer aims for 10 packets a second and rounds down to a power of two. `knobs-compare` uses that size by default. With knobs in 480-frame packets (10 ms, WASAPI's), the full chain's residual is 120.4 dB below the signal (peak −97 dBFS), as M1 predicted. The real chain's peaks at −240 dBFS, in filter tails. Live, OBS and knobs both get win-wasapi's packets from the same code, so like-for-like is the comparison that matters.
- **OBS's recording mix changes a few values.** It adds each source into a zeroed buffer, so −0.0 becomes 0.0 (`obs-audio.c`, `mix_audio`), then turns NaN into 0 and clamps to ±1 (`audio-io.c`, `clamp_audio_output`). The custom FFmpeg output records that; OBS's PCM encoders take the mix unclamped. The monitor does neither, so the cable gets the filters' output as it is, from OBS and from knobs. `knobs-compare` does the same to knobs's side before comparing. With that, a +20 dB gain with no limiter (clipping) and the full chain (whose Invert Polarity turns silence into −0.0) came out bit-identical.
- **The end of the file isn't compared.** OBS's Media Source drops the file's last ~18 ms as it stops, and the mix fills in zeros where knobs's filters still ring at about −240 dBFS. The comparison stops one second before the end of the file, inside the trailing silence.
- **Threshold:** a run passes if its residual is at least 100 dB below the signal (RMS), 20 dB under the packet-size effect. Bit-identical runs are reported as such.
- **No blind ABX.** It can't tell identical renders apart: they can only score at chance. A live ABX would need OBS and knobs processing the same performance at the same time into two separate cables, and VB-Cable's inputs all feed one output. The comparison on real voice replaced it.

**Why `obs_load_source()`:** it's the same code path OBS uses to load the collection, and the scene JSON was written by the same OBS version whose loader reads it. That removes a hand-written settings replayer as a source of drift. Manual replay (`obs_source_create` + `obs_source_filter_add` + setters) is the fallback if it misbehaves.

### OBS 33.0 notes (checked against 33.0.0-beta6)

33.0 is in beta. knobs refuses to start on it (§4 step 4), so nothing breaks silently, but it needs support soon after release. What affects knobs:
- **Monitoring becomes on or off.** `obs_source_set_monitoring_enabled` replaces `obs_source_set_monitoring_type`. The old setter still works, but it's deprecated and logs a warning on every call. Monitor Only is gone: any monitoring mode turns monitoring on, and the source's type becomes Monitor and Output, so a monitored source also goes into the output mix. Keeping it off the stream now means muting it. The cable audio doesn't change, because the monitor takes the audio before the mix and nothing in knobs uses the mix. The mic does get mixed for nothing, which costs some CPU (not measured).
- **The load needed a fix.** Each source's JSON has a `prev_ver`. For sources saved by 33.0 or later, the loader reads a `monitoring_enabled` bool. It only derives that bool from `monitoring_type` for older sources. Overriding only `monitoring_type` would let a mic saved by 33 start monitoring to the current device during the load. Since M2, `LoadSourceJson` overrides both keys; 32.2 ignores the new one.
- **Mute and push-to-talk still don't reach the monitor.** New per-source hotkeys, `libobs.monitor-on` and `libobs.monitor-off`, turn monitoring on and off instead. They don't apply to knobs, because its private source registers no hotkeys.

Supporting 33.0 means:
- re-vendoring the headers at the release tag;
- raising the ceiling in `src/runtime/obs_version.h`;
- resolving `obs_source_set_monitoring_enabled` as an optional export (32.2 doesn't have it);
- ~~the load fix above~~ (done in M2).

### Config import

Checked against OBS 32.2.2's frontend (`OBSApp.cpp`, `OBSBasic_Profiles.cpp`, `OBSBasic_SceneCollections.cpp`). Built in `src/import/`; `knobs-import` runs it and reports each step.

- **Settings folder:** `%AppData%\obs-studio`. A portable install, one with a `portable_mode`, `obs_portable_mode`, `portable_mode.txt` or `obs_portable_mode.txt` file next to `bin\`, keeps its settings in `<install>\config\obs-studio` instead (`obs-main.cpp`). OBS's `--portable` flag also turns portable mode on, which knobs can't see from outside, so the M3 tray needs a way to pick the folder (`--obs-config` in the tools). A folder picked that way counts as portable if it's the `config` folder next to a marker. A picked folder that's OBS's own, however it's spelled, counts as OBS's own (`import::ObsConfigRootFor`). Only then do errors suggest opening OBS once, in the tools as in the tray.
- **Active profile and scene collection:** `[Basic] Profile` and `SceneCollection` in `user.ini` (OBS 31 and later). Without a `user.ini`, they're in `global.ini`, where OBS 30 and older kept them; OBS 31 and later copy `global.ini` to `user.ini` when they first start. `global.ini`'s `[Locations]` (`Configuration`, `SceneCollections`, `Profiles`) can move `user.ini`, the collections and the profiles elsewhere. A location that doesn't exist falls back to the settings folder, and portable mode ignores them.
- **Found by name, not by file.** OBS 32 matches `Profile` against the `[General] Name` in each profile folder's `basic.ini`, or the folder's name if there's none, and `SceneCollection` against the `"name"` saved in each `.json` file in the scenes folder, or the file's name. `ProfileDir` and `SceneCollectionFile` are written but not used for this. The first match in directory order wins, as in OBS.
- **INI files** are read by a parser that matches libobs's (`util/config-file.c`): the BOM is skipped, nothing is trimmed but a key's leading whitespace, `#` starts a comment, `\\`, `\n` and `\r` are unescaped, and a repeated section or key replaces the earlier one. knobs has its own parser because the profile has to be read before libobs starts: it sets libobs's audio format.
- **Scene collections** are parsed by libobs (`obs_data_create_from_json`), as OBS parses them, falling back to `<file>.bak` if the file doesn't parse. OBS falls back the same way through `obs_data_create_from_json_file_safe`, which also renames the backup over the broken file. knobs reads the files itself so that nothing is written.
- **Profile settings:** `basic.ini` `[Audio]`: `SampleRate` (default 48000), `ChannelSetup` (default `Stereo`, mapped to a speaker layout as `OBSBasic::ResetAudio` does), `MonitoringDeviceId` (default `default`) and `MonitoringDeviceName`. libobs runs at the profile's sample rate and layout, as in OBS. Numbers are read as libobs reads them (`strtoull`), so `48000 ` with a trailing space works. A profile left at `default` monitors to the default playback device, usually speakers: `knobs-live --import` won't monitor there without `--output`, and the tray needs to ask for the cable then (M3).
- **Candidate mics:** every `wasapi_input_capture` among the global audio devices (`DesktopAudioDevice1` and `2`, `AuxAudioDevice1` to `4`, in OBS's load order) and in `sources[]`. If there's more than one, the user picks: `--pick` by number or name in the tools, and a picker in M3.
- **Pre-flight**, on the mic's source object before it's loaded:
  - `vst_filter` entries are removed, with a warning (a note if the filter is off).
  - Filters libobs doesn't know, `nvidia_audiofx_filter` among them, stay in: libobs loads a placeholder for each that passes audio through. A warning says so, unless the filter is off. The chain knobs shows leaves them out, since they do nothing (`ImportedMic::filters`).
  - A compressor whose `sidechain_source` isn't empty or `none` becomes what it is while nothing plays on the sidechain, with a warning (decided 2026-10-05): OBS's Gain filter at the compressor's output gain, in its place, or nothing at the default 0 dB. With a sidechain, OBS's compressor follows the sidechain's level instead of the mic's; without one, the mic's (`compressor_filter_audio` in 32.2.2). Left as saved, knobs would compress the mic by its own level with settings tuned for ducking, which can squash the voice. While the sidechain is below the threshold, silence included, the compressor's gain is exactly 1 and it multiplies each sample by `db_to_mul((float)output_gain)`, which is what the Gain filter does with its `db`. `knobs-harness` confirmed it bit for bit: a compressor held at a gain of 1 against a Gain filter at the same dB, and the fixture's Podcast Mic as imported against the same mic with its compressor held there. It's the only filter in obs-filters 32.2.2 with a sidechain.
  - Filters that are off are noted, and stay off.
  - Notes for the source: muted, push-to-talk, push-to-mute or disabled (the monitor ignores all of them), a sync offset (the monitor ignores it for audio-only sources), and a mic OBS doesn't monitor.
  - Mono, balance and volume show in the chain summary libobs reports after loading. The tools also warn if the mic's device or the monitoring device isn't connected.
- **Load:** libobs serializes the pre-flighted object, and `obs_load_private_source` loads it, followed by `obs_source_load2` for a mic from `sources` (§4 step 6).

### Tray and first run (M3 design)

Shaped on 2026-10-05. The tray shell, the first run and the notifications are built (Built: the tray shell, Built: the first run, and Built: notifications, below), and so is the icon. The UI uses Windows' own controls. The brand shows only in the knob icon and the line saying knobs isn't affiliated with the OBS Project.

- **One status format.** Status is the mic's chain, named as OBS shows it, with only the filters that run: not those that are off, and not placeholders for filter types libobs doesn't have, such as NVIDIA's noise removal (pre-flight warns about those). `Mic/Aux › 3-Band EQ › Expander › Compressor › Limiter › CABLE In 16ch`. The menu and tooltip use the short form `Mic/Aux › 4 filters › CABLE In 16ch`. The first run and notifications use the same format.
- **First run:** one `TaskDialogIndirect` window that changes pages with `TDM_NAVIGATE_PAGE`. It stays light in dark mode, which is fine for a window seen once.
  - The first page offers two command links, "I set up my mic in OBS" and "I'm new to OBS". Each note says what knobs found ("Found Mic/Aux with 4 filters.", "Mic/Aux has no filters yet.", "OBS Studio isn't installed."), and the one that fits is the default.
  - The first door shows only the pages it needs. Which mic, when there are several (a `default` device reads "Default communications device (…)", M1 findings). Which cable, when the profile monitors to `default` or to a device that isn't a cable; with no cable installed, it links to VB-Cable and moves on by itself when one appears. Warnings, for pre-flight warnings and notes that change what to expect, such as push-to-talk.
  - The second door lists five steps in OBS: get OBS 32.2 if it's missing, open Filters on Mic/Aux, add filters as needed, adjust each by ear from OBS's defaults, and close OBS. It has an Open OBS button and links to the M4 setup guide. knobs suggests no filters and no settings. When OBS closes, knobs re-imports and continues with the first door's pages if the mic now has filters.
  - The last page shows the chain and says to choose the cable's recording side (CABLE Output) as the mic in other apps. It says where the tray icon is, since Windows 11 puts new icons under ^. A "Start with Windows" checkbox is checked by default.
  - Closing the window early leaves knobs in the tray, needing setup. An unfinished first run resumes where it stopped.
- **Tray menu:** a native menu (`TrackPopupMenuEx`), the same on left and right click. A status line, Pause/Resume, Mic ▸, Cable ▸, Re-import from OBS, Pause while OBS is open, Start with Windows, Setup…, Open log folder, About, Quit.
  - The status line can't be clicked. When something needs the user, the next item is the fix, in bold: "Finish setup…", "Choose a cable…", "Find OBS…".
  - Cable ▸ starts with "Same as OBS (…)" when the profile monitors to a cable. Any other pick is knobs's own setting and survives re-imports. Detected cables come first, and other playback devices go under "Other devices".
  - Dark menus come from uxtheme's `SetPreferredAppMode`, resolved by ordinal. Without it, the menu stays light.
- **Following OBS:** knobs re-imports when it starts and each time `obs64.exe` exits, and says so only when the mic or the chain changed (`Snapshot::chain_revision`). An OBS update to a supported version refreshes the runtime copy and restarts knobs. An unsupported one stops knobs, with a notification.
- **Tray icon:** `knobs.ico`, which is the knob without a tile, scaled up to fill the square. Decided 2026-10-05: no tile behind the logo anywhere in the app, and no separate glyph redrawn for each small size, since the bigger knob reads at 16 px. It has no rim on a dark taskbar either (decided 2026-10-05). Running shows the knob alone, paused adds a pause badge, and needing the user adds a "!" badge, both in Windows 11's style (Built: notifications). The knob itself is never recolored. The tooltip repeats the status line.
- **Notifications** are `Shell_NotifyIcon` balloons: what happened, then what knobs does or what to do. Clicking one opens the fix. They cover a mic missing for 5 s (so a power cycle stays quiet), a missing cable, a chain changed in OBS, a filter knobs can't run, and an unsupported OBS. Nothing shows on a cold boot, or when OBS opens or closes without changes. Built, with the cable missing for 5 s too, and the other problems that stop knobs (Built: notifications).
- **Open:**
  - ~~Pausing while OBS is open when OBS doesn't monitor the mic.~~ Decided 2026-10-05: knobs pauses while OBS is open whether or not OBS monitors the mic. A cable that goes silent while OBS is open is the known cost; turning off Pause while OBS is open keeps knobs running. Reading OBS's saved settings to decide would lag behind what OBS is doing.
  - ~~Cable recording sides.~~ Decided: CABLE Input and CABLE In 16ch go to CABLE Output, CABLE-A Input to CABLE-A Output, VoiceMeeter Input to VoiceMeeter Output (a VB-Audio device's " Input" or " In 16ch" becomes " Output", `CableRecordingSide`). For anything else, "the recording side of …".
  - ~~A re-import that changes the sample rate or channel layout may need `obs_reset_audio` or a restart.~~ It needs a restart, as in OBS (M3 findings).
  - ~~Check the OBS names in the second door's steps against 32.2.2's locale files.~~ Done (Built: the first run).
  - ~~Open OBS has to start `obs64.exe` with its own `bin\64bit` as the working directory.~~ Done (`OpenObs`).
  - ~~Notifications longer than Windows 11 shows.~~ Decided: it shows about 4 lines of text (about 160 characters) and clips the rest with no "…". With more than one new warning, the first line says how many there are and that a click shows them all, and each warning follows on a line of its own. One warning too long to show whole starts with "Click to see it in full." A chain too long to show whole is given in the short form, as the menu has it.
  - ~~Another user's OBS on the same cable.~~ A user's programs keep running when Windows switches to another account, and their sound can keep playing. knobs can't keep another account's audio out of the cable: it only adds its own stream, and the cable mixes every program's. So knobs says when OBS is open in another Windows account, as a notification and a line under the menu's status line (`Snapshot::other_obs`), and runs on. Not tested with two accounts, by decision (2026-10-05): the warning covers it either way.
  - ~~A first-run button that can be clicked twice while an error is up.~~ Fixed: the tray's errors are owned by the first run while it's open, which keeps its buttons from being clicked, and a click that comes while another's handler runs is ignored.

**Built: the tray shell** (2026-10-05, `src/tray`, `knobs.exe`). `knobs-tray` runs it on the real core over a made-up OBS and devices, for every state, and saves pictures of the menu.
- **The status line** says the state and, once there's a chain, the chain in its short form: `Mic/Aux › 4 filters › CABLE In 16ch` while running, `Paused: …`, `Paused while OBS is open: …`, `Mic missing: …`, `Cable missing: …`, `No cable chosen: Mic/Aux › 4 filters`, `OBS has 3 mics`, `No mic in OBS yet`, `Can't read OBS's settings`, `Can't find OBS Studio`, `This version of OBS isn't supported`, `knobs needs to restart`, `Stopped after an error`, `Starting…`. Device names lose their driver's name, as Windows' Sound settings shows them: `CABLE In 16ch (VB-Audio Virtual Cable)` is `CABLE In 16ch`. The tooltip is "knobs" over the status line. While OBS is open in another Windows account, a line under the status says what that means: "OBS in Alex's account may also send audio to CABLE In 16ch".
- **The fixes**, in bold after the status line: Find OBS… (OBS missing; the first run's Can't find OBS Studio page, which says how to get OBS and offers Choose folder…, and Look for OBS when a folder was picked before), Finish setup… (OBS's settings unreadable, or no mic), Choose a mic… (several mics, none picked), Choose a cable… (the profile monitors to the default device, or the cable is missing), Restart knobs, and Try again (failed; a re-import). An unsupported OBS has no fix in the menu; its page, which its notification and Setup… open, offers Choose folder…, and Look for OBS for a picked install. Finish setup…, Choose a mic… and Choose a cable… open the first run at the page the state calls for, and Setup… opens the first run (Built: the first run).
- **Mic ▸** starts with "Same as OBS (Mic/Aux)" when the collection has one mic. That's no pick, so knobs follows the mic if it's renamed or replaced in OBS. Then the collection's mics by name; a pick by name sticks. A pick always means the mic by that name, an exact match first, then one that differs only in case, even when the name is a number (`import::PickMicByName`). Only the tools' `--pick` reads a number as a position. A pick the collection no longer has stays in the list, marked "not in OBS".
- **Cable ▸** starts with "Same as OBS (…)" when the profile monitors to a cable, and also while it's the choice, so the check mark has somewhere to go. Cables are told apart by their names: VB-Audio's (VB-Cable, VoiceMeeter, Hi-Fi Cable) and Virtual Audio Cable. A pick that isn't connected stays in the list, marked "not connected", and so does OBS's device. "No virtual cable found" when there's none.
- **Dark menus follow Windows' mode**, the taskbar's, not the app mode: the menu opens from the taskbar. Under high contrast, Windows draws the menu. Task dialogs stay light.
- **Pause while OBS is open** is `core::Settings::pause_for_obs`. Off, the chain keeps running while OBS is open, and OBS exiting still re-imports.
- **Settings** are in `%AppData%\knobs\settings.ini`, written whole through a temporary file: `[OBS]` `Install`, `Settings` and `PauseWhileOpen`, `[Audio]` `Mic`, `Cable` (an endpoint ID) and `CableName`, and `[Setup]` for the first run's progress (Built: the first run). Escaped as libobs's INI parser reads them, and read with that parser. Pause isn't saved, so a cold boot runs. A picked install or settings folder that stops working stays picked: knobs shows its error and changes nothing by itself (decided 2026-10-05). The first run's page for it offers the way back: Look for OBS drops the picked install and looks where OBS's installer puts it, and Use OBS's own folder drops the picked settings folder. Choose folder… picks another, and Try again checks the same folder, for a drive plugged back in. These are on Can't find OBS Studio, on This version of OBS isn't supported when the install was picked, and on Can't read OBS's settings. The saved setting stays as picked, since OBS's own folder depends on the install, which can change. Opening OBS fills only the folder OBS keeps its settings in (`ObsCheck::own_config`), so a picked settings folder elsewhere gets neither the advice to open OBS once nor the Open OBS button. OBS's own folder picked, however it's spelled (`import::SameFolder`), is as good as no pick.
- **Start with Windows** is a `knobs` value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`: `"<exe>" --startup`. It counts as on only if it names this exe and Task Manager hasn't turned it off (`Explorer\StartupApproved\Run`, an odd first byte). Turning it on in knobs clears Task Manager's switch for it.
- **One copy per session.** The running copy owns the mutex `Local\knobs.tray`. Starting another opens the running copy's menu, or does nothing when Windows started it at sign-in. Restart knobs starts a copy with `--restart`, which waits up to 30 s for the old one to shut libobs down.
- **knobs.exe** is a Windows-subsystem exe with the static CRT and a manifest for per-monitor DPI awareness (v2) and Common Controls 6. It quits cleanly on `WM_CLOSE` and when Windows ends the session.

**Built: the first run** (2026-10-05, `src/tray/first_run.*` and `first_run_dialog.*`). `FirstRun` is the first run as data: the page for a snapshot of the core, what the page says and offers, and what each click or change of state does. The unit tests check every page for the states that call for it. `FirstRunDialog` shows the pages in one `TaskDialogIndirect` window and moves between them with `TDM_NAVIGATE_PAGE`, as the user clicks and as snapshots arrive.
- **The welcome page.** Each door's note says what knobs found, and the door that fits is the default: "Found Mic/Aux with 4 filters.", "Found 3 mics: Mic/Aux, Mic/Aux 2 and Podcast Mic.", "Mic/Aux has no filters yet.", "OBS has no mic yet.", "Can't read OBS's settings.", "Can't find OBS Studio…" with "OBS Studio isn't installed…" on the second door, "OBS 33.0.0 is installed, and knobs doesn't support it yet." (for an OBS too old for knobs, "OBS 31.1.4 is installed, which is too old for knobs."), or "Looking for OBS…" while the core starts. The footer says knobs isn't affiliated with the OBS Project.
- **The first door** shows first whatever blocks the rest, while the state calls for it, then each question only while it's needed or unanswered, then the last page:
  - Looking for OBS (a marquee; the first start makes the runtime copy). Can't find OBS Studio: choose its folder, try again, or get OBS. This version of OBS isn't supported: install a supported one from OBS's releases, or wait for knobs. Can't read OBS's settings: Open OBS, or choose a portable OBS's settings folder (picking `obs-studio` itself counts, `import::SettingsFolderFor`). A chain that stopped (Try again) or a restart (Restart knobs).
  - Which mic, when OBS has several, each with its device and its filters: "Mic/Aux: Microphone (Audient iD4), 4 filters". A `default` mic reads "Default communications device (…)". The default choice is the pick, else a mic with filters that OBS monitors, else one with filters. A pick by name, as in the menu; with one mic, none.
  - A mic with no filters: Show the steps, or Continue without filters. No mic at all: set Mic/Auxiliary Audio in OBS, then close it.
  - Which cable, when OBS monitors to its default device, to a device that isn't a cable, or to one that isn't connected: the connected cables by their short names, then the device OBS uses if it isn't a cable, to keep. Picking OBS's own device sets no cable of knobs's own, so knobs follows OBS. Other devices stay in the tray menu. With no virtual cable installed, the page links to VB-Cable and waits.
  - Warnings: pre-flight warnings, and the notes that change what to expect (`ImportNote::changes_expectations`, so far mute and push-to-talk).
  - The last page: the chain in full, which device other apps choose as their mic ("choose CABLE Output as your mic", `CableRecordingSide`), where the icon is (click ^, Show hidden icons), that knobs pauses while OBS is open, and Start with Windows: checked on a first run, and as it is when a finished one is opened again, such as from a notification, so that Done changes nothing the user didn't. Done turns Start with Windows on or off to match.
- **The second door** lists five steps in OBS 32.2.2's own words, checked against its frontend and the en-US locale files at the tag, which match the install's: "In the Audio Mixer, click Mic/Aux and choose Filters." (`Mixer`, `Basic.AuxDevice1`, `Filters`; in 32.2's mixer a source's name opens its menu, `VolumeControl.cpp`), and "Under Audio Filters, click + and add filters as needed." (an audio-only source's list is `Basic.Filters.AudioFilters`). It names no filters to add, decided 2026-10-05. Without a mic, step 2 starts "In Settings › Audio, set Mic/Auxiliary Audio to your mic" (`Basic.Settings.Audio`, `Basic.Settings.Audio.AuxDevice`). Step 1 is "Open OBS.", or links to obsproject.com when OBS is missing, or to OBS's releases when it's unsupported. Then "Click each filter and adjust it by ear, starting from OBS's defaults." and "Close OBS." Open OBS starts `obs64.exe` in its own `bin\64bit` (`OpenObs`), and is off while OBS runs. The footer links to the setup guide, `kSetupGuideUrl`: the README's Setup section, from M4.
- **Moving on by itself.** On the second door, OBS closing with filters on a mic moves on to the first door's pages, after the core's re-import. On the cable page, one cable appearing is chosen and the first run moves on; several are offered. Pages whose answer OBS can change say OBS is open and that knobs reads its settings again when it closes. The last page says when knobs is paused, by the user or for OBS, or waits for the mic.
- **Applying a choice waits for the core.** The page stays, with nothing to click, until a snapshot carries the new settings (`Snapshot::settings`, new). Snapshots also carry the recording devices, the default communications device and the OBS install, and each mic lists its filters (`MicCandidate::filters`).
- **Resuming.** `settings.ini`'s `[Setup]` has `Done`, and while the first run is unfinished, `Door` (`obs` or `new`) and `Page`: the first of the first door's questions not answered. An answered question shows again only if the state calls for it. knobs.exe opens an unfinished first run when it starts, unless Windows started it at sign-in, and when it's started again while running. Closing the window early leaves knobs in the tray. Setup… reopens it: where it stopped if unfinished, from the start if finished. Finish setup…, Choose a mic… and Choose a cable… open it at the page the state calls for. Back goes to the pages visited, as they are now.
- **Task dialog details.** The window keeps one width (320 dialog units) as pages change. Its check box is disabled unless `TaskDialogIndirect` is given somewhere to put its state, so it gets one, though the state comes from `TDN_VERIFICATION_CLICKED`. A page's strings stay alive until the next page is up. A button's handler can run a dialog of its own (the folder pickers), so snapshots that arrive meanwhile wait for it.
- **knobs-tray** opens any page on fake findings (`--first-run <page>`, with the scenarios, new ones included: `new-user`, `no-filters`, `no-cable`, `not-a-cable`, `warnings`), or on a real OBS settings folder imported by the installed OBS's libobs with made-up devices (`--obs-config tests\fixtures\obs-config`, `--pick`). Keys open and close OBS, plug the cables in and out, add filters, and click the dialog's buttons through its own messages; `--press` scripts them, so whole flows run unattended. Its Open OBS only says so: the real OBS would open the mic.

**Built: notifications** (2026-10-05, `src/tray/notices.*` and `badge.*`). `Notifier` is the notifications as data, like the menu and the first run: it follows the core's snapshots and the time, and says when to show a notification, what it says and what a click on it opens, when to take one down, and the icon's badge. The unit tests run it on the fake core: the real controller on knobs-tray's made-up OBS and devices, with a fake clock. `TrayApp` shows them as `Shell_NotifyIcon` balloons, which Windows 10 and 11 show as notifications.
- **What gets one.** Each says what happened, then what knobs does or what to do. Titles use the status line's words where there's one.
  - A mic missing for 5 s (`Notifier::Options::device_grace`), so a power cycle stays quiet: "Mic missing", "The recording device for Mic/Aux isn't connected. knobs starts again when it's back." A mic whose chain stalls (the core's watchdog) gets "Mic/Aux stopped sending audio. knobs keeps trying to start it again." once. It stays missing until its rebuilt chain has run for 5 s, longer than the watchdog takes to see that it stalled again. A mic whose device comes back is back at once. A click opens the menu.
  - A cable missing, also for 5 s (decided 2026-10-05): Windows' audio service restarting takes every device away for a moment, and the device knobs sends to may be on an audio interface that's power-cycling. "CABLE In 16ch isn't connected. knobs starts again when it's back, or click to choose another cable." A click opens the first run's cable page.
  - The mic or its chain changed in OBS (`Snapshot::chain_revision` went up, and knobs's settings didn't change): "Mic/Aux changed in OBS", "knobs now runs: " and the chain in full. A replaced mic: "OBS's mic is now Podcast Mic". Renaming the mic or a filter only changes the names shown (What reaches the cable, M3 findings). No sound; a click opens the menu. A change knobs's own settings made, such as another mic picked, gets none.
  - OBS opened in another Windows account (`Snapshot::other_obs`): "OBS in Alex's account may send audio to CABLE In 16ch", "Windows keeps it running while you use this account. If it monitors to that cable, apps here hear it with your mic. Close it there or turn monitoring off." It says what that OBS means for the mic, with "may": knobs can't read another account's settings. No sound and no badge, since knobs runs on and the cable may not be affected. It's taken down when that OBS closes, and said again for each new session. It's shown only when no other notification comes with it. When the title won't fit in 63 units, it says "your cable", and the text names the cable if it stays within about 160 characters; a long account name is cut with "…". Your own other session reads "your other session", with "this session" in the text. Before a cable is chosen, the title and the menu line say "your cable".
  - A filter knobs can't run, from new pre-flight warnings, told apart by their kind and the filter's uuid (`ImportNote::key`), not their text, so renaming the filter doesn't make its warning new: "Mic/Aux has a filter knobs can't run", then the warning. It says more than a chain change that comes with it. A click opens the first run's warnings page.
  - OBS updated: to a supported version, knobs restarts by itself (below), and the new copy says "OBS was updated to 32.2.3. knobs restarted to use it." To an unsupported one, knobs stops: "OBS was updated to 33.0.0", "knobs doesn't support it yet, so it stopped. Click for what you can do." A click opens the first run's unsupported page. Only an OBS newer than knobs supports is "not yet" supported (`runtime::IsNewerThanSupportedObs`): one too old reads "OBS 31.1.4 is too old for knobs", and a change to it is "OBS changed to 31.1.4", not "updated".
  - The other problems that stop knobs, as they begin (decided 2026-10-05: the cable goes silent): OBS's settings unreadable, no mic in OBS, several mics and none picked or the pick gone, OBS monitoring to its default device, OBS not found, and a chain that failed. Changes in OBS cause most of them, such as a second mic added. A click opens the first run at the page for each.
- **What doesn't.** A normal start, with OBS open or not, and OBS opening and closing without changing the mic. The first import is the baseline, so a chain or warnings found at start aren't news. Problems found at start do get one: a knobs started at sign-in would otherwise stop silently, and Windows 11 hides new tray icons under ^. Nothing shows while the first run is unfinished or open, since it shows the state itself, and what it showed isn't said again later. Nothing is missing while knobs is paused.
- **Taking them down.** A problem's notification is taken down when it ends (`NIF_INFO` with an empty `szInfo`), so a mic that comes back clears "Mic missing". News stays until the next notification replaces it.
- **Restarting by itself.** A restart the core needs (`Snapshot::restart`: an OBS update, or the profile's sample rate or channels changing) restarts knobs at once, with `--restarted-for obs-updated` or `audio-changed` on the new copy's command line, and the new copy says why. It waits while the menu or one of knobs's windows is open, and tries again as it closes: the menu and the first run check when they close, and any other window is checked every second. The first run's page offers Restart knobs meanwhile. If the new copy can't start, a dialog says why and to quit knobs and start it again, the menu keeps Restart knobs and the first run's page stays. A restart that failed, by itself or clicked, isn't tried by itself again. The format change restarts by itself too (decided 2026-10-05): OBS asks first because restarting OBS interrupts a stream, but restarting knobs costs a moment of silence.
- **The icon's badges**, since Do Not Disturb hides notifications. The tooltip and the menu say the state too. A pause badge while paused, by the user or while OBS is open, and a "!" badge while the cable gets no mic for a reason worth a notification: a mic or cable missing for 5 s, or a problem that stops knobs. They're discs in the logo's colors: a white "!" on the red (`#E5484D`) and white pause bars on the knob's graphite (`#4A4C52`), the same on a light and a dark taskbar (decided 2026-10-07, replacing Windows 11's caution yellow and neutral gray). `AddBadge` draws them at run time over the icon's bottom-right corner, 7/16 of its size (7 px at 16, 14 at 32), with a clear ring between badge and icon. The "!" and the pause bars share one whole-pixel stroke, so they stay crisp: 1 px up to 24 px, 2 at 32. The "!" is about 70% of the disc tall, and its stem stays longer than its dot at every size. Windows' own icon font has a "!" too (Segoe Fluent Icons' status glyphs), but drawn at a badge's sizes it blurs, so knobs draws its own. A problem's notification shows the knob with the "!" badge.
- **Balloon details.** Titles and texts are cut to fit `szInfoTitle` and `szInfo` with "…", counted in UTF-16 units as Windows holds them. Windows 11 shows only about 4 lines of the text, so several warnings start with a line that says how many and that a click shows them all, followed by the warnings that fit whole (only the first is ever cut, after a word), one warning too long to show starts with "Click to see it in full.", and a long chain is given in the short form (Tray and first run, Open). `NIIF_USER | NIIF_LARGE_ICON` shows the knob, not one of Windows' icons. Windows 11 draws it about 50 px wide at 100% and resamples any icon to fit, so knobs gives it the knob at twice 48 px (96 at 100%): measured on this PC, that comes out sharpest, where 32 and 48 px blur and 256 comes out jagged. News plays no sound (`NIIF_NOSOUND`). A click (`NIN_BALLOONUSERCLICK`) opens what the last notification offers, or the menu once that has been taken down. Windows names the sender by the exe's FileDescription, "knobs"; knobs-tray has the same, so its previews match.
- **knobs-tray** shows every notification on the fake core. Keys unplug the mics (m), stall the chain (s), change the first mic's chain (e) or add a VST filter to it (v) in OBS, update OBS (u: to 32.2.3, then 33.0.0), re-import (i), wait a second (.), and click the notification (a). `--press` scripts them with or without the first run. `--screenshot <png> --open notification` takes the notification from the screen's corner, `--restarted-for` starts as a restarted copy, and `--icons` saves a sheet of the icon with each badge at 16, 20, 24 and 32 px on both taskbars.

### The always-on core (M3)

Built on 2026-10-05 in `src/core/`. The tray (Built: the tray shell) shows its state and sends it commands. `knobs-core` runs it with no tray and prints each change of state.

- **States** (`core::State`): starting, running, paused (by the user, or while OBS is open), needs setup, mic missing, cable missing, OBS missing, OBS unsupported, restart needed and failed. Needs setup says what's missing: OBS's settings can't be read, the collection has no mic, it has several and none is picked, or the profile monitors to the default device and no cable is picked. Restart needed says why: OBS was updated, or the profile's audio changed (`Snapshot::restart`). It has no reason when libobs failed after a module loaded and trying again needs a fresh process (M3 findings). Each state comes with the chain in the status format (Tray and first run), the collection's mics, the pre-flight notes, and a chain revision that goes up when an import finds the mic or its chain changed.
- **Order.** After every event the controller works out the state from what it knows, and the first match wins:
  1. a problem with OBS, its settings or the import;
  2. a chain that failed to start;
  3. paused by the user;
  4. paused while OBS is open;
  5. the cable missing;
  6. the mic missing.

  So a missing mic doesn't show while paused, and needing setup shows even while OBS is open. The cable comes before the mic because a missing cable needs the user, while a mic usually comes back by itself.
- **The chain runs only while running.** Every other state releases it, which frees the mic and the cable and costs no CPU. The next time the state is running, libobs's loader builds the chain again (`obs_load_private_source`, §4 step 6). Pausing while OBS is open works the same way, so the cable never gets a stream from both.
- **Following OBS.** knobs pauses while OBS runs (M3 findings explain how it's seen). When OBS exits, knobs imports again: the install, the profile and the collection. A new OBS version needs a restart, which the tray does by itself (Built: notifications), and an unsupported one stops knobs. A re-import while the chain runs, such as Re-import from OBS in the tray, reloads it only if the mic, the chain or the cable changed (`ChainPlan::SameAs`). Chains are compared by `ImportedMic::chain_key`, which leaves out what doesn't reach the cable. A new sample rate or channel layout needs a restart, as in OBS.
- **Devices.** An `IMMNotificationClient` (`audio::DeviceWatch`) hears devices come and go and default devices change. Notifications come in bursts, so the devices are listed again once they've been quiet for 500 ms, or 2 s after the first. A mic or cable that goes away releases the chain (mic missing, cable missing), and the chain is rebuilt when it's back. A mic set to "default" is also rebuilt when the default communications device changes.
- **Watchdog.** Every second while the chain runs, counted from when the chain started rather than from the event that started it, the core checks that the chain's audio capture callback has seen packets. A mic sends them even when silent. After 3 s without, the chain has stalled: the state is mic missing, and the chain is rebuilt after 2 s, then after 5, 15, 30 and 60 s if it keeps stalling. 30 s of audio starts the waits over, and a device notification tries again at once. This covers what notifications don't, such as a device whose format changed in Windows.
- **Restarting the monitor.** While the chain runs, the core restarts libobs's monitor at most every 10 min, once the output has been silent for 3 s, so the delay to the cable can't creep up (Long-run latency).
- **One thread.** `core::Core` runs the controller on a thread of its own. That thread also runs libobs from start to shutdown (§4 step 1) and pumps messages, as a COM single-threaded apartment must. It checks for OBS every second. Device notifications and the tray's commands (pause, resume, re-import, new settings) are posted to it, and the observer (`core::Observer`) hears each change of state on it.
- **Settings** (`core::Settings`): the OBS install, OBS's settings folder, the mic, the cable and whether to pause while OBS is open. They override what knobs imports and survive re-imports. The tray saves them (Built: the tray shell). New settings re-import only when the install, OBS's settings folder or the mic changed; the cable and Pause while OBS is open only change what runs. Snapshots also carry what the tray offers: the connected playback and recording devices, the default communications device, the profile's monitoring device, the OBS install, whether the user paused, and the settings the snapshot was worked out with.
- **Testable without OBS.** The controller has no threads of its own and drives a `Backend` interface. It's told what happened and when, and reads the time itself only once a chain has started (`Timing::clock`), because getting there can take seconds (the first start makes the runtime copy). The unit tests run it, and the core's thread, on a fake. `ObsBackend` is the real one, on `ObsHost` and `LiveChain`. `knobs-core` runs dry by default: the chain loads through libobs's loader as a push source with no device and isn't monitored, so no audio device opens.
- **Not yet:** knobs's log opens when libobs starts, so what happens before that (OBS missing, settings that can't be read) reaches the observer but not the log.

### M3 findings (OBS 32.2.2)

Checked against win-wasapi, libobs and the frontend at 32.2.2, and measured with `knobs-core` on 2026-10-05 while the PC ran other programs. No audio device was opened. The core ran dry, and the portable OBS used to test the process watch (knobs-compare's copy) has no audio capture or monitoring; its log confirms that.

- **Without video, a mic source never restarts.** When the device fails, win-wasapi's capture paths (`CaptureThread`, or `OnStartCapture` and `OnSampleReady` with RTWQ) signal its reconnect thread, and only `activate` creates that thread (M0 findings). So nothing restarts the source when:
  - its device is missing when it loads;
  - its device goes away while it captures (`AUDCLNT_E_DEVICE_INVALIDATED`);
  - it's set to "default" and the default communications device changes. `SetDefaultDevice` asks for a restart through the same signal.
- **Silence still arrives.** win-wasapi passes buffers flagged `AUDCLNT_BUFFERFLAGS_SILENT` on as zeros, so a working mic always delivers packets. The watchdog relies on that.
- **The monitor and a missing cable.** If the cable goes away, libobs's monitor frees its stream and tries to open it again on every packet, 100 times a second, until the cable is back (`on_audio_playback`). If the cable is missing when monitoring starts, there's no monitor at all, and it never comes back (`audio_monitor_create`). So knobs starts the chain only when the cable is there, and releases it when the cable goes.
- **Decision: (b), rebuild the source.** The core releases the chain when the mic or the cable goes, and loads it again with `obs_load_private_source` once both are back. The watchdog catches what notifications miss. It re-runs OBS's own loader, so fidelity is unaffected, and it needs no video, so the footprint stays as measured in M0, without the dummy canvas's D3D11 device, 70 MB of private memory and 70 threads. Filters start fresh after a rebuild, as they do when OBS starts.
- **A new sample rate or channel layout needs a restart, in OBS too.** OBS asks for a restart when either changes in Settings (`AudioChangedRestart`) and when switching to a profile that differs in them (`GetRestartRequirements`). OBS never calls `obs_reset_audio` on a running libobs, so knobs restarts too.
- **What reaches the cable.** A disabled source counts as muted (`source_muted` in obs-source.c), and the monitor ignores mute (M1 findings). OBS saves nothing time-dependent in a source (`obs_save_source`), so an unchanged mic saves the same JSON. The chain key leaves out mute, push-to-talk and push-to-mute, `enabled`, `sync`, monitoring, hotkeys, mixers, deinterlacing, UUIDs (the filters' too), private settings, and the names of the mic and its filters, which libobs only logs (`obs_load_source_type`, obs.c). So muting or renaming the mic or a filter in OBS isn't a change: the names shown update, and the chain keeps running. Cables are compared ignoring case, as endpoint IDs are everywhere else. `knobs-import` prints the key's hash.
- **Finding OBS.** OBS creates a named mutex first thing, before its window or any module, and holds it until it has saved its settings (`CheckIfAlreadyRunning` in obs-main.cpp). It's `OBSStudioCore`, or for a portable OBS, `OBSStudioPortable` followed by its settings folder with every character that isn't a letter or digit replaced. `--multi` doesn't skip it. Both signals are knobs's session's own: the mutex name is in the session's namespace, and the process scan looks only at processes in knobs's session, since another user's OBS (fast user switching, RDP) can't be opened and would otherwise pause knobs for as long as it ran. A process in this session that can't be opened counts as OBS until a scan no longer finds it. The scan notes OBS in other sessions too, by session, with each account's name (`WTSQuerySessionInformation`) and whether it's this account signed in again (`ObsWatch::Others`), for the tray to say: it doesn't pause knobs. Session 0 is skipped. A scan that can't read the process list keeps the last result. The tests use names of their own (`ObsNames`, and a stand-in named `knobs-tests-obs.exe`), so ctest never looks like OBS to a running knobs or OBS. Costs, measured:

  | Check | Cost |
  |---|---|
  | Opening the mutex by name | 0.3 µs |
  | `NtQuerySystemInformation` process list | 1.2 ms |
  | `CreateToolhelp32Snapshot` | 1.9 ms, 3.4 ms read in full |

  Scanning the processes every second cost `knobs-core` 0.2–0.4% of a core. `ObsWatch` checks the mutex every second, and scans the process list every 2 s and when the mutex appears. It watches the OBS processes it finds through their handles. Its work for one minute takes 0.07% of a core. The lean portable copy loads its audio sources 1.6 s after starting, so an installed OBS is seen before it can monitor anything. A portable OBS is seen within 2 s: its mutex name depends on its working directory, so knobs doesn't try to rebuild the name. Measured on the portable copy: seen 1.0 s after it started, and its exit 0.7 s after it ended.
- **Trying libobs again in the same process.** A start that fails before any module DLL loads can be tried again: obs.dll unloads (measured: start, reset audio, shut down and free, then a fresh start works with 0 leaks). Once a module has loaded, it and obs.dll stay loaded (measured; `free_module` never unloads, obs-module.c), so the failure is "stopped after an error", and trying again restarts knobs (`Backend::LibobsCanRetry`). Reporting a restart at once would loop on a failure that repeats, since the tray restarts by itself.
- **Cost of the core**, Release, dry, 60 s runs: 0.10–0.18% of a core without watching for OBS, 17–18 MB working set. Runs with the watch varied more than the watch costs (0.26–0.39%) while other programs ran, so the watch was measured on its own (above). libobs starts and imports in about 30 ms once the runtime copy exists.

### M3 test on real hardware (OBS 32.2.2)

Run on 2026-10-06 with `knobs.exe` (Release) on Daniel's PC: the Audient iD4, VB-Cable, Discord, and OBS 32.2.2 monitoring Mic/Aux to CABLE In 16ch, the cable knobs sends to. Daniel clicked, spoke and listened. Two watchers ran alongside knobs: one listed the audio sessions on the VB-Cable endpoints every 50 ms (`IAudioSessionManager2`: each session's process, state and peak), and one checked for OBS's mutex every 2 ms.

- **Setup.** With no settings, knobs.exe opened the first run. The first door went straight to the last page, and Done wrote the Run entry, `"…\build\x64\Release\knobs.exe" --startup`. The chain loaded 40 ms after knobs started, and its stream on the cable was active 54 ms later.
- **The daily path (§7, 2).** The voice reached CABLE Output at −13 to −18 dBFS peaks, and −64 to −80 dBFS between words, where the expander works. Discord read the level knobs sent within 0.1 dB, and its mic test sounded right.
- **Same sound.** Discord's mic test with knobs, then with OBS open (knobs paused, OBS feeding the cable), then knobs again: the same by ear, as M2's bit-identical output predicts.
- **No doubling (§7, 4).** OBS opened and closed six times, once closed as soon as its window appeared. No two processes were ever active on the cable at once, and no echo was heard. knobs let go of the cable before OBS opened its monitor (`[Loaded global audio device]: 'Mic/Aux'` in OBS's log):

  | Open | knobs saw OBS (after its first log line) | knobs off the cable | OBS on the cable | Gap |
  |---|---|---|---|---|
  | 1 (cold) | 0.89 s | 01:00:21.898 | 01:00:23.350 | 1.45 s |
  | 2 | 0.02 s | 01:03:04.066 | 01:03:05.506 | 1.44 s |
  | 3 | 0.60 s | 01:03:34.074 | 01:03:34.894 | 0.82 s |
  | 4 | 0.69 s | 01:03:51.084 | 01:03:51.844 | 0.76 s |
  | 5 (quick) | 0.35 s | 01:04:10.113 | 01:04:11.242 | 1.13 s |
  | 6 | 0.09 s | 01:10:10.475 | 01:10:11.861 | 1.39 s |

  Pausing takes under 60 ms from seeing OBS to the cable stream stopping. On exit, knobs came back 0.2–0.9 s after `obs64.exe` ended, never before. In the exit timed against the mutex, OBS dropped its mutex 0.31 s before its process ended, and knobs waited for the process.
- **The margin is thinner than M3 assumed.** OBS created its mutex 0.16 s after its process started, within 2 ms of its first log line. From there, a warm OBS on this PC opened its monitor 1.44–1.48 s later in five runs, and 2.37 s on the cold start. knobs checks for the mutex once a second, so in the worst case it lets go of the cable about 0.4 s before OBS starts sending. An OBS that loaded its audio less than a second after its mutex would get the cable while knobs still had it, for up to a second. (On a cold start, this OBS spent about 0.6 s initializing nv-filters and 0.6 s initializing obs-qsv11.) Checking the mutex every 100 ms would leave about 1.3 s here. Decided 2026-10-06: keep the 1 s check. This is a fast PC (Core Ultra 7 265K) running OBS with no third-party plugins, so few setups should load their audio sooner.
- **Losing the mic.** Unplugging the iD4: win-wasapi saw its stream invalidated at 01:06:36.877. knobs listed the devices 0.51 s later (its 500 ms settle), was "mic missing" at once, and its cable stream stopped 13 ms after that. The notification came about 5 s after the unplug, and the "!" badge showed. Plugged back after 43 s: knobs loaded the chain 10 ms after it listed the devices, and the voice was on the cable at its first poll (−15 dBFS). How long Windows took to bring the iD4 back isn't logged; knobs's device settle adds at most 2 s. Not seen: whether the notification is taken down when the mic comes back, because it was closed by hand. `notices_tests` covers that.
- **Idle cost (§7, 5).** Each over 60 s, with nothing clicked. CPU is the change in processor time, which Windows samples in 15.6 ms ticks. The cycle counts (`QueryProcessCycleTime`), which are exact, were taken over 30–60 s:

  | | knobs running (OBS closed) | knobs paused (OBS open) | OBS minimized |
  |---|---|---|---|
  | CPU, one core | 0.72% | 0.44% | 5.64% |
  | CPU by cycles | 1.02% | 0.39% | 7.52% |
  | Working set | 36.2 MB | 30.7 MB | 468.9 MB |
  | Private | 21.0 MB | 17.1 MB | 801.5 MB |
  | Threads | 7–9 | 6–8 | 90–98 |
  | Handles | 687 | 392 | 2205 |
  | GPU | none | none | 0.05% |

  OBS here is `obs64.exe` alone; it started no other processes. Running, knobs's cycles by thread: win-wasapi's capture thread, which runs the four filters and the monitor, 0.62%; the core's thread 0.20%; libobs's audio thread 0.12%; its hotkey thread 0.08%; the tray's thread nothing. The core's thread spends more than the OBS watch's 0.07% alone (M3 findings). What else it spends on wasn't measured.
- **Cold boot (§7, 3).** After a restart, with nothing clicked: Windows booted at 01:22:08.5, and Daniel logged on at 01:22:17.881 (Winlogon event 7001). Explorer started `knobs.exe --startup` from the Run entry 9.26 s after logon. knobs loaded the chain and was running 9.48 s after logon, 0.22 s after it started, before Discord, Steam or Spotify had started. No window or notification appeared. Its stream was active on the cable, with the mic's floor at −75 dBFS, and Discord's mic test sounded right.

### Long-run latency (OBS 32.2.2)

Checked against libobs's monitor (`audio-monitoring/win32/wasapi-output.c`), `obs-source.c` and win-wasapi at 32.2.2, and measured on 2026-10-06 without the mic. `knobs-live --measure-drift` timed the devices' clocks. `--measure-restart` pushed a tone and clicks through the chain into CABLE In 16ch, restarting the monitor on a schedule, and recorded CABLE Output; `--push-ppm` ran the push off real time, as a mic whose clock is fast or slow.

- **What the monitor does with a mic.** For each packet, `on_audio_playback` resamples it to the device's mix format at a fixed ratio, asks the stream for exactly that many frames (`GetBuffer`), copies them in and releases them. It reads how much is queued (`GetCurrentPadding`), but only `process_audio_delay` uses that, and that runs only for sources with video. So for a mic:
  - **A backlog stays.** What's queued plays in order, so the delay to the cable stays wherever a hiccup or drift put it. Nothing in the monitor lowers it.
  - **A full buffer restarts the stream.** The stream asks for a 1 s buffer. When a packet doesn't fit, `GetBuffer` fails, and the monitor frees the stream (`audio_monitor_free_for_reconnect`), dropping everything queued; the next packet opens a fresh one. So the delay can't grow past about 1 s plus the cable's own, and getting there costs up to a second of audio.
  - **An empty buffer plays silence.** That's Windows' audio engine, not the monitor: when a period comes and the stream has too little, the engine plays silence for the rest, and whatever is written later plays that much later. The delay rises by the silence.
- **Nothing corrects drift for an audio-only source.** win-wasapi passes each packet on as it arrives (`ProcessCaptureData`). libobs's resamplers have fixed ratios. Timestamps are smoothed only on the way to the output mix (`source_output_audio_data`), which Monitor Only skips, and the capture callbacks, the monitor's included, get each packet whole. `process_audio_delay`, the monitor's one correction, needs video. So the monitor's queue follows the difference between the mic's clock and the cable's, in OBS as in knobs.
- **The clocks** (`--measure-drift 600` on both at once, playing silence, 48 kHz): a shared-mode stream's `IAudioClock` position against QueryPerformanceCounter.

  | Device | Against QPC | Per hour | Per quarter |
  |---|---|---|---|
  | Analogue 1/2 (Audient iD4) | −31.6 ppm | −114 ms | −31.1 to −32.2 ppm |
  | CABLE In 16ch (VB-Cable) | −0.1 ppm | −0.5 ms | −1.2 to 0.0 ppm |

  VB-Cable runs on the system clock. A second run timed the iD4's mic as well (`--measure-drift 600 --mic`, which opens it and drops what it records unread), next to its playback side and the cable. The mic measured −31.6 to −31.8 ppm in the last three quarters, against −31.1 to −32.0 for the playback side over the same minutes: one clock. The mic's first quarter was thrown off by a few bad readings at the start of the recording stream, not diagnosed; the tool now warns when quarters disagree. So the mic delivers 31.5 ppm less audio than the cable plays: 113 ms an hour.
- **On this PC the delay doesn't creep: the stream runs dry.** A mic slower than its cable drains the monitor's queue instead of filling it, and a hiccup's step drains with it, 10 ms in 5.3 min. Once the queue is empty, the engine plays a period of silence every 5.3 min or so (10 ms ÷ 31.5 ppm), and the delay steps back up by that much. With the push source 1000 ppm slow, a 9 ms gap came every 10 s, as predicted, with restarts or without. At the iD4's own drift (the push source 31.6 ppm slow, for 12 min), gaps of 8–10 ms came at 1.7, 318.7 and 635.1 s, 317 s apart, and the delay fell by about 5 ms from the first half of each 6-minute stretch to the second. OBS runs the same code, so OBS's cable has the same gap. Within speech, it's a 9 ms dropout. M1's 10.6 ms step was most likely one of these: the delay steps up a period, and drift takes it back down over the next 5 min.
- **A mic faster than its cable fills the queue.** With the push source 1000 ppm fast, the delay rose about 1 ms a second, and each restart took it back down to where it started. At 31.5 ppm, as fast as the iD4 is slow here, it would rise 113 ms an hour and overflow the 1 s buffer after about 9 hours. Crystal clocks are commonly rated to ±20–50 ppm, so another interface may as well be fast against its cable.
- **Not every step is the monitor's.** In one of three runs at real time, a 9 ms gap unrelated to any restart raised the delay by 10 ms, and the ten restarts after it left it there (98 ms, against 88 before). That step was downstream of the monitor, in Windows' audio engine or in VB-Cable, and restarting the monitor can't undo it. The other runs held within 0.2 ms.
- **A restart** (`obs_reset_audio_monitoring`, which is how OBS moves its monitors to another device) opens a new stream on the same device, then stops the old one, dropping what it had queued. Packets that come in between are dropped. libobs took 4.2–6.2 ms. On the cable, a restart left a gap of 0–10 ms, about one packet, and twice 20–21 ms: median 9 ms over 33 restarts in five runs. Afterwards the delay is wherever the new stream's start lands, as at any start: 85.8 to 94.3 ms at the start of today's three runs at real time (M1: 84–94 ms).

**Decision: the core restarts the monitor in silence** (2026-10-06, `Controller::FollowLevel`):
- **When.** At most every 10 min after the chain started or the last restart, once the output has been silent for 3 s. The 10 min come from the drift: a mic 31.5 ppm faster than its cable adds 19 ms in 10 min, two engine periods. Where a restart lands varies by about one period (above), so restarting much more often buys little, and much less often lets the delay wander well beyond that. On this PC drift doesn't call for it, but a fast mic would, and a restart also clears a hiccup's step from the monitor's queue.
- **Silent** is measured by the capture callback that already counts packets for the watchdog. It only reads the samples: the peak of each 10 ms packet, scaled by the source's volume, which the monitor applies after the callbacks. Each second is judged by its typical level, what its packets stay at or below once its loudest tenth (100 ms) is left out, so a click, a key or a short breath doesn't spoil the stretch (decided 2026-10-06). A second is silent at or below −50 dBFS. On the hardware test the expander left −64 to −80 dBFS between words, read every 50 ms. The last 200 ms before a restart must be silent too, packet by packet, so a restart never cuts into a word that's just starting; a sound there doesn't spoil the silence before it, and the next check may restart.
- **A mic that's never silent** (no gate or expander) gets as quiet as its noise floor between words. So silent also means within 6 dB of the quietest second in the last 5 min, but the bar is never above −30 dBFS: a floor at −33 dBFS still gets restarts in its pauses, at or below −30. A mic that never gets down to −30 dBFS isn't restarted: a gap in that much noise would be heard. Its delay grows if its clock is fast, and libobs's own restart at a full buffer is the backstop. A pause shorter than 3 s, or a voice more than 6 dB over the floor, doesn't count.
- **Not** while the chain gets no packets, which is the watchdog's business. A rebuilt chain has a new monitor, so the 10 min start over.
- **Each restart is logged:** how long since the last one, how long into silence, and the threshold.
- **Tools.** `knobs-core --restart-monitor <s>` shortens the 10 min for trying the policy. `knobs-live --measure-drift` and `--measure-restart` are the measurements above.
- **What restarts don't fix.** The dropout every 5 min from a mic slower than its cable, and steps downstream of the monitor. Both happen in OBS too. Only following the cable's clock would remove the first, which OBS doesn't do (§8).

**What's left is optional.** Hours of the mic into the cable beside OBS would confirm on real hardware what the code and these measurements say: no creep on this PC, a dropout every 5 min or so in both, and restarts only in silence. It isn't needed to ship.

### Packaging (M4)

Built on 2026-10-07 in `packaging/`. `packaging/package.ps1` takes the Release `knobs.exe` and writes both packages to `build\package`, with a `SHA256SUMS.txt`. The name comes from `src/app_info.h` and the version from `CMakeLists.txt`, and the exe's own version has to match. Each package has `knobs.exe`, `LICENSE.txt` (the GPL) and `NOTICE.txt` (copyright, where this version's source is, the trademarks and the logo's carve-out, and that knobs isn't affiliated with the OBS Project). No libobs and no OBS files.
- **The installer** (`knobs-<version>-setup.exe`, Inno Setup 6, `packaging/knobs.iss`) installs for the current user only, with no admin rights, in `%LocalAppData%\Programs\knobs`, as knobs's Run entry and data are per user. It adds a Start menu shortcut and offers to start knobs at the end. A running knobs is asked to quit first (`WM_CLOSE` to its `knobs.tray` window), on install and on uninstall. Uninstalling removes knobs's Run entry if it starts this install's exe, not a portable copy's, and `%LocalAppData%\knobs` (the copy of OBS's files and the logs), then asks whether to delete `%AppData%\knobs` too: the settings (decided 2026-10-07). A silent uninstall keeps them. Tested with a copy named `knobs-test`, so as not to touch this PC's knobs folders.
- **The portable zip** (`knobs-<version>-portable.zip`) unzips to a `knobs` folder with the same files and `portable_mode.txt`. Beside the exe, that file keeps all of knobs's state in a `data` folder there and nothing in `%AppData%` or `%LocalAppData%` (`AppDirsFor`, decided 2026-10-07). OBS's settings are still read from `%AppData%\obs-studio`. Start with Windows still writes the Run entry, pointing at that folder. The zip's entries are named by hand, since Windows PowerShell's zip writers use backslashes.
- **Signed in Release** (Code signing, below). CI's packages are unsigned.

### Code signing (M4)

Decided 2026-10-07: Azure Artifact Signing, which signs with the developer's own verified name and is open to individuals in Canada and the US ($9.99/month, Basic). The alternatives: Certum's Open Source certificate (from €49/year; its cloud key needs a phone login for each session, so signing would be by hand), SignPath Foundation (free, but its name would be the publisher, it needs a release first, and it accepts only open-source components, which the logo isn't), and the Microsoft Store as MSIX (no warning at all, but Start with Windows would need MSIX's startup task, since MSIX virtualizes the Run key). Since 2024 no certificate skips SmartScreen: EV and OV certificates both earn reputation by downloads. A signature keeps that reputation across releases, where an unsigned build starts over each time.
- **What's signed:** `knobs.exe` before it's packaged, so the installed and the portable copy are signed, then the uninstaller and the installer, which Inno Setup signs as it builds (`SignTool`, `SignedUninstaller`). `packaging/sign.ps1` does each: SignTool with Microsoft's Artifact Signing plugin (`Microsoft.ArtifactSigning.Client`, its x64 `Azure.CodeSigning.Dlib.dll`), SHA-256, timestamped by `http://timestamp.acs.microsoft.com` (the certificates last three days, so the timestamp is what keeps a signature valid), and verified after. It signs as the Azure CLI's sign-in. `package.ps1 -Sign` calls it; `-SignScript` takes another signer with the same parameters. Tested end to end with a throwaway self-signed certificate standing in for Azure.
- **Where:** only the Release workflow's package job, in the `release` GitHub environment, on the exe that CI built and tested. It signs in to Azure with GitHub's OIDC token, so no secret key is stored. Without the environment's signing variables, the release is unsigned with a warning, and its notes say so; with them, a failure to sign fails the release.
- **Set up once** (the identity check takes 1 to 20 business days):
  1. An Azure pay-as-you-go subscription. Free and trial subscriptions can't use Artifact Signing.
  2. In the subscription's Resource providers, register `Microsoft.CodeSigning`.
  3. Create an Artifact Signing account (Basic) in East US, the nearest supported region: its endpoint is `https://eus.codesigning.azure.net`.
  4. On the account's Access control (IAM), give yourself the Artifact Signing Identity Verifier role.
  5. On the account's Identity validations, add a Public, Individual validation with your legal name and address as on your ID, and finish the identity check it sends.
  6. Once it's Completed, create a Public Trust certificate profile on that validation.
  7. In Microsoft Entra ID, register an app for the release workflow. Note its Application (client) ID and the Directory (tenant) ID.
  8. On the app's Certificates & secrets, add a federated credential for GitHub Actions: organization `danielalyoshin`, repository `knobs`, entity Environment, name `release`.
  9. On the certificate profile's Access control (IAM), give the app the Artifact Signing Certificate Profile Signer role.
  10. On GitHub, in the repository's Settings › Environments, create `release`. Add the variables `ARTIFACT_SIGNING_ENDPOINT`, `ARTIFACT_SIGNING_ACCOUNT` and `ARTIFACT_SIGNING_PROFILE`, and the secrets `AZURE_TENANT_ID` and `AZURE_CLIENT_ID`. A required reviewer on the environment makes each release wait for your approval.
- **Then** the README's and the release notes' line about the missing signature changes (the notes change by themselves).

## 5. Milestones

**M0 — Runtime bootstrap** (done 2026-09-30 against OBS 32.2.2; see M0 findings in §4)
- [x] Locate OBS install (registry → default path → manual picker). The picker is `PickObsInstallFolder()`; the smoke test exposes it as `--pick-obs-dir`, and the tray wires it up in M3.
- [x] Work out the exact file set to shadow-copy (`dumpbin /dependents` on `obs.dll` + the two modules); build the runtime copier. The copier derives the set from the import tables itself (§4 step 2).
- [x] Grab headers from the obs-studio tag matching the installed version; build the `GetProcAddress` function table + version-range check. Headers are vendored by `tools/vendor-libobs-headers.ps1`.
- [x] Smoke test: console app that loads `obs.dll` from the runtime copy, runs `obs_startup` → loads the two modules → `obs_shutdown` cleanly, with data files resolving (`knobs-smoke`, also run by CTest)
- [x] Try audio-only init with no `obs_reset_video()`. Audio works, but activation doesn't (M0 findings).
- [ ] *(Optional, off critical path)* from-source libobs debug build for stepping through problems. Not needed yet; OBS's PDBs are copied with the runtime.

**M1 — Audio pipeline + determinism harness** (done 2026-10-04; see M1 findings in §4)
- [x] Hardcoded: create iD4 source → add one gain filter → monitor to VB-Cable. Built: `LiveChain`, run by `knobs-live --run`. Ran with the iD4 on 2026-10-04.
- [x] Verify audio arrives in another app; measure added latency vs OBS. The iD4's audio arrives on CABLE Output. Mic to cable on the same cable input: knobs 88.1 ms, OBS 87.7 ms. Without the mic, libobs adds no measurable latency over a bare stream.
- [x] Offline harness: custom source pushes a 48 kHz float WAV through the chain in fixed chunks with synthetic timestamps; capture via `obs_source_add_audio_capture_callback` → WAV. This is `knobs-harness`, which CTest also runs.
- [x] **Go/no-go gate:** harness output is bit-identical across runs (met), and live audio reaches VB-Cable with latency comparable to OBS (met: 88.1 ms vs 87.7 ms).

**M2 — Config import + OBS comparison**
- [x] Resolve active profile + scene collection (`user.ini` / `global.ini`); verify key names against the installed version. Verified against 32.2.2 (Config import in §4); `FindActiveObsConfig` and `FindSceneCollectionFile` in `src/import/obs_config.h`.
- [x] Enumerate candidate mic sources (`sources[]` + `AuxAudioDevice*` keys); picker if multiple. `SceneCollection::Mics` and `PickMic` (`src/import/mic_import.h`); the tools take `--pick`, and the tray's picker is M3.
- [x] Pre-flight checks (VST strip, unknown IDs, compressor sidechain, push-to-talk/mute) with warnings. `SceneCollection::Import`. CTest runs `knobs-import` on a made-up collection (`tests/fixtures/obs-config`) that sets off every check.
- [x] Load via `obs_load_source()`; apply post-load fixups. Call `obs_source_load2` for a mic from `sources`, not for a global audio device, as OBS does (§4 step 6). `--import` in `knobs-harness` and `knobs-live`. The real collection's mic (Mic/Aux: EQ, expander, compressor, limiter, Mono on) loads with nothing to warn about, runs bit-identically in the harness, and reaches CABLE In 16ch, the profile's monitoring device, 82 ms after each push.
- [x] Comparison vs OBS: same WAV (leading ~2 s of silence so envelopes settle identically) through an OBS Media Source with the chain pasted on and Mono/balance matched → record 32-bit float PCM. Run the same file through the harness with the imported chain. Align via cross-correlation; residual must fall below a set threshold (expect around −120 dB rather than zero, since packet sizes differ; see M1 findings. Investigate anything audible). `knobs-compare` automates all of it, OBS included. Bit-identical when both sides use the same packets; −120 dB with different ones (M2 findings).
- [x] Real voice: OBS vs knobs. Replaced the blind ABX on 2026-10-05, since an ABX can't tell identical renders apart (M2 findings). `knobs-compare --in` on a recording of Daniel's mic: bit-identical.
- [ ] Support OBS 33.0 once it's released (OBS 33.0 notes in §4). Needed here because the comparison runs against the installed OBS, and knobs refuses 33.x until then. Still in beta on 2026-10-05 (33.0.0-beta6). The load fix is in.

**M3 — Tray app** (done 2026-10-06 against OBS 32.2.2; see the M3 sections in §4)
- [x] Always-on core, with no UI: the states the tray shows, following OBS and the audio devices, and rebuilding the chain (The always-on core in §4). `knobs-core` runs it with no tray. Done 2026-10-05.
- [x] Win32 tray: start/stop, device + cable pickers, re-import, autostart toggle, log access (Tray and first run in §4). Built 2026-10-05: `knobs.exe`, with the menu as designed. `knobs-tray` runs it on a fake core.
- [x] OBS folder picker when auto-detection fails; remember the choice in `%AppData%\knobs`. Find OBS… in the menu, saved as `Settings::obs_dir`. OBS's settings folder (a portable OBS started with `--portable`) is picked on the first run's Can't read OBS's settings page, saved as `Settings::obs_config`.
- [x] OBS coexistence: watch for `obs64.exe`; auto-pause while it runs, resume when it exits (toggleable), and re-import when it exits. The core watches for OBS, pauses, resumes and re-imports (M3 findings), and the tray's Pause while OBS is open turns pausing off. Decided 2026-10-05: it pauses whether or not OBS monitors the mic (Tray and first run in §4).
- [x] Detect installed-OBS version change on startup → prompt re-import / runtime refresh (restart the process to load the new copy; prune old copies). The core says a restart is needed when a re-import finds a new version, and why, and stops on an unsupported one. The tray restarts by itself, and the new copy says so (Built: notifications). Started after an update, knobs uses the new version from the start, so nothing needs checking then. Pruning done 2026-10-05: once libobs has started, knobs removes copies of other versions and the copier's leftovers that no process uses (§4 step 2).
- [x] Device disconnect/reconnect. win-wasapi's reconnect thread only runs after `activate`, which needs the video tick (M0 findings). Choose between the dummy canvas and re-creating the source via `obs_load_source()`; either way, knobs surfaces state rather than reimplementing capture. Decided: re-create the source (M3 findings). Built in the core.
- [x] Error surfacing via tray notifications. Built 2026-10-05 (Built: notifications): a missing mic or cable after 5 s, the mic or chain changed in OBS, a filter knobs can't run, OBS updated, and the other problems that stop knobs, each taken down when it ends, with a click opening the fix. The icon's badges show the state while Do Not Disturb hides them. `knobs-tray` shows each.
- [x] First run: one door for a mic already set up in OBS, one for people new to OBS (Tray and first run in §4). Built 2026-10-05 (Built: the first run): one task dialog, pages for each gap (no OBS, an unsupported one, its settings missing, no mic or no filters, several mics, monitoring to the default device, no cable, OBS open), resuming where it stopped, reopened by Setup…. `knobs-tray --first-run` shows any page. The setup guide it links to is the M4 README's.
- [x] Exe icon from `assets/knobs.ico`. Done 2026-10-05: the logo has no tile anywhere in the app. `knobs.ico` is the knob alone, scaled up to fill the square, rendered from `knobs-app-icon.svg` by `tools/render-icon.ps1`. The tray shows it too, with Windows 11's status badges, instead of a glyph of its own (Tray and first run in §4).
- [x] Test on real hardware against §7's criteria 2–5. Done 2026-10-06 (M3 test on real hardware in §4): knobs.exe met all four on the iD4, VB-Cable and Discord.
- [x] Long-run latency, made defensive. Done 2026-10-06 (Long-run latency in §4): checked what libobs's monitor does with a backlog, a full buffer and an empty one, measured the iD4's and VB-Cable's clocks (31.5 ppm apart) and the gap a restart leaves (about 10 ms), and the core restarts the monitor in silence, at most every 10 min.
- [ ] *(Optional)* Long-run latency on real hardware: the mic into the cable for hours, beside OBS, to confirm what the code and the measurements predict (no creep on this PC, a dropout every 5 min or so in both, restarts only in silence).

**M4 — Ship**
- [x] Packaging: small exe (no bundled libobs); simple installer or portable zip. Both, built 2026-10-07 (Packaging in §4).
- [x] README with the pitch ("close OBS, keep your mic"), setup guide, VB-Cable pointer, "requires OBS Studio installed" + supported version range. The first run links to the setup guide as the README's `#setup` anchor (`kSetupGuideUrl`). Written 2026-10-06; since 2026-10-07 Setup downloads the installer or the portable zip from the latest release.
- [x] CI: GitHub Actions builds knobs, runs every test against OBS 32.2.2 installed on the runner (a skipped test fails the run), and packages both on each push and pull request (`.github/workflows/build.yml`, called by `ci.yml`). A tag such as `v0.1.0` that matches `CMakeLists.txt`'s version drafts a release with the packages and their checksums (`release.yml`); publishing it is a click on GitHub. Added 2026-10-07.
- [x] GPL-2.0-or-later compliance (links libobs): publish source, include license texts. Done 2026-10-07: the source is public, and each package has LICENSE.txt and a NOTICE.txt that links this version's source. The logo files in `assets/` are carved out of the GPL, all rights reserved apart from what TRADEMARKS.md allows (decided 2026-10-07).
- [ ] AI-use disclaimer in the forum post (required by the OBS Forum Resource and IP Policy)
- [ ] Post to OBS forums / r/obs — this is where the users who asked for this live

## 6. Risks & open questions

| Risk | Notes / mitigation |
|---|---|
| libobs ABI drift across OBS versions | `GetProcAddress` function table; tested version range; refuse to start outside it with a clear message. 33.0 deprecates the monitoring-type setter knobs uses and changes the saved monitoring key (OBS 33.0 notes). |
| OBS not installed / non-standard install (Steam, portable) | Registry → default path → manual picker. OBS installed is a stated requirement. |
| Loaded DLLs block OBS updates | Always load from the shadow copy in `%LocalAppData%\knobs\runtime\`, never from the install dir. |
| Audio-only init has undocumented video dependencies | Confirmed in M0: activation and `video_tick` need the graphics thread. Audio is unaffected. Decided in M3: the core rebuilds the source when a device comes back, with a watchdog for stalls (M3 findings). |
| `data/` path resolution (`find_libobs_data_file`) | Resolved in M0: the copy mirrors the install, the working directory is the copy's `bin\64bit`, and module paths are passed explicitly. The smoke test verifies both. |
| Monitoring path latency differs from OBS | Same code path as OBS monitoring. Mic to cable on the same cable input: knobs 88.1 ms, OBS 87.7 ms (M1 findings). |
| Latency creeps up over long sessions | libobs's monitor doesn't correct its delay for audio-only sources, in OBS either: a hiccup raises it, and a mic whose clock is faster than the cable's raises it steadily. The core restarts the monitor in silence at most every 10 min, which clears what the monitor queued; a step downstream, in Windows' engine or the cable, stays (Long-run latency). |
| Dropouts from a mic slower than its cable | The iD4's clock is 31.5 ppm slower than VB-Cable's, so the monitor's stream runs dry every 5 min or so: a 9 ms gap, in OBS too. Restarts don't help. Only following the cable's clock would, which OBS doesn't do (§8). |
| Doubled audio when OBS and knobs both monitor to VB-Cable | Auto-pause while `obs64.exe` runs. OBS is seen through its instance mutex within a second of starting, before it can load its audio, and a portable OBS within 2 s (M3 findings). On real hardware, knobs let go of the cable 0.76–1.45 s before OBS came on. A warm OBS opened its monitor 1.44–1.48 s after its mutex, so the worst case leaves about 0.4 s; the 1 s check stays (M3 test on real hardware). |
| Push-to-talk/mute on the imported source | Doesn't silence the cable: libobs's monitor ignores mute, in OBS too, from 32.2.0 on (M1 findings). The import summary notes it. PTT support is a v2 idea. |
| OBS updates change scene JSON schema / filter IDs | `obs_load_source()` from the user's own OBS version; pre-flight validates and warns on unknown IDs. |
| Chain includes a VST filter | Not supported in v1: stripped with a loud warning. v2 feature. |
| NVIDIA noise suppression needs an external runtime | v1 promises Speex/RNNoise. In OBS 32 NVIDIA's filter lives in the separate `nv-filters` module, which v1 doesn't load, so a chain using it imports without it (warned). Loading `nv-filters` is a v2 idea. |
| Trademark ("OBS" in name) | OBS Forum Resource and IP Policy (Jan 2026) asks tools to avoid the OBS acronym in names. The name is knobs, all lowercase, since 2026-10-05 (it was knOBS). If the OBS team asks for a change, reconsider then. The name is one constant in code. The logo's wordmark still colors "obs" red, on purpose; it falls under the same rule. |
| OBS forum rules on AI-assisted code | Policy requires an AI-use disclaimer and discourages listing resources mostly written by AI. Include the disclaimer; be ready to explain authorship. |
| GPL obligations | Fine: knobs is GPL-2.0-or-later (same as libobs) and open source. |

## 7. Success criteria

1. Harness output is bit-identical run-to-run, and the comparison against OBS is below threshold on test signals and real voice. Met in M2: the outputs are bit-identical.
2. OBS closed, knobs in tray: mic sounds identical in Discord/Zoom/games. Met on 2026-10-06 in Discord: knobs and OBS sounded the same by ear (M3 test on real hardware). Zoom and games weren't tried.
3. Cold boot → working filtered mic with zero clicks. Met on 2026-10-06: knobs was running 9.5 s after logon, with nothing clicked.
4. Opening OBS while knobs runs never produces doubled audio. Met on 2026-10-06 over six opens and closes: knobs and OBS were never active on the cable at once, and no echo was heard. The worst case leaves about 0.4 s on that PC.
5. Idle resource usage meaningfully below OBS-minimized. Met on 2026-10-06: running, knobs used 0.72% of a core and 36 MB, against OBS minimized at 5.64% and 469 MB.

## 8. Future ideas (v2+)

- **VST 2.x filters** (deferred from v1). Known constraints: `obs-vst` links Qt Widgets and drives its plugin object through Qt, while knobs has no QApplication. Needs Qt DLLs in the runtime copy, the "open interface when active" setting forced off (opening the editor would construct a QWidget with no QApplication, which aborts), and testing against plugins that expect a message pump on their thread. Give it its own milestone.
- Following the cable's clock, to remove the dropout every few minutes from a mic slower than its cable, and the creep from a faster one (Long-run latency). It means resampling the mic by a few tens of ppm, which OBS doesn't do, so it would no longer be exactly OBS's output. Decide first whether that's acceptable.
- NVIDIA noise suppression / Audio Effects as a supported feature by loading `nv-filters` (see §6)
- Push-to-talk / push-to-mute via global hotkeys. Since the monitor ignores mute from 32.2.0, this would have to turn monitoring on and off, as 33.0's monitor hotkeys do in OBS.
- Level meter / simple visualizer in a tray flyout
- Filter enable/disable toggles (no parameter editing — still tune in OBS)
- Auto re-import on scene collection file change (file watcher)
- Multiple sources (second mic / line input)
- macOS port (CoreAudio monitoring backend)
- "Profiles" — switch between chains (streaming voice vs. calls)

---

### Revision notes — Rev 24 (2026-10-07)

- The tray badges are the logo's colors instead of Windows 11's: a white "!" on red and white pause bars on graphite, on a smaller disc (7/16 of the icon), with the bars as thin as the "!" (Built: notifications).
- A portable copy: a `portable_mode.txt` beside the exe, as OBS's file of that name, keeps all of knobs's state in a `data` folder beside it (`AppDirsFor`). OBS's settings are still read from `%AppData%`.
- Packaging: an installer and a portable zip (Packaging, in section 4). The logo files are carved out of the GPL: TRADEMARKS.md says how the name and logo may be used.
- CI on GitHub Actions, with OBS installed so no test skips, and a release workflow that drafts a release from a version tag (M4).
- Code signing with Azure Artifact Signing, in the Release workflow, once the Azure side is set up (Code signing, in section 4).

### Revision notes — Rev 23 (2026-10-06)

- Fixed `knobs.ico`'s jagged edges: `tools/render-icon.ps1` clamped alpha with `[math]::Max(0, …)`, which PowerShell ran as integer math, so every edge pixel came out fully opaque or fully clear. It now renders each size at 8 times and averages each 8×8 block. The icon has a size for each display scale from 100% to 300%.
- Notifications show the knob from a 96 px icon instead of stretching the 32 px one (Built: notifications, Balloon details).

### Revision notes — Rev 22 (2026-10-06)

- Timed the iD4's mic: it runs on the same clock as its playback side, so the long-run latency findings hold.
- Silence is judged by each second's typical level, leaving out its loudest 100 ms, and a restart waits for the last 200 ms to be quiet. After a review: the −30 dBFS bar described as the code has it.

### Revision notes — Rev 21 (2026-10-06)

- Long-run latency, made defensive: checked against 32.2.2 that the monitor never corrects its delay for a mic, measured the iD4's clock 31.5 ppm slower than VB-Cable's and a restart's gap at about 10 ms, and the core now restarts the monitor in silence, at most every 10 min. The hours-long test is optional.
- Found that on this PC the stream runs dry every 5 min or so, a 9 ms dropout, in OBS too; that's likely M1's 10.6 ms step.

### Revision notes — Rev 20 (2026-10-06)

- Tested knobs.exe on real hardware against §7's criteria 2–5: the daily path, the same sound as OBS, no doubling, losing the mic, idle cost and a cold boot. All passed (M3 test on real hardware).
- Measured how soon OBS opens its monitor after its mutex: 1.44–1.48 s warm, leaving about 0.4 s in the worst case. The 1 s check stays.

### Revision notes — Rev 19 (2026-10-06)

- After the second review: pruning retries setting a copy aside and stops when knobs quits. OBS in other sessions is followed by session, and its notice never hides another, fits its title and says "your cable" before one is chosen. Several warnings list those that fit whole.
- Can't read OBS's settings offers Try again. This version of OBS isn't supported offers Choose folder…, and Look for OBS for a picked install. The tools' `--obs-config` suggests opening OBS only for OBS's own folder.

### Revision notes — Rev 18 (2026-10-05)

- knobs.exe prunes runtime copies it no longer needs once libobs has started: other versions, copies set aside and interrupted staging folders, leaving any in use for next time.
- Corrected §4 step 2: a loaded DLL doesn't stop its folder from being renamed. An open file or a working directory in it does, and libobs's working directory is what protects a running knobs's copy. Pruning also checks the DLLs.
- Making a copy and loading `obs.dll` from it now hold the copy mutex together.

### Revision notes — Rev 17 (2026-10-05)

- knobs pauses while OBS is open whether or not OBS monitors the mic. The OBS coexistence item is done.
- OBS in another account won't be tested with two accounts: the warning covers it.

### Revision notes — Rev 16 (2026-10-05)

- Can't read OBS's settings suggests opening OBS only for the folder OBS keeps its settings in. A picked folder elsewhere offers Choose folder… and Use OBS's own folder.
- Find OBS… always opens the first run's page, which also says how to get OBS.
- The restart error says to quit knobs and start it again.
- OBS in another account is described by what it means for the mic: it may send audio to the same cable.

### Revision notes — Rev 15 (2026-10-05)

- A picked OBS folder or settings folder that stops working stays picked, and the first run offers Look for OBS or Use OBS's own folder, next to Choose folder… (decided 2026-10-05: nothing changes behind the scenes).
- Notifications with several warnings start with how many and that a click shows them all, and a long chain is given in the short form.
- knobs says when OBS is open in another Windows account, since it can't keep that OBS's audio out of the cable.
- Fixed a first-run button that could be clicked again while an error was up.

### Revision notes — Rev 14 (2026-10-05)

- After the M3 code review: the watchdog times a chain from when it started, so a slow first start isn't a stall. libobs isn't started twice in one process once a module has loaded. Settings re-import only when the install, OBS's settings folder or the mic changed. Another user's OBS doesn't pause knobs, and the tests no longer use OBS's real names. Renaming the mic or a filter, or a cable ID's case, doesn't restart the chain or announce a warning again. A saved mic pick always means that name. A picked folder that stops working stays picked, and the first run offers to look for OBS or use OBS's own settings folder instead. Restarts retry once knobs's windows close, and warnings fit the notification.
- New open items: notifications longer than Windows 11 shows, another user's OBS on the same cable, and a first-run button that can be clicked twice while an error is up.

### Revision notes — Rev 13 (2026-10-05)

- The logo has no tile anywhere in the app (decided 2026-10-05). `knobs.ico` is now the knob alone, scaled up to fill the square, and the tray uses it rather than a glyph of its own, so the knob isn't redrawn for each small size. It gets no rim on a dark taskbar.
- The badges are Windows 11's own: its caution yellow and its neutral gray, with a black "!" and black pause bars with whole-pixel strokes. They're the same on a light and a dark taskbar, so knobs's icons don't change with Windows' mode.
- `tools/render-icon.ps1` renders an SVG into an `.ico`, as `knobs.ico` is made.

### Revision notes — Rev 12 (2026-10-05)

- Built the notifications (Built: notifications, §4): a mic or cable missing for 5 s, the mic or its chain changed in OBS with the chain, a filter knobs can't run, and OBS updated. Each is taken down when what it said ends, and a click opens the fix. Nothing shows on a normal start, or when OBS opens and closes without changes.
- The icon shows the state too, for when Do Not Disturb hides notifications: a pause badge and an amber "!" badge, drawn over `knobs.ico` until the glyph is.
- knobs restarts by itself when the core needs it to follow OBS, for an update or a new audio format, and the new copy says why. The core's snapshots say why a restart is needed.
- Decided: the cable gets the mic's 5 s grace too, and the other problems that stop knobs (OBS's settings, the mics, the cable choice, OBS missing, a failed chain) get a notification as well.
- Fixed: a finished first run, opened again, offered Start with Windows checked, so Done turned it back on after the user had turned it off.

### Revision notes — Rev 11 (2026-10-05)

- Built the first run as designed: one task dialog that moves between pages, the two doors, only the pages each case needs, the second door's steps, and the last page with Start with Windows. It covers OBS missing, unsupported or never set up, a mic with no filters or no mic, several mics, monitoring to the default device or to something that isn't a cable, no cable installed (it moves on when one appears), and OBS open. It resumes where it stopped, and Setup… reopens it. Details under Built: the first run (§4).
- The second door's steps use OBS 32.2.2's own names, checked against its frontend and locale files. In 32.2's mixer, a source's name opens its menu.
- Closed three open questions: cable recording sides, the OBS names, and Open OBS's working directory.
- The core's snapshots carry the settings they were worked out with, the recording devices and the OBS install; mics list their filters, and notes say whether they change what to expect.
- Choose a mic… and Choose a cable… open the first run instead of their submenus.
- After review: the second door's steps name no filters ("add filters as needed"), the chain knobs shows leaves out placeholders for filter types libobs doesn't have, such as NVIDIA's, pre-flight now loads a compressor with a sidechain as what it is while nothing plays on the sidechain, bit for bit: OBS's Gain filter at its output gain (Config import), and the logo's cream (the icon's tile, the knob's ticks, the wordmark on dark) became `#e8eaee`, a light tone of the knob's graphite. `knobs.ico` is rendered again from the SVG.

### Revision notes — Rev 10 (2026-10-05)

- Built the tray shell: `knobs.exe` with the tray icon and the menu as designed, the fix for what needs the user in bold, dark menus, settings in `%AppData%\knobs\settings.ini`, Start with Windows and one copy per session. `knobs-tray` runs it on the real core over a made-up OBS and devices, and saves pictures of the menu. Details under Built: the tray shell (§4).
- Decided the menu's details: the status line for each state, the fix for each, when Mic and Cable offer "Same as OBS", and that dark menus follow Windows' mode rather than the app mode.
- The core gained the pause-while-OBS-is-open setting and the name of a picked cable, and its snapshots carry the playback devices, the profile's monitoring device and whether the user paused.

### Revision notes — Rev 9 (2026-10-05)

- M3 started with the always-on core in `src/core`, with no UI yet. It has the states the tray shows, pauses while OBS is open and imports again when OBS exits, hears devices come and go, and rebuilds the chain with libobs's loader when the mic or the cable is back. `knobs-core` runs it. See The always-on core (§4).
- Device loss decided: (b), rebuilding the source, with a watchdog for stalls. Without video, win-wasapi never restarts a mic that lost its device, or a "default" mic after the default device changes (M3 findings).
- Pausing releases the chain, so resuming always loads it fresh. Reloading only on a change applies to a re-import while the chain runs.
- A profile's new sample rate or channel layout needs a restart, as in OBS. Closed that open question.
- OBS is seen through its instance mutex, with a process scan as a backstop: 0.07% of a core, against 0.2–0.4% for scanning every second.

### Revision notes — Rev 8 (2026-10-05)

- Shaped the M3 tray menu and first run (Tray and first run in §4): a native menu, a TaskDialog first run with one door for OBS users and one for people new to OBS, a tray glyph of its own with state badges, and re-import each time OBS exits.
- New open question: pausing while OBS is open silences the cable when OBS doesn't monitor the mic.

### Revision notes — Rev 7 (2026-10-05)

- Added the logo in `assets/`: the knob mark, the app icon, the horizontal lockup (now at the top of the README) and `knobs.ico` for the tray app. The wordmark keeps its red "obs"; the trademark row covers it.
- The audience includes people new to OBS, who'd install it only to tune a mic for knobs. Added an M3 item for a first run that explains the setup. Product context for design work is in `PRODUCT.md`.

### Revision notes — Rev 6 (2026-10-05)

- Renamed to **knobs**, all lowercase (it was knOBS). The display name constant changed; the `%AppData%` and `%LocalAppData%` folders keep working because Windows paths are case-insensitive.
- M2 import done: the active profile and scene collection are found by the names saved in them, as OBS 32 does, with `[Locations]` and portable installs handled. Mics come from the global devices and `sources`, pre-flight runs on the source object, and the load follows the mic's origin. The details are in Config import (§4).
- M2 comparison done: `knobs-compare` runs the user's real OBS from a portable copy and compares. knobs's output is bit-identical to OBS's when the packets match. The −120 dB residual the plan expected comes only from packet size (M2 findings).
- Replaced the blind ABX with a comparison on real voice, since an ABX can't tell identical renders apart. On a recording of Daniel's mic, the output is bit-identical too. Success criterion 1 changed to match.
- `LoadSourceJson` now also overrides 33.0's `monitoring_enabled`. 33.0 is still in beta.
- knobs reads OBS's files itself rather than through `obs_data_create_from_json_file_safe`, which renames a backup over a broken file.
- After the M2 code review:
  - `knobs-compare` lines up short inputs, compares bit for bit, and does to knobs's side what OBS's recording mix does (clamping, −0.0). It loads the Media Source where the mic was, leaves a portable install's settings out of its copy, and swaps copies by renaming.
  - `knobs-live --import` won't monitor to a profile's `default` device without `--output`, and takes `--mic` only with `--external`.
  - `SampleRate` is read as libobs reads it, and `--obs-config` recognizes a portable install's `config` folder.
  - The tools share one start-up path, and each collection file is parsed once.

### Revision notes — Rev 5 (2026-10-04)

- M1 done. With the iD4, knOBS measured 88.1 ms from mic to cable and OBS 87.7 ms on the same cable input. One knOBS run stepped up 10.6 ms partway through. libobs's monitor never corrects that for a mic, so it's a risk for long sessions; added an M3 item to measure it over hours.
- After the M1 code review: OBS loads global audio devices without `obs_source_load2` but scene sources with it, so import has to follow the mic's origin (§4 step 6, M2). Built-in source JSON now loads as the current libobs version.
- Name: keep knOBS without asking the OBS team first, and reconsider only if they ask. Dropped the M4 name-check item.
- The monitor ignoring mute is a 32.2.0 change, not long-standing. Before 32.1, mute and push-to-talk both silenced the monitor. Added the version history to the M1 findings.
- Added OBS 33.0 notes from 33.0.0-beta6. Monitoring becomes on or off: Monitor Only is gone and the monitoring-type setter is deprecated. The saved monitoring key changes, which `LoadSourceJson` has to handle. Added an M2 item to support 33.0 once it's released.

### Revision notes — Rev 4 (2026-09-30)

- M1: the offline harness is done and deterministic, and the live path is built. Mic-free measurements show libobs's monitor adds no latency over VB-Cable's own. The mic runs are still to do. Added the M1 findings (§4).
- libobs's monitor ignores mute, so push-to-talk and mute don't gate what OBS sends to VB-Cable either. Changed §4 step 7, the pre-flight and the risk table: import notes push-to-talk instead of clearing it.
- Import loads through `obs_load_private_source`, the private form of `obs_load_source`.
- The M2 comparison should expect a residual around −120 dB, not zero, because the output depends on packet size at rounding level.

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
