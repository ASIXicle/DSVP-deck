# DSVP_deck — Dead Simple Video Player optimized for the Steam Deck


<img alt="DSVPmenu" src="docs/DSVPmenu.png" />

WHY? Because I can. And education. And I'm a config-fiddler that wanted to offer a mpv-style player without configs or intimidation factor. Think of DSVP as a middle-man between VLC and mpv. It's not as SOTA as mpv but should be more "user-friendly". Or less, if you don't have a  keyboard. Should offer better quality than VLC as it uses more modern FFmpeg libraries. It *should* play anything you throw at it.

There are portable Windows & Debian (and eventually Steam Deck) builds you can download and try [HERE](https://github.com/ASIXicle/DSVP/releases/).

REQUIRES Visual C++ Redistributable runtime (vcruntime140.dll). It's probably already on your PC but you can get it here:
https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170

Claude wrote most of this:

---


![Windows](https://img.shields.io/badge/Windows-supported-blue)
![Linux](https://img.shields.io/badge/Linux-supported-blue)
![macOS](https://img.shields.io/badge/macOS-untested-yellow)

## Features

- **Reference-quality playback** — Lanczos-2 luma scaling (anti-ringing clamp), Catmull-Rom chroma upsampling (siting-corrected), temporal blue noise dithering, faithful color/gamma/framerate
- **10-bit passthrough** — YUV420P10LE content uploads as R16_UNORM planar textures with no truncation
- **Supports everything FFmpeg supports** — H.264, HEVC, AV1, VP9, VC-1, MKV, MP4, and hundreds more
- **Multi-threaded decoding** — uses all available CPU cores
- **Full subtitle support** — text (SRT, ASS/SSA), bitmap (PGS, VobSub), CJK fallback fonts, golden yellow with black outline, cycle tracks with `S`
- **Folder navigation** — `B`/`N` keys to jump between media files in the current folder, with clickable prev/next buttons
- **Portable** — single folder, no installer, no PATH changes
- **Secure** — no networking capabilities whatsoever
- **Cross-platform** — Vulkan on Windows/Linux, Metal on macOS

## Controls

| Key | Action |
|---|---|
| `O` | Open file |
| `Q` | Quit / close current file |
| `Space` | Pause / resume |
| `F` / double-click | Toggle fullscreen |
| `S` | Cycle subtitle tracks (off → track 1 → track 2 → off) |
| `A` | Cycle audio tracks |
| `←` / `→` | Seek ±5 seconds |
| `↑` / `↓` | Volume up / down |
| `B` / `N` | Previous / next file in folder |
| `D` | Toggle debug overlay |
| `I` | Toggle media info overlay |

## Building from Source

### Requirements

- **GCC** (MSYS2 MinGW64 on Windows, gcc on Linux, clang on macOS)
- **FFmpeg 8.0+** shared development libraries
- **SDL3** development libraries
- **SDL3_ttf** development libraries
- **SDL3_shadercross 3.0.0** (bundled — not available via package managers)
- **zlib** (for PGS subtitle decompression)
- **GNU Make**
- **pkg-config**

### Steam Deck Build Instructions:

This needs to be filled out


## Project Structure

```
DSVP/
  src/
    dsvp.h       ← Central state struct, GPU uniforms, constants, declarations
    main.c       ← SDL init, event loop, frame pacing, hotkey handling
    player.c     ← Demux thread, video decode/display, GPU pipelines, HLSL shaders, seeking, media info
    audio.c      ← Audio decode, resample, SDL3 audio stream, A/V clock, track cycling
    subtitle.c   ← Subtitle detection, decode, SDL3_ttf rendering, CJK fallback fonts
    overlay.c    ← GPU-composited overlays: bitmap font, seek bar, debug/info panels, OSD, subtitles
    log.c        ← Crash-safe unbuffered file logger
  Makefile       ← Cross-platform build (sources from src/, output in build/)
  package.ps1    ← Windows portable packaging script
  package.sh     ← Linux/macOS packaging script
```

## Technical Details

DSVP uses a custom GPU rendering pipeline built on SDL_GPU with HLSL shaders cross-compiled to SPIR-V via SDL3_shadercross 3.0.0. The fragment shader performs Lanczos-2 resampling on luma (16-tap windowed sinc with anti-ringing clamp at 0.8), Catmull-Rom bicubic interpolation on chroma (16-tap with sub-texel siting correction), limited→full range expansion, BT.601/BT.709 color matrix conversion, and temporal blue noise dithering (64×64 void-and-cluster texture, per-frame offset) — all in a single pass. YUV420P and YUV420P10LE formats bypass `swscale` entirely; raw decoded planes upload directly to GPU textures.

ADD VAAPI FUNTION FOR HEVC ON DECK (bit perfect, akin to software decode)

The GPU backend is Vulkan on Windows and Linux, Metal on macOS (untested). Audio is the master clock with adaptive bias correction (EMA α=0.05) for OS audio pipeline latency. At 1:1 content/display framerate (≥50fps), VSync is the sole pacing source with frame drops and delay correction bypassed.

## Debug Build

```bash
make debug          # Deck
```

Enables GPU validation layers, console output, verbose FFmpeg logging, and debug symbols. A `dsvp.log` file is written to the working directory.

## License

GPL v3 — see [LICENSE](LICENSE).

A commercial license is available for proprietary use — see [COMMERCIAL_LICENSE.md](COMMERCIAL_LICENSE.md).
