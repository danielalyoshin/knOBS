# Product

<!-- impeccable:product-schema 1 -->

## Platform

windows

## Users

Windows users who want a processed mic (noise suppression, gate, expander, compressor, limiter) in every app they use it in. Two groups, both primary:

- **OBS users with a tuned chain.** Streamers, podcasters and gamers, many with an XLR mic, who already tuned a filter chain in OBS and route it to a virtual cable. Today they keep OBS open all day to do that. They know OBS and its filters.
- **People new to OBS.** People who install OBS only to tune a mic for knobs. They don't know OBS's filters or how to build a chain, so knobs has to explain that setup.

The job for both: tune the mic once, get that sound in every app, and stop thinking about it.

## Product Purpose

knobs runs the mic filter chain from the user's OBS settings and sends the result to a virtual audio cable, with no OBS process. The audio goes through the user's own installed OBS filter code.

Success (measured in docs/design.md, Evidence): with OBS closed and knobs in the tray, the mic sounds identical to OBS in every app. A cold boot gives a working filtered mic with zero clicks, opening OBS while knobs runs never doubles the audio, and idle use is well below OBS minimized.

## Positioning

knobs gives OBS's exact mic sound without OBS. It loads the filter binaries from the user's own OBS install and recreates the mic with OBS's own loader, so its output is bit-identical to OBS's, on test signals and on real voice. Equalizer APO, VoiceMeeter, NVIDIA Broadcast and NoiseTorch use different DSP. OBS plugins such as atkAudio still need OBS open.

## Operating Context

- Lives in the Windows tray with no window by default, and starts with Windows.
- Needs OBS Studio installed (32.2.x for now) and a virtual cable such as VB-Cable. Other apps pick the cable as their mic.
- Users tune filters in OBS. knobs reads the active OBS profile and scene collection and never changes them.
- Pauses while OBS runs and resumes when it exits, so the cable never gets the mic twice.
- Found through GitHub, the OBS forums and r/obs.

## Capabilities and Constraints

- Built: loading libobs from a private copy of the OBS install, importing the mic and its filters, the live path into the cable, the comparison with OBS, and the tray app with its first run, notifications and icon badges. Released on GitHub; open work is tracked in GitHub issues.
- No custom DSP and no "improvements". No filter editing in v1, since users tune in OBS. No VST filters in v1. Windows only in v1.
- Plain Win32 UI with no UI framework. A small exe that ships no libobs. Target: under 1% CPU and a small memory footprint.
- Terms: mic, filter chain, virtual cable (or cable), OBS profile, scene collection, monitoring device.
- The tray menu, first run and tray icon are designed in docs/design.md (The tray app).
- Distribution: an installer (for the current user, no admin rights) and a portable zip that keeps its data beside the exe, from GitHub releases. Releases are signed with Azure Artifact Signing. SmartScreen may still warn on first run until a new release has built up reputation.

## Brand Commitments

- **Name:** "knobs", all lowercase, even at the start of a sentence, as in the audio kind. It was knOBS until 2026-10-05. Code keeps it in one constant (`KNOBS_DISPLAY_NAME`), and user-facing strings use that constant.
- **Tagline:** "Your OBS mic chain, in every app.", the headline of getknobs.app. The README, the About box and the GitHub repository's description use it (decided 2026-10-08, replacing "Your OBS mic chain, without OBS.").
- **Voice:** plain and exact, like the README. Say what happened and what to do, in everyday words. Short sentences, no hype, no exclamation marks, no jokes. Give numbers when they help ("88 ms").
- **Logo:** the knob mark (a dark knob with a red pointer, original artwork), the app icon, and the horizontal and stacked lockups, in `assets/`. Don't recolor, rotate or stretch the knob. Usage rules are in `assets/README.md`.
- **Logo colors:** ink `#17181b` on light and `#e8eaee` on dark. Red `#d63c42` on light and `#e5484d` on dark, for the pointer and "obs" in the wordmark. The knob's ticks are `#e8eaee`, a light tone of the knob's cool graphite. No cream (decided 2026-10-05). The app icon is the knob alone, with no tile (decided 2026-10-05).
- **Red "obs" in the wordmark:** kept on purpose (decided 2026-10-05), although the rename to knobs dropped "OBS" from the name. If the OBS team objects, revisit it, as with the name.
- **Tray badges:** discs in the logo's colors: a white "!" on the red `#e5484d` and white pause bars on the graphite `#4a4c52`, 7/16 of the icon (decided 2026-10-07, replacing Windows 11's yellow and gray).
- **Logo license:** the files in `assets/` are all rights reserved, outside the GPL that covers the code. `TRADEMARKS.md` says how the name and logo may be used; forks need their own (decided 2026-10-07).
- **Independence:** knobs isn't affiliated with or endorsed by the OBS Project. Say so wherever knobs is presented.

## Evidence on Hand

- knobs's output is bit-identical to OBS's on test signals and on a recording of real voice (docs/design.md, Same output as OBS).
- Mic to cable: 88.1 ms in knobs, 87.7 ms in OBS, on the same cable input (Audient iD4; docs/design.md, Mic-to-cable latency).
- On real hardware (docs/design.md, On real hardware): running, knobs used 0.72% of a core and 36 MB, against OBS minimized at 5.64% and 469 MB. After a cold boot, the filtered mic was on the cable 9.5 s after signing in, with nothing clicked.
- [obs-studio#12650](https://github.com/obsproject/obs-studio/issues/12650): since OBS 32.0 the Safe Mode prompt blocks OBS from running unattended as a background mic processor, which is this product's use case.
- The logo, app icon and lockups in `assets/`.
- Pictures of the tray menu, first run, notifications and icon: `knobs-tray --screenshot` and `--icons` take them on its fake core, whose state is made up but realistic.
- Not on hand, and not to be made up: users, testimonials, download counts, a website.

## Product Principles

1. **Fidelity is the product.** The sound is OBS's, bit for bit. Nothing in knobs changes the audio, and the app never offers to "enhance" it.
2. **Invisible while it works.** Tray only, starts with Windows, nothing to look at once it's set up.
3. **Loud when it breaks.** When the mic, the cable or OBS goes missing, say so in a notification that says what to do. Never fail silently.
4. **OBS is the editor.** Tuning happens in OBS. knobs imports the chain, and for people new to OBS it explains how to set one up there rather than building one itself.
5. **Leave OBS alone.** Never change OBS's settings or install. Make way while OBS runs instead of competing with it.
