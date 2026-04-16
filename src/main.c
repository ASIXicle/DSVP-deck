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

/* ═══════════════════════════════════════════════════════════════════
 * Folder Playlist — prev/next file navigation
 * ═══════════════════════════════════════════════════════════════════
 *
 * Scans the parent directory of the current file for playable media,
 * sorts alphabetically, and allows navigating to adjacent entries.
 */

const char *video_extensions[] = {
    ".mkv", ".mp4", ".avi", ".mov", ".wmv", ".flv", ".webm", ".m4v",
    ".ts", ".m2ts", ".mpg", ".mpeg", ".3gp",
    ".mp3", ".flac", ".wav", ".aac", ".ogg", ".opus", ".m4a", ".wma",
    NULL
};

/* HDR midtone gain index — file-scope so it can be reset on file open.
 * Index 3 = 1.3f, matching the default gpu_uniforms.hdr_midtone_gain. */
static int s_gain_idx = 3;

int is_media_file(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (int i = 0; video_extensions[i]; i++) {
        if (strcasecmp(dot, video_extensions[i]) == 0) return 1;
    }
    return 0;
}

static int cmp_strings(const void *a, const void *b) {
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    return strcasecmp(sa, sb);
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
        strcpy(dir, ".");
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
            log_msg("playlist_scan: cannot open directory: %s", dir);
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
        /* Compare against full filepath */
        if (strcmp(files[i], ps->filepath) == 0) {
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
    log_msg("FFmpeg %s (libavcodec %d.%d)", av_version_info(),
            LIBAVCODEC_VERSION_MAJOR, LIBAVCODEC_VERSION_MINOR);

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
    ps.hdr_target_idx = 0;  /* default: 203 nits (industry standard) */
    ps.gpu_uniforms.hdr_target_nits = 203.0f;
    ps.gpu_uniforms.hdr_midtone_gain = 1.3f;  /* default: moderate midtone lift */
    ps.audio_mode = AUDIO_MODE_AUTO;  /* probe HDMI sink, passthrough if supported */

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
            log_msg("ERROR: Failed to open: %s", open_path);
        } else {
            s_gain_idx = 3;
            playlist_scan(&ps);
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
    if (ps.playing && ps.filepath[0]) {
        /* Set browser to directory of the opened file */
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
    while (!ps.quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {

            case SDL_EVENT_QUIT:
                if (ps.playing) player_close(&ps);
                ps.quit = 1;
                break;

            case SDL_EVENT_KEY_DOWN:
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
                                    log_anon_active() ? "[redacted]" : ps.browser_selected_file);
                            if (player_open(&ps, ps.browser_selected_file) != 0) {
                                log_msg("ERROR: Failed to open file");
                            } else {
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
                switch (ev.key.key) {

                case SDLK_Q:
                    if (ps.playing) {
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
                     * If playing, close first so we return to browser. */
                    log_msg("File browser requested (O key)");
                    if (ps.playing) player_close(&ps);
                    s_gain_idx = 3;
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
                    /* Returning to windowed: resize to current video's aspect ratio.
                     * Without this, opening a different-aspect file while fullscreen
                     * leaves the old window shape (stale black bars). */
                    if (!ps.fullscreen && ps.playing && ps.vid_w > 0 && ps.vid_h > 0) {
                        const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(
                            SDL_GetPrimaryDisplay());
                        int max_w = dm ? (int)(dm->w * 0.8) : 1920;
                        int max_h = dm ? (int)(dm->h * 0.8) : 1080;
                        int w = ps.vid_w, h = ps.vid_h;
                        if (w > max_w || h > max_h) {
                            double scale = fmin((double)max_w / w, (double)max_h / h);
                            w = (int)(w * scale);
                            h = (int)(h * scale);
                        }
                        ps.win_w = w;
                        ps.win_h = h;
                        SDL_SetWindowSize(window, w, h);
                    }
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

                case SDLK_H:
                    if (ps.playing && ps.gpu_uniforms.is_hdr > 0.0f) {
                        int mode = (int)ps.gpu_uniforms.hdr_debug;
                        mode = (mode + 1) % 4;
                        ps.gpu_uniforms.hdr_debug = (float)mode;
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
                        static const float targets[] = { 203.0f, 300.0f, 400.0f };
                        ps.hdr_target_idx = (ps.hdr_target_idx + 1) % 3;
                        ps.gpu_uniforms.hdr_target_nits = targets[ps.hdr_target_idx];
                        snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                 "SDR target: %.0f nits", targets[ps.hdr_target_idx]);
                        ps.aud_osd_until = get_time_sec() + 2.0;
                        log_msg("HDR: SDR target changed to %.0f nits",
                                targets[ps.hdr_target_idx]);
                    }
                    break;

                case SDLK_G:
                    if (ps.playing && ps.gpu_uniforms.is_hdr > 0.0f) {
                        static const float gains[] = { 1.0f, 1.1f, 1.2f, 1.3f, 1.35f, 1.4f };
                        s_gain_idx = (s_gain_idx + 1) % 6;
                        ps.gpu_uniforms.hdr_midtone_gain = gains[s_gain_idx];
                        snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                 "Midtone gain: %.2f", gains[s_gain_idx]);
                        ps.aud_osd_until = get_time_sec() + 2.0;
                        log_msg("HDR: midtone gain changed to %.2f",
                                gains[s_gain_idx]);
                    }
                    break;

                case SDLK_S:
                    sub_cycle(&ps);
                    break;

                case SDLK_V:
                    /* Toggle VSync mode: FIFO (strict) ↔ MAILBOX (triple-buf).
                     * MAILBOX prevents deadline-miss cascades at the cost of
                     * occasional repeated frames instead of hard stalls.
                     * Falls back if MAILBOX not supported (some Vulkan drivers). */
                {
                    int want_mailbox = !ps.present_mailbox;
                    SDL_GPUPresentMode mode = want_mailbox
                        ? SDL_GPU_PRESENTMODE_MAILBOX
                        : SDL_GPU_PRESENTMODE_VSYNC;
                    if (SDL_SetGPUSwapchainParameters(gpu_device, window,
                            SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode)) {
                        ps.present_mailbox = want_mailbox;
                        snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                 "Present: %s",
                                 want_mailbox ? "MAILBOX" : "VSYNC");
                        log_msg("Present mode: %s",
                                want_mailbox ? "MAILBOX" : "VSYNC");
                    } else {
                        snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                                 "MAILBOX not supported");
                        log_msg("Present mode: MAILBOX not supported (%s)",
                                SDL_GetError());
                    }
                    ps.aud_osd_until = get_time_sec() + 2.0;
                    break;
                }

                case SDLK_A:
                    audio_cycle(&ps);
                    break;

                case SDLK_P:
                {
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
                     * Wire the mode change to the audio subsystem so it
                     * takes effect immediately, not just on next file open.
                     * After restart, seek to current position to force the
                     * demux to reposition -- otherwise its 8s read-ahead
                     * fills the audio queue with future packets. */
                    if (ps.playing && ps.audio_codec_ctx) {
                        int did_seek = 0;
                        if (ps.audio_mode == AUDIO_MODE_PCM && ps.bitstream_active) {
                            bitstream_stop(&ps);
                            /* If current track is TrueHD, we CANNOT PCM-decode it
                             * on the Deck — 1200 pkt/sec MLP decode starves video
                             * within 200ms. Switch to the EAC3 compatibility track.
                             * audio_cycle does its own codec switch + seek. */
                            if (ps.audio_codec_ctx &&
                                ps.audio_codec_ctx->codec_id == AV_CODEC_ID_TRUEHD) {
                                log_msg("Audio: TrueHD on PCM return — auto-switching to decodable track");
                                audio_open(&ps);
                                /* Pause immediately — without this, the audio
                                 * callback fires while audio_codec_ctx is still
                                 * the TrueHD context, producing garbage/silence.
                                 * audio_cycle resumes after switching to EAC3. */
                                if (ps.audio_stream)
                                    SDL_PauseAudioStreamDevice(ps.audio_stream);
                                audio_cycle(&ps);
                                did_seek = 1;  /* audio_cycle already seeked */
                            } else {
                                audio_open(&ps);
                                /* Resume SDL audio immediately — seek_recovering
                                 * gate is for initial file open, not mode switch.
                                 * Without this, audio stays paused until a frame
                                 * display triggers seek_recovery clear. */
                                if (ps.audio_stream && !ps.paused)
                                    SDL_ResumeAudioStreamDevice(ps.audio_stream);
                            }
                        } else if (ps.audio_mode != AUDIO_MODE_PCM && !ps.bitstream_active) {
                            audio_close(&ps);
                            if (!bitstream_start(&ps))
                                audio_open(&ps);  /* fallback to PCM */
                        }
                        if (!did_seek)
                            player_seek(&ps, 0.0);
                    } else if (ps.audio_mode == AUDIO_MODE_PCM) {
                        ps.bitstream_active = 0;
                    }

                    snprintf(ps.aud_osd, sizeof(ps.aud_osd),
                             "Audio Mode: %s", mode_names[ps.audio_mode]);
                    ps.aud_osd_until = get_time_sec() + 2.0;
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
                            /* Save playlist state before close */
                            char **saved_files = ps.playlist_files;
                            int saved_count = ps.playlist_count;
                            int saved_index = next;
                            ps.playlist_files = NULL; /* prevent close from freeing */
                            ps.playlist_count = 0;

                            int was_fs = ps.fullscreen;
                            player_close(&ps);
                            ps.fullscreen = was_fs;

                            log_msg("Playlist nav: opening [%d/%d] %s",
                                    saved_index + 1, saved_count,
                                    saved_files[saved_index]);

                            if (player_open(&ps, saved_files[saved_index]) == 0) {
                                s_gain_idx = 3;
                                ps.playlist_files = saved_files;
                                ps.playlist_count = saved_count;
                                ps.playlist_index = saved_index;
                            } else {
                                log_msg("ERROR: Failed to open: %s",
                                        saved_files[saved_index]);
                                /* Restore playlist so user can try again */
                                ps.playlist_files = saved_files;
                                ps.playlist_count = saved_count;
                                ps.playlist_index = saved_index;
                            }
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

                /* Click on seek bar — buttons and progress track.
                 * Geometry must match overlay.c draw_seekbar() layout:
                 *   [btn_margin][◀][gap][▶][gap][time][12][==track==][12][sep][vol][margin]
                 * s = UI scale factor (1 windowed, 2 fullscreen) must
                 * match s_ui_scale in overlay.c. */
                if (ev.button.button == SDL_BUTTON_LEFT && ps.playing
                      && ps.show_seekbar) {
                    int h_now;
                    SDL_GetWindowSizeInPixels(window, NULL, &h_now);
                    float density = SDL_GetWindowPixelDensity(window);
                    int mx = (int)(ev.button.x * density);
                    int my = (int)(ev.button.y * density);
                    int s = ps.game_mode ? 3 : (ps.fullscreen ? 2 : 1);
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

            case SDL_EVENT_WINDOW_RESIZED:
                    ps.win_w = ev.window.data1;
                    ps.win_h = ev.window.data2;
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
                                    log_anon_active() ? "[redacted]" : ps.browser_selected_file);
                            if (player_open(&ps, ps.browser_selected_file) != 0) {
                                log_msg("ERROR: Failed to open file");
                            } else {
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
                        if (!ps.paused)
                            ps.frame_timer = get_time_sec();
                    }
                    break;

                case SDL_GAMEPAD_BUTTON_EAST:   /* B — Back / Stop */
                    if (ps.playing && ps.transport_active) {
                        ps.transport_active = 0;
                        ps.seekbar_hide_time = get_time_sec() + 3.0;
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
                                player_seek(&ps, -30.0);
                                ps.transport_seek_dir = -1;
                                ps.transport_seek_start = get_time_sec();
                                ps.transport_seek_last = ps.transport_seek_start;
                            } else if (new_zone == 1) {
                                player_seek(&ps, 30.0);
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
                double amount = 30.0 + 15.0 * held;
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

            /* ── Consume decoded frame from decode thread ──
             *
             * The decode thread runs video_decode_frame() asynchronously
             * and writes one frame into ps.decoded_frame.  The main loop
             * consumes it here when frame_timer permits, then signals
             * the decode thread to decode the next frame.
             *
             * On ticks with no new frame (the common case at 24fps on
             * 60/120/144Hz), the main loop falls through to video_reblit()
             * which keeps the compositor fed at display refresh rate.
             * This eliminates the 22-37ms VAAPI decode stall that was
             * blocking reblits and causing visible judder. */

            int is_1to1 = (ps.frame_last_delay > 0.001
                           && ps.frame_last_delay < 0.020);

            SDL_LockMutex(ps.decode_mutex);
            int frame_avail = ps.decode_frame_ready;

            if (frame_avail && now >= ps.frame_timer) {
                /* ── Move decoded frame → video_frame ── */
                av_frame_unref(ps.video_frame);
                av_frame_move_ref(ps.video_frame, ps.decoded_frame);
                ps.video_clock = ps.decoded_pts;
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
                double av_diff_c = 0.0;
                int one_to_one = 0;
                if (ps.audio_stream_idx >= 0) {
                    av_diff = ps.video_clock - ps.audio_clock_sync;

                    /* Adaptive bias correction: EMA of av_diff
                     * absorbs systematic OS audio pipeline latency.
                     * Only the catch-up (negative) branch uses the
                     * corrected value — the slow-down (positive)
                     * branch uses raw av_diff to avoid overcorrection. */
                    if (!ps.seek_recovering) {
                        ps.av_bias = ps.av_bias * 0.95 + av_diff * 0.05;
                        ps.av_bias_samples++;
                    }
                    av_diff_c = av_diff;
                    if (ps.av_bias_samples >= 60) {
                        double bias = ps.av_bias;
                        if (bias < -0.200) bias = -0.200;
                        if (bias >  0.200) bias =  0.200;
                        av_diff_c = av_diff - bias;
                    }

                    /* 1:1 VSync pacing: when content frame rate
                     * matches display refresh (~50-60fps), VSync
                     * alone provides the pacing heartbeat. Full
                     * A/V delay correction at 1:1 causes oscillation
                     * because any jitter triggers multi-decode
                     * bunching.
                     *
                     * Instead, once the bias EMA has converged
                     * (~2s of playback), apply a micro-correction:
                     * 2% of the converged bias per frame.  At 50ms
                     * bias this is ~1ms/frame on a 16.67ms period —
                     * too small to cause a tick skip, converges in
                     * ~1 second. */
                    one_to_one = (pts_delay > 0.001
                                  && pts_delay < 0.020);

                    double threshold = fmax(pts_delay, 0.01);
                    if (!one_to_one) {
                        if (av_diff > threshold) {
                            delay = pts_delay + av_diff;
                        } else if (av_diff_c < -threshold) {
                            delay = 0.0;
                        }
                    } else if (ps.av_bias_samples >= 120) {
                        /* Micro-correction: nudge frame_timer toward
                         * audio clock without triggering oscillation */
                        double bias = ps.av_bias;
                        if (bias < -0.200) bias = -0.200;
                        if (bias >  0.200) bias =  0.200;
                        delay = pts_delay + bias * 0.02;
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

                /* Cap: never let frame_timer get more than 100ms ahead
                 * of wall time.  Post-seek rapid frame consumption
                 * (catch-up drops with delay≈0) can accumulate
                 * frame_timer seconds ahead, causing a prolonged
                 * stall when the burst ends. */
                if (ps.frame_timer > now + 0.1)
                    ps.frame_timer = now + 0.1;

                /* Drop frame if video is genuinely behind audio.
                 *
                 * At 1:1 (content fps ≈ display refresh), drops are
                 * DISABLED. VSync provides the pacing heartbeat and
                 * the snap-forward handles genuine stalls. The raw
                 * av_diff at 1:1 includes a fixed pipeline offset
                 * (decode latency + OS audio buffering) that isn't
                 * growing drift — the decoder IS keeping up. Dropping
                 * on that offset replaces smooth 60fps video with a
                 * frozen frame, which is far worse than the offset.
                 *
                 * For non-1:1 content (e.g. 24fps on 60Hz), the
                 * accumulator-based timing needs active correction,
                 * so bias-corrected drops still apply at -50ms. */
                if (!one_to_one && ps.audio_stream_idx >= 0
                        && !ps.seek_recovering) {
                    double drop_diff = (ps.av_bias_samples >= 60)
                                       ? av_diff_c : av_diff;
                    if (drop_diff < -0.05) {
                        new_frame = 0;
                        ps.diag_frames_dropped++;
                        log_msg("DIAG: frame dropped at %.3fs "
                                "(A/V drift: %.1fms)",
                                ps.video_clock, av_diff * 1000.0);
                    }
                }
            } else {
                SDL_UnlockMutex(ps.decode_mutex);

                /* I/O error (stale NFS, network loss) — close and return to browser */
                if (ps.io_error) {
                    log_msg("I/O error detected — closing playback, returning to browser");
                    player_close(&ps);
                    ps.browser_active = 1;
                    ps.quit = 0;
                    continue;
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
                        char **saved_files = ps.playlist_files;
                        int saved_count = ps.playlist_count;
                        ps.playlist_files = NULL;
                        ps.playlist_count = 0;

                        int was_fs = ps.fullscreen;
                        player_close(&ps);
                        ps.fullscreen = was_fs;

                        log_msg("Auto-play next: [%d/%d] %s",
                                next + 1, saved_count,
                                saved_files[next]);

                        if (player_open(&ps, saved_files[next]) == 0) {
                            ps.playlist_files = saved_files;
                            ps.playlist_count = saved_count;
                            ps.playlist_index = next;
                            auto_played = 1;
                        } else {
                            log_msg("ERROR: Auto-play failed: %s",
                                    saved_files[next]);
                            ps.playlist_files = saved_files;
                            ps.playlist_count = saved_count;
                            ps.playlist_index = next;
                        }
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

            /* Deferred warm-reset for 60fps content: after 1.5s of playback,
             * seek to current position to reset timing with warm pipeline.
             * Cold-start decode variance causes irrecoverable drift at mc=1. */
            if (ps.warm_reset_time > 0.0 && now >= ps.warm_reset_time) {
                ps.warm_reset_time = 0.0;
                log_msg("DIAG: warm-reset seek at %.3fs (60fps cold-start fix)",
                        ps.video_clock);
                player_seek(&ps, 0);  /* seek to current position — resets timing */
            }

            /* Snap forward on extreme stall */
            if (ps.frame_timer < now - 0.1) {
                ps.frame_timer = now;
                ps.diag_timer_snaps++;
                /* Suppress log for the first-frame snap at file open (always
                 * fires because decode pipeline takes >100ms to produce frame 1).
                 * Only log mid-playback snaps which indicate real stalls. */
                if (ps.video_clock > 0.5)
                    log_msg("DIAG: frame_timer snapped forward "
                            "(stall recovery at %.3fs)", ps.video_clock);
            }

            /* Display the last decoded frame via GPU */
            if (new_frame) {
                video_display(&ps);
                ps.diag_frames_displayed++;
                ps.last_frame_wall = now;

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
                    ps.av_bias          = 0.0;
                    ps.av_bias_samples  = 0;
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
                    SDL_ClearAudioStream(ps.audio_stream);
                    ps.audio_clock      = ps.video_clock;
                    ps.audio_clock_sync = ps.video_clock;
                    ps.av_bias          = 0.0;
                    ps.av_bias_samples  = 0;
                    ps.frame_timer      = get_time_sec();

                    if (!ps.paused)
                        SDL_ResumeAudioStreamDevice(ps.audio_stream);

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
                    && ps.last_frame_wall > 0.0
                    && now - ps.last_frame_wall > 0.2) {
                SDL_PauseAudioStreamDevice(ps.audio_stream);
                ps.audio_stalled = 1;
                log_msg("DIAG: audio paused — video stall detected "
                        "(%.0fms gap at %.3fs)",
                        (now - ps.last_frame_wall) * 1000.0,
                        ps.video_clock);
            }
            /* Periodic diagnostics (every 10 seconds) */
            if (ps.playing && now - ps.diag_last_report >= 10.0) {
                double av_now = (ps.audio_stream_idx >= 0)
                    ? ps.video_clock - ps.audio_clock_sync : 0.0;
                log_msg("DIAG: [%.0fs] decoded=%d displayed=%d "
                        "dropped=%d snaps=%d "
                        "A/V=%.1fms peak=%.1fms bias=%.1fms "
                        "pacing=%s",
                        ps.video_clock,
                        ps.diag_frames_decoded,
                        ps.diag_frames_displayed,
                        ps.diag_frames_dropped,
                        ps.diag_timer_snaps,
                        av_now * 1000.0,
                        ps.diag_max_av_drift * 1000.0,
                        ps.av_bias * 1000.0,
                        is_1to1 ? "1:1(mc=1)" : "N:1(mc=4)");
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

            /* Re-blit on ticks with no new frame (GPU double-buffering) */
            if (!new_frame && ps.playing && ps.gpu_tex_y && ps.video_ready) {
                video_reblit(&ps);
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

        /* Don't burn CPU when idle or paused */
        if (!ps.playing || ps.paused) {
            SDL_Delay(16); /* ~60fps idle */
        }
    }

    /* ── Cleanup ── */
    log_msg("Shutting down");
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
