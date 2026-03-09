/*
 * DSVP — Dead Simple Video Player
 * main.c — Entry point, SDL initialization, event loop, overlays
 *
 * This is the application's main loop. It:
 *   1. Initializes SDL (video, audio, events)
 *   2. Creates the window and renderer
 *   3. Processes keyboard/mouse events
 *   4. Drives video decode and rendering
 *   5. Draws overlay text (debug, media info)
 *   6. Handles the native file-open dialog
 */

#include "dsvp.h"

/* Platform-specific file dialog */
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <commdlg.h>
#endif

/* ═══════════════════════════════════════════════════════════════════
 * File Open Dialog
 * ═══════════════════════════════════════════════════════════════════ */

/* Returns 1 if a file was selected (path written to `out`), 0 if cancelled. */
static int open_file_dialog(char *out, int out_size) {
#ifdef _WIN32
    /* Native Win32 file dialog — use Wide (Unicode) APIs so filenames
     * with non-ASCII characters (accented, CJK, fullwidth, etc.) work.
     * The result is converted to UTF-8 which FFmpeg expects. */
    OPENFILENAMEW ofn;
    wchar_t file[1024] = {0};

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = NULL;
    ofn.lpstrFile    = file;
    ofn.nMaxFile     = sizeof(file) / sizeof(file[0]);
    ofn.lpstrFilter  = L"Video Files\0"
                       L"*.mkv;*.mp4;*.avi;*.mov;*.wmv;*.flv;*.webm;*.m4v;*.ts;*.mpg;*.mpeg\0"
                       L"Audio Files\0"
                       L"*.mp3;*.flac;*.wav;*.aac;*.ogg;*.opus;*.m4a;*.wma\0"
                       L"All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags        = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        int len = WideCharToMultiByte(CP_UTF8, 0, file, -1, out, out_size, NULL, NULL);
        if (len > 0) return 1;
        log_msg("ERROR: UTF-8 conversion failed");
    }
    return 0;

#else
    /* Linux/macOS: try multiple dialog backends */
    FILE *fp = NULL;

    #ifdef __APPLE__
    fp = popen("osascript -e 'POSIX path of (choose file of type {\"public.movie\", \"public.audio\"})'", "r");
    #else
    /* Try zenity, then kdialog, then yad */
    const char *commands[] = {
        "zenity --file-selection --title='Open Media File' "
            "--file-filter='Media files|*.mkv *.mp4 *.avi *.mov *.wmv *.flv *.webm *.m4v *.ts *.mpg *.mpeg *.mp3 *.flac *.wav *.aac *.ogg *.opus *.m4a *.wma' "
            "--file-filter='All files|*' 2>/dev/null",
        "kdialog --getopenfilename . "
            "'Media files (*.mkv *.mp4 *.avi *.mov *.wmv *.flv *.webm *.m4v *.ts *.mpg *.mpeg *.mp3 *.flac *.wav *.aac *.ogg *.opus *.m4a *.wma)' 2>/dev/null",
        "yad --file-selection --title='Open Media File' 2>/dev/null",
        NULL
    };
    const char *names[] = { "zenity", "kdialog", "yad" };

    for (int i = 0; commands[i]; i++) {
        /* Check if the tool exists before trying it */
        char which_cmd[64];
        snprintf(which_cmd, sizeof(which_cmd), "which %s >/dev/null 2>&1", names[i]);
        if (system(which_cmd) == 0) {
            log_msg("File dialog: using %s", names[i]);
            fp = popen(commands[i], "r");
            break;
        }
    }

    if (!fp) {
        log_msg("ERROR: No file dialog available. Install zenity, kdialog, or yad.");
        log_msg("  Debian/Ubuntu: sudo apt install zenity");
        log_msg("  Fedora: sudo dnf install zenity");
        log_msg("  Tip: you can also pass a file path on the command line: ./dsvp video.mp4");
        return 0;
    }
    #endif

    if (!fp) return 0;

    if (fgets(out, out_size, fp)) {
        /* Remove trailing newline */
        size_t len = strlen(out);
        if (len > 0 && out[len - 1] == '\n') out[len - 1] = '\0';
        pclose(fp);
        return (strlen(out) > 0) ? 1 : 0;
    }
    pclose(fp);
    return 0;
#endif
}


/* ═══════════════════════════════════════════════════════════════════
 * Simple Text Overlay
 * ═══════════════════════════════════════════════════════════════════
 *
 * Phase 1 uses basic SDL rectangle-based text rendering.
 * Each character is drawn as a small filled rect (bitmap font style).
 * This is intentionally crude — Phase 2 will add proper GUI via Nuklear.
 *
 * For now we use a simple approach: render a semi-transparent background
 * box and draw monospace text character by character using a minimal
 * built-in bitmap font.
 */

/* Minimal 5x7 bitmap font - covers ASCII 32-126.
 * Each character is 5 columns × 7 rows, stored as 7 bytes (1 bit per pixel column).
 * This avoids any dependency on font files or SDL_ttf. */

static const uint8_t font_5x7[][7] = {
    /* ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* '!' */ {0x04,0x04,0x04,0x04,0x04,0x00,0x04},
    /* '"' */ {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00},
    /* '#' */ {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A},
    /* '$' */ {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04},
    /* '%' */ {0x18,0x19,0x02,0x04,0x08,0x13,0x03},
    /* '&' */ {0x08,0x14,0x14,0x08,0x15,0x12,0x0D},
    /* ''' */ {0x04,0x04,0x00,0x00,0x00,0x00,0x00},
    /* '(' */ {0x02,0x04,0x08,0x08,0x08,0x04,0x02},
    /* ')' */ {0x08,0x04,0x02,0x02,0x02,0x04,0x08},
    /* '*' */ {0x04,0x15,0x0E,0x1F,0x0E,0x15,0x04},
    /* '+' */ {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
    /* ',' */ {0x00,0x00,0x00,0x00,0x00,0x04,0x08},
    /* '-' */ {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    /* '.' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x04},
    /* '/' */ {0x01,0x01,0x02,0x04,0x08,0x10,0x10},
    /* '0' */ {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    /* '1' */ {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    /* '2' */ {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    /* '3' */ {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E},
    /* '4' */ {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    /* '5' */ {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    /* '6' */ {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
    /* '7' */ {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    /* '8' */ {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    /* '9' */ {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    /* ':' */ {0x00,0x00,0x04,0x00,0x00,0x04,0x00},
    /* ';' */ {0x00,0x00,0x04,0x00,0x00,0x04,0x08},
    /* '<' */ {0x02,0x04,0x08,0x10,0x08,0x04,0x02},
    /* '=' */ {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},
    /* '>' */ {0x08,0x04,0x02,0x01,0x02,0x04,0x08},
    /* '?' */ {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},
    /* '@' */ {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E},
    /* 'A' */ {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* 'B' */ {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    /* 'C' */ {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    /* 'D' */ {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    /* 'E' */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    /* 'F' */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    /* 'G' */ {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E},
    /* 'H' */ {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* 'I' */ {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    /* 'J' */ {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    /* 'K' */ {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    /* 'L' */ {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    /* 'M' */ {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    /* 'N' */ {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    /* 'O' */ {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* 'P' */ {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    /* 'Q' */ {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    /* 'R' */ {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    /* 'S' */ {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E},
    /* 'T' */ {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    /* 'U' */ {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* 'V' */ {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},
    /* 'W' */ {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
    /* 'X' */ {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    /* 'Y' */ {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    /* 'Z' */ {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    /* '[' */ {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},
    /* '\' */ {0x10,0x10,0x08,0x04,0x02,0x01,0x01},
    /* ']' */ {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},
    /* '^' */ {0x04,0x0A,0x11,0x00,0x00,0x00,0x00},
    /* '_' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
    /* '`' */ {0x08,0x04,0x00,0x00,0x00,0x00,0x00},
    /* 'a' */ {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F},
    /* 'b' */ {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E},
    /* 'c' */ {0x00,0x00,0x0E,0x11,0x10,0x11,0x0E},
    /* 'd' */ {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F},
    /* 'e' */ {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E},
    /* 'f' */ {0x06,0x08,0x1E,0x08,0x08,0x08,0x08},
    /* 'g' */ {0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E},
    /* 'h' */ {0x10,0x10,0x1E,0x11,0x11,0x11,0x11},
    /* 'i' */ {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E},
    /* 'j' */ {0x02,0x00,0x06,0x02,0x02,0x12,0x0C},
    /* 'k' */ {0x10,0x10,0x12,0x14,0x18,0x14,0x12},
    /* 'l' */ {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E},
    /* 'm' */ {0x00,0x00,0x1A,0x15,0x15,0x15,0x15},
    /* 'n' */ {0x00,0x00,0x1E,0x11,0x11,0x11,0x11},
    /* 'o' */ {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E},
    /* 'p' */ {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10},
    /* 'q' */ {0x00,0x00,0x0F,0x11,0x0F,0x01,0x01},
    /* 'r' */ {0x00,0x00,0x16,0x19,0x10,0x10,0x10},
    /* 's' */ {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E},
    /* 't' */ {0x08,0x08,0x1E,0x08,0x08,0x09,0x06},
    /* 'u' */ {0x00,0x00,0x11,0x11,0x11,0x11,0x0F},
    /* 'v' */ {0x00,0x00,0x11,0x11,0x0A,0x0A,0x04},
    /* 'w' */ {0x00,0x00,0x11,0x11,0x15,0x15,0x0A},
    /* 'x' */ {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11},
    /* 'y' */ {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E},
    /* 'z' */ {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F},
    /* '{' */ {0x02,0x04,0x04,0x08,0x04,0x04,0x02},
    /* '|' */ {0x04,0x04,0x04,0x04,0x04,0x04,0x04},
    /* '}' */ {0x08,0x04,0x04,0x02,0x04,0x04,0x08},
    /* '~' */ {0x00,0x00,0x08,0x15,0x02,0x00,0x00},
};

/* Draw a single character at (x,y) using the bitmap font. Scale=pixel size. */
static void draw_char(SDL_Renderer *r, int x, int y, char ch, int scale) {
    if (ch < 32 || ch > 126) ch = '?';
    int idx = ch - 32;

    for (int row = 0; row < 7; row++) {
        uint8_t bits = font_5x7[idx][row];
        for (int col = 0; col < 5; col++) {
            if (bits & (1 << (4 - col))) {
                SDL_Rect px = { x + col * scale, y + row * scale, scale, scale };
                SDL_RenderFillRect(r, &px);
            }
        }
    }
}

/* Draw a string. Returns the Y position after the last line. */
static int draw_text(SDL_Renderer *r, int x, int y, const char *text,
                     int scale, SDL_Color fg) {
    SDL_SetRenderDrawColor(r, fg.r, fg.g, fg.b, fg.a);

    int cx = x, cy = y;
    int char_w = 6 * scale;  /* 5px + 1px gap */
    int char_h = 8 * scale;  /* 7px + 1px gap */

    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            cx = x;
            cy += char_h;
            continue;
        }
        draw_char(r, cx, cy, *p, scale);
        cx += char_w;
    }
    return cy + char_h;
}

/* Draw a semi-transparent overlay background, then text on top. */
static void draw_overlay(SDL_Renderer *renderer, const char *text,
                         int x, int y, int scale) {
    /* Estimate text bounds */
    int max_w = 0, cur_w = 0, lines = 1;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') { lines++; cur_w = 0; }
        else { cur_w++; if (cur_w > max_w) max_w = cur_w; }
    }
    int pad = 8;
    int box_w = max_w * 6 * scale + pad * 2;
    int box_h = lines * 8 * scale + pad * 2;

    /* Background */
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
    SDL_Rect bg = { x - pad, y - pad, box_w, box_h };
    SDL_RenderFillRect(renderer, &bg);

    /* Text */
    SDL_Color white = {220, 220, 220, 255};
    draw_text(renderer, x, y, text, scale, white);
}


/* ═══════════════════════════════════════════════════════════════════
 * Seek Bar & Volume Overlay
 * ═══════════════════════════════════════════════════════════════════ */

static void draw_seek_bar(PlayerState *ps) {
    if (!ps->playing || !ps->fmt_ctx) return;

    double duration = (ps->fmt_ctx->duration != AV_NOPTS_VALUE)
        ? (double)ps->fmt_ctx->duration / AV_TIME_BASE : 0.0;
    if (duration <= 0.0) return;

    double pos = ps->video_clock;
    double frac = pos / duration;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;

    int w, h;
    SDL_GetWindowSize(ps->window, &w, &h);

    int bar_h   = 4;
    int bar_y   = h - 30;
    int bar_x   = 20;
    int bar_w   = w - 40;

    SDL_SetRenderDrawBlendMode(ps->renderer, SDL_BLENDMODE_BLEND);

    /* Track background */
    SDL_SetRenderDrawColor(ps->renderer, 100, 100, 100, 150);
    SDL_Rect track = { bar_x, bar_y, bar_w, bar_h };
    SDL_RenderFillRect(ps->renderer, &track);

    /* Filled portion */
    SDL_SetRenderDrawColor(ps->renderer, 200, 200, 200, 220);
    SDL_Rect filled = { bar_x, bar_y, (int)(bar_w * frac), bar_h };
    SDL_RenderFillRect(ps->renderer, &filled);

    /* Time text */
    int pos_m = (int)pos / 60, pos_s = (int)pos % 60;
    int dur_m = (int)duration / 60, dur_s = (int)duration % 60;
    char time_str[64];
    snprintf(time_str, sizeof(time_str), "%d:%02d / %d:%02d", pos_m, pos_s, dur_m, dur_s);
    SDL_Color dim = {180, 180, 180, 200};
    draw_text(ps->renderer, bar_x, bar_y + 8, time_str, 1, dim);

    /* Volume indicator */
    char vol_str[32];
    snprintf(vol_str, sizeof(vol_str), "Vol: %.0f%%", ps->volume * 100.0);
    draw_text(ps->renderer, bar_x + bar_w - 60, bar_y + 8, vol_str, 1, dim);
}


/* ═══════════════════════════════════════════════════════════════════
 * Idle Screen (no media loaded)
 * ═══════════════════════════════════════════════════════════════════ */

static void draw_idle_screen(PlayerState *ps) {
    SDL_SetRenderDrawColor(ps->renderer, 24, 24, 28, 255);
    SDL_RenderClear(ps->renderer);

    int w, h;
    SDL_GetWindowSize(ps->window, &w, &h);

    const char *title = "DSVP";
    const char *ver   = "Dead Simple Video Player v" DSVP_VERSION;
    const char *help  =
        "[O] Open file\n"
        "[Q] Quit\n"
        "[F] Fullscreen\n"
        "[A] Cycle audio tracks\n"
        "[S] Cycle subtitles\n"
        "[D] Debug overlay\n"
        "[I] Media info\n"
        "\n"
        "Arrow keys: seek / volume\n"
        "Space: pause/resume\n"
        "Double-click: fullscreen";

    /* Title */
    int scale = 3;
    int tx = (w - 4 * 6 * scale) / 2;
    SDL_Color bright = {200, 200, 210, 255};
    draw_text(ps->renderer, tx, h / 4, title, scale, bright);

    /* Subtitle */
    scale = 1;
    int sx = (w - (int)strlen(ver) * 6 * scale) / 2;
    SDL_Color dim = {120, 120, 130, 255};
    draw_text(ps->renderer, sx, h / 4 + 30, ver, scale, dim);

    /* Help */
    SDL_Color help_col = {160, 160, 170, 255};
    draw_text(ps->renderer, 30, h / 2, help, 2, help_col);
}


/* ═══════════════════════════════════════════════════════════════════
 * Hover Menu
 * ═══════════════════════════════════════════════════════════════════ */

/* Track mouse hover state for the top menu bar */
typedef struct {
    int visible;
} MenuState;

static void draw_menu(PlayerState *ps, MenuState *menu) {
    if (!menu->visible) return;

    int w, h;
    SDL_GetWindowSize(ps->window, &w, &h);

    /* Menu bar background */
    SDL_SetRenderDrawBlendMode(ps->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ps->renderer, 30, 30, 34, 220);
    SDL_Rect bar = { 0, 0, w, 32 };
    SDL_RenderFillRect(ps->renderer, &bar);

    /* Menu items */
    SDL_Color menu_col = {180, 180, 190, 255};
    const char *items = "[O]Open  [A]Audio  [S]Subs  [F]Fullscreen  [D]Debug  [I]Info  [Q]Quit";
    draw_text(ps->renderer, 10, 10, items, 1, menu_col);
}


/* ═══════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    /* ── Initialize logging (before anything else) ── */
    log_init();
    log_msg("Starting DSVP (argc=%d)", argc);

    /* ── Initialize SDL ── */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
        fprintf(stderr, "[DSVP] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Suppress FFmpeg's internal warnings (container quirks, timestamp
     * heuristics, etc.). In debug builds, keep them visible. */
#ifdef DSVP_DEBUG
    av_log_set_level(AV_LOG_VERBOSE);
#else
    av_log_set_level(AV_LOG_ERROR);
#endif

    /* ── Create window ── */
    SDL_Window *window = SDL_CreateWindow(
        DSVP_WINDOW_TITLE,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        DEFAULT_WIN_W, DEFAULT_WIN_H,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!window) {
        fprintf(stderr, "[DSVP] Cannot create window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    /* ── Create renderer ──
     * SDL_RENDERER_SOFTWARE forces CPU rendering (no GPU compositing).
     * We use PRESENTVSYNC to match the display refresh rate. */
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        /* Fallback: try without VSYNC */
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        fprintf(stderr, "[DSVP] Cannot create renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    /* ── Initialize subtitle font ── */
    if (sub_init_font() < 0) {
        log_msg("WARNING: Subtitle rendering disabled (no font)");
    }

    /* ── Initialize player state ── */
    PlayerState ps;
    memset(&ps, 0, sizeof(ps));
    ps.window   = window;
    ps.renderer = renderer;
    ps.volume   = 0.75;
    ps.video_stream_idx = -1;
    ps.audio_stream_idx = -1;
    ps.sub_active_idx   = -1;
    ps.win_w = DEFAULT_WIN_W;
    ps.win_h = DEFAULT_WIN_H;

    /* ── Overlay visibility state ── */
    MenuState menu = {0};
    int show_overlays = 1;          /* controls both seek bar and menu */
    double overlay_hide_time = 0.0; /* hide after this timestamp       */
    double overlay_timeout = 3.0;   /* seconds of inactivity to hide   */

    /* ── Open file from command line if provided ── */
    if (argc > 1) {
        if (player_open(&ps, argv[1]) != 0) {
            log_msg("ERROR: Failed to open: %s", argv[1]);
        } else {
            show_overlays = 1;
            overlay_hide_time = get_time_sec() + overlay_timeout;
        }
    }

    /* ── Main loop ── */
    while (!ps.quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {

            case SDL_QUIT:
                ps.quit = 1;
                break;

            case SDL_KEYDOWN:
                switch (ev.key.keysym.sym) {

                case SDLK_q:
                    if (ps.playing) {
                        player_close(&ps);
                        ps.quit = 0; /* don't exit, return to idle */
                    } else {
                        ps.quit = 1;
                    }
                    break;

                case SDLK_o: {
                    char path[1024] = {0};
                    log_msg("File open dialog requested");
                    if (open_file_dialog(path, sizeof(path))) {
                        log_msg("Opening file: %s", path);
                        if (ps.playing) player_close(&ps);
                        ps.quit = 0;
                        if (player_open(&ps, path) != 0) {
                            log_msg("ERROR: Failed to open: %s", path);
                        } else {
                            show_overlays = 1;
                            overlay_hide_time = get_time_sec() + overlay_timeout;
                        }
                    } else {
                        log_msg("File dialog cancelled");
                    }
                    break;
                }

                case SDLK_SPACE:
                    if (ps.playing) {
                        ps.paused = !ps.paused;
                        if (ps.audio_dev) {
                            SDL_PauseAudioDevice(ps.audio_dev, ps.paused);
                        }
                        if (!ps.paused) {
                            ps.frame_timer = get_time_sec();
                        }
                        show_overlays = 1;
                        overlay_hide_time = get_time_sec() + overlay_timeout;
                    }
                    break;

                case SDLK_f:
                    ps.fullscreen = !ps.fullscreen;
                    SDL_SetWindowFullscreen(window,
                        ps.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    break;

                case SDLK_d:
                    ps.show_debug = !ps.show_debug;
                    break;

                case SDLK_i:
                    ps.show_info = !ps.show_info;
                    break;

                case SDLK_s:
                    sub_cycle(&ps);
                    show_overlays = 1;
                    overlay_hide_time = get_time_sec() + overlay_timeout;
                    break;

                case SDLK_a:
                    audio_cycle(&ps);
                    show_overlays = 1;
                    overlay_hide_time = get_time_sec() + overlay_timeout;
                    break;

                case SDLK_LEFT:
                    player_seek(&ps, -SEEK_STEP_SEC);
                    show_overlays = 1;
                    overlay_hide_time = get_time_sec() + overlay_timeout;
                    break;

                case SDLK_RIGHT:
                    player_seek(&ps, SEEK_STEP_SEC);
                    show_overlays = 1;
                    overlay_hide_time = get_time_sec() + overlay_timeout;
                    break;

                case SDLK_UP:
                    ps.volume += VOLUME_STEP;
                    if (ps.volume > 1.0) ps.volume = 1.0;
                    show_overlays = 1;
                    overlay_hide_time = get_time_sec() + overlay_timeout;
                    break;

                case SDLK_DOWN:
                    ps.volume -= VOLUME_STEP;
                    if (ps.volume < 0.0) ps.volume = 0.0;
                    show_overlays = 1;
                    overlay_hide_time = get_time_sec() + overlay_timeout;
                    break;

                default:
                    break;
                }
                break;

            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT && ev.button.clicks == 2) {
                    /* Double-click → toggle fullscreen */
                    ps.fullscreen = !ps.fullscreen;
                    SDL_SetWindowFullscreen(window,
                        ps.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                }

                /* Click on seek bar to seek */
                if (ev.button.button == SDL_BUTTON_LEFT && ps.playing) {
                    int w_now, h_now;
                    SDL_GetWindowSize(window, &w_now, &h_now);
                    int bar_y = h_now - 30;
                    if (ev.button.y >= bar_y - 10 && ev.button.y <= bar_y + 20) {
                        double frac = (double)(ev.button.x - 20) / (w_now - 40);
                        if (frac < 0.0) frac = 0.0;
                        if (frac > 1.0) frac = 1.0;
                        double duration = (ps.fmt_ctx->duration != AV_NOPTS_VALUE)
                            ? (double)ps.fmt_ctx->duration / AV_TIME_BASE : 0.0;
                        double target = frac * duration;
                        player_seek(&ps, target - ps.video_clock);
                    }
                }
                break;

            case SDL_MOUSEMOTION:
                /* Show overlays on any mouse movement */
                show_overlays = 1;
                overlay_hide_time = get_time_sec() + overlay_timeout;
                SDL_ShowCursor(SDL_ENABLE);
                break;

            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    ps.win_w = ev.window.data1;
                    ps.win_h = ev.window.data2;
                }
                break;
            }
        }

        /* ── Render ── */
        if (ps.playing && !ps.paused) {
            /* Decode pending subtitles */
            sub_decode_pending(&ps);
            /* Decode and display video frames with A/V sync */
            double now = get_time_sec();

            int vret = video_decode_frame(&ps);
            if (vret > 0) {
                /* Compute delay based on A/V sync */
                double pts_delay = ps.video_clock - ps.frame_last_pts;
                if (pts_delay <= 0.0 || pts_delay >= 1.0) {
                    pts_delay = ps.frame_last_delay;
                }
                ps.frame_last_pts   = ps.video_clock;
                ps.frame_last_delay = pts_delay;

                /* Sync to audio */
                double delay = pts_delay;
                if (ps.audio_stream_idx >= 0) {
                    double diff = ps.video_clock - ps.audio_clock;
                    double threshold = fmax(pts_delay, 0.01);

                    if (diff > threshold) {
                        delay = pts_delay + diff;
                    } else if (diff < -threshold) {
                        delay = 0.0;
                    }
                }

                ps.frame_timer += delay;
                double actual_delay = ps.frame_timer - now;
                if (actual_delay > 0.0 && actual_delay < 1.0) {
                    SDL_Delay((Uint32)(actual_delay * 1000.0));
                }

                video_display(&ps);
            } else if (vret < 0) {
                log_msg("Video decode error at clock=%.3f", ps.video_clock);
            } else if (ps.eof && ps.video_pq.nb_packets == 0
                            && ps.audio_pq.nb_packets == 0) {
                /* End of file — return to idle screen */
                log_msg("Playback finished, returning to idle");
                player_close(&ps);
                ps.quit = 0;
            }
        } else if (ps.playing && ps.paused) {
            /* Paused — decode pending subs, redraw current frame */
            sub_decode_pending(&ps);
            if (ps.texture) {
                SDL_SetRenderDrawColor(ps.renderer, 0, 0, 0, 255);
                SDL_RenderClear(ps.renderer);
                player_update_display_rect(&ps);
                SDL_RenderCopy(ps.renderer, ps.texture, NULL, &ps.display_rect);
            }
        } else {
            /* No media loaded — draw idle screen */
            draw_idle_screen(&ps);
            SDL_ShowCursor(SDL_ENABLE);
        }

        /* ── Draw overlays ── */
        if (ps.playing) {
            /* Subtitles — always visible when active (independent of overlay timer) */
            sub_render(&ps, ps.renderer, ps.win_w, ps.win_h);
            /* Auto-hide timer check */
            if (show_overlays && get_time_sec() > overlay_hide_time) {
                show_overlays = 0;
                SDL_ShowCursor(SDL_DISABLE);
            }

            /* Seek bar + menu bar (unified visibility) */
            if (show_overlays) {
                draw_seek_bar(&ps);
                menu.visible = 1;
                draw_menu(&ps, &menu);
            } else {
                menu.visible = 0;
            }

            /* Debug overlay */
            if (ps.show_debug) {
                player_build_debug_info(&ps);
                draw_overlay(ps.renderer, ps.debug_info, 10, 40, 2);
            }

            /* Media info overlay */
            if (ps.show_info) {
                draw_overlay(ps.renderer, ps.media_info, 10, 40, 2);
            }

            /* Pause indicator */
            if (ps.paused) {
                int w, h;
                SDL_GetWindowSize(window, &w, &h);
                SDL_Color pause_col = {200, 200, 200, 180};
                draw_text(ps.renderer, w / 2 - 30, h / 2 - 10,
                          "PAUSED", 2, pause_col);
            }
        }

        /* Present everything */
        SDL_RenderPresent(ps.renderer);

        /* Don't burn CPU when idle */
        if (!ps.playing || ps.paused) {
            SDL_Delay(16); /* ~60fps idle */
        }
    }

    /* ── Cleanup ── */
    log_msg("Shutting down");
    if (ps.playing) player_close(&ps);
    sub_close_font();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    log_close();

    return 0;
}
