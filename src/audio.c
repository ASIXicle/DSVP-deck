/*
 * DSVP — Dead Simple Video Player
 * audio.c — Audio decode, resample, and SDL audio callback
 *
 * How audio playback works:
 *
 *   1. SDL opens an audio device with a callback function.
 *   2. SDL's audio thread calls audio_callback() whenever it needs
 *      more samples to play.
 *   3. audio_callback() pulls data from an internal buffer. When the
 *      buffer runs out, it calls audio_decode_frame() to decode more
 *      packets from the audio packet queue and resample them to the
 *      output format (signed 16-bit, stereo, device sample rate).
 *   4. As samples are consumed, we update audio_clock to track the
 *      current playback position. The video sync uses this clock
 *      as the master reference.
 */

#include "dsvp.h"

/* ═══════════════════════════════════════════════════════════════════
 * Audio Decode
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Decode one audio frame from the packet queue and resample it
 * to the SDL output format. Returns the number of bytes of
 * resampled data, or -1 on error / no data.
 */
int audio_decode_frame(PlayerState *ps) {
    AVPacket pkt;
    int ret;
    int data_size = 0;

    for (;;) {
        /* Try to receive a decoded frame */
        ret = avcodec_receive_frame(ps->audio_codec_ctx, ps->audio_frame);
        if (ret == 0) {
            /* Got a frame — resample to output format */

            /* Lazy-init SwrContext on first frame (we need the actual
             * frame's channel layout which may differ from codecpar) */
            if (!ps->swr_ctx) {
                AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;

                ret = swr_alloc_set_opts2(&ps->swr_ctx,
                    &out_layout,                        /* out ch layout  */
                    AV_SAMPLE_FMT_S16,                  /* out format     */
                    ps->audio_spec.freq,                /* out sample rate*/
                    &ps->audio_frame->ch_layout,        /* in ch layout   */
                    ps->audio_frame->format,            /* in format      */
                    ps->audio_frame->sample_rate,       /* in sample rate */
                    0, NULL
                );
            if (ret < 0 || swr_init(ps->swr_ctx) < 0) {
                    log_msg("ERROR: swr init failed: %s", av_err2str(ret));
                    return -1;
                }
            }

            /* Calculate output buffer size */
            int out_samples = swr_get_out_samples(ps->swr_ctx, ps->audio_frame->nb_samples);
            int out_size = out_samples * 2 * 2; /* stereo * 16-bit */

            /* Grow buffer if needed */
            if (!ps->audio_buf || out_size > AUDIO_BUF_SIZE) {
                av_free(ps->audio_buf);
                ps->audio_buf = av_malloc(AUDIO_BUF_SIZE);
                if (!ps->audio_buf) return -1;
            }

            uint8_t *out_buf = ps->audio_buf;
            int converted = swr_convert(ps->swr_ctx,
                &out_buf, out_samples,
                (const uint8_t **)ps->audio_frame->data,
                ps->audio_frame->nb_samples
            );

            if (converted < 0) {
                fprintf(stderr, "[DSVP] Resample error\n");
                return -1;
            }

            data_size = converted * 2 * 2; /* stereo * 16-bit */

            /* Update audio clock from frame PTS.
             * Prefer best_effort_timestamp for same reasons as video. */
            int64_t frame_pts = ps->audio_frame->best_effort_timestamp;
            if (frame_pts == AV_NOPTS_VALUE)
                frame_pts = ps->audio_frame->pts;
            if (frame_pts != AV_NOPTS_VALUE) {
                AVStream *as = ps->fmt_ctx->streams[ps->audio_stream_idx];
                ps->audio_clock = (double)frame_pts * av_q2d(as->time_base);
            }
            /* Advance clock by the duration of the samples we just decoded */
            ps->audio_clock += (double)converted / ps->audio_spec.freq;

            av_frame_unref(ps->audio_frame);
            return data_size;
        }

        if (ret != AVERROR(EAGAIN)) {
            return -1; /* decoder error or EOF */
        }

        /* Need more packets — pull from queue (non-blocking) */
        ret = pq_get(&ps->audio_pq, &pkt, 0);
        if (ret <= 0) return -1; /* no packets right now */

        ret = avcodec_send_packet(ps->audio_codec_ctx, &pkt);
        av_packet_unref(&pkt);

        if (ret < 0) return -1;
    }
}


/* ═══════════════════════════════════════════════════════════════════
 * SDL Audio Callback
 * ═══════════════════════════════════════════════════════════════════
 *
 * Called by SDL's audio thread whenever the device needs more samples.
 * We fill `stream` with `len` bytes of audio data, mixing our volume.
 */

void audio_callback(void *userdata, Uint8 *stream, int len) {
    PlayerState *ps = (PlayerState *)userdata;

    /* Silence the buffer first (prevents noise on underrun) */
    memset(stream, 0, len);

    if (ps->paused || ps->seek_request || ps->seeking) return;

    int written = 0;

    while (written < len) {
        /* If our internal buffer is exhausted, decode more */
        if (ps->audio_buf_index >= ps->audio_buf_size) {
            int decoded = audio_decode_frame(ps);
            if (decoded <= 0) {
                /* No data available — output silence for remainder */
                break;
            }
            ps->audio_buf_size  = decoded;
            ps->audio_buf_index = 0;
        }

        /* Copy from our buffer to SDL's buffer */
        int remaining = ps->audio_buf_size - ps->audio_buf_index;
        int to_copy   = len - written;
        if (to_copy > remaining) to_copy = remaining;

        /* Apply volume: mix into the stream buffer */
        SDL_MixAudioFormat(
            stream + written,
            ps->audio_buf + ps->audio_buf_index,
            AUDIO_S16SYS,
            to_copy,
            (int)(ps->volume * SDL_MIX_MAXVOLUME)
        );

        written             += to_copy;
        ps->audio_buf_index += to_copy;
    }
}


/* ═══════════════════════════════════════════════════════════════════
 * Open / Close Audio Device
 * ═══════════════════════════════════════════════════════════════════ */

int audio_open(PlayerState *ps) {
    if (!ps->audio_codec_ctx) return -1;

    SDL_AudioSpec wanted;
    SDL_zero(wanted);

    wanted.freq     = ps->audio_codec_ctx->sample_rate;
    wanted.format   = AUDIO_S16SYS;     /* signed 16-bit, native byte order */
    wanted.channels = 2;                /* always output stereo              */
    wanted.samples  = SDL_AUDIO_BUFFER_SZ;
    wanted.callback = audio_callback;
    wanted.userdata = ps;

    ps->audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted, &ps->audio_spec, 0);
    if (ps->audio_dev == 0) {
        log_msg("ERROR: SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return -1;
    }

    /* Allocate initial audio buffer */
    ps->audio_buf       = av_malloc(AUDIO_BUF_SIZE);
    ps->audio_buf_size  = 0;
    ps->audio_buf_index = 0;

    /* Start playback */
    SDL_PauseAudioDevice(ps->audio_dev, 0);

    log_msg("Audio opened: %d Hz, %d ch, buffer %d samples",
        ps->audio_spec.freq, ps->audio_spec.channels, ps->audio_spec.samples);

    return 0;
}

void audio_close(PlayerState *ps) {
    if (ps->audio_dev) {
        SDL_CloseAudioDevice(ps->audio_dev);
        ps->audio_dev = 0;
    }
}


/* ═══════════════════════════════════════════════════════════════════
 * Audio Stream Discovery
 * ═══════════════════════════════════════════════════════════════════
 *
 * Catalogs all audio streams in the container. Called once during
 * player_open, after the initial audio codec is already opened.
 */

void audio_find_streams(PlayerState *ps) {
    ps->aud_count     = 0;
    ps->aud_selection = 0;

    for (unsigned i = 0; i < ps->fmt_ctx->nb_streams && ps->aud_count < MAX_AUDIO_STREAMS; i++) {
        AVStream *st = ps->fmt_ctx->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;

        int idx = ps->aud_count;
        ps->aud_stream_indices[idx] = (int)i;

        /* Build a display name from metadata + codec info */
        const AVDictionaryEntry *lang  = av_dict_get(st->metadata, "language", NULL, 0);
        const AVDictionaryEntry *title = av_dict_get(st->metadata, "title", NULL, 0);
        const char *codec_name = avcodec_get_name(st->codecpar->codec_id);
        int channels = st->codecpar->ch_layout.nb_channels;
        int rate     = st->codecpar->sample_rate;

        char desc[128] = {0};
        if (title && lang) {
            snprintf(desc, sizeof(desc), "%s (%s)", title->value, lang->value);
        } else if (lang) {
            snprintf(desc, sizeof(desc), "%s", lang->value);
        } else if (title) {
            snprintf(desc, sizeof(desc), "%s", title->value);
        } else {
            snprintf(desc, sizeof(desc), "Track %d", idx + 1);
        }

        snprintf(ps->aud_stream_names[idx], sizeof(ps->aud_stream_names[idx]),
            "%s [%s %dch %dHz]", desc, codec_name, channels, rate);

        /* If this is the stream that was auto-selected, mark it */
        if ((int)i == ps->audio_stream_idx)
            ps->aud_selection = idx;

        log_msg("Audio stream %d: [%d] %s", idx, (int)i,
            ps->aud_stream_names[idx]);
        ps->aud_count++;
    }

    log_msg("Found %d audio stream(s), active: %d (%s)",
        ps->aud_count, ps->aud_selection,
        ps->aud_count > 0 ? ps->aud_stream_names[ps->aud_selection] : "none");
}


/* ═══════════════════════════════════════════════════════════════════
 * Audio Track Cycling
 * ═══════════════════════════════════════════════════════════════════
 *
 * Switches to the next audio track:
 *   1. Pause SDL audio device (stop callback)
 *   2. Flush audio queue
 *   3. Close old audio codec + resampler
 *   4. Open new audio codec
 *   5. Reopen SDL audio device (sample rate may differ)
 *   6. Seek to current position (resets demux read-head)
 *   7. Resume playback
 */

void audio_cycle(PlayerState *ps) {
    if (ps->aud_count <= 1) {
        snprintf(ps->aud_osd, sizeof(ps->aud_osd),
            ps->aud_count == 0 ? "No audio tracks" : "Only one audio track");
        ps->aud_osd_until = get_time_sec() + 2.0;
        return;
    }

    /* Cycle to next track */
    int new_sel = (ps->aud_selection + 1) % ps->aud_count;
    int new_stream_idx = ps->aud_stream_indices[new_sel];

    log_msg("Audio: switching to %s (stream %d)",
        ps->aud_stream_names[new_sel], new_stream_idx);

    /* 1. Pause audio device — stops callback from touching codec */
    if (ps->audio_dev)
        SDL_PauseAudioDevice(ps->audio_dev, 1);

    /* 2. Flush audio queue — discard old stream's packets */
    pq_flush(&ps->audio_pq);

    /* 3. Close old audio codec and resampler */
    if (ps->audio_codec_ctx) {
        avcodec_free_context(&ps->audio_codec_ctx);
    }
    if (ps->swr_ctx) {
        swr_free(&ps->swr_ctx);
    }

    /* Reset audio buffer */
    ps->audio_buf_size  = 0;
    ps->audio_buf_index = 0;

    /* 4. Open new audio codec */
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

    /* 5. Reopen SDL audio device if sample rate changed */
    int new_rate = ps->audio_codec_ctx->sample_rate;
    if (new_rate != ps->audio_spec.freq) {
        log_msg("Audio: sample rate changed %d → %d, reopening device",
            ps->audio_spec.freq, new_rate);
        audio_close(ps);
        audio_open(ps);
    }

    /* Update state */
    ps->aud_selection    = new_sel;
    ps->audio_stream_idx = new_stream_idx;

    log_msg("Audio: now playing %s (%s %dHz)",
        ps->aud_stream_names[new_sel], codec->name, new_rate);

    /* 6. Seek to current position — resets demux read-head so the new
     * audio stream picks up packets from the right spot. */
    double pos = ps->audio_clock;
    if (pos < 0.1) pos = 0.1;
    ps->seek_target  = (int64_t)(pos * AV_TIME_BASE);
    ps->seek_flags   = AVSEEK_FLAG_BACKWARD;
    ps->seek_request = 1;

    /* 7. Resume */
    if (ps->audio_dev && !ps->paused)
        SDL_PauseAudioDevice(ps->audio_dev, 0);

    /* OSD */
    snprintf(ps->aud_osd, sizeof(ps->aud_osd), "Audio: %s",
        ps->aud_stream_names[new_sel]);
    ps->aud_osd_until = get_time_sec() + 2.0;
}
