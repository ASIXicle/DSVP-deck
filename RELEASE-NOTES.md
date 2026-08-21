# DSVP-deck 0.3.8-beta

## ⚠️ KNOWN ISSUE — READ THIS FIRST ⚠️

**On current SteamOS, KWin (the compositor) can freeze fullscreen SDR
playback when Desktop Mode is NOT in HDR mode.** Playback continues
underneath — audio runs, seeking works — but the screen shows a frozen
frame. This is a compositor bug, not a DSVP bug; we have root-caused
it and are waiting on a SteamOS/KWin update.

**Any ONE of these avoids it entirely:**

1. Enable **HDR for Desktop Mode** (Display settings) — recommended;
   DSVP is tuned for this configuration.
2. Launch with `DSVP_FS_HDR_FALLBACK=1`.
3. Play **windowed** — unaffected.

---

## Why you should update anyway: a display-safety fix

**0.3.7-beta could permanently disable your display's wide color gamut
setting** after HDR playback — surviving reboots, and looking like a
broken TV. 0.3.8 fixes this completely: DSVP now reads display state
before ever writing it, restores **exactly** what it found on exit,
and — new — if the player is killed mid-playback, the next launch
detects the interrupted session and restores your display's baseline
automatically. If 0.3.7 already did this to you: re-enable wide gamut
once in Display settings (or play one HDR file in 0.3.8 and exit).

## Performance

- **60fps playback overhauled.** A month-long pacing investigation
  closed: dropped frames on 60fps content reduced from ~10% to ~0.1%
  in the tested fullscreen configurations. 4K60 holds locked cadence.
- **4K HDR (HEVC) decode path**: the Vulkan zero-copy route now caches
  its per-frame GPU imports (~48 kernel-side DMA-BUF imports/second
  eliminated) and synchronizes on its own copies instead of draining
  the shared GPU queue.
- Debug overlay no longer perturbs playback while open (it previously
  cost enough to cause drops on the tightest paths — it now updates at
  10Hz and uploads only on change).

## Picture quality

- SDR on a wide-gamut/HDR desktop now rides a properly encoded
  PQ container with LUT-exact encoding (sqrt-domain, verified
  numerically end to end).
- Chroma anti-ringing now applies whenever the output container is
  PQ — previously HDR-only, leaving SDR-in-PQ content with visible
  edge color bleed. (Restore old behavior: `DSVP_CHROMA_AR=hdr`.)
- Exact 2× upscales use a constant-weight Lanczos-2 path —
  bit-comparable output, a fraction of the GPU cost.
- The rare software-scaling fallback (8-bit non-4:2:0 sources) now
  preserves source range exactly instead of a lossy integer stretch.
- Error-diffusion dithering in format conversion is now actually
  requested (a mislabeled constant selected "auto" before).

## Reliability

- Closed a class of total-playback-freeze bugs where losing the audio
  device (dock bump, TV input switch) mid-session wedged the pipeline
  — all six call sites now degrade to video-only with an on-screen
  notice.
- Passthrough capability detection now asks PipeWire (the component
  that actually enforces it) instead of scanning `/proc` — also
  groundwork for a future Flatpak.
- Container format whitelist: a file's contents can no longer select
  an unexpected demuxer (e.g. a renamed playlist).
- Crash-restore state is validated on read and never placed in
  world-writable locations.
- `DSVP_LOG_ANON=1` now redacts every file path in the log (it
  previously missed most of them), so logs are safe to share.

## Debug overlay (D)

Rebuilt as a trustworthy instrument: shows the build hash, the
sampler kernel actually bound, live pacing state and cadence,
zero-copy cache statistics, drop percentage (same formula as the
close-out summary), and honest audio-output state during passthrough.

## Known issues (besides the banner above)

- Windowed 60fps has a compositor-side frame-rate ceiling
  (~54–58 fps) under investigation.
- Seeking in long-GOP (large keyframe interval) files can show a
  brief burst of catch-up jerk after the seek lands.

---
*DSVP — Dead Simple Video Player. Reference-grade playback, no
settings maze.*
