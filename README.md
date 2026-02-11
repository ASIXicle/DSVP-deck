# DSVP — Dead Simple Video Player


<img width="1069" height="695" alt="DSVPmenu" src="https://github.com/user-attachments/assets/e831c7b9-53ca-436a-83ed-7109307e4329" />


WIP, this 1.2

WHY? Because I can. And education. And I want to offer a mpv-style player without configs or intimidation-factor. Think of DSVP as a middle-man between VLC and mpv. It's not as SOTA as mpv but should be more "user-friendly". Perhaps not as much so as VLC but should offer better quality than VLC as it uses more modern FFmpeg libraries. It *should* play anything you throw at it.

There is a portable Windows build you can download and try [HERE](https://github.com/ASIXicle/DSVP/releases/)

REQUIRES Visual C++ Redistributable runtime (vcruntime140.dll). It's probably already on your PC but you can get it here:
https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170

Claude wrote most of this stuff:

A minimalist, reference-quality video player. Software decode only. No networking. No nonsense.


![Windows](https://img.shields.io/badge/Windows-supported-blue)
![Linux](https://img.shields.io/badge/Linux-supported-blue)
![macOS](https://img.shields.io/badge/macOS-supported-blue)

## Features

- **Reference-quality playback** — Lanczos scaling, error-diffusion dithering, faithful color/gamma/framerate adherence
- **Software decode only** — no hardware acceleration, no driver quirks, bit-exact output
- **Supports everything FFmpeg supports** — H.264, HEVC, AV1, VP9, MKV, MP4, and hundreds more
- **Multi-threaded decoding** — uses all available CPU cores
- **Minimal interface** — overlays appear on mouse activity, auto-hide after 3 seconds
- **Subtitle support** — golden yellow text with black outline, cycle tracks with `S` key (SRT, ASS/SSA, WebVTT, MOV text)
- **Audio track switching** — cycle audio tracks with `A` key (commentary, alternate languages, etc.)
- **Portable** — single folder, no installer, no PATH changes
- **Secure** — no networking capabilities whatsoever

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
| `D` | Toggle debug overlay |
| `I` | Toggle media info overlay |

## Building from Source

### Requirements

- **GCC** (MinGW on Windows, gcc/clang on Linux/macOS)
- **FFmpeg 6.0+** shared development libraries
- **SDL2 2.28+** development libraries
- **SDL2_ttf 2.20+** for subtitle rendering
- **GNU Make**

### Windows (MinGW)

**1. Install build tools** (via [Scoop](https://scoop.sh)):
```powershell
scoop install gcc make
```

**2. Download dependencies:**

- FFmpeg shared build: [gyan.dev/ffmpeg/builds](https://www.gyan.dev/ffmpeg/builds/) → `ffmpeg-release-full-shared.7z`
- SDL2 MinGW dev: [github.com/libsdl-org/SDL/releases](https://github.com/libsdl-org/SDL/releases) → `SDL2-devel-x.xx.x-mingw.zip`
- SDL2_ttf MinGW dev: [github.com/libsdl-org/SDL_ttf/releases](https://github.com/libsdl-org/SDL_ttf/releases) → `SDL2_ttf-devel-x.xx.x-mingw.zip`

**3. Place in `deps/`:**
```
deps/
  ffmpeg/
    bin/      ← DLLs
    include/  ← headers
    lib/      ← import libraries
  SDL2/
    bin/      ← SDL2.dll
    include/  ← SDL2/ headers
    lib/      ← libSDL2.a, libSDL2main.a
  SDL2_ttf/
    bin/      ← SDL2_ttf.dll
    include/  ← SDL2/ headers (SDL_ttf.h)
    lib/      ← libSDL2_ttf.a
```

**4. Build:**
```powershell
mingw32-make
```

**5. Package for distribution:**
```powershell
.\package.ps1
```

This creates a `DSVP-portable/` folder with the exe and all required DLLs.

### Linux

```bash
# Debian/Ubuntu
sudo apt install gcc make libavformat-dev libavcodec-dev libswscale-dev \
    libswresample-dev libavutil-dev libsdl2-dev libsdl2-ttf-dev \
    fonts-dejavu-core zenity

make
./package.sh
```

### macOS

```bash
brew install ffmpeg sdl2 sdl2_ttf pkg-config
make
./package.sh
```

## Project Structure

```
DSVP/
  src/
    dsvp.h      ← shared types, constants, declarations
    main.c      ← SDL init, event loop, overlays, file dialog
    player.c    ← demux, video decode, display, seeking, info
    audio.c     ← audio decode, resample, SDL audio callback
    subtitle.c  ← subtitle decode, TTF rendering, track cycling
    log.c       ← crash-safe file logger
  Makefile      ← cross-platform build
  package.ps1   ← Windows packaging script
  package.sh    ← Linux/macOS packaging script
```

## Debug Build

```powershell
mingw32-make debug
```

This enables console output, verbose FFmpeg logging, and debug symbols. A `dsvp.log` file is written to the working directory with timestamped events.

## License

MIT — see [LICENSE](LICENSE).
