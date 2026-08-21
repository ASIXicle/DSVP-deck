/*
 * bitstream_pw.c — IEC 61937 compressed-audio passthrough over native
 * PipeWire (pw_stream with SPA iec958 encoded format).
 *
 * The plug-and-play backend (docs/TODO-BITSTREAM.md Track A): DSVP
 * declares {codec, rate} in an iec958 format pod and feeds the server
 * the IEC 61937 bursts FFmpeg's spdif muxer already produces. The
 * SERVER owns the ALSA device, the AES channel-status bits, and the
 * non-audio bit — no root helper, no udev rule, no hwdep arm. The
 * only surface used is the PipeWire client socket, which is exactly
 * what a Flatpak sandbox provides.
 *
 * Model (from the reference study of Kodi's AESinkPipewire and mpv's
 * ao_pipewire, verified against source 2026-08-09):
 *  - Sink discovery: registry enumeration of Audio/Sink nodes; a sink
 *    that can take our codec advertises it in its "iec958.codecs"
 *    node property (WirePlumber derives this from the ELD). Tracks
 *    dock/undock naturally — no /proc scanning.
 *  - Negotiation: one EnumFormat pod via spa_format_audio_iec958_build
 *    (codec + rate ONLY — channel count is implied by codec and lives
 *    in stride math), connect AUTOCONNECT|INACTIVE|MAP_BUFFERS|
 *    EXCLUSIVE. No RT_PROCESS: the process callback may take the ring
 *    lock (it runs on this file's own pw thread loop).
 *  - Delivery: feeder thread (audio.c) blocks into a byte ring; the
 *    process callback copies out up to buffer->requested frames.
 *    IEC 61937 over S/PDIF framing is a byte stream, so partial
 *    fills are legal; pw_buffer.size is kept in FRAMES so
 *    pw_time.queued stays unit-consistent (the Kodi convention).
 *  - Lifecycle: pause = set_active(false) — bursts were queued whole,
 *    resume is burst-aligned for free; seek = deactivate +
 *    flush(drain=false) + ring reset; stop = flush + stop the loop
 *    FIRST, then remove hooks, then destroy (the mpv teardown order —
 *    avoids callback races). Negotiation failure = ERROR state or a
 *    timeout waiting for PAUSED → caller falls back to PCM decode.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/iec958.h>
#include <spa/param/audio/iec958-utils.h>

#include "dsvp.h"   /* SDL3, FFmpeg, PlayerState, log_msg, get_time_sec */

#define BPW_RING_SECONDS      1.0   /* feeder headroom, like the ALSA buffer */
#define BPW_START_SECONDS     0.25  /* buffered audio before first activation */
#define BPW_PERIOD_SECONDS    0.05  /* requested pw buffer granularity        */
#define BPW_CONNECT_TIMEOUT_S 5     /* wait for PAUSED after connect          */
#define BPW_ROUNDTRIP_TIMEOUT_S 2   /* wait for registry enumeration         */
#define BPW_MAX_CANDIDATES    16    /* Audio/Sink nodes bound for inspection */

struct BitstreamPW;

/* A bound Audio/Sink node awaiting its info event. Registry globals
 * carry only a SUBSET of node properties — iec958.codecs lives in the
 * node's full info dict, so every sink must be bound and asked
 * (field-discovered 2026-08-09: the global-props shortcut saw nothing). */
typedef struct BPWCandidate {
    struct BitstreamPW *owner;
    struct pw_proxy    *proxy;
    struct spa_hook     hook;
    char                name[256];   /* node.name from the global props   */
    char                desc[256];   /* sized = name: either may feed the
                                      * other through the info fallback   */
} BPWCandidate;

typedef struct BitstreamPW {
    struct pw_thread_loop *loop;
    struct pw_context     *context;
    struct pw_core        *core;
    struct spa_hook        core_listener;
    struct pw_registry    *registry;
    struct spa_hook        registry_listener;
    struct pw_stream      *stream;
    struct spa_hook        stream_listener;

    /* Sink discovery (filled by registry callbacks under the loop lock) */
    char  codec_needle[32];    /* "\"AC3\"" — quoted token match            */
    char  target_name[256];    /* node.name of the chosen sink              */
    char  target_desc[256];    /* node.nick/description, for the log        */
    int   target_found;
    int   roundtrip_done;
    int   roundtrip_seq;
    int   core_error;
    BPWCandidate cand[BPW_MAX_CANDIDATES];
    int   n_cand;

    /* Stream state */
    enum pw_stream_state state;
    int   activated;           /* set_active(true) has been issued          */
    int   quit;                /* wakes any blocked writer                  */

    /* Byte ring (guarded by the pw thread loop lock) */
    uint8_t *ring;
    size_t   ring_size;
    size_t   ring_fill;
    size_t   ring_rpos;

    int      stride;           /* bytes per IEC frame: 4 (2ch) / 16 (8ch)   */
    int      rate;             /* IEC sample rate                           */
    int      logged_graph_rate;/* one-shot graph-clock diagnostic           */
    int64_t  activations;      /* set_active(true) count (seeks re-arm)     */
    double   last_activation_log;
    int64_t  underruns;
    double   last_underrun_log;
} BitstreamPW;

/* ── Codec mapping ─────────────────────────────────────────────────── */

static int bpw_map_codec(enum AVCodecID id, enum spa_audio_iec958_codec *spa,
                         const char **name) {
    switch (id) {
        case AV_CODEC_ID_AC3:    *spa = SPA_AUDIO_IEC958_CODEC_AC3;    *name = "AC3";    return 0;
        case AV_CODEC_ID_EAC3:   *spa = SPA_AUDIO_IEC958_CODEC_EAC3;   *name = "EAC3";   return 0;
        case AV_CODEC_ID_DTS:    *spa = SPA_AUDIO_IEC958_CODEC_DTS;    *name = "DTS";    return 0;
        case AV_CODEC_ID_TRUEHD: *spa = SPA_AUDIO_IEC958_CODEC_TRUEHD; *name = "TrueHD"; return 0;
        default: return -1;
    }
}

/* ── PipeWire callbacks (run on the loop thread, loop lock held) ───── */

static void bpw_on_core_done(void *data, uint32_t id, int seq) {
    BitstreamPW *b = (BitstreamPW *)data;
    if (id == PW_ID_CORE && seq == b->roundtrip_seq) {
        b->roundtrip_done = 1;
        pw_thread_loop_signal(b->loop, false);
    }
}

static void bpw_on_core_error(void *data, uint32_t id, int seq, int res,
                              const char *message) {
    BitstreamPW *b = (BitstreamPW *)data;
    (void)seq;
    log_msg("Bitstream[pw]: core error id=%u res=%d: %s", id, res, message);
    b->core_error = 1;
    pw_thread_loop_signal(b->loop, false);
}

static const struct pw_core_events bpw_core_events = {
    PW_VERSION_CORE_EVENTS,
    .done  = bpw_on_core_done,
    .error = bpw_on_core_error,
};

/* Node info event: the FULL property dict, where WirePlumber publishes
 * the ELD-derived codec list: iec958.codecs = ["PCM","DTS","AC3",...].
 * Quoted-token match so "DTS" can't false-hit "DTS-HD". */
static void bpw_on_node_info(void *data, const struct pw_node_info *info) {
    BPWCandidate *c = (BPWCandidate *)data;
    BitstreamPW *b = c->owner;
    if (b->target_found || !info || !info->props)
        return;
    const char *codecs = spa_dict_lookup(info->props, "iec958.codecs");
    if (!codecs || !strstr(codecs, b->codec_needle))
        return;
    const char *name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME);
    if (!name) name = c->name;
    if (!name[0])
        return;
    snprintf(b->target_name, sizeof(b->target_name), "%s", name);
    const char *nick = spa_dict_lookup(info->props, PW_KEY_NODE_NICK);
    if (!nick) nick = spa_dict_lookup(info->props, PW_KEY_NODE_DESCRIPTION);
    if (!nick) nick = c->desc[0] ? c->desc : name;
    snprintf(b->target_desc, sizeof(b->target_desc), "%s", nick);
    b->target_found = 1;
    pw_thread_loop_signal(b->loop, false);
}

static const struct pw_node_events bpw_node_events = {
    PW_VERSION_NODE_EVENTS,
    .info = bpw_on_node_info,
};

/* Registry global: bind every Audio/Sink node so its info event (full
 * props) can be inspected — registry globals only carry a subset, and
 * iec958.codecs is not in it. */
static void bpw_on_registry_global(void *data, uint32_t id,
                                   uint32_t permissions, const char *type,
                                   uint32_t version,
                                   const struct spa_dict *props) {
    BitstreamPW *b = (BitstreamPW *)data;
    (void)permissions; (void)version;
    if (b->target_found || !props || b->n_cand >= BPW_MAX_CANDIDATES)
        return;
    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
        return;
    const char *mc = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!mc || strcmp(mc, "Audio/Sink") != 0)
        return;
    BPWCandidate *c = &b->cand[b->n_cand];
    memset(c, 0, sizeof(*c));
    c->owner = b;
    const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (name)
        snprintf(c->name, sizeof(c->name), "%s", name);
    const char *nick = spa_dict_lookup(props, PW_KEY_NODE_NICK);
    if (!nick) nick = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    if (nick)
        snprintf(c->desc, sizeof(c->desc), "%s", nick);
    c->proxy = (struct pw_proxy *)pw_registry_bind(b->registry, id,
        PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
    if (!c->proxy)
        return;
    pw_node_add_listener((struct pw_node *)c->proxy, &c->hook,
                         &bpw_node_events, c);
    b->n_cand++;
}

static const struct pw_registry_events bpw_registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = bpw_on_registry_global,
};

/* Drop candidate node proxies (loop lock must be held, or loop stopped) */
static void bpw_free_candidates(BitstreamPW *b) {
    for (int i = 0; i < b->n_cand; i++) {
        if (b->cand[i].proxy) {
            spa_hook_remove(&b->cand[i].hook);
            pw_proxy_destroy(b->cand[i].proxy);
            b->cand[i].proxy = NULL;
        }
    }
    b->n_cand = 0;
}

/* Format the server ACCEPTED — the ground truth of negotiation. If
 * this ever disagrees with what we offered (codec or rate), that is
 * the smoking gun for a silent-output run. */
static void bpw_on_param_changed(void *data, uint32_t id,
                                 const struct spa_pod *param) {
    (void)data;
    if (id != SPA_PARAM_Format || !param)
        return;
    struct spa_audio_info_iec958 info;
    memset(&info, 0, sizeof(info));
    if (spa_format_audio_iec958_parse(param, &info) < 0) {
        log_msg("Bitstream[pw]: server format is NOT iec958 — "
                "stream would be treated as PCM");
        return;
    }
    static const char *names[] = { "UNKNOWN", "PCM", "DTS", "AC3", "MPEG",
                                   "MPEG2_AAC", "EAC3", "TrueHD", "DTS-HD" };
    const char *nm = (info.codec < SPA_N_ELEMENTS(names))
                   ? names[info.codec] : "?";
    log_msg("Bitstream[pw]: server accepted format: iec958 codec=%s "
            "rate=%u", nm, info.rate);
}

static void bpw_on_state_changed(void *data, enum pw_stream_state old,
                                 enum pw_stream_state state,
                                 const char *error) {
    BitstreamPW *b = (BitstreamPW *)data;
    (void)old;
    b->state = state;
    if (state == PW_STREAM_STATE_ERROR)
        log_msg("Bitstream[pw]: stream error: %s", error ? error : "unknown");
    pw_thread_loop_signal(b->loop, false);
}

/* Process: copy up to buffer->requested frames out of the ring.
 * Partial fills are legal (byte stream); an empty ring queues a
 * zero-size chunk, which the server renders as silence. */
static void bpw_on_process(void *data) {
    BitstreamPW *b = (BitstreamPW *)data;
    struct pw_buffer *pb = pw_stream_dequeue_buffer(b->stream);
    if (!pb)
        return;
    /* One-shot: the rate the GRAPH is actually running us at. If this
     * is not our IEC rate, the server is converting an encoded stream
     * — instant garbage at the sink. */
    if (!b->logged_graph_rate) {
        struct pw_time t;
        memset(&t, 0, sizeof(t));
        if (pw_stream_get_time_n(b->stream, &t, sizeof(t)) == 0 &&
            t.rate.denom != 0) {
            log_msg("Bitstream[pw]: graph clock %u/%u (stream rate %d)%s",
                    t.rate.num, t.rate.denom, b->rate,
                    ((int)t.rate.denom == b->rate && t.rate.num == 1)
                        ? "" : " — MISMATCH");
            b->logged_graph_rate = 1;
        }
    }
    struct spa_data *d = &pb->buffer->datas[0];
    uint32_t max_frames = d->maxsize / (uint32_t)b->stride;
    uint32_t want = max_frames;
    if (pb->requested > 0 && pb->requested < (uint64_t)max_frames)
        want = (uint32_t)pb->requested;

    uint32_t avail = (uint32_t)(b->ring_fill / (size_t)b->stride);
    uint32_t fill  = avail < want ? avail : want;

    uint8_t *dst = (uint8_t *)d->data;
    size_t bytes = (size_t)fill * (size_t)b->stride;
    size_t first = b->ring_size - b->ring_rpos;
    if (first > bytes) first = bytes;
    memcpy(dst, b->ring + b->ring_rpos, first);
    if (bytes > first)
        memcpy(dst + first, b->ring, bytes - first);
    b->ring_rpos = (b->ring_rpos + bytes) % b->ring_size;
    b->ring_fill -= bytes;

    d->chunk->offset = 0;
    d->chunk->stride = b->stride;
    d->chunk->size   = (uint32_t)bytes;
    pb->size = fill;                    /* FRAMES — keeps pw_time.queued sane */
    pw_stream_queue_buffer(b->stream, pb);

    if (fill < want && b->activated) {
        b->underruns++;
        double now = get_time_sec();
        if (now - b->last_underrun_log > 1.0) {
            log_msg("Bitstream[pw]: underrun (%u/%u frames, total %lld)",
                    fill, want, (long long)b->underruns);
            b->last_underrun_log = now;
        }
    }
    /* Wake a writer blocked on a full ring */
    pw_thread_loop_signal(b->loop, false);
}

static const struct pw_stream_events bpw_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = bpw_on_state_changed,
    .param_changed = bpw_on_param_changed,
    .process       = bpw_on_process,
};

/* ── Teardown (shared by failure paths and close) ─────────────────── */

static void bpw_destroy(BitstreamPW *b) {
    if (!b)
        return;
    if (b->loop) {
        /* Wake any blocked writer, drop pending data. */
        pw_thread_loop_lock(b->loop);
        b->quit = 1;
        if (b->stream)
            pw_stream_flush(b->stream, false);
        pw_thread_loop_signal(b->loop, false);
        pw_thread_loop_unlock(b->loop);
        /* mpv teardown order: stop the loop FIRST so no callback can
         * race the destroys below. */
        pw_thread_loop_stop(b->loop);
    }
    if (b->stream) {
        spa_hook_remove(&b->stream_listener);
        pw_stream_destroy(b->stream);
        b->stream = NULL;
    }
    bpw_free_candidates(b);   /* loop is stopped — proxy destroys are safe */
    if (b->registry) {
        spa_hook_remove(&b->registry_listener);
        pw_proxy_destroy((struct pw_proxy *)b->registry);
        b->registry = NULL;
    }
    if (b->core) {
        spa_hook_remove(&b->core_listener);
        pw_core_disconnect(b->core);
        b->core = NULL;
    }
    if (b->context) {
        pw_context_destroy(b->context);
        b->context = NULL;
    }
    if (b->loop) {
        pw_thread_loop_destroy(b->loop);
        b->loop = NULL;
    }
    free(b->ring);
    free(b);
}

static void bpw_ensure_init(void) {
    static int pw_inited = 0;
    if (!pw_inited) {
        pw_init(NULL, NULL);
        pw_inited = 1;
    }
}

/* ── Capability probe (replaces the /proc/asound ELD scan) ──────────
 *
 * TODO-BITSTREAM P1/P6: capability questions are answered by the
 * AUDIO SERVER, not by walking /proc — a Flatpak sandbox gets exactly
 * one audio surface (the PipeWire socket) and no /proc/asound.
 * WirePlumber publishes each sink's ELD-derived codec list as
 * iec958.codecs in the node's FULL prop dict (info event; registry
 * globals carry only a subset — same field lesson as the open path,
 * 2026-08-09). We take the UNION across Audio/Sink nodes because the
 * caps cache answers pre-decision questions (e.g. TrueHD track
 * selection) before any stream exists to target one sink; the open
 * path then targets a specific sink and negotiation stays the
 * authority. Quoted-token match so "DTS" cannot false-hit "DTS-HD" —
 * needle names must stay identical to bpw_map_codec's. */

typedef struct BPWProbeNode {
    struct BPWProbe *owner;
    struct pw_proxy *proxy;
    struct spa_hook  hook;
} BPWProbeNode;

typedef struct BPWProbe {
    struct pw_thread_loop *loop;
    struct pw_context     *context;
    struct pw_core        *core;
    struct spa_hook        core_listener;
    struct pw_registry    *registry;
    struct spa_hook        registry_listener;
    int   roundtrip_done;
    int   roundtrip_seq;
    int   core_error;
    BPWProbeNode nodes[BPW_MAX_CANDIDATES];
    int   n_nodes;
    int   n_with_codecs;      /* sinks that advertised iec958.codecs   */
    char  codecs_union[512];  /* concatenated iec958.codecs values     */
} BPWProbe;

static void probe_on_core_done(void *data, uint32_t id, int seq) {
    BPWProbe *p = (BPWProbe *)data;
    if (id == PW_ID_CORE && seq == p->roundtrip_seq) {
        p->roundtrip_done = 1;
        pw_thread_loop_signal(p->loop, false);
    }
}

static void probe_on_core_error(void *data, uint32_t id, int seq, int res,
                                const char *message) {
    BPWProbe *p = (BPWProbe *)data;
    (void)seq;
    log_msg("Bitstream[pw]: probe core error id=%u res=%d: %s",
            id, res, message);
    p->core_error = 1;
    pw_thread_loop_signal(p->loop, false);
}

static const struct pw_core_events probe_core_events = {
    PW_VERSION_CORE_EVENTS,
    .done  = probe_on_core_done,
    .error = probe_on_core_error,
};

static void probe_on_node_info(void *data, const struct pw_node_info *info) {
    BPWProbeNode *n = (BPWProbeNode *)data;
    BPWProbe *p = n->owner;
    if (!info || !info->props)
        return;
    const char *codecs = spa_dict_lookup(info->props, "iec958.codecs");
    if (!codecs)
        return;
    p->n_with_codecs++;
    size_t len = strlen(p->codecs_union);
    if (len < sizeof(p->codecs_union) - 2)
        snprintf(p->codecs_union + len, sizeof(p->codecs_union) - len,
                 "%s ", codecs);
}

static const struct pw_node_events probe_node_events = {
    PW_VERSION_NODE_EVENTS,
    .info = probe_on_node_info,
};

static void probe_on_registry_global(void *data, uint32_t id,
                                     uint32_t permissions, const char *type,
                                     uint32_t version,
                                     const struct spa_dict *props) {
    BPWProbe *p = (BPWProbe *)data;
    (void)permissions; (void)version;
    if (!props || p->n_nodes >= BPW_MAX_CANDIDATES)
        return;
    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
        return;
    const char *mc = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!mc || strcmp(mc, "Audio/Sink") != 0)
        return;
    BPWProbeNode *n = &p->nodes[p->n_nodes];
    memset(n, 0, sizeof(*n));
    n->owner = p;
    n->proxy = (struct pw_proxy *)pw_registry_bind(p->registry, id,
        PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
    if (!n->proxy)
        return;
    pw_node_add_listener((struct pw_node *)n->proxy, &n->hook,
                         &probe_node_events, n);
    p->n_nodes++;
}

static const struct pw_registry_events probe_registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = probe_on_registry_global,
};

static void probe_destroy(BPWProbe *p) {
    if (p->loop)
        pw_thread_loop_stop(p->loop);
    for (int i = 0; i < p->n_nodes; i++) {
        if (p->nodes[i].proxy) {
            spa_hook_remove(&p->nodes[i].hook);
            pw_proxy_destroy(p->nodes[i].proxy);
        }
    }
    if (p->registry) {
        spa_hook_remove(&p->registry_listener);
        pw_proxy_destroy((struct pw_proxy *)p->registry);
    }
    if (p->core) {
        spa_hook_remove(&p->core_listener);
        pw_core_disconnect(p->core);
    }
    if (p->context)
        pw_context_destroy(p->context);
    if (p->loop)
        pw_thread_loop_destroy(p->loop);
    free(p);
}

int bitstream_pw_probe_caps(BitstreamCaps *caps) {
    bpw_ensure_init();

    BPWProbe *p = (BPWProbe *)calloc(1, sizeof(*p));
    if (!p)
        return 0;

    p->loop = pw_thread_loop_new("dsvp-bpw-probe", NULL);
    if (!p->loop) { free(p); return 0; }
    if (pw_thread_loop_start(p->loop) < 0) {
        pw_thread_loop_destroy(p->loop);
        free(p);
        return 0;
    }

    pw_thread_loop_lock(p->loop);
    p->context = pw_context_new(pw_thread_loop_get_loop(p->loop), NULL, 0);
    p->core = p->context ? pw_context_connect(p->context, NULL, 0) : NULL;
    if (!p->core) {
        log_msg("Bitstream[pw]: probe cannot connect to PipeWire");
        pw_thread_loop_unlock(p->loop);
        probe_destroy(p);
        return 0;
    }
    pw_core_add_listener(p->core, &p->core_listener, &probe_core_events, p);
    p->registry = pw_core_get_registry(p->core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(p->registry, &p->registry_listener,
                             &probe_registry_events, p);

    /* Two roundtrips, same shape as the open path: globals first
     * (sinks get bound), then the bound nodes' info events. NO early
     * exit — the union wants every sink heard from. */
    for (int rt = 0; rt < 2 && !p->core_error; rt++) {
        p->roundtrip_done = 0;
        p->roundtrip_seq = pw_core_sync(p->core, PW_ID_CORE, 0);
        while (!p->roundtrip_done && !p->core_error) {
            if (pw_thread_loop_timed_wait(p->loop,
                                          BPW_ROUNDTRIP_TIMEOUT_S) != 0)
                break;
        }
    }
    int ok = !p->core_error;
    int sinks = p->n_nodes, advertised = p->n_with_codecs;

    caps->support_ac3    = strstr(p->codecs_union, "\"AC3\"")    != NULL;
    caps->support_eac3   = strstr(p->codecs_union, "\"EAC3\"")   != NULL;
    caps->support_truehd = strstr(p->codecs_union, "\"TrueHD\"") != NULL;
    caps->support_dts    = strstr(p->codecs_union, "\"DTS\"")    != NULL;
    caps->support_dtshd  = strstr(p->codecs_union, "\"DTS-HD\"") != NULL;

    pw_thread_loop_unlock(p->loop);
    probe_destroy(p);

    log_msg("Bitstream: probed via PipeWire (%d sinks, %d advertising "
            "iec958.codecs): AC3=%d EAC3=%d TrueHD=%d DTS=%d DTS-HD=%d",
            sinks, advertised,
            caps->support_ac3, caps->support_eac3, caps->support_truehd,
            caps->support_dts, caps->support_dtshd);
    return ok;
}

/* ── Public API (called from audio.c) ─────────────────────────────── */

int bitstream_pw_open(PlayerState *ps, int av_codec_id, int rate,
                      int channels) {
    enum spa_audio_iec958_codec spa_codec;
    const char *codec_name;
    if (bpw_map_codec((enum AVCodecID)av_codec_id, &spa_codec, &codec_name)) {
        log_msg("Bitstream[pw]: codec %d not mappable to iec958", av_codec_id);
        return 0;
    }

    bpw_ensure_init();

    BitstreamPW *b = (BitstreamPW *)calloc(1, sizeof(*b));
    if (!b)
        return 0;
    b->stride = channels * 2;   /* S16 IEC frames */
    b->rate   = rate;
    b->state  = PW_STREAM_STATE_UNCONNECTED;
    snprintf(b->codec_needle, sizeof(b->codec_needle), "\"%s\"", codec_name);

    b->ring_size = (size_t)((double)rate * BPW_RING_SECONDS) * (size_t)b->stride;
    b->ring = (uint8_t *)malloc(b->ring_size);
    if (!b->ring) {
        free(b);
        return 0;
    }

    b->loop = pw_thread_loop_new("dsvp-bitstream-pw", NULL);
    if (!b->loop) {
        log_msg("Bitstream[pw]: pw_thread_loop_new failed");
        free(b->ring); free(b);
        return 0;
    }
    if (pw_thread_loop_start(b->loop) < 0) {
        log_msg("Bitstream[pw]: pw_thread_loop_start failed");
        pw_thread_loop_destroy(b->loop);
        free(b->ring); free(b);
        return 0;
    }

    pw_thread_loop_lock(b->loop);

    b->context = pw_context_new(pw_thread_loop_get_loop(b->loop), NULL, 0);
    b->core = b->context ? pw_context_connect(b->context, NULL, 0) : NULL;
    if (!b->core) {
        log_msg("Bitstream[pw]: cannot connect to PipeWire — PCM fallback");
        pw_thread_loop_unlock(b->loop);
        bpw_destroy(b);
        return 0;
    }
    pw_core_add_listener(b->core, &b->core_listener, &bpw_core_events, b);

    /* ── Sink discovery: does any Audio/Sink advertise our codec? ──
     * Two roundtrips: the first delivers the registry globals (each
     * Audio/Sink gets bound as a candidate), the second flushes the
     * bound nodes' info events, which carry the full prop dicts where
     * iec958.codecs actually lives. */
    b->registry = pw_core_get_registry(b->core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(b->registry, &b->registry_listener,
                             &bpw_registry_events, b);
    for (int rt = 0; rt < 2 && !b->target_found && !b->core_error; rt++) {
        b->roundtrip_done = 0;
        b->roundtrip_seq = pw_core_sync(b->core, PW_ID_CORE, 0);
        while (!b->roundtrip_done && !b->core_error && !b->target_found) {
            if (pw_thread_loop_timed_wait(b->loop,
                                          BPW_ROUNDTRIP_TIMEOUT_S) != 0)
                break;
        }
    }
    int sinks_inspected = b->n_cand;
    bpw_free_candidates(b);
    if (b->target_found) {
        log_msg("Bitstream[pw]: sink '%s' (%s) advertises %s",
                b->target_desc, b->target_name, codec_name);
    } else {
        /* Discovery found nothing — offer the format to the default
         * sink anyway (mpv's model) and let negotiation be the
         * authority. On a dock that can take the codec this still
         * lights up; on speakers it refuses and we land in PCM. */
        log_msg("Bitstream[pw]: no sink advertised %s in node props "
                "(%d sinks inspected) — offering format to default sink",
                codec_name, sinks_inspected);
    }

    /* ── Stream ── */
    int period_frames = (int)((double)rate * BPW_PERIOD_SECONDS);
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE,     "Movie",
        PW_KEY_APP_NAME,       "DSVP",
        PW_KEY_NODE_NAME,      "dsvp-bitstream",
        NULL);
    if (b->target_found)
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, b->target_name);
    /* Ask the graph to run at the IEC rate — no resampling of an
     * encoded stream is survivable, this is load-bearing. */
    pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%d", rate);
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d",
                       period_frames, rate);

    b->stream = pw_stream_new(b->core, "DSVP passthrough", props);
    if (!b->stream) {
        log_msg("Bitstream[pw]: pw_stream_new failed");
        pw_thread_loop_unlock(b->loop);
        bpw_destroy(b);
        return 0;
    }
    pw_stream_add_listener(b->stream, &b->stream_listener,
                           &bpw_stream_events, b);

    uint8_t podbuf[1024];
    struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(podbuf, sizeof(podbuf));
    struct spa_audio_info_iec958 info;
    memset(&info, 0, sizeof(info));
    info.codec = spa_codec;
    info.rate  = (uint32_t)rate;   /* never 0 — build() would omit it and
                                    * a server-defaulted rate plays HBR at
                                    * 4x-wrong speed */
    const struct spa_pod *params[2];
    params[0] = spa_format_audio_iec958_build(&pb, SPA_PARAM_EnumFormat,
                                              &info);
    params[1] = (const struct spa_pod *)spa_pod_builder_add_object(&pb,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
            period_frames * b->stride, period_frames * b->stride, INT32_MAX),
        SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(b->stride));

    int ret = pw_stream_connect(b->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT |
                               PW_STREAM_FLAG_INACTIVE    |
                               PW_STREAM_FLAG_MAP_BUFFERS |
                               PW_STREAM_FLAG_EXCLUSIVE),
        params, 2);
    if (ret < 0) {
        log_msg("Bitstream[pw]: pw_stream_connect failed: %d", ret);
        pw_thread_loop_unlock(b->loop);
        bpw_destroy(b);
        return 0;
    }

    /* Wait for negotiation: INACTIVE stream lands in PAUSED on success,
     * ERROR on refusal. Timeout = negotiation failed some third way. */
    double deadline = get_time_sec() + BPW_CONNECT_TIMEOUT_S;
    while (b->state != PW_STREAM_STATE_PAUSED &&
           b->state != PW_STREAM_STATE_ERROR &&
           get_time_sec() < deadline) {
        if (pw_thread_loop_timed_wait(b->loop, 1) != 0)
            continue;
    }
    if (b->state != PW_STREAM_STATE_PAUSED) {
        log_msg("Bitstream[pw]: negotiation failed (state=%s) — PCM fallback",
                pw_stream_state_as_string(b->state));
        pw_thread_loop_unlock(b->loop);
        bpw_destroy(b);
        return 0;
    }

    pw_thread_loop_unlock(b->loop);

    log_msg("Bitstream[pw]: %s @ %d Hz negotiated (%dch stride, period %d "
            "frames) — server owns channel status", codec_name, rate,
            channels, period_frames);
    ps->bpw = b;
    return 1;
}

/* Blocking write of framed IEC bytes into the ring. Activates the
 * stream once the start threshold is buffered. Returns 0 on success,
 * -1 if the backend is quitting/gone. */
int bitstream_pw_write(PlayerState *ps, const uint8_t *data, int len) {
    BitstreamPW *b = (BitstreamPW *)ps->bpw;
    if (!b || len <= 0)
        return b ? 0 : -1;

    size_t start_bytes = (size_t)((double)b->rate * BPW_START_SECONDS)
                       * (size_t)b->stride;

    pw_thread_loop_lock(b->loop);
    size_t off = 0;
    /* Also watch the player's quit flag: a stop while the stream is
     * paused/inactive would otherwise leave this writer blocked on a
     * full ring until close — which runs only after the thread join
     * that's waiting on us. The 1s timed_wait bounds the latency. */
    while (off < (size_t)len && !b->quit && !ps->bitstream_quit &&
           b->state != PW_STREAM_STATE_ERROR &&
           b->state != PW_STREAM_STATE_UNCONNECTED) {
        size_t space = b->ring_size - b->ring_fill;
        if (space == 0) {
            /* Ring full: if we're not consuming yet, that's the cue to
             * start; otherwise wait for the process callback. */
            if (!b->activated) {
                pw_stream_set_active(b->stream, true);
                b->activated = 1;
                b->activations++;
                log_msg("Bitstream[pw]: activated (ring full)");
                continue;
            }
            pw_thread_loop_timed_wait(b->loop, 1);
            continue;
        }
        size_t chunk = (size_t)len - off;
        if (chunk > space) chunk = space;
        size_t wpos = (b->ring_rpos + b->ring_fill) % b->ring_size;
        size_t first = b->ring_size - wpos;
        if (first > chunk) first = chunk;
        memcpy(b->ring + wpos, data + off, first);
        if (chunk > first)
            memcpy(b->ring, data + off + first, chunk - first);
        b->ring_fill += chunk;
        off += chunk;
    }
    if (!b->activated && b->ring_fill >= start_bytes) {
        pw_stream_set_active(b->stream, true);
        b->activated = 1;
        b->activations++;
        /* Every seek re-arms activation — a held arrow key wrote 38
         * identical lines in the P4 torture. First activation always
         * logs; re-activations throttle to one per 2s, counted at
         * close. */
        double now = get_time_sec();
        if (b->activations == 1 || now - b->last_activation_log > 2.0) {
            log_msg("Bitstream[pw]: activated (%.0f ms buffered)%s",
                    1000.0 * (double)b->ring_fill
                           / (double)(b->rate * b->stride),
                    b->activations > 1 ? " [re-arm after seek]" : "");
            b->last_activation_log = now;
        }
    }
    /* Post-open the stream lives in PAUSED/STREAMING; ERROR or
     * UNCONNECTED here means the server side died (undock, HDMI
     * unplug). Report it as a hard failure so the feeder triggers
     * the PCM fallback instead of buffering into a dead ring
     * forever (review P1-4). The state callback signals the loop,
     * so a writer blocked in timed_wait wakes promptly. */
    int dead = b->quit || ps->bitstream_quit ||
               b->state == PW_STREAM_STATE_ERROR ||
               b->state == PW_STREAM_STATE_UNCONNECTED;
    pw_thread_loop_unlock(b->loop);
    return dead ? -1 : 0;
}

/* EOF tail: no more writes are coming and the ring holds less than
 * the start threshold — activate so the remainder renders instead of
 * being discarded at stop (review P2-18). Idempotent; called from the
 * feeder's empty-queue poll once demux EOF is set. */
void bitstream_pw_eof_drain(PlayerState *ps) {
    BitstreamPW *b = (BitstreamPW *)ps->bpw;
    if (!b) return;
    pw_thread_loop_lock(b->loop);
    if (b->stream && !b->activated && b->ring_fill > 0) {
        pw_stream_set_active(b->stream, true);
        b->activated = 1;
        b->activations++;
        log_msg("Bitstream[pw]: activated (EOF drain, %zu bytes buffered)",
                b->ring_fill);
    }
    pw_thread_loop_unlock(b->loop);
}

/* Seconds of audio between "written into the ring" and "heard":
 * ring fill + frames queued in pw buffers + the server's own
 * delay/buffered estimate (rate ticks, per the Kodi formula). */
double bitstream_pw_buffered(PlayerState *ps) {
    BitstreamPW *b = (BitstreamPW *)ps->bpw;
    if (!b)
        return 0.0;
    pw_thread_loop_lock(b->loop);
    double sec = (double)b->ring_fill / (double)(b->rate * b->stride);
    struct pw_time t;
    memset(&t, 0, sizeof(t));
    if (b->stream && pw_stream_get_time_n(b->stream, &t, sizeof(t)) == 0) {
        double num = t.rate.num ? (double)t.rate.num : 1.0;
        double den = t.rate.denom ? (double)t.rate.denom : (double)b->rate;
        sec += ((double)t.delay + (double)t.buffered) * num / den;
        sec += (double)t.queued / (double)b->rate;   /* queued is FRAMES */
    }
    pw_thread_loop_unlock(b->loop);
    return sec;
}

void bitstream_pw_pause(PlayerState *ps, int paused) {
    BitstreamPW *b = (BitstreamPW *)ps->bpw;
    if (!b)
        return;
    pw_thread_loop_lock(b->loop);
    if (b->activated && b->stream)
        pw_stream_set_active(b->stream, !paused);
    pw_thread_loop_unlock(b->loop);
}

/* Seek: deactivate, drop everything in flight (server + ring), and
 * let the feeder refill; the next write re-activates at threshold. */
void bitstream_pw_seek_reset(PlayerState *ps) {
    BitstreamPW *b = (BitstreamPW *)ps->bpw;
    if (!b)
        return;
    pw_thread_loop_lock(b->loop);
    if (b->stream) {
        if (b->activated) {
            pw_stream_set_active(b->stream, false);
            b->activated = 0;
        }
        pw_stream_flush(b->stream, false);
    }
    b->ring_fill = 0;
    b->ring_rpos = 0;
    pw_thread_loop_signal(b->loop, false);
    pw_thread_loop_unlock(b->loop);
}

void bitstream_pw_close(PlayerState *ps) {
    BitstreamPW *b = (BitstreamPW *)ps->bpw;
    if (!b)
        return;
    log_msg("Bitstream[pw]: closing (activations: %lld, underruns: %lld)",
            (long long)b->activations, (long long)b->underruns);
    bpw_destroy(b);
    ps->bpw = NULL;
}
