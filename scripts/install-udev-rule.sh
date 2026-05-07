#!/bin/bash
# install-udev-rule.sh — DSVP boot-time IEC 61937 codec arm
#
# DSVP needs the HDA codec's IEC 61937 non-audio bit set so the TV/AVR
# can distinguish compressed (Dolby Digital Plus, TrueHD, DTS-HD, Atmos)
# audio from PCM. Setting that bit requires writing an HDA verb to
# /dev/snd/hwC*D0, which is owned by root:audio with mode 0660.
#
# Asking DSVP itself to grab those permissions at runtime would mean
# users running it as root or joining the audio group — friction that
# breaks the "Dead Simple" promise and prevents Flatpak distribution.
#
# Instead, we install a small helper binary (dsvp-arm-iec958) and a
# udev rule that fires the helper whenever an HDA hwdep device appears
# (boot) or changes (HDMI hotplug, dock connect/disconnect). The helper
# walks the codec, identifies digital audio-output converter widgets,
# and arms each one. The codec retains the bit across snd_pcm_open and
# stream close, so DSVP itself runs unprivileged and just opens hw:N,M.
#
# Survives:
#   - reboots (rule lives in /etc/udev, helper in /usr/local/sbin)
#   - HDMI hotplug (udev re-fires on change events)
#   - SteamOS atomic updates (/etc and /usr/local persist)
#
# What it changes on the system:
#   - Installs /usr/local/sbin/dsvp-arm-iec958
#   - Installs /etc/udev/rules.d/70-dsvp-hda-hwdep.rules
#   - Installs /etc/systemd/system-sleep/dsvp-arm-iec958 (resume hook)
#
# To uninstall:
#   sudo rm /usr/local/sbin/dsvp-arm-iec958
#   sudo rm /etc/udev/rules.d/70-dsvp-hda-hwdep.rules
#   sudo rm /etc/systemd/system-sleep/dsvp-arm-iec958
#   sudo udevadm control --reload-rules
#
# Note: uninstalling does NOT clear the non-audio bit from the codec —
# the bit persists until codec re-init. After uninstall, reboot OR run:
#   systemctl --user restart pipewire pipewire-pulse wireplumber
# to drop the bit and let HDMI audio return to normal PCM mode.

set -e

if [ "$EUID" -ne 0 ]; then
    echo "This script needs root to install the helper and udev rule."
    echo "Run with sudo:  sudo $0"
    exit 1
fi

# Locate the build artifact
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
HELPER_BIN="$PROJECT_DIR/build/dsvp-arm-iec958"

if [ ! -x "$HELPER_BIN" ]; then
    echo "ERROR: helper binary not found at $HELPER_BIN"
    echo "Build DSVP first:  cd $PROJECT_DIR && make"
    exit 1
fi

HELPER_DEST=/usr/local/sbin/dsvp-arm-iec958
RULE_FILE=/etc/udev/rules.d/70-dsvp-hda-hwdep.rules
SLEEP_HOOK=/etc/systemd/system-sleep/dsvp-arm-iec958

# 1. Install the helper binary
install -m 0755 -o root -g root "$HELPER_BIN" "$HELPER_DEST"
echo "Installed $HELPER_DEST"

# 2. Install the udev rule
cat > "$RULE_FILE" <<'EOF'
# DSVP — arm IEC 61937 non-audio bit on HDA codec at boot / HDMI hotplug.
# The helper walks every digital-output converter on each HDA codec
# present and arms the HBR-capable converter (--nid 0x06 on Steam Deck
# OLED Rembrandt) with DIGI_CONVERT_1 = DIGEN | V | NAUDIO. The codec
# retains the state across snd_pcm_open, so DSVP runs unprivileged
# afterward.
#
# Why no %n arg: %n's parse semantics for sound devices vary across
# udev versions. The helper handles "no args → walk all cards" cleanly,
# and arming is fast enough (microseconds per card) that scanning
# every card on every event is fine.
#
# Why --nid 0x06: only the HBR-capable converter (PCM device 8 on
# Steam Deck Rembrandt) needs the non-audio bit set. The other digital
# converters carry desktop HDMI audio (Plasma routes through PCM
# device 3 = NID 0x02 by default) and must stay in PCM mode so the TV
# decodes them correctly.
KERNEL=="hwC*D0", SUBSYSTEM=="sound", ACTION=="add|change", \
    RUN+="/usr/local/sbin/dsvp-arm-iec958 --nid 0x06"
EOF
echo "Installed $RULE_FILE"

# 3. Install the suspend/resume hook
#    Linux power management re-initializes HDA codecs on resume,
#    which resets the non-audio bit. Re-arm whenever the system wakes.
mkdir -p "$(dirname "$SLEEP_HOOK")"
cat > "$SLEEP_HOOK" <<'EOF'
#!/bin/sh
# DSVP — re-arm IEC 61937 non-audio bit after system resume.
# systemd calls this script with $1 = "pre"|"post" and $2 = "suspend"|"hibernate"|...
# We only act on the "post" call (after resume), and only for sleep states.
# --nid 0x06 matches the udev rule: arm only the HBR converter, leave the
# other HDMI converters in PCM mode for desktop audio.
case "$1" in
    post)
        case "$2" in
            suspend|hibernate|hybrid-sleep|suspend-then-hibernate)
                /usr/local/sbin/dsvp-arm-iec958 --nid 0x06 >/dev/null 2>&1 || true
                ;;
        esac
        ;;
esac
EOF
chmod 0755 "$SLEEP_HOOK"
echo "Installed $SLEEP_HOOK"

# 4. Reload udev rules and trigger immediately so the bit gets set right now,
#    not just at the next boot.
udevadm control --reload-rules
udevadm trigger --action=change --subsystem-match=sound

echo
echo "Installation complete. The codec is being armed now and will be"
echo "re-armed automatically at boot, on HDMI hotplug, and after sleep."
echo
echo "To verify it worked, look for the non-audio bit on the HBR converter:"
echo "  cat /proc/asound/card0/codec#0 | grep -A3 'Node 0x06'"
echo "  # Look for: 'Digital: Enabled Validity Non-Audio'"
echo
echo "Other HDMI converters (NIDs 0x02, 0x04, 0x08) should still show"
echo "plain 'Digital: Enabled' — they carry desktop HDMI audio in PCM mode."
echo
echo "If you don't see 'Validity Non-Audio' on Node 0x06, run the helper"
echo "manually with verbose output:"
echo "  sudo $HELPER_DEST --nid 0x06 -v"
