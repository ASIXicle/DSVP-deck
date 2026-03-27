# DSVP Steam Deck — Build from Source

Step-by-step instructions for building DSVP natively on the Steam Deck. The portable tarball produced by this process is self-contained — once built, none of the dev tools are needed to run it.

**Why build natively?** SteamOS is Arch-based. Binaries built on Debian link against a different glibc/toolchain and suffer catastrophic performance loss (90%+ frame drops) or won't launch at all on the Deck. Building on the Deck itself produces binaries linked against the correct runtime.

**Time estimate:** About 30–45 minutes for a clean build of everything.

## Prerequisites

- SSH access to the Deck, or a keyboard connected in Desktop Mode
- Internet access on the Deck
- Deck password set (run `passwd` in Konsole if not done)

## Phase 1 — Unlock SteamOS and Install Dev Tools

SteamOS has a read-only root filesystem. Dev tools must be installed via `pacman`, which requires unlocking it. These packages will be wiped on the next SteamOS update, but that's fine — the build output in `~/` persists.

```bash
sudo steamos-readonly disable
sudo pacman-key --init
sudo pacman-key --populate archlinux holo
sudo pacman -Syu --noconfirm
```

Install the build toolchain:

```bash
sudo pacman -S --needed --noconfirm \
    base-devel glibc linux-api-headers \
    git cmake nasm yasm pkg-config zlib
```

SteamOS ships runtime `.so` files but strips headers and `.pc` files. Reinstall these to restore dev headers:

```bash
sudo pacman -S --noconfirm \
    libx11 libxext libxrandr libxcursor libxi libxfixes libxss \
    xorgproto libxcb libxrender \
    libpulse alsa-lib \
    wayland wayland-protocols \
    libdrm mesa vulkan-headers vulkan-icd-loader \
    libdecor pipewire libpipewire \
    libglvnd libxkbcommon libsystemd dbus \
    freetype2 harfbuzz libffi

sudo pacman -S --noconfirm \
    zlib bzip2 libpng brotli glib2 graphite pcre2 libsysprof-capture

sudo pacman -S --noconfirm \
    libva libva-utils
    
sudo pacman -S --noconfirm \
    dav1d
```

Verify everything is in place:

```bash
ls /usr/include/ctype.h
ls /usr/include/X11/X.h
pkg-config --modversion egl libpipewire-0.3 xkbcommon dbus-1 libva
vainfo 2>&1 | grep -i hevc
```

The `vainfo` output should show `VAProfileHEVCMain` and `VAProfileHEVCMain10` with `VAEntrypointVLD`.

If any package fails with key trust errors: `sudo pacman -S --noconfirm holo-keyring archlinux-keyring`

## Phase 2 — Build FFmpeg 8.1

Build into a local prefix in your home directory. The `--enable-vaapi` flag is critical for HEVC hardware decode.

```bash
cd ~
wget https://ffmpeg.org/releases/ffmpeg-8.1.tar.xz
tar xf ffmpeg-8.1.tar.xz
cd ffmpeg-8.1

./configure \
    --prefix=$HOME/ffmpeg-8.1-local \
    --disable-programs \
    --disable-doc \
    --disable-encoders \
    --disable-muxers \
    --enable-shared \
    --disable-static \
    --enable-vaapi \
    --enable-libdav1d
```

Check the configure output for `vaapi: yes`. If it says `no`, the `libva` headers aren't installed — go back to Phase 1.

```bash
make -j4
make install
```

Verify:

```bash
PKG_CONFIG_PATH=$HOME/ffmpeg-8.1-local/lib/pkgconfig pkg-config --modversion libavcodec
# Should show 62.28.100
```

## Phase 3 — Build SDL3 and SDL3_ttf

SteamOS doesn't ship SDL3 — build from source.

### SDL3

```bash
cd ~
git clone --depth 1 --branch release-3.4.2 https://github.com/libsdl-org/SDL.git SDL3-src
cd SDL3-src
mkdir build && cd build

cmake .. \
    -DCMAKE_INSTALL_PREFIX=$HOME/sdl3-local \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_VULKAN=ON \
    -DSDL_WAYLAND=ON \
    -DSDL_X11=ON \
    -DSDL_PIPEWIRE=ON \
    -DSDL_PULSEAUDIO=ON \
    -DSDL_ALSA=ON
```

Before running `make`, check the cmake summary: `SDL_WAYLAND`, `SDL_PIPEWIRE`, `SDL_DBUS`, `SDL_X11`, and `SDL_VULKAN` should all be ON. If any are OFF, a header or `.pc` file is missing — go back to Phase 1.

```bash
make -j4
make install
```

### SDL3_ttf

```bash
cd ~
git clone --depth 1 --branch release-3.2.2 https://github.com/libsdl-org/SDL_ttf.git SDL3_ttf-src
cd SDL3_ttf-src
mkdir build && cd build

cmake .. \
    -DCMAKE_INSTALL_PREFIX=$HOME/sdl3-local \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL3_DIR=$HOME/sdl3-local/lib/cmake/SDL3

make -j4
make install
```

## Phase 4 — Clone DSVP and Build

```bash
cd ~
git clone https://github.com/ASIXicle/DSVP.git DSVP-build
cd DSVP-build
git checkout steamdeck

export PKG_CONFIG_PATH=$HOME/ffmpeg-8.1-local/lib/pkgconfig:$HOME/sdl3-local/lib/pkgconfig:$PKG_CONFIG_PATH

# Verify all deps are found
pkg-config --modversion libavcodec libavformat sdl3 sdl3-ttf
# Should show: 62.28.100 / 62.12.100 / 3.4.2 / 3.2.2

make clean && make
```

SDL3_shadercross is bundled in the repo at `shadercross/SDL3_shadercross-3.0.0-linux-x64/` — the Makefile handles it automatically.

## Phase 5 — Package and Deploy

```bash
cd ~/DSVP-build
export LD_LIBRARY_PATH=$HOME/ffmpeg-8.1-local/lib:$HOME/sdl3-local/lib:$LD_LIBRARY_PATH
./package.sh
rm -f DSVP-portable/dsvp.log
rm -rf ~/DSVP-old && mv ~/DSVP ~/DSVP-old && mv DSVP-portable ~/DSVP
```

Test (must test directly on Deck docked to your display, not via SSH):

```bash
cd ~/DSVP
./dsvp.sh /path/to/video.mkv
```

After playback, check the log:

```bash
grep -E "VAAPI|version|Frames dropped|Peak A/V|bias" dsvp.log
```

You should see `VAAPI: active — HEVC hardware decode, P010 output` for HEVC content.

To force software decode for comparison: `DSVP_HWDEC=0 ./dsvp.sh /path/to/video.mkv`

## Phase 6 — Re-Lock Filesystem

```bash
sudo steamos-readonly enable
```

Dev tools in `/usr` will be wiped on the next SteamOS update. Everything in `/home/deck/` persists.

## Rebuilding After Code Changes

Once the dependencies are built, rebuilding DSVP after a code change is fast:

```bash
cd ~/DSVP-build
git pull
export PKG_CONFIG_PATH=$HOME/ffmpeg-8.1-local/lib/pkgconfig:$HOME/sdl3-local/lib/pkgconfig:$PKG_CONFIG_PATH
make clean && make
export LD_LIBRARY_PATH=$HOME/ffmpeg-8.1-local/lib:$HOME/sdl3-local/lib:$LD_LIBRARY_PATH
./package.sh
rm -f DSVP-portable/dsvp.log
rm -rf ~/DSVP-old && mv ~/DSVP ~/DSVP-old && mv DSVP-portable ~/DSVP
```

## Filesystem Layout

```
~/ffmpeg-8.1-local/     — Native FFmpeg 8.1 with VAAPI (headers, libs, pkg-config)
~/sdl3-local/           — Native SDL3 3.4.2 + SDL3_ttf 3.2.2
~/DSVP-build/           — Source checkout (steamdeck branch)
~/DSVP/                 — Deployed portable build (binary + bundled libs)
~/DSVP-old/             — Previous build (backup)
```

Source trees (`~/SDL3-src/`, `~/SDL3_ttf-src/`, `~/ffmpeg-8.1/`) can be deleted after building to free space.

## Troubleshooting

**`steamos-readonly disable` fails:** Try `sudo steamos-devmode enable` instead.

**FFmpeg configure says "Compiler lacks C11 support":** Not actually a compiler issue — glibc dev headers are missing. Reinstall `glibc` and `linux-api-headers` via pacman.

**FFmpeg configure shows `vaapi: no`:** The `libva` dev headers aren't installed. Run `sudo pacman -S --noconfirm libva libva-utils` and re-run configure.

**SDL3 cmake shows Wayland/Pipewire OFF:** Missing `.pc` files. Reinstall: `libpipewire`, `libglvnd`, `libxkbcommon`, `libsystemd`.

**SDL3 build can't find headers (`X11/X.h`, `libdecor.h`, `xcb/xcb.h`):** Install `xorgproto`, `libffi` (for libdecor transitive dep), `libxcb`, `libxrender`. Then nuke the cmake build dir (`rm -rf build && mkdir build && cd build`) and re-run cmake.

**SDL3_ttf cmake errors on freetype2/harfbuzz:** Transitive `.pc` chain broken. Install: `zlib`, `bzip2`, `libpng`, `brotli`, `glib2`, `graphite`, `pcre2`, `libsysprof-capture`.

**General "header not found" on SteamOS:** SteamOS aggressively strips dev files. Fix is almost always `sudo pacman -S --noconfirm <package>`. Use `pacman -F <filename>` to find which package owns a missing file.

**cmake cached broken state:** If cmake ran while `.pc` files were missing, it caches the bad result. Fix: `rm -rf build && mkdir build && cd build` and re-run cmake.

**Binary runs but drops 90% of frames:** Check thread count. Use `DSVP_THREADS=N` env var to test different values. See the README for the adaptive heuristic.

## Debug Build

```bash
make debug
```

Enables GPU validation layers, console output, verbose FFmpeg logging, and debug symbols. A `dsvp.log` file is written to the working directory.
