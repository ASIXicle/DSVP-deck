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
 * Phase 1 (v0.1.4-beta): Video-only GPU rendering. Overlays (debug,
 * info, seek bar, subtitles) are disabled — log output provides all
 * diagnostic data. Phase 2 will composite overlays as GPU textures.
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
    /* Native Win32 file dialog */
    OPENFILENAMEA ofn;
    char file[1024] = {0};

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = NULL;
    ofn.lpstrFile    = file;
    ofn.nMaxFile     = sizeof(file);
    ofn.lpstrFilter  = "Video Files\0"
                       "*.mkv;*.mp4;*.avi;*.mov;*.wmv;*.flv;*.webm;*.m4v;*.ts;*.mpg;*.mpeg\0"
                       "Audio Files\0"
                       "*.mp3;*.flac;*.wav;*.aac;*.ogg;*.opus;*.m4a;*.wma\0"
                       "All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags        = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        snprintf(out, out_size, "%s", file);
        return 1;
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
 * GPU Idle Screen (no media loaded)
 * ═══════════════════════════════════════════════════════════════════
 *
 * Dark background with DSVP title, version, and hotkey reference
 * rendered via the overlay system.
 */

static void gpu_draw_idle(PlayerState *ps) {
    /* Render idle screen text to overlay pixel buffer */
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
        return;
    }
    if (!swapchain_tex) {
        SDL_CancelGPUCommandBuffer(cmd);
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
}


/* ═══════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
    /* ── Initialize logging (before anything else) ── */
    log_init();
    log_msg("Starting DSVP v" DSVP_VERSION " (argc=%d)", argc);

    /* ── Initialize SDL ── */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
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

    /* ── Create GPU device ──
     * SDL_GPU replaces SDL_Renderer. Uses shadercross to query which
     * shader formats the platform supports (DXIL on Windows D3D12,
     * SPIRV on Vulkan, MSL on Metal). Debug mode enables GPU validation. */
    SDL_GPUDevice *gpu_device = SDL_CreateGPUDevice(
        SDL_ShaderCross_GetSPIRVShaderFormats(),
        true,   /* debug_mode — GPU validation layers */
        NULL    /* preferred driver — auto-select */
    );
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

    /* ── Set VSync via swapchain parameters ── */
    SDL_SetGPUSwapchainParameters(gpu_device, window,
        SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
        SDL_GPU_PRESENTMODE_VSYNC);
    log_msg("GPU: swapchain set to SDR + VSync");

    /* ── Initialize subtitle font (Phase 2 will use for GPU overlay) ── */
    if (sub_init_font() < 0) {
        log_msg("WARNING: Subtitle rendering disabled (no font)");
    }

    /* ── Initialize player state ── */
    PlayerState ps;
    memset(&ps, 0, sizeof(ps));
    ps.window     = window;
    ps.gpu_device = gpu_device;
    ps.volume     = 1.00;
    ps.video_stream_idx = -1;
    ps.audio_stream_idx = -1;
    ps.sub_active_idx   = -1;
    ps.win_w = DEFAULT_WIN_W;
    ps.win_h = DEFAULT_WIN_H;

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
    if (argc > 1) {
        if (player_open(&ps, argv[1]) != 0) {
            log_msg("ERROR: Failed to open: %s", argv[1]);
        }
    }

    /* ── Main loop ── */
    while (!ps.quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {

            case SDL_EVENT_QUIT:
                if (ps.playing) player_close(&ps);
                ps.quit = 1;
                break;

            case SDL_EVENT_KEY_DOWN:
                switch (ev.key.key) {

                case SDLK_Q:
                    if (ps.playing) {
                        player_close(&ps);
                        ps.quit = 0; /* don't exit, return to idle */
                    } else {
                        ps.quit = 1;
                    }
                    break;

                case SDLK_O: {
                    char path[1024] = {0};
                    log_msg("File open dialog requested");
                    /* Pause audio while dialog blocks the render loop */
                    int was_playing = ps.playing && !ps.paused;
                    if (was_playing && ps.audio_stream)
                        SDL_PauseAudioStreamDevice(ps.audio_stream);
                    if (open_file_dialog(path, sizeof(path))) {
                        log_msg("Opening file: %s", path);
                        if (ps.playing) player_close(&ps);
                        ps.quit = 0;
                        if (player_open(&ps, path) != 0) {
                            log_msg("ERROR: Failed to open: %s", path);
                        }
                    } else {
                        log_msg("File dialog cancelled");
                        /* Resume audio and resync frame timer */
                        if (was_playing && ps.audio_stream) {
                            ps.frame_timer = get_time_sec();
                            SDL_ResumeAudioStreamDevice(ps.audio_stream);
                        }
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
                        }
                    }
                    break;

                case SDLK_F:
                    /* Pause audio during mode switch to prevent drift */
                    if (ps.playing && !ps.paused && ps.audio_stream)
                        SDL_PauseAudioStreamDevice(ps.audio_stream);
                    ps.fullscreen = !ps.fullscreen;
                    SDL_SetWindowFullscreen(window,
                        ps.fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
                    if (ps.playing) {
                        ps.frame_timer = get_time_sec();
                        if (!ps.paused && ps.audio_stream)
                            SDL_ResumeAudioStreamDevice(ps.audio_stream);
                    }
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

                case SDLK_S:
                    sub_cycle(&ps);
                    break;

                case SDLK_A:
                    audio_cycle(&ps);
                    break;

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

                default:
                    break;
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.button.button == SDL_BUTTON_LEFT && ev.button.clicks == 2) {
                    /* Double-click → toggle fullscreen */
                    if (ps.playing && !ps.paused && ps.audio_stream)
                        SDL_PauseAudioStreamDevice(ps.audio_stream);
                    ps.fullscreen = !ps.fullscreen;
                    SDL_SetWindowFullscreen(window,
                        ps.fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
                    if (ps.playing) {
                        ps.frame_timer = get_time_sec();
                        if (!ps.paused && ps.audio_stream)
                            SDL_ResumeAudioStreamDevice(ps.audio_stream);
                    }
                }

                /* Click on seek bar to seek (Phase 2: re-enable with overlays) */
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

            case SDL_EVENT_MOUSE_MOTION:
                /* Show overlays on mouse movement, auto-hide after 3s */
                SDL_ShowCursor();
                if (ps.playing) {
                    ps.show_seekbar = 1;
                    ps.seekbar_hide_time = get_time_sec() + 1.5;
                }
                break;

            case SDL_EVENT_WINDOW_RESIZED:
                    ps.win_w = ev.window.data1;
                    ps.win_h = ev.window.data2;
                break;
            }
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
            int decoded_this_tick = 0;

            int max_catchup = 4;
            while (now >= ps.frame_timer && max_catchup-- > 0) {
                int vret = video_decode_frame(&ps);
                if (vret > 0) {
                    decoded_this_tick++;
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
                    if (ps.audio_stream_idx >= 0) {
                        av_diff = ps.video_clock - ps.audio_clock;
                        double threshold = fmax(pts_delay, 0.01);

                        if (av_diff > threshold) {
                            delay = pts_delay + av_diff;
                        } else if (av_diff < -threshold) {
                            delay = 0.0;
                        }

                        if (!ps.seek_recovering &&
                                fabs(av_diff) > fabs(ps.diag_max_av_drift))
                            ps.diag_max_av_drift = av_diff;
                    }

                    /* Minimum delay floor */
                    double min_delay = ps.frame_last_delay * 0.5;
                    if (delay < min_delay)
                        delay = min_delay;

                    ps.frame_timer += delay;
                    new_frame = 1;

                    /* Drop frame if video is >50ms behind audio */
                    if (ps.audio_stream_idx >= 0 && av_diff < -0.05
                            && !ps.seek_recovering) {
                        new_frame = 0;
                        ps.diag_frames_dropped++;
                        log_msg("DIAG: frame dropped at %.3fs "
                                "(A/V drift: %.1fms)",
                                ps.video_clock, av_diff * 1000.0);
                    }
                } else {
                    if (vret < 0) {
                        log_msg("Video decode error at clock=%.3f",
                                ps.video_clock);
                    } else if (ps.eof && ps.video_pq.nb_packets == 0
                                    && ps.audio_pq.nb_packets == 0) {
                        log_msg("Playback finished, returning to idle");
                        player_close(&ps);
                        ps.quit = 0;
                    }
                    break;
                }
            }

            if (decoded_this_tick > 1) {
                ps.diag_multi_decodes++;
            }

            /* Snap forward on extreme stall */
            if (ps.frame_timer < now - 0.1) {
                ps.frame_timer = now;
                ps.diag_timer_snaps++;
                log_msg("DIAG: frame_timer snapped forward "
                        "(stall recovery at %.3fs)", ps.video_clock);
            }

            /* Display the last decoded frame via GPU */
            if (new_frame) {
                video_display(&ps);
                ps.diag_frames_displayed++;

                if (ps.seek_recovering) {
                    ps.seek_recovering = 0;
                    ps.frame_timer = get_time_sec();
                    log_msg("DIAG: seek recovery complete at %.3fs",
                            ps.video_clock);
                }
            }

            /* Periodic diagnostics (every 10 seconds) */
            if (ps.playing && now - ps.diag_last_report >= 10.0) {
                double av_now = (ps.audio_stream_idx >= 0)
                    ? ps.video_clock - ps.audio_clock : 0.0;
                log_msg("DIAG: [%.0fs] decoded=%d displayed=%d "
                        "dropped=%d multi_ticks=%d snaps=%d "
                        "A/V=%.1fms peak=%.1fms",
                        ps.video_clock,
                        ps.diag_frames_decoded,
                        ps.diag_frames_displayed,
                        ps.diag_frames_dropped,
                        ps.diag_multi_decodes,
                        ps.diag_timer_snaps,
                        av_now * 1000.0,
                        ps.diag_max_av_drift * 1000.0);
                ps.diag_last_report = now;
            }

            /* Re-blit on ticks with no new frame (GPU double-buffering) */
            if (!new_frame && ps.playing && ps.gpu_tex_y) {
                video_reblit(&ps);
            }


        } else if (ps.playing && ps.paused) {
            /* Paused — decode pending subs, render overlays, redraw current frame */
            sub_decode_pending(&ps);
            overlay_render(&ps);
            if (ps.gpu_tex_y) {
                video_reblit(&ps);
            }
        } else {
            /* No media loaded — draw idle screen */
            gpu_draw_idle(&ps);
            SDL_ShowCursor();
        }

        /* Don't burn CPU when idle or paused */
        if (!ps.playing || ps.paused) {
            SDL_Delay(16); /* ~60fps idle */
        }
    }

    /* ── Cleanup ── */
    log_msg("Shutting down");
    if (ps.playing) player_close(&ps);
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
