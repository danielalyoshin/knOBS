# knOBS

**Your OBS mic chain, without OBS.**

knOBS is a Windows tray app in the making. It runs the microphone filter chain you tuned in OBS Studio (noise suppression, gate, expander, compressor, limiter) and sends the result to a virtual audio cable. Discord, Zoom and games get the same processed mic while OBS stays closed.

> **Status: early development, not usable yet.** There's no tray app. Loading libobs and the live audio path work, and the dev tools below exercise them. Importing your OBS settings is next. [plan.md](plan.md) has the design and milestones.

## How it works

- **Your own OBS does the processing.** knOBS ships no libobs and no DSP of its own. It loads `obs.dll` from your installed OBS Studio, with only the `win-wasapi` and `obs-filters` modules. It loads from a private copy in `%LocalAppData%\knOBS`, so a running knOBS doesn't block OBS updates.
- **Your settings, loaded by OBS's loader.** knOBS will read your active OBS profile and scene collection without changing them. It recreates your mic source and its filters with OBS's own loader, so the settings are exactly the ones you tuned.
- **Out to a virtual cable.** libobs's audio monitoring sends the processed mic to the cable. There's no video, no window and no OBS process.

## Requirements

- 64-bit Windows
- OBS Studio 32.2.x installed. Support for 33.0 follows its release.
- A virtual audio cable, such as [VB-Cable](https://vb-audio.com/Cable/)

## Building

You need Visual Studio 2022 or newer with the C++ desktop workload, and CMake 3.25 or newer. From a Developer PowerShell for VS:

```powershell
cmake --preset x64
cmake --build --preset release   # or: debug
ctest --preset release           # tests that need OBS skip if it isn't installed
```

## Dev tools

The build puts these in `build\x64\<config>\`. Each one takes `--help`.

- `knobs-smoke` checks that libobs loads from the private copy and shuts down cleanly.
- `knobs-import` imports the mic from your active OBS profile and scene collection, without changing them, and shows what it found: the mics, warnings about anything knOBS can't reproduce, and the filter chain. It opens no audio devices.
- `knobs-harness` pushes a WAV through a filter chain, such as your imported one (`--import`), offline and checks that the output is bit-identical across runs. It opens no audio devices.
- `knobs-live` runs the live path into a virtual cable and measures its latency. Only `--run` and `--measure-mic` open the microphone.

## License

GPL-2.0-or-later, the same as libobs. See [LICENSE](LICENSE). `third_party/libobs` holds headers from obs-studio under their own GPL-2.0-or-later notices.

knOBS is an independent project. It isn't affiliated with or endorsed by the OBS Project.
