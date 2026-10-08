# Logo

The knob mark, the app icon and the lockups. The SVGs are the masters. The wordmark in them is outlined, so they need no fonts.

- `knobs-mark.svg`: the mark alone.
- `knobs-app-icon.svg`: the app icon: the mark alone, scaled up to fill a square. It has no tile (decided 2026-10-05).
- `knobs.ico`: the app icon for Windows, for the tray app's exe, its tray icon, its windows and its notifications. It has the small (16 px) and large (32 px) icons at every display scale from 100% to 300%: 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80 and 96 px, and 256. `tools/render-icon.ps1 -Svg assets\knobs-app-icon.svg -Out assets\knobs.ico` makes it: headless Edge renders the SVG at 8 times each size, and each 8×8 block is averaged into a pixel, for smooth edges. Sizes below 256 are 32-bit bitmaps, and 256 is a PNG.
- `knobs-lockup-horizontal-dark.svg`: mark and wordmark with dark text, for light backgrounds.
- `knobs-lockup-horizontal-light.svg`: mark and wordmark with light text, for dark backgrounds.
- `knobs-lockup-stacked-dark.svg` and `knobs-lockup-stacked-light.svg`: the mark above the wordmark, for tall spaces such as the installer's side panel. The wordmark is 1.3 times the knob's width, 24 units below it.
- `installer/`: the installer's images, at each size Inno Setup asks for from 100% to 250% display scaling. `wizard-<width>.png` is the stacked lockup centered on `#e8eaee`, for the "Completing setup" page, and `small-<size>.png` the app icon on a transparent ground, top right on the other pages. `tools/render-installer-images.ps1` makes them from the SVGs, as `render-icon.ps1` makes `knobs.ico`.

## Colors

| | On light backgrounds | On dark backgrounds |
|---|---|---|
| "kn" in the wordmark | `#17181b` | `#e8eaee` |
| "obs" in the wordmark | `#d63c42` | `#e5484d` |

The knob is the same everywhere: a dark body, a `#e5484d` pointer and `#e8eaee` ticks. That light tone is the knob's own cool graphite gray, lightened; it replaced the cream (`#f2f0eb`, `#efebe3`, `#ebe8e1`) on 2026-10-05.

## Rules

- Keep clear space around the mark at least as long as its pointer. The app icon is the exception: it fills its square.
- Don't show the mark smaller than 32 px. The app icon's smaller sizes are the exception.
- Don't recolor, rotate or stretch the knob.
- In the horizontal lockup, the wordmark is about 73% of the knob's height, centered on it, and starts 66 units right of the knob, as on getknobs.app (decided 2026-10-08, up from 60%).
- The wordmark is Sora: "kn" in SemiBold 600 and "obs" in Bold 700, tracked −0.055em.
- The mark is original artwork. Where it appears next to the OBS name or logo, check the OBS Project's trademark guidelines.

## License

These files aren't under the GPL like the rest of knobs. They're Copyright © 2026 Daniel Alyoshin, all rights reserved, apart from what [TRADEMARKS.md](../TRADEMARKS.md) allows: sharing them unmodified with the knobs source and its official builds, and using them unmodified to refer to knobs. A fork that you distribute needs its own icon in place of `knobs.ico`.
