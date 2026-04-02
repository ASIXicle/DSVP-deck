# DSVP on SteamOS (Steam Deck)

DSVP runs natively in both Game Mode and Desktop Mode via a portable tarball — no developer mode, no pacman, no root access required. Everything lives in your home directory and survives SteamOS updates.

Tested: 4K 60fps VAAPI hardware decode + zero-copy, Vulkan, zero sustained drops on Steam Deck OLED via official dock at 4K 4:4:4 60Hz.

## Quick Start

**1. Download** the latest Steam Deck tarball from the [steamdeck Release](https://github.com/ASIXicle/DSVP/releases/tag/v0.2.0-beta-steamdeck).

**2. Extract and install.** Switch to Desktop Mode, open Konsole, and run:

```bash
cd ~
tar xzf ~/Downloads/DSVP-*-steamdeck.tar.gz
mv DSVP-portable DSVP
chmod +x DSVP/dsvp DSVP/dsvp.sh
```

**3. Run:**

```bash
~/DSVP/dsvp.sh
```

DSVP opens to the integrated file browser. Navigate with keyboard or gamepad and select a file to play.

## Add as Non-Steam Game (Game Mode)

1. In Desktop Mode, open Steam
2. Games → Add a Non-Steam Game to My Library
3. Click Browse, navigate to `/home/deck/DSVP/`
4. Select `dsvp.sh`, click Add

That's it. Launch DSVP from Game Mode and use the gamepad to browse files and control playback. No launch options needed — the integrated file browser handles everything.

DSVP auto-detects Game Mode vs Desktop Mode and adapts: OSD scales 3× in Game Mode, 16:10 crop-to-fill activates, and the menubar hides.

## USB / SD Card Drives

**Just plug them in.** DSVP auto-mounts USB and SD card drives in Game Mode via `udisksctl` — no root, no reformatting. NTFS, exFAT, and ext4 filesystems all work. Your drive appears as `[USB]` at the top of the file browser.

This means you can copy files onto a USB drive from a Windows PC and plug it directly into the Deck's dock. No need to format as ext4.

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

## VAAPI Hardware Decode

DSVP automatically uses VAAPI hardware decode for HEVC content on the Steam Deck. This offloads the decode from the CPU to the APU's VCN engine, which is critical for 4K HEVC 10-bit content that the Deck's Zen 2 can't sustain in software. H.264 and AV1 content stays software decoded (both play perfectly at 4K 60fps).

The zero-copy path imports VAAPI surfaces directly into Vulkan via DMA-BUF interop, eliminating GPU readback entirely. Any zero-copy failure falls back to CPU readback transparently.

VAAPI decode is bit-exact — identical output to software decode, no quality compromise. You can verify it's active by pressing `D` (debug overlay) during playback.

To force software decode for comparison: `DSVP_HWDEC=0 ~/DSVP/dsvp.sh`

## Display Settings

For best results with DSVP's quality pipeline:

- **Chroma mode:** Set your TV/monitor to 4:4:4 (or RGB Full). DSVP's Catmull-Rom chroma upscaling reconstructs full-resolution color from the source — 4:4:4 output preserves this work all the way to the panel.
- **Refresh rate:** 60Hz is ideal for film and most video content. 4K 4:4:4 at 60Hz uses nearly the full bandwidth of HDMI 2.0, so higher refresh rates may require dropping to 4:2:2.
- **Resolution:** The Deck outputs 4K over the official dock. DSVP handles upscaling with Lanczos luma and Catmull-Rom chroma in its GPU shaders.

## Controls

| Key | Gamepad | Function |
|-----|---------|----------|
| O | A (idle) | Open integrated file browser |
| Q | B | Close file / Quit (returns to browser) |
| Space | X | Pause / resume |
| ← / → | LB / RB | Seek ±5 seconds |
| — | LT / RT | Analog seek (0–64× quadratic curve) |
| ↑ / ↓ (play) | D-pad U/D | Volume |
| ↑ / ↓ (browse) | D-pad U/D | Navigate (hold for rapid scroll) |
| ← / → (browse) | D-pad L/R | Page up / Page down |
| Enter | A (browse) | Open file / Enter directory |
| Backspace | B (browse) | Go up directory |
| B / N | D-pad L/R (play) | Prev / Next file |
| S | Y | Cycle subtitle tracks |
| A-key | R3 | Cycle audio tracks |
| D | Back/Select | Debug overlay |
| — | Start | Controls overlay (toggle) |
| H | — | Cycle HDR debug views |
| T | — | Cycle SDR target nits (203 / 300 / 400) |
| G | — | Cycle midtone gain (1.0–1.4) |
| V | — | Toggle VSync / Mailbox |

## Environment Variables

| Variable | Effect |
| --- | --- |
| `DSVP_THREADS=N` | Override adaptive thread count (0 = FFmpeg auto) |
| `DSVP_HWDEC=0` | Disable VAAPI hardware decode, force software |

## Notes

- **No root required.** The entire install lives in `~/DSVP/`. Nothing touches the system partition.
- **Survives updates.** SteamOS wipes system packages on every update, but `/home/deck/` is untouched.
- **Built natively on SteamOS.** The portable tarball bundles all shared libraries compiled on the Deck's Arch-based toolchain. No cross-build compatibility issues.
- **Vulkan only.** DSVP forces Vulkan via `SDL_SetHint`. The Steam Deck's AMD APU supports this natively.

## Troubleshooting

**"error while loading shared libraries"** — Make sure you're running `./dsvp.sh`, not `./dsvp` directly. The launcher script sets `LD_LIBRARY_PATH` to find the bundled libraries.

**Black screen or no video** — Press `D` to check the debug overlay. Verify Vulkan is working: install `vulkan-tools` via Discover (flatpak) and run `vulkaninfo`.

**No audio** — SteamOS desktop mode uses PipeWire. DSVP outputs via SDL3's audio backend which supports PipeWire natively. Check that your output device is set correctly in System Settings → Sound.

**USB drive not showing up** — Check `dsvp.log` for `browser: automount:` lines. If you see `udisksctl` errors, the drive's filesystem may not be supported. NTFS, exFAT, and ext4 are all supported. If the drive doesn't appear at all in the log, it may not be connected — try a different USB port on your dock.

**HEVC content dropping frames** — Press `D` to check if VAAPI is active. If the debug overlay shows "Decoder Threads: N" instead of "Decode: VAAPI (hardware)", VAAPI isn't engaged. This likely means FFmpeg was built without `--enable-vaapi`. See [SETUP.md](SETUP.md) for rebuild instructions.
