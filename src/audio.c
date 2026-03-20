/*
 * DSVP — Dead Simple Video Player
 * audio.c — Audio decode, resample, and SDL3 audio stream
 *
 * SDL3 audio model:
 *
 *   1. We open an SDL_AudioStream via SDL_OpenAudioDeviceStream(),
 *      which creates a stream bound to a playback device.
 *   2. A "get" callback fires when the device needs more samples.
 *      We decode FFmpeg audio frames, resample to S16 stereo, and
 *      push data into the stream via SDL_PutAudioStreamData().
 *   3. Volume is controlled via SDL_SetAudioStreamGain() — no
 *      manual mixing needed.
 *   4. audio_clock tracks playback position for A/V sync.
 */

#include "dsvp.h"

/* ═══════════════════════════════════════════════════════════════════
 * Audio Decode
 * ═══════════════════════════════════════════════════════════════════ */

int audio_decode_frame(PlayerState *ps) {
    AVPacket pkt;
    int ret;
    int data_size = 0;

    for (;;) {
        ret = avcodec_receive_frame(ps->audio_codec_ctx, ps->audio_frame);
        if (ret == 0) {
            if (!ps->swr_ctx) {
                AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
                ret = swr_alloc_set_opts2(&ps->swr_ctx,
                    &out_layout, AV_SAMPLE_FMT_S16, ps->audio_spec.freq,
                    &ps->audio_frame->ch_layout, ps->audio_frame->format,
                    ps->audio_frame->sample_rate, 0, NULL);
                if (ret < 0 || swr_init(ps->swr_ctx) < 0) {
                    log_msg("ERROR: swr init failed: %s", av_err2str(ret));
                    return -1;
                }
            }

            int out_samples = swr_get_out_samples(ps->swr_ctx, ps->audio_frame->nb_samples);
            int out_size = out_samples * 2 * 2;

            if (!ps->audio_buf || out_size > AUDIO_BUF_SIZE) {
                av_free(ps->audio_buf);
                ps->audio_buf = av_malloc(AUDIO_BUF_SIZE);
                if (!ps->audio_buf) return -1;
            }

            uint8_t *out_buf = ps->audio_buf;
            int converted = swr_convert(ps->swr_ctx,
                &out_buf, out_samples,
                (const uint8_t **)ps->audio_frame->data,
                ps->audio_frame->nb_samples);

            if (converted < 0) {
                fprintf(stderr, "[DSVP] Resample error\n");
                return -1;
            }

            data_size = converted * 2 * 2;

            int64_t frame_pts = ps->audio_frame->best_effort_timestamp;
            if (frame_pts == AV_NOPTS_VALUE)
                frame_pts = ps->audio_frame->pts;
            if (frame_pts != AV_NOPTS_VALUE) {
                AVStream *as = ps->fmt_ctx->streams[ps->audio_stream_idx];
                ps->audio_clock = (double)frame_pts * av_q2d(as->time_base);
            }
            ps->audio_clock += (double)converted / ps->audio_spec.freq;

            av_frame_unref(ps->audio_frame);
            return data_size;
        }

        if (ret != AVERROR(EAGAIN))
            return -1;

        ret = pq_get(&ps->audio_pq, &pkt, 0);
        if (ret <= 0) return -1;

        ret = avcodec_send_packet(ps->audio_codec_ctx, &pkt);
        av_packet_unref(&pkt);
        if (ret < 0) return -1;
    }
}


/* ═══════════════════════════════════════════════════════════════════
 * SDL3 Audio Stream Callback
 * ═══════════════════════════════════════════════════════════════════ */

void SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream,
                             int additional_amount, int total_amount) {
    PlayerState *ps = (PlayerState *)userdata;
    (void)total_amount;

    if (ps->paused || ps->seek_request || ps->seeking) return;
    if (additional_amount <= 0) return;

    int written = 0;
    while (written < additional_amount) {
        if (ps->audio_buf_index >= ps->audio_buf_size) {
            int decoded = audio_decode_frame(ps);
            if (decoded <= 0) break;
            ps->audio_buf_size  = decoded;
            ps->audio_buf_index = 0;
        }

        int remaining = ps->audio_buf_size - ps->audio_buf_index;
        int to_push   = additional_amount - written;
        if (to_push > remaining) to_push = remaining;

        SDL_PutAudioStreamData(stream,
            ps->audio_buf + ps->audio_buf_index, to_push);

        written             += to_push;
        ps->audio_buf_index += to_push;
    }

    /* ── Audio clock sync snapshot ──
     *
     * audio_clock reflects the PTS at the END of the last decoded frame.
     * But audio_decode_frame() updates audio_clock multiple times during
     * the callback (line 70: set PTS, line 72: += samples, loop repeats).
     * The main thread reads audio_clock for A/V sync at arbitrary times.
     *
     * If the main thread reads DURING this callback, it sees the raw
     * decode position (too far ahead) instead of the corrected playback
     * position.  This data race causes audio_clock to appear ~20-40ms
     * ahead, making av_diff chronically negative and locking 60fps
     * content into a two-decode-per-VSync equilibrium.
     *
     * Fix: compute the corrected value ONCE at the end of the callback
     * and write it to audio_clock_sync.  The main thread reads ONLY
     * audio_clock_sync, which always has the full correction applied.
     *
     * CRITICAL: Cap the correction at 100ms to prevent FLAC/large-buffer
     * runaway where SDL reports huge queued amounts during startup. */
    if (ps->audio_spec.freq > 0 && !ps->seek_recovering) {
        int bytes_per_sample = 2 * 2;  /* S16 stereo = 4 bytes/frame */

        /* Our internal buffer: decoded but not yet pushed to SDL */
        int internal_pending = ps->audio_buf_size - ps->audio_buf_index;
        if (internal_pending < 0) internal_pending = 0;

        /* SDL stream pipeline: pushed but not yet played by device */
        int stream_pending = SDL_GetAudioStreamQueued(stream);
        if (stream_pending < 0) stream_pending = 0;

        double buffered_sec = (double)(internal_pending + stream_pending)
                            / (ps->audio_spec.freq * bytes_per_sample);

        /* Cap at 100ms — prevents FLAC/large-buffer runaway */
        if (buffered_sec > 0.1) buffered_sec = 0.1;

        /* Single atomic write — main thread reads only this field */
        ps->audio_clock_sync = ps->audio_clock - buffered_sec;
    } else {
        ps->audio_clock_sync = ps->audio_clock;
    }
}


/* ═══════════════════════════════════════════════════════════════════
 * Open / Close Audio Device
 * ═══════════════════════════════════════════════════════════════════ */

int audio_open(PlayerState *ps) {
    if (!ps->audio_codec_ctx) return -1;

    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.format   = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq     = ps->audio_codec_ctx->sample_rate;

    ps->audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &spec, audio_callback, ps);

    if (!ps->audio_stream) {
        log_msg("ERROR: SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
        return -1;
    }

    ps->audio_spec = spec;

    ps->audio_buf       = av_malloc(AUDIO_BUF_SIZE);
    ps->audio_buf_size  = 0;
    ps->audio_buf_index = 0;

    SDL_SetAudioStreamGain(ps->audio_stream, ps->volume);
    SDL_ResumeAudioStreamDevice(ps->audio_stream);

    log_msg("Audio opened: %d Hz, %d ch (SDL3 stream)",
        spec.freq, spec.channels);
    return 0;
}

void audio_close(PlayerState *ps) {
    if (ps->audio_stream) {
        SDL_DestroyAudioStream(ps->audio_stream);
        ps->audio_stream = NULL;
    }
}


/* ═══════════════════════════════════════════════════════════════════
 * Audio Stream Discovery
 * ═══════════════════════════════════════════════════════════════════ */

void audio_find_streams(PlayerState *ps) {
    ps->aud_count     = 0;
    ps->aud_selection = 0;

    for (unsigned i = 0; i < ps->fmt_ctx->nb_streams && ps->aud_count < MAX_AUDIO_STREAMS; i++) {
        AVStream *st = ps->fmt_ctx->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;

        int idx = ps->aud_count;
        ps->aud_stream_indices[idx] = (int)i;

        const AVDictionaryEntry *lang  = av_dict_get(st->metadata, "language", NULL, 0);
        const AVDictionaryEntry *title = av_dict_get(st->metadata, "title", NULL, 0);
        const char *codec_name = avcodec_get_name(st->codecpar->codec_id);
        int channels = st->codecpar->ch_layout.nb_channels;
        int rate     = st->codecpar->sample_rate;

        char desc[128] = {0};
        if (title && lang)
            snprintf(desc, sizeof(desc), "%s (%s)", title->value, lang->value);
        else if (lang)
            snprintf(desc, sizeof(desc), "%s", lang->value);
        else if (title)
            snprintf(desc, sizeof(desc), "%s", title->value);
        else
            snprintf(desc, sizeof(desc), "Track %d", idx + 1);

        snprintf(ps->aud_stream_names[idx], sizeof(ps->aud_stream_names[idx]),
            "%s [%s %dch %dHz]", desc, codec_name, channels, rate);

        if ((int)i == ps->audio_stream_idx)
            ps->aud_selection = idx;

        log_msg("Audio stream %d: [%d] %s", idx, (int)i, ps->aud_stream_names[idx]);
        ps->aud_count++;
    }

    log_msg("Found %d audio stream(s), active: %d (%s)",
        ps->aud_count, ps->aud_selection,
        ps->aud_count > 0 ? ps->aud_stream_names[ps->aud_selection] : "none");
}


/* ═══════════════════════════════════════════════════════════════════
 * Audio Track Cycling
 * ═══════════════════════════════════════════════════════════════════ */

void audio_cycle(PlayerState *ps) {
    if (ps->aud_count <= 1) {
        snprintf(ps->aud_osd, sizeof(ps->aud_osd),
            ps->aud_count == 0 ? "No audio tracks" : "Only one audio track");
        ps->aud_osd_until = get_time_sec() + 2.0;
        return;
    }

    int new_sel = (ps->aud_selection + 1) % ps->aud_count;

    /* Skip TrueHD tracks — unusable without HDMI bitstreaming */
    int checked = 0;
    while (checked < ps->aud_count) {
        int idx = ps->aud_stream_indices[new_sel];
        AVStream *st = ps->fmt_ctx->streams[idx];
        if (st->codecpar->codec_id != AV_CODEC_ID_TRUEHD)
            break;
        log_msg("Audio: skipping TrueHD track %d (%s)",
            new_sel, ps->aud_stream_names[new_sel]);
        new_sel = (new_sel + 1) % ps->aud_count;
        checked++;
    }
    if (checked >= ps->aud_count || new_sel == ps->aud_selection) {
        /* All other tracks are TrueHD — stay on current */
        snprintf(ps->aud_osd, sizeof(ps->aud_osd),
            "No other non-TrueHD audio tracks");
        ps->aud_osd_until = get_time_sec() + 2.0;
        return;
    }

    int new_stream_idx = ps->aud_stream_indices[new_sel];

    log_msg("Audio: switching to %s (stream %d)",
        ps->aud_stream_names[new_sel], new_stream_idx);

    if (ps->audio_stream)
        SDL_PauseAudioStreamDevice(ps->audio_stream);

    pq_flush(&ps->audio_pq);

    if (ps->audio_codec_ctx)
        avcodec_free_context(&ps->audio_codec_ctx);
    if (ps->swr_ctx)
        swr_free(&ps->swr_ctx);

    ps->audio_buf_size  = 0;
    ps->audio_buf_index = 0;

    AVStream *as = ps->fmt_ctx->streams[new_stream_idx];
    const AVCodec *codec = avcodec_find_decoder(as->codecpar->codec_id);
    if (!codec) {
        log_msg("ERROR: No decoder for audio codec %s",
            avcodec_get_name(as->codecpar->codec_id));
        snprintf(ps->aud_osd, sizeof(ps->aud_osd), "Audio: codec error");
        ps->aud_osd_until = get_time_sec() + 2.0;
        return;
    }

    ps->audio_codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ps->audio_codec_ctx, as->codecpar);
    ps->audio_codec_ctx->thread_count = 0;

    int ret = avcodec_open2(ps->audio_codec_ctx, codec, NULL);
    if (ret < 0) {
        log_msg("ERROR: Cannot open audio codec: %s", av_err2str(ret));
        avcodec_free_context(&ps->audio_codec_ctx);
        snprintf(ps->aud_osd, sizeof(ps->aud_osd), "Audio: codec error");
        ps->aud_osd_until = get_time_sec() + 2.0;
        return;
    }

    int new_rate = ps->audio_codec_ctx->sample_rate;
    if (new_rate != ps->audio_spec.freq) {
        log_msg("Audio: sample rate changed %d -> %d, reopening stream",
            ps->audio_spec.freq, new_rate);
        audio_close(ps);
        audio_open(ps);
    }

    ps->aud_selection    = new_sel;
    ps->audio_stream_idx = new_stream_idx;

    log_msg("Audio: now playing %s (%s %dHz)",
        ps->aud_stream_names[new_sel], codec->name, new_rate);

    double pos = ps->audio_clock_sync;
    if (pos < 0.1) pos = 0.1;
    ps->seek_target  = (int64_t)(pos * AV_TIME_BASE);
    ps->seek_flags   = AVSEEK_FLAG_BACKWARD;
    ps->seek_request = 1;

    if (ps->audio_stream && !ps->paused)
        SDL_ResumeAudioStreamDevice(ps->audio_stream);

    snprintf(ps->aud_osd, sizeof(ps->aud_osd), "Audio: %s",
        ps->aud_stream_names[new_sel]);
    ps->aud_osd_until = get_time_sec() + 2.0;
}
