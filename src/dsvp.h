/*
 * DSVP — Dead Simple Video Player
 * dsvp.h — Shared types, constants, and declarations
 *
 * This header defines the central PlayerState and all supporting structures.
 * Every source file includes this.
 */

#ifndef DSVP_H
#define DSVP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

/* FFmpeg libraries */
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

/* SDL3 — SDL_MAIN_HANDLED prevents SDL from injecting WinMain */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>

/* SDL3 shadercross — runtime HLSL→SPIRV→native compilation */
#include <SDL3_shadercross/SDL_shadercross.h>

/* ── Constants ──────────────────────────────────────────────────────── */

#define DSVP_VERSION        "0.1.4-beta"
#define DSVP_WINDOW_TITLE   "DSVP"

#define PACKET_QUEUE_MAX    256     /* max packets buffered per stream  */
#define AUDIO_BUF_SIZE      192000  /* max decoded audio buffer bytes   */
#define SEEK_STEP_SEC       5.0     /* arrow key seek increment         */
#define VOLUME_STEP         0.05    /* arrow key volume increment       */

#define MAX_SUB_STREAMS     16      /* max subtitle tracks to catalog   */
#define MAX_AUDIO_STREAMS   16      /* max audio tracks to catalog      */
#define SUB_TEXT_SIZE       4096    /* max subtitle text buffer         */
#define MAX_SUB_BITMAPS     4       /* max bitmap rects per subtitle    */

/* Default window size when no video is loaded */
#define DEFAULT_WIN_W       960
#define DEFAULT_WIN_H       540

/* ── Packet Queue ───────────────────────────────────────────────────
 *
 * Thread-safe FIFO queue for AVPackets. The demux thread pushes packets,
 * and the video/audio decode paths pop them. Uses SDL mutex + condvar
 * because SDL's threading is cross-platform (no need for pthreads).
 */

typedef struct PacketNode {
    AVPacket           *pkt;
    struct PacketNode  *next;
} PacketNode;

typedef struct PacketQueue {
    PacketNode  *first;
    PacketNode  *last;
    int          nb_packets;
    int          size;          /* total byte size of queued packet data */
    SDL_Mutex   *mutex;
    SDL_Condition *cond;
    int          abort_request; /* signal threads to stop blocking      */
} PacketQueue;

/* ── GPU Uniform Data ──────────────────────────────────────────────
 *
 * Pushed to the fragment shader each frame via SDL_PushGPUFragmentUniformData.
 * Layout must match the HLSL cbuffer exactly (std140-ish packing).
 */

typedef struct GPUUniforms {
    float colorMatrix[16];  /* 4×4 YUV→RGB matrix (row-major)   64 bytes */
    float rangeY[2];        /* { offset, scale } for Y plane      8 bytes */
    float rangeUV[2];       /* { offset, scale } for UV planes    8 bytes */
    float texSizeY[2];      /* { width, height } of Y texture     8 bytes */
    float texSizeUV[2];     /* { width, height } of UV textures   8 bytes */
} GPUUniforms;              /*                                   96 bytes */

/* ── Player State ───────────────────────────────────────────────────
 *
 * Central structure holding everything: format/codec contexts, queues,
 * SDL handles, clocks, and UI state. One instance per playback session.
 */

typedef struct PlayerState {
    /* ── Format / streams ── */
    AVFormatContext    *fmt_ctx;
    int                 video_stream_idx;
    int                 audio_stream_idx;

    /* ── Video decode ── */
    AVCodecContext     *video_codec_ctx;
    struct SwsContext  *sws_ctx;
    AVFrame            *video_frame;      /* raw decoded frame          */
    AVFrame            *rgb_frame;        /* scaled/converted for SDL   */
    uint8_t            *rgb_buffer;       /* backing buffer for rgb_frame */

    /* ── Audio decode ── */
    AVCodecContext     *audio_codec_ctx;
    struct SwrContext  *swr_ctx;
    AVFrame            *audio_frame;
    uint8_t            *audio_buf;        /* resampled audio buffer     */
    unsigned int        audio_buf_size;   /* bytes of valid data in buf */
    unsigned int        audio_buf_index;  /* read cursor into buf       */

    /* ── Packet queues ── */
    PacketQueue         video_pq;
    PacketQueue         audio_pq;

    /* ── Audio stream catalog ── */
    int                 aud_stream_indices[MAX_AUDIO_STREAMS];
    char                aud_stream_names[MAX_AUDIO_STREAMS][128];
    int                 aud_count;          /* number of audio streams      */
    int                 aud_selection;      /* 0-based index into catalog   */

    /* Audio track change OSD */
    char                aud_osd[256];
    double              aud_osd_until;

    /* ── SDL handles ── */
    SDL_Window         *window;
    SDL_AudioStream    *audio_stream;    /* SDL3: owns the device        */
    SDL_AudioSpec       audio_spec;       /* actual device spec           */

    /* ── SDL_GPU handles (lifetime: application) ── */
    SDL_GPUDevice              *gpu_device;
    SDL_GPUGraphicsPipeline    *gpu_pipeline_yuv;   /* planar YUV420P   */
    SDL_GPUSampler             *gpu_sampler;         /* linear filtering */
    SDL_GPUSampler             *gpu_sampler_nearest; /* nearest for overlay */

    /* ── SDL_GPU handles (lifetime: per-file, created/destroyed in player_open/close) ── */
    SDL_GPUTexture             *gpu_tex_y;           /* Y plane          */
    SDL_GPUTexture             *gpu_tex_u;           /* U plane          */
    SDL_GPUTexture             *gpu_tex_v;           /* V plane          */
    SDL_GPUTexture         *gpu_tex_noise;           /* 64×64 blue noise dither (app lifetime) */
    SDL_GPUTransferBuffer      *gpu_xfer_y;          /* CPU→GPU staging  */
    SDL_GPUTransferBuffer      *gpu_xfer_u;
    SDL_GPUTransferBuffer      *gpu_xfer_v;
    GPUUniforms                 gpu_uniforms;         /* current color params */

    /* ── Overlay GPU handles (lifetime: application, resized as needed) ── */
    SDL_GPUGraphicsPipeline    *gpu_pipeline_overlay; /* RGBA + alpha blend */
    SDL_GPUTexture             *gpu_overlay_tex;      /* RGBA8888 overlay  */
    SDL_GPUTransferBuffer      *gpu_overlay_xfer;     /* CPU→GPU staging   */
    int                         overlay_tex_w;         /* current texture dimensions */
    int                         overlay_tex_h;
    int                         overlay_dirty;         /* 1 = need re-upload */

    /* ── Timing / A/V sync ── */
    double              audio_clock;      /* current audio PTS in secs (audio thread internal) */
    double              audio_clock_sync; /* latency-corrected snapshot for main thread A/V sync */
    double              av_bias;          /* adaptive A/V offset (EMA of av_diff) */
    int                 av_bias_samples;  /* warmup counter (apply after 60)     */
    double              video_clock;      /* current video PTS in secs  */
    double              frame_timer;      /* when we last showed a frame*/
    double              frame_last_delay; /* last frame display duration*/
    double              frame_last_pts;   /* PTS of last displayed frame*/
    int64_t             seek_target;      /* seek target in AV_TIME_BASE*/
    int                 seek_request;     /* 1 = seek pending           */
    int                 seek_flags;
    int                 seek_recovering;  /* 1 = waiting for first displayed frame post-seek */

    /* ── Threads ── */
    SDL_Thread         *demux_thread;
    SDL_Mutex          *seek_mutex;    /* protects codec flush vs decode  */
    int                 seeking;       /* 1 = flush in progress, skip decode */

    /* ── Playback state ── */
    int                 playing;          /* 1 = file is loaded/playing */
    int                 paused;
    int                 quit;             /* 1 = application exiting    */
    double              volume;           /* 0.0 — 1.0                  */
    int                 fullscreen;
    int                 eof;              /* demuxer hit end of file    */
    int                 video_ready;      /* 1 after first frame uploaded — gates reblit */

    /* ── Window geometry ── */
    int                 win_w, win_h;     /* current window size        */
    int                 vid_w, vid_h;     /* video native resolution    */
    SDL_Rect            display_rect;     /* letterboxed video area     */
    int                 sc_w, sc_h;       /* last swapchain dims (physical pixels) */

    /* ── Overlay visibility state ── */
    int                 show_debug;
    int                 show_info;
    int                 show_seekbar;         /* 1 = seek bar visible       */
    double              seekbar_hide_time;    /* auto-hide after this time  */
    int                 overlay_active;       /* 1 = overlay has content    */

    /* ── Subtitles ── */
    int                 sub_stream_indices[MAX_SUB_STREAMS];
    char                sub_stream_names[MAX_SUB_STREAMS][128];
    int                 sub_count;          /* number of subtitle streams   */
    int                 sub_selection;      /* user selection: 0=off, 1..N  */
    int                 sub_active_idx;     /* AVStream index or -1         */
    AVCodecContext     *sub_codec_ctx;
    PacketQueue         sub_pqs[MAX_SUB_STREAMS]; /* one queue per stream */

    /* Current subtitle display */
    char                sub_text[SUB_TEXT_SIZE];
    double              sub_start_pts;      /* show from this PTS           */
    double              sub_end_pts;        /* hide after this PTS          */
    int                 sub_valid;          /* 1 = sub_text should display  */
    int                 sub_is_bitmap;      /* 1 = bitmap sub, 0 = text     */

    /* Bitmap subtitle pixel data (RGBA, freed via av_free) */
    uint8_t            *sub_bitmap_data[MAX_SUB_BITMAPS];
    int                 sub_bitmap_w[MAX_SUB_BITMAPS];
    int                 sub_bitmap_h[MAX_SUB_BITMAPS];
    SDL_Rect            sub_bitmap_rects[MAX_SUB_BITMAPS];
    int                 sub_bitmap_count;

    /* Track change OSD */
    char                sub_osd[256];       /* "Subtitles: English" etc.    */
    double              sub_osd_until;      /* hide OSD after this time     */

    /* ── Media info cache ── */
    char                filepath[1024];
    char                media_info[8192]; /* formatted info string      */
    char                debug_info[4096]; /* formatted debug string     */

    /* ── Playback diagnostics ── */
    int                 diag_frames_displayed; /* total frames shown       */
    int                 diag_frames_decoded;   /* total frames decoded     */
    int                 diag_frames_dropped;   /* frames decoded but not shown */
    int                 diag_multi_decodes;    /* ticks with >1 decode     */
    int                 diag_timer_snaps;      /* frame_timer snap-forwards*/
    double              diag_max_av_drift;     /* worst A/V drift (signed) */
    double              diag_last_report;      /* time of last periodic log*/

} PlayerState;

/* ── Packet Queue API ─────────────────────────────────────────────── */

void  pq_init(PacketQueue *q);
void  pq_destroy(PacketQueue *q);
int   pq_put(PacketQueue *q, AVPacket *pkt);
int   pq_get(PacketQueue *q, AVPacket *pkt, int block);
void  pq_flush(PacketQueue *q);

/* ── Player API (player.c) ────────────────────────────────────────── */

int   player_open(PlayerState *ps, const char *filename);
void  player_close(PlayerState *ps);
int   demux_thread_func(void *arg);
int   video_decode_frame(PlayerState *ps);
void  video_display(PlayerState *ps);
void  video_reblit(PlayerState *ps);
void  player_seek(PlayerState *ps, double incr);
void  player_build_media_info(PlayerState *ps);
void  player_build_debug_info(PlayerState *ps);
void  player_update_display_rect(PlayerState *ps);

/* ── GPU Init (player.c) ──────────────────────────────────────────── */

int   gpu_create_pipelines(PlayerState *ps);
void  gpu_destroy_pipelines(PlayerState *ps);

/* ── Overlay GPU (player.c) ──────────────────────────────────────── */

int   gpu_overlay_ensure(PlayerState *ps, int width, int height);
void  gpu_overlay_upload(PlayerState *ps, const uint8_t *rgba, int width, int height);
void  gpu_overlay_copy_cmd(SDL_GPUCommandBuffer *cmd, PlayerState *ps);
void  gpu_overlay_draw(SDL_GPURenderPass *pass, SDL_GPUCommandBuffer *cmd,
                        PlayerState *ps, Uint32 sc_w, Uint32 sc_h);
void  gpu_overlay_destroy(PlayerState *ps);

/* ── Audio API (audio.c) ──────────────────────────────────────────── */

int   audio_open(PlayerState *ps);
void  audio_close(PlayerState *ps);
void  SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream,
                              int additional_amount, int total_amount);
int   audio_decode_frame(PlayerState *ps);
void  audio_find_streams(PlayerState *ps);
void  audio_cycle(PlayerState *ps);

/* ── Subtitle API (subtitle.c) ───────────────────────────────────── */

void  sub_find_streams(PlayerState *ps);
int   sub_open_codec(PlayerState *ps, int stream_idx);
void  sub_close_codec(PlayerState *ps);
void  sub_cycle(PlayerState *ps);
void  sub_decode_pending(PlayerState *ps);
int   sub_init_font(void);
void  sub_close_font(void);
TTF_Font *sub_get_font(void);
TTF_Font *sub_get_outline_font(void);
/* Phase 2: sub_render will be reworked for GPU compositing */
void  sub_render(PlayerState *ps, SDL_Renderer *renderer, int win_w, int win_h);

/* ── Overlay API (overlay.c) ─────────────────────────────────────── */

void  overlay_render(PlayerState *ps);
void  overlay_render_idle(PlayerState *ps);

/* ── Logging API (log.c) ───────────────────────────────────────────── */

void  log_init(void);
void  log_close(void);
void  log_msg(const char *fmt, ...);

/* ── Utility ──────────────────────────────────────────────────────── */

static inline double get_time_sec(void) {
    return (double)av_gettime_relative() / 1000000.0;
}

/* SDL3 render functions use SDL_FRect. Convert from our int rects.
 * Retained for Phase 2 overlay compositing. */
static inline SDL_FRect rect_to_frect(const SDL_Rect *r) {
    return (SDL_FRect){ (float)r->x, (float)r->y, (float)r->w, (float)r->h };
}

#endif /* DSVP_H */
