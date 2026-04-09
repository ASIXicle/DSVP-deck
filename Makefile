# DSVP — Dead Simple Video Player (Steam Deck / Linux)
# Makefile for SDL_GPU build

CC      = gcc
SRCDIR  = src
BUILDDIR = build

# ── Base flags (SDL3, FFmpeg) ──
BASE_CFLAGS  = -Wall -Wextra -O2 $(shell pkg-config --cflags sdl3 SDL3_ttf libavformat libavcodec libavutil libswscale libswresample)
BASE_LDFLAGS = $(shell pkg-config --libs sdl3 SDL3_ttf libavformat libavcodec libavutil libswscale libswresample) -lm -lz

# If pkg-config doesn't find SDL3_ttf, try sdl3-ttf
ifeq ($(shell pkg-config --exists SDL3_ttf 2>/dev/null && echo yes),)
  BASE_CFLAGS  = -Wall -Wextra -O2 $(shell pkg-config --cflags sdl3 sdl3-ttf libavformat libavcodec libavutil libswscale libswresample 2>/dev/null)
  BASE_LDFLAGS = $(shell pkg-config --libs sdl3 sdl3-ttf libavformat libavcodec libavutil libswscale libswresample 2>/dev/null) -lm -lz
endif

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
