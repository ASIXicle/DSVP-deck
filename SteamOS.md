# DSVP on SteamOS (Steam Deck)

DSVP runs natively in both Game Mode and Desktop Mode via a portable tarball — no developer mode, no pacman, no root access required. Everything lives in your home directory and survives SteamOS updates.

Tested: 4K 60fps VAAPI hardware decode + zero-copy, Vulkan, zero sustained drops on Steam Deck OLED via official dock at 4K 4:4:4 60Hz.

## Quick Start

**1. Download** the latest Steam Deck tarball from the [Releases](https://github.com/ASIXicle/DSVP-deck/releases).

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
| P | — | Toggle bitstream passthrough / PCM decode |
| H | — | Cycle HDR debug views *(HDR content only)* |
| T | — | Cycle SDR target nits *(HDR content only: 203 / 300 / 400)* |
| G | — | Cycle midtone gain *(HDR content only: 1.0–1.4)* |
| E | — | Cycle output gamma *(HDR content only: sRGB / 2.2 / 2.4)* |
| Z | — | Toggle HDR passthrough / SDR tone-map *(HDR content, HDR display path)* |

**DSVP never changes your display mode.** Fullscreen is always borderless — it
reuses whatever mode the desktop is already running, so the display link is
never renegotiated and the player cannot alter your screen's mode, placement or
handshake state. An older build had an "exclusive" fullscreen path that issued a
real mode change; it was removed because on some docks and adapters the link
does not reliably come back, which could leave the picture offset or the screen
blank. Nothing you press in DSVP can put your display into that state.

`H`, `T` and `G` are deliberately inert on SDR content — there is nothing to tone map, so they do nothing rather than inventing an adjustment.

## Environment Variables

| Variable | Effect |
| --- | --- |
| `DSVP_THREADS=N` | Override adaptive thread count (0 = FFmpeg auto) |
| `DSVP_HWDEC=0` | Disable VAAPI hardware decode, force software |
| `DSVP_PCM=1` | Force PCM decode at startup, never attempt bitstream passthrough |
| `DSVP_AUDIO_DELAY=<ms>` | Audio-latency offset for A/V sync in passthrough — positive if your TV/soundbar delays audio relative to video |
| `DSVP_HD_PASSTHROUGH=1` | Re-enable EAC3/TrueHD passthrough attempts (currently platform-blocked — see Notes; retest after SteamOS updates) |
| `DSVP_LOG_ANON=1` | Redact file paths from `dsvp.log` before sharing it |

None of these are required for normal use — DSVP picks the correct behaviour on its own. They exist for diagnosis and A/B comparison.

## Notes

- **No root required.** The entire install lives in `~/DSVP/`. Nothing touches the system partition.
- **Survives updates.** SteamOS wipes system packages on every update, but `/home/deck/` is untouched.
- **Built natively on SteamOS.** The portable tarball bundles all shared libraries compiled on the Deck's Arch-based toolchain. No cross-build compatibility issues.
- **Vulkan only.** DSVP forces Vulkan via `SDL_SetHint`. The Steam Deck's AMD APU supports this natively.
- **Audio passthrough works out of the box.** Dolby Digital (AC3) and DTS
  bitstream to your TV/receiver with zero setup — DSVP talks to PipeWire
  like any other app; no root, no scripts, nothing system-wide. Dolby
  Digital Plus (EAC3) and TrueHD/Atmos are currently **not deliverable on
  the Deck at all**: SteamOS/the dock cannot carry >48 kHz compressed
  audio to the display (this also affects Kodi and mpv — it is a platform
  issue, not a player setting). Those tracks decode to PCM automatically,
  so they always play with sound. When a SteamOS update improves this,
  one run with `DSVP_HD_PASSTHROUGH=1` re-tests it.

## Troubleshooting

**"error while loading shared libraries"** — Make sure you're running `./dsvp.sh`, not `./dsvp` directly. The launcher script sets `LD_LIBRARY_PATH` to find the bundled libraries.

**Black screen or no video** — Press `D` to check the debug overlay. Verify Vulkan is working: install `vulkan-tools` via Discover (flatpak) and run `vulkaninfo`.

**No audio** — SteamOS desktop mode uses PipeWire. DSVP outputs via SDL3's audio backend which supports PipeWire natively. Check that your output device is set correctly in System Settings → Sound.

**System-wide audio dead after a bitstream/passthrough attempt** —
Current DSVP builds pass audio through PipeWire like any other
application: the audio server is never bypassed, never asked to yield
a device, and never restarted, so DSVP cannot take system audio down.
Two recoveries kept for older builds or forced experiments:

1. *PipeWire's HDMI sink died* (older ALSA-direct builds could leave
   this after yielding the device):
   `systemctl --user restart pipewire pipewire-pulse wireplumber`
   — all three; wireplumber alone is not sufficient.
2. *The TV's audio decoder wedged* (some sinks — LG OLEDs confirmed —
   latch into mute if fed compressed audio they cannot lock onto,
   e.g. after forcing `DSVP_HD_PASSTHROUGH=1`, and stay muted
   through every software restart). Recover by forcing an HDMI link
   retrain: unplug/replug the HDMI cable at the dock, or toggle the
   TV to another input and back.

**Picture and UI cut off at the screen edges (overscan)** — if menu bars,
seek bar or the edges of the picture are missing on a TV, the TV is almost
certainly scaling the signal rather than displaying it 1:1. Confirm it in ten
seconds: this affects the *whole desktop*, not just DSVP, so check whether your
file manager's toolbar buttons and the taskbar are also clipped. If they are,
no player setting can fix it.

Measure it exactly with a test pattern — red box at the true screen edge, green
at 2.5% in, cyan at 5% in:

```bash
ffmpeg -f lavfi -i "color=black:s=3840x2160" \
  -vf "drawbox=x=0:y=0:w=3840:h=2160:color=red@1:t=6,\
drawbox=x=96:y=54:w=3648:h=2052:color=lime@1:t=6,\
drawbox=x=192:y=108:w=3456:h=1944:color=cyan@1:t=6" \
  -frames:v 1 -y ~/overscan-test.png
```

Open it fullscreen in an image viewer. The outermost box you can see fully is
your overscan figure. Losing red but keeping green is the classic 2.5%
broadcast overscan.

Fixes are on the TV, in rough order of likelihood:

- **Label the input as a PC.** On LG (webOS): Home → the input row → highlight
  the HDMI input → Edit → set the *icon* to **PC**. The icon is what switches
  the panel to 1:1 mapping; renaming the input does nothing.
- **Enable the input's full-bandwidth mode** — LG calls it *HDMI Ultra HD Deep
  Colour* (Settings → General → External Devices → HDMI Settings), per port. It
  needs a fresh handshake, so re-select the input afterwards.
- **Turn off the zoom directly** — Picture → Aspect Ratio → **Just Scan** (or
  *Original*). If Just Scan is greyed out, the input is not being treated as a
  PC source yet; fix the two items above first.
- **Try a different HDMI port.** These settings are per-input on most TVs, and
  ports are not always equivalent.

**Do not "fix" this with the desktop's overscan compensation** (KDE's
`kscreen-doctor output.<name>.overscan.N`). It works by rendering into a
smaller area and letting the driver scale the result, so every pixel is
resampled on its way to the panel. It will make your desktop usable and it will
end any chance of bit-exact playback — which is the entire point of this
player. If the TV insists on scaling, that input cannot deliver reference
output no matter what DSVP does.

**USB drive not showing up** — Check `dsvp.log` for `browser: automount:` lines. If you see `udisksctl` errors, the drive's filesystem may not be supported. NTFS, exFAT, and ext4 are all supported. If the drive doesn't appear at all in the log, it may not be connected — try a different USB port on your dock.

**HEVC content dropping frames** — Press `D` to check if VAAPI is active. If the debug overlay shows "Decoder Threads: N" instead of "Decode: VAAPI (hardware)", VAAPI isn't engaged. This likely means FFmpeg was built without `--enable-vaapi`. See [SETUP.md](SETUP.md) for rebuild instructions.
