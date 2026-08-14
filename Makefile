# DSVP — Dead Simple Video Player (Steam Deck / Linux)
# Makefile for SDL_GPU build

CC      = gcc
SRCDIR  = src
BUILDDIR = build

# ── Local dep discovery (SteamOS strips dev metadata on updates) ──
# SDL3 and FFmpeg are built from source into these prefixes on the Deck
# and survive SteamOS updates (unlike /usr/include, which gets wiped).
# We hardcode -I/-L rather than using pkg-config because SteamOS also
# wipes /usr/lib/pkgconfig, leaving SDL3_ttf's transitive deps unresolvable
# through pkg-config. Override on the make line if paths differ.
SDL3_LOCAL   ?= /home/deck/sdl3-local
# FFmpeg 9.0 (avcodec 63) as of 2026-08-08 — the deck migrated with
# SETUP.md. Rollback to the still-installed previous prefix:
#   make FFMPEG_LOCAL=/home/deck/ffmpeg-8.1-local
FFMPEG_LOCAL ?= /home/deck/ffmpeg-9.0-local

# SteamOS keeps the system headers for SDL3_ttf's transitive deps (freetype,
# harfbuzz, libpng, glib) but wipes their .pc files. Inject the include
# paths directly; harmless where they'd be resolved automatically.
SYSTEM_FONT_CFLAGS = -I/usr/include/freetype2 -I/usr/include/harfbuzz -I/usr/include/libpng16 -I/usr/include/glib-2.0 -I/usr/lib/glib-2.0/include

# ── Base flags (SDL3, FFmpeg) ──
BASE_CFLAGS  = -Wall -Wextra -O2 \
               -I$(SDL3_LOCAL)/include \
               -I$(FFMPEG_LOCAL)/include \
               $(SYSTEM_FONT_CFLAGS)

# Portable/bundle link (`make PORTABLE=1`, what package.sh runs): omit ALL
# rpath entries. The bundle's launcher supplies LD_LIBRARY_PATH; a baked
# rpath leaks builder paths into the shipped binary AND lets an incomplete
# bundle pass its ldd closure check on the build machine by silently
# resolving through these prefixes (review P2-24/P1-8). Dev builds keep
# the rpaths so build/dsvp runs bare from the tree.
PORTABLE ?= 0
ifeq ($(PORTABLE),1)
RPATH_LDFLAGS =
SC_RPATH      =
else
RPATH_LDFLAGS = -Wl,-rpath,$(SDL3_LOCAL)/lib \
                -Wl,-rpath,$(FFMPEG_LOCAL)/lib \
                -Wl,--enable-new-dtags
SC_RPATH      = -Wl,-rpath,'$$ORIGIN/../shadercross/SDL3_shadercross-3.0.0-linux-x64/lib'
endif

BASE_LDFLAGS = -L$(SDL3_LOCAL)/lib -lSDL3_ttf -lSDL3 \
               -L$(FFMPEG_LOCAL)/lib -lavformat -lavcodec -lswscale -lswresample -lavutil \
               $(RPATH_LDFLAGS) \
               -lm -lz

# ── SDL3_shadercross (bundled) ──
SC_ROOT    = shadercross/SDL3_shadercross-3.0.0-linux-x64
SC_CFLAGS  = -I$(SC_ROOT)/include
SC_LDFLAGS = -L$(SC_ROOT)/lib -lSDL3_shadercross $(SC_RPATH)

# VAAPI zero-copy interop: libva (surface export), libva-drm (DRM_PRIME),
# libvulkan (DMA-BUF import + GPU copy)
BASE_LDFLAGS += -lva -lva-drm -lvulkan

# PipeWire-native passthrough backend (bitstream_pw.c) — the only audio
# bitstream transport.
# Hardcoded include paths per house style (SteamOS wipes pkg-config files
# on updates); headers verified present on SteamOS 3.8.x (pipewire 1.6.8).
# If an OS update ever wipes /usr/include/pipewire-0.3, add a pipewire
# local-prefix build to SETUP.md. Runtime libpipewire-0.3.so.0 always
# ships with SteamOS (the OS itself runs on it).
BASE_CFLAGS  += -I/usr/include/pipewire-0.3 -I/usr/include/spa-0.2
BASE_LDFLAGS += -lpipewire-0.3

# Stamp the build with its commit so a log can never again be ambiguous about
# which tree produced it — a wrong-branch binary once cost a day of debugging a
# fix that was never in the binary being tested. "unknown" outside a git tree.
GIT_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
# +dirty must catch staged AND untracked changes: `git diff --quiet` compares
# worktree-vs-index only, so `git add` + build stamped clean with uncommitted
# code — the exact ambiguity the stamp exists to kill (review P2-22).
GIT_DIRTY  := $(shell test -n "$$(git status --porcelain 2>/dev/null)" && echo +dirty)
BASE_CFLAGS += -DDSVP_GIT_COMMIT=\"$(GIT_COMMIT)$(GIT_DIRTY)\"


CFLAGS  = $(BASE_CFLAGS) $(SC_CFLAGS)
LDFLAGS = $(BASE_LDFLAGS) $(SC_LDFLAGS)

SRCS    = main.c player.c audio.c bitstream_pw.c subtitle.c overlay.c browser.c log.c hdrwire.c
OBJS    = $(SRCS:%.c=$(BUILDDIR)/%.o)
TARGET  = $(BUILDDIR)/dsvp

.PHONY: all clean debug profile

all: $(TARGET)

debug: CFLAGS += -g -DDSVP_DEBUG
debug: $(TARGET)

profile: CFLAGS += -O2 -DDSVP_PROFILE
profile: $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# The stamp is baked into main.o at compile time, so an incremental
# build that does not touch main.c ships a STALE stamp (field case: a
# 1845ee0 binary logging "build 6576c0c"). This stamp file's content
# changes exactly when the commit/dirty state does, and main.o depends
# on it — so the startup line is always true, at the cost of one
# main.o rebuild per commit. NOTE: rules must stay BELOW `all:` — a
# rule above it becomes make's default goal (field case: bare `make`
# built FORCE, i.e. nothing).
GITSTAMP = $(BUILDDIR)/.gitstamp
.PHONY: FORCE
FORCE:
$(GITSTAMP): FORCE | $(BUILDDIR)
	@echo '$(GIT_COMMIT)$(GIT_DIRTY)' | cmp -s - $@ 2>/dev/null || echo '$(GIT_COMMIT)$(GIT_DIRTY)' > $@
$(BUILDDIR)/main.o: $(GITSTAMP)

# `make debug` after `make` (or the reverse) silently reused objects built
# with the other variant's flags — nothing invalidated them, so a release
# package could ship debug/profile objects and vice versa (review P2-21).
# The stamp records variant + PORTABLE; every object depends on it, so any
# switch rebuilds the world instead of mixing flags. Target-specific
# VARIANT propagates to this recipe through the prerequisite chain.
VARIANT = release
debug:   VARIANT = debug
profile: VARIANT = profile
FLAGSTAMP = $(BUILDDIR)/.buildflags
$(FLAGSTAMP): FORCE | $(BUILDDIR)
	@echo '$(VARIANT) PORTABLE=$(PORTABLE)' | cmp -s - $@ 2>/dev/null || echo '$(VARIANT) PORTABLE=$(PORTABLE)' > $@
$(OBJS): $(FLAGSTAMP)

# Deleting the objects after every link forced a full rebuild of all seven
# translation units on every single make — including the 5000-line player.c —
# for no benefit. Objects stay; `make clean` still removes build/ entirely.
$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

# -MMD -MP emits a .d per object listing every header it actually included,
# so touching dsvp.h, dsvp_icon.h or any future header rebuilds exactly what
# depends on it. Previously only dsvp.h was tracked, by hand.
$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(OBJS:.o=.d)

# HDR metadata probe — same zero-dep philosophy. Not part of `all`;
# build on demand: make hdr-probe, run during HDR playback.
HDRPROBE = $(BUILDDIR)/dsvp-hdr-probe
.PHONY: hdr-probe
hdr-probe: $(HDRPROBE)
$(HDRPROBE): tools/dsvp-hdr-probe.c | $(BUILDDIR)
	$(CC) -Wall -Wextra -O2 -o $@ $<

clean:
	rm -rf $(BUILDDIR)
