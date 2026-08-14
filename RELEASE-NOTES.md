# DSVP-deck 0.3.7-beta

The biggest update since launch. Since 0.2.7: real HDR output, PipeWire-native
bitstream passthrough, a rebuilt frame-pacing engine, multi-cue subtitles,
FFmpeg 9.0, and a top-to-bottom review-and-fix cycle shipped in
field-verified batches.

## HDR, played as HDR

- **True HDR output on an HDR display** — per-file HDR10/ST2084 swapchain
  carries the content's PQ/BT.2020 code values untouched; the display does
  the tone mapping with its own hardware. DSVP engages and reverts the
  display's HDR mode automatically per file — the desktop never leaves SDR,
  and there is nothing to configure.
- **Z** toggles passthrough ↔ player tone-map live for an instant A/B
  against your TV (your choice sticks for the session).
- Dolby Vision Profile 5 (per-frame RPU, polynomial + MMR reshaping) and
  Profile 8; HLG converts in-shader and rides the same paths.
- The HDR→SDR tone map (BT.2390, scene-adaptive peak detection) remains the
  fallback and internal-panel path, with live SDR-target (**T**), midtone
  gain (**G**), and output-transfer (**E**) controls.
- HDR10 static metadata is parsed, sanitized, and staged for the compositor;
  the final metadata-to-display hop lights up as SteamOS support matures.
  Every HDR session logs the actual wire infoframe (`HDRWIRE:`).

## Bitstream passthrough (PipeWire-native)

- **AC3 and DTS pass through to your TV/AVR end-to-end** — no setup, no
  root, no config. Sink capability is probed via ELD; passthrough survives
  seeks and restarts automatically on audio-track switches.
- **P** toggles passthrough/PCM live; the OSD shows the active audio mode.
- E-AC3, TrueHD, and DTS-HD MA currently decode to PCM: SteamOS cannot yet
  carry high-bitrate IEC to the display. That limitation is upstream, and
  DSVP falls back cleanly until it lifts.

## Frame pacing, rebuilt

- Two-mode engine driven by a measured display heartbeat: vsync-locked at
  true 1:1, slot-assignment scheduling everywhere else. Drops and repeats
  are planned and evenly spaced instead of threshold-reactive, so A/V sync
  holds without stutter storms — including windowed, where compositors
  deliver fewer slots than the content needs.
- Audio is never resampled, stretched, or paused for pacing.

## Subtitles

- **Multi-cue text subtitles** — up to 4 overlapping cues display at once,
  stacked broadcast-style (signs + dialogue, second speakers). SRT and
  ASS/SSA.
- PGS robustness: zlib-compressed tracks decode correctly, END-stripped
  MKV muxes display instead of accumulating silently, and seek storms with
  subtitles enabled are clean.

## Under the hood

- FFmpeg 9.0, SDL3 3.4.14 (with the bundled HDR-metadata patch),
  SDL3_shadercross 3.0.0.

## Install

Extract the tarball in Desktop Mode and run `dsvp.sh` — see the bundled
`README.txt` and `SteamOS.md` for Game Mode setup, gamepad mapping, and
USB/SD drive notes. No root, no installer; survives SteamOS updates.

## Known limitations

- E-AC3 / TrueHD / DTS-HD MA decode to PCM (SteamOS IEC limitation — not
  the player).
- Windowed 60fps content drops frames structurally; fullscreen plays clean.
- Media on a network share freezes if the network drops (kernel-level;
  every player behaves this way).
- The Z toggle can take a few seconds on a TV — that's the TV's own HDR
  mode switch.
- If fullscreen ever freezes while audio keeps playing: see the README's
  Troubleshooting section (wedged TV/display state — power-cycle the TV at
  the wall).
