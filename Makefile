# DSVP — Dead Simple Video Player
# Makefile for SDL_GPU build (v0.1.4-beta)

CC      = gcc
SRCDIR  = src
BUILDDIR = build

# ── Base flags (SDL3, FFmpeg) ──
BASE_CFLAGS  = -Wall -Wextra -O2 $(shell pkg-config --cflags sdl3 SDL3_ttf libavformat libavcodec libavutil libswscale libswresample)
BASE_LDFLAGS = $(shell pkg-config --libs sdl3 SDL3_ttf libavformat libavcodec libavutil libswscale libswresample) -lm

# If pkg-config doesn't find SDL3_ttf, try sdl3-ttf
ifeq ($(shell pkg-config --exists SDL3_ttf 2>/dev/null && echo yes),)
  BASE_CFLAGS  = -Wall -Wextra -O2 $(shell pkg-config --cflags sdl3 sdl3-ttf libavformat libavcodec libavutil libswscale libswresample 2>/dev/null)
  BASE_LDFLAGS = $(shell pkg-config --libs sdl3 sdl3-ttf libavformat libavcodec libavutil libswscale libswresample 2>/dev/null) -lm
endif

# ── SDL3_shadercross (bundled on Windows, pkg-config on Linux) ──
ifeq ($(OS),Windows_NT)
  SC_ROOT    = deps/SDL3_shadercross-3.0.0-windows-mingw-x64
  SC_CFLAGS  = -I$(SC_ROOT)/include
  SC_LDFLAGS = -L$(SC_ROOT)/lib -lSDL3_shadercross
else
  SC_CFLAGS  = $(shell pkg-config --cflags SDL3_shadercross 2>/dev/null)
  SC_LDFLAGS = $(shell pkg-config --libs SDL3_shadercross 2>/dev/null)
endif

CFLAGS  = $(BASE_CFLAGS) $(SC_CFLAGS)
LDFLAGS = $(BASE_LDFLAGS) $(SC_LDFLAGS)

SRCS    = main.c player.c audio.c subtitle.c log.c
OBJS    = $(SRCS:%.c=$(BUILDDIR)/%.o)

# Windows: append .exe, locate SDL3 DLLs via pkg-config
ifeq ($(OS),Windows_NT)
  TARGET   = $(BUILDDIR)/dsvp.exe
  SDL3_BIN = $(shell pkg-config --variable=prefix sdl3)/bin
else
  TARGET   = $(BUILDDIR)/dsvp
endif

.PHONY: all clean debug

all: $(BUILDDIR) $(TARGET)

debug: CFLAGS += -g -DDSVP_DEBUG
debug: $(BUILDDIR) $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	rm -f $(OBJS)
ifeq ($(OS),Windows_NT)
	cp -u $(SDL3_BIN)/SDL3.dll $(BUILDDIR)/
	cp -u $(SDL3_BIN)/SDL3_ttf.dll $(BUILDDIR)/
	cp -u $(SC_ROOT)/bin/SDL3_shadercross.dll $(BUILDDIR)/
	cp -u $(SC_ROOT)/bin/dxcompiler.dll $(BUILDDIR)/
	cp -u $(SC_ROOT)/bin/dxil.dll $(BUILDDIR)/
endif

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/dsvp.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILDDIR)
