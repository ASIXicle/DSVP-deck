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
#include <libavcodec/codec_desc.h>   /* avcodec_descriptor_get — text-vs-bitmap track dispatch */
#include <limits.h>
#include <zlib.h>

/* ── Font state (module-level) ─────────────────────────────────────── */

static TTF_Font *sub_font         = NULL;
static TTF_Font *sub_font_outline = NULL;
static TTF_Font *sub_font_cjk         = NULL;
static TTF_Font *sub_font_cjk_outline = NULL;
static int       font_loaded      = 0;

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
    sub_text_cues_clear(ps);
}

/* All multi-cue state gone at once — seek flushes, track changes, and
 * codec close all route through here so no path can leave a stale cue
 * displaying against a new timeline. */
void sub_text_cues_clear(PlayerState *ps) {
    for (int i = 0; i < SUB_TEXT_CUES; i++)
        ps->sub_cues[i].valid = 0;
    ps->sub_cue_count = 0;
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

    /* Seek guard: sub_close_codec/sub_open_codec free and replace the
     * context the demux seek handler flushes under seek_mutex. TryLock —
     * an S press landing inside a seek is dropped, not stalled on. */
    if (!SDL_TryLockMutex(ps->seek_mutex)) return;

    /* Cycle: 0 (off) → 1 → 2 → ... → N → 0 (off) */
    ps->sub_selection = (ps->sub_selection + 1) % (ps->sub_count + 1);

    /* Queues are NOT flushed here (revised from DSVP main 55834d4): the
     * demuxer keeps every track as a rolling ~35s window precisely so the
     * newly selected track has the current moment's packets on hand —
     * flushing made S appear dead for the ~10s it took playback to reach
     * the demux read position. The decode-side stale-skip absorbs the
     * (bounded) backlog. */

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
        sub_text_cues_clear(ps);

        snprintf(ps->sub_osd, sizeof(ps->sub_osd), "Subtitles: %s",
            ps->sub_stream_names[sel]);
        log_msg("Subtitles: %s (stream %d)",
            ps->sub_stream_names[sel], stream_idx);
    }

    ps->sub_osd_until = get_time_sec() + 2.0;
    SDL_UnlockMutex(ps->seek_mutex);
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

static void sub_decode_pending_locked(PlayerState *ps);

/* ── P2-16: multi-cue text drain ──
 *
 * The single-slot design stopped consuming packets while a cue was on
 * screen, so a cue starting inside another's window was popped only
 * after the first expired — and then discarded as already expired.
 * Overlapping dialogue and ASS signs/second-speaker events never
 * displayed. Text tracks instead drain EVERY due packet each tick into
 * up to SUB_TEXT_CUES simultaneous cues.
 *
 * Bitmap tracks (PGS/DVB/VobSub) keep the single-set flow in
 * sub_decode_pending_locked — display sets are full-screen compositions
 * where one-active-set is the format's own model, and that path's
 * clear-packet/END-inject machinery stays exactly as field-verified. */

static void sub_text_cue_push(PlayerState *ps, const char *text,
                              double start, double end) {
    int slot = -1;
    for (int i = 0; i < SUB_TEXT_CUES; i++) {
        if (!ps->sub_cues[i].valid) { slot = i; break; }
    }
    if (slot < 0) {
        /* Full: evict the cue that ends soonest — it has the least
         * display time left to lose. */
        slot = 0;
        for (int i = 1; i < SUB_TEXT_CUES; i++)
            if (ps->sub_cues[i].end_pts < ps->sub_cues[slot].end_pts)
                slot = i;
    }
    snprintf(ps->sub_cues[slot].text, SUB_TEXT_SIZE, "%s", text);
    ps->sub_cues[slot].start_pts = start;
    ps->sub_cues[slot].end_pts   = end;
    ps->sub_cues[slot].valid     = 1;
}

static void sub_text_drain_multi(PlayerState *ps, PacketQueue *spq, double now) {
    /* Expire first so eviction never has to fight cues that are
     * already off screen. */
    for (int i = 0; i < SUB_TEXT_CUES; i++)
        if (ps->sub_cues[i].valid && now > ps->sub_cues[i].end_pts)
            ps->sub_cues[i].valid = 0;

    AVPacket pkt;
    for (;;) {
        /* Due-only gate — same rule as the bitmap drain below: never
         * consume a packet whose time has not come. */
        {
            int64_t head_pts;
            if (pq_peek_pts(spq, &head_pts) && head_pts != AV_NOPTS_VALUE) {
                AVStream *head_st =
                    ps->fmt_ctx->streams[ps->sub_active_idx];
                double head_sec =
                    (double)head_pts * av_q2d(head_st->time_base);
                if (head_sec > now) break;
            }
        }
        if (pq_get(spq, &pkt, 0) <= 0) break;

        AVSubtitle sub;
        int got_sub = 0;
        int ret = avcodec_decode_subtitle2(ps->sub_codec_ctx, &sub,
                                           &got_sub, &pkt);
        log_msg("Sub: MAIN-LOOP pkt_size=%d got_sub=%d rects=%u ret=%d",
                pkt.size, got_sub, got_sub ? sub.num_rects : 0, ret);
        if (ret < 0) {
            log_msg("Sub: decode error ret=%d", ret);
            av_packet_unref(&pkt);
            continue;
        }
        if (!got_sub) {
            av_packet_unref(&pkt);
            continue;
        }

        AVStream *st = ps->fmt_ctx->streams[ps->sub_active_idx];
        double pkt_pts = 0.0;
        if (pkt.pts != AV_NOPTS_VALUE)
            pkt_pts = (double)pkt.pts * av_q2d(st->time_base);

        double start = pkt_pts + (double)sub.start_display_time / 1000.0;
        double end   = pkt_pts + (double)sub.end_display_time / 1000.0;

        /* SRT/subrip decoded by FFmpeg often sets end_display_time=0.
         * The actual duration is in pkt.duration in stream time_base. */
        if (sub.end_display_time == 0 && pkt.duration > 0)
            end = pkt_pts + (double)pkt.duration * av_q2d(st->time_base);
        else if (sub.end_display_time == 0)
            end = start + 3.0;  /* last resort fallback */

        if (end - start > SUB_STALE_CAP_SEC)
            end = start + SUB_STALE_CAP_SEC;

        if (end < now) {
            log_msg("Sub: skipped expired (end=%.1f < now=%.1f)", end, now);
            avsubtitle_free(&sub);
            av_packet_unref(&pkt);
            continue;
        }

        /* One cue per rect: a packet carrying several ASS events is
         * several simultaneous cues, not one (the single-slot code
         * kept only the last rect). */
        for (unsigned i = 0; i < sub.num_rects; i++) {
            AVSubtitleRect *rect = sub.rects[i];
            char text[SUB_TEXT_SIZE] = {0};
            if (rect->type == SUBTITLE_TEXT && rect->text) {
                snprintf(text, sizeof(text), "%s", rect->text);
                log_msg("Sub [TEXT] %.1f-%.1f: \"%.*s\"", start, end, 60, text);
            } else if (rect->type == SUBTITLE_ASS && rect->ass) {
                strip_ass_markup(rect->ass, text, sizeof(text));
                log_msg("Sub [ASS] %.1f-%.1f: \"%.*s\"", start, end, 60, text);
            }
            if (text[0])
                sub_text_cue_push(ps, text, start, end);
        }

        avsubtitle_free(&sub);
        av_packet_unref(&pkt);
    }

    int n = 0;
    for (int i = 0; i < SUB_TEXT_CUES; i++)
        if (ps->sub_cues[i].valid) n++;
    ps->sub_cue_count = n;
}

/* Public entry — seek guard. The demux seek handler flushes
 * sub_codec_ctx under seek_mutex (player.c); the video decode thread
 * (seek_mutex TryLock) and the audio callback (seeking check) both
 * carry guards against that flush — this path carried neither, and
 * avcodec_decode_subtitle2 racing avcodec_flush_buffers on one
 * context is heap-corruption class. TryLock like the video decode
 * thread: skip this tick rather than stall the main loop. */
void sub_decode_pending(PlayerState *ps) {
    if (ps->seeking || ps->seek_request) return;
    if (!SDL_TryLockMutex(ps->seek_mutex)) return;
    sub_decode_pending_locked(ps);
    SDL_UnlockMutex(ps->seek_mutex);
}

static void sub_decode_pending_locked(PlayerState *ps) {
    if (ps->sub_active_idx < 0 || !ps->sub_codec_ctx) return;
    if (ps->sub_selection <= 0 || ps->sub_selection > ps->sub_count) return;

    /* Get the queue for the active subtitle stream */
    int queue_idx = ps->sub_selection - 1;
    PacketQueue *spq = &ps->sub_pqs[queue_idx];

    double now = player_clock(ps);

    /* P2-16: text tracks take the multi-cue drain; everything below
     * this dispatch is the bitmap flow, untouched. Track type is a
     * codec property, fixed for the life of the selection. */
    {
        const AVCodecDescriptor *d =
            avcodec_descriptor_get(ps->sub_codec_ctx->codec_id);
        if (d && (d->props & AV_CODEC_PROP_TEXT_SUB)) {
            sub_text_drain_multi(ps, spq, now);
            return;
        }
    }

    /* If current subtitle is still valid and on-screen, keep it.
     * Exception: bitmap subs currently DISPLAYING need to drain the queue
     * for "clear" packets (0 rects) that signal when to hide.
     * Once the clear is found (end_pts updated from the 30s cap), stop draining. */
    if (ps->sub_valid && now <= ps->sub_end_pts) {
        if (!ps->sub_is_bitmap) return;
        if (now < ps->sub_start_pts) return;  /* not showing yet, don't drain */
        /* Duration below the cap means a real clear packet already set
         * end_pts — the drain-for-clear is done. */
        if (ps->sub_end_pts - ps->sub_start_pts < SUB_CLEAR_FOUND_SEC) return;
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
    for (;;) {
        /* Never consume packets whose time has not come (DSVP main
         * 25f54d7, gate made unconditional in review). The old code
         * decoded everything queued hunting for a 0-rect clear and
         * permanently DISCARDED any display set that had rects — i.e.
         * the next caption. Segments of one display set share the
         * set's PTS, so stopping at a future PTS cannot split a due
         * set.
         *
         * The gate must apply to EVERY drain, not just drain-for-clear:
         * an S-press onto an END-stripped PGS track drains with
         * sub_valid == 0, and ungated it consumed the ~10s of demux
         * read-ahead too — every due set decoded got_sub=0, each PCS
         * clobbered the previous accumulation, and the single post-
         * drain END inject assembled only the last, FUTURE set. The
         * current moment's caption (the whole point of the rolling
         * window) was destroyed unseen. Gated, the last accumulated
         * set is the due one, and the inject assembles exactly it. */
        {
            int64_t head_pts;
            if (pq_peek_pts(spq, &head_pts) && head_pts != AV_NOPTS_VALUE) {
                AVStream *head_st =
                    ps->fmt_ctx->streams[ps->sub_active_idx];
                double head_sec =
                    (double)head_pts * av_q2d(head_st->time_base);
                if (head_sec > now) break;
            }
        }
        if (pq_get(spq, &pkt, 0) <= 0) break;

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
         * until the clear packet arrives). Cap as a safety net — the
         * 0-rect clear packet will expire it earlier. */
        if (end - start > SUB_STALE_CAP_SEC) {
            end = start + SUB_STALE_CAP_SEC;
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
            /* A due display set WITH rects is the next caption directly
             * replacing the current one (epoch continuation). It ends the
             * displayed set now — fall through to normal extraction and
             * display it instead of destroying it. */
            draining_for_clear = 0;
            ps->sub_valid = 0;
        }

        /* Skip expired sets BEFORE rect extraction — the palette→RGBA
         * conversion (av_malloc + per-pixel fill) for sets that were
         * then discarded was a per-S-press hitch on a deep backlog.
         * Cannot move above the decode (the decoder must see every
         * packet to stay in sync) nor above the drain-for-clear block
         * (an expired set replacing the displayed one must still end
         * it). A stale 0-rect clear is safe to skip: at this point
         * nothing is displaying that it could fail to clear. */
        if (end < now) {
            log_msg("Sub: skipped expired (end=%.1f < now=%.1f)", end, now);
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

                /* Convert paletted pixels to RGBA.
                 * w*h*4 is int arithmetic and w/h come from a decoded
                 * subtitle rect in an untrusted file — reject anything that
                 * would overflow into a small allocation while the fill loop
                 * writes offsets computed the same overflowing way. */
                uint8_t *rgba = ((int64_t)w * h > (int64_t)INT_MAX / 4)
                                ? NULL : av_malloc((size_t)w * h * 4);
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

        /* A LATER due display set still queued supersedes this one.
         * On an S-press onto a track with backlog, every set younger
         * than SUB_STALE_CAP_SEC survives the expiry check — keeping
         * the first and breaking flashed N obsolete captions, one per
         * frame, before the current one settled. The newest due set is
         * the one that reflects "now"; the future head stays queued. */
        {
            int64_t head_pts;
            if (pq_peek_pts(spq, &head_pts) && head_pts != AV_NOPTS_VALUE) {
                double head_sec = (double)head_pts * av_q2d(st->time_base);
                if (head_sec <= now) {
                    log_msg("Sub: superseded by due set (head=%.1f)", head_sec);
                    sub_clear_bitmaps(ps);
                    continue;
                }
            }
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
     * from the current display set are loaded and END can assemble them.
     *
     * NOT gated on !sub_valid (DSVP main 7f09ae0): on END-stripped MKV,
     * the replacement caption B (or a genuine clear) drained while
     * caption A displays decodes with got_sub=0 and NEEDS the inject to
     * fire — gating on !sub_valid meant B accumulated silently, was
     * clobbered by the next set's PCS, and never showed while A stuck
     * for its 30s cap. Premature-END safety holds regardless: the
     * peek-based drain above only consumes DUE packets, and a display
     * set's segments share the set's PTS, so consumed sets are
     * complete. */
    if (pgs_packets_this_drain > 0 &&
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
            if (end - start > SUB_STALE_CAP_SEC) end = start + SUB_STALE_CAP_SEC;

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
                        uint8_t *rgba = ((int64_t)w * h > (int64_t)INT_MAX / 4)
                                        ? NULL : av_malloc((size_t)w * h * 4);
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

