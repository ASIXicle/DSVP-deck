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
#include <alsa/asoundlib.h>

/* ═══════════════════════════════════════════════════════════════════
 * Audio Decode
 * ═══════════════════════════════════════════════════════════════════ */

int audio_decode_frame(PlayerState *ps) {
    AVPacket pkt;
    int ret;
    int data_size;

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

            if (!ps->swr_ctx) {
                AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
                ret = swr_alloc_set_opts2(&ps->swr_ctx,
                    &out_layout, AV_SAMPLE_FMT_FLT, ps->audio_spec.freq,
                    &ps->audio_frame->ch_layout, ps->audio_frame->format,
                    ps->audio_frame->sample_rate, 0, NULL);
                if (ret < 0 || swr_init(ps->swr_ctx) < 0) {
                    log_msg("ERROR: swr init failed: %s", av_err2str(ret));
                    return -1;
                }
            }

            int out_samples = swr_get_out_samples(ps->swr_ctx, ps->audio_frame->nb_samples);
            int out_size = out_samples * 2 * 4;  /* stereo F32 = 8 bytes/frame */

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
     * runaway where SDL reports huge queued amounts during startup. */
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

void audio_cycle(PlayerState *ps) {
    if (ps->aud_count <= 1) {
        snprintf(ps->aud_osd, sizeof(ps->aud_osd),
            ps->aud_count == 0 ? "No audio tracks" : "Only one audio track");
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

/* ═══════════════════════════════════════════════════════════════════
 * Bitstream Probe — HDMI sink capability detection via ALSA ELD
 *
 * Scans /proc/asound/cardN/eld#M.X files for a connected HDMI monitor.
 * Parses Short Audio Descriptors (SADs) to determine which compressed
 * codecs the sink supports. Maps the ELD index to an ALSA PCM device
 * via /proc/asound/cardN/pcmDp/info matching.
 *
 * Called once at startup or when the user toggles audio mode.
 * Results cached in ps->bitstream_caps (re-probe by clearing .probed).
 * ═══════════════════════════════════════════════════════════════════ */

void bitstream_probe(PlayerState *ps) {
    memset(&ps->bitstream_caps, 0, sizeof(BitstreamCaps));

    /* ── Scan ELD files for a connected HDMI monitor ── */
    int card = -1, eld_idx = -1;
    char eld_path[256];

    for (int c = 0; c < 8; c++) {
        for (int i = 0; i < 16; i++) {
            snprintf(eld_path, sizeof(eld_path),
                     "/proc/asound/card%d/eld#0.%d", c, i);
            FILE *f = fopen(eld_path, "r");
            if (!f) continue;

            int present = 0, valid = 0;
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                sscanf(line, " monitor_present %d", &present);
                sscanf(line, " eld_valid %d", &valid);
            }
            fclose(f);

            if (present && valid) {
                card = c;
                eld_idx = i;
                break;
            }
        }
        if (card >= 0) break;
    }

    if (card < 0) {
        log_msg("Bitstream: no HDMI monitor detected in ELD scan");
        return;
    }

    /* ── Parse SADs from the active ELD ── */
    snprintf(eld_path, sizeof(eld_path),
             "/proc/asound/card%d/eld#0.%d", card, eld_idx);
    FILE *f = fopen(eld_path, "r");
    if (!f) return;

    char line[256];
    char monitor[128] = "";

    while (fgets(line, sizeof(line), f)) {
        sscanf(line, " monitor_name %127[^\n]", monitor);

        /* Parse SAD coding types: "sadN_coding_type  [0xHEX] ..." */
        int sad_idx, coding_type;
        if (sscanf(line, " sad%d_coding_type [0x%x]", &sad_idx, &coding_type) == 2) {
            switch (coding_type) {
                case 0x2:  ps->bitstream_caps.support_ac3    = 1; break;
                case 0x7:  ps->bitstream_caps.support_dts    = 1; break;
                case 0xa:  ps->bitstream_caps.support_eac3   = 1; break;
                case 0xb:  ps->bitstream_caps.support_dtshd  = 1; break;
                case 0xc:  ps->bitstream_caps.support_truehd = 1; break;
            }
        }

        /* Track max channel count across all SADs */
        int channels;
        if (sscanf(line, " sad%d_channels %d", &sad_idx, &channels) == 2) {
            if (channels > ps->bitstream_caps.max_channels)
                ps->bitstream_caps.max_channels = channels;
        }
    }
    fclose(f);

    /* TrueHD requires HBR — if the sink reports TrueHD, it supports HBR */
    if (ps->bitstream_caps.support_truehd)
        ps->bitstream_caps.hbr_capable = 1;

    /* ── Map ELD index → ALSA PCM device ──
     *
     * ELD index N corresponds to "HDMI N" in the ALSA PCM device info.
     * Scan /proc/asound/cardC/pcmDp/info files for a matching id line.
     */
    char search_id[32];
    snprintf(search_id, sizeof(search_id), "HDMI %d", eld_idx);

    for (int dev = 0; dev < 32; dev++) {
        char info_path[256];
        snprintf(info_path, sizeof(info_path),
                 "/proc/asound/card%d/pcm%dp/info", card, dev);
        FILE *info = fopen(info_path, "r");
        if (!info) continue;

        char info_line[256];
        while (fgets(info_line, sizeof(info_line), info)) {
            char id[64];
            if (sscanf(info_line, " id: %63[^\n]", id) == 1) {
                if (strcmp(id, search_id) == 0) {
                    snprintf(ps->bitstream_caps.alsa_device,
                             sizeof(ps->bitstream_caps.alsa_device),
                             "hw:%d,%d", card, dev);
                }
                break;  /* id is always near the top — stop reading */
            }
        }
        fclose(info);
        if (ps->bitstream_caps.alsa_device[0]) break;
    }

    ps->bitstream_caps.probed = 1;

    log_msg("Bitstream: probed %s via ELD (card%d, eld#0.%d)",
            monitor[0] ? monitor : "unknown", card, eld_idx);
    log_msg("Bitstream: AC3=%d EAC3=%d TrueHD=%d DTS=%d DTS-HD=%d "
            "HBR=%d maxch=%d device=%s",
            ps->bitstream_caps.support_ac3,
            ps->bitstream_caps.support_eac3,
            ps->bitstream_caps.support_truehd,
            ps->bitstream_caps.support_dts,
            ps->bitstream_caps.support_dtshd,
            ps->bitstream_caps.hbr_capable,
            ps->bitstream_caps.max_channels,
            ps->bitstream_caps.alsa_device[0] ? ps->bitstream_caps.alsa_device : "none");
}

/* ═══════════════════════════════════════════════════════════════════
 * Bitstream Output — IEC 61937 framing (spdifenc) + ALSA direct
 *
 * Bypasses SDL3 audio entirely. Compressed packets from the demuxer
 * are wrapped in IEC 61937 bursts by FFmpeg's spdif muxer, then
 * written directly to the ALSA HDMI device. The TV/AVR decodes.
 *
 * Flow: audio_pq → spdifenc → spdif_buf → ALSA hw:X,Y → HDMI → sink
 *
 * Called from player_open when audio_mode != PCM and the sink
 * supports the current codec. Falls back to PCM if ALSA open fails.
 * ═══════════════════════════════════════════════════════════════════ */

#define SPDIF_MAX_BUF  65536   /* max IEC 61937 burst (TrueHD HBR=61440) */

/* ── PipeWire device contention ──
 * PipeWire holds ALSA HDMI devices exclusively. For bitstream passthrough
 * we need raw ALSA access, so we temporarily stop PipeWire. This is the
 * same approach mpv/Kodi users use on Linux for audio passthrough.
 * Services are restarted in bitstream_stop or on error fallback. */

static void pipewire_stop(void) {
    log_msg("Bitstream: stopping PipeWire for exclusive ALSA access");
    system("systemctl --user stop wireplumber pipewire-pulse.socket pipewire.socket pipewire 2>/dev/null");
    SDL_Delay(100);  /* give PipeWire time to release ALSA devices */
}

static void pipewire_start(void) {
    log_msg("Bitstream: restarting PipeWire");
    system("systemctl --user start pipewire.socket pipewire-pulse.socket wireplumber 2>/dev/null");
    SDL_Delay(200);  /* give PipeWire time to reclaim audio devices */
}

/* ── IEC 60958 AES3 sample rate code ──
 * Maps ALSA sample rate to IEC 60958 channel status byte 3 value. */

static int iec958_rate_code(int rate) {
    switch (rate) {
        case 32000:  return 3;
        case 44100:  return 0;
        case 48000:  return 2;
        case 88200:  return 8;
        case 96000:  return 10;
        case 176400: return 12;
        case 192000: return 14;
        default:     return 1;  /* not indicated */
    }
}

/* ── IEC 60958 channel status for HDMI passthrough ──
 * HDMI transmitters check the channel status to distinguish PCM from
 * compressed audio. Without the non-audio bit set, the TV receives
 * IEC 61937 bursts but interprets them as PCM → static noise.
 *
 * AMD HDA HDMI driver uses MIXER interface with INDEX (not DEVICE).
 * The IEC958 controls are indexed by HDMI port order (0,1,2,3),
 * not by PCM device number (3,7,8,9 on Steam Deck). We discover
 * the correct index by enumerating "HDMI/DP,pcm=N Jack" controls. */

static void set_iec958_nonpcm(int card, int dev, int aes3) {
    char ctl_name[32];
    snprintf(ctl_name, sizeof(ctl_name), "hw:%d", card);

    snd_ctl_t *ctl;
    int err = snd_ctl_open(&ctl, ctl_name, 0);
    if (err < 0) {
        log_msg("Bitstream: IEC958 control open failed: %s", snd_strerror(err));
        return;
    }

    /* ── Find mixer index for our PCM device ──
     * Enumerate HDMI/DP Jack controls in device order.
     * The position of our device in the list = the IEC958 mixer index. */
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_value_t *val;
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_value_alloca(&val);

    int iec958_idx = -1;
    int idx_count = 0;
    for (int d = 0; d < 16; d++) {
        char jack_name[64];
        snprintf(jack_name, sizeof(jack_name), "HDMI/DP,pcm=%d Jack", d);
        snd_ctl_elem_id_clear(id);
        snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_CARD);
        snd_ctl_elem_id_set_name(id, jack_name);
        snd_ctl_elem_value_set_id(val, id);
        if (snd_ctl_elem_read(ctl, val) >= 0) {
            if (d == dev) {
                iec958_idx = idx_count;
                log_msg("Bitstream: pcm device %d → IEC958 mixer index %d", dev, iec958_idx);
            }
            idx_count++;
        }
    }

    if (iec958_idx < 0) {
        log_msg("Bitstream: could not find IEC958 index for pcm device %d", dev);
        snd_ctl_close(ctl);
        return;
    }

    /* ── Set channel status: non-audio + sample rate ── */
    snd_ctl_elem_id_clear(id);
    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(id, "IEC958 Playback Default");
    snd_ctl_elem_id_set_index(id, iec958_idx);
    snd_ctl_elem_value_set_id(val, id);

    snd_aes_iec958_t iec958;
    memset(&iec958, 0, sizeof(iec958));
    iec958.status[0] = 0x06;  /* non-audio + no copyright */
    iec958.status[1] = 0x82;  /* digital-digital converter */
    iec958.status[2] = 0x00;
    iec958.status[3] = aes3;  /* sample rate */
    snd_ctl_elem_value_set_iec958(val, &iec958);

    err = snd_ctl_elem_write(ctl, val);
    if (err < 0)
        log_msg("Bitstream: IEC958 set failed (idx=%d): %s", iec958_idx, snd_strerror(err));
    else
        log_msg("Bitstream: IEC958 non-audio set (idx=%d, AES0=0x06 AES3=0x%02x)",
                iec958_idx, aes3);

    snd_ctl_close(ctl);
}

/* ── spdifenc AVIO write callback ──
 * Called by av_write_frame when the spdif muxer has framed data.
 * Accumulates into ps->spdif_buf for subsequent ALSA writei. */

static int s_spdif_pos = 0;   /* write cursor — reset before each frame */

static int spdif_write_cb(void *opaque, const uint8_t *data, int len) {
    PlayerState *ps = (PlayerState *)opaque;
    if (s_spdif_pos + len > SPDIF_MAX_BUF) {
        log_msg("Bitstream: spdif buffer overflow (%d + %d > %d)",
                s_spdif_pos, len, SPDIF_MAX_BUF);
        return AVERROR(ENOMEM);
    }
    memcpy(ps->spdif_buf + s_spdif_pos, data, len);
    s_spdif_pos += len;
    return len;
}

/* ── Bitstream output thread ──
 * Pops compressed audio packets from audio_pq, frames them via
 * spdifenc, and writes IEC 61937 bursts to the ALSA device. */

static int bitstream_thread_func(void *arg) {
    PlayerState *ps = (PlayerState *)arg;
    snd_pcm_t *pcm = (snd_pcm_t *)ps->alsa_pcm;
    AVFormatContext *spdif = (AVFormatContext *)ps->spdif_ctx;
    int spdif_err_count = 0;

    log_msg("Bitstream: output thread started");

    while (!ps->bitstream_quit) {
        /* Pop a packet from the audio queue (blocking) */
        AVPacket pkt;
        int ret = pq_get(&ps->audio_pq, &pkt, 1);  /* blocking */
        if (ret <= 0 || ps->bitstream_quit) {
            if (ret > 0) av_packet_unref(&pkt);
            break;
        }

        /* Skip packets from wrong stream */
        if (pkt.stream_index != ps->audio_stream_idx) {
            av_packet_unref(&pkt);
            continue;
        }

        /* Update audio clock from packet PTS */
        if (pkt.pts != AV_NOPTS_VALUE) {
            AVStream *as = ps->fmt_ctx->streams[ps->audio_stream_idx];
            double pts = (double)pkt.pts * av_q2d(as->time_base);
            ps->audio_clock = pts;

            /* Estimate buffered time from ALSA delay */
            snd_pcm_sframes_t delay = 0;
            snd_pcm_delay(pcm, &delay);
            double buffered = (delay > 0)
                ? (double)delay / 48000.0   /* TODO: use actual rate */
                : 0.0;
            if (buffered > 0.1) buffered = 0.1;
            ps->audio_clock_sync = pts - buffered;
        }

        /* Frame the packet through spdifenc → spdif_buf */
        s_spdif_pos = 0;
        pkt.stream_index = 0;  /* spdifenc has one stream at index 0 */
        ret = av_write_frame(spdif, &pkt);
        av_packet_unref(&pkt);

        if (ret < 0) {
            if (++spdif_err_count <= 3)
                log_msg("Bitstream: spdifenc write failed: %s", av_err2str(ret));
            else if (spdif_err_count == 4)
                log_msg("Bitstream: suppressing further spdifenc errors");
            continue;
        }

        if (s_spdif_pos <= 0) continue;

        /* Write IEC 61937 burst to ALSA — S16LE stereo, 4 bytes/frame */
        int frames = s_spdif_pos / 4;
        const uint8_t *ptr = ps->spdif_buf;
        while (frames > 0) {
            snd_pcm_sframes_t written = snd_pcm_writei(pcm, ptr, frames);
            if (written < 0) {
                if (written == -EPIPE) {
                    /* Underrun — recover and retry */
                    snd_pcm_prepare(pcm);
                    log_msg("Bitstream: ALSA underrun — recovered");
                    continue;
                }
                log_msg("Bitstream: ALSA write error: %s",
                        snd_strerror((int)written));
                break;
            }
            frames -= (int)written;
            ptr += written * 4;
        }
    }

    log_msg("Bitstream: output thread exiting");
    return 0;
}

/* ── Check if the current audio codec is supported for passthrough ── */
static int bitstream_codec_supported(PlayerState *ps) {
    if (!ps->audio_codec_ctx) return 0;
    BitstreamCaps *caps = &ps->bitstream_caps;

    switch (ps->audio_codec_ctx->codec_id) {
        case AV_CODEC_ID_AC3:     return caps->support_ac3;
        case AV_CODEC_ID_EAC3:    return caps->support_eac3;
        case AV_CODEC_ID_TRUEHD:  return caps->support_truehd;
        case AV_CODEC_ID_DTS:     return caps->support_dts || caps->support_dtshd;
        default: return 0;
    }
}

/* ── Determine IEC 61937 sample rate for codec ── */
static int bitstream_rate_for_codec(enum AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_AC3:    return 48000;
        case AV_CODEC_ID_EAC3:   return 192000;  /* 4× for IEC 61937 */
        case AV_CODEC_ID_DTS:    return 48000;
        case AV_CODEC_ID_TRUEHD: return 192000;  /* HBR */
        default: return 48000;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * bitstream_start — Open ALSA + spdifenc, launch output thread
 *
 * Returns 1 on success, 0 on failure (caller should fall back to PCM).
 * ═══════════════════════════════════════════════════════════════════ */

int bitstream_start(PlayerState *ps) {
    if (!ps->audio_codec_ctx || !ps->bitstream_caps.alsa_device[0]) {
        log_msg("Bitstream: no codec or no ALSA device — cannot start");
        return 0;
    }

    if (!bitstream_codec_supported(ps)) {
        log_msg("Bitstream: codec %s not supported by sink — falling back to PCM",
                avcodec_get_name(ps->audio_codec_ctx->codec_id));
        return 0;
    }

    enum AVCodecID codec_id = ps->audio_codec_ctx->codec_id;
    int rate = bitstream_rate_for_codec(codec_id);
    int channels = 2;  /* IEC 61937 is always stereo (except TrueHD HBR=8ch) */
    if (codec_id == AV_CODEC_ID_TRUEHD) channels = 8;

    log_msg("Bitstream: starting %s passthrough at %d Hz %dch on %s",
            avcodec_get_name(codec_id), rate, channels,
            ps->bitstream_caps.alsa_device);

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
        avio_context_free(&avio);
        avformat_free_context(spdif);
        av_free(ps->spdif_buf); ps->spdif_buf = NULL;
        return 0;
    }
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id   = codec_id;
    st->codecpar->sample_rate = ps->audio_codec_ctx->sample_rate;
    st->codecpar->ch_layout   = ps->audio_codec_ctx->ch_layout;

    int ret = avformat_write_header(spdif, NULL);
    if (ret < 0) {
        log_msg("Bitstream: spdifenc write_header failed: %s", av_err2str(ret));
        avio_context_free(&avio);
        avformat_free_context(spdif);
        av_free(ps->spdif_buf); ps->spdif_buf = NULL;
        return 0;
    }

    ps->spdif_ctx  = spdif;
    ps->spdif_avio = avio;

    /* ── Open ALSA device for passthrough ──
     * 1. Stop PipeWire (always holds HDMI devices exclusively)
     * 2. Set IEC958 channel status via snd_ctl (non-audio bit)
     * 3. Open the raw hw device */
    int card_num = 0, dev_num = 0;
    sscanf(ps->bitstream_caps.alsa_device, "hw:%d,%d", &card_num, &dev_num);
    int aes3 = iec958_rate_code(rate);

    pipewire_stop();
    ps->pipewire_stopped = 1;

    /* Set non-audio bit BEFORE opening PCM (some drivers latch on open) */
    set_iec958_nonpcm(card_num, dev_num, aes3);

    snd_pcm_t *pcm = NULL;
    ret = snd_pcm_open(&pcm, ps->bitstream_caps.alsa_device,
                        SND_PCM_STREAM_PLAYBACK, 0);
    if (ret < 0) {
        log_msg("Bitstream: ALSA open '%s' failed: %s",
                ps->bitstream_caps.alsa_device, snd_strerror(ret));
        pipewire_start(); ps->pipewire_stopped = 0;
        av_write_trailer(spdif);
        avio_context_free(&avio);
        avformat_free_context(spdif);
        av_free(ps->spdif_buf); ps->spdif_buf = NULL;
        ps->spdif_ctx = NULL; ps->spdif_avio = NULL;
        return 0;
    }
    
    /* Set ALSA hardware parameters */
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, hw, channels);

    unsigned int actual_rate = rate;
    snd_pcm_hw_params_set_rate_near(pcm, hw, &actual_rate, NULL);
    if ((int)actual_rate != rate) {
        log_msg("Bitstream: ALSA rate %d requested, got %d", rate, actual_rate);
    }

    /* Buffer: 200ms, period: 50ms — generous for passthrough */
    snd_pcm_uframes_t buffer_size = actual_rate / 5;   /* 200ms */
    snd_pcm_uframes_t period_size = actual_rate / 20;   /* 50ms */
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer_size);
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &period_size, NULL);

    ret = snd_pcm_hw_params(pcm, hw);
    if (ret < 0) {
        log_msg("Bitstream: ALSA hw_params failed: %s", snd_strerror(ret));
        snd_pcm_close(pcm);
        if (ps->pipewire_stopped) { pipewire_start(); ps->pipewire_stopped = 0; }
        av_write_trailer(spdif);
        avio_context_free(&avio);
        avformat_free_context(spdif);
        av_free(ps->spdif_buf); ps->spdif_buf = NULL;
        ps->spdif_ctx = NULL; ps->spdif_avio = NULL;
        return 0;
    }

    snd_pcm_prepare(pcm);
    ps->alsa_pcm = pcm;

    log_msg("Bitstream: ALSA opened %s — %d Hz %dch S16LE (buf=%lu period=%lu)",
            ps->bitstream_caps.alsa_device, actual_rate, channels,
            buffer_size, period_size);

    /* ── Launch output thread ── */
    ps->bitstream_quit = 0;
    ps->bitstream_active = 1;
    ps->bitstream_thread = SDL_CreateThread(
        bitstream_thread_func, "bitstream", ps);

    if (!ps->bitstream_thread) {
        log_msg("Bitstream: failed to create thread: %s", SDL_GetError());
        snd_pcm_close(pcm);
        ps->alsa_pcm = NULL;
        ps->bitstream_active = 0;
        if (ps->pipewire_stopped) { pipewire_start(); ps->pipewire_stopped = 0; }
        av_write_trailer(spdif);
        avio_context_free(&avio);
        avformat_free_context(spdif);
        av_free(ps->spdif_buf); ps->spdif_buf = NULL;
        ps->spdif_ctx = NULL; ps->spdif_avio = NULL;
        return 0;
    }

    log_msg("Bitstream: passthrough active — %s → %s",
            avcodec_get_name(codec_id), ps->bitstream_caps.alsa_device);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════
 * bitstream_stop — Shut down ALSA + spdifenc, join output thread
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

    /* Close ALSA */
    if (ps->alsa_pcm) {
        snd_pcm_drop((snd_pcm_t *)ps->alsa_pcm);
        snd_pcm_close((snd_pcm_t *)ps->alsa_pcm);
        ps->alsa_pcm = NULL;
    }

    /* Close spdifenc */
    if (ps->spdif_ctx) {
        AVFormatContext *spdif = (AVFormatContext *)ps->spdif_ctx;
        av_write_trailer(spdif);
        avformat_free_context(spdif);
        ps->spdif_ctx = NULL;
    }
    if (ps->spdif_avio) {
        AVIOContext *avio = (AVIOContext *)ps->spdif_avio;
        av_free(avio->buffer);
        avio_context_free(&avio);
        ps->spdif_avio = NULL;
    }

    if (ps->spdif_buf) {
        av_free(ps->spdif_buf);
        ps->spdif_buf = NULL;
    }
    ps->spdif_buf_size = 0;

    ps->bitstream_active = 0;
    ps->bitstream_quit = 0;

    /* Reset abort so audio_pq works normally for PCM fallback */
    ps->audio_pq.abort_request = 0;

    /* Restart PipeWire if we stopped it for ALSA access */
    if (ps->pipewire_stopped) {
        pipewire_start();
        ps->pipewire_stopped = 0;
    }

    log_msg("Bitstream: passthrough stopped");
}