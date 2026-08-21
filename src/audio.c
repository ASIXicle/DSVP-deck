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
    int data_size;

    /* Codec can be NULL mid-session if audio_cycle() failed to open the
     * next track — the device stays open, so the callback still fires.
     * Produce silence instead of dereferencing NULL. */
    if (!ps->audio_codec_ctx) return -1;

    for (;;) {
        ret = avcodec_receive_frame(ps->audio_codec_ctx, ps->audio_frame);
        if (ret == 0) {
            /* ── Post-seek stale-frame skip ──
             *
             * After a seek in MPEG-TS, the demuxer reads packets in stream
             * order.  Audio packets interleaved before the video keyframe
             * enter the audio queue with PTS well below the first video
             * frame.  Seek recovery resets audio_clock to video_clock, but
             * the next audio decode would overwrite audio_clock backward,
             * creating multi-second positive A/V drift.
             *
             * Fix: discard decoded audio whose PTS is more than 50ms before
             * the recovery point.  The 50ms tolerance absorbs normal
             * interleave jitter without rejecting valid frames.  The floor
             * clears itself on the first accepted frame. */
            if (ps->audio_pts_floor > 0.0) {
                int64_t fp = ps->audio_frame->best_effort_timestamp;
                if (fp == AV_NOPTS_VALUE) fp = ps->audio_frame->pts;
                if (fp != AV_NOPTS_VALUE) {
                    AVStream *as = ps->fmt_ctx->streams[ps->audio_stream_idx];
                    double pts_sec = (double)fp * av_q2d(as->time_base);
                    if (pts_sec < ps->audio_pts_floor - 0.05) {
                        av_frame_unref(ps->audio_frame);
                        continue;   /* skip stale frame, pull next */
                    }
                }
                ps->audio_pts_floor = 0.0;  /* floor satisfied — clear */
            }

            /* Mid-stream format change (DVB/TS program boundaries do
             * this on legal streams): swr configured for the old frame
             * shape would read missing plane pointers (6ch planar →
             * 2ch = NULL derefs) or produce garbled output. Compare
             * against the configured shape and rebuild (review P1-7). */
            if (ps->swr_ctx &&
                (ps->audio_frame->format      != ps->swr_in_fmt ||
                 ps->audio_frame->sample_rate != ps->swr_in_rate ||
                 av_channel_layout_compare(&ps->audio_frame->ch_layout,
                                           &ps->swr_in_layout) != 0)) {
                log_msg("Audio: stream format changed "
                        "(fmt %d %dch %dHz -> fmt %d %dch %dHz) — "
                        "rebuilding resampler",
                        ps->swr_in_fmt, ps->swr_in_layout.nb_channels,
                        ps->swr_in_rate,
                        ps->audio_frame->format,
                        ps->audio_frame->ch_layout.nb_channels,
                        ps->audio_frame->sample_rate);
                swr_free(&ps->swr_ctx);
            }

            if (!ps->swr_ctx) {
                AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
                ret = swr_alloc_set_opts2(&ps->swr_ctx,
                    &out_layout, AV_SAMPLE_FMT_FLT, ps->audio_spec.freq,
                    &ps->audio_frame->ch_layout, ps->audio_frame->format,
                    ps->audio_frame->sample_rate, 0, NULL);
                int init_err = (ret < 0) ? ret : swr_init(ps->swr_ctx);
                if (init_err < 0) {
                    /* Free the half-built ctx or the next callback sees
                     * a non-NULL swr_ctx with stale in-shape records and
                     * re-runs this alloc/fail cycle every frame — while
                     * the old log printed av_err2str(0) = "Success"
                     * (review 2026-08-20 finding 15). */
                    log_msg("ERROR: swr init failed: %s", av_err2str(init_err));
                    swr_free(&ps->swr_ctx);
                    return -1;
                }
                ps->swr_in_fmt  = ps->audio_frame->format;
                ps->swr_in_rate = ps->audio_frame->sample_rate;
                av_channel_layout_uninit(&ps->swr_in_layout);
                if (av_channel_layout_copy(&ps->swr_in_layout,
                                           &ps->audio_frame->ch_layout) < 0) {
                    swr_free(&ps->swr_ctx);
                    return -1;
                }
            }

            int out_samples = swr_get_out_samples(ps->swr_ctx, ps->audio_frame->nb_samples);
            if (out_samples < 0) return -1;
            int out_size = out_samples * 2 * 4;  /* stereo F32 = 8 bytes/frame */

            /* HEAP-OVERFLOW FIX: the old code re-malloc'd the SAME
             * AUDIO_BUF_SIZE when out_size exceeded it, then told
             * swr_convert the buffer held out_samples anyway — a large
             * decoded frame plus heavy upsampling could write past the
             * end of the allocation. Grow the buffer to fit instead. */
            unsigned int need = (out_size > AUDIO_BUF_SIZE)
                              ? (unsigned int)out_size
                              : (unsigned int)AUDIO_BUF_SIZE;
            if (!ps->audio_buf || need > ps->audio_buf_cap) {
                av_free(ps->audio_buf);
                ps->audio_buf = av_malloc(need);
                if (!ps->audio_buf) { ps->audio_buf_cap = 0; return -1; }
                ps->audio_buf_cap = need;
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

            data_size = converted * 2 * 4;  /* stereo F32 = 8 bytes/frame */

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
     * runaway where SDL reports huge queued amounts during startup.
     *
     * The freq > 0 clause doubles as a liveness gate: audio_close
     * zeroes audio_spec.freq as its dead-stream sentinel (see the
     * COUPLING note there) — it both guards the division below and
     * keeps a post-failure callback from computing a correction
     * against a dead device's remembered rate. */
    if (ps->audio_spec.freq > 0 && !ps->seek_recovering) {
        int bytes_per_sample = 2 * 4;  /* F32 stereo = 8 bytes/frame */

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
    spec.format   = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq     = ps->audio_codec_ctx->sample_rate;

    /* Retry with backoff — PipeWire may still be reclaiming ALSA after restart.
     * 5 attempts × 400ms = 2s max wait, which covers PipeWire's typical
     * startup time on SteamOS (500-1500ms observed). */
    for (int attempt = 0; attempt < 5; attempt++) {
        ps->audio_stream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
            &spec, audio_callback, ps);
        if (ps->audio_stream) break;
        if (attempt < 4) {
            log_msg("Audio: SDL open attempt %d/5 failed: %s — retrying",
                    attempt + 1, SDL_GetError());
            SDL_Delay(400);
        }
    }

    if (!ps->audio_stream) {
        log_msg("ERROR: SDL_OpenAudioDeviceStream failed after retries: %s", SDL_GetError());
        return -1;
    }

    ps->audio_spec = spec;

    av_free(ps->audio_buf);  /* prevent leak on PCM↔bitstream mode switch */
    ps->audio_buf       = av_malloc(AUDIO_BUF_SIZE);
    ps->audio_buf_cap   = ps->audio_buf ? AUDIO_BUF_SIZE : 0;
    ps->audio_buf_size  = 0;
    ps->audio_buf_index = 0;

    SDL_SetAudioStreamGain(ps->audio_stream, ps->volume);
    /* Audio device starts paused. Resume is deferred until the first
     * video frame is displayed (seek_recovering gate in main.c).
     * This prevents audio from running ahead during VAAPI DPB warmup
     * or any other initial decode latency. */

    log_msg("Audio opened: %s %d Hz, %d ch (SDL3 stream)",
        (spec.format == SDL_AUDIO_F32) ? "F32" : "S16",
        spec.freq, spec.channels);
    return 0;
}

void audio_close(PlayerState *ps) {
    if (ps->audio_stream) {
        SDL_DestroyAudioStream(ps->audio_stream);
        ps->audio_stream = NULL;
    }
    /* Invalidate the remembered device rate (DSVP main 7f09ae0).
     * audio_cycle's reopen decision compares the new track's rate against
     * this — a stale value from a dead stream let the same-rate case skip
     * the reopen entirely, setting audio_stream_idx with no device and no
     * callback: demux refilled audio_pq, nothing drained it, and the
     * queue-full throttle froze all playback.
     *
     * COUPLING: this zero is also read as a liveness signal by the
     * clock-correction gate in the audio callback (the freq > 0 check
     * above audio_clock_sync's computation) and shows as "0 Hz" in the
     * info OSD after an audio failure. Do not remove it without
     * migrating those readers — the reopen predicate's direct
     * !audio_stream check alone does not cover them. */
    ps->audio_spec.freq = 0;
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

    /* Disambiguate identical display names */
    for (int a = 0; a < ps->aud_count; a++) {
        for (int b = a + 1; b < ps->aud_count; b++) {
            if (strcmp(ps->aud_stream_names[a], ps->aud_stream_names[b]) == 0) {
                char tmp[128];
                snprintf(tmp, sizeof(tmp), "%s #1", ps->aud_stream_names[a]);
                snprintf(ps->aud_stream_names[a], sizeof(ps->aud_stream_names[a]), "%s", tmp);
                snprintf(tmp, sizeof(tmp), "%s #2", ps->aud_stream_names[b]);
                snprintf(ps->aud_stream_names[b], sizeof(ps->aud_stream_names[b]), "%s", tmp);
            }
        }
    }

    log_msg("Found %d audio stream(s), active: %d (%s)",
        ps->aud_count, ps->aud_selection,
        ps->aud_count > 0 ? ps->aud_stream_names[ps->aud_selection] : "none");
}


/* ═══════════════════════════════════════════════════════════════════
 * Audio Track Cycling
 * ═══════════════════════════════════════════════════════════════════ */

/* Take audio fully offline after a failure: no index, no queue, OSD
 * notice. The ORDER is load-bearing and must exist in exactly one
 * place: the index is cleared BEFORE the flush because demux routes
 * packets without seek_mutex — a packet read during the failure
 * window would land after an earlier flush and sit orphaned forever,
 * blocking the EOF close (and, at throttle depth, stalling demux).
 * One racing in-flight pq_put can still land post-flush; the main
 * loop self-heals that case before its EOF gate. */
static int bitstream_codec_supported(PlayerState *ps);  /* defined below */

static void audio_disable(PlayerState *ps, const char *osd_msg) {
    ps->audio_stream_idx = -1;
    pq_flush(&ps->audio_pq);
    snprintf(ps->aud_osd, sizeof(ps->aud_osd), "%s", osd_msg);
    ps->aud_osd_until = get_time_sec() + 2.0;
}

/* Main-loop entry to the same disable (the bitstream-failed fallback
 * path needs it and lives in main.c). */
void audio_disable_public(PlayerState *ps, const char *osd_msg) {
    audio_disable(ps, osd_msg);
}

void audio_cycle(PlayerState *ps) {
    if (ps->aud_count <= 1) {
        snprintf(ps->aud_osd, sizeof(ps->aud_osd),
            ps->aud_count == 0 ? "No audio tracks" : "Only one audio track");
        ps->aud_osd_until = get_time_sec() + 2.0;
        return;
    }

    /* Seek guard: everything below frees/rebuilds audio_codec_ctx,
     * swr_ctx, and audio_stream — the same objects the demux seek
     * handler flushes and clears under seek_mutex. The SDL stream-lock
     * barrier below serializes against the audio CALLBACK only, not
     * against demux. TryLock: an A press landing inside an in-flight
     * seek is dropped (press again), never stalled on for up to the
     * 10s seek io_deadline. */
    if (!SDL_TryLockMutex(ps->seek_mutex)) {
        snprintf(ps->aud_osd, sizeof(ps->aud_osd), "Audio: busy (seeking)");
        ps->aud_osd_until = get_time_sec() + 2.0;
        return;
    }

    /* Stop bitstream before switching tracks — the bitstream thread
     * holds audio_pq and the spdifenc is configured for the current codec.
     * Switching tracks without stopping causes a race on pq_flush and
     * feeds wrong-codec packets to spdifenc. User can press P to
     * re-enable passthrough for the new codec. */
    int was_bitstream = ps->bitstream_active;
    if (was_bitstream) {
        bitstream_stop(ps);
        audio_open(ps);  /* need SDL audio for PCM fallback */
        if (!ps->paused && ps->audio_stream)
            SDL_ResumeAudioStreamDevice(ps->audio_stream);
    }

    int new_sel = (ps->aud_selection + 1) % ps->aud_count;

    /* Skip TrueHD tracks when not in bitstream passthrough */
    int skip_truehd = (ps->audio_mode == AUDIO_MODE_PCM || !was_bitstream);
    int checked = 0;
    while (skip_truehd && checked < ps->aud_count) {
        int idx = ps->aud_stream_indices[new_sel];
        AVStream *st = ps->fmt_ctx->streams[idx];
        if (st->codecpar->codec_id != AV_CODEC_ID_TRUEHD)
            break;
        log_msg("Audio: skipping TrueHD track %d (%s)",
            new_sel, ps->aud_stream_names[new_sel]);
        new_sel = (new_sel + 1) % ps->aud_count;
        checked++;
    }
    if (skip_truehd && (checked >= ps->aud_count || new_sel == ps->aud_selection)) {
        /* All other tracks are TrueHD — stay on current */
        snprintf(ps->aud_osd, sizeof(ps->aud_osd),
            "No other non-TrueHD audio tracks");
        ps->aud_osd_until = get_time_sec() + 2.0;
        SDL_UnlockMutex(ps->seek_mutex);
        return;
    }

    int new_stream_idx = ps->aud_stream_indices[new_sel];

    log_msg("Audio: switching to %s (stream %d)",
        ps->aud_stream_names[new_sel], new_stream_idx);

    if (ps->audio_stream) {
        SDL_PauseAudioStreamDevice(ps->audio_stream);
        /* BARRIER: pausing does not wait for a callback that's already
         * in flight. SDL runs the get-callback while holding the stream
         * lock, so lock+unlock returns only after any in-flight
         * audio_decode_frame() finishes — only then is it safe to
         * flush the queue and free the codec and swr contexts below. */
        SDL_LockAudioStream(ps->audio_stream);
        SDL_UnlockAudioStream(ps->audio_stream);
    }

    pq_flush(&ps->audio_pq);

    if (ps->audio_codec_ctx)
        avcodec_free_context(&ps->audio_codec_ctx);
    if (ps->swr_ctx)
        swr_free(&ps->swr_ctx);
    av_channel_layout_uninit(&ps->swr_in_layout);

    ps->audio_buf_size  = 0;
    ps->audio_buf_index = 0;

    AVStream *as = ps->fmt_ctx->streams[new_stream_idx];
    const AVCodec *codec = avcodec_find_decoder(as->codecpar->codec_id);
    if (!codec) {
        log_msg("ERROR: No decoder for audio codec %s",
            avcodec_get_name(as->codecpar->codec_id));
        audio_disable(ps, "Audio: codec error, audio off");
        SDL_UnlockMutex(ps->seek_mutex);
        return;
    }

    ps->audio_codec_ctx = avcodec_alloc_context3(codec);
    if (!ps->audio_codec_ctx
            || avcodec_parameters_to_context(ps->audio_codec_ctx,
                                             as->codecpar) < 0) {
        /* OOM / bad params: degrade like every other failure here
         * instead of dereferencing NULL in avcodec_open2. */
        log_msg("ERROR: audio codec context alloc/params failed");
        avcodec_free_context(&ps->audio_codec_ctx);  /* NULL-safe */
        audio_disable(ps, "Audio: codec error, audio off");
        SDL_UnlockMutex(ps->seek_mutex);
        return;
    }
    ps->audio_codec_ctx->thread_count = 0;

    int ret = avcodec_open2(ps->audio_codec_ctx, codec, NULL);
    if (ret < 0) {
        log_msg("ERROR: Cannot open audio codec: %s", av_err2str(ret));
        avcodec_free_context(&ps->audio_codec_ctx);
        audio_disable(ps, "Audio: codec error, audio off");
        SDL_UnlockMutex(ps->seek_mutex);
        return;
    }

    int new_rate = ps->audio_codec_ctx->sample_rate;
    /* Reopen when the rate changed OR when there is no live PCM stream
     * (recovering from an earlier audio failure — the reason the user is
     * pressing A). Keying on rate alone let the common same-rate case land
     * in the success path with a codec but no device: total-playback
     * freeze (DSVP main 7f09ae0). Bitstream/passthrough is excluded — it
     * legitimately runs with audio_stream == NULL via ALSA. */
    if ((!ps->audio_stream && !ps->bitstream_active)
            || new_rate != ps->audio_spec.freq) {
        log_msg("Audio: %s (rate %d -> %d)",
            ps->audio_stream ? "sample rate changed, reopening stream"
                             : "no live stream, reopening device",
            ps->audio_spec.freq, new_rate);
        audio_close(ps);
        if (audio_open(ps) < 0) {
            log_msg("Audio: device open failed — continuing video-only");
            avcodec_free_context(&ps->audio_codec_ctx);
            audio_disable(ps, "Audio: device error, audio off");
            SDL_UnlockMutex(ps->seek_mutex);
            return;
        }
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

    /* Auto-restart passthrough when the mode permits and the sink
     * supports the NEW codec (field 2026-08-13: an AC3→AC3 track
     * switch silently demoted to PCM until a manual P press — the
     * restart was TrueHD-only). TrueHD especially must not linger in
     * PCM: 1200 pkt/sec MLP floods audio_pq and starves video within
     * 200ms. bitstream_codec_supported covers sink caps and the >48k
     * platform blocklist. */
    if (ps->audio_mode != AUDIO_MODE_PCM
        && bitstream_codec_supported(ps)) {
        audio_close(ps);
        if (bitstream_start(ps)) {
            snprintf(ps->aud_osd, sizeof(ps->aud_osd), "Audio: %s (passthrough)",
                ps->aud_stream_names[new_sel]);
            ps->aud_osd_until = get_time_sec() + 2.0;
            SDL_UnlockMutex(ps->seek_mutex);
            return;
        }
        /* Bitstream failed -- fall back to PCM. Checked like the open
         * above: an unchecked failure here left a codec with no device
         * — audio_pq fills, the demux throttle gates on it forever,
         * total playback freeze (review 2026-08-20 finding 2, the
         * 7f09ae0 class this file already documents). */
        if (audio_open(ps) < 0) {
            log_msg("Audio: PCM fallback open failed after bitstream "
                    "failure — continuing video-only");
            avcodec_free_context(&ps->audio_codec_ctx);
            audio_disable(ps, "Audio: device error, audio off");
            SDL_UnlockMutex(ps->seek_mutex);
            return;
        }
        log_msg("Audio: bitstream restart failed, falling back to PCM decode");
    }

    if (ps->audio_stream && !ps->paused)
        SDL_ResumeAudioStreamDevice(ps->audio_stream);

    snprintf(ps->aud_osd, sizeof(ps->aud_osd), "Audio: %s",
        ps->aud_stream_names[new_sel]);
    ps->aud_osd_until = get_time_sec() + 2.0;
    SDL_UnlockMutex(ps->seek_mutex);
}

/* ═══════════════════════════════════════════════════════════════════
 * Bitstream Probe — sink capability detection via PipeWire
 *
 * Asks the audio server (bitstream_pw_probe_caps) which compressed
 * codecs the connected sinks advertise (iec958.codecs, ELD-derived by
 * WirePlumber server-side). Called once at startup or when the user
 * toggles audio mode; results cached in ps->bitstream_caps (re-probe
 * by clearing .probed).
 *
 * History: until 2026-08-20 this walked /proc/asound ELD files and
 * matched PCM ids against the literal "HDMI <n>" to fill alsa_device,
 * which bitstream_start then gated on — but the PipeWire backend
 * never read it, /proc/asound does not exist in the Flatpak sandbox
 * (the shipping target), and any sink whose PCM id was not literally
 * "HDMI <n>" lost passthrough (Knot audit finding 2; the P6 batch
 * that deleted the ALSA transport stopped one file short). The server
 * that enforces the codec list is now also the one asked about it.
 * ═══════════════════════════════════════════════════════════════════ */

void bitstream_probe(PlayerState *ps) {
    memset(&ps->bitstream_caps, 0, sizeof(BitstreamCaps));
    /* probed stays 0 when the server is unreachable, so the next file
     * open retries — mirrors the old no-monitor early return. */
    if (!bitstream_pw_probe_caps(&ps->bitstream_caps))
        return;
    ps->bitstream_caps.probed = 1;
}

/* ═══════════════════════════════════════════════════════════════════
 * Bitstream Output — IEC 61937 framing (spdifenc) → PipeWire
 *
 * Bypasses SDL3 audio entirely. Compressed packets from the demuxer
 * are wrapped in IEC 61937 bursts by FFmpeg's spdif muxer, then
 * written directly to the ALSA HDMI device. The TV/AVR decodes.
 *
 * Flow: audio_pq → spdifenc → spdif_buf → ALSA hw:X,Y → HDMI → sink
 *
 * Called from player_open when audio_mode != PCM and the sink
 * supports the current codec. Falls back to PCM on any failure.
 * ═══════════════════════════════════════════════════════════════════ */

#define SPDIF_MAX_BUF  65536   /* max IEC 61937 burst (TrueHD HBR=61440) */

/* spdifenc's AVIO write callback — collects the framed IEC 61937
 * burst into ps->spdif_buf for the feeder thread to hand to the
 * PipeWire backend. */
static int spdif_write_cb(void *opaque, const uint8_t *data, int len) {
    PlayerState *ps = (PlayerState *)opaque;
    if (ps->spdif_write_pos + len > SPDIF_MAX_BUF) {
        log_msg("Bitstream: spdif buffer overflow (%d + %d > %d)",
                ps->spdif_write_pos, len, SPDIF_MAX_BUF);
        return AVERROR(ENOMEM);
    }
    memcpy(ps->spdif_buf + ps->spdif_write_pos, data, len);
    ps->spdif_write_pos += len;
    return len;
}

/* ── Check if the current audio codec is supported for passthrough ── */
static int bitstream_rate_for_codec(enum AVCodecID id, int src_rate);

/* ── Platform blocklist: every IEC rate above 48k ──
 * The deck currently cannot deliver >48k IEC 61937 to the wire on ANY
 * path. Proven 2026-08-09: EAC3 2ch@192k fails via PipeWire, via mpv
 * (two server routes), and via ALSA-direct with every arm engaged
 * (verb + IEC958 control + register verified non-audio all run) —
 * sink mutes, and for EAC3 latches until HDMI reseat. TrueHD 8ch@192k
 * HBR fails silently on both backends TODAY despite having WORKED via
 * ALSA+arm on 2026-07-27 — a REGRESSION in that window (SteamOS
 * updated several times; suspect dock/DP audio path). Community
 * reports the same DD+ hole via Kodi/pavucontrol. 48k normal-layout
 * (AC3, DTS core) is the one class that reaches the sink, so AUTO
 * attempts only that; everything faster decodes to PCM like any
 * non-bitstreamable codec — no attempt, no silence, no wedge.
 * WAITING ON VALVE (SteamOS/dock). Retest after updates with
 * DSVP_HD_PASSTHROUGH=1 (one run, no rebuild); if the banner ever
 * lights, delete this gate. */
int bitstream_hd_blocked(int av_codec_id, int src_rate) {
    if (bitstream_rate_for_codec((enum AVCodecID)av_codec_id,
                                 src_rate) <= 48000)
        return 0;
    return SDL_getenv("DSVP_HD_PASSTHROUGH") == NULL;
}

static int bitstream_codec_supported(PlayerState *ps) {
    if (!ps->audio_codec_ctx) return 0;
    BitstreamCaps *caps = &ps->bitstream_caps;

    if (bitstream_hd_blocked(ps->audio_codec_ctx->codec_id,
                             ps->audio_codec_ctx->sample_rate)) {
        log_msg("Bitstream: %s → PCM decode (platform cannot carry >48k "
                "IEC — waiting on Valve; override: DSVP_HD_PASSTHROUGH=1)",
                avcodec_get_name(ps->audio_codec_ctx->codec_id));
        return 0;
    }

    switch (ps->audio_codec_ctx->codec_id) {
        case AV_CODEC_ID_AC3:     return caps->support_ac3;
        case AV_CODEC_ID_EAC3:    return caps->support_eac3;
        case AV_CODEC_ID_TRUEHD:  return caps->support_truehd;
        case AV_CODEC_ID_DTS:     return caps->support_dts || caps->support_dtshd;
        default: return 0;
    }
}

/* ── Determine IEC 61937 sample rate for codec ── */
/* IEC 61937 carrier rate. AC3/DTS ride at the stream's own sample rate;
 * EAC3 and TrueHD use a 4x HBR carrier. src_rate is the decoder's reported
 * rate — previously ignored, so a 44.1kHz DTS or AC3 track (DVD-era rips,
 * some concert discs) was carried at 48kHz and labeled 48kHz in the channel
 * status, which the receiver decodes at the wrong pitch or rejects. */
static int bitstream_rate_for_codec(enum AVCodecID id, int src_rate) {
    if (src_rate <= 0) src_rate = 48000;
    switch (id) {
        case AV_CODEC_ID_AC3:    return src_rate;
        case AV_CODEC_ID_EAC3:   return src_rate * 4;   /* 4x for IEC 61937 */
        case AV_CODEC_ID_DTS:    return src_rate;
        case AV_CODEC_ID_TRUEHD: return src_rate * 4;   /* HBR */
        default: return 48000;
    }
}


/* ── Tear down spdifenc muxer + AVIO + output buffer ──
 *
 * LEAK FIX: avio_context_free() frees ONLY the context, never the
 * buffer — the earlier comment here claiming FFmpeg 8.x frees both
 * was wrong, and each P-key bitstream cycle leaked the 64KB AVIO
 * buffer. The buffer must be freed via avio->buffer (NOT the original
 * malloc'd pointer): FFmpeg may swap the buffer internally, which is
 * also why freeing the original pointer double-freed in the past.
 * Pattern per FFmpeg's own avio_reading.c example. */
static void spdif_free(PlayerState *ps) {
    if (ps->spdif_ctx) {
        AVFormatContext *spdif = (AVFormatContext *)ps->spdif_ctx;
        av_write_trailer(spdif);
        avformat_free_context(spdif);
        ps->spdif_ctx = NULL;
    }
    if (ps->spdif_avio) {
        AVIOContext *avio = (AVIOContext *)ps->spdif_avio;
        av_freep(&avio->buffer);
        avio_context_free(&avio);
        ps->spdif_avio = NULL;
    }
    if (ps->spdif_buf) {
        av_free(ps->spdif_buf);
        ps->spdif_buf = NULL;
    }
    ps->spdif_buf_size = 0;
    ps->spdif_write_pos = 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * bitstream_start — spdifenc + PipeWire stream, launch feeder thread
 *
 * Returns 1 on success, 0 on failure (caller should fall back to PCM).
 * ═══════════════════════════════════════════════════════════════════ */


/* ═══════════════════════════════════════════════════════════════════
 * bitstream_pw_thread_func — feeder thread for the PipeWire backend
 *
 * Packets pop from audio_pq, frame through spdifenc, and block into
 * bitstream_pw's ring; the server owns the device and channel status.
 * The clock is pts minus the backend's measured buffered time (ring +
 * pw queue + server delay) minus the user latency offset.
 * ═══════════════════════════════════════════════════════════════════ */
static int bitstream_pw_thread_func(void *arg) {
    PlayerState *ps = (PlayerState *)arg;
    AVFormatContext *spdif = (AVFormatContext *)ps->spdif_ctx;
    int spdif_err_count = 0;

    /* TrueHD major-sync gating — same contract as the ALSA thread:
     * spdifenc rejects packets until a major sync frame arrives. */
    int need_truehd_sync = (ps->audio_codec_ctx &&
        ps->audio_codec_ctx->codec_id == AV_CODEC_ID_TRUEHD);
    int truehd_synced = !need_truehd_sync;
    int truehd_skipped = 0;

    log_msg("Bitstream[pw]: feeder thread started%s",
            need_truehd_sync ? " (waiting for TrueHD major sync)" : "");

    while (!ps->bitstream_quit) {
        /* ── Pause gate ──
         * set_active(false) freezes the stream with its queued bursts
         * intact; resume is burst-aligned for free (buffers were only
         * ever queued whole). No clock reset needed — buffered time
         * is measured, not modeled. */
        if (ps->paused && !ps->bitstream_quit) {
            bitstream_pw_pause(ps, 1);
            while (ps->paused && !ps->bitstream_quit)
                SDL_Delay(10);
            if (ps->bitstream_quit) break;
            bitstream_pw_pause(ps, 0);
        }

        /* ── Seek reset gate ── */
        if (ps->bitstream_seek_pending && !ps->bitstream_quit) {
            bitstream_pw_seek_reset(ps);
            if (need_truehd_sync) {
                truehd_synced = 0;
                truehd_skipped = 0;
            }
            ps->bitstream_seek_pending = 0;
        }

        /* Pop a packet — non-blocking poll, same rationale as the
         * ALSA thread (instant pause/seek/quit response). */
        AVPacket pkt;
        int ret = 0;
        while (!ps->bitstream_quit && !ps->paused && !ps->audio_stalled &&
               !ps->bitstream_seek_pending) {
            ret = pq_get(&ps->audio_pq, &pkt, 0);
            if (ret != 0) break;
            /* Demux EOF with an empty queue: whatever sits in the ring
             * below the start threshold is all the audio there is —
             * activate so the tail renders (review P2-18). */
            if (ps->eof) bitstream_pw_eof_drain(ps);
            SDL_Delay(5);
        }
        if (ps->paused || ps->audio_stalled || ps->bitstream_seek_pending) {
            /* The poll can return WITH an owned packet before the flag
             * check lands (TrueHD ~1200 pkt/s keeps re-rolling the
             * window) — unref like the quit path below or it leaks. */
            if (ret > 0) av_packet_unref(&pkt);
            /* audio_stalled has no sleep anywhere on its path (pause
             * sleeps at the loop top, seek resets instantly) — this
             * continue used to busy-spin a full core for the whole
             * duration of a video stall, exactly when the decoder
             * needs CPU to recover (review 2026-08-20 finding 7). */
            SDL_Delay(5);
            continue;
        }
        if (ret <= 0 || ps->bitstream_quit) {
            if (ret > 0) av_packet_unref(&pkt);
            break;
        }

        if (pkt.stream_index != ps->audio_stream_idx) {
            av_packet_unref(&pkt);
            continue;
        }

        if (!truehd_synced) {
            if (pkt.size >= 8 &&
                pkt.data[4] == 0xF8 && pkt.data[5] == 0x72 &&
                pkt.data[6] == 0x6F && pkt.data[7] == 0xBA) {
                truehd_synced = 1;
                log_msg("Bitstream[pw]: TrueHD major sync found "
                        "(skipped %d packets)", truehd_skipped);
            } else {
                truehd_skipped++;
                av_packet_unref(&pkt);
                continue;
            }
        }

        /* Audio clock: pts of this packet minus measured buffered time,
         * minus the user latency offset (the receiver's decode delay is
         * invisible to every clock we can read — DSVP_AUDIO_DELAY). */
        if (pkt.pts != AV_NOPTS_VALUE) {
            AVStream *as = ps->fmt_ctx->streams[ps->audio_stream_idx];
            double pts = (double)pkt.pts * av_q2d(as->time_base);
            ps->audio_clock = pts;
            double buffered = bitstream_pw_buffered(ps);
            if (buffered < 0) buffered = 0;
            if (buffered > 1.5) buffered = 1.5;
            ps->audio_clock_sync = pts - buffered - ps->audio_delay_sec;
        }

        /* Frame through spdifenc → spdif_buf */
        ps->spdif_write_pos = 0;
        pkt.stream_index = 0;
        ret = av_write_frame(spdif, &pkt);
        av_packet_unref(&pkt);

        if (ret < 0) {
            if (++spdif_err_count <= 3)
                log_msg("Bitstream[pw]: spdifenc write failed: %s",
                        av_err2str(ret));
            else if (spdif_err_count == 4)
                log_msg("Bitstream[pw]: suppressing further spdifenc errors");
            continue;
        }
        if (ps->spdif_write_pos <= 0) continue;

        if (bitstream_pw_write(ps, ps->spdif_buf, ps->spdif_write_pos) < 0) {
            /* Normal on stop: the quit flag wakes a blocked writer.
             * Anything else is the stream dying under us (undock /
             * HDMI unplug → PW_STREAM_STATE_ERROR) — flag it so the
             * main loop completes the PCM fallback; audio must never
             * just go silent (charter / review P1-4). */
            if (!ps->bitstream_quit) {
                log_msg("Bitstream[pw]: backend gone mid-write — "
                        "thread exiting (PCM fallback pending)");
                ps->bitstream_failed = 1;
            }
            break;
        }
    }

    log_msg("Bitstream[pw]: feeder thread exiting");
    return 0;
}

int bitstream_start(PlayerState *ps) {
    ps->bitstream_failed = 0;
    if (!ps->audio_codec_ctx) {
        log_msg("Bitstream: no audio codec — cannot start");
        return 0;
    }

    if (!bitstream_codec_supported(ps)) {
        log_msg("Bitstream: codec %s not supported by sink — falling back to PCM",
                avcodec_get_name(ps->audio_codec_ctx->codec_id));
        return 0;
    }

    enum AVCodecID codec_id = ps->audio_codec_ctx->codec_id;
    int rate = bitstream_rate_for_codec(codec_id,
                  ps->audio_codec_ctx->sample_rate);
    int channels = 2;  /* IEC 61937 is always stereo (except TrueHD HBR=8ch) */
    if (codec_id == AV_CODEC_ID_TRUEHD) channels = 8;

    log_msg("Bitstream: starting %s passthrough at %d Hz %dch (PipeWire)",
            avcodec_get_name(codec_id), rate, channels);

    /* ── Allocate IEC 61937 output buffer ── */
    ps->spdif_buf = (uint8_t *)av_malloc(SPDIF_MAX_BUF);
    if (!ps->spdif_buf) {
        log_msg("Bitstream: failed to allocate spdif buffer");
        return 0;
    }
    ps->spdif_buf_size = SPDIF_MAX_BUF;

    /* ── Set up spdifenc muxer with memory AVIO ── */
    const AVOutputFormat *ofmt = av_guess_format("spdif", NULL, NULL);
    if (!ofmt) {
        log_msg("Bitstream: spdif muxer not found in FFmpeg");
        av_free(ps->spdif_buf); ps->spdif_buf = NULL;
        return 0;
    }

    AVFormatContext *spdif = avformat_alloc_context();
    if (!spdif) {
        log_msg("Bitstream: failed to allocate spdifenc context");
        av_free(ps->spdif_buf); ps->spdif_buf = NULL;
        return 0;
    }
    spdif->oformat = ofmt;

    /* Custom AVIO — writes framed data to ps->spdif_buf */
    uint8_t *avio_buf = (uint8_t *)av_malloc(SPDIF_MAX_BUF);
    if (!avio_buf) {
        avformat_free_context(spdif);
        av_free(ps->spdif_buf); ps->spdif_buf = NULL;
        return 0;
    }
    AVIOContext *avio = avio_alloc_context(
        avio_buf, SPDIF_MAX_BUF, 1 /* writable */, ps,
        NULL /* no read */, spdif_write_cb, NULL /* no seek */);
    if (!avio) {
        av_free(avio_buf);
        avformat_free_context(spdif);
        av_free(ps->spdif_buf); ps->spdif_buf = NULL;
        return 0;
    }
    spdif->pb = avio;

    /* Add one stream matching the audio codec */
    AVStream *st = avformat_new_stream(spdif, NULL);
    if (!st) {
        log_msg("Bitstream: failed to create spdifenc stream");
        av_freep(&avio->buffer);
        avio_context_free(&avio);
        avformat_free_context(spdif);
        av_free(ps->spdif_buf); ps->spdif_buf = NULL;
        return 0;
    }
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id   = codec_id;
    st->codecpar->sample_rate = ps->audio_codec_ctx->sample_rate;
    /* Deep copy — struct assignment aliases u.map for CUSTOM-order
     * layouts, and both spdif_free and avcodec_free_context would
     * then uninit the same heap pointer (double free). */
    if (av_channel_layout_copy(&st->codecpar->ch_layout,
                               &ps->audio_codec_ctx->ch_layout) < 0) {
        log_msg("Bitstream: channel layout copy failed");
        av_freep(&avio->buffer);
        avio_context_free(&avio);
        avformat_free_context(spdif);
        av_free(ps->spdif_buf); ps->spdif_buf = NULL;
        return 0;
    }

    int ret = avformat_write_header(spdif, NULL);
    if (ret < 0) {
        log_msg("Bitstream: spdifenc write_header failed: %s", av_err2str(ret));
        av_freep(&avio->buffer);
        avio_context_free(&avio);
        avformat_free_context(spdif);
        av_free(ps->spdif_buf); ps->spdif_buf = NULL;
        return 0;
    }

    ps->spdif_ctx  = spdif;
    ps->spdif_avio = avio;

    /* ── PipeWire-native transport ──
     * (docs/TODO-BITSTREAM.md Track A). The server owns the device,
     * the AES bits, and the non-audio bit — no root anywhere, and
     * WirePlumber is never disturbed: failure falls back to PCM with
     * the audio stack untouched. */
    if (!bitstream_pw_open(ps, (int)codec_id, rate, channels)) {
        spdif_free(ps);
        snprintf(ps->aud_osd, sizeof(ps->aud_osd),
                 "Passthrough unavailable — using PCM");
        ps->aud_osd_until = get_time_sec() + 5.0;
        return 0;
    }
    ps->bitstream_quit = 0;
    ps->bitstream_seek_pending = 0;
    ps->audio_pq.abort_request = 0;

    /* Flush stale queue contents from the PCM era before the feeder
     * starts — wrong-payload packets must never reach spdifenc
     * (Wren's PCM→bitstream switch fix, preserved across the backend
     * change). */
    pq_flush(&ps->audio_pq);

    ps->bitstream_thread = SDL_CreateThread(bitstream_pw_thread_func,
                                            "bitstream_pw", ps);
    if (!ps->bitstream_thread) {
        log_msg("Bitstream[pw]: thread create failed");
        bitstream_pw_close(ps);
        spdif_free(ps);
        return 0;
    }
    ps->bitstream_active = 1;
    snprintf(ps->aud_osd, sizeof(ps->aud_osd),
             "Passthrough: %s (PipeWire)", avcodec_get_name(codec_id));
    ps->aud_osd_until = get_time_sec() + 3.0;
    log_msg("Bitstream[pw]: passthrough active — %s %d Hz %dch",
            avcodec_get_name(codec_id), rate, channels);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════
 * bitstream_stop — drain PipeWire stream + spdifenc, join feeder
 * ═══════════════════════════════════════════════════════════════════ */

void bitstream_stop(PlayerState *ps) {
    if (!ps->bitstream_active) return;

    log_msg("Bitstream: stopping passthrough");

    /* Signal thread to exit and wake it from pq_get block */
    ps->bitstream_quit = 1;
    ps->audio_pq.abort_request = 1;
    SDL_SignalCondition(ps->audio_pq.cond);

    if (ps->bitstream_thread) {
        SDL_WaitThread(ps->bitstream_thread, NULL);
        ps->bitstream_thread = NULL;
    }

    /* PipeWire backend teardown (Track A) — flush + ordered destroy;
     * WirePlumber was never disturbed, so there is nothing to rebuild
     * and no audio-stack restart on this path. */
    if (ps->bpw)
        bitstream_pw_close(ps);

    /* Close spdifenc + AVIO + output buffer (leak-safe — see spdif_free) */
    spdif_free(ps);

    ps->bitstream_active = 0;
    ps->bitstream_quit = 0;

    /* Reset audio clocks — bitstream thread was updating audio_clock_sync
     * via wall-clock frame counting. Without reset, the stale value persists
     * between bitstream_stop and the next seek, causing multi-second A/V
     * drift spikes that corrupt av_bias for the rest of the session. */
    ps->audio_clock = ps->video_clock;
    ps->audio_clock_sync = ps->video_clock;
    ps->av_bias = 0.0;
    ps->av_bias_samples = 0;

    /* Reset abort so audio_pq works normally for PCM fallback */
    ps->audio_pq.abort_request = 0;

    log_msg("Bitstream: passthrough stopped");
}


/* ═══════════════════════════════════════════════════════════════════
 * bitstream_stop_immediate — Fast stop for async mode switch
 *
 * Identical to bitstream_stop now that the profile bounce is gone.
 * Kept as a separate symbol because main.c's async-switch flow still
 * launches audio_switch_bg_func after this returns; collapsing the
 * two would require touching the caller too. Subtractive patch only.
 * ═══════════════════════════════════════════════════════════════════ */
void bitstream_stop_immediate(PlayerState *ps) {
    if (!ps->bitstream_active) return;

    log_msg("Bitstream: stopping passthrough (async)");

    /* Signal thread to exit and wake it from pq_get */
    ps->bitstream_quit = 1;
    ps->audio_pq.abort_request = 1;
    SDL_SignalCondition(ps->audio_pq.cond);

    if (ps->bitstream_thread) {
        SDL_WaitThread(ps->bitstream_thread, NULL);
        ps->bitstream_thread = NULL;
    }

    /* PipeWire backend teardown (Track A) — flush + ordered destroy;
     * WirePlumber was never disturbed, so there is nothing to rebuild
     * and no audio-stack restart on this path. */
    if (ps->bpw)
        bitstream_pw_close(ps);

    /* Close spdifenc + AVIO + output buffer (leak-safe — see spdif_free) */
    spdif_free(ps);

    ps->bitstream_active = 0;
    ps->bitstream_quit = 0;

    /* Reset clocks */
    ps->audio_clock = ps->video_clock;
    ps->audio_clock_sync = ps->video_clock;
    ps->av_bias = 0.0;
    ps->av_bias_samples = 0;

    /* Reset abort so audio_pq works normally for PCM */
    ps->audio_pq.abort_request = 0;

    /* NOTE: nothing else to defer — profile bounce removed entirely */
}


/* ═══════════════════════════════════════════════════════════════════
 * audio_switch_bg_func — Background thread for async mode switch
 *
 * Thin signaler. The slow parts (profile bounce, HBR settle) were
 * deleted. Sets audio_switch_phase = 2 immediately so the main loop
 * can complete the switch (audio_open + seek).
 * ═══════════════════════════════════════════════════════════════════ */

int audio_switch_bg_func(void *arg) {
    PlayerState *ps = (PlayerState *)arg;

    /* Profile bounce was deleted; nothing slow to do here.
     * Kept as a thread for caller-API compatibility — main.c launches
     * this and waits for audio_switch_phase == 2 before completing
     * the switch (audio_open + seek). Just signal completion. */

    log_msg("Bitstream: async restore complete — ready for audio_open");

    /* Signal main loop to complete the switch */
    ps->audio_switch_phase = 2;
    return 0;
}