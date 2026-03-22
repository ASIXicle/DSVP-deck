#!/bin/bash
# DSVP Portable Packaging Script (Linux/macOS)
# Creates a self-contained DSVP-portable/ folder with binary + shared libs.
#
# Usage:
#   ./package.sh
#   ./package.sh --skip-build

set -e

VERSION="0.1.6-beta"
OUTDIR="DSVP-portable"
SKIP_BUILD=0

if [ "$1" = "--skip-build" ]; then
    SKIP_BUILD=1
fi

echo "=== DSVP Packager v${VERSION} ==="

# ── Build ──────────────────────────────────────────────────────────

if [ "$SKIP_BUILD" -eq 0 ]; then
    echo -e "\n[1/5] Building..."
    make clean 2>/dev/null || true
    make
    echo "      Build OK"
else
    echo -e "\n[1/5] Skipping build"
fi

# ── Verify binary ─────────────────────────────────────────────────

if [ ! -f "build/dsvp" ]; then
    echo "ERROR: build/dsvp not found."
    exit 1
fi

# ── Create output directory ────────────────────────────────────────

echo "[2/5] Creating ${OUTDIR}/"
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR/lib"

# ── Copy binary ───────────────────────────────────────────────────

echo "[3/5] Copying binary..."
cp build/dsvp "$OUTDIR/"

# ── Bundle shared libraries ───────────────────────────────────────

echo "[4/5] Bundling shared libraries..."

if [ "$(uname)" = "Linux" ]; then
    # Use ldd to find all linked shared libraries, skip system libs
    SYSTEM_LIBS="linux-vdso|ld-linux|libc\.so|libm\.so|libpthread|libdl|librt\.so|libgcc_s|libstdc\+\+"

    ldd build/dsvp | grep "=>" | grep -vE "$SYSTEM_LIBS" | \
        awk '{print $3}' | sort -u | while read -r lib; do
        if [ -f "$lib" ]; then
            cp "$lib" "$OUTDIR/lib/"
            echo "      $(basename "$lib")"
        fi
    done

    # Also bundle shadercross libs that may not show in ldd (loaded via rpath)
    SC_LIB="shadercross/SDL3_shadercross-3.0.0-linux-x64/lib"
    if [ -d "$SC_LIB" ]; then
        for so in "$SC_LIB"/*.so.*; do
            if [ -f "$so" ] && [ ! -L "$so" ]; then
                base=$(basename "$so")
                if [ ! -f "$OUTDIR/lib/$base" ]; then
                    cp "$so" "$OUTDIR/lib/"
                    echo "      $base (shadercross)"
                fi
            fi
        done
    fi

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

    # Create launcher script
    cat > "$OUTDIR/dsvp.sh" << 'LAUNCHER'
#!/bin/bash
# DSVP launcher — sets library path for bundled shared libs
DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$DIR/lib:$LD_LIBRARY_PATH"
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
  F          Toggle fullscreen
  S          Cycle subtitle tracks
  A          Cycle audio tracks
  Left/Right Seek ±5 seconds
  Up/Down    Volume
  B/N        Previous / next file in folder
  D          Debug overlay
  I          Media info overlay

More info: https://github.com/ASIXicle/DSVP
README
    echo "      Created README.txt"
fi

if [ "$(uname)" = "Darwin" ]; then
    # On macOS, use otool to find dylibs
    SYSTEM_LIBS="/usr/lib/|/System/"

    otool -L build/dsvp | tail -n +2 | awk '{print $1}' | \
        grep -vE "$SYSTEM_LIBS" | sort -u | while read -r lib; do
        if [ -f "$lib" ]; then
            cp "$lib" "$OUTDIR/lib/"
            echo "      $(basename "$lib")"
        fi
    done

    LIB_COUNT=$(ls -1 "$OUTDIR/lib/" 2>/dev/null | wc -l)
    echo "      Bundled $LIB_COUNT dylibs"

    # Create launcher script
    cat > "$OUTDIR/dsvp.sh" << 'LAUNCHER'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export DYLD_LIBRARY_PATH="$DIR/lib:$DYLD_LIBRARY_PATH"
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
  F          Toggle fullscreen
  S          Cycle subtitle tracks
  A          Cycle audio tracks
  Left/Right Seek ±5 seconds
  Up/Down    Volume
  B/N        Previous / next file in folder
  D          Debug overlay
  I          Media info overlay

More info: https://github.com/ASIXicle/DSVP
README
    echo "      Created README.txt"
fi

# ── Summary ────────────────────────────────────────────────────────

echo -e "\n[5/5] Package complete!"
FILE_COUNT=$(find "$OUTDIR" -type f | wc -l)
TOTAL_SIZE=$(du -sh "$OUTDIR" | cut -f1)
echo ""
echo "  Location:  ${OUTDIR}/"
echo "  Files:     ${FILE_COUNT}"
echo "  Size:      ${TOTAL_SIZE}"
echo ""
echo "  Run with:  ./${OUTDIR}/dsvp.sh"
echo ""
