#!/bin/bash
# DSVP Portable Packaging Script (Linux / Steam Deck)
# Creates a self-contained DSVP-portable/ folder with binary + shared libs.
#
# Usage:
#   ./package.sh
#   ./package.sh --skip-build   (reuse existing build/dsvp — it must have
#                                been built with `make PORTABLE=1`; the
#                                rpath check below enforces this)

set -e

# Linux-only. The macOS branch was unreachable dead code that had already
# diverged (review Q-14) — deleted rather than maintained.
if [ "$(uname)" != "Linux" ]; then
    echo "ERROR: package.sh is Linux-only."
    exit 1
fi

# Single source of truth: DSVP_VERSION in src/dsvp.h. Hardcoding it here
# drifted 7 releases behind (0.2.0 vs 0.2.7) before anyone noticed.
VERSION="$(sed -n 's/^#define DSVP_VERSION *"\(.*\)".*/\1/p' src/dsvp.h)"
[ -n "$VERSION" ] || { echo "ERROR: cannot read DSVP_VERSION from src/dsvp.h"; exit 1; }
OUTDIR="DSVP-portable"
SKIP_BUILD=0

# Builder lib prefixes — must mirror the Makefile defaults (override via
# env like the Makefile's ?= vars). The ldd walk resolves against these
# EXPLICITLY rather than through the binary's rpath: the portable binary
# carries no rpath at all, and putting SDL3_LOCAL first means the patched
# SDL always beats shadercross's vanilla libSDL3 copy (review P2-23).
SDL3_LOCAL="${SDL3_LOCAL:-/home/deck/sdl3-local}"
FFMPEG_LOCAL="${FFMPEG_LOCAL:-/home/deck/ffmpeg-9.0-local}"
SC_LIB="shadercross/SDL3_shadercross-3.0.0-linux-x64/lib"
BUILDER_LIBS="$SDL3_LOCAL/lib:$FFMPEG_LOCAL/lib:$PWD/$SC_LIB"

if [ "$1" = "--skip-build" ]; then
    SKIP_BUILD=1
fi

echo "=== DSVP Packager v${VERSION} ==="

# ── Build ──────────────────────────────────────────────────────────

if [ "$SKIP_BUILD" -eq 0 ]; then
    echo -e "\n[1/6] Building (PORTABLE=1, no rpath)..."
    make clean 2>/dev/null || true
    make PORTABLE=1
    echo "      Build OK"
else
    echo -e "\n[1/6] Skipping build"
fi

# ── Verify binary ─────────────────────────────────────────────────

if [ ! -f "build/dsvp" ]; then
    echo "ERROR: build/dsvp not found."
    exit 1
fi

# A baked rpath would leak builder paths into the shipped binary AND let
# the closure check below pass on this machine while the bundle fails on
# a user's — missing libs would resolve through the builder's prefixes
# (review P1-8 + P2-24). Refuse to package such a binary.
if readelf -d build/dsvp | grep -qE '\(RPATH\)|\(RUNPATH\)'; then
    echo "ERROR: build/dsvp carries an rpath — not a portable build."
    echo "       Rebuild with: make clean && make PORTABLE=1"
    echo "       (--skip-build reusing a dev binary?)"
    exit 1
fi

# ── Create output directory ────────────────────────────────────────

echo "[2/6] Creating ${OUTDIR}/"
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR/lib"

# ── Copy binary ───────────────────────────────────────────────────

echo "[3/6] Copying binary..."
cp build/dsvp "$OUTDIR/"

# SteamOS.md ships in the bundle — the README points gamepad users at it,
# so it has to actually be there, not just in the repo.
cp SteamOS.md "$OUTDIR/"

# ── Bundle shared libraries ───────────────────────────────────────

echo "[4/6] Bundling shared libraries..."

# Kept on the user's system rather than bundled: the loader itself,
# glibc's pieces, and the compiler runtimes every distro ships.
SYSTEM_LIBS="linux-vdso|ld-linux|libc\.so|libm\.so|libpthread|libdl|librt\.so|libgcc_s|libstdc\+\+"

# The walk must be clean BEFORE copying anything: a "not found" here means
# the closure is unknowable and the bundle WILL be incomplete. The old
# `awk '{print $3}' | while [ -f ]` pipeline turned "libX.so => not found"
# into the token "not" and dropped it silently, then printed "Package
# complete!" over a broken bundle (review P1-8).
WALK=$(LD_LIBRARY_PATH="$BUILDER_LIBS" ldd build/dsvp)
if echo "$WALK" | grep -q "not found"; then
    echo "ERROR: unresolved libraries in the dependency walk:"
    echo "$WALK" | grep "not found"
    echo "       (are SDL3_LOCAL/FFMPEG_LOCAL correct? $SDL3_LOCAL / $FFMPEG_LOCAL)"
    exit 1
fi

echo "$WALK" | grep "=>" | grep -vE "$SYSTEM_LIBS" | \
    awk '{print $3}' | sort -u | while read -r lib; do
    if [ ! -f "$lib" ]; then
        echo "ERROR: ldd reported '$lib' but it does not exist on disk"
        exit 1
    fi
    cp "$lib" "$OUTDIR/lib/"
    echo "      $(basename "$lib")"
done

# NOTE: no blanket copy from the shadercross dist anymore. Its
# libSDL3_shadercross + libdxcompiler are DT_NEEDED and arrive via the
# walk above; its vanilla libSDL3.so.0.5.0 must NEVER ship (it lacks the
# HDR-metadata patch — review P2-23); libdxil is not needed on the
# SPIRV/Vulkan path (field-proven: clean-deck bundle compiled shaders
# without it, 2026-08-13).

# P2-23 guard, belt and braces: whatever SDL got bundled must be
# byte-identical to the patched build in SDL3_LOCAL.
for f in "$OUTDIR"/lib/libSDL3.so.0.*; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    if ! cmp -s "$f" "$SDL3_LOCAL/lib/$base"; then
        echo "ERROR: bundled $base is not the patched SDL from $SDL3_LOCAL/lib"
        exit 1
    fi
done

# Create soname symlinks (dynamic linker needs these)
for so in "$OUTDIR/lib"/*.so.*.*.*; do
    if [ -f "$so" ]; then
        base=$(basename "$so")
        # libFoo.so.X.Y.Z → libFoo.so.X
        soname=$(echo "$base" | sed 's/\(\.so\.[0-9]*\)\..*/\1/')
        if [ "$soname" != "$base" ] && [ ! -e "$OUTDIR/lib/$soname" ]; then
            ln -s "$base" "$OUTDIR/lib/$soname"
            echo "      $soname → $base (symlink)"
        fi
    fi
done

LIB_COUNT=$(ls -1 "$OUTDIR/lib/" 2>/dev/null | wc -l)
echo "      Bundled $LIB_COUNT libraries"

# Create launcher script. ${VAR:+...} guards the empty-element trap:
# with LD_LIBRARY_PATH unset, "$DIR/lib:$LD_LIBRARY_PATH" ends in ":" —
# an empty element ld.so treats as the CURRENT DIRECTORY, so running
# from an untrusted CWD would load libraries from it (review P2-25).
cat > "$OUTDIR/dsvp.sh" << 'LAUNCHER'
#!/bin/bash
# DSVP launcher — sets library path for bundled shared libs
DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$DIR/dsvp" "$@"
LAUNCHER
chmod +x "$OUTDIR/dsvp.sh"
echo "      Created launcher: dsvp.sh"

# Create README
cat > "$OUTDIR/README.txt" << 'README'
DSVP — Dead Simple Video Player

Run:
  ./dsvp.sh                        Open DSVP (press O to open a file)
  ./dsvp.sh /path/to/movie.mkv    Open a file directly

Do NOT run ./dsvp directly — the launcher script sets up the bundled libraries.

Controls:
  O          Open file
  Q          Quit / close file
  Space      Pause / resume
  F          Toggle fullscreen (double-click also works)
  S          Cycle subtitle tracks
  A          Cycle audio tracks
  P          Toggle bitstream passthrough / PCM decode
  Left/Right Seek ±5 seconds
  Up/Down    Volume
  B/N        Previous / next file in folder
  D          Debug overlay
  I          Media info overlay
  H          Cycle HDR debug views      (HDR content only)
  T          Cycle SDR target nits      (HDR content only: 203/300/400)
  G          Cycle midtone gain         (HDR content only: 1.0-1.4)
  E          Cycle output transfer      (HDR content only: sRGB/2.2/2.4)
  Z          HDR output: display passthrough vs player tone-map
             (HDR content only. Your choice is remembered for the whole
             session, across files — press Z again to switch back.)

Gamepad works throughout — see the included SteamOS.md for the full
mapping, Game Mode setup, and USB/SD drive notes.

AC3/DTS bitstream passthrough to an AVR or TV works out of the box —
no setup, no root, nothing to configure. (DD+/TrueHD currently decode
to PCM: SteamOS cannot yet carry them to the display — not a player
limitation.)

More info: https://github.com/ASIXicle/DSVP-deck
README
echo "      Created README.txt"

# ── Verify bundle closure ──────────────────────────────────────────

# The real test (review P1-8): the BUNDLED binary against ONLY the
# bundle's lib/. The binary has no rpath (checked above) and the
# assignment REPLACES any inherited LD_LIBRARY_PATH, so nothing outside
# the bundle can satisfy a dependency. Any gap fails the package loudly
# — this script must never again print success over a broken bundle.
echo "[5/6] Verifying bundle closure..."
CLOSURE=$(LD_LIBRARY_PATH="$PWD/$OUTDIR/lib" ldd "$OUTDIR/dsvp")
if echo "$CLOSURE" | grep -q "not found"; then
    echo "ERROR: bundle is INCOMPLETE — these do not resolve from ${OUTDIR}/lib:"
    echo "$CLOSURE" | grep "not found"
    exit 1
fi
echo "      Closure clean — every dependency resolves from the bundle"

# ── Summary ────────────────────────────────────────────────────────

echo -e "\n[6/6] Package complete!"
FILE_COUNT=$(find "$OUTDIR" -type f | wc -l)
TOTAL_SIZE=$(du -sh "$OUTDIR" | cut -f1)
echo ""
echo "  Location:  ${OUTDIR}/"
echo "  Files:     ${FILE_COUNT}"
echo "  Size:      ${TOTAL_SIZE}"
echo ""
echo "  Run with:  ./${OUTDIR}/dsvp.sh"
echo ""
