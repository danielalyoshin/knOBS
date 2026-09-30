# libobs headers (obs-studio 32.2.2)

Declarations knOBS compiles its `GetProcAddress` function table against.
knOBS never links libobs and never ships it; at runtime it loads the user's
installed `obs.dll` from a shadow copy.

- Source: https://github.com/obsproject/obs-studio/tree/32.2.2/libobs
- Commit: `ba2f32bdf791005443988a4955e963663e16b1ed`
- License: GPL-2.0-or-later, as stated in each file.

Only the include closure of `obs.h`, `util/base.h` and `util/bmem.h` is
vendored. `obsconfig.h` is a knOBS stub for the header obs-studio generates
at build time.

Don't edit these files. Regenerate them for another tag with:

```powershell
.\tools\vendor-libobs-headers.ps1 -Tag <tag>
```

Files:

- `callback/calldata.h`
- `callback/proc.h`
- `callback/signal.h`
- `graphics/graphics.h`
- `graphics/input.h`
- `graphics/math-defs.h`
- `graphics/srgb.h`
- `graphics/vec2.h`
- `graphics/vec3.h`
- `graphics/vec4.h`
- `media-io/audio-io.h`
- `media-io/frame-rate.h`
- `media-io/media-io-defs.h`
- `media-io/video-io.h`
- `obs.h`
- `obs-audio-controls.h`
- `obs-config.h`
- `obs-data.h`
- `obs-defs.h`
- `obs-encoder.h`
- `obs-hotkey.h`
- `obs-hotkeys.h`
- `obs-interaction.h`
- `obs-missing-files.h`
- `obs-output.h`
- `obs-properties.h`
- `obs-service.h`
- `obs-source.h`
- `util/base.h`
- `util/bmem.h`
- `util/c99defs.h`
- `util/darray.h`
- `util/profiler.h`
- `util/sse-intrin.h`
- `util/text-lookup.h`
- `util/util_uint64.h`
