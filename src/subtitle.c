/*
 * DSVP — Dead Simple Video Player
 * subtitle.c — Subtitle stream detection, decoding, and rendering
 *
 * Handles:
 *   - Cataloging available subtitle tracks in a container
 *   - Opening/closing subtitle codecs
 *   - Decoding text subtitles (SRT, ASS/SSA)
 *   - Rendering with SDL_ttf: golden yellow (#FFDF00) + black outline
 *   - Track cycling with 'S' key (including "Off" option)
 */

#include "dsvp.h"
#include <zlib.h>

/* ── Font state (module-level) ─────────────────────────────────────── */

static TTF_Font *sub_font         = NULL;
static TTF_Font *sub_font_outline = NULL;
static TTF_Font *sub_font_cjk         = NULL;
static TTF_Font *sub_font_cjk_outline = NULL;
static int       font_loaded      = 0;

/* Golden Yellow subtitle color and black outline */
static const SDL_Color COLOR_SUB     = { 255, 223, 0, 255 };   /* #FFDF00 */
static const SDL_Color COLOR_OUTLINE = { 0,   0,   0, 255 };

/* ── Font discovery ────────────────────────────────────────────────── */

static const char *find_system_font(void) {
    static const char *candidates[] = {
        "/usr/share/fonts/truetype/msttcorefonts/Verdana.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
        NULL
    };

    for (int i = 0; candidates[i]; i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            return candidates[i];
        }
    }
    return NULL;
}

static const char *find_cjk_font(void) {
    static const char *candidates[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/OTF/NotoSansCJK-Regular.ttc",
        NULL
    };

    for (int i = 0; candidates[i]; i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            return candidates[i];
        }
    }
    return NULL;
}


/* ═══════════════════════════════════════════════════════════════════
 * Font Init / Close
 * ═══════════════════════════════════════════════════════════════════ */

int sub_init_font(void) {
    if (font_loaded) return 0;

    if (!TTF_Init()) {
        log_msg("ERROR: TTF_Init failed: %s", SDL_GetError());
        return -1;
    }

    const char *font_path = find_system_font();
    if (!font_path) {
        log_msg("ERROR: No suitable TTF font found on system");
        log_msg("  Windows: needs Verdana or Arial in C:\\Windows\\Fonts\\");
        log_msg("  Linux: sudo apt install fonts-dejavu-core");
        TTF_Quit();
        return -1;
    }

    int font_size = 32;

    sub_font = TTF_OpenFont(font_path, font_size);
    if (!sub_font) {
        log_msg("ERROR: Cannot open font %s: %s", font_path, SDL_GetError());
        TTF_Quit();
        return -1;
    }

    sub_font_outline = TTF_OpenFont(font_path, font_size);
    if (sub_font_outline) {
        TTF_SetFontOutline(sub_font_outline, 2);
    }

    TTF_SetFontHinting(sub_font, TTF_HINTING_LIGHT);
    if (sub_font_outline)
        TTF_SetFontHinting(sub_font_outline, TTF_HINTING_LIGHT);

    /* Try to attach CJK fallback font for Chinese/Japanese/Korean glyphs */
    const char *cjk_path = find_cjk_font();
    if (cjk_path) {
        sub_font_cjk = TTF_OpenFont(cjk_path, font_size);
        if (sub_font_cjk) {
            TTF_SetFontHinting(sub_font_cjk, TTF_HINTING_LIGHT);
            TTF_AddFallbackFont(sub_font, sub_font_cjk);

            sub_font_cjk_outline = TTF_OpenFont(cjk_path, font_size);
            if (sub_font_cjk_outline) {
                TTF_SetFontOutline(sub_font_cjk_outline, 2);
                TTF_SetFontHinting(sub_font_cjk_outline, TTF_HINTING_LIGHT);
                TTF_AddFallbackFont(sub_font_outline, sub_font_cjk_outline);
            }
            log_msg("CJK fallback font loaded: %s", cjk_path);
        }
    }

    font_loaded = 1;
    log_msg("Subtitle font loaded: %s (%dpt)", font_path, font_size);
    return 0;
}

void sub_close_font(void) {
    if (sub_font_cjk)         { TTF_CloseFont(sub_font_cjk);         sub_font_cjk = NULL; }
    if (sub_font_cjk_outline) { TTF_CloseFont(sub_font_cjk_outline); sub_font_cjk_outline = NULL; }
    if (sub_font)         { TTF_CloseFont(sub_font);         sub_font = NULL; }
    if (sub_font_outline) { TTF_CloseFont(sub_font_outline); sub_font_outline = NULL; }
    if (font_loaded)      { TTF_Quit(); font_loaded = 0; }
}

/* Font accessors for overlay.c (GPU-composited subtitle rendering) */
TTF_Font *sub_get_font(void)         { return sub_font; }
TTF_Font *sub_get_outline_font(void) { return sub_font_outline; }

/* Free any active bitmap subtitle data */
static void sub_clear_bitmaps(PlayerState *ps) {
    for (int i = 0; i < ps->sub_bitmap_count; i++) {
        if (ps->sub_bitmap_data[i]) {
            av_free(ps->sub_bitmap_data[i]);
            ps->sub_bitmap_data[i] = NULL;
        }
    }
    ps->sub_bitmap_count = 0;
}


/* ═══════════════════════════════════════════════════════════════════
 * Stream Discovery
 * ═══════════════════════════════════════════════════════════════════ */

void sub_find_streams(PlayerState *ps) {
    ps->sub_count      = 0;
    ps->sub_selection  = 0;
    ps->sub_active_idx = -1;

    for (unsigned i = 0; i < ps->fmt_ctx->nb_streams && ps->sub_count < MAX_SUB_STREAMS; i++) {
        AVStream *st = ps->fmt_ctx->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;

        enum AVCodecID cid = st->codecpar->codec_id;

        /* Check if this is a supported text subtitle */
        int is_text = (cid == AV_CODEC_ID_SRT ||
                       cid == AV_CODEC_ID_SUBRIP ||
                       cid == AV_CODEC_ID_ASS ||
                       cid == AV_CODEC_ID_SSA ||
                       cid == AV_CODEC_ID_MOV_TEXT ||
                       cid == AV_CODEC_ID_TEXT ||
                       cid == AV_CODEC_ID_WEBVTT);

        /* Check if this is a supported bitmap subtitle */
        int is_bitmap = (cid == AV_CODEC_ID_HDMV_PGS_SUBTITLE ||
                         cid == AV_CODEC_ID_DVD_SUBTITLE ||
                         cid == AV_CODEC_ID_DVB_SUBTITLE);

        if (!is_text && !is_bitmap) {
            log_msg("Subtitle stream %d: skipping unsupported codec %s", i,
                avcodec_get_name(cid));
            continue;
        }

        int idx = ps->sub_count;
        ps->sub_stream_indices[idx] = (int)i;

        const AVDictionaryEntry *lang  = av_dict_get(st->metadata, "language", NULL, 0);
        const AVDictionaryEntry *title = av_dict_get(st->metadata, "title", NULL, 0);

        if (title && lang) {
            snprintf(ps->sub_stream_names[idx], sizeof(ps->sub_stream_names[idx]),
                "%s (%s)", title->value, lang->value);
        } else if (lang) {
            snprintf(ps->sub_stream_names[idx], sizeof(ps->sub_stream_names[idx]),
                "%s", lang->value);
        } else if (title) {
            snprintf(ps->sub_stream_names[idx], sizeof(ps->sub_stream_names[idx]),
                "%s", title->value);
        } else {
            snprintf(ps->sub_stream_names[idx], sizeof(ps->sub_stream_names[idx]),
                "Track %d", idx + 1);
        }

        log_msg("Subtitle stream %d: [%d] %s (%s)", idx, (int)i,
            ps->sub_stream_names[idx], avcodec_get_name(cid));
        ps->sub_count++;
    }

    log_msg("Found %d text subtitle stream(s)", ps->sub_count);
}


/* ═══════════════════════════════════════════════════════════════════
 * Codec Open / Close
 * ═══════════════════════════════════════════════════════════════════ */

int sub_open_codec(PlayerState *ps, int stream_idx) {
    sub_close_codec(ps);

    if (stream_idx < 0) return 0;

    AVStream *st = ps->fmt_ctx->streams[stream_idx];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        log_msg("ERROR: No decoder for subtitle codec %s",
            avcodec_get_name(st->codecpar->codec_id));
        return -1;
    }

    ps->sub_codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ps->sub_codec_ctx, st->codecpar);

    int ret = avcodec_open2(ps->sub_codec_ctx, codec, NULL);
    if (ret < 0) {
        log_msg("ERROR: Cannot open subtitle codec: %s", av_err2str(ret));
        avcodec_free_context(&ps->sub_codec_ctx);
        return -1;
    }

    ps->sub_active_idx = stream_idx;
    log_msg("Subtitle codec opened: %s (stream %d), canvas %dx%d",
        codec->name, stream_idx,
        ps->sub_codec_ctx->width, ps->sub_codec_ctx->height);

    /* Diagnostic: log codec extradata for PGS format analysis */
    if (ps->sub_codec_ctx->extradata_size > 0) {
        char hex[128] = {0};
        int dump_len = ps->sub_codec_ctx->extradata_size < 20
                      ? ps->sub_codec_ctx->extradata_size : 20;
        for (int i = 0; i < dump_len; i++)
            snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02X ",
                     ps->sub_codec_ctx->extradata[i]);
        log_msg("Subtitle extradata (%d bytes): %s",
                ps->sub_codec_ctx->extradata_size, hex);
    } else {
        log_msg("Subtitle extradata: none");
    }
    return 0;
}

void sub_close_codec(PlayerState *ps) {
    if (ps->sub_codec_ctx) {
        avcodec_free_context(&ps->sub_codec_ctx);
    }
    ps->sub_active_idx = -1;
    ps->sub_valid = 0;
    ps->sub_is_bitmap = 0;
    ps->sub_text[0] = '\0';
    sub_clear_bitmaps(ps);
}


/* ═══════════════════════════════════════════════════════════════════
 * Track Cycling
 * ═══════════════════════════════════════════════════════════════════
 *
 * No seeking is performed — subtitles appear from the next event
 * in the container. This is standard behavior (VLC, mpv do the same).
 */

void sub_cycle(PlayerState *ps) {
    if (ps->sub_count == 0) {
        snprintf(ps->sub_osd, sizeof(ps->sub_osd), "No subtitles available");
        ps->sub_osd_until = get_time_sec() + 2.0;
        return;
    }

    /* Cycle: 0 (off) → 1 → 2 → ... → N → 0 (off) */
    ps->sub_selection = (ps->sub_selection + 1) % (ps->sub_count + 1);

    if (ps->sub_selection == 0) {
        sub_close_codec(ps);
        snprintf(ps->sub_osd, sizeof(ps->sub_osd), "Subtitles: Off");
        log_msg("Subtitles disabled");
    } else {
        int sel = ps->sub_selection - 1;
        int stream_idx = ps->sub_stream_indices[sel];

        sub_open_codec(ps, stream_idx);

        /* Clear current display so new track takes effect immediately */
        ps->sub_valid = 0;
        ps->sub_is_bitmap = 0;
        ps->sub_text[0] = '\0';
        sub_clear_bitmaps(ps);

        snprintf(ps->sub_osd, sizeof(ps->sub_osd), "Subtitles: %s",
            ps->sub_stream_names[sel]);
        log_msg("Subtitles: %s (stream %d)",
            ps->sub_stream_names[sel], stream_idx);
    }

    ps->sub_osd_until = get_time_sec() + 2.0;
}


/* ═══════════════════════════════════════════════════════════════════
 * ASS Markup Stripping
 * ═══════════════════════════════════════════════════════════════════ */

static void strip_ass_markup(const char *ass_event, char *out, int out_size) {
    const char *p = ass_event;
    int commas = 0;
    while (*p && commas < 8) {
        if (*p == ',') commas++;
        p++;
    }

    if (commas < 8) p = ass_event;

    int o = 0;
    while (*p && o < out_size - 1) {
        if (*p == '{') {
            while (*p && *p != '}') p++;
            if (*p == '}') p++;
            continue;
        }
        if (*p == '\\' && (*(p + 1) == 'N' || *(p + 1) == 'n')) {
            if (o < out_size - 1) out[o++] = '\n';
            p += 2;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';

    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\n' || out[o - 1] == '\r')) {
        out[--o] = '\0';
    }
    char *start = out;
    while (*start == ' ' || *start == '\n' || *start == '\r') start++;
    if (start != out) memmove(out, start, strlen(start) + 1);
}


/* ═══════════════════════════════════════════════════════════════════
 * PGS Zlib Decompression
 * ═══════════════════════════════════════════════════════════════════
 *
 * Some MKV muxers apply ContentCompression (zlib) to PGS subtitle
 * tracks. FFmpeg's matroska demuxer doesn't always decompress these
 * transparently, leaving raw zlib data in the AVPacket. Detect via
 * the 0x78 zlib magic byte and decompress before decoding.
 *
 * Returns: newly allocated decompressed buffer (caller must av_free),
 *          or NULL if not compressed / decompression failed.
 *          *out_size is set to the decompressed length on success.
 */
static uint8_t *pgs_try_decompress(const uint8_t *data, int size, int *out_size) {
    if (size < 2 || data[0] != 0x78) return NULL;
    /* 0x78 followed by 0x01/0x5E/0x9C/0xDA = valid zlib header */
    uint8_t flg = data[1];
    if (flg != 0x01 && flg != 0x5E && flg != 0x9C && flg != 0xDA)
        return NULL;

    /* Start with 10x buffer, retry with larger if needed */
    uLongf dst_len = (uLongf)size * 10;
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t *dst = av_malloc(dst_len);
        if (!dst) return NULL;

        int zret = uncompress(dst, &dst_len, data, (uLong)size);
        if (zret == Z_OK) {
            *out_size = (int)dst_len;
            return dst;
        }
        av_free(dst);
        if (zret == Z_BUF_ERROR) {
            dst_len *= 4;  /* buffer too small, try larger */
            continue;
        }
        /* Z_DATA_ERROR or other — not valid zlib */
        return NULL;
    }
    return NULL;
}


/* ═══════════════════════════════════════════════════════════════════
 * Subtitle Decoding
 * ═══════════════════════════════════════════════════════════════════
 *
 * Called from the main thread each frame. Pops ONE subtitle at a
 * time from the queue and holds it until its display time expires.
 * Skips subtitles whose end time has already passed.
 */

void sub_decode_pending(PlayerState *ps) {
    if (ps->sub_active_idx < 0 || !ps->sub_codec_ctx) return;
    if (ps->sub_selection <= 0 || ps->sub_selection > ps->sub_count) return;

    /* Get the queue for the active subtitle stream */
    int queue_idx = ps->sub_selection - 1;
    PacketQueue *spq = &ps->sub_pqs[queue_idx];

    double now = ps->audio_clock_sync;
    if (ps->audio_stream_idx < 0) now = ps->video_clock;

    /* If current subtitle is still valid and on-screen, keep it.
     * Exception: bitmap subs currently DISPLAYING need to drain the queue
     * for "clear" packets (0 rects) that signal when to hide.
     * Once the clear is found (end_pts updated from the 30s cap), stop draining. */
    if (ps->sub_valid && now <= ps->sub_end_pts) {
        if (!ps->sub_is_bitmap) return;
        if (now < ps->sub_start_pts) return;  /* not showing yet, don't drain */
        /* If end_pts was updated from the 30s cap, clear was already found */
        if (ps->sub_end_pts - ps->sub_start_pts < 29.0) return;
        /* Bitmap currently displayed, clear not yet found — drain for it */
    }

    /* Current subtitle expired or bitmap needs clear-packet drain */
    int draining_for_clear = (ps->sub_is_bitmap && ps->sub_valid
                              && now >= ps->sub_start_pts && now <= ps->sub_end_pts);
    if (!draining_for_clear) {
        ps->sub_valid = 0;
        sub_clear_bitmaps(ps);
    }

    AVPacket pkt;
    int pgs_packets_this_drain = 0;
    double last_pgs_pts = 0.0;
    while (pq_get(spq, &pkt, 0) > 0) {
        AVSubtitle sub;
        int got_sub = 0;

        /* PGS zlib fix: some MKV muxers apply ContentCompression (zlib)
         * to PGS tracks but FFmpeg's demuxer doesn't always decompress.
         * Detect 0x78 zlib magic and decompress before decoding. */
        uint8_t *decompressed = NULL;
        int decomp_size = 0;
        AVPacket decode_pkt = pkt;
        if (ps->sub_codec_ctx->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE) {
            decompressed = pgs_try_decompress(pkt.data, pkt.size, &decomp_size);
            if (decompressed) {
                decode_pkt.data = decompressed;
                decode_pkt.size = decomp_size;
            }
        }

        int ret = avcodec_decode_subtitle2(ps->sub_codec_ctx, &sub, &got_sub, &decode_pkt);

        if (ps->sub_codec_ctx->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE) {
            log_msg("Sub: MAIN-LOOP pkt_size=%d%s got_sub=%d rects=%u ret=%d seg=0x%02X",
                    pkt.size, decompressed ? " (zlib)" : "",
                    got_sub, got_sub ? sub.num_rects : 0, ret,
                    decode_pkt.size > 0 ? decode_pkt.data[0] : 0);
        } else {
            log_msg("Sub: MAIN-LOOP pkt_size=%d got_sub=%d rects=%u ret=%d",
                    pkt.size, got_sub, got_sub ? sub.num_rects : 0, ret);
        }

        av_free(decompressed);  /* NULL-safe */

        /* Track PGS packets fed this drain cycle */
        if (ps->sub_codec_ctx->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE) {
            pgs_packets_this_drain++;
            AVStream *pgs_st = ps->fmt_ctx->streams[ps->sub_active_idx];
            if (pkt.pts != AV_NOPTS_VALUE)
                last_pgs_pts = (double)pkt.pts * av_q2d(pgs_st->time_base);
        }
        if (ret < 0) {
            log_msg("Sub: decode error ret=%d", ret);
            av_packet_unref(&pkt);
            continue;
        }
        if (!got_sub) {
            /* Normal for PGS: decoder accumulates segments (PCS, WDS,
             * PDS, ODS) and only outputs on DISPLAY_SEGMENT (0x80). */
            av_packet_unref(&pkt);
            continue;
        }

        /* Compute display timing */
        AVStream *st = ps->fmt_ctx->streams[ps->sub_active_idx];
        double pkt_pts = 0.0;
        if (pkt.pts != AV_NOPTS_VALUE) {
            pkt_pts = (double)pkt.pts * av_q2d(st->time_base);
        }

        double start = pkt_pts + (double)sub.start_display_time / 1000.0;
        double end   = pkt_pts + (double)sub.end_display_time / 1000.0;

        /* SRT/subrip decoded by FFmpeg often sets end_display_time=0.
         * The actual duration is in pkt.duration in stream time_base. */
        if (sub.end_display_time == 0 && pkt.duration > 0) {
            end = pkt_pts + (double)pkt.duration * av_q2d(st->time_base);
        } else if (sub.end_display_time == 0) {
            end = start + 3.0;  /* last resort fallback */
        }

        /* PGS/DVB: end_display_time is often UINT32_MAX (duration unknown
         * until the clear packet arrives). Cap to 30s as a safety net —
         * the 0-rect clear packet will expire it earlier. */
        if (end - start > 30.0) {
            end = start + 30.0;
        }

        /* If we're only draining for a clear packet, handle it here
         * without touching the currently-displaying bitmap data. */
        if (draining_for_clear) {
            if (sub.num_rects == 0) {
                /* Found the clear signal */
                log_msg("Sub: clear signal (0 rects, pts=%.1f)", pkt_pts);
                if (pkt_pts > now) {
                    /* Clear is in the future — set the real end time.
                     * The sub will expire naturally via the time check. */
                    ps->sub_end_pts = pkt_pts;
                    avsubtitle_free(&sub);
                    av_packet_unref(&pkt);
                    break;
                }
                /* Clear is for now or past — expire immediately */
                ps->sub_valid = 0;
                sub_clear_bitmaps(ps);
                draining_for_clear = 0;
                avsubtitle_free(&sub);
                av_packet_unref(&pkt);
                continue;
            }
            /* Not a clear packet — skip it, keep looking */
            avsubtitle_free(&sub);
            av_packet_unref(&pkt);
            continue;
        }

        /* Extract text or bitmap data */
        char text[SUB_TEXT_SIZE] = {0};
        int got_bitmap = 0;

        /* Clear any previous bitmap textures */
        sub_clear_bitmaps(ps);

        for (unsigned i = 0; i < sub.num_rects; i++) {
            AVSubtitleRect *rect = sub.rects[i];

            if (rect->type == SUBTITLE_TEXT && rect->text) {
                snprintf(text, sizeof(text), "%s", rect->text);
                log_msg("Sub [TEXT] %.1f-%.1f: \"%.*s\"", start, end, 60, text);
            } else if (rect->type == SUBTITLE_ASS && rect->ass) {
                strip_ass_markup(rect->ass, text, sizeof(text));
                log_msg("Sub [ASS] %.1f-%.1f: \"%.*s\"", start, end, 60, text);
            } else if (rect->type == SUBTITLE_BITMAP &&
                       rect->data[0] && rect->data[1] &&
                       rect->w > 0 && rect->h > 0 &&
                       ps->sub_bitmap_count < MAX_SUB_BITMAPS) {
                /*
                 * Bitmap subtitles (PGS, VobSub, DVB):
                 *   rect->data[0] = pixel indices into palette
                 *   rect->data[1] = RGBA palette (4 bytes per entry, 0xAARRGGBB native)
                 *   rect->w/h     = dimensions
                 *   rect->x/y     = position relative to video frame
                 */
                uint32_t *palette = (uint32_t *)rect->data[1];
                int w = rect->w;
                int h = rect->h;

                /* Convert paletted pixels to RGBA */
                uint8_t *rgba = av_malloc(w * h * 4);
                if (rgba) {
                    for (int row = 0; row < h; row++) {
                        for (int col = 0; col < w; col++) {
                            uint8_t idx = rect->data[0][row * rect->linesize[0] + col];
                            uint32_t color = palette[idx];
                            int off = (row * w + col) * 4;
                            rgba[off + 0] = (color >> 16) & 0xFF;  /* R */
                            rgba[off + 1] = (color >> 8)  & 0xFF;  /* G */
                            rgba[off + 2] =  color        & 0xFF;  /* B */
                            rgba[off + 3] = (color >> 24) & 0xFF;  /* A */
                        }
                    }

                    /* Store RGBA data for GPU overlay compositing */
                    int bi = ps->sub_bitmap_count;
                    ps->sub_bitmap_data[bi] = rgba;  /* ownership transferred */
                    ps->sub_bitmap_w[bi] = w;
                    ps->sub_bitmap_h[bi] = h;
                    ps->sub_bitmap_rects[bi] = (SDL_Rect){ rect->x, rect->y, w, h };
                    ps->sub_bitmap_count++;
                    got_bitmap = 1;

                    log_msg("Sub [BITMAP] %.1f-%.1f: %dx%d at (%d,%d)",
                        start, end, w, h, rect->x, rect->y);
                }
            } else {
                log_msg("Sub: unknown rect type %d", rect->type);
            }
        }

        if (sub.num_rects == 0) {
            /* PGS/DVB: a 0-rect packet is the "clear" signal.
             * (Drain-for-clear case is handled above; this covers
             * clear packets encountered during normal scanning.) */
            log_msg("Sub: clear signal (0 rects, pts=%.1f)", pkt_pts);
            ps->sub_valid = 0;
            sub_clear_bitmaps(ps);
            avsubtitle_free(&sub);
            av_packet_unref(&pkt);
            continue;
        }

        avsubtitle_free(&sub);
        av_packet_unref(&pkt);

        if (text[0] == '\0' && !got_bitmap) continue;

        /* Skip subtitles that have already expired */
        if (end < now) {
            log_msg("Sub: skipped expired (end=%.1f < now=%.1f)", end, now);
            sub_clear_bitmaps(ps);
            continue;
        }

        /* Keep this subtitle */
        if (got_bitmap) {
            ps->sub_is_bitmap = 1;
            ps->sub_text[0] = '\0';
        } else {
            ps->sub_is_bitmap = 0;
            snprintf(ps->sub_text, sizeof(ps->sub_text), "%s", text);
        }
        ps->sub_start_pts = start;
        ps->sub_end_pts   = end;
        ps->sub_valid     = 1;
        break;  /* Show this one, leave rest in queue for later */
    }

    /* ── PGS post-drain: inject synthetic END segment ──
     * MKV muxers strip the zero-length END segment (0x80) that triggers
     * display set output in FFmpeg's PGS decoder. The decoder accumulates
     * PCS/WDS/PDS/ODS across calls but never fires without END.
     *
     * Key: only inject ONCE after draining real PGS packets — not every
     * idle frame. display_end_segment() resets presentation state, so a
     * premature END (before all segments arrive) would clear accumulated
     * data. By waiting until the queue is fully drained, all segments
     * from the current display set are loaded and END can assemble them. */
    if (pgs_packets_this_drain > 0 && !ps->sub_valid &&
        ps->sub_codec_ctx &&
        ps->sub_codec_ctx->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE) {

        static const uint8_t end_seg[] = { 0x80, 0x00, 0x00 };
        AVPacket end_pkt;
        memset(&end_pkt, 0, sizeof(end_pkt));
        end_pkt.data = (uint8_t *)end_seg;
        end_pkt.size = sizeof(end_seg);

        AVSubtitle sub;
        int got_sub = 0;
        int ret = avcodec_decode_subtitle2(ps->sub_codec_ctx, &sub, &got_sub, &end_pkt);
        log_msg("Sub: PGS-END inject after %d pkts: got_sub=%d rects=%u ret=%d last_pts=%.1f",
                pgs_packets_this_drain, got_sub, got_sub ? sub.num_rects : 0, ret, last_pgs_pts);

        if (ret >= 0 && got_sub) {
            double start = last_pgs_pts + (double)sub.start_display_time / 1000.0;
            double end   = last_pgs_pts + (double)sub.end_display_time / 1000.0;
            if (sub.end_display_time == 0) end = start + 5.0;
            if (end - start > 30.0) end = start + 30.0;

            if (sub.num_rects == 0) {
                log_msg("Sub: PGS-END clear (0 rects, pts=%.1f)", last_pgs_pts);
                ps->sub_valid = 0;
                sub_clear_bitmaps(ps);
                avsubtitle_free(&sub);
            } else {
                sub_clear_bitmaps(ps);
                int got_bitmap = 0;
                for (unsigned i = 0; i < sub.num_rects; i++) {
                    AVSubtitleRect *rect = sub.rects[i];
                    if (rect->type == SUBTITLE_BITMAP &&
                        rect->data[0] && rect->data[1] &&
                        rect->w > 0 && rect->h > 0 &&
                        ps->sub_bitmap_count < MAX_SUB_BITMAPS) {
                        uint32_t *palette = (uint32_t *)rect->data[1];
                        int w = rect->w, h = rect->h;
                        uint8_t *rgba = av_malloc(w * h * 4);
                        if (rgba) {
                            for (int row = 0; row < h; row++) {
                                for (int col = 0; col < w; col++) {
                                    uint8_t idx = rect->data[0][row * rect->linesize[0] + col];
                                    uint32_t color = palette[idx];
                                    int off = (row * w + col) * 4;
                                    rgba[off + 0] = (color >> 16) & 0xFF;
                                    rgba[off + 1] = (color >> 8)  & 0xFF;
                                    rgba[off + 2] =  color        & 0xFF;
                                    rgba[off + 3] = (color >> 24) & 0xFF;
                                }
                            }
                            int bi = ps->sub_bitmap_count;
                            ps->sub_bitmap_data[bi] = rgba;
                            ps->sub_bitmap_w[bi] = w;
                            ps->sub_bitmap_h[bi] = h;
                            ps->sub_bitmap_rects[bi] = (SDL_Rect){ rect->x, rect->y, w, h };
                            ps->sub_bitmap_count++;
                            got_bitmap = 1;
                            log_msg("Sub [PGS BITMAP] %.1f-%.1f: %dx%d at (%d,%d)",
                                    start, end, w, h, rect->x, rect->y);
                        }
                    }
                }
                avsubtitle_free(&sub);

                if (got_bitmap) {
                    ps->sub_is_bitmap = 1;
                    ps->sub_text[0] = '\0';
                    ps->sub_start_pts = start;
                    ps->sub_end_pts   = end;
                    ps->sub_valid     = 1;
                }
            }
        }
    }
}


/* ═══════════════════════════════════════════════════════════════════
 * Subtitle Rendering
 * ═══════════════════════════════════════════════════════════════════ */

static void render_text_outlined(SDL_Renderer *renderer, TTF_Font *font,
                                  TTF_Font *outline_font, const char *text,
                                  int x, int y, SDL_Color fg, SDL_Color outline_col) {
    if (!text || !text[0]) return;

    if (outline_font) {
        SDL_Surface *outline_surf = TTF_RenderText_Blended(outline_font, text, 0, outline_col);
        if (outline_surf) {
            SDL_Texture *outline_tex = SDL_CreateTextureFromSurface(renderer, outline_surf);
            if (outline_tex) {
                int outline_offset = TTF_GetFontOutline(outline_font);
                SDL_Rect dst = { x - outline_offset, y - outline_offset,
                                 outline_surf->w, outline_surf->h };
                {SDL_FRect _fdst = rect_to_frect(&dst); SDL_RenderTexture(renderer, outline_tex, NULL, &_fdst);}
                SDL_DestroyTexture(outline_tex);
            }
            SDL_DestroySurface(outline_surf);
        }
    }

    SDL_Surface *text_surf = TTF_RenderText_Blended(font, text, 0, fg);
    if (text_surf) {
        SDL_Texture *text_tex = SDL_CreateTextureFromSurface(renderer, text_surf);
        if (text_tex) {
            SDL_Rect dst = { x, y, text_surf->w, text_surf->h };
            {SDL_FRect _fdst = rect_to_frect(&dst); SDL_RenderTexture(renderer, text_tex, NULL, &_fdst);}
            SDL_DestroyTexture(text_tex);
        }
        SDL_DestroySurface(text_surf);
    }
}

void sub_render(PlayerState *ps, SDL_Renderer *renderer, int win_w, int win_h) {

    /* ── Render active subtitle ── */
    if (ps->sub_valid) {
        double now = ps->audio_clock_sync;
        if (ps->audio_stream_idx < 0) now = ps->video_clock;

        if (now >= ps->sub_start_pts && now <= ps->sub_end_pts) {
            if (ps->sub_is_bitmap && ps->sub_bitmap_count > 0) {
                /* Bitmap subtitles now rendered via overlay.c GPU path.
                 * This legacy sub_render() is unused. */
                (void)renderer; (void)win_w; (void)win_h;
            } else if (!ps->sub_is_bitmap && ps->sub_text[0]) {
                /* ── Text subtitle: render with SDL_ttf ── */
                if (font_loaded) {
                    char buf[SUB_TEXT_SIZE];
                    snprintf(buf, sizeof(buf), "%s", ps->sub_text);

                    char *lines[64];
                    int nlines = 0;
                    char *tok = strtok(buf, "\n");
                    while (tok && nlines < 64) {
                        if (tok[0] != '\0') {
                            lines[nlines++] = tok;
                        }
                        tok = strtok(NULL, "\n");
                    }

                    int font_size = win_h / 24;
                    if (font_size < 14) font_size = 14;
                    if (font_size > 54) font_size = 54;

                    TTF_SetFontSize(sub_font, font_size);
                    if (sub_font_outline)
                        TTF_SetFontSize(sub_font_outline, font_size);

                    int line_height = TTF_GetFontLineSkip(sub_font);
                    int total_height = nlines * line_height;
                    int y_base = win_h - 60 - total_height;

                    for (int i = 0; i < nlines; i++) {
                        int tw = 0, th = 0;
                        TTF_GetStringSize(sub_font, lines[i], 0, &tw, &th);
                        int x = (win_w - tw) / 2;
                        int y = y_base + i * line_height;

                        render_text_outlined(renderer, sub_font, sub_font_outline,
                            lines[i], x, y, COLOR_SUB, COLOR_OUTLINE);
                    }
                }
            }
        } else if (now > ps->sub_end_pts) {
            ps->sub_valid = 0;
            sub_clear_bitmaps(ps);
        }
    }

    /* ── Render track-change OSD (subtitle or audio) ── */
    const char *osd_text = NULL;
    double osd_until = 0;

    /* Audio OSD takes priority if both are active */
    if (ps->aud_osd[0] && get_time_sec() < ps->aud_osd_until) {
        osd_text = ps->aud_osd;
        osd_until = ps->aud_osd_until;
    } else if (ps->aud_osd[0]) {
        ps->aud_osd[0] = '\0';
    }

    if (ps->sub_osd[0] && get_time_sec() < ps->sub_osd_until) {
        /* If no audio OSD, show subtitle OSD; otherwise audio wins */
        if (!osd_text) {
            osd_text = ps->sub_osd;
            osd_until = ps->sub_osd_until;
        }
    } else if (ps->sub_osd[0]) {
        ps->sub_osd[0] = '\0';
    }

    if (osd_text && get_time_sec() < osd_until && font_loaded) {
        int font_size = win_h / 40;
        if (font_size < 12) font_size = 12;
        if (font_size > 32) font_size = 32;

        TTF_SetFontSize(sub_font, font_size);
        if (sub_font_outline)
            TTF_SetFontSize(sub_font_outline, font_size);

        int tw = 0, th = 0;
        TTF_GetStringSize(sub_font, osd_text, 0, &tw, &th);
        int x = (win_w - tw) / 2;
        int y = 30;

        render_text_outlined(renderer, sub_font, sub_font_outline,
            osd_text, x, y, COLOR_SUB, COLOR_OUTLINE);
    }
}
