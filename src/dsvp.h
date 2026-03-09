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

/* SDL2 */
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

/* ── Constants ──────────────────────────────────────────────────────── */

#define DSVP_VERSION        "0.1.2.1"
#define DSVP_WINDOW_TITLE   "DSVP"

#define PACKET_QUEUE_MAX    256     /* max packets buffered per stream  */
#define AUDIO_BUF_SIZE      192000  /* max decoded audio buffer bytes   */
#define SEEK_STEP_SEC       5.0     /* arrow key seek increment         */
#define VOLUME_STEP         0.05    /* arrow key volume increment       */
#define SDL_AUDIO_BUFFER_SZ 1024    /* SDL audio callback buffer size   */

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
    SDL_mutex   *mutex;
    SDL_cond    *cond;
    int          abort_request; /* signal threads to stop blocking      */
} PacketQueue;

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
    SDL_Renderer       *renderer;
    SDL_Texture        *texture;
    SDL_AudioDeviceID   audio_dev;
    SDL_AudioSpec       audio_spec;       /* actual device spec         */

    /* ── Timing / A/V sync ── */
    double              audio_clock;      /* current audio PTS in secs  */
    double              video_clock;      /* current video PTS in secs  */
    double              frame_timer;      /* when we last showed a frame*/
    double              frame_last_delay; /* last frame display duration*/
    double              frame_last_pts;   /* PTS of last displayed frame*/
    int64_t             seek_target;      /* seek target in AV_TIME_BASE*/
    int                 seek_request;     /* 1 = seek pending           */
    int                 seek_flags;

    /* ── Threads ── */
    SDL_Thread         *demux_thread;
    SDL_mutex          *seek_mutex;    /* protects codec flush vs decode  */
    int                 seeking;       /* 1 = flush in progress, skip decode */

    /* ── Playback state ── */
    int                 playing;          /* 1 = file is loaded/playing */
    int                 paused;
    int                 quit;             /* 1 = application exiting    */
    double              volume;           /* 0.0 — 1.0                  */
    int                 fullscreen;
    int                 eof;              /* demuxer hit end of file    */

    /* ── Window geometry ── */
    int                 win_w, win_h;     /* current window size        */
    int                 vid_w, vid_h;     /* video native resolution    */
    SDL_Rect            display_rect;     /* letterboxed video area     */

    /* ── Overlays ── */
    int                 show_debug;
    int                 show_info;

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

    /* Bitmap subtitle textures (PGS, VobSub, DVB) */
    SDL_Texture        *sub_bitmaps[MAX_SUB_BITMAPS];
    SDL_Rect            sub_bitmap_rects[MAX_SUB_BITMAPS];
    int                 sub_bitmap_count;

    /* Track change OSD */
    char                sub_osd[256];       /* "Subtitles: English" etc.    */
    double              sub_osd_until;      /* hide OSD after this time     */

    /* ── Media info cache ── */
    char                filepath[1024];
    char                media_info[8192]; /* formatted info string      */
    char                debug_info[4096]; /* formatted debug string     */

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
void  player_seek(PlayerState *ps, double incr);
void  player_build_media_info(PlayerState *ps);
void  player_build_debug_info(PlayerState *ps);
void  player_update_display_rect(PlayerState *ps);

/* ── Audio API (audio.c) ──────────────────────────────────────────── */

int   audio_open(PlayerState *ps);
void  audio_close(PlayerState *ps);
void  audio_callback(void *userdata, Uint8 *stream, int len);
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
void  sub_render(PlayerState *ps, SDL_Renderer *renderer, int win_w, int win_h);

/* ── Logging API (log.c) ───────────────────────────────────────────── */

void  log_init(void);
void  log_close(void);
void  log_msg(const char *fmt, ...);

/* ── Utility ──────────────────────────────────────────────────────── */

static inline double get_time_sec(void) {
    return (double)av_gettime_relative() / 1000000.0;
}

#endif /* DSVP_H */
