/*
 * DSVP — Dead Simple Video Player
 * overlay.c — GPU-composited overlay system
 *
 * Renders all UI overlays to a single RGBA pixel buffer which is
 * uploaded to the GPU as a texture and alpha-composited over the
 * video frame. The built-in 5×7 bitmap font provides the classic
 * DSVP aesthetic for all overlays except subtitles (which use
 * SDL_ttf for proper Unicode and variable sizing).
 *
 * Overlays rendered (in draw order, back to front):
 *   - Seek bar + time (auto-hides after 3s)
 *   - Debug info (D key, mutually exclusive with media info)
 *   - Media info (I key, mutually exclusive with debug)
 *   - Pause indicator
 *   - Track-change / volume OSD
 *   - Subtitles (SDL_ttf rendered)
 */

#include "dsvp.h"

/* ═══════════════════════════════════════════════════════════════════
 * 5×7 Bitmap Font — ASCII 32–126
 * ═══════════════════════════════════════════════════════════════════
 *
 * Each character is 5 columns × 7 rows, stored as 7 bytes.
 * Bit 4 = leftmost column, bit 0 = rightmost column.
 * Indexed by (character - 32).
 */

#define FONT_W      5
#define FONT_H      7
#define FONT_GAP    1   /* pixel gap between characters */
#define FONT_LINE   2   /* extra pixel gap between lines */
#define FONT_FIRST  32
#define FONT_LAST   126

static const uint8_t font_5x7[95][7] = {
    /* 32 ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 33 '!' */ {0x04,0x04,0x04,0x04,0x04,0x00,0x04},
    /* 34 '"' */ {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00},
    /* 35 '#' */ {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A},
    /* 36 '$' */ {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04},
    /* 37 '%' */ {0x18,0x19,0x02,0x04,0x08,0x13,0x03},
    /* 38 '&' */ {0x08,0x14,0x14,0x08,0x15,0x12,0x0D},
    /* 39 ''' */ {0x04,0x04,0x00,0x00,0x00,0x00,0x00},
    /* 40 '(' */ {0x02,0x04,0x08,0x08,0x08,0x04,0x02},
    /* 41 ')' */ {0x08,0x04,0x02,0x02,0x02,0x04,0x08},
    /* 42 '*' */ {0x00,0x04,0x15,0x0E,0x15,0x04,0x00},
    /* 43 '+' */ {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
    /* 44 ',' */ {0x00,0x00,0x00,0x00,0x00,0x04,0x08},
    /* 45 '-' */ {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    /* 46 '.' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x04},
    /* 47 '/' */ {0x00,0x01,0x02,0x04,0x08,0x10,0x00},
    /* 48 '0' */ {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    /* 49 '1' */ {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    /* 50 '2' */ {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F},
    /* 51 '3' */ {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E},
    /* 52 '4' */ {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    /* 53 '5' */ {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    /* 54 '6' */ {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
    /* 55 '7' */ {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    /* 56 '8' */ {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    /* 57 '9' */ {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    /* 58 ':' */ {0x00,0x00,0x04,0x00,0x00,0x04,0x00},
    /* 59 ';' */ {0x00,0x00,0x04,0x00,0x00,0x04,0x08},
    /* 60 '<' */ {0x02,0x04,0x08,0x10,0x08,0x04,0x02},
    /* 61 '=' */ {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},
    /* 62 '>' */ {0x08,0x04,0x02,0x01,0x02,0x04,0x08},
    /* 63 '?' */ {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},
    /* 64 '@' */ {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E},
    /* 65 'A' */ {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* 66 'B' */ {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    /* 67 'C' */ {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    /* 68 'D' */ {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},
    /* 69 'E' */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    /* 70 'F' */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    /* 71 'G' */ {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
    /* 72 'H' */ {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* 73 'I' */ {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    /* 74 'J' */ {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    /* 75 'K' */ {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    /* 76 'L' */ {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    /* 77 'M' */ {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    /* 78 'N' */ {0x11,0x11,0x19,0x15,0x13,0x11,0x11},
    /* 79 'O' */ {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* 80 'P' */ {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    /* 81 'Q' */ {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    /* 82 'R' */ {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    /* 83 'S' */ {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E},
    /* 84 'T' */ {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    /* 85 'U' */ {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* 86 'V' */ {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    /* 87 'W' */ {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
    /* 88 'X' */ {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    /* 89 'Y' */ {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    /* 90 'Z' */ {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    /* 91 '[' */ {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},
    /* 92 '\' */ {0x00,0x10,0x08,0x04,0x02,0x01,0x00},
    /* 93 ']' */ {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},
    /* 94 '^' */ {0x04,0x0A,0x11,0x00,0x00,0x00,0x00},
    /* 95 '_' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
    /* 96 '`' */ {0x08,0x04,0x00,0x00,0x00,0x00,0x00},
    /* 97 'a' */ {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F},
    /* 98 'b' */ {0x10,0x10,0x16,0x19,0x11,0x11,0x1E},
    /* 99 'c' */ {0x00,0x00,0x0E,0x10,0x10,0x11,0x0E},
    /*100 'd' */ {0x01,0x01,0x0D,0x13,0x11,0x11,0x0F},
    /*101 'e' */ {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E},
    /*102 'f' */ {0x06,0x09,0x08,0x1C,0x08,0x08,0x08},
    /*103 'g' */ {0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E},
    /*104 'h' */ {0x10,0x10,0x16,0x19,0x11,0x11,0x11},
    /*105 'i' */ {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E},
    /*106 'j' */ {0x02,0x00,0x06,0x02,0x02,0x12,0x0C},
    /*107 'k' */ {0x10,0x10,0x12,0x14,0x18,0x14,0x12},
    /*108 'l' */ {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E},
    /*109 'm' */ {0x00,0x00,0x1A,0x15,0x15,0x11,0x11},
    /*110 'n' */ {0x00,0x00,0x16,0x19,0x11,0x11,0x11},
    /*111 'o' */ {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E},
    /*112 'p' */ {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10},
    /*113 'q' */ {0x00,0x00,0x0D,0x13,0x0F,0x01,0x01},
    /*114 'r' */ {0x00,0x00,0x16,0x19,0x10,0x10,0x10},
    /*115 's' */ {0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E},
    /*116 't' */ {0x08,0x08,0x1C,0x08,0x08,0x09,0x06},
    /*117 'u' */ {0x00,0x00,0x11,0x11,0x11,0x13,0x0D},
    /*118 'v' */ {0x00,0x00,0x11,0x11,0x11,0x0A,0x04},
    /*119 'w' */ {0x00,0x00,0x11,0x11,0x15,0x15,0x0A},
    /*120 'x' */ {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11},
    /*121 'y' */ {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E},
    /*122 'z' */ {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F},
    /*123 '{' */ {0x02,0x04,0x04,0x08,0x04,0x04,0x02},
    /*124 '|' */ {0x04,0x04,0x04,0x04,0x04,0x04,0x04},
    /*125 '}' */ {0x08,0x04,0x04,0x02,0x04,0x04,0x08},
    /*126 '~' */ {0x00,0x00,0x08,0x15,0x02,0x00,0x00},
};


/* ═══════════════════════════════════════════════════════════════════
 * Pixel Drawing Primitives
 * ═══════════════════════════════════════════════════════════════════
 *
 * All drawing operates on a flat RGBA pixel buffer (R at byte 0,
 * A at byte 3 — matches SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM).
 * Buffer is cleared to transparent at the start of each frame.
 */

/* Fill a rectangle with a solid RGBA color. No blending — just overwrites. */
static void fill_rect(uint8_t *buf, int bw, int bh,
                      int rx, int ry, int rw, int rh,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    int x1 = (rx < 0) ? 0 : rx;
    int y1 = (ry < 0) ? 0 : ry;
    int x2 = (rx + rw > bw) ? bw : rx + rw;
    int y2 = (ry + rh > bh) ? bh : ry + rh;

    int stride = bw * 4;
    for (int py = y1; py < y2; py++) {
        uint8_t *row = buf + py * stride + x1 * 4;
        for (int px = x1; px < x2; px++) {
            row[0] = r; row[1] = g; row[2] = b; row[3] = a;
            row += 4;
        }
    }
}


/* ═══════════════════════════════════════════════════════════════════
 * Bitmap Font Rendering
 * ═══════════════════════════════════════════════════════════════════ */

/* Draw a single character at (x, y) with the given scale and color. */
static void draw_char(uint8_t *buf, int bw, int bh,
                      int x, int y, char ch, int scale,
                      uint8_t r, uint8_t g, uint8_t b) {
    if (ch < FONT_FIRST || ch > FONT_LAST) return;
    const uint8_t *glyph = font_5x7[ch - FONT_FIRST];

    int stride = bw * 4;
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_W; col++) {
            if (!(bits & (0x10 >> col))) continue;
            /* Fill a scale×scale block for this pixel */
            for (int sy = 0; sy < scale; sy++) {
                int py = y + row * scale + sy;
                if (py < 0 || py >= bh) continue;
                for (int sx = 0; sx < scale; sx++) {
                    int px = x + col * scale + sx;
                    if (px < 0 || px >= bw) continue;
                    uint8_t *p = buf + py * stride + px * 4;
                    p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
                }
            }
        }
    }
}

/* Draw a text string. Handles newlines. Returns total height drawn. */
static int draw_text(uint8_t *buf, int bw, int bh,
                     int x, int y, const char *text, int scale,
                     uint8_t r, uint8_t g, uint8_t b) {
    int cx = x;
    int start_y = y;
    int cell_w = (FONT_W + FONT_GAP) * scale;
    int line_h = (FONT_H + FONT_LINE) * scale;

    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            cx = x;
            y += line_h;
            continue;
        }
        draw_char(buf, bw, bh, cx, y, *p, scale, r, g, b);
        cx += cell_w;
    }
    return (y - start_y) + FONT_H * scale;
}

/* Measure text width in pixels (widest line). */
static int text_width(const char *text, int scale) {
    int cell_w = (FONT_W + FONT_GAP) * scale;
    int max_w = 0, cur_w = 0;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            if (cur_w > max_w) max_w = cur_w;
            cur_w = 0;
        } else {
            cur_w += cell_w;
        }
    }
    return (cur_w > max_w) ? cur_w : max_w;
}

/* Count number of lines in text. */
static int text_lines(const char *text) {
    int n = 1;
    for (const char *p = text; *p; p++)
        if (*p == '\n') n++;
    return n;
}

/* Total text height in pixels. */
static int text_height(const char *text, int scale) {
    int n = text_lines(text);
    return n * (FONT_H + FONT_LINE) * scale - FONT_LINE * scale;
}


/* ═══════════════════════════════════════════════════════════════════
 * SDL_Surface Blit (for TTF subtitle rendering)
 * ═══════════════════════════════════════════════════════════════════ */

/* Blit an SDL_Surface onto the overlay pixel buffer with alpha blending.
 * Used for TTF-rendered subtitle text. */
static void blit_surface(uint8_t *buf, int bw, int bh,
                         SDL_Surface *surf, int dst_x, int dst_y) {
    if (!surf) return;

    /* Convert to our RGBA byte order */
    SDL_Surface *rgba = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
    if (!rgba) return;

    int stride = bw * 4;
    for (int sy = 0; sy < rgba->h; sy++) {
        int dy = dst_y + sy;
        if (dy < 0 || dy >= bh) continue;
        uint8_t *src_row = (uint8_t *)rgba->pixels + sy * rgba->pitch;
        uint8_t *dst_row = buf + dy * stride;
        for (int sx = 0; sx < rgba->w; sx++) {
            int dx = dst_x + sx;
            if (dx < 0 || dx >= bw) continue;
            uint8_t *sp = src_row + sx * 4;
            uint8_t sa = sp[3];
            if (sa == 0) continue;
            uint8_t *dp = dst_row + dx * 4;
            if (sa == 255 || dp[3] == 0) {
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sa;
            } else {
                /* Alpha compositing */
                float sf = sa / 255.0f;
                float df = dp[3] / 255.0f;
                float of = sf + df * (1.0f - sf);
                if (of > 0.0f) {
                    dp[0] = (uint8_t)((sp[0] * sf + dp[0] * df * (1.0f - sf)) / of);
                    dp[1] = (uint8_t)((sp[1] * sf + dp[1] * df * (1.0f - sf)) / of);
                    dp[2] = (uint8_t)((sp[2] * sf + dp[2] * df * (1.0f - sf)) / of);
                    dp[3] = (uint8_t)(of * 255.0f);
                }
            }
        }
    }
    SDL_DestroySurface(rgba);
}


/* ═══════════════════════════════════════════════════════════════════
 * Overlay Components
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Seek Bar ──
 *
 * Dark semi-transparent bar at bottom of screen.
 * Layout: [time] [==progress track==] [separator] [volume]
 * The progress track is shortened to give volume its own area. */
static void draw_seekbar(uint8_t *buf, int bw, int bh, PlayerState *ps) {
    double duration = (ps->fmt_ctx && ps->fmt_ctx->duration != AV_NOPTS_VALUE)
        ? (double)ps->fmt_ctx->duration / AV_TIME_BASE : 0.0;
    double pos = ps->video_clock;
    if (pos < 0.0) pos = 0.0;
    if (duration > 0.0 && pos > duration) pos = duration;

    int bar_h = 30;
    int bar_y = bh - bar_h;
    int margin = 20;

    /* Background bar */
    fill_rect(buf, bw, bh, 0, bar_y, bw, bar_h, 0, 0, 0, 160);

    /* ── Volume area (right side) ── */
    char vol_str[16];
    snprintf(vol_str, sizeof(vol_str), "Vol: %.0f%%", ps->volume * 100.0);
    int vol_tw = text_width(vol_str, 1);
    int vol_area_w = vol_tw + margin;  /* right margin included */
    int vol_x = bw - margin - vol_tw;

    /* Separator line */
    int sep_x = bw - vol_area_w - 12;
    fill_rect(buf, bw, bh, sep_x, bar_y + 6, 1, bar_h - 12, 100, 100, 100, 160);

    draw_text(buf, bw, bh, vol_x, bar_y + (bar_h - FONT_H) / 2,
              vol_str, 1, 180, 180, 180);

    /* ── Time text (left side) ── */
    int p_h = (int)pos / 3600, p_m = ((int)pos % 3600) / 60, p_s = (int)pos % 60;
    int d_h = (int)duration / 3600, d_m = ((int)duration % 3600) / 60, d_s = (int)duration % 60;

    char time_str[64];
    if (d_h > 0 || p_h > 0)
        snprintf(time_str, sizeof(time_str), "%d:%02d:%02d / %d:%02d:%02d",
                 p_h, p_m, p_s, d_h, d_m, d_s);
    else
        snprintf(time_str, sizeof(time_str), "%d:%02d / %d:%02d",
                 p_m, p_s, d_m, d_s);

    int time_tw = text_width(time_str, 1);
    draw_text(buf, bw, bh, margin, bar_y + (bar_h - FONT_H) / 2,
              time_str, 1, 200, 200, 200);

    /* ── Progress track (between time and separator) ── */
    int track_x = margin + time_tw + 12;
    int track_w = sep_x - track_x - 12;
    int track_y = bar_y + bar_h / 2 - 2;
    int track_h = 4;

    if (track_w > 20) {
        fill_rect(buf, bw, bh, track_x, track_y, track_w, track_h, 80, 80, 80, 200);

        /* Progress fill */
        if (duration > 0.0) {
            double frac = pos / duration;
            int fill_w = (int)(track_w * frac);
            fill_rect(buf, bw, bh, track_x, track_y, fill_w, track_h,
                      200, 200, 200, 240);

            /* Playhead dot */
            int dot_x = track_x + fill_w - 4;
            int dot_y = track_y - 2;
            fill_rect(buf, bw, bh, dot_x, dot_y, 8, 8, 240, 240, 240, 255);
        }
    }
}


/* ── Debug / Info Overlay ──
 *
 * Multi-line text panel with semi-transparent background box.
 * Both use the same rendering — just different content. */
static void draw_text_panel(uint8_t *buf, int bw, int bh,
                            const char *text, int x, int y, int scale) {
    if (!text || !text[0]) return;

    int pad = 8;
    int tw = text_width(text, scale);
    int th = text_height(text, scale);

    /* Background box */
    fill_rect(buf, bw, bh,
              x - pad, y - pad,
              tw + pad * 2, th + pad * 2,
              0, 0, 0, 180);

    /* Text */
    draw_text(buf, bw, bh, x, y, text, scale, 220, 220, 220);
}


/* ── Pause Indicator ── */
static void draw_pause(uint8_t *buf, int bw, int bh) {
    const char *msg = "PAUSED";
    int scale = 3;
    int tw = text_width(msg, scale);
    int th = FONT_H * scale;
    int x = (bw - tw) / 2;
    int y = (bh - th) / 2;

    fill_rect(buf, bw, bh,
              x - 16, y - 12, tw + 32, th + 24,
              0, 0, 0, 140);
    draw_text(buf, bw, bh, x, y, msg, scale, 200, 200, 200);
}


/* ── Track-Change / Volume OSD ──
 *
 * Brief notification centered at top of screen. */
static void draw_osd(uint8_t *buf, int bw, int bh, const char *text) {
    if (!text || !text[0]) return;

    int scale = 2;
    int tw = text_width(text, scale);
    int th = FONT_H * scale;
    int x = (bw - tw) / 2;
    int y = 30;

    fill_rect(buf, bw, bh,
              x - 12, y - 8, tw + 24, th + 16,
              0, 0, 0, 160);
    draw_text(buf, bw, bh, x, y, text, scale, 255, 223, 0);
}


/* ── Menu Bar (top) ──
 *
 * Semi-transparent bar at top of screen showing key bindings.
 * Appears with the seek bar, auto-hides together. */
static void draw_menubar(uint8_t *buf, int bw, int bh) {
    int bar_h = 22;

    /* Background bar */
    fill_rect(buf, bw, bh, 0, 0, bw, bar_h, 0, 0, 0, 140);

    /* Hotkey list — abbreviated to fit comfortably */
    const char *items[] = {
        "[O] Open",  "[Space] Pause",  "[F] Fullscreen",
        "[D] Debug",  "[I] Info",  "[S] Sub",
        "[A] Audio",  "[Q] Close",  NULL
    };

    int margin = 12;
    int gap = 16;
    int x = margin;
    int y = (bar_h - FONT_H) / 2;

    for (int i = 0; items[i]; i++) {
        /* Key notation in brighter color */
        const char *p = items[i];
        while (*p) {
            if (*p == '[') {
                /* Opening bracket + key in accent color */
                p++;  /* skip '[' */
                while (*p && *p != ']') {
                    draw_char(buf, bw, bh, x, y, *p, 1, 180, 200, 240);
                    x += (FONT_W + FONT_GAP);
                    p++;
                }
                if (*p == ']') p++;  /* skip ']' */
                /* Space before label */
                x += (FONT_W + FONT_GAP) / 2;
            } else {
                draw_char(buf, bw, bh, x, y, *p, 1, 150, 150, 150);
                x += (FONT_W + FONT_GAP);
                p++;
            }
        }
        x += gap;
    }
}


/* ── Subtitles ──
 *
 * Rendered via SDL_ttf for proper Unicode support. The TTF surfaces
 * are blitted onto the overlay pixel buffer. Outline is drawn first
 * (black), then the main text (golden yellow) on top. */
static void draw_subtitles(uint8_t *buf, int bw, int bh, PlayerState *ps) {
    if (!ps->sub_valid || ps->sub_selection == 0) return;

    double now = (ps->audio_stream_idx >= 0) ? ps->audio_clock : ps->video_clock;
    if (now < ps->sub_start_pts || now > ps->sub_end_pts) return;

    /* Bitmap subtitles — not yet supported in GPU overlay, skip for now */
    if (ps->sub_is_bitmap) return;

    /* Text subtitles — need the font */
    TTF_Font *font = sub_get_font();
    TTF_Font *outline_font = sub_get_outline_font();
    if (!font || !ps->sub_text[0]) return;

    /* Set font size relative to window height */
    int font_size = bh / 24;
    if (font_size < 14) font_size = 14;
    if (font_size > 54) font_size = 54;
    TTF_SetFontSize(font, font_size);
    if (outline_font)
        TTF_SetFontSize(outline_font, font_size);

    /* Split text into lines */
    char text_buf[SUB_TEXT_SIZE];
    snprintf(text_buf, sizeof(text_buf), "%s", ps->sub_text);

    char *lines[64];
    int nlines = 0;
    char *tok = strtok(text_buf, "\n");
    while (tok && nlines < 64) {
        if (tok[0] != '\0')
            lines[nlines++] = tok;
        tok = strtok(NULL, "\n");
    }

    int line_height = TTF_GetFontLineSkip(font);
    int total_h = nlines * line_height;
    int y_base = bh - 60 - total_h;

    SDL_Color fg      = { 255, 223, 0, 255 };   /* golden yellow */
    SDL_Color outline  = { 0, 0, 0, 255 };

    for (int i = 0; i < nlines; i++) {
        int tw = 0, th = 0;
        TTF_GetStringSize(font, lines[i], 0, &tw, &th);
        int x = (bw - tw) / 2;
        int y = y_base + i * line_height;

        /* Outline (rendered first, behind main text) */
        if (outline_font) {
            SDL_Surface *osurf = TTF_RenderText_Blended(
                outline_font, lines[i], 0, outline);
            if (osurf) {
                int oo = TTF_GetFontOutline(outline_font);
                blit_surface(buf, bw, bh, osurf, x - oo, y - oo);
                SDL_DestroySurface(osurf);
            }
        }

        /* Main text */
        SDL_Surface *tsurf = TTF_RenderText_Blended(font, lines[i], 0, fg);
        if (tsurf) {
            blit_surface(buf, bw, bh, tsurf, x, y);
            SDL_DestroySurface(tsurf);
        }
    }
}


/* Static pixel buffer — avoids malloc/free per frame.
 * Shared between overlay_render and overlay_render_idle. */
static uint8_t *s_pixels  = NULL;
static int       s_pix_w  = 0;
static int       s_pix_h  = 0;


/* ═══════════════════════════════════════════════════════════════════
 * Idle Screen — shown when no media is loaded
 * ═══════════════════════════════════════════════════════════════════
 *
 * Renders DSVP title, version, and hotkey reference centered on the
 * dark background. Called from main.c's gpu_draw_idle path.
 */

void overlay_render_idle(PlayerState *ps) {
    int w = ps->win_w;
    int h = ps->win_h;
    if (w <= 0 || h <= 0) return;

    if (gpu_overlay_ensure(ps, w, h) < 0) {
        ps->overlay_active = 0;
        return;
    }

    size_t buf_size = (size_t)w * h * 4;
    if (s_pix_w != w || s_pix_h != h) {
        free(s_pixels);
        s_pixels = malloc(buf_size);
        if (!s_pixels) { ps->overlay_active = 0; return; }
        s_pix_w = w;
        s_pix_h = h;
    }
    memset(s_pixels, 0, buf_size);

    /* ── Title: "DSVP" in large bitmap font ── */
    int title_scale = 7;
    const char *title = "DSVP";
    int title_tw = text_width(title, title_scale);
    int title_x = (w - title_tw) / 2;
    int title_y = h / 5;
    draw_text(s_pixels, w, h, title_x, title_y, title, title_scale,
              180, 190, 210);

    /* ── Version ── */
    int ver_scale = 2;
    char ver_str[64];
    snprintf(ver_str, sizeof(ver_str), "v%s", DSVP_VERSION);
    int ver_tw = text_width(ver_str, ver_scale);
    int ver_y = title_y + FONT_H * title_scale + 16;
    draw_text(s_pixels, w, h, (w - ver_tw) / 2, ver_y, ver_str, ver_scale,
              120, 120, 130);

    /* ── Subtitle ── */
    int sub_scale = 1;
    const char *subtitle = "Dead Simple Video Player";
    int sub_tw = text_width(subtitle, sub_scale);
    int sub_y = ver_y + FONT_H * ver_scale + 10;
    draw_text(s_pixels, w, h, (w - sub_tw) / 2, sub_y, subtitle, sub_scale,
              100, 100, 110);

    /* ── Hotkey reference ── */
    int key_scale = 2;
    int key_y = sub_y + FONT_H * sub_scale + 40;

    /* Two-column layout: key on left, description on right */
    static const char *keys[][2] = {
        { "O",     "Open file" },
        { "Space", "Play / Pause" },
        { "F",     "Toggle fullscreen" },
        { "D",     "Debug overlay" },
        { "I",     "Media info" },
        { "S",     "Cycle subtitles" },
        { "A",     "Cycle audio tracks" },
        { "Left/Right", "Seek 5s" },
        { "Up/Down",    "Volume" },
        { "Q",     "Close / Quit" },
        { NULL, NULL }
    };

    int line_h = (FONT_H + FONT_LINE) * key_scale;
    int col_gap = 14 * (FONT_W + FONT_GAP) * key_scale;  /* key column width */

    /* Measure total height to center vertically from key_y */
    int num_keys = 0;
    while (keys[num_keys][0]) num_keys++;
    int block_h = num_keys * line_h;
    (void)block_h;

    /* Center the key block horizontally */
    int block_x = (w - col_gap - text_width("Toggle fullscreen", key_scale)) / 2;
    if (block_x < 40) block_x = 40;

    for (int i = 0; keys[i][0]; i++) {
        int y = key_y + i * line_h;
        /* Key name in accent color */
        draw_text(s_pixels, w, h, block_x, y,
                  keys[i][0], key_scale, 180, 200, 240);
        /* Description in dim color */
        draw_text(s_pixels, w, h, block_x + col_gap, y,
                  keys[i][1], key_scale, 130, 130, 140);
    }

    gpu_overlay_upload(ps, s_pixels, w, h);
    ps->overlay_active = 1;
}


/* ═══════════════════════════════════════════════════════════════════
 * Master Render — called once per frame from main.c
 * ═══════════════════════════════════════════════════════════════════
 *
 * Composites all active overlays into a single RGBA pixel buffer,
 * uploads to the GPU overlay texture. Skips entirely when nothing
 * is visible (sets overlay_active = 0 so the draw call is a no-op).
 */

void overlay_render(PlayerState *ps) {
    int w = ps->win_w;
    int h = ps->win_h;
    if (w <= 0 || h <= 0 || !ps->playing) {
        ps->overlay_active = 0;
        return;
    }

    double now = get_time_sec();

    /* ── Check seekbar auto-hide ── */
    if (ps->show_seekbar && now > ps->seekbar_hide_time) {
        ps->show_seekbar = 0;
    }

    /* ── Determine which overlays are needed ── */
    int need_seekbar = ps->show_seekbar;
    int need_debug   = ps->show_debug;
    int need_info    = ps->show_info;
    int need_pause   = ps->paused;
    int need_sub     = (ps->sub_valid && ps->sub_selection > 0);

    /* OSD: audio or subtitle track change */
    const char *osd_text = NULL;
    if (ps->aud_osd[0] && now < ps->aud_osd_until)
        osd_text = ps->aud_osd;
    else if (ps->aud_osd[0])
        ps->aud_osd[0] = '\0';

    if (!osd_text && ps->sub_osd[0] && now < ps->sub_osd_until)
        osd_text = ps->sub_osd;
    else if (!osd_text && ps->sub_osd[0])
        ps->sub_osd[0] = '\0';

    int need_osd = (osd_text != NULL);

    if (!need_seekbar && !need_debug && !need_info &&
        !need_pause && !need_osd && !need_sub) {
        ps->overlay_active = 0;
        return;
    }

    /* ── Ensure GPU overlay texture matches window size ── */
    if (gpu_overlay_ensure(ps, w, h) < 0) {
        ps->overlay_active = 0;
        return;
    }

    /* ── Ensure pixel buffer ── */
    size_t buf_size = (size_t)w * h * 4;
    if (s_pix_w != w || s_pix_h != h) {
        free(s_pixels);
        s_pixels = malloc(buf_size);
        if (!s_pixels) { ps->overlay_active = 0; return; }
        s_pix_w = w;
        s_pix_h = h;
    }
    /* Clear to fully transparent */
    memset(s_pixels, 0, buf_size);

    /* ── Draw overlays (back to front) ── */

    if (need_seekbar) {
        draw_seekbar(s_pixels, w, h, ps);
        draw_menubar(s_pixels, w, h);
    }

    if (need_debug) {
        player_build_debug_info(ps);  /* refresh live data */
        draw_text_panel(s_pixels, w, h, ps->debug_info, 10, 40, 2);
    }

    if (need_info)
        draw_text_panel(s_pixels, w, h, ps->media_info, 10, 40, 2);

    if (need_pause)
        draw_pause(s_pixels, w, h);

    if (need_osd)
        draw_osd(s_pixels, w, h, osd_text);

    if (need_sub)
        draw_subtitles(s_pixels, w, h, ps);

    /* ── Upload to GPU ── */
    gpu_overlay_upload(ps, s_pixels, w, h);
    ps->overlay_active = 1;
}
