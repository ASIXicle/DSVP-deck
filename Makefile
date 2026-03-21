# DSVP — Dead Simple Video Player
# Makefile for SDL_GPU build (v0.1.6-beta)

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

# ── Windows: explicit link for Unicode Win32 APIs ──
ifeq ($(OS),Windows_NT)
  BASE_LDFLAGS += -lshell32 -lcomdlg32
endif

# ── SDL3_shadercross (bundled on Windows, pkg-config on Linux) ──
ifeq ($(OS),Windows_NT)
  SC_ROOT    = deps/SDL3_shadercross-3.0.0-windows-mingw-x64
  SC_CFLAGS  = -I$(SC_ROOT)/include
  SC_LDFLAGS = -L$(SC_ROOT)/lib -lSDL3_shadercross
else
  SC_ROOT    = shadercross/SDL3_shadercross-3.0.0-linux-x64
  SC_CFLAGS  = -I$(SC_ROOT)/include
  SC_LDFLAGS = -L$(SC_ROOT)/lib -lSDL3_shadercross -Wl,-rpath,'$$ORIGIN/../shadercross/SDL3_shadercross-3.0.0-linux-x64/lib'
endif

CFLAGS  = $(BASE_CFLAGS) $(SC_CFLAGS)
LDFLAGS = $(BASE_LDFLAGS) $(SC_LDFLAGS)

SRCS    = main.c player.c audio.c subtitle.c overlay.c log.c
OBJS    = $(SRCS:%.c=$(BUILDDIR)/%.o)

# Windows: append .exe, locate SDL3 DLLs via pkg-config, compile .rc for icon
ifeq ($(OS),Windows_NT)
  TARGET   = $(BUILDDIR)/dsvp.exe
  SDL3_BIN = $(shell pkg-config --variable=prefix sdl3)/bin
  RC_OBJ   = $(BUILDDIR)/dsvp_res.o
else
  TARGET   = $(BUILDDIR)/dsvp
  RC_OBJ   =
endif

.PHONY: all clean debug

all: $(BUILDDIR) $(TARGET)

debug: CFLAGS += -g -DDSVP_DEBUG
debug: $(BUILDDIR) $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(TARGET): $(OBJS) $(RC_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)
	rm -f $(OBJS) $(RC_OBJ)
ifeq ($(OS),Windows_NT)
	cp -u $(SDL3_BIN)/SDL3.dll $(BUILDDIR)/
	cp -u $(SDL3_BIN)/SDL3_ttf.dll $(BUILDDIR)/
	cp -u $(SC_ROOT)/bin/SDL3_shadercross.dll $(BUILDDIR)/
	cp -u $(SC_ROOT)/bin/dxcompiler.dll $(BUILDDIR)/
	cp -u $(SC_ROOT)/bin/dxil.dll $(BUILDDIR)/
endif

# Windows resource file (application icon for taskbar/explorer)
$(BUILDDIR)/dsvp_res.o: dsvp.rc src/dsvp.ico
	windres $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/dsvp.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILDDIR)
