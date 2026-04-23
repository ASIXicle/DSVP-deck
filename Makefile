# DSVP — Dead Simple Video Player (Steam Deck / Linux)
# Makefile for SDL_GPU build

CC      = gcc
SRCDIR  = src
BUILDDIR = build

# ── Local dep discovery (SteamOS strips dev metadata on updates) ──
# SDL3 and FFmpeg 8.1 are built from source into these prefixes on the Deck
# and survive SteamOS updates (unlike /usr/include, which gets wiped).
# We hardcode -I/-L rather than using pkg-config because SteamOS also
# wipes /usr/lib/pkgconfig, leaving SDL3_ttf's transitive deps unresolvable
# through pkg-config. Override on the make line if paths differ.
SDL3_LOCAL   ?= /home/deck/sdl3-local
FFMPEG_LOCAL ?= /home/deck/ffmpeg-8.1-local

# SteamOS keeps the system headers for SDL3_ttf's transitive deps (freetype,
# harfbuzz, libpng, glib) but wipes their .pc files. Inject the include
# paths directly; harmless where they'd be resolved automatically.
SYSTEM_FONT_CFLAGS = -I/usr/include/freetype2 -I/usr/include/harfbuzz -I/usr/include/libpng16 -I/usr/include/glib-2.0 -I/usr/lib/glib-2.0/include

# ── Base flags (SDL3, FFmpeg) ──
BASE_CFLAGS  = -Wall -Wextra -O2 \
               -I$(SDL3_LOCAL)/include \
               -I$(FFMPEG_LOCAL)/include \
               $(SYSTEM_FONT_CFLAGS)

BASE_LDFLAGS = -L$(SDL3_LOCAL)/lib -lSDL3_ttf -lSDL3 \
               -L$(FFMPEG_LOCAL)/lib -lavformat -lavcodec -lswscale -lswresample -lavutil \
               -Wl,-rpath,$(SDL3_LOCAL)/lib \
               -Wl,-rpath,$(FFMPEG_LOCAL)/lib \
               -Wl,--enable-new-dtags \
               -lm -lz

# ── SDL3_shadercross (bundled) ──
SC_ROOT    = shadercross/SDL3_shadercross-3.0.0-linux-x64
SC_CFLAGS  = -I$(SC_ROOT)/include
SC_LDFLAGS = -L$(SC_ROOT)/lib -lSDL3_shadercross -Wl,-rpath,'$$ORIGIN/../shadercross/SDL3_shadercross-3.0.0-linux-x64/lib'

# VAAPI zero-copy interop: libva (surface export), libva-drm (DRM_PRIME),
# libvulkan (DMA-BUF import + GPU copy)
# ALSA: direct PCM access for bitstream audio passthrough (bypasses PipeWire)
BASE_LDFLAGS += -lva -lva-drm -lvulkan -lasound

CFLAGS  = $(BASE_CFLAGS) $(SC_CFLAGS)
LDFLAGS = $(BASE_LDFLAGS) $(SC_LDFLAGS)

SRCS    = main.c player.c audio.c subtitle.c overlay.c browser.c log.c
OBJS    = $(SRCS:%.c=$(BUILDDIR)/%.o)
TARGET  = $(BUILDDIR)/dsvp

.PHONY: all clean debug profile

all: $(BUILDDIR) $(TARGET)

debug: CFLAGS += -g -DDSVP_DEBUG
debug: $(BUILDDIR) $(TARGET)

profile: CFLAGS += -O2 -DDSVP_PROFILE
profile: $(BUILDDIR) $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	rm -f $(OBJS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/dsvp.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILDDIR)
