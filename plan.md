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
2. **Copies the needed subset** into `%LocalAppData%\knobs\runtime\<obs-version>\`, mirroring the install layout (`bin\64bit`, `obs-plugins\64bit`, `data\libobs`, `data\obs-plugins\<module>`) so relative data lookups still resolve. Loading from the copy keeps OBS's own DLLs unlocked, so OBS updates aren't blocked by a running knobs. The subset is computed from the PE import tables, not hardcoded: the import closure of `obs.dll`, `libobs-d3d11.dll` and the two modules, limited to DLLs the install ships, plus their PDBs, `data\libobs` and the two modules' data folders. For 32.2.2 that's 200 files, 53.6 MB, mostly FFmpeg: `obs.dll` imports avcodec, avformat, avutil, swscale and swresample directly, and those pull in libx264, librist, srt and zlib. The VC++ runtime and Windows DLLs come from the system. The copy happens in a staging folder with a manifest written last, so an interrupted copy is never used. Files are flushed to disk before the manifest. The manifest records each file's size and the install file's timestamp. The copy is reused only while the install still matches, because OBS drops beta and RC suffixes from `obs.dll`'s version resource, so the version alone can't tell a beta from the final release. Copies are serialized across processes with a named mutex. A replaced copy is renamed aside rather than deleted, so replacing fails as a whole while a running knobs has it loaded.
3. **Loads `obs.dll` from the copy** and resolves the functions it needs (31 in M0, 54 after M1, 72 after the M2 import) via `GetProcAddress` into a function table (`KNOBS_OBS_API` in `src/runtime/obs_api.h`, typed from the vendored headers in `third_party/libobs`). No import lib needed, and a missing export fails gracefully instead of at process load. `obs.dll`'s own dependencies resolve from its folder and the system, never PATH or the install. A successful load changes two process-wide settings; a failed load undoes both. `AddDllDirectory(bin\64bit)` lets libobs find the graphics module by bare name. The working directory becomes the copy's `bin\64bit`, because libobs resolves `../../data/libobs/` against the working directory (`obs-windows.c`, `find_libobs_data_file`), so nothing may change it while libobs runs (the folder picker uses `FOS_NOCHANGEDIR`). Separately, the host process calls `SetDefaultDllDirectories` at startup, so plain `LoadLibrary` calls skip PATH and the working directory.
4. **Checks `obs_get_version()`** against the supported range. Outside it, refuse to start with a clear tray message. The range is 32.2.0 up to, but not including, 33.0.0. The floor is the minor version of the vendored headers (32.2.2); patch releases don't change the libobs API, so 32.2.0 and 32.2.1 have the same declarations. The ceiling is the next major, which is where libobs makes breaking API changes (`obs-config.h`). Only 32.2.2 has been tested. 33.0 is in beta and changes the monitoring API (OBS 33.0 notes). The install's version is checked before anything is copied, and again from `obs_get_version()` after loading.

When the installed OBS version changes, knobs notices on startup and prompts a re-import, which refreshes the runtime copy. The refresh needs a process restart: libobs never unloads module DLLs (`os_dlclose` is commented out in `free_module`), and they keep `obs.dll` loaded too.

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
4. Video: **no** `obs_reset_video()` through M2. Audio flows without it. Device-loss recovery is the open question; see M0 findings.
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
  - `video_tick` callbacks never run. The compressor looks up its sidechain source there. That only matters if a sidechain is set, and knobs doesn't load other sources anyway, so it's an M2 pre-flight warning.
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
- **Latency can step up after a hiccup and stays there.** In the knobs run above, the last quarter measured 98.7 ms, 10.6 ms above the rest of the run. That's about one audio period, so it looks like a hiccup that dropped or delayed about 10 ms of audio. Every other run held within ±0.2 ms. libobs's monitor only corrects its delay for sources with video (`process_audio_delay` in `wasapi-output.c`). For a mic, audio queued by a hiccup stays in the monitor's 1 s buffer, so the latency stays higher until the stream restarts. OBS runs the same code. Open questions for G4: how often this happens over hours, whether knobs hits it more often than OBS, and whether the iD4 and the cable drift apart. A 5-minute run without the mic (`--measure-output 300` into CABLE In 16ch) held at 102.0 ms in every quarter. So the monitor and cable alone didn't step there. The capture side, or the tool's own recording of the mic, is the likelier source.
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

- **Settings folder:** `%AppData%\obs-studio`. A portable install, one with a `portable_mode`, `obs_portable_mode`, `portable_mode.txt` or `obs_portable_mode.txt` file next to `bin\`, keeps its settings in `<install>\config\obs-studio` instead (`obs-main.cpp`). OBS's `--portable` flag also turns portable mode on, which knobs can't see from outside, so the M3 tray needs a way to pick the folder (`--obs-config` in the tools). A folder picked that way counts as portable if it's the `config` folder next to a marker.
- **Active profile and scene collection:** `[Basic] Profile` and `SceneCollection` in `user.ini` (OBS 31 and later). Without a `user.ini`, they're in `global.ini`, where OBS 30 and older kept them; OBS 31 and later copy `global.ini` to `user.ini` when they first start. `global.ini`'s `[Locations]` (`Configuration`, `SceneCollections`, `Profiles`) can move `user.ini`, the collections and the profiles elsewhere. A location that doesn't exist falls back to the settings folder, and portable mode ignores them.
- **Found by name, not by file.** OBS 32 matches `Profile` against the `[General] Name` in each profile folder's `basic.ini`, or the folder's name if there's none, and `SceneCollection` against the `"name"` saved in each `.json` file in the scenes folder, or the file's name. `ProfileDir` and `SceneCollectionFile` are written but not used for this. The first match in directory order wins, as in OBS.
- **INI files** are read by a parser that matches libobs's (`util/config-file.c`): the BOM is skipped, nothing is trimmed but a key's leading whitespace, `#` starts a comment, `\\`, `\n` and `\r` are unescaped, and a repeated section or key replaces the earlier one. knobs has its own parser because the profile has to be read before libobs starts: it sets libobs's audio format.
- **Scene collections** are parsed by libobs (`obs_data_create_from_json`), as OBS parses them, falling back to `<file>.bak` if the file doesn't parse. OBS falls back the same way through `obs_data_create_from_json_file_safe`, which also renames the backup over the broken file. knobs reads the files itself so that nothing is written.
- **Profile settings:** `basic.ini` `[Audio]`: `SampleRate` (default 48000), `ChannelSetup` (default `Stereo`, mapped to a speaker layout as `OBSBasic::ResetAudio` does), `MonitoringDeviceId` (default `default`) and `MonitoringDeviceName`. libobs runs at the profile's sample rate and layout, as in OBS. Numbers are read as libobs reads them (`strtoull`), so `48000 ` with a trailing space works. A profile left at `default` monitors to the default playback device, usually speakers: `knobs-live --import` won't monitor there without `--output`, and the tray needs to ask for the cable then (M3).
- **Candidate mics:** every `wasapi_input_capture` among the global audio devices (`DesktopAudioDevice1` and `2`, `AuxAudioDevice1` to `4`, in OBS's load order) and in `sources[]`. If there's more than one, the user picks: `--pick` by number or name in the tools, and a picker in M3.
- **Pre-flight**, on the mic's source object before it's loaded:
  - `vst_filter` entries are removed, with a warning (a note if the filter is off).
  - Filters libobs doesn't know, `nvidia_audiofx_filter` among them, stay in: libobs loads a placeholder for each that passes audio through. A warning says so, unless the filter is off.
  - A compressor whose `sidechain_source` isn't empty or `none` gets a warning.
  - Filters that are off are noted, and stay off.
  - Notes for the source: muted, push-to-talk, push-to-mute or disabled (the monitor ignores all of them), a sync offset (the monitor ignores it for audio-only sources), and a mic OBS doesn't monitor.
  - Mono, balance and volume show in the chain summary libobs reports after loading. The tools also warn if the mic's device or the monitoring device isn't connected.
- **Load:** libobs serializes the pre-flighted object, and `obs_load_private_source` loads it, followed by `obs_source_load2` for a mic from `sources` (§4 step 6).

### Tray and first run (M3 design)

Shaped on 2026-10-05, not built yet. The UI uses Windows' own controls. The brand shows only in the knob icon and the line saying knobs isn't affiliated with the OBS Project.

- **One status format.** Status is the mic's chain, named as OBS shows it, enabled filters only: `Mic/Aux › 3-Band EQ › Expander › Compressor › Limiter › CABLE In 16ch`. The menu and tooltip use the short form `Mic/Aux › 4 filters › CABLE In 16ch`. The first run and notifications use the same format.
- **First run:** one `TaskDialogIndirect` window that changes pages with `TDM_NAVIGATE_PAGE`. It stays light in dark mode, which is fine for a window seen once.
  - The first page offers two command links, "I set up my mic in OBS" and "I'm new to OBS". Each note says what knobs found ("Found Mic/Aux with 4 filters.", "Mic/Aux has no filters yet.", "OBS Studio isn't installed."), and the one that fits is the default.
  - The first door shows only the pages it needs. Which mic, when there are several (a `default` device reads "Default communications device (…)", M1 findings). Which cable, when the profile monitors to `default` or to a device that isn't a cable; with no cable installed, it links to VB-Cable and moves on by itself when one appears. Warnings, for pre-flight warnings and notes that change what to expect, such as push-to-talk.
  - The second door lists five steps in OBS: get OBS 32.2 if it's missing, open Filters on Mic/Aux, add Noise Suppression, Noise Gate, Compressor and Limiter in that order, adjust each by ear from OBS's defaults, and close OBS. It has an Open OBS button and links to the M4 setup guide. knobs suggests no settings. When OBS closes, knobs re-imports and continues with the first door's pages if the mic now has filters.
  - The last page shows the chain and says to choose the cable's recording side (CABLE Output) as the mic in other apps. It says where the tray icon is, since Windows 11 puts new icons under ^. A "Start with Windows" checkbox is checked by default.
  - Closing the window early leaves knobs in the tray, needing setup. An unfinished first run resumes where it stopped.
- **Tray menu:** a native menu (`TrackPopupMenuEx`), the same on left and right click. A status line, Pause/Resume, Mic ▸, Cable ▸, Re-import from OBS, Pause while OBS is open, Start with Windows, Setup…, Open log folder, About, Quit.
  - The status line can't be clicked. When something needs the user, the next item is the fix, in bold: "Finish setup…", "Choose a cable…", "Find OBS…".
  - Cable ▸ starts with "Same as OBS (…)" when the profile monitors to a cable. Any other pick is knobs's own setting and survives re-imports. Detected cables come first, and other playback devices go under "Other devices".
  - Dark menus come from uxtheme's `SetPreferredAppMode`, resolved by ordinal. Without it, the menu stays light.
- **Following OBS:** knobs re-imports when it starts and each time `obs64.exe` exits, and says so only when the mic or the chain changed. An OBS update to a supported version refreshes the runtime copy and restarts knobs. An unsupported one stops knobs, with a notification.
- **Tray icon:** a glyph of its own, not `knobs.ico`: the knob without its tile, filling the square, fitted to the pixel grid at 16, 20, 24 and 32 px, with a light rim on a dark taskbar. Running shows the knob alone, paused adds a pause badge, and needing the user adds an amber "!" badge. The knob itself is never recolored. The tooltip repeats the status line.
- **Notifications** are `Shell_NotifyIcon` balloons: what happened, then what knobs does or what to do. Clicking one opens the fix. They cover a mic missing for 5 s (so a power cycle stays quiet), a missing cable, a chain changed in OBS, a filter knobs can't run, and an unsupported OBS. Nothing shows on a cold boot, or when OBS opens or closes without changes.
- **Open:**
  - Pausing while OBS is open leaves the cable silent if OBS doesn't monitor the mic, which is likely once knobs does that job. Pausing only when OBS's saved settings monitor the mic to the same cable avoids that, but saved settings can lag behind what OBS is doing.
  - Cable recording sides: CABLE Input and CABLE In 16ch go to CABLE Output, CABLE-A Input to CABLE-A Output, VoiceMeeter Input to VoiceMeeter Output. For anything else, say "the recording side of …".
  - A re-import that changes the sample rate or channel layout may need `obs_reset_audio` or a restart.
  - Check the OBS names in the second door's steps against 32.2.2's locale files.
  - Open OBS has to start `obs64.exe` with its own `bin\64bit` as the working directory.

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

**M3 — Tray app**
- [ ] Win32 tray: start/stop, device + cable pickers, re-import, autostart toggle, log access (Tray and first run in §4)
- [ ] OBS folder picker when auto-detection fails; remember the choice in `%AppData%\knobs`
- [ ] OBS coexistence: watch for `obs64.exe`; auto-pause while it runs, resume when it exits (toggleable), and re-import when it exits. Decide whether to pause when OBS doesn't monitor the mic (Tray and first run in §4).
- [ ] Detect installed-OBS version change on startup → prompt re-import / runtime refresh (restart the process to load the new copy; prune old copies)
- [ ] Device disconnect/reconnect. win-wasapi's reconnect thread only runs after `activate`, which needs the video tick (M0 findings). Choose between the dummy canvas and re-creating the source via `obs_load_source()`; either way, knobs surfaces state rather than reimplementing capture.
- [ ] Error surfacing via tray notifications
- [ ] First run: one door for a mic already set up in OBS, one for people new to OBS (Tray and first run in §4). The M4 setup guide covers it too.
- [ ] Exe icon from `assets/knobs.ico`. The tray gets a glyph of its own, with badges for paused and needing the user (Tray and first run in §4). It still needs drawing.
- [ ] Long-run latency: run the mic into the cable for hours, alongside OBS for comparison, and watch for latency steps and clock drift (M1 findings). If latency creeps up, restarting the monitor resets it. Decide whether knobs should do that, for example while the mic is silent.

**M4 — Ship**
- [ ] Packaging: small exe (no bundled libobs); simple installer or portable zip
- [ ] README with the pitch ("close OBS, keep your mic"), setup guide, VB-Cable pointer, "requires OBS Studio installed" + supported version range
- [ ] GPL-2.0-or-later compliance (links libobs): publish source, include license texts
- [ ] AI-use disclaimer in the forum post (required by the OBS Forum Resource and IP Policy)
- [ ] Post to OBS forums / r/obs — this is where the users who asked for this live

## 6. Risks & open questions

| Risk | Notes / mitigation |
|---|---|
| libobs ABI drift across OBS versions | `GetProcAddress` function table; tested version range; refuse to start outside it with a clear message. 33.0 deprecates the monitoring-type setter knobs uses and changes the saved monitoring key (OBS 33.0 notes). |
| OBS not installed / non-standard install (Steam, portable) | Registry → default path → manual picker. OBS installed is a stated requirement. |
| Loaded DLLs block OBS updates | Always load from the shadow copy in `%LocalAppData%\knobs\runtime\`, never from the install dir. |
| Audio-only init has undocumented video dependencies | Confirmed in M0: activation and `video_tick` need the graphics thread. Audio is unaffected; device reconnect is decided in M3 (dummy canvas vs. source re-creation), with costs measured. |
| `data/` path resolution (`find_libobs_data_file`) | Resolved in M0: the copy mirrors the install, the working directory is the copy's `bin\64bit`, and module paths are passed explicitly. The smoke test verifies both. |
| Monitoring path latency differs from OBS | Same code path as OBS monitoring. Mic to cable on the same cable input: knobs 88.1 ms, OBS 87.7 ms (M1 findings). |
| Latency creeps up over long sessions | libobs's monitor doesn't correct its delay for audio-only sources, so a hiccup raises latency until the stream restarts. OBS has the same behavior. One 10.6 ms step seen in M1. Measured over hours in M3. |
| Doubled audio when OBS and knobs both monitor to VB-Cable | Auto-pause while `obs64.exe` runs. |
| Push-to-talk/mute on the imported source | Doesn't silence the cable: libobs's monitor ignores mute, in OBS too, from 32.2.0 on (M1 findings). The import summary notes it. PTT support is a v2 idea. |
| OBS updates change scene JSON schema / filter IDs | `obs_load_source()` from the user's own OBS version; pre-flight validates and warns on unknown IDs. |
| Chain includes a VST filter | Not supported in v1: stripped with a loud warning. v2 feature. |
| NVIDIA noise suppression needs an external runtime | v1 promises Speex/RNNoise. In OBS 32 NVIDIA's filter lives in the separate `nv-filters` module, which v1 doesn't load, so a chain using it imports without it (warned). Loading `nv-filters` is a v2 idea. |
| Trademark ("OBS" in name) | OBS Forum Resource and IP Policy (Jan 2026) asks tools to avoid the OBS acronym in names. The name is knobs, all lowercase, since 2026-10-05 (it was knOBS). If the OBS team asks for a change, reconsider then. The name is one constant in code. The logo's wordmark still colors "obs" red, on purpose; it falls under the same rule. |
| OBS forum rules on AI-assisted code | Policy requires an AI-use disclaimer and discourages listing resources mostly written by AI. Include the disclaimer; be ready to explain authorship. |
| GPL obligations | Fine: knobs is GPL-2.0-or-later (same as libobs) and open source. |

## 7. Success criteria

1. Harness output is bit-identical run-to-run, and the comparison against OBS is below threshold on test signals and real voice. Met in M2: the outputs are bit-identical.
2. OBS closed, knobs in tray: mic sounds identical in Discord/Zoom/games.
3. Cold boot → working filtered mic with zero clicks.
4. Opening OBS while knobs runs never produces doubled audio.
5. Idle resource usage meaningfully below OBS-minimized.

## 8. Future ideas (v2+)

- **VST 2.x filters** (deferred from v1). Known constraints: `obs-vst` links Qt Widgets and drives its plugin object through Qt, while knobs has no QApplication. Needs Qt DLLs in the runtime copy, the "open interface when active" setting forced off (opening the editor would construct a QWidget with no QApplication, which aborts), and testing against plugins that expect a message pump on their thread. Give it its own milestone.
- NVIDIA noise suppression / Audio Effects as a supported feature by loading `nv-filters` (see §6)
- Push-to-talk / push-to-mute via global hotkeys. Since the monitor ignores mute from 32.2.0, this would have to turn monitoring on and off, as 33.0's monitor hotkeys do in OBS.
- Level meter / simple visualizer in a tray flyout
- Filter enable/disable toggles (no parameter editing — still tune in OBS)
- Auto re-import on scene collection file change (file watcher)
- Multiple sources (second mic / line input)
- macOS port (CoreAudio monitoring backend)
- "Profiles" — switch between chains (streaming voice vs. calls)

---

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
