/*
 * DSVP — Dead Simple Video Player
 * player.c — Demux, video decode, display, seeking, media info
 *
 * Threading model:
 *   - Demux thread: reads packets from the container, pushes to queues
 *   - Main thread:  pops video packets, decodes, scales, renders
 *   - SDL audio thread: calls audio_callback() which decodes audio
 *
 * A/V sync strategy:
 *   Audio is the master clock. Video frame display timing is adjusted
 *   to match the audio clock. This is the standard approach (same as
 *   ffplay) because audio glitches are far more perceptible than
 *   dropped/delayed video frames. Words so I can remove embarassing commit spelling error
 */

#include "dsvp.h"

/* ═══════════════════════════════════════════════════════════════════
 * Packet Queue — thread-safe FIFO for AVPackets
 * ═══════════════════════════════════════════════════════════════════ */

void pq_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond  = SDL_CreateCond();
}

void pq_destroy(PacketQueue *q) {
    pq_flush(q);
    if (q->mutex) SDL_DestroyMutex(q->mutex);
    if (q->cond)  SDL_DestroyCond(q->cond);
}

/* Push a packet onto the queue. Caller still owns pkt after this call
 * returns — we move the packet data into a new AVPacket internally. */
int pq_put(PacketQueue *q, AVPacket *pkt) {
    PacketNode *node = av_malloc(sizeof(PacketNode));
    if (!node) return -1;

    node->pkt = av_packet_alloc();
    if (!node->pkt) {
        av_free(node);
        return -1;
    }
    av_packet_move_ref(node->pkt, pkt);
    node->next = NULL;

    SDL_LockMutex(q->mutex);

    if (!q->last) {
        q->first = node;
    } else {
        q->last->next = node;
    }
    q->last = node;
    q->nb_packets++;
    q->size += node->pkt->size;

    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
    return 0;
}

/* Pop a packet from the queue. If block=1, waits until data arrives
 * or abort_request is set. Returns 1 on success, 0 if non-blocking
 * and empty, -1 if aborted. */
int pq_get(PacketQueue *q, AVPacket *pkt, int block) {
    int ret = -1;

    SDL_LockMutex(q->mutex);
    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        PacketNode *node = q->first;
        if (node) {
            q->first = node->next;
            if (!q->first) q->last = NULL;
            q->nb_packets--;
            q->size -= node->pkt->size;

            av_packet_move_ref(pkt, node->pkt);
            av_packet_free(&node->pkt);
            av_free(node);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

/* Flush all packets from the queue. Called on seek or close. */
void pq_flush(PacketQueue *q) {
    SDL_LockMutex(q->mutex);
    PacketNode *node = q->first;
    while (node) {
        PacketNode *next = node->next;
        av_packet_free(&node->pkt);
        av_free(node);
        node = next;
    }
    q->first = NULL;
    q->last  = NULL;
    q->nb_packets = 0;
    q->size = 0;
    SDL_UnlockMutex(q->mutex);
}


/* ═══════════════════════════════════════════════════════════════════
 * Open / Close
 * ═══════════════════════════════════════════════════════════════════ */

/* Open a media file: probe format, find best streams, init decoders,
 * set up scaling context, create SDL texture, start demux thread. */
int player_open(PlayerState *ps, const char *filename) {
    int ret;

    strncpy(ps->filepath, filename, sizeof(ps->filepath) - 1);
    log_msg("player_open: %s", filename);

    /* ── Open container ── */
    ps->fmt_ctx = NULL;
    ret = avformat_open_input(&ps->fmt_ctx, filename, NULL, NULL);
    if (ret < 0) {
        log_msg("ERROR: avformat_open_input failed: %s", av_err2str(ret));
        return -1;
    }

    ret = avformat_find_stream_info(ps->fmt_ctx, NULL);
    if (ret < 0) {
        log_msg("ERROR: avformat_find_stream_info failed: %s", av_err2str(ret));
        avformat_close_input(&ps->fmt_ctx);
        return -1;
    }
    log_msg("Container: %s (%s), streams=%d",
        ps->fmt_ctx->iformat->name, ps->fmt_ctx->iformat->long_name,
        ps->fmt_ctx->nb_streams);

    /* ── Find best video stream ── */
    ps->video_stream_idx = av_find_best_stream(ps->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    ps->audio_stream_idx = av_find_best_stream(ps->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, ps->video_stream_idx, NULL, 0);

    if (ps->video_stream_idx < 0) {
        log_msg("ERROR: No video stream found");
        avformat_close_input(&ps->fmt_ctx);
        return -1;
    }
    log_msg("Video stream: idx=%d, Audio stream: idx=%d",
        ps->video_stream_idx, ps->audio_stream_idx);

    /* ── Open video decoder (SOFTWARE ONLY) ── */
    {
        AVStream *vs = ps->fmt_ctx->streams[ps->video_stream_idx];
        const AVCodec *codec = avcodec_find_decoder(vs->codecpar->codec_id);
        if (!codec) {
            log_msg("ERROR: Unsupported video codec id=%d", vs->codecpar->codec_id);
            avformat_close_input(&ps->fmt_ctx);
            return -1;
        }
        log_msg("Video codec: %s (%s)", codec->name, codec->long_name);

        ps->video_codec_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(ps->video_codec_ctx, vs->codecpar);

        /* Force software decode — use all available CPU threads */
        ps->video_codec_ctx->thread_count = 0; /* auto-detect */
        ps->video_codec_ctx->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;

        ret = avcodec_open2(ps->video_codec_ctx, codec, NULL);
        if (ret < 0) {
            fprintf(stderr, "[DSVP] Cannot open video codec: %s\n", av_err2str(ret));
            avformat_close_input(&ps->fmt_ctx);
            return -1;
        }

        ps->vid_w = ps->video_codec_ctx->width;
        ps->vid_h = ps->video_codec_ctx->height;
        log_msg("Video: %dx%d, pix_fmt=%s, threads=%d",
            ps->vid_w, ps->vid_h,
            av_get_pix_fmt_name(ps->video_codec_ctx->pix_fmt),
            ps->video_codec_ctx->thread_count);
    }

    /* ── Open audio decoder ── */
    if (ps->audio_stream_idx >= 0) {
        AVStream *as = ps->fmt_ctx->streams[ps->audio_stream_idx];
        const AVCodec *codec = avcodec_find_decoder(as->codecpar->codec_id);
        if (codec) {
            ps->audio_codec_ctx = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(ps->audio_codec_ctx, as->codecpar);
            ps->audio_codec_ctx->thread_count = 0;
            ret = avcodec_open2(ps->audio_codec_ctx, codec, NULL);
            if (ret < 0) {
                fprintf(stderr, "[DSVP] Cannot open audio codec: %s\n", av_err2str(ret));
                avcodec_free_context(&ps->audio_codec_ctx);
                ps->audio_stream_idx = -1;
            }
        } else {
            ps->audio_stream_idx = -1;
        }
    }

    /* ── Find subtitle streams ── */
    sub_find_streams(ps);

    /* ── Find audio streams ── */
    audio_find_streams(ps);

    /* ── Allocate decode frames ── */
    ps->video_frame = av_frame_alloc();
    ps->rgb_frame   = av_frame_alloc();
    ps->audio_frame = av_frame_alloc();

    /* ── Set up swscale ── */
    /*
     * Two modes based on whether the source needs spatial scaling:
     *
     * 1. RESIZE (src size ≠ dst size): Full Lanczos pipeline with
     *    SWS_FULL_CHR_H_INT for maximum spatial quality.
     *
     * 2. FORMAT ONLY (src size == dst size, e.g. 10-bit → 8-bit):
     *    SWS_POINT (nearest neighbor) for the spatial component,
     *    which is a no-op identity at 1:1 resolution. The quality-
     *    relevant work is error-diffusion dithering (set below),
     *    which is independent of the scaling filter.
     *
     *    Lanczos at 1:1 computes an expensive multi-tap convolution
     *    that produces identical output to SWS_POINT. For 2960×2160
     *    10-bit HEVC, this wastes ~70% of the sws_scale CPU time.
     */
    {
        enum AVPixelFormat dst_fmt = AV_PIX_FMT_YUV420P;
        int dst_w = ps->vid_w;
        int dst_h = ps->vid_h;

        /* Detect whether we need spatial scaling or just format conversion */
        int same_size = (ps->vid_w == dst_w && ps->vid_h == dst_h);
        int sws_flags;
        const char *sws_mode;

        if (same_size) {
            /* Format conversion only — spatial filter is identity */
            sws_flags = SWS_POINT | SWS_ACCURATE_RND;
            sws_mode = "format-only (SWS_POINT + error-diffusion)";
        } else {
            /* Spatial resize — full quality pipeline */
            sws_flags = SWS_LANCZOS | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
            sws_mode = "resize (SWS_LANCZOS + error-diffusion)";
        }

        ps->sws_ctx = sws_getContext(
            ps->vid_w, ps->vid_h, ps->video_codec_ctx->pix_fmt,
            dst_w, dst_h, dst_fmt,
            sws_flags,
            NULL, NULL, NULL
        );

        if (!ps->sws_ctx) {
            log_msg("ERROR: Cannot create swscale context");
            player_close(ps);
            return -1;
        }

        /* Enable error-diffusion dithering for bit-depth conversion.
         * Value 1 = SWS_DITHER_ED in FFmpeg's internal enum. Using the
         * integer directly for compatibility across FFmpeg versions. */
        av_opt_set_int(ps->sws_ctx, "dithering", 1, 0);

        /* ── Colorspace and range ──
         *
         * Tell swscale the correct source colorspace (BT.601 vs BT.709)
         * and range (limited/TV vs full/PC). Without this, swscale guesses
         * based on resolution heuristics, which can produce crushed blacks
         * (limited treated as full) or washed-out grays (full treated as
         * limited). We read the actual values from the stream metadata. */
        {
            AVCodecParameters *par = ps->fmt_ctx->streams[ps->video_stream_idx]->codecpar;

            /* Determine source colorspace matrix */
            int src_cs;
            if (par->color_space != AVCOL_SPC_UNSPECIFIED) {
                /* Use what the container/codec reports */
                src_cs = (par->color_space == AVCOL_SPC_BT709)
                    ? SWS_CS_ITU709 : SWS_CS_ITU601;
            } else {
                /* Fallback: HD (≥720p) → BT.709, SD → BT.601 */
                src_cs = (ps->vid_h >= 720) ? SWS_CS_ITU709 : SWS_CS_ITU601;
            }

            /* Output uses the same matrix (no gamut conversion needed
             * since we're just converting bit depth, not colorspace) */
            int dst_cs = src_cs;

            /* Determine source range: 0 = limited/TV, 1 = full/PC */
            int src_range;
            if (par->color_range == AVCOL_RANGE_JPEG) {
                src_range = 1; /* full range */
            } else if (par->color_range == AVCOL_RANGE_MPEG) {
                src_range = 0; /* limited range */
            } else {
                src_range = 0; /* assume limited (vast majority of video) */
            }
            int dst_range = src_range;

            /* Retrieve defaults, then override with correct values */
            int *inv_table, *table;
            int cur_src_range, cur_dst_range, brightness, contrast, saturation;
            sws_getColorspaceDetails(ps->sws_ctx,
                &inv_table, &cur_src_range, &table, &cur_dst_range,
                &brightness, &contrast, &saturation);

            sws_setColorspaceDetails(ps->sws_ctx,
                sws_getCoefficients(src_cs), src_range,
                sws_getCoefficients(dst_cs), dst_range,
                brightness, contrast, saturation);

            log_msg("swscale: colorspace=%s range=%s",
                (src_cs == SWS_CS_ITU709) ? "BT.709" : "BT.601",
                src_range ? "full" : "limited");
        }

        /* ── Chroma siting ──
         *
         * For 4:2:0 subsampled video, the exact position of chroma samples
         * affects interpolation quality. MPEG-style (chroma between luma
         * rows) and JPEG-style (chroma co-sited with top-left luma) produce
         * different results. We read chroma_location from the stream and
         * pass it to swscale. This is "free" correctness. */
        {
            AVCodecParameters *par = ps->fmt_ctx->streams[ps->video_stream_idx]->codecpar;
            const char *chroma_desc = "default";

            if (par->chroma_location == AVCHROMA_LOC_LEFT) {
                av_opt_set_int(ps->sws_ctx, "src_h_chr_pos", 0, 0);
                av_opt_set_int(ps->sws_ctx, "src_v_chr_pos", 128, 0);
                chroma_desc = "left (MPEG-2)";
            } else if (par->chroma_location == AVCHROMA_LOC_CENTER) {
                av_opt_set_int(ps->sws_ctx, "src_h_chr_pos", 128, 0);
                av_opt_set_int(ps->sws_ctx, "src_v_chr_pos", 128, 0);
                chroma_desc = "center (MPEG-1/JPEG)";
            } else if (par->chroma_location == AVCHROMA_LOC_TOPLEFT) {
                av_opt_set_int(ps->sws_ctx, "src_h_chr_pos", 0, 0);
                av_opt_set_int(ps->sws_ctx, "src_v_chr_pos", 0, 0);
                chroma_desc = "top-left";
            }

            log_msg("swscale: chroma siting=%s", chroma_desc);
        }

        log_msg("swscale: mode=%s", sws_mode);

        /* Allocate buffer for the converted frame */
        int buf_size = av_image_get_buffer_size(dst_fmt, dst_w, dst_h, 32);
        ps->rgb_buffer = av_malloc(buf_size);
        av_image_fill_arrays(ps->rgb_frame->data, ps->rgb_frame->linesize,
                             ps->rgb_buffer, dst_fmt, dst_w, dst_h, 32);
    }

    /* ── Resize window to video dimensions ── */
    {
        /* Cap to 80% of screen, maintain aspect ratio */
        SDL_DisplayMode dm;
        SDL_GetCurrentDisplayMode(0, &dm);
        int max_w = (int)(dm.w * 0.8);
        int max_h = (int)(dm.h * 0.8);

        int w = ps->vid_w;
        int h = ps->vid_h;

        if (w > max_w || h > max_h) {
            double scale = fmin((double)max_w / w, (double)max_h / h);
            w = (int)(w * scale);
            h = (int)(h * scale);
        }

        ps->win_w = w;
        ps->win_h = h;

        SDL_SetWindowSize(ps->window, w, h);
        SDL_SetWindowPosition(ps->window,
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

        /* Update window title with filename */
        const char *basename = strrchr(filename, '/');
        if (!basename) basename = strrchr(filename, '\\');
        basename = basename ? basename + 1 : filename;
        char title[512];
        snprintf(title, sizeof(title), "DSVP — %s", basename);
        SDL_SetWindowTitle(ps->window, title);
    }

    /* ── Create SDL texture for video ── */

    /* Set texture scaling quality — applies to the NEXT texture created.
     * "2" = anisotropic (best), "1" = bilinear (good fallback).
     * This controls how SDL scales the texture when the display rect
     * differs from the native video resolution (window resize, fullscreen).
     * Without this, SDL defaults to nearest-neighbor (blocky). */
    if (!SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "2")) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        log_msg("SDL: using bilinear texture scaling (anisotropic unavailable)");
    } else {
        log_msg("SDL: using anisotropic texture scaling");
    }

    /* Set YUV→RGB conversion matrix based on resolution.
     * BT.601 is the SD standard (< 720p), BT.709 is HD (≥ 720p).
     * SDL defaults to BT.601, which produces subtly wrong colors on HD. */
    if (ps->vid_h >= 720) {
        SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT709);
        log_msg("SDL: YUV conversion set to BT.709 (HD)");
    } else {
        SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT601);
        log_msg("SDL: YUV conversion set to BT.601 (SD)");
    }

    if (ps->texture) SDL_DestroyTexture(ps->texture);
    ps->texture = SDL_CreateTexture(
        ps->renderer,
        SDL_PIXELFORMAT_IYUV,         /* = YUV420P */
        SDL_TEXTUREACCESS_STREAMING,
        ps->vid_w, ps->vid_h
    );
    if (!ps->texture) {
        fprintf(stderr, "[DSVP] Cannot create texture: %s\n", SDL_GetError());
        player_close(ps);
        return -1;
    }

    /* ── Init packet queues ── */
    pq_init(&ps->video_pq);
    pq_init(&ps->audio_pq);
    for (int i = 0; i < ps->sub_count; i++)
        pq_init(&ps->sub_pqs[i]);

    /* ── Seek mutex (protects codec flush vs decode) ── */
    ps->seek_mutex = SDL_CreateMutex();
    ps->seeking    = 0;

    /* ── Init timing ── */
    ps->frame_timer      = get_time_sec();
    ps->frame_last_delay = 0.04;   /* assume ~25fps initially */
    ps->frame_last_pts   = 0.0;
    ps->audio_clock      = 0.0;
    ps->video_clock      = 0.0;

    /* Suppress frame drops until the first frame is displayed.
     * Adapts automatically to any codec's keyframe recovery time. */
    ps->seek_recovering = 1;

    /* ── Reset diagnostics ── */
    ps->diag_frames_displayed = 0;
    ps->diag_frames_decoded   = 0;
    ps->diag_frames_dropped   = 0;
    ps->diag_multi_decodes    = 0;
    ps->diag_timer_snaps      = 0;
    ps->diag_max_av_drift     = 0.0;
    ps->diag_last_report      = get_time_sec();

    /* ── Open audio output ── */
    if (ps->audio_codec_ctx) {
        audio_open(ps);
    }

    /* ── Start demux thread ── */
    ps->eof     = 0;
    ps->playing = 1;
    ps->paused  = 0;
    ps->demux_thread = SDL_CreateThread(demux_thread_func, "demux", ps);

    /* Build media info string */
    player_build_media_info(ps);

    return 0;
}

/* Close playback: stop threads, free all resources. */
void player_close(PlayerState *ps) {
    if (!ps->playing && !ps->fmt_ctx) return;
    log_msg("player_close: stopping playback");

    /* ── Playback diagnostics summary ── */
    if (ps->diag_frames_decoded > 0) {
        double drop_pct = (ps->diag_frames_decoded > 0)
            ? (100.0 * ps->diag_frames_dropped / ps->diag_frames_decoded)
            : 0.0;
        log_msg("DIAG: === Playback Summary ===");
        log_msg("DIAG:   Frames decoded:   %d", ps->diag_frames_decoded);
        log_msg("DIAG:   Frames displayed:  %d", ps->diag_frames_displayed);
        log_msg("DIAG:   Frames dropped:    %d (%.2f%%)",
                ps->diag_frames_dropped, drop_pct);
        log_msg("DIAG:   Multi-decode ticks: %d", ps->diag_multi_decodes);
        log_msg("DIAG:   Timer snap-forwards: %d", ps->diag_timer_snaps);
        log_msg("DIAG:   Peak A/V drift:    %.1fms",
                ps->diag_max_av_drift * 1000.0);
    }

    ps->quit = 1;

    /* Signal queues to unblock any waiting threads */
    ps->video_pq.abort_request = 1;
    ps->audio_pq.abort_request = 1;
    SDL_CondSignal(ps->video_pq.cond);
    SDL_CondSignal(ps->audio_pq.cond);

    /* Wait for demux thread */
    if (ps->demux_thread) {
        SDL_WaitThread(ps->demux_thread, NULL);
        ps->demux_thread = NULL;
    }

    /* Close audio */
    audio_close(ps);

    /* Close subtitles */
    sub_close_codec(ps);

    /* Flush queues */
    pq_destroy(&ps->video_pq);
    pq_destroy(&ps->audio_pq);
    for (int i = 0; i < ps->sub_count; i++)
        pq_destroy(&ps->sub_pqs[i]);

    /* Destroy seek mutex */
    if (ps->seek_mutex) { SDL_DestroyMutex(ps->seek_mutex); ps->seek_mutex = NULL; }

    /* Free frames */
    if (ps->video_frame)  av_frame_free(&ps->video_frame);
    if (ps->rgb_frame)    av_frame_free(&ps->rgb_frame);
    if (ps->audio_frame)  av_frame_free(&ps->audio_frame);

    /* Free buffers */
    if (ps->rgb_buffer)   { av_free(ps->rgb_buffer); ps->rgb_buffer = NULL; }
    if (ps->audio_buf)    { av_free(ps->audio_buf);  ps->audio_buf  = NULL; }

    /* Free scale/resample contexts */
    if (ps->sws_ctx)      { sws_freeContext(ps->sws_ctx); ps->sws_ctx = NULL; }
    if (ps->swr_ctx)      { swr_free(&ps->swr_ctx); ps->swr_ctx = NULL; }

    /* Free codecs */
    if (ps->video_codec_ctx) avcodec_free_context(&ps->video_codec_ctx);
    if (ps->audio_codec_ctx) avcodec_free_context(&ps->audio_codec_ctx);

    /* Close format */
    if (ps->fmt_ctx) avformat_close_input(&ps->fmt_ctx);

    /* Destroy texture */
    if (ps->texture) { SDL_DestroyTexture(ps->texture); ps->texture = NULL; }

    /* Reset state */
    ps->playing            = 0;
    ps->paused             = 0;
    ps->eof                = 0;
    ps->quit               = 0;
    ps->video_stream_idx   = -1;
    ps->audio_stream_idx   = -1;
    ps->audio_buf_size     = 0;
    ps->audio_buf_index    = 0;
    ps->seek_request       = 0;
    ps->seeking            = 0;
    ps->seek_recovering    = 0;
    ps->show_debug         = 0;
    ps->show_info          = 0;
    ps->aud_count          = 0;
    ps->aud_selection      = 0;
    ps->aud_osd[0]         = '\0';
    ps->sub_count          = 0;
    ps->sub_selection      = 0;
    ps->sub_active_idx     = -1;
    ps->sub_valid          = 0;
    ps->sub_is_bitmap      = 0;
    ps->sub_bitmap_count   = 0;
    ps->sub_text[0]        = '\0';
    ps->sub_osd[0]         = '\0';

    /* Reset window */
    SDL_SetWindowTitle(ps->window, DSVP_WINDOW_TITLE);
    SDL_SetWindowSize(ps->window, DEFAULT_WIN_W, DEFAULT_WIN_H);
    SDL_SetWindowPosition(ps->window,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}


/* ═══════════════════════════════════════════════════════════════════
 * Demux Thread
 * ═══════════════════════════════════════════════════════════════════
 *
 * Reads packets from the container file and distributes them to
 * the video and audio packet queues.
 */

int demux_thread_func(void *arg) {
    PlayerState *ps = (PlayerState *)arg;
    AVPacket *pkt = av_packet_alloc();
    log_msg("Demux thread started");

    while (!ps->quit) {
        /* ── Handle seek requests ── */
        if (ps->seek_request) {
            int64_t target = ps->seek_target;
            log_msg("Demux: seeking to %.3f s", (double)target / AV_TIME_BASE);

            /* CRITICAL: Lock the seek mutex. This prevents the main thread
             * from calling avcodec_send_packet/receive_frame on the video
             * codec while we flush it. The audio callback is also paused. */
            SDL_LockMutex(ps->seek_mutex);
            ps->seeking = 1;

            /* Pause audio device so callback can't touch audio codec */
            if (ps->audio_dev)
                SDL_PauseAudioDevice(ps->audio_dev, 1);

            int ret = av_seek_frame(ps->fmt_ctx, -1, target, ps->seek_flags);
            if (ret < 0) {
                log_msg("ERROR: Seek failed: %s", av_err2str(ret));
            } else {
                log_msg("Demux: av_seek_frame OK, flushing queues");
                pq_flush(&ps->video_pq);
                pq_flush(&ps->audio_pq);
                for (int i = 0; i < ps->sub_count; i++)
                    pq_flush(&ps->sub_pqs[i]);
                log_msg("Demux: queues flushed, flushing video codec");
                if (ps->video_codec_ctx)
                    avcodec_flush_buffers(ps->video_codec_ctx);
                log_msg("Demux: video codec flushed, flushing audio codec");
                if (ps->audio_codec_ctx)
                    avcodec_flush_buffers(ps->audio_codec_ctx);
                if (ps->sub_codec_ctx)
                    avcodec_flush_buffers(ps->sub_codec_ctx);
                ps->sub_valid = 0;
                ps->sub_text[0] = '\0';
                log_msg("Demux: all codecs flushed");
            }
            ps->seek_request = 0;
            ps->eof = 0;

            /* Reset audio decode buffer (safe — callback is paused) */
            ps->audio_buf_size  = 0;
            ps->audio_buf_index = 0;

            /* Reset both clocks to the seek target. Without this,
             * video_clock retains the old position until the first
             * frame is decoded, causing a phantom drift spike equal
             * to the entire seek distance (e.g. 895 seconds). */
            {
                double seek_pos = (double)target / AV_TIME_BASE;
                ps->audio_clock = seek_pos;
                ps->video_clock = seek_pos;
            }

            ps->seeking = 0;
            SDL_UnlockMutex(ps->seek_mutex);

            /* Suppress frame drops until the first frame is displayed
             * post-seek. Adapts to any codec — H.264 recovers in
             * ~100ms, HEVC with long GOPs may take 5–10 seconds.
             * Cleared in main.c when a frame is actually shown. */
            ps->seek_recovering = 1;

            /* Resume audio playback */
            if (ps->audio_dev && !ps->paused)
                SDL_PauseAudioDevice(ps->audio_dev, 0);

            log_msg("Demux: seek complete");
        }

        /* ── Throttle if queues are full ── */
        if (ps->video_pq.nb_packets > PACKET_QUEUE_MAX ||
            ps->audio_pq.nb_packets > PACKET_QUEUE_MAX) {
            SDL_Delay(10);
            continue;
        }

        /* ── Read next packet ── */
        int ret = av_read_frame(ps->fmt_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF || avio_feof(ps->fmt_ctx->pb)) {
                if (!ps->eof) log_msg("Demux: reached end of file");
                ps->eof = 1;
                SDL_Delay(100);
                continue;
            }
            log_msg("ERROR: av_read_frame failed: %s", av_err2str(ret));
            break; /* real error */
        }

        /* Route packet to the correct queue */
        if (pkt->stream_index == ps->video_stream_idx) {
            pq_put(&ps->video_pq, pkt);
        } else if (pkt->stream_index == ps->audio_stream_idx) {
            pq_put(&ps->audio_pq, pkt);
        } else {
            /* Check subtitle streams */
            int routed = 0;
            for (int i = 0; i < ps->sub_count; i++) {
                if (pkt->stream_index == ps->sub_stream_indices[i]) {
                    pq_put(&ps->sub_pqs[i], pkt);
                    routed = 1;
                    break;
                }
            }
            if (!routed) {
                av_packet_unref(pkt);
            }
        }
    }

    av_packet_free(&pkt);
    log_msg("Demux thread exiting");
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════
 * Video Decode & Display
 * ═══════════════════════════════════════════════════════════════════ */

/* Decode one video frame from the packet queue.
 * Returns 1 if a frame was decoded, 0 if no packets available, -1 on error. */
int video_decode_frame(PlayerState *ps) {
    AVPacket pkt;
    int ret;

    /* If a seek is in progress, skip decode entirely.
     * The demux thread holds seek_mutex and is flushing codecs. */
    if (ps->seeking) return 0;

    /* Lock to prevent demux thread from flushing codecs mid-decode */
    if (SDL_TryLockMutex(ps->seek_mutex) != 0) {
        return 0; /* mutex held by seek — skip this frame */
    }

    for (;;) {
        /* Try to receive a decoded frame first (may have buffered frames) */
        ret = avcodec_receive_frame(ps->video_codec_ctx, ps->video_frame);
        if (ret == 0) {
            /* Got a frame — compute its PTS in seconds */
            AVStream *vs = ps->fmt_ctx->streams[ps->video_stream_idx];
            double pts = 0.0;

            if (ps->video_frame->pts != AV_NOPTS_VALUE) {
                pts = (double)ps->video_frame->pts * av_q2d(vs->time_base);
            }
            ps->video_clock = pts;
            SDL_UnlockMutex(ps->seek_mutex);
            return 1;
        }
        if (ret != AVERROR(EAGAIN)) {
            log_msg("ERROR: avcodec_receive_frame (video) failed: %s", av_err2str(ret));
            SDL_UnlockMutex(ps->seek_mutex);
            return -1; /* decoder error */
        }

        /* Need to feed more packets to the decoder */
        ret = pq_get(&ps->video_pq, &pkt, 0);
        if (ret <= 0) {
            SDL_UnlockMutex(ps->seek_mutex);
            return 0;  /* no packets available right now */
        }

        avcodec_send_packet(ps->video_codec_ctx, &pkt);
        av_packet_unref(&pkt);
    }
}

/* Compute the letterboxed display rectangle for the video.
 * Maintains aspect ratio within the current window, centering with
 * black bars on the shorter axis. Call after window resize or video open. */
void player_update_display_rect(PlayerState *ps) {
    if (ps->vid_w <= 0 || ps->vid_h <= 0 || ps->win_w <= 0 || ps->win_h <= 0) {
        ps->display_rect = (SDL_Rect){ 0, 0, ps->win_w, ps->win_h };
        return;
    }

    double video_aspect = (double)ps->vid_w / ps->vid_h;
    double win_aspect   = (double)ps->win_w / ps->win_h;

    int disp_w, disp_h;
    if (video_aspect > win_aspect) {
        /* Video is wider than window — pillarbox (bars top/bottom) */
        disp_w = ps->win_w;
        disp_h = (int)(ps->win_w / video_aspect);
    } else {
        /* Video is taller than window — letterbox (bars left/right) */
        disp_h = ps->win_h;
        disp_w = (int)(ps->win_h * video_aspect);
    }

    ps->display_rect.x = (ps->win_w - disp_w) / 2;
    ps->display_rect.y = (ps->win_h - disp_h) / 2;
    ps->display_rect.w = disp_w;
    ps->display_rect.h = disp_h;
}

/* Display the current video frame: scale → upload to texture → render.
 *
 * A/V sync logic:
 *   We compare the video frame's PTS to the audio clock.
 *   If the video is early, we delay. If late, we skip the delay.
 *   This keeps video in sync with audio without audio glitches.
 */
void video_display(PlayerState *ps) {
    if (!ps->texture || !ps->video_frame || !ps->video_frame->data[0]) return;
    if (ps->seeking) return;

    /* ── YUV420P passthrough optimization ──
     *
     * When the source is already YUV420P (the SDL texture format), skip
     * sws_scale entirely and upload the decoded frame directly. This
     * avoids an expensive Lanczos + error-diffusion pass that would
     * produce byte-identical output anyway (same format, same size).
     *
     * For any other pixel format (10-bit, 4:2:2, RGB, etc.), we run
     * the full sws_scale pipeline with Lanczos + error-diffusion
     * dithering for reference-quality conversion. */
    if (ps->video_codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P) {
        /* Direct upload — no conversion needed */
        SDL_UpdateYUVTexture(ps->texture, NULL,
            ps->video_frame->data[0], ps->video_frame->linesize[0],
            ps->video_frame->data[1], ps->video_frame->linesize[1],
            ps->video_frame->data[2], ps->video_frame->linesize[2]);
    } else {
        /* Format conversion required — full quality pipeline */
        sws_scale(ps->sws_ctx,
            (const uint8_t *const *)ps->video_frame->data,
            ps->video_frame->linesize,
            0, ps->vid_h,
            ps->rgb_frame->data,
            ps->rgb_frame->linesize);

        SDL_UpdateYUVTexture(ps->texture, NULL,
            ps->rgb_frame->data[0], ps->rgb_frame->linesize[0],
            ps->rgb_frame->data[1], ps->rgb_frame->linesize[1],
            ps->rgb_frame->data[2], ps->rgb_frame->linesize[2]);
    }

    /* ── Render with correct aspect ratio ── */
    SDL_SetRenderDrawColor(ps->renderer, 0, 0, 0, 255);
    SDL_RenderClear(ps->renderer);
    player_update_display_rect(ps);
    SDL_RenderCopy(ps->renderer, ps->texture, NULL, &ps->display_rect);
    /* Note: overlays are drawn on top in main.c before RenderPresent */
}


/* ═══════════════════════════════════════════════════════════════════
 * Seeking
 * ═══════════════════════════════════════════════════════════════════ */

/* Seek by `incr` seconds relative to current position. */
void player_seek(PlayerState *ps, double incr) {
    if (!ps->playing) return;

    double pos = ps->video_clock + incr;
    if (pos < 0.0) pos = 0.0;

    ps->seek_target  = (int64_t)(pos * AV_TIME_BASE);
    ps->seek_flags   = (incr < 0) ? AVSEEK_FLAG_BACKWARD : 0;
    ps->seek_request = 1;

    /* Reset video timing after seek */
    ps->frame_timer      = get_time_sec();
    ps->frame_last_delay = 0.04;
}


/* ═══════════════════════════════════════════════════════════════════
 * Media Info / Debug
 * ═══════════════════════════════════════════════════════════════════ */

void player_build_media_info(PlayerState *ps) {
    if (!ps->fmt_ctx) return;

    char *buf = ps->media_info;
    int   sz  = sizeof(ps->media_info);
    int   off = 0;

    off += snprintf(buf + off, sz - off, "=== MEDIA INFO ===\n");
    off += snprintf(buf + off, sz - off, "File: %s\n", ps->filepath);
    off += snprintf(buf + off, sz - off, "Format: %s (%s)\n",
        ps->fmt_ctx->iformat->name, ps->fmt_ctx->iformat->long_name);

    double duration = (ps->fmt_ctx->duration != AV_NOPTS_VALUE)
        ? (double)ps->fmt_ctx->duration / AV_TIME_BASE : 0.0;
    int hrs = (int)duration / 3600;
    int min = ((int)duration % 3600) / 60;
    int sec = (int)duration % 60;
    off += snprintf(buf + off, sz - off, "Duration: %02d:%02d:%02d\n", hrs, min, sec);

    if (ps->fmt_ctx->bit_rate > 0) {
        off += snprintf(buf + off, sz - off, "Bitrate: %"PRId64" kb/s\n",
            ps->fmt_ctx->bit_rate / 1000);
    }

    /* Video stream info */
    if (ps->video_stream_idx >= 0) {
        AVStream *vs = ps->fmt_ctx->streams[ps->video_stream_idx];
        AVCodecParameters *par = vs->codecpar;
        off += snprintf(buf + off, sz - off, "\n--- Video ---\n");
        off += snprintf(buf + off, sz - off, "Codec: %s\n",
            avcodec_get_name(par->codec_id));
        off += snprintf(buf + off, sz - off, "Resolution: %dx%d\n",
            par->width, par->height);
        off += snprintf(buf + off, sz - off, "Pixel Format: %s\n",
            av_get_pix_fmt_name(par->format));

        if (vs->avg_frame_rate.den > 0) {
            off += snprintf(buf + off, sz - off, "Frame Rate: %.3f fps\n",
                av_q2d(vs->avg_frame_rate));
        }
        if (vs->r_frame_rate.den > 0) {
            off += snprintf(buf + off, sz - off, "Real Frame Rate: %.3f fps\n",
                av_q2d(vs->r_frame_rate));
        }
        if (par->bit_rate > 0) {
            off += snprintf(buf + off, sz - off, "Video Bitrate: %"PRId64" kb/s\n",
                par->bit_rate / 1000);
        }

        /* Color info */
        off += snprintf(buf + off, sz - off, "Color Space: %s\n",
            av_color_space_name(par->color_space));
        off += snprintf(buf + off, sz - off, "Color Range: %s\n",
            av_color_range_name(par->color_range));
        off += snprintf(buf + off, sz - off, "Color Primaries: %s\n",
            av_color_primaries_name(par->color_primaries));
        off += snprintf(buf + off, sz - off, "Color TRC: %s\n",
            av_color_transfer_name(par->color_trc));
    }

    /* Audio stream info */
    if (ps->audio_stream_idx >= 0) {
        AVStream *as = ps->fmt_ctx->streams[ps->audio_stream_idx];
        AVCodecParameters *par = as->codecpar;
        off += snprintf(buf + off, sz - off, "\n--- Audio ---\n");
        off += snprintf(buf + off, sz - off, "Codec: %s\n",
            avcodec_get_name(par->codec_id));
        off += snprintf(buf + off, sz - off, "Sample Rate: %d Hz\n",
            par->sample_rate);
        off += snprintf(buf + off, sz - off, "Channels: %d\n",
            par->ch_layout.nb_channels);

        char ch_layout_str[128];
        av_channel_layout_describe(&par->ch_layout, ch_layout_str, sizeof(ch_layout_str));
        off += snprintf(buf + off, sz - off, "Channel Layout: %s\n", ch_layout_str);

        off += snprintf(buf + off, sz - off, "Sample Format: %s\n",
            av_get_sample_fmt_name(par->format));
        if (par->bit_rate > 0) {
            off += snprintf(buf + off, sz - off, "Audio Bitrate: %"PRId64" kb/s\n",
                par->bit_rate / 1000);
        }
    }

    /* Metadata */
    AVDictionaryEntry *tag = NULL;
    int first = 1;
    while ((tag = av_dict_get(ps->fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        if (first) {
            off += snprintf(buf + off, sz - off, "\n--- Metadata ---\n");
            first = 0;
        }
        off += snprintf(buf + off, sz - off, "%s: %s\n", tag->key, tag->value);
    }
}

void player_build_debug_info(PlayerState *ps) {
    if (!ps->playing) return;

    char *buf = ps->debug_info;
    int   sz  = sizeof(ps->debug_info);
    int   off = 0;

    off += snprintf(buf + off, sz - off, "=== DEBUG ===\n");
    off += snprintf(buf + off, sz - off, "Video Clock: %.3f s\n", ps->video_clock);
    off += snprintf(buf + off, sz - off, "Audio Clock: %.3f s\n", ps->audio_clock);
    off += snprintf(buf + off, sz - off, "A/V Diff:    %.3f ms\n",
        (ps->video_clock - ps->audio_clock) * 1000.0);
    off += snprintf(buf + off, sz - off, "Video Queue: %d pkts (%d KB)\n",
        ps->video_pq.nb_packets, ps->video_pq.size / 1024);
    off += snprintf(buf + off, sz - off, "Audio Queue: %d pkts (%d KB)\n",
        ps->audio_pq.nb_packets, ps->audio_pq.size / 1024);
    off += snprintf(buf + off, sz - off, "Volume:      %.0f%%\n", ps->volume * 100.0);
    off += snprintf(buf + off, sz - off, "Paused:      %s\n", ps->paused ? "yes" : "no");
    off += snprintf(buf + off, sz - off, "EOF:         %s\n", ps->eof ? "yes" : "no");

    if (ps->video_codec_ctx) {
        off += snprintf(buf + off, sz - off, "Decoder Threads: %d\n",
            ps->video_codec_ctx->thread_count);
    }
    if (ps->video_codec_ctx) {
        if (ps->video_codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P) {
            off += snprintf(buf + off, sz - off, "SWS: passthrough (no conversion)\n");
        } else {
            off += snprintf(buf + off, sz - off,
                "SWS: format-only (point + ED dither)\n");
        }
    }

    /* Audio track info */
    if (ps->aud_count > 1) {
        off += snprintf(buf + off, sz - off, "Audio Track: %s (%d/%d)\n",
            ps->aud_stream_names[ps->aud_selection],
            ps->aud_selection + 1, ps->aud_count);
    }

    /* Subtitle info */
    if (ps->sub_count > 0) {
        if (ps->sub_selection == 0) {
            off += snprintf(buf + off, sz - off, "Subtitles: off (%d available)\n",
                ps->sub_count);
        } else {
            off += snprintf(buf + off, sz - off, "Subtitles: %s\n",
                ps->sub_stream_names[ps->sub_selection - 1]);
        }
    } else {
        off += snprintf(buf + off, sz - off, "Subtitles: none found\n");
    }

    double duration = (ps->fmt_ctx && ps->fmt_ctx->duration != AV_NOPTS_VALUE)
        ? (double)ps->fmt_ctx->duration / AV_TIME_BASE : 0.0;
    double pos = ps->video_clock;
    off += snprintf(buf + off, sz - off, "Position:    %.1f / %.1f s\n", pos, duration);

    /* Playback diagnostics */
    off += snprintf(buf + off, sz - off, "\n--- Diagnostics ---\n");
    off += snprintf(buf + off, sz - off, "Decoded:     %d\n", ps->diag_frames_decoded);
    off += snprintf(buf + off, sz - off, "Displayed:   %d\n", ps->diag_frames_displayed);
    off += snprintf(buf + off, sz - off, "Dropped:     %d\n", ps->diag_frames_dropped);
    off += snprintf(buf + off, sz - off, "Multi-ticks: %d\n", ps->diag_multi_decodes);
    off += snprintf(buf + off, sz - off, "Stall snaps: %d\n", ps->diag_timer_snaps);
    off += snprintf(buf + off, sz - off, "Peak drift:  %.1f ms\n",
        ps->diag_max_av_drift * 1000.0);
}
