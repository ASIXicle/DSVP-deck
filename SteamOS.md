# DSVP on SteamOS (Steam Deck)

DSVP runs on SteamOS desktop mode via a portable tarball — no developer mode, no pacman, no root access required. Everything lives in your home directory and survives SteamOS updates.

Tested: 4K 60fps software/VAAPI decode, Vulkan, zero dropped frames on LG OLED via official dock at 4K 4:4:4 60Hz.

## Quick Start

**1. Download** the latest `DSVP-*-linux-x64.tar.gz` from [Releases](https://github.com/ASIXicle/DSVP/releases/).

**2. Extract and install.** Switch to Desktop Mode, open Konsole, and run:

```bash
cd ~
tar xzf ~/Downloads/DSVP-*-linux-x64.tar.gz
mv DSVP-portable DSVP
chmod +x DSVP/dsvp DSVP/dsvp.sh
```

**3. Run:**

```bash
~/DSVP/dsvp.sh                         # idle window, press O to open file
~/DSVP/dsvp.sh /path/to/movie.mkv      # open directly
```

## Add to Desktop App Menu

To make DSVP show up in KDE's application launcher:

```bash
mkdir -p ~/.local/share/applications
cat > ~/.local/share/applications/dsvp.desktop << 'EOF'
[Desktop Entry]
Name=DSVP
Comment=Dead Simple Video Player
Exec=/home/deck/DSVP/dsvp.sh %f
Icon=video-player
Terminal=false
Type=Application
Categories=AudioVideo;Video;Player;
MimeType=video/x-matroska;video/mp4;video/avi;video/webm;video/x-msvideo;video/quicktime;video/x-flv;video/ogg;video/mpeg;
EOF
```

DSVP will appear under Multimedia in the app menu. You can also right-click video files → Open With → DSVP.

## Add as Non-Steam Game (Game Mode)

1. In Desktop Mode, open Steam
2. Games → Add a Non-Steam Game to My Library
3. Click Browse, navigate to `/home/deck/DSVP/`
4. Select `dsvp.sh`, click Add
5. Right-click the entry in your library → Properties
6. Set Launch Options to the path of a video file, e.g.: `/home/deck/Videos/movie.mkv`

You can now launch DSVP from Game Mode with controller input.

## Display Settings

For best results with DSVP's quality pipeline:

- **Chroma mode:** Set your TV/monitor to 4:4:4 (or RGB Full). DSVP's Catmull-Rom chroma upscaling reconstructs full-resolution color from the source — 4:4:4 output preserves this work all the way to the panel.
- **Refresh rate:** 60Hz is ideal for film and most video content. 4K 4:4:4 at 60Hz uses nearly the full bandwidth of HDMI 2.0, so higher refresh rates may require dropping to 4:2:2.
- **Resolution:** The Deck outputs 4K over the official dock. DSVP handles upscaling with Lanczos luma and Catmull-Rom chroma in its GPU shaders.

## Notes

- **No root required.** The entire install lives in `~/DSVP/`. Nothing touches the system partition.
- **Survives updates.** SteamOS wipes system packages on every update, but `/home/deck/` is untouched.
- **Built on Debian, runs on SteamOS.** The portable tarball bundles all shared libraries. No dependencies to install.
- **Vulkan only.** DSVP forces Vulkan via `SDL_SetHint`. The Steam Deck's AMD APU supports this natively.

## Controls

| Key | Action |
| --- | --- |
| `O` | Open file |
| `Q` | Quit / close current file |
| `Space` | Pause / resume |
| `F` / double-click | Toggle fullscreen |
| `S` | Cycle subtitle tracks |
| `A` | Cycle audio tracks |
| `←` / `→` | Seek ±5 seconds |
| `↑` / `↓` | Volume up / down |
| `B` / `N` | Previous / next file in folder |
| `D` | Debug overlay |
| `I` | Media info overlay |

## Troubleshooting

**"error while loading shared libraries"** — Make sure you're running `./dsvp.sh`, not `./dsvp` directly. The launcher script sets `LD_LIBRARY_PATH` to find the bundled libraries.

**Black screen or no video** — Press `D` to check the debug overlay. Verify Vulkan is working: install `vulkan-tools` via Discover (flatpak) and run `vulkaninfo`.

**No audio** — SteamOS desktop mode uses PipeWire. DSVP outputs via SDL3's audio backend which supports PipeWire natively. Check that your output device is set correctly in System Settings → Sound.

**No file dialog** — The file-open dialog (`O` key) uses `zenity`, which is included in SteamOS desktop mode. If it's missing, install via Discover or just pass files as command-line arguments.
