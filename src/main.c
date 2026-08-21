/*
 * DSVP — Dead Simple Video Player
 * main.c — Entry point, SDL initialization, event loop
 *
 * This is the application's main loop. It:
 *   1. Initializes SDL (video, audio, events)
 *   2. Creates the window and GPU device (SDL_GPU)
 *   3. Compiles HLSL shaders via shadercross
 *   4. Processes keyboard/mouse events
 *   5. Drives video decode and rendering via GPU
 *
 * Phase 2 (v0.1.4-beta): Full GPU rendering with overlay system.
 * Video frames rendered via custom HLSL shaders (SDL_GPU). Overlays
 * (debug, info, seek bar, subtitles, OSD) composited as RGBA texture
 * with alpha blending over the video quad.
 */

#include "dsvp.h"
#include "dsvp_icon.h"
#include <dirent.h>
#include <signal.h>

/* Baked in by the Makefile (git short SHA, +dirty when the tree is modified).
 * Fallback keeps the file compilable outside the build system. */
#ifndef DSVP_GIT_COMMIT
#define DSVP_GIT_COMMIT "unknown"
#endif

/* Ctrl-C / SIGTERM must not skip cleanup. The dangerous case is being
 * killed mid-passthrough: the receiver is being fed IEC 61937 and the
 * stream just stops without the non-audio bit being cleared, which is
 * exactly the state that latches some TVs (LG OLEDs confirmed) into
 * silence. Flag it here, let the main loop exit normally, and every
 * teardown path runs as usual. */
static volatile sig_atomic_t g_signal_quit = 0;
static void on_terminate_signal(int sig) { (void)sig; g_signal_quit = 1; }

/* ═══════════════════════════════════════════════════════════════════
 * Pacing v2 (DSVP_PACING=v2) — median cadence sensor + explicit
 * LOCKED/SCHEDULED mode machine. Design: docs/DESIGN-PACING.md.
 * Batch 2 scope: the machine decides the mode; the mode BODIES are
 * still the v1 mechanisms (LOCKED = the 1:1 path, SCHEDULED = the
 * accumulator + drop stack). Inert unless the env flag is set.
 * ═══════════════════════════════════════════════════════════════════ */

/* Called once per PRESENTED tick (video_display or video_reblit —
 * drop ticks present nothing and are not heartbeats). Maintains the
 * median of the last 32 presented-frame intervals (a mean chases
 * transition outliers: field tick=25.3 described no real cadence)
 * and drives the cadence half of the mode contracts: LOCKED entry
 * needs a 1.5% match sustained 30 presents AND settled drift; the
 * cadence exit needs 4% mismatch sustained 10. The drift exit
 * contract lives in the consume path (per displayed frame). */
static void pacing_v2_present_tick(PlayerState *ps, double now) {
    if (!ps->playing)
        return;

    /* Content cadence: EMA of pts deltas, not the last delta. A
     * 1ms-quantized container timebase makes 59.94fps deltas
     * alternate ~16/17ms; comparing the 32-median against ONE such
     * sample can never hold the 0.25ms entry tolerance for 30
     * straight presents (PACE-DIAG conviction 2026-08-20: streak
     * peaked at 6 in 124s while the median sat rock-steady). The
     * machine locks to the AVERAGE cadence; per-delta wobble is
     * quantization, not content. */
    if (ps->frame_last_delay > 0.0) {
        if (ps->pace_content_ema <= 0.0)
            ps->pace_content_ema = ps->frame_last_delay;
        else
            ps->pace_content_ema = ps->pace_content_ema * 0.95
                                 + ps->frame_last_delay * 0.05;
    }

    /* Measure: interval between presents; transients >100ms are
     * stall/seek boundaries, not cadence. Gaps spanning DROPPED
     * ticks are not cadence either — a drop presents nothing, so
     * the next present's dt is ~2 slots and each slip-crossing
     * burst poisoned the median for ~half a second (field:
     * median=20.00ms right after the startup burst). */
    if (ps->pace_last_present > 0.0) {
        double dt = now - ps->pace_last_present;
        if (dt < 0.1
                && (ps->pace_content_ema <= 0.0
                    || dt < ps->pace_content_ema * 1.5)) {
            ps->pace_ring[ps->pace_ring_pos] = dt;
            ps->pace_ring_pos = (ps->pace_ring_pos + 1) % 32;
            if (ps->pace_ring_n < 32)
                ps->pace_ring_n++;
            if (ps->pace_ring_n >= 8) {
                double sorted[32];
                memcpy(sorted, ps->pace_ring,
                       ps->pace_ring_n * sizeof(double));
                for (int i = 1; i < ps->pace_ring_n; i++) {
                    double v = sorted[i];
                    int j = i - 1;
                    while (j >= 0 && sorted[j] > v) {
                        sorted[j + 1] = sorted[j];
                        j--;
                    }
                    sorted[j + 1] = v;
                }
                ps->pace_median = sorted[ps->pace_ring_n / 2];
            }
        }
    }
    ps->pace_last_present = now;

    /* Mode machine — cadence contracts (smoothed content side). */
    double content_dt = (ps->pace_content_ema > 0.0)
                        ? ps->pace_content_ema : ps->frame_last_delay;
    int rate_ok = (content_dt > 0.001 && content_dt < 0.020);
    if (ps->pace_median <= 0.0 || !rate_ok) {
        ps->pace_enter_streak = 0;
        /* Content left the 1:1 range (playlist advance mid-mode):
         * LOCKED is meaningless, exit now. A merely-invalid median
         * (ring refilling after a window hint) holds the mode — the
         * drift exit contract keeps guarding meanwhile. */
        if (ps->pace_mode == PACE_LOCKED && !rate_ok) {
            ps->pace_mode = PACE_SCHEDULED;
            ps->pace_exit_streak  = 0;
            ps->pace_drift_streak = 0;
            log_msg("PACE: -> SCHEDULED (content cadence %.1fms left "
                    "the 1:1 range)", content_dt * 1000.0);
        }
        return;
    }

    double err = fabs(ps->pace_median - content_dt);
    /* PACE-DIAG (rate-limited): the entry contract's live state, so a
     * never-locking file names WHICH gate fails instead of leaving it
     * to inference. Added hunting the 1080p60 case: 138s in SCHEDULED
     * with clean ticks and (apparently) satisfiable gates, zero
     * entries — and the periodic drop bursts feed ~33ms present gaps
     * into the cadence ring, so the median may be resetting the
     * streak every slip crossing. This line settles it in one run. */
    if (ps->pace_mode == PACE_SCHEDULED) {
        static double s_pace_diag_last = 0.0;
        if (now - s_pace_diag_last >= 5.0) {
            s_pace_diag_last = now;
            log_msg("PACE-DIAG: median=%.2fms content=%.2fms "
                    "err=%.2fms streak=%d av_diff=%.1fms bias=%.1fms",
                    ps->pace_median * 1000.0, content_dt * 1000.0,
                    err * 1000.0,
                    ps->pace_enter_streak,
                    ps->last_av_diff * 1000.0, ps->av_bias * 1000.0);
        }
    }
    if (ps->pace_mode == PACE_SCHEDULED) {
        if (err < content_dt * 0.015) {
            ps->pace_enter_streak++;
            /* ENTRY CONTRACT: cadence match sustained AND drift
             * settled — never enter LOCKED owing frames (review F2:
             * LOCKED cannot repay). Video-only owes nothing. */
            double resid = (ps->audio_stream_idx >= 0)
                ? fabs(ps->last_av_diff - ps->av_bias) : 0.0;
            if (ps->pace_enter_streak >= 30 && resid < content_dt) {
                ps->pace_mode = PACE_LOCKED;
                ps->pace_enter_streak = 0;
                ps->pace_exit_streak  = 0;
                ps->pace_drift_streak = 0;
                /* Latency reference for this LOCKED residency: a
                 * snapshot. The live EMA keeps learning (that is
                 * LOCKED's job) but exits measure against the value
                 * at entry — an EMA chases slow drift and absorbs it
                 * (the slow-sink case); a snapshot cannot. */
                ps->pace_bias_ref = ps->av_bias;
                log_msg("PACE: -> LOCKED (median %.2fms, content "
                        "%.2fms, resid drift %.1fms)",
                        ps->pace_median * 1000.0, content_dt * 1000.0,
                        resid * 1000.0);
            }
        } else {
            ps->pace_enter_streak = 0;
        }
    } else {
        if (err > content_dt * 0.04) {
            if (++ps->pace_exit_streak >= 10) {
                ps->pace_mode = PACE_SCHEDULED;
                ps->pace_enter_streak = 0;
                ps->pace_exit_streak  = 0;
                ps->pace_drift_streak = 0;
                log_msg("PACE: -> SCHEDULED (cadence mismatch: median "
                        "%.2fms vs content %.2fms)",
                        ps->pace_median * 1000.0, content_dt * 1000.0);
            }
        } else {
            ps->pace_exit_streak = 0;
        }
    }
}

/* Window-event hint: geometry changed, cadence may be about to
 * change. Restart the measurement window and sustain streaks so the
 * machine re-decides from fresh data. Hints never switch modes
 * themselves — fullscreen ≠ direct scanout (KWin re-promotion is
 * fullscreen at degraded cadence and must read as such). */
static void pacing_v2_window_hint(PlayerState *ps) {
    ps->pace_ring_n       = 0;
    ps->pace_ring_pos     = 0;
    ps->pace_median       = 0.0;
    ps->pace_last_present = 0.0;
    ps->pace_enter_streak = 0;
    ps->pace_exit_streak  = 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * Folder Playlist — prev/next file navigation
 * ═══════════════════════════════════════════════════════════════════
 *
 * Scans the parent directory of the current file for playable media,
 * sorts alphabetically, and allows navigating to adjacent entries.
 */

/* Video containers only: player_open hard-requires a video stream, so
 * advertising audio extensions (.mp3/.flac/…) put unopenable files in
 * the browser and playlist — EOF auto-advance hitting a coverless
 * soundtrack file dumped the user back to the browser (review P2-15).
 * Re-add them when audio-only playback exists. */
/* Adding an extension here means adding its demuxer to the
 * format_whitelist in player_open (player.c) — the two lists must
 * stay in step or the new type opens in the browser and fails in the
 * player. */
const char *video_extensions[] = {
    ".mkv", ".mp4", ".avi", ".mov", ".wmv", ".flv", ".webm", ".m4v",
    ".ts", ".m2ts", ".mpg", ".mpeg", ".3gp",
    NULL
};

/* HDR midtone gain — file-scope so it can be reset on file open.
 * Index 3 = 1.3f, the default. gain_reset writes BOTH the index and
 * the live uniform: resetting only the index left the effective gain
 * carried over from the previous file while the OSD claimed default —
 * first G press then jumped two steps at once (review P2-14). */
static const float s_gain_table[6] = { 1.0f, 1.1f, 1.2f, 1.3f, 1.35f, 1.4f };
static int s_gain_idx = 3;
static void gain_reset(PlayerState *ps) {
    s_gain_idx = 3;
    ps->gpu_uniforms.hdr_midtone_gain = s_gain_table[s_gain_idx];
}

int is_media_file(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (int i = 0; video_extensions[i]; i++) {
        if (strcasecmp(dot, video_extensions[i]) == 0) return 1;
    }
    return 0;
}

/* Natural-order, case-insensitive compare (DSVP main 7f09ae0): digit
 * runs compare as numbers, so "E2" sorts before "E10" — byte-wise
 * folding played episodes out of story order in any season folder with
 * unpadded numbering. ASCII folding; deterministic leading-zero
 * tie-break keeps the ordering a strict total order for qsort. */
int natural_casecmp(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= '0' && ca <= '9' && cb >= '0' && cb <= '9') {
            const char *pa = a, *pb = b;
            while (*pa == '0') pa++;
            while (*pb == '0') pb++;
            const char *da = pa, *db = pb;
            while (*da >= '0' && *da <= '9') da++;
            while (*db >= '0' && *db <= '9') db++;
            ptrdiff_t la = da - pa, lb = db - pb;
            if (la != lb) return (la < lb) ? -1 : 1;
            for (; pa < da; pa++, pb++)
                if (*pa != *pb) return (*pa < *pb) ? -1 : 1;
            ptrdiff_t za = pa - a - la, zb = pb - b - lb;
            if (za != zb) return (za < zb) ? -1 : 1;
            a = da; b = db;
            continue;
        }
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (ca < cb) ? -1 : 1;
        a++; b++;
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

static int cmp_strings(const void *a, const void *b) {
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    return natural_casecmp(sa, sb);
}

static void playlist_free(PlayerState *ps) {
    if (ps->playlist_files) {
        for (int i = 0; i < ps->playlist_count; i++)
            free(ps->playlist_files[i]);
        free(ps->playlist_files);
        ps->playlist_files = NULL;
    }
    ps->playlist_count = 0;
    ps->playlist_index = -1;
}

/* Scan the directory containing `filepath` for playable media files.
 * Populates ps->playlist_files (sorted), playlist_count, playlist_index. */
static void playlist_scan(PlayerState *ps) {
    playlist_free(ps);
    if (!ps->filepath[0]) return;

    /* Extract directory and filename from filepath */
    char dir[1024], base[1024];
    snprintf(dir, sizeof(dir), "%s", ps->filepath);

    /* Find last separator */
    char *sep = strrchr(dir, '/');
    if (sep) {
        snprintf(base, sizeof(base), "%s", sep + 1);
        *(sep + 1) = '\0';  /* dir now ends with separator */
    } else {
        snprintf(base, sizeof(base), "%s", dir);
        /* Trailing slash matters: the entry constructor below joins
         * with "%s%s" — bare "." produced ".<name>" paths and a
         * permanently-lost playlist index (review P2-12). */
        strcpy(dir, "./");
    }

    /* Scan directory */
    int capacity = 64;
    char **files = malloc(capacity * sizeof(char *));
    if (!files) return;
    int count = 0;

    /* POSIX: opendir/readdir */
    {
        DIR *d = opendir(dir);
        if (!d) {
            log_msg("playlist_scan: cannot open directory: %s",
                    log_path(dir));
            free(files);
            return;
        }

        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (!is_media_file(ent->d_name)) continue;

            char fullpath[2048];
            snprintf(fullpath, sizeof(fullpath), "%s%s", dir, ent->d_name);

            if (count >= capacity) {
                capacity *= 2;
                char **tmp = realloc(files, capacity * sizeof(char *));
                if (!tmp) break;
                files = tmp;
            }
            files[count] = strdup(fullpath);
            if (!files[count]) break;
            count++;
        }
        closedir(d);
    }

    if (count == 0) {
        free(files);
        return;
    }

    /* Sort alphabetically */
    qsort(files, count, sizeof(char *), cmp_strings);

    ps->playlist_files = files;
    ps->playlist_count = count;

    /* Find current file's index */
    ps->playlist_index = -1;
    for (int i = 0; i < count; i++) {
        /* Compare against full filepath. Relative launches store the
         * bare name while entries carry the "./" prefix — strip it
         * for the comparison (P2-12). */
        const char *entry = files[i];
        if (entry[0] == '.' && entry[1] == '/'
                && strchr(ps->filepath, '/') == NULL)
            entry += 2;
        if (strcmp(entry, ps->filepath) == 0) {
            ps->playlist_index = i;
            break;
        }
    }

    log_msg("playlist_scan: %d files in folder, current index=%d",
            count, ps->playlist_index);
}


/* ═══════════════════════════════════════════════════════════════════
 * GPU Idle Screen (no media loaded)
 * ═══════════════════════════════════════════════════════════════════
 *
 * Dark background with DSVP title, version, and hotkey reference
 * rendered via the overlay system.
 */

static void gpu_draw_idle(PlayerState *ps) {
    /* Update physical pixel dimensions for the idle window.
     * After player_close resets the window to 960×540, the stale
     * sc_w/sc_h from the video session would cause overlay_render_idle
     * to draw at the wrong size. */
    int phys_w, phys_h;
    SDL_GetWindowSizeInPixels(ps->window, &phys_w, &phys_h);
    ps->sc_w = phys_w;
    ps->sc_h = phys_h;

    /* Render browser or idle screen to overlay pixel buffer */
    if (ps->browser_active)
        overlay_render_browser(ps);
    else
        overlay_render_idle(ps);

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(ps->gpu_device);
    if (!cmd) return;

    /* Upload overlay texture if dirty */
    gpu_overlay_copy_cmd(cmd, ps);

    SDL_GPUTexture *swapchain_tex = NULL;
    Uint32 sc_w, sc_h;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, ps->window,
            &swapchain_tex, &sc_w, &sc_h)) {
        SDL_CancelGPUCommandBuffer(cmd);
        /* Was silent (same class as the reblit acquire — see player.c). */
        static int s_idle_acq_fail = 0;
        if (s_idle_acq_fail++ % 60 == 0)
            log_msg("WARN: idle swapchain acquire failed (x%d): %s",
                    s_idle_acq_fail, SDL_GetError());
        return;
    }
    if (!swapchain_tex) {
        SDL_CancelGPUCommandBuffer(cmd);
        static int s_idle_null_tex = 0;
        if (s_idle_null_tex++ % 60 == 0)
            log_msg("WARN: idle acquire returned no texture (x%d)",
                    s_idle_null_tex);
        return;
    }

    /* Dark background — same color as old idle screen (24, 24, 28) */
    SDL_GPUColorTargetInfo color_target;
    SDL_zero(color_target);
    color_target.texture    = swapchain_tex;
    color_target.clear_color = (SDL_FColor){ 0.094f, 0.094f, 0.110f, 1.0f };
    color_target.load_op    = SDL_GPU_LOADOP_CLEAR;
    color_target.store_op   = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, NULL);
    {
        /* Overlay quad with title/version/hotkeys */
        gpu_overlay_draw(pass, cmd, ps, sc_w, sc_h);
    }
    SDL_EndGPURenderPass(pass);

    SDL_SubmitGPUCommandBuffer(cmd);
    ps->presents++;
}


/* ═══════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Fullscreen toggle — the ONLY place that changes fullscreen state ──
 *
 * Both the F key and double-click route here. They used to be separate
 * implementations: F did an exclusive modeset (SDL_SetWindowFullscreenMode
 * with a concrete mode) and cleared it on exit, while double-click called
 * SDL_SetWindowFullscreen alone. Because the mode is sticky per-window,
 * mixing them produced order-dependent behaviour — a double-click after an
 * F-cycle could enter exclusive fullscreen with a mode chosen for a display
 * that may no longer be attached — and the double-click exit skipped the
 * aspect-ratio window restore, leaving a stale window shape.
 *
 * Exclusive fullscreen is deliberate: SDL_WINDOW_FULLSCREEN alone gives
 * borderless desktop fullscreen, which drifted at 4K60 on the dock. */
/* ── Fullscreen — borderless only, by design ──────────────────────────
 *
 * DSVP never changes the display mode. It asks for borderless desktop
 * fullscreen, which reuses whatever mode the desktop is already running.
 *
 * There used to be an "exclusive" path that called
 * SDL_SetWindowFullscreenMode() with a concrete SDL_DisplayMode, added long
 * ago to chase 4K60 pacing drift on an older software stack. It was removed
 * 2026-07-31 because it issued a real display modeset on every keypress, and
 * a modeset forces the whole link to renegotiate — through a dock's DP->HDMI
 * converter, a TV's input handling, and whatever else sits in the path. On
 * the Steam Deck dock that renegotiation does not reliably come back clean:
 * observed outcomes included the picture landing vertically offset, a blank
 * screen needing manual recovery, and SDL's mode list going stale after a
 * hotplug so fullscreen silently stopped working entirely.
 *
 * No end user should be able to reach any of those states by pressing F in a
 * video player, and nothing ever demonstrated exclusive producing a better
 * picture than borderless. The safe path is the only path.
 *
 * ps->fullscreen is a mirror of compositor state, so the toggle reads the
 * window's ACTUAL flags rather than trusting it — the compositor can change
 * fullscreen state without asking us (WM shortcut, session change), and a
 * desynced mirror used to make F toggle the flag instead of the window.
 * ────────────────────────────────────────────────────────────────────── */
static void set_fullscreen(PlayerState *ps, SDL_Window *window, bool want_fs) {
    /* Pause audio across the transition to prevent drift */
    if (ps->playing && !ps->paused && ps->audio_stream)
        SDL_PauseAudioStreamDevice(ps->audio_stream);

    ps->fullscreen = want_fs;
    pacing_v2_window_hint(ps);

    /* NULL mode = "use the desktop's current mode". This is what keeps the
     * display link untouched, and it also clears any mode a previous build
     * may have left stuck on the window. */
    SDL_SetWindowFullscreenMode(window, NULL);
    SDL_SetWindowFullscreen(window, want_fs);

    if (want_fs) {
        log_msg("FS: entered borderless fullscreen (desktop mode, no modeset)");
    } else {
        log_msg("FS: returned to windowed");

        /* Resize to the current video's aspect ratio. Without this, opening a
         * different-aspect file while fullscreen leaves the old window shape
         * behind as stale black bars. */
        if (ps->playing && ps->vid_w > 0 && ps->vid_h > 0) {
            const SDL_DisplayMode *dm =
                SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
            int max_w = dm ? (int)(dm->w * 0.8) : 1920;
            int max_h = dm ? (int)(dm->h * 0.8) : 1080;
            int w = ps->vid_w, h = ps->vid_h;
            if (w > max_w || h > max_h) {
                double scale = fmin((double)max_w / w, (double)max_h / h);
                w = (int)(w * scale);
                h = (int)(h * scale);
            }
            ps->win_w = w;
            ps->win_h = h;
            SDL_SetWindowSize(window, w, h);
        }
    }

    if (ps->playing && !ps->paused && ps->audio_stream)
        SDL_ResumeAudioStreamDevice(ps->audio_stream);
}

static void toggle_fullscreen(PlayerState *ps, SDL_Window *window) {
    bool is_fs = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    set_fullscreen(ps, window, !is_fs);
}

/* Shim position hand-off: atomically rewrite DSVP_POS_FILE so the shim
 * daemon can turn positions into server progress reports. Write-tmp-
 * then-rename so the daemon can never see a partial line. ENDED marks
 * natural end-of-file (daemon reports the item finished) as opposed to
 * a user stop (position becomes the resume point). */
/* Every path that ends playback in a shim session must report the stop
 * and EXIT — the daemon blocks in waitpid on this process; parking in
 * the local browser instead hangs it and loses the stop report (review
 * 2026-08-20 finding 6: gamepad B, the O key, window close and startup
 * open-failure all did exactly that). Returns 1 in shim mode so UI
 * callers skip their browser path; call BEFORE player_close so the
 * position fields are still live. */
static void shim_write_pos(const char *path, PlayerState *ps, int ended);
static int shim_session_end(PlayerState *ps, int shim,
                            const char *pos_file, int ended) {
    if (!shim) return 0;
    if (pos_file) shim_write_pos(pos_file, ps, ended);
    ps->quit = 1;
    return 1;
}

static void shim_write_pos(const char *path, PlayerState *ps, int ended) {
    char tmp[1088];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    double dur = (ps->fmt_ctx && ps->fmt_ctx->duration > 0)
               ? ps->fmt_ctx->duration / (double)AV_TIME_BASE : 0.0;
    fprintf(f, "POS %.3f DUR %.3f PAUSED %d ENDED %d\n",
            ps->video_clock, dur, ps->paused ? 1 : 0, ended);
    fclose(f);
    rename(tmp, path);
}

int main(int argc, char *argv[]) {
    /* ── Initialize logging (before anything else) ── */
    log_init();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_terminate_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    log_msg("Starting DSVP v" DSVP_VERSION " build " DSVP_GIT_COMMIT " (argc=%d)", argc);
    log_msg("FFmpeg %s (libavcodec %d.%d)", av_version_info(),
            LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR);

    /* ── Shim session (DSVP_SHIM=1, set only by the shim daemon) ──
     * Single-stream appliance mode: HTTP whitelist opens (player.c),
     * every close path exits instead of returning to the browser (the
     * daemon owns what happens next), and position is handed off via
     * DSVP_POS_FILE for server progress reports. */
    const int s_shim = (getenv("DSVP_SHIM") != NULL);
    const char *s_pos_file = getenv("DSVP_POS_FILE");
    if (s_shim)
        log_msg("Shim session: HTTP whitelist active, exit-on-close%s",
                s_pos_file ? ", position hand-off on" : "");

    /* ── Get filepath from command line ── */
    char *open_path = NULL;
    if (argc > 1)
        open_path = strdup(argv[1]);

    /* ── Initialize SDL ── */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "[DSVP] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* ── Initialize shadercross (must be before GPU device creation) ── */
    if (!SDL_ShaderCross_Init()) {
        fprintf(stderr, "[DSVP] SDL_ShaderCross_Init failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    log_msg("SDL_ShaderCross initialized");

    /* Suppress FFmpeg's internal warnings (container quirks, timestamp
     * heuristics, etc.). In debug builds, keep them visible. */
#ifdef DSVP_DEBUG
    av_log_set_level(AV_LOG_VERBOSE);
#else
    av_log_set_level(AV_LOG_ERROR);
#endif
    /* Bypass X11 compositor (eliminates KWin jitter in desktop mode) */
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "1");

    /* ── Create window ── */
    SDL_Window *window = SDL_CreateWindow(
        DSVP_WINDOW_TITLE,
        DEFAULT_WIN_W, DEFAULT_WIN_H,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
    );
    if (!window) {
        fprintf(stderr, "[DSVP] Cannot create window: %s\n", SDL_GetError());
        SDL_ShaderCross_Quit();
        SDL_Quit();
        return 1;
    }
    /* Center window on screen (fixes offset at high DPI / 200% scaling) */
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    /* ── Set window icon ── */
    {
        SDL_IOStream *io = SDL_IOFromConstMem(dsvp_icon_bmp, dsvp_icon_bmp_size);
        if (io) {
            SDL_Surface *icon = SDL_LoadBMP_IO(io, true);  /* true = auto-close io */
            if (icon) {
                SDL_SetWindowIcon(window, icon);
                SDL_DestroySurface(icon);
            }
        }
    }

    /* ── Create GPU device ──
     * Force Vulkan on all platforms. SDL_GPU's D3D12 backend has a
     * transfer buffer synchronization bottleneck: SDL_MapGPUTransferBuffer
     * stalls on GPU fences from the previous frame's copy command,
     * adding 30-180ms per frame depending on texture size. On 4K 10-bit
     * content (19.2MB/frame), this made real-time playback impossible.
     * Vulkan's memory model handles transfer buffer cycling without
     * fence stalls, giving ~1-2ms per frame on the same content.*/
    /* Force Vulkan — D3D12 has transfer buffer fence stalls (30-180ms/frame). */
    SDL_SetHint(SDL_HINT_GPU_DRIVER, "vulkan");

#ifdef DSVP_DEBUG
    bool gpu_debug = true;
#else
    bool gpu_debug = false;
#endif

    /* ── Request DMA-BUF import extensions for VAAPI zero-copy ──
     *
     * SDL_CreateGPUDeviceWithProperties lets us pass SDL_GPUVulkanOptions
     * to request additional Vulkan device extensions at creation time.
     * These are required for importing VAAPI decoded surfaces as VkImages
     * via DMA-BUF, eliminating the 35-42ms GPU→CPU readback.
     *
     * If the extensions aren't available, SDL falls back to a device
     * without them — we detect this later and use the readback path. */
    const char *vk_ext_names[] = {
        "VK_KHR_external_memory",
        "VK_KHR_external_memory_fd",
        "VK_EXT_external_memory_dma_buf",
        "VK_EXT_image_drm_format_modifier",
    };
    SDL_GPUVulkanOptions vk_opts;
    SDL_zero(vk_opts);
    vk_opts.device_extension_count = 4;
    vk_opts.device_extension_names = vk_ext_names;

    SDL_PropertiesID gpu_props = SDL_CreateProperties();
    SDL_SetBooleanProperty(gpu_props,
        SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, gpu_debug);
    SDL_SetBooleanProperty(gpu_props,
        SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetStringProperty(gpu_props,
        SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan");
    SDL_SetPointerProperty(gpu_props,
        SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER, &vk_opts);

    SDL_GPUDevice *gpu_device = SDL_CreateGPUDeviceWithProperties(gpu_props);
    SDL_DestroyProperties(gpu_props);

    if (!gpu_device) {
        /* Extension request may have caused failure — retry without */
        log_msg("GPU: device creation with DMA-BUF extensions failed, retrying basic...");
        gpu_device = SDL_CreateGPUDevice(
            SDL_ShaderCross_GetSPIRVShaderFormats(), gpu_debug, NULL);
    }
    if (!gpu_device) {
        fprintf(stderr, "[DSVP] Cannot create GPU device: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_ShaderCross_Quit();
        SDL_Quit();
        return 1;
    }
    log_msg("GPU device created (driver: %s)",
            SDL_GetGPUDeviceDriver(gpu_device));

    /* ── Claim window for GPU rendering ── */
    if (!SDL_ClaimWindowForGPUDevice(gpu_device, window)) {
        fprintf(stderr, "[DSVP] Cannot claim window for GPU: %s\n", SDL_GetError());
        SDL_DestroyGPUDevice(gpu_device);
        SDL_DestroyWindow(window);
        SDL_ShaderCross_Quit();
        SDL_Quit();
        return 1;
    }

    /* A previous session that died holding display HDR left a stamp;
     * restore the recorded baseline BEFORE any probe of our own reads
     * the stranded state as "the user's configuration" (review
     * 2026-08-20 finding 18). */
    hdr_sys_reconcile_stamp();

    /* ── Set VSync via swapchain parameters ── */
    /* Claimed as SDR; the real choice needs the display probe and is made
     * once player state exists, still before any pipeline is built. */
    SDL_SetGPUSwapchainParameters(gpu_device, window,
        SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
        SDL_GPU_PRESENTMODE_VSYNC);
    log_msg("GPU: swapchain set to SDR + VSync");

    /* ── HDR capability probe (docs/TODO-HDR.md) ──
     * Which swapchain compositions will this window actually get?
     * This single log line is the architecture decision for HDR
     * passthrough: HDR10_ST2084 supported in a given session type
     * (desktop / desktop+KDE-HDR / Game Mode) means per-file HDR
     * output works there. Permanent diagnostic — costs four queries
     * at startup and answers "why no HDR?" forever after. */
    log_msg("GPU: swapchain support: SDR=%d SDR_LINEAR=%d "
            "HDR_EXTENDED_LINEAR=%d HDR10_ST2084=%d",
        SDL_WindowSupportsGPUSwapchainComposition(gpu_device, window,
            SDL_GPU_SWAPCHAINCOMPOSITION_SDR),
        SDL_WindowSupportsGPUSwapchainComposition(gpu_device, window,
            SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR),
        SDL_WindowSupportsGPUSwapchainComposition(gpu_device, window,
            SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR),
        SDL_WindowSupportsGPUSwapchainComposition(gpu_device, window,
            SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084));

    /* ── Initialize subtitle font (Phase 2 will use for GPU overlay) ── */
    if (sub_init_font() < 0) {
        log_msg("WARNING: Subtitle rendering disabled (no font)");
    }

    /* ── Initialize player state ── */
    PlayerState ps;
    memset(&ps, 0, sizeof(ps));
    ps.window     = window;
    ps.gpu_device = gpu_device;
    /* HDR output defaults to auto (docs/TODO-HDR.md): passthrough
     * engages per-file when content + display path support it; Z
     * toggles live. Revisit the default once the first passthrough
     * test settles whether desktop-mode KWin truly switches the
     * display or tone-maps our surface itself. */
    ps.hdr_out_mode = 1;
    /* Output gamut: which primaries the DISPLAY expects. BT.709 is the
     * safe default and what every previous build assumed unconditionally.
     * Set DSVP_OUT_GAMUT=2020 when the output is running wide gamut
     * (KDE's "Wide Color Gamut", DRM Colorspace BT2020_RGB) — otherwise
     * 709 code values are read against wider primaries and everything
     * comes out oversaturated. M toggles it live for the A/B. */
    /* Carry SDR in an HDR10/PQ container when the display needs it.
     *
     * AUTOMATIC: if the output we can drive is running wide gamut, plain
     * BT.709 output is wrong there — the display reads our 709 code
     * values against BT.2020 primaries and stretches every colour
     * outward. Converting primaries into the 8-bit SDR swapchain fixes
     * the hue but costs precision: Rec.2020 wants 10 bits and that
     * surface has 8. So on a wide-gamut display we hand over a proper
     * HDR10 signal instead — BT.2020 primaries, PQ-encoded at the SDR
     * reference white, 10-bit container. On an ordinary BT.709 display
     * none of this applies and the plain SDR path is already correct,
     * so nothing changes.
     *
     * 100 nits because that is what SDR is mastered against (the
     * BT.1886 / studio reference), so mapping SDR white to 100 nits in
     * the container reproduces the grading intent. BT.2408's 203 nits
     * is HDR *graphics* white and reads too hot for SDR content —
     * confirmed by eye on the C4, which is the instrument that settles
     * this. Affects SDR only: tone-mapped HDR keeps using the T-key
     * target as its reference white.
     *
     * The cost is that the display stays in HDR for the session, which
     * hdr_sys_preenable engages and hdr_output_shutdown restores.
     *   DSVP_OUT_PQ=0     force off (plain SDR output)
     *   DSVP_OUT_PQ=1     force on at 100 nits
     *   DSVP_OUT_PQ=<n>   force on at n nits
     *   DSVP_NO_SYS_HDR=1 opts out of display control entirely
     */
    {
        const char *pq = SDL_getenv("DSVP_OUT_PQ");
        if (pq && (strcmp(pq, "0") == 0 || strcmp(pq, "off") == 0)) {
            log_msg("Output: PQ container disabled by DSVP_OUT_PQ=%s", pq);
        } else if (pq) {
            double v = atof(pq);
            ps.out_pq_nits = (v >= 50.0 && v <= 1000.0) ? (float)v : 100.0f;
            log_msg("Output: PQ container forced on at %.0f nits "
                    "(DSVP_OUT_PQ=%s)", ps.out_pq_nits, pq);
        } else if (hdr_sys_display_is_wide_gamut(&ps)) {
            ps.out_pq_nits = 100.0f;
            log_msg("Output: %s is running wide gamut — SDR will ride an "
                    "HDR10/PQ container at 100 nits. DSVP_OUT_PQ=0 opts out.",
                    ps.hdr_sys_output);
        }

        if (ps.out_pq_nits > 0.0f
                && !SDL_WindowSupportsGPUSwapchainComposition(gpu_device,
                        window, SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084)) {
            log_msg("WARN: PQ container wanted but this window has no HDR10 "
                    "swapchain — staying SDR");
            ps.out_pq_nits = 0.0f;
        }
        if (ps.out_pq_nits > 0.0f) {
            /* The return matters: support was probed above, but support
             * is not success. Latching swapchain_hdr10 on a failed set
             * would make the recreate-skip trust a wrong mirror for the
             * whole session — PQ-encoding uniforms into an SDR surface
             * with no log line anywhere (review 2026-08-20 finding 8). */
            if (SDL_SetGPUSwapchainParameters(gpu_device, window,
                    SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084,
                    SDL_GPU_PRESENTMODE_VSYNC)) {
                ps.swapchain_hdr10 = 1;   /* recreate-skip tracks composition */
                log_msg("GPU: swapchain → HDR10/ST2084 (10-bit, SDR in an HDR "
                        "container)");
            } else {
                log_msg("WARN: HDR10 swapchain set FAILED (%s) — staying "
                        "SDR, PQ container off", SDL_GetError());
                ps.out_pq_nits = 0.0f;
            }
        }
    }

    {
        const char *og = SDL_getenv("DSVP_OUT_GAMUT");
        if (og && (strcmp(og, "2020") == 0 || strcmp(og, "bt2020") == 0))
            ps.out_gamut_pref = 1;
        else if (og && strcmp(og, "709") != 0 && strcmp(og, "bt709") != 0)
            log_msg("WARN: DSVP_OUT_GAMUT='%s' ignored (want 709 or 2020)", og);
    }

    /* ── Session display-HDR hold: engage NOW, not after shader
     * compilation. PQ mode holds display HDR for the whole session
     * (and DSVP_FS_HDR_FALLBACK=1 opts into the same hold for
     * displays that freeze in SDR fullscreen — visibly worse for SDR,
     * kept for the case where restoring output properties isn't
     * enough). This used to run AFTER the ~1.5s shader compile, so
     * every wide-gamut launch presented PQ on a still-SDR output for
     * ~2s — the inverse of the ordering hdr_output_apply documents as
     * load-bearing (review 2026-08-20 finding 14). Firing the async
     * kscreen switch here lets the display's mode change overlap the
     * compile instead of following it. */
    hdr_sys_preenable(&ps);
    /* Verify the hold once the async switch has had time to land
     * (~3s covers kscreen + the TV's own mode change). */
    double hold_verify_at =
        ps.hdr_sys_enabled_by_us ? get_time_sec() + 3.0 : 0.0;

    /* Opt-out for the render-at-content-rate intermediate (see dsvp.h) */
    ps.no_intermediate = (SDL_getenv("DSVP_NO_INTERMEDIATE") != NULL);
    /* Falsification switch for the decode-thread staging (review M1):
     * DSVP_NO_PRESTAGE=1 restores the old main-thread upload path. */
    ps.no_prestage = (SDL_getenv("DSVP_NO_PRESTAGE") != NULL);
    if (ps.no_prestage)
        log_msg("Decode-thread staging disabled (DSVP_NO_PRESTAGE)");
    /* Falsification switch for the get_buffer2 zero-copy pool
     * (TODO-PACING item 1): DSVP_NO_POOL=1 restores decode into
     * FFmpeg-owned buffers + the prestage path. */
    ps.no_pool = (SDL_getenv("DSVP_NO_POOL") != NULL);
    if (ps.no_pool)
        log_msg("Zero-copy decode pool disabled (DSVP_NO_POOL)");
    if (ps.no_intermediate)
        log_msg("Render: DSVP_NO_INTERMEDIATE set — direct render path");
    {
        const char *pv = SDL_getenv("DSVP_PACING");
        if (pv != NULL && strcmp(pv, "v1") == 0)
            log_msg("WARN: DSVP_PACING=v1 requested — the legacy "
                    "threshold stack was removed at 0.3.6");
        log_msg("Pacing: v2 mode machine (median sensor + slot "
                "scheduler)");
    }
    ps.volume     = 1.00;
    ps.video_stream_idx = -1;
    ps.audio_stream_idx = -1;
    ps.sub_active_idx   = -1;
    ps.win_w = DEFAULT_WIN_W;
    ps.win_h = DEFAULT_WIN_H;
    ps.hdr_target_idx = 0;  /* default: 203 nits (industry standard) */
    ps.gpu_uniforms.hdr_target_nits = 203.0f;
    ps.gpu_uniforms.hdr_midtone_gain = 1.3f;  /* default: moderate midtone lift */
    /* DSVP_PCM=1 forces PCM decode at startup. Escape hatch while the
     * SteamOS 192kHz HDMI audio regression (June 2026) blocks EAC3/TrueHD
     * passthrough on docked Deck: any 192k stream is silent and wedges the
     * LG C4 (mute latch until HDMI reseat). 48k passthrough (AC3) verified
     * unaffected. P-key mode cycling remains available as usual. */
    ps.audio_mode = getenv("DSVP_PCM")
        ? AUDIO_MODE_PCM   /* decode everything, no bitstream at open */
        : AUDIO_MODE_AUTO; /* probe HDMI sink, passthrough if supported */

    /* ── User audio-latency offset (madVR-style escape hatch) ──
     * DSVP_AUDIO_DELAY=<ms>, positive = the sink chain (TV decode,
     * AVR, soundbar) delays audio by that much relative to video.
     * Passthrough latency can't be measured in-app — the receiver
     * decodes downstream of every clock we can read — so the eye/ear
     * sets this once per setup. Applied to the bitstream sync clock. */
    {
        const char *ad = getenv("DSVP_AUDIO_DELAY");
        ps.audio_delay_sec = ad ? atof(ad) / 1000.0 : 0.0;
        if (ps.audio_delay_sec != 0.0)
            log_msg("Audio: user latency offset %+.0f ms (DSVP_AUDIO_DELAY)",
                    ps.audio_delay_sec * 1000.0);
    }

    /* ── Detect Game Mode vs Desktop Mode ──
     * Gamescope (SteamOS Game Mode compositor) sets GAMESCOPE_WAYLAND_DISPLAY.
     * When present: larger UI scale, gamepad-first UX, 16:10 fill.
     * When absent: Desktop Mode — normal scale, keyboard/mouse UX. */
    ps.game_mode = (getenv("GAMESCOPE_WAYLAND_DISPLAY") != NULL);
    ps.ui_scale  = ps.game_mode ? 3 : 1;
    log_msg("Mode: %s (ui_scale=%d)", ps.game_mode ? "Game Mode" : "Desktop", ps.ui_scale);

    /* ── Compile shaders and create GPU pipelines ── */
    if (gpu_create_pipelines(&ps) < 0) {
        fprintf(stderr, "[DSVP] GPU pipeline creation failed\n");
        SDL_DestroyGPUDevice(gpu_device);
        SDL_DestroyWindow(window);
        SDL_ShaderCross_Quit();
        SDL_Quit();
        return 1;
    }

    /* ── Open file from command line if provided ── */
    if (open_path) {
        if (player_open(&ps, open_path) != 0) {
            log_msg("ERROR: Failed to open: %s", log_path(open_path));
            /* Shim: a failed open (401, expired token, dead server)
             * must end the session, not park an appliance process on
             * the idle screen forever (finding 6d). NULL pos_file:
             * nothing ever played, and a POS 0.000 stop report would
             * clobber the server-side resume point — the daemon seeds
             * its stop report with the resume position when no
             * position file appears. */
            shim_session_end(&ps, s_shim, NULL, 0);
        } else {
            gain_reset(&ps);
            if (!s_shim) {
                playlist_scan(&ps);
            } else {
                /* No folder playlist for a URL; resume where the
                 * server left off instead (relative seek from 0). */
                const char *ss = getenv("DSVP_START_SEC");
                double start = ss ? atof(ss) : 0.0;
                if (start > 1.0) {
                    log_msg("Shim session: resuming at %.1fs", start);
                    player_seek(&ps, start);
                }
            }
        }
        free(open_path);
        open_path = NULL;
    }

    /* ── Auto-detect gamepad already connected at startup ── */
    {
        int count = 0;
        SDL_JoystickID *gamepads = SDL_GetGamepads(&count);
        if (gamepads && count > 0) {
            ps.gamepad = SDL_OpenGamepad(gamepads[0]);
            if (ps.gamepad) {
                ps.gamepad_active = 1;
                log_msg("Gamepad detected at startup: %s",
                        SDL_GetGamepadName(ps.gamepad));
            }
        }
        SDL_free(gamepads);
    }

    /* ── Initialize built-in file browser ──
     * Game Mode: browser is the default screen (gamepad-navigable).
     * Desktop Mode: classic idle screen with keyboard shortcuts shown.
     *   Browser is still available via O-key → navigate, or when
     *   returning from playback. */
    browser_init(&ps);
    if (!ps.game_mode)
        ps.browser_active = 0;  /* Desktop: show idle screen first */
    if (ps.playing && ps.filepath[0] && !s_shim) {
        /* Set browser to directory of the opened file. Shim excluded:
         * the "file" is an HTTP URL — strrchr-truncating it into
         * browser_path persisted a fake directory the next desktop
         * launch spent a 2s accessibility timeout rejecting
         * (finding 11). */
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", ps.filepath);
        char *sep = strrchr(dir, '/');
        if (sep) {
            *(sep + 1) = '\0';
            snprintf(ps.browser_path, sizeof(ps.browser_path), "%s", dir);
            browser_scan(&ps);
            browser_save_path(&ps);
        }
    }

    /* ── Main loop ── */
    double pr_t0 = 0.0;   /* PRESENT DIAG window start (VRR investigation) */
    int    pr_n  = 0;     /* iterations in current window */
    long   pr_presents_last = 0;  /* ps.presents at window start */
    const int s_diag = (getenv("DSVP_DIAG") != NULL);
    while (!ps.quit) {
        if (g_signal_quit) { log_msg("Signal received — shutting down cleanly"); ps.quit = 1; break; }
        /* One-shot launch-hold verification (see hdr_sys_verify_hold). */
        if (hold_verify_at > 0.0 && get_time_sec() >= hold_verify_at) {
            hold_verify_at = 0.0;
            hdr_sys_verify_hold(&ps);
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {

            case SDL_EVENT_QUIT:
                /* Flush the shim position BEFORE close zeroes
                 * ps.playing — the exit backstop is gated on playing
                 * and could never fire for window close despite its
                 * comment claiming to cover it (finding 6c). */
                if (ps.playing) {
                    shim_session_end(&ps, s_shim, s_pos_file, 0);
                    player_close(&ps);
                }
                ps.quit = 1;
                break;

            case SDL_EVENT_KEY_DOWN:
                /* Raw key report under DSVP_DIAG=1. Bindings that "do nothing"
                 * are almost always the event not being what the code assumed
                 * — scancode vs keycode, or modifiers not arriving — so print
                 * what SDL actually delivered rather than guessing at it. */
                if (s_diag)
                    log_msg("KEY: scancode=%d key=%d mod=0x%04x repeat=%d",
                            (int)ev.key.scancode, (int)ev.key.key,
                            (unsigned)ev.key.mod, (int)ev.key.repeat);

                /* ── Browser navigation (when shown and no file playing) ── */
                if (ps.browser_active && !ps.playing) {
                    int browser_consumed = 1;
                    switch (ev.key.key) {
                    case SDLK_UP:
                        browser_navigate(&ps, -1);
                        break;
                    case SDLK_DOWN:
                        browser_navigate(&ps, 1);
                        break;
                    case SDLK_LEFT:
                        browser_page(&ps, -1);
                        break;
                    case SDLK_RIGHT:
                        browser_page(&ps, 1);
                        break;
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER:
                        if (browser_enter(&ps)) {
                            ps.show_controls = 0;
                            log_msg("Browser: opening %s",
                                    log_path(ps.browser_selected_file));
                            if (player_open(&ps, ps.browser_selected_file) != 0) {
                                log_msg("ERROR: Failed to open file");
                            } else {
                                gain_reset(&ps);
                                playlist_scan(&ps);
                            }
                        }
                        break;
                    case SDLK_BACKSPACE:
                        if (!browser_at_root(&ps))
                            browser_back(&ps);
                        break;
                    default:
                        browser_consumed = 0;
                        break;
                    }
                    if (browser_consumed) break;
                }

                /* Auto-repeat is wanted in the browser above (hold to scroll)
                 * but nowhere below: a held or bouncing A/P/S/N/B key fires a
                 * track switch, passthrough toggle or file change per repeat
                 * event, which is how one keypress produced six identical
                 * "skipping TrueHD track" refusals in a row. */
                if (ev.key.repeat) break;

                switch (ev.key.key) {

                case SDLK_ESCAPE:
                    /* The controls overlay tells the user Esc closes it; until
                     * now nothing bound Esc, so it lied on a keyboard. */
                    if (ps.show_controls) ps.show_controls = 0;
                    break;

                case SDLK_Q:
                    if (ps.playing && shim_session_end(&ps, s_shim,
                                                       s_pos_file, 0)) {
                        /* Shim session: close means done; position
                         * flushed before the state vanished. */
                        player_close(&ps);
                    } else if (ps.playing) {
                        /* Update browser to current file's directory */
                        if (ps.filepath[0]) {
                            char dir[1024];
                            snprintf(dir, sizeof(dir), "%s", ps.filepath);
                            char *sep = strrchr(dir, '/');
                            if (sep) {
                                *(sep + 1) = '\0';
                                snprintf(ps.browser_path, sizeof(ps.browser_path), "%s", dir);
                                browser_scan(&ps);
                                browser_save_path(&ps);
                            }
                        }
                        player_close(&ps);
                        ps.browser_active = 1; /* show browser to pick next file */
                        ps.quit = 0; /* return to browser, not exit */
                    } else {
                        ps.quit = 1;
                    }
                    break;

                case SDLK_O: {
                    /* Open integrated file browser (replaces external dialog).
                     * If playing, close first so we return to browser.
                     * Shim: there is no local browser to return to —
                     * ending playback ends the session (finding 6b). */
                    log_msg("File browser requested (O key)");
                    if (ps.playing) {
                        if (shim_session_end(&ps, s_shim, s_pos_file, 0)) {
                            player_close(&ps);
                            break;
                        }
                        player_close(&ps);
                    }
                    gain_reset(&ps);
                    ps.show_controls = 0;
                    if (!ps.browser_active) {
                        browser_init(&ps);
                        ps.browser_active = 1;
                    }
                    break;
                }

                case SDLK_SPACE:
                    if (ps.playing) {
                        ps.paused = !ps.paused;
                        if (ps.audio_stream) {
                            if (ps.paused)
                                SDL_PauseAudioStreamDevice(ps.audio_stream);
                            else
                                SDL_ResumeAudioStreamDevice(ps.audio_stream);
                        }
                        if (!ps.paused) {
                            ps.frame_timer = get_time_sec();
                            /* Restart FPS window — the paused gap would
                             * otherwise skew the first reading on resume */
                            ps.fps_window_start   = 0.0;
                            ps.fps_window_frames  = 0;
                            ps.rfps_window_frames = 0;
                        }
                    }
                    break;

                case SDLK_F:
                    toggle_fullscreen(&ps, window);
                    break;

                case SDLK_D:
                    if (ps.playing) {
                        ps.show_debug = !ps.show_debug;
                        if (ps.show_debug) {
                            ps.show_info = 0;  /* mutually exclusive */
                            player_build_debug_info(&ps);
                        }
                    }
                    break;

                case SDLK_I:
                    if (ps.playing) {
                        ps.show_info = !ps.show_info;
                        if (ps.show_info) {
                            ps.show_debug = 0;  /* mutually exclusive */
                            player_build_media_info(&ps);
                        }
                    }
                    break;

                case SDLK_H:
                    if (ps.playing && ps.gpu_uniforms.is_hdr > 0.0f) {
                        if (ps.hdr_out_active) {
                            snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                     "HDR debug: no effect in passthrough "
                                     "(Z = tone-map)");
                            ps.aud_osd_until = get_time_sec() + 2.0;
                            break;
                        }
                        int mode = (int)ps.gpu_uniforms.hdr_debug;
                        mode = (mode + 1) % 4;
                        ps.gpu_uniforms.hdr_debug = (float)mode;
                        ps.frame_render_dirty = 1;
                        float tn = ps.gpu_uniforms.hdr_target_nits;
                        const char *fmt[] = {
                            "HDR: BT.2390 (%.0f nit target)",
                            "HDR: BT.2390 (%.0f+100 nit target)",
                            "HDR: PQ bypass (raw stream)",
                            "HDR: Luminance visualization"
                        };
                        snprintf(ps.aud_osd, sizeof(ps.aud_osd), fmt[mode],
                                 mode <= 1 ? tn : 0.0f);
                        ps.aud_osd_until = get_time_sec() + 2.0;
                    }
                    break;

                case SDLK_T:
                    if (ps.playing && ps.gpu_uniforms.is_hdr > 0.0f) {
                        if (ps.hdr_out_active) {
                            snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                     "SDR target: no effect in passthrough "
                                     "(Z = tone-map)");
                            ps.aud_osd_until = get_time_sec() + 2.0;
                            break;
                        }
                        static const float targets[] = { 203.0f, 300.0f, 400.0f };
                        ps.hdr_target_idx = (ps.hdr_target_idx + 1) % 3;
                        ps.gpu_uniforms.hdr_target_nits = targets[ps.hdr_target_idx];
                        ps.frame_render_dirty = 1;
                        snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                 "SDR target: %.0f nits", targets[ps.hdr_target_idx]);
                        ps.aud_osd_until = get_time_sec() + 2.0;
                        log_msg("HDR: SDR target changed to %.0f nits",
                                targets[ps.hdr_target_idx]);
                    }
                    break;

                case SDLK_G:
                    if (ps.playing && ps.gpu_uniforms.is_hdr > 0.0f) {
                        if (ps.hdr_out_active) {
                            snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                     "Midtone gain: no effect in passthrough "
                                     "(Z = tone-map)");
                            ps.aud_osd_until = get_time_sec() + 2.0;
                            break;
                        }
                        s_gain_idx = (s_gain_idx + 1) % 6;
                        ps.gpu_uniforms.hdr_midtone_gain = s_gain_table[s_gain_idx];
                        ps.frame_render_dirty = 1;
                        snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                 "Midtone gain: %.2f", s_gain_table[s_gain_idx]);
                        ps.aud_osd_until = get_time_sec() + 2.0;
                        log_msg("HDR: midtone gain changed to %.2f",
                                s_gain_table[s_gain_idx]);
                    }
                    break;

                case SDLK_Z:
                    /* Toggle HDR passthrough ↔ SDR tone-map, live.
                     * The A/B: display's tone mapping vs ours. */
                    if (!ps.playing || ps.gpu_uniforms.is_hdr <= 0.0f)
                        break;
                    if (!ps.hdr_pass_content) {
                        snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                 "HDR passthrough: n/a for this format yet");
                        ps.aud_osd_until = get_time_sec() + 2.0;
                        break;
                    }
                    if (!SDL_WindowSupportsGPUSwapchainComposition(
                            ps.gpu_device, ps.window,
                            SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084)) {
                        snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                 "HDR passthrough: display path has no HDR");
                        ps.aud_osd_until = get_time_sec() + 2.0;
                        break;
                    }
                    ps.hdr_out_mode = ps.hdr_out_mode ? 0 : 1;
                    hdr_output_apply(&ps);
                    snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                             "HDR output: %s",
                             ps.hdr_out_active
                                 ? "passthrough (display tone-maps)"
                                 : "tone-map (SDR)");
                    ps.aud_osd_until = get_time_sec() + 2.0;
                    break;

                case SDLK_E:
                    /* Cycle output transfer for tone-mapped content:
                     * sRGB piecewise → 2.2 → 2.4. Same effect as
                     * DSVP_OUTPUT_GAMMA, but live — the docked-vs-
                     * internal-panel A/B is an eye test, not a
                     * relaunch. Only meaningful on the tone-mapped
                     * paths, so gated on HDR like T and G. */
                    if (ps.playing && ps.gpu_uniforms.is_hdr > 0.0f
                            && (ps.gpu_uniforms.hdr_pass > 0.5f
                                || ps.out_pq_nits > 0.0f)) {
                        /* out_gamma is never read in passthrough (the
                         * shader returns before encode) nor in PQ mode
                         * (encode_output takes the PQ branch first) —
                         * cycling it silently claimed changes that
                         * could not appear (review 2026-08-20
                         * finding 17). Say so instead. */
                        snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                 "Output transfer: no effect (%s)",
                                 ps.gpu_uniforms.hdr_pass > 0.5f
                                     ? "passthrough" : "PQ container");
                        ps.aud_osd_until = get_time_sec() + 2.0;
                    } else if (ps.playing && ps.gpu_uniforms.is_hdr > 0.0f) {
                        float next;
                        const char *name;
                        if (ps.out_gamma_pref == 1.0f)      { next = 2.2f; name = "gamma 2.2"; }
                        else if (ps.out_gamma_pref == 2.2f) { next = 2.4f; name = "gamma 2.4 (BT.1886)"; }
                        else                                { next = 1.0f; name = "sRGB piecewise"; }
                        ps.out_gamma_pref = next;
                        ps.gpu_uniforms.out_gamma =
                            (next == 1.0f) ? 0.0f : next;
                        ps.frame_render_dirty = 1;
                        snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                 "Output transfer: %s", name);
                        ps.aud_osd_until = get_time_sec() + 2.0;
                        log_msg("Output transfer changed to %s", name);
                    }
                    break;

                case SDLK_M:
                    /* Toggle the OUTPUT gamut: encode for a BT.709
                     * display or for a BT.2020 one. Unlike E/T/G this
                     * is NOT gated on HDR — ordinary BT.709 SDR on a
                     * wide-gamut display is exactly the case it exists
                     * for, and that is SDR content by definition.
                     * Live toggle because a primaries change can only
                     * be judged by eye, on the same frame, A/B. */
                    if (ps.playing) {
                        ps.out_gamut_pref = !ps.out_gamut_pref;
                        /* Same derivation as gpu_setup_uniforms: the
                         * HDR10 container is BT.2020 by definition, so
                         * PQ output clamps the uniform on regardless of
                         * the preference. Recomputing from the bare
                         * preference here dropped that clamp — two M
                         * presses desaturated tone-mapped HDR until the
                         * next file open (review 2026-08-20 finding 5). */
                        int m_gamut_2020 = ps.out_gamut_pref
                                        || ps.out_pq_nits > 0.0f;
                        ps.gpu_uniforms.out_gamut =
                            m_gamut_2020 ? 1.0f : 0.0f;
                        ps.frame_render_dirty = 1;
                        const char *name =
                            (m_gamut_2020 && !ps.out_gamut_pref)
                            ? "BT.2020 (forced by PQ container)"
                            : ps.out_gamut_pref
                              ? "BT.2020 (wide-gamut display)"
                              : "BT.709 (default)";
                        snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                 "Output gamut: %s", name);
                        ps.aud_osd_until = get_time_sec() + 2.0;
                        log_msg("Output gamut changed to %s", name);
                    }
                    break;

                case SDLK_S:
                    sub_cycle(&ps);
                    break;

                case SDLK_A:
                    audio_cycle(&ps);
                    break;

                case SDLK_P:
                {
                    /* Block P during async mode switch — prevents mode counter
                     * from advancing while the background thread is working */
                    if (ps.audio_switch_phase != 0) break;

                    /* Cycle audio mode: PCM → AUTO → PASSTHROUGH → PCM */
                    int m = (int)ps.audio_mode + 1;
                    if (m > 2) m = 0;
                    ps.audio_mode = (AudioMode)m;

                    /* Re-probe HDMI sink if entering a passthrough mode */
                    if (ps.audio_mode != AUDIO_MODE_PCM && !ps.bitstream_caps.probed)
                        bitstream_probe(&ps);

                    static const char *mode_names[] = {
                        "PCM (decode)", "AUTO", "PASSTHROUGH"
                    };

                    /* ── Live audio mode switch during playback ──
                     * Bitstream→PCM uses async pattern: fast stop, then
                     * background thread handles pactl + delays so video
                     * keeps rendering. Completion in main loop below.
                     * PCM→Bitstream stays synchronous (simpler, audio is
                     * already silent so the brief freeze is less jarring). */
                    if (ps.playing && ps.audio_codec_ctx
                            && ps.audio_switch_phase == 0) {
                        if (ps.audio_mode == AUDIO_MODE_PCM && ps.bitstream_active) {
                            /* ── Async bitstream→PCM ── */
                            ps.audio_switch_was_truehd =
                                (ps.audio_codec_ctx->codec_id == AV_CODEC_ID_TRUEHD);
                            bitstream_stop_immediate(&ps);

                            /* Launch background thread (now a thin signaler) */
                            ps.audio_switch_to_mode = AUDIO_MODE_PCM;
                            ps.audio_switch_phase = 1;
                            ps.audio_switch_thread = SDL_CreateThread(
                                audio_switch_bg_func, "audioswitch", &ps);
                            if (!ps.audio_switch_thread) {
                                /* Thread creation failed: complete the
                                 * switch synchronously next tick, or
                                 * phase sticks at 1 forever — audio
                                 * already torn down, P key dead. */
                                log_msg("WARN: audio switch thread failed "
                                        "(%s) — completing synchronously",
                                        SDL_GetError());
                                ps.audio_switch_phase = 2;
                            }

                            snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                     "Audio Mode: switching...");
                            ps.aud_osd_until = get_time_sec() + 3.0;

                        } else if (ps.audio_mode != AUDIO_MODE_PCM && !ps.bitstream_active) {
                            audio_close(&ps);
                            if (!bitstream_start(&ps) &&
                                audio_open(&ps) < 0) {
                                /* Checked like audio.c's track-switch twin:
                                 * an unchecked failure leaves a codec with
                                 * no device — audio_pq fills and the demux
                                 * throttle freezes all playback (the
                                 * finding-2 class, third site). */
                                log_msg("Audio: PCM fallback open failed on "
                                        "mode switch — audio off");
                                avcodec_free_context(&ps.audio_codec_ctx);
                                audio_disable_public(&ps,
                                        "Audio: device error, audio off");
                                /* Out of the case before the mode OSD
                                 * below replaces the disable notice
                                 * with an "Audio Mode" claim; no
                                 * resync seek for a dead sink (the
                                 * audio.c twin returns here too). */
                                break;
                            }
                            player_seek(&ps, 0.0);
                        }
                    } else if (ps.audio_mode == AUDIO_MODE_PCM) {
                        ps.bitstream_active = 0;
                    }

                    if (ps.audio_switch_phase == 0) {
                        snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                 "Audio Mode: %s", mode_names[ps.audio_mode]);
                        ps.aud_osd_until = get_time_sec() + 2.0;
                    }
                    log_msg("Audio mode: %s (bitstream_active=%d)",
                            mode_names[ps.audio_mode], ps.bitstream_active);
                    break;
                }

                case SDLK_LEFT:
                    player_seek(&ps, -SEEK_STEP_SEC);
                    break;

                case SDLK_RIGHT:
                    player_seek(&ps, SEEK_STEP_SEC);
                    break;

                case SDLK_UP:
                    ps.volume += VOLUME_STEP;
                    if (ps.volume > 1.0) ps.volume = 1.0;
                    if (ps.audio_stream)
                        SDL_SetAudioStreamGain(ps.audio_stream, ps.volume);
                    ps.show_seekbar = 1;
                    ps.seekbar_hide_time = get_time_sec() + 1.5;
                    break;

                case SDLK_DOWN:
                    ps.volume -= VOLUME_STEP;
                    if (ps.volume < 0.0) ps.volume = 0.0;
                    if (ps.audio_stream)
                        SDL_SetAudioStreamGain(ps.audio_stream, ps.volume);
                    ps.show_seekbar = 1;
                    ps.seekbar_hide_time = get_time_sec() + 1.5;
                    break;

                case SDLK_N:  /* Next file in folder */
                case SDLK_B:  /* Previous (Back) file in folder */
                {
                    int delta = (ev.key.key == SDLK_N) ? 1 : -1;
                    if (ps.playlist_count > 0 && ps.playlist_index >= 0) {
                        int next = ps.playlist_index + delta;
                        if (next < 0 || next >= ps.playlist_count) {
                            /* At boundary — show OSD */
                            snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                     "No %s file in folder",
                                     delta > 0 ? "next" : "previous");
                            ps.aud_osd_until = get_time_sec() + 2.0;
                        } else {
                            /* The playlist is owned by this loop;
                             * player_close/player_open never touch its
                             * fields, so it survives the cycle as-is. */
                            player_close(&ps);

                            log_msg("Playlist nav: opening [%d/%d] %s",
                                    next + 1, ps.playlist_count,
                                    log_path(ps.playlist_files[next]));

                            if (player_open(&ps, ps.playlist_files[next]) == 0)
                                gain_reset(&ps);
                            else
                                log_msg("ERROR: Failed to open: %s",
                                        log_path(ps.playlist_files[next]));
                            ps.playlist_index = next;
                        }
                    }
                    break;
                }

                default:
                    break;
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.button.button == SDL_BUTTON_LEFT && ev.button.clicks == 2) {
                    /* Double-click -> same path as F, no divergence */
                    toggle_fullscreen(&ps, window);
                }

                /* Click on seek bar — buttons and progress track.
                 * Geometry must match overlay.c draw_seekbar() layout:
                 *   [btn_margin][◀][gap][▶][gap][time][12][==track==][12][sep][vol][margin]
                 * s = UI scale factor (1 windowed, 2 fullscreen) must
                 * match s_ui_scale in overlay.c. */
                if (ev.button.button == SDL_BUTTON_LEFT && ps.playing
                      && ev.button.clicks == 1   /* P2-13: the second click of a
                                                    double-click toggles fullscreen
                                                    above — it must not also seek
                                                    against post-toggle geometry */
                      && ps.show_seekbar) {
                    int h_now;
                    SDL_GetWindowSizeInPixels(window, NULL, &h_now);
                    float density = SDL_GetWindowPixelDensity(window);
                    int mx = (int)(ev.button.x * density);
                    int my = (int)(ev.button.y * density);
                    int s = ui_scale_for(&ps, h_now);
                    int bar_h = 30 * s;
                    int bar_y = h_now - bar_h;

                    if (my >= bar_y && my <= h_now) {
                        /* Button geometry (must match overlay.c) */
                        int btn_x = 8 * s;
                        int btn_sz = 8 * s;
                        int btn_gap = 10 * s;
                        int btn2_x = btn_x + btn_sz + btn_gap;


                        /* Prev button click area */
                        if (mx >= btn_x &&
                                mx <= btn_x + btn_sz) {
                            SDL_Event fake = {0};
                            fake.type = SDL_EVENT_KEY_DOWN;
                            fake.key.key = SDLK_B;
                            SDL_PushEvent(&fake);
                        }
                        /* Next button click area */
                        else if (mx >= btn2_x &&
                                mx <= btn2_x + btn_sz) {
                            SDL_Event fake = {0};
                            fake.type = SDL_EVENT_KEY_DOWN;
                            fake.key.key = SDLK_N;
                            SDL_PushEvent(&fake);
                        }
                        /* Seek track */
                        else {
                            int track_x = ps.seekbar_track_x;
                            int track_w = ps.seekbar_track_w;

                            if (track_w > 20 && mx >= track_x
                                    && mx <= track_x + track_w) {
                                double frac = (double)(mx - track_x) / track_w;
                                if (frac < 0.0) frac = 0.0;
                                if (frac > 1.0) frac = 1.0;
                                double duration = (ps.fmt_ctx->duration != AV_NOPTS_VALUE)
                                    ? (double)ps.fmt_ctx->duration / AV_TIME_BASE : 0.0;
                                double target = frac * duration;
                                player_seek(&ps, target - ps.video_clock);
                            }
                        }
                    }
                }
                break;

            case SDL_EVENT_MOUSE_MOTION:
                /* Show overlays on mouse movement, auto-hide after 3s */
                SDL_ShowCursor();
                if (ps.playing) {
                    ps.show_seekbar = 1;
                    ps.seekbar_hide_time = get_time_sec() + 1.5;
                }
                break;

            case SDL_EVENT_DISPLAY_ADDED:
            case SDL_EVENT_DISPLAY_REMOVED:
            case SDL_EVENT_DISPLAY_ORIENTATION:
            case SDL_EVENT_DISPLAY_MOVED:
                /* Hotplug can leave SDL's view of the display stale — after an
                 * unplug/replug, fullscreen silently stopped working with no
                 * event handled and nothing logged. Log it so the next
                 * occurrence is visible rather than mysterious. */
                log_msg("DISPLAY EVENT: type=%u displayID=%u (mode list may be stale)",
                        (unsigned)ev.type, (unsigned)ev.display.displayID);
                break;

            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
                log_msg("DISPLAY: window moved to displayID=%u", (unsigned)ev.window.data1);
                break;

            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
                if (!ps.fullscreen)
                    log_msg("FS-sync: compositor entered fullscreen (we thought windowed)");
                ps.fullscreen = 1;
                break;

            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                if (ps.fullscreen)
                    log_msg("FS-sync: compositor left fullscreen (we thought fullscreen)");
                ps.fullscreen = 0;
                break;

            case SDL_EVENT_WINDOW_RESIZED:
                    ps.win_w = ev.window.data1;
                    ps.win_h = ev.window.data2;
                    log_msg("SDL_EVENT_WINDOW_RESIZED: %dx%d",
                            ev.window.data1, ev.window.data2);
                    pacing_v2_window_hint(&ps);
                break;

            /* ── Gamepad hotplug ── */
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!ps.gamepad) {
                    ps.gamepad = SDL_OpenGamepad(ev.gdevice.which);
                    if (ps.gamepad) {
                        ps.gamepad_active = 1;
                        log_msg("Gamepad connected: %s",
                                SDL_GetGamepadName(ps.gamepad));
                    }
                }
                break;

            case SDL_EVENT_GAMEPAD_REMOVED:
                if (ps.gamepad &&
                        SDL_GetGamepadID(ps.gamepad) == ev.gdevice.which) {
                    SDL_CloseGamepad(ps.gamepad);
                    ps.gamepad = NULL;
                    ps.gamepad_active = 0;
                    ps.trigger_seek_speed = 0.0f;
                    log_msg("Gamepad disconnected");
                }
                break;

            /* ── Gamepad buttons ── */
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                switch (ev.gbutton.button) {

                case SDL_GAMEPAD_BUTTON_SOUTH:  /* A — Select / Play */
                    if (!ps.playing && ps.browser_active) {
                        /* Browser: select entry */
                        if (browser_enter(&ps)) {
                            ps.show_controls = 0;
                            log_msg("Browser: opening %s",
                                    log_path(ps.browser_selected_file));
                            if (player_open(&ps, ps.browser_selected_file) != 0) {
                                log_msg("ERROR: Failed to open file");
                            } else {
                                gain_reset(&ps);
                                playlist_scan(&ps);
                            }
                        }
                    } else if (!ps.playing) {
                        /* Activate integrated file browser */
                        if (!ps.browser_active) {
                            browser_init(&ps);
                            ps.browser_active = 1;
                        }
                    } else if (ps.transport_active) {
                        /* Transport: activate focused element */
                        if (ps.transport_focus == 0) {
                            SDL_Event fake = {0};
                            fake.type = SDL_EVENT_KEY_DOWN;
                            fake.key.key = SDLK_B;
                            SDL_PushEvent(&fake);
                        } else if (ps.transport_focus == 2) {
                            SDL_Event fake = {0};
                            fake.type = SDL_EVENT_KEY_DOWN;
                            fake.key.key = SDLK_N;
                            SDL_PushEvent(&fake);
                        }
                        /* focus==1 (scrubber): no-op */
                    } else {
                        /* Playing: show seek bar */
                        ps.show_seekbar = 1;
                        ps.seekbar_hide_time = get_time_sec() + 3.0;
                    }
                    break;

                case SDL_GAMEPAD_BUTTON_WEST:   /* X — Pause */
                    if (ps.playing) {
                        ps.paused = !ps.paused;
                        if (ps.audio_stream) {
                            if (ps.paused)
                                SDL_PauseAudioStreamDevice(ps.audio_stream);
                            else
                                SDL_ResumeAudioStreamDevice(ps.audio_stream);
                        }
                        if (!ps.paused) {
                            ps.frame_timer = get_time_sec();
                            /* Restart FPS window after the paused gap */
                            ps.fps_window_start   = 0.0;
                            ps.fps_window_frames  = 0;
                            ps.rfps_window_frames = 0;
                        }
                    }
                    break;

                case SDL_GAMEPAD_BUTTON_EAST:   /* B — Back / Stop */
                    if (ps.playing && ps.transport_active) {
                        ps.transport_active = 0;
                        ps.seekbar_hide_time = get_time_sec() + 3.0;
                    } else if (ps.playing) {
                        /* Shim: B is the Deck's primary stop control —
                         * end the session instead of becoming a local
                         * file browser the daemon waits on forever
                         * (finding 6a). This also stops the shim URL
                         * being strrchr-truncated into browser_path
                         * and persisted (finding 11's second copy). */
                        if (shim_session_end(&ps, s_shim, s_pos_file, 0)) {
                            player_close(&ps);
                            ps.transport_active = 0;
                            break;
                        }
                        /* Update browser to current file's directory */
                        if (ps.filepath[0]) {
                            char dir[1024];
                            snprintf(dir, sizeof(dir), "%s", ps.filepath);
                            char *sep = strrchr(dir, '/');
                            if (sep) {
                                *(sep + 1) = '\0';
                                snprintf(ps.browser_path, sizeof(ps.browser_path), "%s", dir);
                                browser_scan(&ps);
                                browser_save_path(&ps);
                            }
                        }
                        player_close(&ps);
                        ps.transport_active = 0;
                        ps.browser_active = 1;
                        ps.quit = 0;
                    } else if (ps.browser_active && !browser_at_root(&ps)) {
                        browser_back(&ps);
                    } else {
                        ps.quit = 1;
                    }
                    break;

                case SDL_GAMEPAD_BUTTON_NORTH:  /* Y — Cycle subtitles */
                    sub_cycle(&ps);
                    break;

                case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  /* LB — Seek -5s */
                    player_seek(&ps, -SEEK_STEP_SEC);
                    break;

                case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: /* RB — Seek +5s */
                    player_seek(&ps, SEEK_STEP_SEC);
                    break;

                case SDL_GAMEPAD_BUTTON_RIGHT_STICK:    /* R3 — Cycle audio */
                    audio_cycle(&ps);
                    break;

                case SDL_GAMEPAD_BUTTON_LEFT_STICK:  /* L3 — Transport mode */
                    if (ps.playing) {
                        ps.transport_active = !ps.transport_active;
                        if (ps.transport_active) {
                            ps.transport_focus = 1;  /* start on scrubber */
                            ps.show_seekbar = 1;
                            ps.seekbar_hide_time = 1e18; /* don't auto-hide */
                        } else {
                            ps.seekbar_hide_time = get_time_sec() + 3.0;
                        }
                    }
                    break;

                case SDL_GAMEPAD_BUTTON_DPAD_UP:    /* D-pad: nav/volume */
                    if (!ps.playing && ps.browser_active) {
                        browser_navigate(&ps, -1);
                        ps.dpad_held_dir = -1;
                        ps.dpad_held_since = get_time_sec();
                        ps.dpad_last_repeat = ps.dpad_held_since;
                    } else if (ps.playing) {
                        ps.volume += VOLUME_STEP;
                        if (ps.volume > 1.0) ps.volume = 1.0;
                        if (ps.audio_stream)
                            SDL_SetAudioStreamGain(ps.audio_stream, ps.volume);
                        ps.show_seekbar = 1;
                        ps.seekbar_hide_time = get_time_sec() + 1.5;
                    }
                    break;

                case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                    if (!ps.playing && ps.browser_active) {
                        browser_navigate(&ps, 1);
                        ps.dpad_held_dir = 1;
                        ps.dpad_held_since = get_time_sec();
                        ps.dpad_last_repeat = ps.dpad_held_since;
                    } else if (ps.playing) {
                        ps.volume -= VOLUME_STEP;
                        if (ps.volume < 0.0) ps.volume = 0.0;
                        if (ps.audio_stream)
                            SDL_SetAudioStreamGain(ps.audio_stream, ps.volume);
                        ps.show_seekbar = 1;
                        ps.seekbar_hide_time = get_time_sec() + 1.5;
                    }
                    break;

                case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  /* Prev file / Page up */
                {
                    if (!ps.playing && ps.browser_active) {
                        browser_page(&ps, -1);
                    } else {
                        SDL_Event fake = {0};
                        fake.type = SDL_EVENT_KEY_DOWN;
                        fake.key.key = SDLK_B;
                        SDL_PushEvent(&fake);
                    }
                    break;
                }

                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: /* Next file / Page down */
                {
                    if (!ps.playing && ps.browser_active) {
                        browser_page(&ps, 1);
                    } else {
                        SDL_Event fake = {0};
                        fake.type = SDL_EVENT_KEY_DOWN;
                        fake.key.key = SDLK_N;
                        SDL_PushEvent(&fake);
                    }
                    break;
                }

                case SDL_GAMEPAD_BUTTON_START:  /* Menu — toggle controls overlay */
                    ps.show_controls = !ps.show_controls;
                    break;

                case SDL_GAMEPAD_BUTTON_BACK:   /* Select — Debug overlay */
                    if (ps.playing) {
                        ps.show_debug = !ps.show_debug;
                        if (ps.show_debug) {
                            ps.show_info = 0;
                            player_build_debug_info(&ps);
                        }
                    }
                    break;

                default:
                    break;
                }
                break;

            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP ||
                    ev.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                    ps.dpad_held_dir = 0;
                }
                break;

            /* ── Gamepad analog triggers — continuous seek ──
             *
             * LT/RT axis ranges 0 (released) to 32767 (full pull).
             * Quadratic power curve: gentle at light pull, fast at deep pull.
             *   25% pull → 4×, 50% → 16×, 75% → 36×, 100% → 64×.
             * Dead zone at 15% to avoid drift from resting triggers.
             * Applied each frame in the render section below. */
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            {
                float dead_zone = 4915.0f;  /* ~15% of 32767 */
                float max_range = 32767.0f - dead_zone;
                if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX && ps.transport_active) {
                    static int stick_x_zone = 0;
                    float threshold = 19660.0f;  /* ~60% of 32767 */
                    int new_zone = 0;
                    if (ev.gaxis.value < -threshold) new_zone = -1;
                    else if (ev.gaxis.value > threshold) new_zone = 1;

                    if (new_zone != stick_x_zone) {
                        if (ps.transport_focus == 1) {
                            /* Scrubber: edge-trigger first seek, start hold timer */
                            if (new_zone == -1) {
                                player_seek(&ps, -SEEK_LARGE_SEC);
                                ps.transport_seek_dir = -1;
                                ps.transport_seek_start = get_time_sec();
                                ps.transport_seek_last = ps.transport_seek_start;
                            } else if (new_zone == 1) {
                                player_seek(&ps, SEEK_LARGE_SEC);
                                ps.transport_seek_dir = 1;
                                ps.transport_seek_start = get_time_sec();
                                ps.transport_seek_last = ps.transport_seek_start;
                            } else {
                                ps.transport_seek_dir = 0;
                            }
                        } else {
                            /* Prev/Next focused: L/R navigates focus */
                            if (new_zone == -1 && ps.transport_focus > 0)
                                ps.transport_focus--;
                            else if (new_zone == 1 && ps.transport_focus < 2)
                                ps.transport_focus++;
                        }
                        stick_x_zone = new_zone;
                    }
                } else if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY && ps.transport_active) {
                    /* Y-axis always navigates focus (up=prev, down=next) */
                    static int stick_y_zone = 0;
                    float threshold = 19660.0f;
                    int new_zone = 0;
                    if (ev.gaxis.value < -threshold) new_zone = -1;  /* up */
                    else if (ev.gaxis.value > threshold) new_zone = 1; /* down */

                    if (new_zone != stick_y_zone) {
                        if (new_zone == -1 && ps.transport_focus > 0)
                            ps.transport_focus--;
                        else if (new_zone == 1 && ps.transport_focus < 2)
                            ps.transport_focus++;
                        stick_y_zone = new_zone;
                    }
                } else if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
                    float val = (float)ev.gaxis.value;
                    if (val < dead_zone) {
                        ps.trigger_seek_speed = 0.0f;
                    } else {
                        float norm = (val - dead_zone) / max_range;
                        ps.trigger_seek_speed = -(norm * norm) * 64.0f;
                    }
                } else if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
                    float val = (float)ev.gaxis.value;
                    if (val < dead_zone) {
                        ps.trigger_seek_speed = 0.0f;
                    } else {
                        float norm = (val - dead_zone) / max_range;
                        ps.trigger_seek_speed = (norm * norm) * 64.0f;
                    }
                }
                break;
            }
            }
        }

        /* ── Analog trigger seek (gamepad) ──
         * Applies a proportional seek each tick while a trigger is held.
         * Speed scales 0–64× via quadratic power curve.
         * Throttled to ~4 seeks/sec to avoid flooding the demuxer. */
        if (ps.playing && !ps.paused && ps.trigger_seek_speed != 0.0f) {
            static double last_trigger_seek = 0.0;
            double tnow = get_time_sec();
            if (tnow - last_trigger_seek >= 0.25) {
                double seek_delta = ps.trigger_seek_speed * 0.25;
                player_seek(&ps, seek_delta);
                last_trigger_seek = tnow;
                ps.show_seekbar = 1;                          /* ADD */
                ps.seekbar_hide_time = get_time_sec() + 3.0;  /* ADD */
            }
        }
        
        /* ── Transport stick-hold seek (accelerating) ──
         * 400ms initial delay, then repeats every 200ms.
         * Seek amount ramps: 30s base, +15s per second held.
         * Caps at 180s per tick (~5x speed at 10s hold). */
        if (ps.transport_active && ps.transport_seek_dir != 0) {
            double tnow = get_time_sec();
            double held = tnow - ps.transport_seek_start;
            if (held >= 0.40 && tnow - ps.transport_seek_last >= 0.20) {
                double amount = SEEK_LARGE_SEC + 15.0 * held;
                if (amount > 180.0) amount = 180.0;
                player_seek(&ps, ps.transport_seek_dir * amount);
                ps.transport_seek_last = tnow;
            }
        }

        /* ── D-pad repeat for browser scrolling ──
         * 300ms initial delay, then 80ms repeat rate. */
        if (ps.dpad_held_dir != 0 && ps.browser_active && !ps.playing) {
            double dnow = get_time_sec();
            double elapsed = dnow - ps.dpad_held_since;
            if (elapsed >= 0.30 && dnow - ps.dpad_last_repeat >= 0.08) {
                browser_navigate(&ps, ps.dpad_held_dir);
                ps.dpad_last_repeat = dnow;
            }
        }

        /* ── Async audio mode switch completion ──
         * Background thread (audio_switch_bg_func) is a thin signaler
         * since the profile bounce was removed. When it sets phase=2,
         * we complete the switch here on the main thread where it's
         * safe to touch audio state. */
        if (ps.audio_switch_phase == 2) {
            /* Join the background thread */
            if (ps.audio_switch_thread) {
                SDL_WaitThread(ps.audio_switch_thread, NULL);
                ps.audio_switch_thread = NULL;
            }

            static const char *mode_names_cpl[] = {
                "PCM (decode)", "AUTO", "PASSTHROUGH"
            };

            int switch_audio_ok = 1;
            if (ps.audio_switch_was_truehd) {
                log_msg("Audio: TrueHD on PCM return — auto-switching to decodable track");
                /* Unchecked open tolerated here only because audio_cycle's
                 * own reopen path is checked and disables on failure. */
                audio_open(&ps);
                if (ps.audio_stream)
                    SDL_PauseAudioStreamDevice(ps.audio_stream);
                audio_cycle(&ps);
            } else if (audio_open(&ps) < 0) {
                /* Checked like every sibling site (the finding-2 freeze
                 * class, fifth site): passthrough live, dock bumped, P
                 * pressed to fall back — device gone, and an unchecked
                 * open left a codec with no sink. audio_pq fills, the
                 * demux throttle gates forever, total playback freeze,
                 * while the mode OSD below claimed success. */
                log_msg("Audio: PCM open failed on mode-switch "
                        "completion — audio off");
                avcodec_free_context(&ps.audio_codec_ctx);
                audio_disable_public(&ps, "Audio: device error, audio off");
                switch_audio_ok = 0;
            } else {
                if (ps.audio_stream && !ps.paused)
                    SDL_ResumeAudioStreamDevice(ps.audio_stream);
                player_seek(&ps, 0.0);
            }

            ps.audio_switch_phase = 0;
            if (switch_audio_ok) {
                snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                         "Audio Mode: %s", mode_names_cpl[ps.audio_mode]);
                ps.aud_osd_until = get_time_sec() + 2.0;
            }
            log_msg("Audio mode switch complete: %s", mode_names_cpl[ps.audio_mode]);
        }

        /* ── Render ── */
        if (ps.playing && !ps.paused) {
            /* Decode pending subtitles (still queued for Phase 2) */
            sub_decode_pending(&ps);

            /* ── Render overlays to pixel buffer (before GPU submission) ── */
            overlay_render(&ps);

            /* Hide cursor when seek bar auto-hides */
            if (!ps.show_seekbar && !ps.show_debug && !ps.show_info)
                SDL_HideCursor();

            /* ── Video decode and A/V sync ──
             *
             * Two-tier pacing (unchanged from SDL_Renderer version):
             *   1. frame_timer governs WHEN to show a new frame based
             *      on content frame rate.
             *   2. VSync (via GPU swapchain) governs render loop rate.
             *
             * video_display() handles the full GPU submission:
             *   copy pass (upload planes) → render pass (shader draw) → submit.
             * video_reblit() re-draws the last frame without uploading. */
            double now = get_time_sec();
            int new_frame = 0;
            int frame_dropped = 0;   /* consumed but NOT displayed */

            /* v2: smoothed wall↔playback-clock offset — the scheduler's
             * time base (never raw audio_clock_sync samples: callback
             * granularity and the buffered_sec estimate would smear the
             * schedule). EMA α=0.02/tick ≈ 1s; a jump > 250ms is a
             * discontinuity (seek, pause, stall-resume clock snap) and
             * re-anchors hard. Video-only anchors once at the current
             * frame and never chases — wall clock = tempo fidelity. */
            if (!ps.paused && !ps.seek_recovering
                    && !ps.audio_stalled) {
                if (ps.audio_stream_idx >= 0) {
                    double off = now - ps.audio_clock_sync;
                    if (!ps.sched_off_valid
                            || fabs(off - ps.sched_off) > 0.25) {
                        ps.sched_off = off;
                        ps.sched_off_valid = 1;
                    } else {
                        ps.sched_off += 0.02 * (off - ps.sched_off);
                    }
                } else if (!ps.sched_off_valid && ps.video_ready
                           && ps.video_clock > 0.0) {
                    ps.sched_off = now - ps.video_clock;
                    ps.sched_off_valid = 1;
                }
            }

            /* ── Consume decoded frame from decode thread ──
             *
             * The decode thread decodes frames asynchronously
             * and writes one frame into ps.decoded_frame.  The main loop
             * consumes it here when frame_timer permits, then signals
             * the decode thread to decode the next frame.
             *
             * On ticks with no new frame (the common case at 24fps on
             * 60/120/144Hz), the main loop falls through to video_reblit()
             * which keeps the compositor fed at display refresh rate.
             * This eliminates the 22-37ms VAAPI decode stall that was
             * blocking reblits and causing visible judder. */

            int is_1to1 = (ps.pace_mode == PACE_LOCKED);

            SDL_LockMutex(ps.decode_mutex);
            int frame_avail = ps.decode_frame_ready;

            /* Consume gate. v2 SCHEDULED: a frame is admitted when its
             * scheduled slot arrives — now ≥ t_ideal − slot/2, with
             * t_ideal = pts + sched_off − modeled latency. An early
             * frame leaves the handoff full (decode backpressure, as
             * v1) and the tick reblits: that IS the repeat pattern
             * (2:3 for 24p; one repeat per ~17s for 59.94-on-60.00).
             * Everything else (LOCKED, seek recovery, stalled audio,
             * unanchored clock) gates on frame_timer. */
            int consume_ok;
            if (ps.pace_mode == PACE_SCHEDULED
                    && ps.sched_off_valid && !ps.seek_recovering
                    && !ps.audio_stalled) {
                double slot = (ps.pace_median > 0.0)
                              ? ps.pace_median : ps.frame_last_delay;
                if (slot <= 0.0 || slot > 0.1)
                    slot = 1.0 / 60.0;
                consume_ok = (now >= ps.decoded_pts + ps.sched_off
                                     - ps.av_bias - slot * 0.5);
                if (frame_avail && !consume_ok)
                    ps.sched_chain = 0;   /* next frame is early — no chase */
            } else {
                consume_ok = (now >= ps.frame_timer);
            }

            if (frame_avail && consume_ok) {
                /* ── Move decoded frame → video_frame ── */
                av_frame_unref(ps.video_frame);
                av_frame_move_ref(ps.video_frame, ps.decoded_frame);
                ps.video_clock = ps.decoded_pts;
                /* The pre-staged transfer set / pool slot follows its
                 * frame. The slot's plane refs moved with move_ref, so
                 * the slot stays alive exactly as long as video_frame
                 * holds this frame. */
                ps.video_frame_xfer = ps.decoded_frame_xfer;
                ps.decoded_frame_xfer = -1;
                ps.video_frame_slot = ps.decoded_frame_slot;
                ps.decoded_frame_slot = -1;
                ps.decode_frame_ready = 0;
                SDL_SignalCondition(ps.decode_cond);
                SDL_UnlockMutex(ps.decode_mutex);

                ps.diag_frames_decoded++;

                /* Compute inter-frame delay from PTS */
                double pts_delay = ps.video_clock - ps.frame_last_pts;
                if (pts_delay <= 0.0 || pts_delay >= 1.0)
                    pts_delay = ps.frame_last_delay;
                ps.frame_last_pts   = ps.video_clock;
                ps.frame_last_delay = pts_delay;

                /* A/V sync adjustment */
                double delay = pts_delay;
                double av_diff = 0.0;
                int one_to_one = 0;
                if (ps.audio_stream_idx >= 0) {
                    av_diff = ps.video_clock - ps.audio_clock_sync;

                    /* Latency model: EMA of av_diff absorbs
                     * systematic OS/device audio pipeline latency.
                     * v2 SCHEDULED: bias FROZEN. The scheduler pins
                     * av_diff onto the schedule (which subtracts the
                     * bias), so feeding the EMA here would be the
                     * controller measuring itself — the bias-
                     * absorption blind spot from the batch-2 field
                     * notes. LOCKED lets av_diff float free and
                     * learns the latency honestly; the scheduler
                     * consumes what LOCKED learned. */
                    if (!ps.seek_recovering && !ps.audio_stalled
                            && ps.pace_mode == PACE_LOCKED) {
                        ps.av_bias = ps.av_bias * 0.95 + av_diff * 0.05;
                        ps.av_bias_samples++;
                    }
                    ps.last_av_diff = av_diff;

                    /* v2 LOCKED EXIT CONTRACT (the entry contract's
                     * mirror): drift beyond the modeled bias by more
                     * than one frame, sustained ~10 displayed frames
                     * → hand back to SCHEDULED to repay. Wrong LOCKED
                     * entries self-heal for the cost of a couple of
                     * repeats; the warm reseek stays a deep-fault
                     * backstop. Evaluated before one_to_one below so
                     * an exit takes effect the same tick. */
                    if (ps.pace_mode == PACE_LOCKED
                            && !ps.seek_recovering) {
                        if (fabs(av_diff - ps.pace_bias_ref) > pts_delay) {
                            if (++ps.pace_drift_streak >= 10) {
                                ps.pace_mode = PACE_SCHEDULED;
                                ps.pace_drift_streak = 0;
                                ps.pace_enter_streak = 0;
                                ps.pace_exit_streak  = 0;
                                log_msg("PACE: -> SCHEDULED (drift "
                                        "exit: A/V %.1fms vs ref "
                                        "%.1fms)",
                                        av_diff * 1000.0,
                                        ps.pace_bias_ref * 1000.0);
                            }
                        } else {
                            ps.pace_drift_streak = 0;
                        }
                    }

                    /* LOCKED = 1:1 vsync-slaved: drops disabled,
                     * vsync is the pacing heartbeat; the mode machine
                     * (entry/exit contracts, cadence sensor) decides
                     * when that trust is warranted. */
                    one_to_one = (ps.pace_mode == PACE_LOCKED);

                    if (one_to_one && ps.av_bias_samples >= 120) {
                        /* Micro-correction: nudge frame_timer toward
                         * audio clock without triggering oscillation */
                        double bias = ps.av_bias;
                        if (bias < -0.200) bias = -0.200;
                        if (bias >  0.200) bias =  0.200;
                        delay = pts_delay + bias * 0.02;
                    }

                    /* Warmup guard: until the bias converges the raw diff still
                     * carries the seek transient, which is how a 4467ms "Peak
                     * A/V drift" got recorded for what was really a 5s seek.
                     * Under v2 SCHEDULED the bias is frozen and samples never
                     * accrue — an anchored scheduler clock is the equivalent
                     * warmed-up state (field: peak= stuck at 0.0 all run). */
                    if (!ps.seek_recovering
                            && (ps.av_bias_samples >= 60
                                || ps.sched_off_valid)
                            && fabs(av_diff) > fabs(ps.diag_max_av_drift))
                        ps.diag_max_av_drift = av_diff;
                }

                if (ps.pace_mode == PACE_SCHEDULED
                        && !ps.audio_stalled) {
                    /* ── SCHEDULED (batch 3): slot-assignment
                     * scheduler. The consume gate admitted this frame
                     * because its scheduled slot arrived. Show it
                     * unless it is LATE by more than half a slot —
                     * then its slot has already passed and the
                     * nearest-slot assignment belongs to a later
                     * frame: consume WITHOUT presenting (the b4265e7
                     * actuator) and chain straight to the next frame.
                     * Structural drops fall out of the assignment
                     * (evenly spaced, ~1 per R frames); repeats fall
                     * out of the reblit path when nothing is due. No
                     * thresholds, no rate limiter, no deep-burn
                     * special case. frame_timer is bookkeeping only:
                     * pinned to `now` so a LOCKED handover inherits a
                     * fresh timer and the v1 snap-forward stays
                     * silent. */
                    ps.frame_timer = now;
                    new_frame = 1;
                    if (ps.sched_off_valid && !ps.seek_recovering) {
                        double late = now - (ps.video_clock
                                             + ps.sched_off
                                             - ps.av_bias);
                        if (ps.audio_stream_idx < 0
                                && fabs(late) > 0.25) {
                            /* Video-only re-anchor (design decision —
                             * tempo fidelity): nothing to catch up TO
                             * after a discontinuity; resume exact
                             * tempo from here. */
                            ps.sched_off = now - ps.video_clock;
                            late = 0.0;
                        }
                        /* Drop test compares against the CONTENT
                         * period, not the slot: dropping is only
                         * right when the NEXT frame is nearer to this
                         * slot than the current one (late >
                         * pts_delay/2). Half-a-SLOT was wrong for
                         * content slower than the display — 24p
                         * due-times sweep the tick grid (41.7ms =
                         * 2.5 slots), so frames landed ~8ms late from
                         * pure tick quantization and were dropped for
                         * zero benefit: nothing newer existed to show
                         * for another 41.7ms (field: 14% drops,
                         * judder). For content at/above slot rate
                         * pts_delay ≈ slot and behavior is unchanged
                         * (structural drops, backlog burns). */
                        /* Video-only backstop: a late stuck in
                         * (pts_delay/2, 0.25) re-drops forever — no
                         * audio watchdog exists without a stream, and
                         * the 0.25 re-anchor above never fires. Cap
                         * the run and re-anchor tempo (the same op as
                         * the discontinuity re-anchor); this frame
                         * then displays. Audio-present streams keep
                         * their own 200ms stall watchdog path. */
                        if (late > pts_delay * 0.5
                                && ps.audio_stream_idx < 0
                                && ps.sched_drop_run >= 8) {
                            ps.sched_off = now - ps.video_clock;
                            late = 0.0;
                            ps.sched_drop_run = 0;
                            log_msg("DIAG: drop-chain backstop — "
                                    "re-anchored after 8 consecutive "
                                    "drops");
                        }
                        if (late > pts_delay * 0.5) {
                            new_frame = 0;
                            frame_dropped = 1;
                            ps.sched_chain = 1;
                            ps.sched_chain_start = now;
                            ps.sched_drop_run++;
                            ps.diag_frames_dropped++;
                            log_msg("DIAG: frame dropped at %.3fs "
                                    "(sched late %.1fms)",
                                    ps.video_clock, late * 1000.0);
                        } else {
                            ps.sched_drop_run = 0;
                            ps.sched_chain = 0;
                        }
                    }
                } else {
                /* LOCKED / fallback consume body: frame_timer paces at
                 * content cadence (vsync is the clock in LOCKED; the
                 * brief seek-recovery and audio-stall windows pace the
                 * same way until their handlers re-sync). */
                /* Minimum delay floor */
                double min_delay = ps.frame_last_delay * 0.5;
                if (delay < min_delay)
                    delay = min_delay;

                ps.frame_timer += delay;
                new_frame = 1;

                /* Cap: never let frame_timer get more than 100ms ahead
                 * of wall time.  Post-seek rapid frame consumption
                 * (catch-up drops with delay≈0) can accumulate
                 * frame_timer seconds ahead, causing a prolonged
                 * stall when the burst ends. */
                if (ps.frame_timer > now + 0.1)
                    ps.frame_timer = now + 0.1;

                }   /* end LOCKED/fallback consume body */

                /* 1:1 drift resync — the warm-reset, generalized.
                 * At 1:1, drops are disabled and the decode gate
                 * (mc=1) caps consumption at exactly content rate, so
                 * once video falls behind audio (compositor stall
                 * after a fullscreen toggle) NOTHING can ever catch
                 * up — the deficit is permanent, and the av_bias EMA
                 * slowly absorbs it and hides it from the corrected
                 * metric (field log: bias -4.4s). Detect sustained
                 * genuine drift on the RAW diff — 0.5s is far above
                 * any legitimate pipeline offset — and reseek to the
                 * audio clock, the same warm-reset that fixes
                 * cold-start drift. 90 CONSUMED ticks (drops count):
                 * ≈1.5s at 1:1 display cadence, but a slot-free drop
                 * burst can run the counter in well under a second —
                 * in N:1 this is a deep-fault backstop, not a timed
                 * grace period. Runs in every pacing mode. */
                /* v2: the backstop threshold is bias-relative and
                 * scaled in frame periods (a fixed −0.5s missed the
                 * −0.2s stranding class), referenced to the LOCKED
                 * snapshot when in LOCKED. Floor at −0.2s so a noisy
                 * reference can never hair-trigger a reseek. Counted
                 * on DISPLAYED frames under v2 so the ~1.5s grace is
                 * wall-time-true (drops counted it down in <1s). */
                double resync_ref = (ps.pace_mode == PACE_LOCKED)
                                    ? ps.pace_bias_ref : ps.av_bias;
                double resync_thresh = resync_ref - 10.0 * pts_delay;
                if (resync_thresh > -0.2)
                    resync_thresh = -0.2;
                if (ps.audio_stream_idx >= 0
                        && !ps.seek_recovering && av_diff < resync_thresh) {
                    if (new_frame
                            && ++ps.drift_resync_ticks >= 90) {
                        ps.drift_resync_ticks = 0;
                        log_msg("DIAG: drift resync — video %.2fs "
                                "behind audio, warm reseek",
                                -av_diff);
                        double pos = ps.audio_clock_sync;
                        if (pos < 0.1) pos = 0.1;
                        ps.seek_target  = (int64_t)(pos * AV_TIME_BASE);
                        ps.seek_flags   = AVSEEK_FLAG_BACKWARD;
                        ps.seek_request = 1;
                    }
                } else {
                    ps.drift_resync_ticks = 0;
                }

            } else {
                SDL_UnlockMutex(ps.decode_mutex);

                /* v2 SCHEDULED drop-chain: after a drop the next frame
                 * is ~0.3ms away in the decode thread. Reblitting now
                 * would spend a full display slot per dropped frame,
                 * and a backlog could never burn faster than the slot
                 * rate (the review-F3 arithmetic). Yield 1ms and
                 * re-poll instead; the safety timeout hands back to
                 * reblits if decode has genuinely stalled. */
                if (ps.pace_mode == PACE_SCHEDULED
                        && ps.sched_chain) {
                    if (now - ps.sched_chain_start > 0.05)
                        ps.sched_chain = 0;
                    else
                        SDL_Delay(1);
                }

                /* I/O error (stale NFS, network loss) — close and return to browser */
                if (ps.io_error) {
                    /* Unsupported-VAAPI-profile abort (P2-17): retry
                     * the same file in software decode instead of
                     * dumping the user to the browser. get_format set
                     * the flag; the hard-error streak escalated here. */
                    if (ps.vaapi_unsupported && !ps.force_swdec
                            && ps.filepath[0]) {
                        char retry_path[sizeof(ps.filepath)];
                        snprintf(retry_path, sizeof(retry_path), "%s",
                                 ps.filepath);
                        /* Keep the position across the close/reopen —
                         * the retry used to restart at 0:00, losing a
                         * shim resume entirely (finding 12). Flush the
                         * shim position too, in case the reopen fails. */
                        double retry_pos = ps.video_clock;
                        /* The decode-error streak can escalate before
                         * the demux thread services the startup
                         * DSVP_START_SEC seek — video_clock then still
                         * reads ~0 and the resume would silently drop.
                         * Nothing has played yet in that state, so the
                         * env resume point is the truth. */
                        if (s_shim && retry_pos <= 1.0) {
                            const char *ss = getenv("DSVP_START_SEC");
                            double sv = ss ? atof(ss) : 0.0;
                            if (sv > 1.0) retry_pos = sv;
                        }
                        if (s_shim && s_pos_file) {
                            /* Flush the resume point, not a possibly
                             * unserviced-seek zero; the file is about
                             * to be closed either way. */
                            ps.video_clock = retry_pos;
                            shim_write_pos(s_pos_file, &ps, 0);
                        }
                        log_msg("VAAPI: profile unsupported — reopening "
                                "in software decode");
                        player_close(&ps);
                        ps.quit = 0;
                        ps.force_swdec = 1;
                        int ok = (player_open(&ps, retry_path) == 0);
                        ps.force_swdec = 0;
                        if (ok && retry_pos > 1.0) {
                            log_msg("VAAPI retry: resuming at %.1fs",
                                    retry_pos);
                            player_seek(&ps, retry_pos);
                        }
                        if (ok) {
                            snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                     "Software decode (profile unsupported "
                                     "by hardware)");
                            ps.aud_osd_until = get_time_sec() + 3.0;
                            continue;
                        }
                        /* retry failed — fall through to the browser */
                    }
                    log_msg("I/O error detected — closing playback");
                    if (s_shim) {
                        /* A dead stream ends the shim session; the
                         * daemon reports the stop at last position.
                         * Only write one while a file is actually open:
                         * on the retry-failed fall-through the player
                         * is already closed and this wrote POS 0 over
                         * the flush above. */
                        if (s_pos_file && ps.fmt_ctx)
                            shim_write_pos(s_pos_file, &ps, 0);
                        player_close(&ps);
                        ps.quit = 1;
                        continue;
                    }
                    player_close(&ps);
                    ps.browser_active = 1;
                    ps.quit = 0;
                    continue;
                }

                /* Self-heal the audio-disable unwind race: demux routes
                 * packets by stream index with no lock, so one in-flight
                 * pq_put can land AFTER audio_disable's flush. With
                 * audio off nothing drains the queue, and a single
                 * stray packet blocked the EOF gate below forever —
                 * auto-play-next never fired after an audio failure. */
                if (ps.audio_stream_idx < 0 && ps.audio_pq.nb_packets > 0)
                    pq_flush(&ps.audio_pq);

                /* Stall-pause raced EOF (P2-10 edge): the watchdog can
                 * fire just before decode_eof lands, and stall-resume
                 * needs a new video frame that will never come — the
                 * paused stream then holds the audio tail in the queue
                 * and the gate below never passes. Unstick here. */
                if (ps.eof && ps.decode_eof && ps.audio_stalled) {
                    if (ps.audio_stream && !ps.paused)
                        SDL_ResumeAudioStreamDevice(ps.audio_stream);
                    ps.audio_stalled = 0;
                    log_msg("DIAG: audio resumed at EOF (tail drain)");
                }

                /* EOF detection: decode thread drained, no packets left */
                if (!frame_avail && ps.eof && ps.decode_eof
                        && ps.video_pq.nb_packets == 0
                        && ps.audio_pq.nb_packets == 0) {

                    /* ── Auto-play next file in folder ── */
                    int auto_played = 0;
                    if (ps.playlist_count > 0 && ps.playlist_index >= 0
                            && ps.playlist_index + 1 < ps.playlist_count) {
                        int next = ps.playlist_index + 1;
                        /* Playlist fields survive close/open untouched
                         * (owned here, not by player.c). */
                        player_close(&ps);

                        log_msg("Auto-play next: [%d/%d] %s",
                                next + 1, ps.playlist_count,
                                log_path(ps.playlist_files[next]));

                        if (player_open(&ps, ps.playlist_files[next]) == 0) {
                            gain_reset(&ps);
                            auto_played = 1;
                        } else {
                            log_msg("ERROR: Auto-play failed: %s",
                                    ps.playlist_files[next]);
                        }
                        ps.playlist_index = next;
                    }

                    if (!auto_played && s_shim) {
                        /* Natural EOF in a shim session: mark ended so
                         * the daemon reports the item finished, then
                         * exit — there is no browser to return to. */
                        log_msg("Playback finished — shim session, exiting");
                        if (s_pos_file) shim_write_pos(s_pos_file, &ps, 1);
                        player_close(&ps);
                        ps.quit = 1;
                        continue;
                    }
                    if (!auto_played) {
                        log_msg("Playback finished, returning to browser");
                        /* Sync browser to current file's directory */
                        if (ps.filepath[0]) {
                            char dir[1024];
                            snprintf(dir, sizeof(dir), "%s", ps.filepath);
                            char *sep = strrchr(dir, '/');
                            if (sep) {
                                *(sep + 1) = '\0';
                                snprintf(ps.browser_path, sizeof(ps.browser_path), "%s", dir);
                                browser_scan(&ps);
                                browser_save_path(&ps);
                            }
                        }
                        player_close(&ps);
                        ps.browser_active = 1;
                        ps.quit = 0;
                    }
                }
            }

            /* Snap forward on extreme stall.
             * In 1:1 VSync mode, use a wider 500ms threshold — VSync handles
             * pacing, so small drifts self-correct. The tight 100ms threshold
             * fires on minor GPU hiccups and causes visible judder.
             * In N:1 mode, keep the original 100ms for responsive catch-up. */
            {
                double snap_threshold = is_1to1 ? 0.5 : 0.1;
                if (ps.frame_timer < now - snap_threshold) {
                    ps.frame_timer = now;
                    ps.diag_timer_snaps++;
                    if (ps.video_clock > 0.5)
                        log_msg("DIAG: frame_timer snapped forward "
                                "(stall recovery at %.3fs)", ps.video_clock);
                }
            }

            /* Display the last decoded frame via GPU */
            if (new_frame) {
                video_display(&ps);
                ps.diag_frames_displayed++;
                ps.fps_window_frames++;   /* real-time FPS: content frame */
                ps.last_frame_wall = now;
                pacing_v2_present_tick(&ps, now);

                /* Resume from seek: first displayed frame post-seek.
                 *
                 * CRITICAL: re-sync audio clocks to video_clock here.
                 * av_seek_frame lands on a keyframe that may be seconds
                 * away from the seek target. The demux thread pre-sets
                 * both clocks to the target, but the first decoded frame
                 * overwrites video_clock to the actual keyframe PTS.
                 * Without this re-sync:
                 *   Forward seek: video_clock > audio_clock → A/V sync
                 *     computes multi-second delay, freezing the main loop.
                 *   Backward seek: video_clock < audio_clock → massive
                 *     negative drift, burst of frame drops.
                 */
                if (ps.seek_recovering) {
                    ps.seek_recovering = 0;
                    ps.frame_timer = get_time_sec();

                    /* Re-sync clocks to the actual first-frame PTS */
                    ps.audio_clock      = ps.video_clock;
                    ps.audio_clock_sync = ps.video_clock;
                    ps.audio_pts_floor  = ps.video_clock;
                    /* Bias deliberately preserved across seeks — see the note
                     * in player.c's seek handler. The clocks above are what
                     * needed re-syncing; the measured output latency did not. */
                    ps.frame_last_pts   = ps.video_clock;

                    /* Flush stale audio and resume */
                    if (ps.audio_stream) {
                        SDL_ClearAudioStream(ps.audio_stream);
                        if (!ps.paused)
                            SDL_ResumeAudioStreamDevice(ps.audio_stream);
                    }

                    log_msg("DIAG: seek recovery complete at %.3fs",
                            ps.video_clock);
                }

                /* Resume from stall: video is flowing again */
                if (ps.audio_stalled) {
                    if (ps.audio_stream) {
                        SDL_ClearAudioStream(ps.audio_stream);
                        ps.audio_clock      = ps.video_clock;
                        ps.audio_clock_sync = ps.video_clock;
                        ps.av_bias          = 0.0;
                        ps.av_bias_samples  = 0;
                        ps.frame_timer      = get_time_sec();

                        if (!ps.paused)
                            SDL_ResumeAudioStreamDevice(ps.audio_stream);
                    } else if (ps.bitstream_active) {
                        /* Passthrough: the feeder was gated by
                         * audio_stalled; a seek-style reset drops the
                         * stale ring and re-zeroes its clock (P2-11). */
                        ps.frame_timer = get_time_sec();
                        ps.bitstream_seek_pending = 1;
                    }

                    ps.audio_stalled = 0;
                    log_msg("DIAG: audio resumed after stall "
                            "(re-synced at %.3fs)", ps.video_clock);
                }
            }

            /* Stall watchdog: if 200ms passes without a displayed frame
             * during active playback, pause audio to prevent drift.
             * VAAPI can stall for seconds rebuilding its DPB after seeks
             * or on complex GOPs — audio must not run free during that. */
            if (ps.playing && !ps.paused && !ps.seek_recovering
                    && !ps.audio_stalled && ps.audio_stream_idx >= 0
                    && !(ps.eof && ps.decode_eof)
                    && ps.last_frame_wall > 0.0
                    && now - ps.last_frame_wall > 0.2) {
                /* eof exemption (P2-10): once video has fully ended,
                 * missing frames are not a stall — pausing here froze
                 * the audio tail in the queue and the EOF gate above
                 * (audio_pq empty) could never pass, killing
                 * auto-play-next on audio-tail files.
                 * Passthrough (P2-11): audio_stream is NULL — the
                 * pause below was a no-op and bitstream audio ran
                 * free through stalls. The feeder now gates on
                 * audio_stalled directly (bounded by ring depth). */
                if (ps.audio_stream)
                    SDL_PauseAudioStreamDevice(ps.audio_stream);
                ps.audio_stalled = 1;
                log_msg("DIAG: audio paused — video stall detected "
                        "(%.0fms gap at %.3fs)",
                        (now - ps.last_frame_wall) * 1000.0,
                        ps.video_clock);
            }

            /* ── Passthrough backend death → PCM fallback (P1-4) ──
             * The feeder exits and raises this flag when the PipeWire
             * stream errors post-open (undock / HDMI unplug). Finish
             * the fallback here on the main thread: the charter says
             * audio degrades to PCM, never to silence. */
            if (ps.bitstream_failed) {
                ps.bitstream_failed = 0;
                /* Stale-flag guard (review 2026-08-20 finding 3): the
                 * flag can outlive the stream that raised it (raised
                 * just before a pause, or before a mode/file change) —
                 * acting on it with no bitstream running reopened a
                 * stream over a live one: leaked SDL stream, two
                 * callbacks decoding one codec ctx. Only fall back
                 * when passthrough is actually the current sink; a
                 * stale flag just clears. */
                if (ps.bitstream_active) {
                    log_msg("Bitstream: backend failed — falling back to PCM decode");
                    bitstream_stop(&ps);
                    if (audio_open(&ps) < 0) {
                        /* Same shape as audio.c's twin: free the codec
                         * and stop here — falling through overwrote the
                         * disable notice with a "PCM decode" OSD while
                         * audio was actually off. */
                        log_msg("Audio: PCM fallback open failed — "
                                "audio off (finding 2 class)");
                        avcodec_free_context(&ps.audio_codec_ctx);
                        audio_disable_public(&ps,
                                "Audio: device error, audio off");
                    } else {
                        if (ps.audio_stream && !ps.paused)
                            SDL_ResumeAudioStreamDevice(ps.audio_stream);
                        snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                 "Passthrough lost — PCM decode");
                        ps.aud_osd_until = get_time_sec() + 3.0;
                    }
                }
            }
            /* Periodic diagnostics (every 10 seconds) */
            if (ps.playing && now - ps.diag_last_report >= 10.0) {
                double av_now = (ps.audio_stream_idx >= 0)
                    ? ps.video_clock - ps.audio_clock_sync : 0.0;
                log_msg("DIAG: [%.0fs] decoded=%d displayed=%d "
                        "dropped=%d snaps=%d "
                        "A/V=%.1fms peak=%.1fms bias=%.1fms "
                        "pacing=%s tick=%.1fms",
                        ps.video_clock,
                        ps.diag_frames_decoded,
                        ps.diag_frames_displayed,
                        ps.diag_frames_dropped,
                        ps.diag_timer_snaps,
                        av_now * 1000.0,
                        ps.diag_max_av_drift * 1000.0,
                        ps.av_bias * 1000.0,
                        ps.pace_mode == PACE_LOCKED
                            ? "LOCKED" : "SCHEDULED",
                        ps.pace_median * 1000.0);

                /* One-shot wire-truth capture: once per file, at the
                 * first periodic report with HDR passthrough active —
                 * KWin needs a couple of seconds after engage to
                 * program the connector, and by the first 10s report
                 * it has long settled. Logs the ACTUAL infoframe the
                 * kernel is sending (HDRWIRE: lines) so every HDR
                 * session carries its own metadata ground truth. */
                if (!ps.hdrwire_logged
                        && ps.gpu_uniforms.hdr_pass > 0.5f) {
                    hdrwire_log_state();
                    ps.hdrwire_logged = 1;
                }
#ifdef DSVP_PROFILE
                if (ps.prof_n > 0) {
                    log_msg("PROF: [%.0fs] n=%d  "
                            "decode=%.1f/%.1f  upload=%.1f/%.1f  "
                            "peak=%.1f/%.1f  vsync=%.1f/%.1f  "
                            "total=%.1f/%.1fms  (avg/max)",
                            ps.video_clock, ps.prof_n,
                            ps.prof_sum_decode / ps.prof_n,
                            ps.prof_max_decode,
                            ps.prof_sum_upload / ps.prof_n,
                            ps.prof_max_upload,
                            ps.prof_sum_peak / ps.prof_n,
                            ps.prof_max_peak,
                            ps.prof_sum_vsync / ps.prof_n,
                            ps.prof_max_vsync,
                            ps.prof_sum_total / ps.prof_n,
                            ps.prof_max_total);
                    /* Reset for next window */
                    ps.prof_n = 0;
                    ps.prof_sum_upload = ps.prof_sum_peak = 0.0;
                    ps.prof_sum_vsync = ps.prof_sum_total = 0.0;
                    ps.prof_sum_decode = 0.0;
                    ps.prof_max_upload = ps.prof_max_peak = 0.0;
                    ps.prof_max_vsync = ps.prof_max_total = 0.0;
                    ps.prof_max_decode = 0.0;
                }
#endif
                ps.diag_last_report = now;
            }

            /* Re-blit on ticks with no new frame (GPU double-buffering).
             *
             * NOT after a drop: a reblit blocks a full vsync slot
             * re-showing the OLD frame, so a drop that reblits costs
             * exactly what a display costs — video can never advance
             * faster than the slot rate, and in windowed (~57 slots/s
             * for 60fps content) catch-up was mathematically
             * impossible: drift pinned at the deep-drop threshold and
             * the audio stall-pause did the "recovery" (the storm).
             * Skipping the present lets the loop consume the next
             * frame immediately — the screen keeps the last presented
             * image, and a 250ms backlog burns in ~15 slot-free drops
             * (~15ms), invisible. This is what makes drops actually
             * DROP. */
            if (!new_frame && !frame_dropped && !ps.sched_chain
                    && ps.playing && ps.gpu_tex_y && ps.video_ready) {
                video_reblit(&ps);
                pacing_v2_present_tick(&ps, now);
            }

            /* ── Real-time FPS measurement (debug overlay) ──
             * One present per playing tick (video_display or video_reblit)
             * EXCEPT drop ticks, which present nothing and must not
             * count as renders. Roll a 0.5s window: long enough to be
             * stable, short enough to track seeks and stalls. Guarded
             * on ps.playing — if the EOF branch closed playback this
             * tick, no video present happened. */
            if (ps.playing) {
                if (!frame_dropped)
                    ps.rfps_window_frames++;
                if (ps.fps_window_start <= 0.0)
                    ps.fps_window_start = now;
                double fps_dt = now - ps.fps_window_start;
                if (fps_dt >= 0.5) {
                    ps.fps_content = ps.fps_window_frames  / fps_dt;
                    ps.fps_render  = ps.rfps_window_frames / fps_dt;
                    ps.fps_window_frames  = 0;
                    ps.rfps_window_frames = 0;
                    ps.fps_window_start   = now;
                }
            }

            /* If playback ended this tick (player_close was called in the
             * decode loop above), draw idle immediately so the swapchain
             * gets a frame.  Without this, one tick has no GPU submission
             * and some compositors (Gamescope/Steam Deck) show a stale
             * buffer instead of the last presented frame. */
            if (!ps.playing) {
                gpu_draw_idle(&ps);
                SDL_ShowCursor();
            }


        } else if (ps.playing && ps.paused) {
            /* Paused — decode pending subs, render overlays, redraw current frame */
            sub_decode_pending(&ps);
            overlay_render(&ps);
            if (!ps.show_seekbar && !ps.show_debug && !ps.show_info)
                SDL_HideCursor();
            if (ps.gpu_tex_y) {
                video_reblit(&ps);
            }
        } else {
            /* No media loaded — draw browser (or idle if browser inactive) */
            gpu_draw_idle(&ps);
            SDL_ShowCursor();
        }

        /* ── PRESENT-RATE DIAG (opt-in: DSVP_DIAG=1) ──
         * Reports BOTH loop iteration rate and TRUE submit rate
         * (ps.presents, counted at the three actual submit sites).
         * The old iteration-only figure lied in exactly the states
         * that matter: acquire failures and drop chains iterate
         * without presenting (2026-08-14 FS investigation). A healthy
         * gap of ~0 means "every iteration reached the glass"; iters
         * high with presents low is the freeze signature. This is
         * what proved the KWin VRR-floor blanking below — it stays in
         * the tree, off by default rather than deleted. "ret2browser"
         * is the return-to-browser latch, not an active browser: it
         * stays 1 through playback by design. */
        if (s_diag) {
            double pr_now = get_time_sec();
            pr_n++;
            if (pr_t0 <= 0.0) pr_t0 = pr_now;
            if (pr_now - pr_t0 >= 1.0) {
                log_msg("PRESENT DIAG: %.0f iters/s, %.0f presents/s (playing=%d paused=%d ret2browser=%d fs=%d)",
                        pr_n / (pr_now - pr_t0),
                        (ps.presents - pr_presents_last) / (pr_now - pr_t0),
                        ps.playing, ps.paused,
                        ps.browser_active, ps.fullscreen);
                pr_n = 0;
                pr_presents_last = ps.presents;
                pr_t0 = pr_now;
            }
        }

        /* Shim position hand-off, ~1/s while playing. */
        if (s_pos_file && ps.playing) {
            static double s_pos_t0 = 0.0;
            double pnow = get_time_sec();
            if (pnow - s_pos_t0 >= 1.0) {
                s_pos_t0 = pnow;
                shim_write_pos(s_pos_file, &ps, 0);
            }
        }

        /* Don't burn CPU when idle or paused — EXCEPT in fullscreen.
         * KWin engages adaptive sync (VRR "Automatic") for fullscreen
         * surfaces as of Plasma 6.4 (SteamOS 3.8.24). This delay plus
         * the VSync block in swapchain acquire throttles paused/browser
         * presents to ~30/s — below an OLED TV's VRR floor (~40Hz on
         * the LG C4, dock DP-1) — and the panel blanks until the rate
         * recovers. In fullscreen, skip the delay: VSync alone paces
         * the loop at refresh rate and the acquire blocks, so CPU cost
         * stays negligible. Windowed keeps the throttle (compositor
         * retains the last buffer there, and it never blanked). */
        if ((!ps.playing || ps.paused) && !ps.fullscreen) {
            SDL_Delay(16); /* ~60fps idle */
        }
    }

    /* ── Cleanup ── */
    log_msg("Shutting down");
    /* Never leave the display in HDR after we exit — player_close
     * covers normal file close; this covers every app-exit path. */
    hdr_output_shutdown(&ps);
    /* An async audio-track switch still in flight reads &ps, which is main's
     * stack — join it before that stack frame goes away. */
    if (ps.audio_switch_thread) {
        SDL_WaitThread(ps.audio_switch_thread, NULL);
        ps.audio_switch_thread = NULL;
    }
    /* Covers the exit paths that didn't flush (SIGTERM, window close):
     * the daemon's stop report uses whatever position landed last. */
    if (s_pos_file && ps.playing) shim_write_pos(s_pos_file, &ps, 0);
    if (ps.playing) player_close(&ps);
    if (ps.gamepad) SDL_CloseGamepad(ps.gamepad);
    browser_free_entries(&ps);
    playlist_free(&ps);
    overlay_cleanup();
    sub_close_font();
    gpu_destroy_pipelines(&ps);
    SDL_ReleaseWindowFromGPUDevice(gpu_device, window);
    SDL_DestroyGPUDevice(gpu_device);
    SDL_DestroyWindow(window);
    SDL_ShaderCross_Quit();
    SDL_Quit();
    log_close();

    return 0;
}
