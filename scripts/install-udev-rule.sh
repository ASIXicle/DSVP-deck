#!/bin/bash
# install-udev-rule.sh — DSVP HDA hwdep access for IEC 61937 passthrough
#
# DSVP needs to write the IEC 61937 non-audio bit to the HDA codec via
# /dev/snd/hwC<N>D0. By default this device is owned by root:audio with
# mode 0660 — only root or members of the audio group can write to it.
# On SteamOS the default user (deck) is NOT in the audio group, so DSVP
# can't access the device without help.
#
# This script installs a udev rule that adds an ACL grant for the deck
# user on /dev/snd/hwC*D0 so DSVP can write IEC 61937 channel-status
# verbs without requiring the user to be in the audio group.
#
# Run once after installing DSVP. Persists across reboots. Survives
# SteamOS atomic updates IF run from /etc (not /usr); we install to
# /etc/udev/rules.d/ which is on the persistent partition.

set -e

if [ "$EUID" -ne 0 ]; then
    echo "Run with sudo:  sudo $0"
    exit 1
fi

RULE_FILE=/etc/udev/rules.d/70-dsvp-hda-hwdep.rules

cat > "$RULE_FILE" <<'EOF'
# DSVP — grant the active console user RW access to HDA hwdep devices
# so DSVP can issue SET_DIGI_CONVERT_1 verbs to enable IEC 61937
# (Dolby Digital Plus, TrueHD, DTS-HD) passthrough on HDMI.
KERNEL=="hwC*D0", SUBSYSTEM=="sound", TAG+="uaccess"
EOF

echo "Wrote $RULE_FILE"

# Reload udev rules and trigger
udevadm control --reload-rules
udevadm trigger --subsystem-match=sound

echo
echo "Verify access (should show ACL with deck user):"
echo "  getfacl /dev/snd/hwC0D0"
echo
echo "If DSVP still can't open the device, log out and back in once to"
echo "let logind re-evaluate the uaccess tag, or reboot."
