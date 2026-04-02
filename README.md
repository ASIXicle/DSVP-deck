# DSVP_deck — Dead Simple Video Player for the Steam Deck

<img alt="DSVPmenu" src="docs/DSVPmenu.png" />

A reference-quality video player purpose-built for the Steam Deck. Plug-and-play from Game Mode — no configs, no root, no reformatting your USB drives.

DSVP sits between VLC and mpv: better quality than VLC (modern FFmpeg, Lanczos scaling, temporal dithering, BT.2390 tone mapping), simpler than mpv (zero config, gamepad-native, best quality out of the box). It plays anything FFmpeg supports.

There are portable Windows & Debian builds on the [main branch](https://github.com/ASIXicle/DSVP/tree/main). Steam Deck builds are on this branch — download the latest tarball from [Releases](https://github.com/ASIXicle/DSVP/releases/tag/v0.2.0-beta-steamdeck). 

HDR is Coming Soon. Two weeks, tops. Maybe three.

File Explorer:

<img alt="DSVP_file_explorer" src="docs/DSVP_file_explorer.png" />

---

![Steam Deck](https://img.shields.io/badge/Steam_Deck-Game_Mode_+_Desktop-green)
![Linux](https://img.shields.io/badge/Linux-supported-blue)

## Highlights

- **USB / SD card auto-mount in Game Mode** — Plug in any NTFS, exFAT, or ext4 USB drive and it just works. DSVP auto-mounts via `udisksctl` (no root, no reformatting). Your drive appears as `[USB]` in the file browser.
- **Gamepad-native** — Full controller support: d-pad navigation, analog trigger seek (0–64× quadratic curve), **Start** for controls overlay, A/B/X/Y mapped to core functions. Works identically in Game Mode and Desktop Mode. Keyboard also supported.
- **Integrated file browser** — No external file dialogs. Navigate your files with d-pad or keyboard, rapid-scroll with held d-pad, page through directories. Stays within `/home/deck/` to keep things simple — external drives injected at the top level.
- **Reference-quality playback** — Lanczos-2 luma (anti-ringing clamp), Catmull-Rom chroma (siting-corrected), temporal blue noise dithering, faithful color/gamma/framerate. Image quality is paramount.
- **VAAPI zero-copy** — HEVC decoded on the APU's VCN hardware, imported directly into Vulkan via DMA-BUF. Zero sustained drops on 4K content.
- **Game Mode + Desktop Mode** — Auto-detects environment, scales OSD (3× in Game Mode), 16:10 crop-to-fill in Game Mode, standard letterboxing in Desktop Mode.

## Features

- **HDR→SDR tone mapping** — BT.2390 EETF with dynamic scene-adaptive peak detection (99.875th percentile histogram, asymmetric temporal smoothing), adjustable SDR target (203/300/400 nits) and midtone gain (1.0–1.4)
- **Dolby Vision** — Profile 5 decode with per-frame RPU updates and piecewise polynomial reshaping; Profile 8 falls through to standard HDR10 path
- **10-bit passthrough** — YUV420P10LE uploads as R16_UNORM planar textures, no truncation, no swscale
- **VAAPI hardware decode** — HEVC on the Deck's VCN engine (bit-exact P010 output), software decode for everything else
- **Supports everything FFmpeg supports** — H.264, HEVC, AV1, VP9, VC-1, MKV, MP4, Webm, and hundreds more
- **Adaptive thread tuning** — Per-codec/per-file thread selection optimized for the Deck's 4C/8T Zen 2
- **Full subtitle support** — Text (SRT, ASS/SSA), bitmap (PGS, VobSub), CJK fallback fonts, golden yellow with black outline
- **Auto-play** — Sequential playback through folder contents
- **Portable** — Single folder, no installer, no root, survives SteamOS updates
- **Secure** — No networking capabilities whatsoever

## Controls

| Key            | Gamepad          | Function                                |
| -------------- | ---------------- | --------------------------------------- |
| O              | A (idle)         | Open integrated file browser            |
| Q              | B                | Close file / Quit (returns to browser)  |
| Space          | X                | Pause / resume                          |
| ← / →          | LB / RB          | Seek ±5 seconds                         |
| —              | LT / RT          | Analog seek (0–64× quadratic curve)     |
| ↑ / ↓ (play)   | D-pad U/D        | Volume                                  |
| ↑ / ↓ (browse) | D-pad U/D        | Navigate (hold for rapid scroll)        |
| ← / → (browse) | D-pad L/R        | Page up / Page down                     |
| Enter          | A (browse)       | Open file / Enter directory             |
| Backspace      | B (browse)       | Go up directory                         |
| B / N          | D-pad L/R (play) | Prev / Next file                        |
| S              | Y                | Cycle subtitle tracks                   |
| A-key          | R3               | Cycle audio tracks                      |
| D              | Back/Select      | Debug overlay                           |
| —              | Start            | Controls overlay (toggle)               |
| —              | L3               | Transport control mode (toggle)         |
| H              | —                | Cycle HDR debug views                   |
| T              | —                | Cycle SDR target nits (203 / 300 / 400) |
| G              | —                | Cycle midtone gain (1.0–1.4)            |
| V              | —                | Toggle VSync / Mailbox                  |

## Installing on Steam Deck

See [SteamOS.md](SteamOS.md) for download, install, desktop/Game Mode setup, and display configuration.

## Building from Source on Steam Deck

See [SETUP.md](SETUP.md) for the full build-from-source walkthrough. The short version:

SteamOS has a read-only root filesystem and ships no development headers. Building from source requires unlocking the filesystem, installing dev tools via `pacman`, and building FFmpeg 8.1, SDL3, and SDL3_ttf from source into `~/` prefixes. The resulting portable tarball is self-contained and runs without any of the dev tools installed.

### Requirements

- **SteamOS** with filesystem unlocked (`sudo steamos-readonly disable`)
- **base-devel** (gcc, make, pkg-config) via `pacman`
- **FFmpeg 8.1** built from source with `--enable-vaapi`
- **SDL3 3.4.2** built from source
- **SDL3_ttf 3.2.2** built from source
- **SDL3_shadercross 3.0.0** (bundled in repo — not available via package managers)
- **libva + libva-utils** for VAAPI hardware decode
- **zlib** (for PGS subtitle decompression)

### Quick Build

```bash
cd ~/DSVP-build
git checkout steamdeck
export PKG_CONFIG_PATH=$HOME/ffmpeg-8.1-local/lib/pkgconfig:$HOME/sdl3-local/lib/pkgconfig:$PKG_CONFIG_PATH
make clean && make
export LD_LIBRARY_PATH=$HOME/ffmpeg-8.1-local/lib:$HOME/sdl3-local/lib:$LD_LIBRARY_PATH
./package.sh
rm -f DSVP-portable/dsvp.log
rm -rf ~/DSVP-old && mv ~/DSVP ~/DSVP-old && mv DSVP-portable ~/DSVP
```

## Project Structure

```
DSVP/
  src/
    dsvp.h       ← Central state struct, GPU uniforms, constants, declarations
    main.c       ← SDL init, event loop, frame pacing, hotkey handling, gamepad input
    player.c     ← Demux thread, video decode/display, GPU pipelines, HLSL shaders, VAAPI, seeking
    audio.c      ← Audio decode, resample, SDL3 audio stream, A/V clock, track cycling
    subtitle.c   ← Subtitle detection, decode, SDL3_ttf rendering, CJK fallback fonts
    overlay.c    ← GPU-composited overlays: bitmap font, seek bar, debug/info panels, OSD, controls overlay
    browser.c    ← Integrated file browser, USB/SD auto-mount, path persistence, NFS timeout
    log.c        ← Crash-safe unbuffered file logger
  Makefile       ← Build (sources from src/, output in build/)
  package.sh     ← Linux packaging script
```

## Technical Details

<img alt="DSVP_example" src="docs/DSVP_example.png" />

DSVP uses a custom GPU rendering pipeline built on SDL_GPU with HLSL shaders cross-compiled to SPIR-V via SDL3_shadercross 3.0.0. The fragment shader performs Lanczos-2 resampling on luma (16-tap windowed sinc with anti-ringing clamp at 0.8), Catmull-Rom bicubic interpolation on chroma (16-tap with sub-texel siting correction), limited→full range expansion, BT.601/BT.709/BT.2020 color matrix conversion, and temporal blue noise dithering (64×64 void-and-cluster texture, per-frame offset) — all in a single pass. YUV420P and YUV420P10LE formats bypass `swscale` entirely; raw decoded planes upload directly to GPU textures.

For HDR10 content, the shader applies PQ EOTF, BT.2390 tone mapping with scene-adaptive dynamic peak detection (CPU-side histogram scan with asymmetric temporal smoothing), BT.2020→BT.709 gamut mapping, and configurable midtone gain. Dolby Vision Profile 5 content goes through per-frame RPU-driven piecewise polynomial reshaping before tone mapping. Profile 8 uses the standard HDR10 path via its backward-compatible base layer.

HEVC content on the Steam Deck uses VAAPI hardware decode via the APU's VCN engine. The zero-copy path imports VAAPI surfaces as Vulkan images via DMA-BUF interop (`VK_KHR_external_memory_fd`), eliminating GPU readback entirely for P010 content. Semi-planar UV is handled in-shader (`is_semiplanar` uniform). Any zero-copy failure falls back to CPU readback transparently. H.264 and AV1 content remains software decoded. The GPU backend is Vulkan-only on this branch.

USB and SD card auto-mount works via `udisksctl`, which is present on every SteamOS install and requires no root access. On browser open, DSVP scans `/dev/disk/by-id/` for unmounted USB and MMC partitions, mounts them to `/run/media/deck/<label>`, and injects them into the file browser as `[USB]` entries. NTFS, exFAT, and ext4 filesystems are all supported — users can plug in a drive straight from a Windows PC without reformatting.

Audio is the master clock with adaptive bias correction (EMA α=0.05) for OS audio pipeline latency. At 1:1 content/display framerate (≥50fps), VSync is the sole pacing source with frame drops and delay correction bypassed.

## Debug Build

```bash
make debug
```

Enables GPU validation layers, console output, verbose FFmpeg logging, and debug symbols. A `dsvp.log` file is written to the working directory.

## Environment Variables

| Variable | Effect |
|---|---|
| `DSVP_THREADS=N` | Override adaptive thread count (0 = FFmpeg auto) |
| `DSVP_HWDEC=0` | Disable VAAPI hardware decode, force software |

## AI Disclosure

Built with the assistance of Claude Opus 4.6 (Anthropic)

## License

GPL v3 — see [LICENSE](LICENSE).

A commercial license is available for proprietary use — see [COMMERCIAL_LICENSE.md](COMMERCIAL_LICENSE.md).
