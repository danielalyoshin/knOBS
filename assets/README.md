# Logo

The knob mark, the app icon and the horizontal lockup. The SVGs are the masters. The wordmark in them is outlined, so they need no fonts.

- `knobs-mark.svg`: the mark alone.
- `knobs-app-icon.svg`: the mark on a rounded tile.
- `knobs.ico`: the app icon for Windows, at 16, 20, 24, 32, 40, 48, 64 and 256 px, for the tray app's exe and tray icon. Each size is `knobs-app-icon.svg` rendered at that size in headless Edge. Sizes below 256 are 32-bit bitmaps, and 256 is a PNG.
- `knobs-lockup-horizontal-dark.svg`: mark and wordmark with dark text, for light backgrounds.
- `knobs-lockup-horizontal-light.svg`: mark and wordmark with light text, for dark backgrounds.

## Colors

| | On light backgrounds | On dark backgrounds |
|---|---|---|
| "kn" in the wordmark | `#17181b` | `#e8eaee` |
| "obs" in the wordmark | `#d63c42` | `#e5484d` |

The knob is the same everywhere: a dark body, a `#e5484d` pointer and `#e8eaee` ticks. The app icon's tile is `#e8eaee` too. That light tone is the knob's own cool graphite gray, lightened; it replaced the cream (`#f2f0eb`, `#efebe3`, `#ebe8e1`) on 2026-10-05.

## Rules

- Keep clear space around the mark at least as long as its pointer.
- Don't show the mark smaller than 32 px. The icon's 16 px size is the exception.
- Don't recolor, rotate or stretch the knob.
- The wordmark is Sora: "kn" in SemiBold 600 and "obs" in Bold 700, tracked −0.055em.
- The mark is original artwork. Where it appears next to the OBS name or logo, check the OBS Project's trademark guidelines.
