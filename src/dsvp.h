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
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/pixdesc.h>

/* SDL3 — SDL_MAIN_HANDLED prevents SDL from injecting WinMain */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>

/* SDL3 shadercross — runtime HLSL→SPIRV→native compilation */
#include <SDL3_shadercross/SDL_shadercross.h>

/* ── VAAPI zero-copy interop ──────────────────────────────────────── */
#include <vulkan/vulkan.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>  /* VADRMPRIMESurfaceDescriptor */
#include <libavutil/hwcontext_vaapi.h> /* AVVAAPIDeviceContext */
#include <unistd.h>  /* dup(), close() for DMA-BUF fd management */

/* ── Constants ──────────────────────────────────────────────────────── */

#define DSVP_VERSION        "0.3.8-beta"
#define DSVP_WINDOW_TITLE   "DSVP"

#define PACKET_QUEUE_MAX    256     /* max packets buffered per stream  */
#define AUDIO_BUF_SIZE      192000  /* max decoded audio buffer bytes   */
#define SEEK_STEP_SEC       5.0     /* arrow key seek increment         */
#define SEEK_LARGE_SEC     30.0    /* transport mode large seek increment  */
#define VOLUME_STEP         0.05    /* arrow key volume increment       */

#define MAX_SUB_STREAMS     16      /* max subtitle tracks to catalog   */
#define MAX_AUDIO_STREAMS   16      /* max audio tracks to catalog      */
#define SUB_TEXT_SIZE       4096    /* max subtitle text buffer         */

/* Multi-cue text subtitles (P2-16): up to SUB_TEXT_CUES overlapping text
 * cues display at once — second speaker, signs, nested timing; common in
 * ASS and overlapping SRT. Text tracks only: bitmap tracks (PGS/DVB/
 * VobSub) keep the single-display-set model, which is those formats' own
 * composition semantics. Eviction when full: the cue ending soonest. */
#define SUB_TEXT_CUES       4

typedef struct {
    char   text[SUB_TEXT_SIZE];
    double start_pts;
    double end_pts;
    int    valid;
} SubTextCue;
#define MAX_SUB_BITMAPS     4       /* max bitmap rects per subtitle    */

/* Default window size when no video is loaded */
#define DEFAULT_WIN_W       960
#define DEFAULT_WIN_H       540

/* Built-in file browser */
#define BROWSER_MAX_VISIBLE 40   /* max visible entries in file list  */
#define BROWSER_PATH_MAX    1024



/* ── Bitstream Audio Passthrough ────────────────────────────────────
 *
 * AudioMode controls how audio reaches the output device:
 *   PCM         — always decode to F32 stereo (current default behavior)
 *   AUTO        — probe HDMI sink via EDID; passthrough if supported, else PCM
 *   PASSTHROUGH — force passthrough; falls back to PCM on handshake failure
 *
 * BitstreamCaps is populated by bitstream_probe() from the EDID Short Audio
 * Descriptors reported by the connected HDMI sink via /sys/class/drm/.
 */

typedef enum {
    AUDIO_MODE_PCM         = 0,   /* decode → swr → F32 stereo (safe default) */
    AUDIO_MODE_AUTO        = 1,   /* probe sink, passthrough if possible       */
    AUDIO_MODE_PASSTHROUGH = 2    /* force passthrough, fallback on failure    */
} AudioMode;

/* Filled by bitstream_probe via the PipeWire registry (union of
 * iec958.codecs across Audio/Sink nodes). Answers PRE-DECISION
 * questions (TrueHD track selection, restart-on-track-switch); the
 * open-time negotiation in bitstream_pw is the final authority.
 * (alsa_device/hbr_capable/max_channels deleted 2026-08-20 with the
 * /proc ELD scan — Knot audit finding 2.) */
typedef struct BitstreamCaps {
    int  support_ac3;      /* sink decodes AC-3 (Dolby Digital)         */
    int  support_eac3;     /* sink decodes E-AC-3 (DD+ / Atmos)        */
    int  support_truehd;   /* sink decodes TrueHD (lossless / Atmos)   */
    int  support_dts;      /* sink decodes DTS core                    */
    int  support_dtshd;    /* sink decodes DTS-HD MA (lossless)        */
    int  probed;           /* 1 = caps have been queried this session   */
} BitstreamCaps;

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
    float chromaOffset[2];  /* chroma siting correction (texels)  8 bytes */
    float frameCount;       /* frame counter for temporal dither  4 bytes */
    float is_hdr;           /* 1.0 = HDR content detected         4 bytes */
    float hdr_peak_nits;    /* source peak luminance (nits)       4 bytes */
    float hdr_gamut;        /* 0.0=BT.709, 1.0=BT.2020 primaries 4 bytes */
    float hdr_debug;        /* 0-3: HDR debug viz mode             4 bytes */
    float hdr_target_nits;  /* SDR display peak (T key toggle)     4 bytes */
    float hdr_midtone_gain; /* midtone lift exponent (G key)       4 bytes */
    float is_dovi;          /* 1.0 = DV reshaping active           4 bytes */
    float is_semiplanar;    /* 1.0 = UV from single R16G16 tex    4 bytes */
    float out_gamma;        /* 0=sRGB piecewise, else power exp.   4 bytes */
    /* ── 144B boundary ── */
    float is_hlg;           /* 1.0 = HLG transfer (ARIB STD-B67)   4 bytes */
    float hdr_pass;         /* 1.0 = HDR passthrough (PQ out)      4 bytes */
    float out_gamut;        /* 0.0=BT.709 output, 1.0=BT.2020      4 bytes
                               (took half of _pad1 — every offset
                               below is unchanged) */
    float out_pq;           /* >0 = encode output as PQ/ST.2084 with  4 bytes
                               this value as reference white in nits
                               (SDR carried in an HDR10 container).
                               0 = normal SDR encode. Consumed the
                               last _pad1 float; the 16B boundary
                               below is unchanged. */
    /* ── 160B boundary ── */
    float dovi_num_pieces[4]; /* [I, Ct, Cp, 0] piece counts      16 bytes */
    float dovi_pivots[9][4];  /* [pivot][comp] normalized pivots 144 bytes */
    float dovi_c0[8][4];     /* [piece][comp] poly coef c0       128 bytes */
    float dovi_c1[8][4];     /* [piece][comp] poly coef c1       128 bytes */
    float dovi_c2[8][4];     /* [piece][comp] poly coef c2       128 bytes */
    float dovi_ycc_r0[4];   /* ycc→rgb row 0 [m,m,m,offset]      16 bytes */
    float dovi_ycc_r1[4];   /* ycc→rgb row 1 [m,m,m,offset]      16 bytes */
    float dovi_ycc_r2[4];   /* ycc→rgb row 2 [m,m,m,offset]      16 bytes */
    float dovi_out_r0[4];   /* output row 0 [m,m,m,0] (lms→2020) 16 bytes */
    float dovi_out_r1[4];   /* output row 1 [m,m,m,0]            16 bytes */
    float dovi_out_r2[4];   /* output row 2 [m,m,m,0]            16 bytes */
    /* ── DV MMR chroma reshaping (appended — offsets above unchanged) ──
     * Real-world P5 RPUs overwhelmingly reshape Ct/Cp with MMR, a
     * cross-channel polynomial over the full (I,Ct,Cp) triple; the
     * per-component poly path cannot represent it and those components
     * silently fell back to identity (right luma, wrong colour).
     * Ported from DSVP main bc20d0a. Single-piece MMR, order 1-3. */
    float dovi_mmr_meta[4];   /* [ct_order, cp_order, ct_const, cp_const] */
    float dovi_mmr_ct[6][4];  /* 21 coeffs, [order*7+term] packed  96 bytes */
    float dovi_mmr_cp[6][4];  /*                                   96 bytes */
} GPUUniforms;              /*                                 1008 bytes */

/* ── Player State ───────────────────────────────────────────────────
 *
 * Central structure holding everything: format/codec contexts, queues,
 * SDL handles, clocks, and UI state. One instance per playback session.
 */

/* Pacing v2 mode machine states (docs/DESIGN-PACING.md) */
#define PACE_SCHEDULED 0
#define PACE_LOCKED    1

/* ── get_buffer2 zero-copy decode pool (TODO-PACING open item 1) ──
 * Decoder frames are allocated directly inside persistently-mapped
 * SDL transfer buffers, so decode writes once into GPU-visible
 * memory and the staging memcpy disappears. A slot is owned by the
 * decoder from get_buffer2 until FFmpeg drops the last plane ref
 * (frame reordering / frame-threading hold slots for a while), then
 * COOLS for a few presents so the GPU's copy pass is provably past
 * before the decoder can write it again. These buffers are mapped
 * once at creation and NEVER cycled — cycling would swap the backing
 * out from under both the mapping and FFmpeg's frame pointers.
 * 8-bit yuv420p software decode only; every other path keeps the
 * prestage/upload fallbacks. */
#define DSVP_XFER_POOL_SLOTS 20
#define DSVP_XFER_POOL_COOL  3   /* swapchain SUBMITS before a freed slot
                                    recycles. All three submit sites count
                                    (video_display, reblit, idle draw) and
                                    that stays safe: each counted unit is a
                                    queue-ordered submit BEHIND the slot's
                                    copy pass, and the acquire throttle
                                    (2-3 swapchain images) bounds how far
                                    the GPU can lag those submits — so 3
                                    of them guarantee the copy retired,
                                    regardless of which loop submitted
                                    (review 2026-08-20 finding 19). */

enum { XFER_SLOT_FREE = 0, XFER_SLOT_BUSY, XFER_SLOT_COOLING };

typedef struct XferSlot {
    void                       *ps;        /* PlayerState, for the free cb */
    SDL_GPUTransferBuffer      *xy, *xu, *xv;
    uint8_t                    *my, *mu, *mv;  /* persistent mappings */
    SDL_AtomicInt               plane_refs;    /* live AVBufferRefs (3/frame) */
    int                         state;
    int                         cool_stamp;    /* ps->presents at release */
} XferSlot;

typedef struct PlayerState {
    /* ── Format / streams ── */
    AVFormatContext    *fmt_ctx;
    int                 video_stream_idx;
    int                 audio_stream_idx;

    /* ── Video decode ── */
    AVCodecContext     *video_codec_ctx;
    struct SwsContext  *sws_ctx;
    int                 sws_out_10bit;    /* sws destination is yuv420p10le (deep
                                             source kept at 10-bit through the
                                             R16 upload path) */
    int                 sws_dst_siting;   /* AVChromaLocation the sws OUTPUT was
                                             pinned to — the shader offset is
                                             derived from this when sws is active */
    AVFrame            *video_frame;      /* raw decoded frame          */
    AVFrame            *rgb_frame;        /* scaled/converted for SDL   */
    uint8_t            *rgb_buffer;       /* backing buffer for rgb_frame */

    /* ── VAAPI hardware decode (Linux only, HEVC) ── */
    AVBufferRef        *hw_device_ctx;    /* VAAPI device (/dev/dri/renderD128) */
    AVFrame            *hw_frame;         /* temp frame for hw→sw transfer      */
    uint8_t            *p010_u_plane;     /* deinterleaved U from P010 UV       */
    uint8_t            *p010_v_plane;     /* deinterleaved V from P010 UV       */
    int                 vaapi_active;     /* 1 = current file uses VAAPI decode */
    int                 vaapi_nv12;       /* 1 = VAAPI outputs NV12 (8-bit)     */

    /* ── VAAPI zero-copy interop (Linux, HEVC 10-bit P010) ── */
    int                 vaapi_zerocopy;   /* 1 = DMA-BUF→Vulkan path active     */
    VkDevice            vk_device;        /* extracted from SDL_GPU              */
    VkQueue             vk_queue;         /* graphics queue                      */
    uint32_t            vk_queue_family;  /* queue family index                  */
    VkCommandPool       vk_cmd_pool;      /* for DMA-BUF copy commands           */
    VkCommandBuffer     vk_cmd_buf;       /* reused each frame                   */
    VkFence             vk_copy_fence;    /* signals OUR copy submit (gains #3)  */
    int                 vk_copy_pending;  /* fence submitted, not yet waited     */
    VADisplay           va_display;       /* VAAPI display for surface export    */
    int                 vk_tex_image_offset; /* offset: SDL_GPUTexture → VkImage */

    /* ── Zero-copy import cache (Knot gains #2, Tier A) ──
     * VAAPI serves decode surfaces from a fixed DPB pool, so the same
     * VASurfaceIDs cycle for the life of the file. Until 2026-08-20
     * every frame re-ran vkCreateImage×2 + DMA-BUF vkAllocateMemory×2
     * + dup×2 and destroyed it all again (~48 kernel-side imports/s at
     * 24fps). Cache {VkImage, VkDeviceMemory} per surface, keyed on
     * the exported layout — vaExportSurfaceHandle still runs every
     * frame and stays the authority: any key mismatch rebuilds the
     * entry. DSVP_ZC_NOCACHE=1 restores per-frame import (A/B). */
    struct ZCImportEntry {
        uint32_t        surface;          /* VASurfaceID                */
        uint64_t        mod_y, mod_uv;    /* DRM format modifiers       */
        uint32_t        off_y, pitch_y;
        uint32_t        off_uv, pitch_uv;
        size_t          size_y, size_uv;  /* DMA-BUF object sizes       */
        VkImage         img_y, img_uv;
        VkDeviceMemory  mem_y, mem_uv;
        int             valid;
    }                   zc_imports[32];
    int                 zc_cache_hits;    /* diagnostics — logged at cleanup */
    int                 zc_cache_misses;
    int                 zc_cache_rebuilds;/* key mismatch on a live entry    */

    /* ── Audio decode ── */
    AVCodecContext     *audio_codec_ctx;
    struct SwrContext  *swr_ctx;
    int                 swr_in_fmt;      /* format swr was configured for      */
    int                 swr_in_rate;     /* rate swr was configured for        */
    AVChannelLayout     swr_in_layout;   /* layout swr was configured for
                                            (owned copy — uninit with swr)    */
    AVFrame            *audio_frame;
    uint8_t            *audio_buf;        /* resampled audio buffer     */
    unsigned int        audio_buf_size;   /* bytes of valid data in buf */
    unsigned int        audio_buf_index;  /* read cursor into buf       */
    unsigned int        audio_buf_cap;    /* allocated capacity of audio_buf (bytes) */

    /* ── Bitstream passthrough ── */
    AudioMode           audio_mode;       /* PCM / Auto / Passthrough   */
    BitstreamCaps       bitstream_caps;   /* HDMI sink capabilities     */
    int                 bitstream_active; /* 1 = currently passing through */
    void               *spdif_ctx;       /* AVFormatContext* — spdifenc muxer      */
    void               *spdif_avio;      /* AVIOContext* — spdifenc memory buffer  */
    uint8_t            *spdif_buf;       /* IEC 61937 framed output buffer         */
    int                 spdif_buf_size;  /* allocated size of spdif_buf            */
    int                 spdif_write_pos; /* write cursor into spdif_buf (per-frame) */
    SDL_Thread         *bitstream_thread;
    int                 bitstream_quit;  /* signal bitstream thread to exit        */
    int                 bitstream_seek_pending; /* signal feeder to flush + resync      */
    int                 bitstream_failed; /* feeder saw stream ERROR — main loop
                                             completes the PCM fallback            */
    void               *bpw;             /* BitstreamPW* — PipeWire passthrough
                                          * backend state (bitstream_pw.c)     */
    double              audio_delay_sec; /* DSVP_AUDIO_DELAY user offset:
                                          * positive = sink chain delays audio
                                          * vs video; applied to the bitstream
                                          * sync clock */

    /* ── Async audio mode switch ──
     * P-key mode switch uses a background thread for caller-API
     * compatibility, even though the slow pactl/delay work is gone.
     * audio_switch_phase:
     *   0 = idle
     *   1 = background thread running
     *   2 = ready for completion (main loop finishes the switch)
     */
    int                 audio_switch_phase;
    int                 audio_switch_to_mode;   /* target AudioMode */
    int                 audio_switch_was_truehd; /* 1 = was TrueHD, need track switch */
    SDL_Thread         *audio_switch_thread;

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
    SDL_GPUTexture             *gpu_tex_uv;          /* UV interleaved (R16G16, zero-copy) */
    SDL_GPUTexture         *gpu_tex_noise;           /* 64×64 blue noise dither (app lifetime) */
    SDL_GPUTexture         *gpu_tex_lut_lin;         /* 1024×1 R16 x^1.2 (shader squares) linearise LUT */
    SDL_GPUTexture         *gpu_tex_lut_pq;          /* 1024×1 R16 PQ-encode LUT, sqrt-domain,
                                                        nits baked in (REVIEW-PERF §3) */
    int                     pq_lut_active;           /* LUT pipelines compiled + textures live */
    /* CPU→GPU staging. Sets 0/1 are a ping-pong owned by the DECODE
     * thread (it pre-fills the set the main thread is not reading —
     * the 12.4MB/frame Y+U+V memcpy used to run on the vsync-gated
     * main thread). Set 2 is owned by the MAIN thread for every path
     * the decode thread cannot serve (VAAPI readback deinterleave,
     * swscale conversion, fallbacks). No set is ever touched by both
     * threads — that ownership split is the whole synchronization
     * story, on top of the existing decode_mutex frame handoff. */
    SDL_GPUTransferBuffer      *gpu_xfer_y[3];
    SDL_GPUTransferBuffer      *gpu_xfer_u[3];
    SDL_GPUTransferBuffer      *gpu_xfer_v[3];
    int                         xfer_fill;          /* decode-thread: next set to fill (0/1) */
    int                         decoded_frame_xfer; /* set staged for decoded_frame, -1 none */
    int                         video_frame_xfer;   /* set staged for video_frame, -1 none */
    /* get_buffer2 pool (see XferSlot above). pool_n == 0 = pool off
     * for this file; every consumer falls back to the sets above. */
    XferSlot                    xfer_pool[DSVP_XFER_POOL_SLOTS];
    int                         xfer_pool_n;
    int                         xfer_pool_pitch_y;  /* bytes/row (== pixels, 8-bit) */
    int                         xfer_pool_pitch_uv;
    int                         xfer_pool_h;        /* padded plane heights */
    int                         xfer_pool_ch;
    SDL_Mutex                  *xfer_pool_mutex;
    int                         xfer_pool_misses;   /* default-alloc fallbacks */
    int                         no_pool;            /* DSVP_NO_POOL */
    int                         decoded_frame_slot; /* pool slot of decoded_frame, -1 */
    int                         video_frame_slot;   /* pool slot of video_frame, -1 */
    int                         swapchain_hdr10;    /* current composition is
                                                       HDR10_ST2084 (recreate-skip) */
    int                         sub_cue_gen;        /* bumped on any text-cue
                                                       change; overlay skips the
                                                       stack re-join when unchanged */
    int                         xfer_pool_served;   /* frames the decoder wrote
                                                       straight into pool slots */
    GPUUniforms                 gpu_uniforms;         /* current color params */

    /* ── HDR dynamic peak detection (Layer 1: CPU scan) ── */
    float                       hdr_smoothed_peak;    /* temporally smoothed peak (nits) */
    float                       hdr_prev_frame_peak;  /* raw peak from previous frame    */
    float                       hdr_static_peak;      /* metadata peak (fallback ceiling) */
    int                         hdr_target_idx;       /* index into SDR target nit table  */

    /* ── HDR output (passthrough) — docs/TODO-HDR.md ── */
    int                         hdr_out_mode;         /* 0 = off (tone-map), 1 = auto.
                                                         Z key toggles; survives file
                                                         opens within the session */
    int                         hdr_out_active;       /* swapchain is currently
                                                         HDR10/ST2084 */
    int                         hdr_pass_content;     /* current file's signal can pass
                                                         through as-is (PQ, non-DV-P5,
                                                         non-HLG for now) */
    char                        hdr_sys_output[32];   /* HDR-capable output name from
                                                         kscreen-doctor (e.g. "DP-1") */
    int                         hdr_sys_prior_hdr;    /* output HDR state before we
                                                         touched it (restore target) */
    int                         hdr_sys_prior_wcg;    /* same for wide-colour-gamut:
                                                         1 on, 0 off, -1 unknown or
                                                         unreported. We write this
                                                         property, so we must read it —
                                                         -1 means never touch it */
    int                         hdr_sys_enabled_by_us;/* we flipped output HDR on —
                                                         restore on revert/shutdown */
    float                       out_pq_nits;          /* >0 = carry SDR in an HDR10/PQ
                                                         container at this reference
                                                         white (BT.2408 says 203).
                                                         DSVP_OUT_PQ[=nits]. */
    int                         out_gamut_pref;       /* 0 = encode for a BT.709/sRGB
                                                         display (default), 1 = encode for
                                                         a BT.2020 display. M key. See the
                                                         out_gamut shader uniform. */
    float                       out_gamma_pref;       /* output transfer, preserved across
                                                         opens (E key / DSVP_OUTPUT_GAMMA).
                                                         0 = unset (parse env on first
                                                         open), 1.0 = sRGB piecewise,
                                                         else pure power exponent — the
                                                         uniform encodes sRGB as 0.0, but
                                                         0 must mean "unset" here so
                                                         zero-init works */
    int                         dovi_metadata_logged; /* 1 = logged DV RPU for this file  */

    /* ── Overlay GPU handles (lifetime: application, resized as needed) ── */
    SDL_GPUGraphicsPipeline    *gpu_pipeline_overlay; /* RGBA + alpha blend */
    SDL_GPUGraphicsPipeline    *gpu_pipeline_blit;    /* frame tex → swapchain copy */
    SDL_GPUGraphicsPipeline    *gpu_pipeline_yuv_frame;   /* YUV → RGBA16 intermediate */
    SDL_GPUGraphicsPipeline    *gpu_pipeline_yuv_dilated_frame; /* dilated → intermediate */
    SDL_GPUGraphicsPipeline    *gpu_pipeline_yuv_direct;       /* exact-1:1 single-fetch */
    SDL_GPUGraphicsPipeline    *gpu_pipeline_yuv_direct_frame; /* direct → intermediate  */
    SDL_GPUGraphicsPipeline    *gpu_pipeline_yuv_scale2x;       /* exact-2x constant weights */
    SDL_GPUGraphicsPipeline    *gpu_pipeline_yuv_scale2x_frame; /* scale2x → intermediate   */
    SDL_GPUGraphicsPipeline    *gpu_pipeline_yuv_dilated; /* downscale-only sampler
                                                             variant (DSVP_DILATE) */

    /* ── Intermediate frame texture (render-at-content-rate) ──
     * The full video shader runs only on NEW frames (or when render
     * state changes), into a UNORM16 texture; every vsync then blits
     * it to the swapchain (memcpy-class). Re-running the 4K sampler
     * chain 60x/s for 24fps content was ~2.5x redundant GPU work —
     * the headroom KWin's HDR compositing pushed over the edge.
     * UNORM16 precision (2^-16, uniform) exceeds both output depths;
     * the video shader's dither already cycles per CONTENT frame
     * (frameCount = frames_displayed), so output is bit-identical to
     * the direct path. DSVP_NO_INTERMEDIATE=1 restores direct. */
    SDL_GPUTexture             *gpu_tex_frame;
    int                         frame_tex_w, frame_tex_h;
    int                         frame_tex_valid;   /* holds current content   */
    int                         frame_render_dirty;/* uniforms/geometry moved —
                                                      re-render before blit   */
    int                         no_intermediate;   /* env opt-out / fallback  */
    int                         no_prestage;       /* DSVP_NO_PRESTAGE: decode-thread
                                                      staging off (falsification switch) */
    int                         drift_resync_ticks;/* consecutive 1:1 ticks with
                                                      video >0.5s behind audio —
                                                      triggers the warm reseek */
    int                         intermediate_apt;  /* per-file: content rate is
                                                      below display refresh, so
                                                      redundant re-renders exist
                                                      for the intermediate to
                                                      eliminate. At content ==
                                                      refresh (4K60 SDR) the
                                                      intermediate saves nothing
                                                      and its blit bandwidth
                                                      broke 60 present — field
                                                      regression 2026-08-07 */

    /* ── Pacing: median cadence sensor + explicit LOCKED/SCHEDULED
     * mode machine (docs/DESIGN-PACING.md). The v1 threshold stack
     * was removed at 0.3.6. ── */
    double                      pace_ring[32];     /* presented-frame intervals */
    int                         pace_ring_n;       /* filled entries (≤32); reset
                                                      by window-event hints for a
                                                      fast re-measure           */
    int                         pace_ring_pos;     /* next write index          */
    double                      pace_content_ema;  /* EMA of content pts deltas —
                                                      the contracts compare cadence
                                                      against THIS, not the last
                                                      delta (1ms-quantized container
                                                      timebases alternate ~16/17ms
                                                      at 59.94 and a single sample
                                                      can never hold a 0.25ms
                                                      tolerance for 30 presents) */
    double                      pace_median;       /* median of ring; 0 = still
                                                      measuring (blocks LOCKED
                                                      entry, suspends cadence
                                                      exit; drift exit stays)   */
    double                      pace_last_present; /* wall time of last present
                                                      (display OR reblit; drop
                                                      ticks are not presents)   */
    int                         pace_mode;         /* PACE_SCHEDULED / PACE_LOCKED */
    int                         active_variant;    /* sampler variant bound last
                                                      render: -1 none yet, 0 fixed,
                                                      1 dilated, 2 direct,
                                                      3 scale2x — feeds the debug
                                                      panel, which must never
                                                      claim a kernel that is not
                                                      running (the Lanczos-2 line
                                                      lied on 3 of 4 variants). */
    int                         pace_enter_streak; /* presented frames meeting
                                                      LOCKED entry cadence match */
    int                         pace_exit_streak;  /* presented frames failing
                                                      LOCKED cadence (exit path) */
    int                         pace_drift_streak; /* displayed frames with drift
                                                      beyond bias ± one frame —
                                                      the LOCKED exit contract   */
    double                      last_av_diff;      /* raw av_diff at last consume;
                                                      the entry contract's
                                                      settled-drift check        */
    double                      sched_off;         /* smoothed (wall − playback
                                                      clock) offset — the
                                                      scheduler's time base:
                                                      t_ideal = pts + sched_off
                                                      − av_bias                  */
    int                         sched_off_valid;   /* 0 until anchored           */
    int                         sched_chain;       /* mid drop-chain: the next
                                                      frame is due NOW — skip
                                                      reblit, yield for decode
                                                      delivery                   */
    double                      sched_chain_start; /* chain safety-timeout base  */
    double                      pace_bias_ref;     /* av_bias SNAPSHOT taken at
                                                      LOCKED entry — the exit
                                                      contract and reseek
                                                      backstop compare against
                                                      this, never the live EMA:
                                                      a snapshot cannot absorb
                                                      drift (batch-2 field
                                                      notes, lesson 10)         */
    int                         hdrwire_logged;    /* one-shot: wire HDR state
                                                      logged for this file's
                                                      passthrough session       */
    SDL_GPUTexture             *gpu_overlay_tex;      /* RGBA8888 overlay  */
    SDL_GPUTransferBuffer      *gpu_overlay_xfer;     /* CPU→GPU staging   */
    int                         overlay_tex_w;         /* current texture dimensions */
    int                         overlay_tex_h;
    int                         overlay_dirty;         /* 1 = need re-upload */
    int                         overlay_tex_undefined; /* fresh texture, no full-height
                                                          upload yet — gpu_overlay_upload
                                                          widens the next band to full */
    int                         overlay_up_y0;         /* pending upload rows */
    int                         overlay_up_y1;         /* (exclusive)         */

    /* ── Timing / A/V sync ── */
    double              io_deadline;      /* abort FFmpeg I/O after this time */
    double              audio_clock;      /* current audio PTS in secs (audio thread internal) */
    double              audio_clock_sync; /* latency-corrected snapshot for main thread A/V sync */
    double              av_bias;          /* adaptive A/V offset (EMA of av_diff) */
    int                 av_bias_samples;  /* warmup counter (apply after 60)     */
    double              audio_pts_floor;  /* post-seek: discard audio frames with PTS below this */
    double              video_clock;      /* current video PTS in secs  */
    double              frame_timer;      /* when we last showed a frame*/
    double              frame_last_delay; /* last frame display duration*/
    double              frame_last_pts;   /* PTS of last displayed frame*/
    int64_t             seek_target;      /* seek target in AV_TIME_BASE*/
    int                 seek_request;     /* 1 = seek pending           */
    int                 seek_flags;
    int                 seek_recovering;  /* 1 = waiting for first displayed frame post-seek */
    double              last_frame_wall;  /* wall-clock of last displayed frame */
    int                 audio_stalled;    /* 1 = audio paused due to video stall */

    /* ── Threads ── */
    SDL_Thread         *demux_thread;
    SDL_Mutex          *seek_mutex;    /* protects codec flush vs decode  */
    int                 seeking;       /* 1 = flush in progress, skip decode */

    /* ── Decode thread (async video decode) ──
     *
     * Moves video decoding off the main loop into a background
     * thread.  The thread continuously decodes one frame ahead into
     * decoded_frame.  Ownership is gated by decode_frame_ready:
     *   ready == 0  →  decode thread may write decoded_frame
     *   ready == 1  →  main loop may consume decoded_frame
     * The main loop signals decode_cond after consuming so the decode
     * thread can proceed to the next frame. */
    SDL_Thread         *decode_thread;
    SDL_Mutex          *decode_mutex;       /* protects decoded_frame handoff    */
    SDL_Condition      *decode_cond;        /* signal decode thread after consume */
    AVFrame            *decoded_frame;      /* decode thread's output buffer     */
    double              decoded_pts;        /* PTS of decoded_frame              */
    int                 decode_frame_ready; /* 1 = decoded_frame has new frame   */
    int                 decode_eof;         /* 1 = decoder drained, no more frames */

    /* ── Playback state ── */
    int                 playing;          /* 1 = file is loaded/playing */
    int                 paused;
    int                 quit;             /* 1 = application exiting    */
    int                 closing;          /* 1 = player_close in progress —
                                             stops this file's threads.
                                             NEVER doubles as quit: the
                                             shim sets quit before close
                                             and it must survive (Knot
                                             audit finding 1; the same
                                             double-duty shape as
                                             hdr_sys_enabled_by_us).   */
    double              volume;           /* 0.0 — 1.0                  */
    int                 fullscreen;
    int                 eof;              /* demuxer hit end of file    */
    int                 io_error;         /* demux thread hit I/O error (NFS loss etc.) */
    int                 vaapi_unsupported; /* get_format saw no VAAPI for this stream
                                              (unsupported profile) — main retries the
                                              file in software (review P2-17)          */
    int                 force_swdec;       /* next player_open skips VAAPI (the retry)  */
    int                 video_ready;      /* 1 after first frame uploaded — gates reblit */
    int                 res_change_logged;/* per-file: mid-stream resolution warn fired */

    /* ── Gamepad (steamdeck branch) ── */
    SDL_Gamepad        *gamepad;           /* first connected gamepad (NULL if none) */
    int                 gamepad_active;    /* 1 = gamepad connected, show pad hints  */
    float               trigger_seek_speed; /* analog seek multiplier (0.0 = idle)   */
    int                 dpad_held_dir;     /* -1=up, 1=down, 0=none (browser repeat) */
    double              dpad_held_since;   /* wall time when d-pad was pressed        */
    double              dpad_last_repeat;  /* wall time of last repeat fire           */
    int                 transport_active;   /* 1 = transport control mode (L3)    */
    int                 transport_focus;    /* 0=prev, 1=scrubber, 2=next         */
    int                 transport_seek_dir;  /* -1=left held, 0=idle, 1=right held */
    double              transport_seek_start; /* wall time when stick entered zone  */
    double              transport_seek_last;  /* wall time of last repeat fire */

    /* ── Game Mode detection ── */
    int                 game_mode;         /* 1 = Gamescope (Game Mode), 0 = Desktop */
    int                 ui_scale;          /* font scale: 3 in Game Mode, 1 Desktop  */
    int                 show_controls;     /* 1 = controls overlay visible (Start)   */

    /* ── Built-in file browser (steamdeck branch) ── */
    int                 browser_active;       /* 1 = browser is shown (replaces idle)  */
    char                browser_path[BROWSER_PATH_MAX]; /* current directory            */
    char              **browser_entries;       /* full paths of entries                  */
    char              **browser_names;         /* display names (with [DIR] prefix)     */
    int                *browser_is_dir;        /* 1 = directory entry                   */
    int                 browser_count;         /* total entries in current dir           */
    int                 browser_sel;           /* selected index                        */
    int                 browser_scroll;        /* first visible index (scroll offset)   */
    char                browser_selected_file[BROWSER_PATH_MAX]; /* result of Enter     */

    /* ── Window geometry ── */
    int                 win_w, win_h;     /* current window size        */
    int                 vid_w, vid_h;     /* video native resolution    */
    int                 chroma_location;  /* AVChromaLocation for debug overlay */
    SDL_Rect            display_rect;     /* letterboxed video area     */
    int                 sc_w, sc_h;       /* last swapchain dims (physical pixels) */

    /* ── Overlay visibility state ── */
    int                 show_debug;
    int                 show_info;
    int                 show_seekbar;         /* 1 = seek bar visible       */
    double              seekbar_hide_time;    /* auto-hide after this time  */
    int                 seekbar_track_x;      /* progress track left edge   */
    int                 seekbar_track_w;      /* progress track width       */
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

    /* Multi-cue text display (P2-16) — text tracks fill these instead
     * of the single slot above; bitmap tracks never touch them. */
    SubTextCue          sub_cues[SUB_TEXT_CUES];
    int                 sub_cue_count;      /* valid entries in sub_cues    */

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
    long                presents;         /* actual swapchain submits — the
                                           * truth PRESENT DIAG reports;
                                           * iteration count lies during
                                           * acquire failures/drop chains */
    int                 sched_drop_run;   /* consecutive SCHEDULED drops
                                           * (video-only backstop) */
    int                 diag_multi_decodes;    /* ticks with >1 decode     */
    int                 diag_timer_snaps;      /* frame_timer snap-forwards*/
    double              diag_max_av_drift;     /* worst A/V drift (signed) */
    double              diag_last_report;      /* time of last periodic log*/

    /* ── Real-time FPS measurement (debug overlay, 0.5s window) ── */
    double              fps_window_start;      /* window anchor (wall sec) */
    int                 fps_window_frames;     /* CONTENT frames in window */
    int                 rfps_window_frames;    /* GPU presents in window   */
    double              fps_content;           /* measured content fps     */
    double              fps_render;            /* measured present rate    */

    /* ── Folder playlist (prev/next navigation) ── */
    char              **playlist_files;      /* sorted full paths          */
    int                 playlist_count;      /* number of playable files   */
    int                 playlist_index;      /* current file's index (-1)  */

#ifdef DSVP_PROFILE
    /* ── Frame pacing profiler (build with `make profile`) ── */
    double              prof_upload_ms;       /* last frame: deinterleave+upload */
    double              prof_peak_ms;         /* last frame: hdr_compute_scene_peak */
    double              prof_vsync_ms;        /* last frame: copy pass + VSync wait */
    double              prof_render_ms;       /* last frame: render pass + submit */
    double              prof_display_ms;      /* last frame: total video_display() */
    double              prof_decode_ms;       /* last frame: video decode          */

    /* Running stats (reset every 10s DIAG window) */
    int                 prof_n;
    double              prof_sum_upload;
    double              prof_sum_peak;
    double              prof_sum_vsync;
    double              prof_sum_decode;
    double              prof_sum_total;
    double              prof_max_upload;
    double              prof_max_peak;
    double              prof_max_vsync;
    double              prof_max_decode;
    double              prof_max_total;
#endif

} PlayerState;

/* ── Subtitle staleness contract ──────────────────────────────────────
 * One knob, three coupled windows — these lived as free-standing
 * literals in two files and drifted apart is exactly the failure mode.
 *
 * SUB_STALE_CAP_SEC: decode-side safety net. PGS/DVB sets carry
 *   end_display_time = UINT32_MAX (duration unknown until the clear
 *   packet); cap so an orphaned set can't stick forever.
 * SUB_PRUNE_WINDOW_SEC: demux-side rolling-window depth per track.
 *   MUST exceed the cap, or the demuxer prunes packets the decoder
 *   still considers displayable.
 * SUB_CLEAR_FOUND_SEC: a displayed bitmap whose duration is below the
 *   cap already had its end set by a real clear packet — the
 *   drain-for-clear can stop. Must sit just under the cap. */
#define SUB_STALE_CAP_SEC     30.0
#define SUB_PRUNE_WINDOW_SEC  (SUB_STALE_CAP_SEC + 5.0)
#define SUB_CLEAR_FOUND_SEC   (SUB_STALE_CAP_SEC - 1.0)

/* Hard byte backstop per subtitle queue. pq_prune_stale can't judge a
 * NOPTS head (DVB teletext in MPEG-TS emits them), which halted PTS
 * pruning permanently and let an unselected track's queue grow for the
 * whole file. Far above any real prune-window footprint; only a
 * NOPTS-emitting track ever reaches it. */
#define SUB_PQ_MAX_BYTES  (8 * 1024 * 1024)

/* Master playback clock: audio-driven when an audio stream is active,
 * video clock otherwise. This exact ternary lived at four call sites
 * across three files — a new consumer must use this, not re-derive. */
static inline double player_clock(const PlayerState *ps) {
    return (ps->audio_stream_idx >= 0) ? ps->audio_clock_sync
                                       : ps->video_clock;
}

/* ── Packet Queue API ─────────────────────────────────────────────── */

void  pq_init(PacketQueue *q);
void  pq_destroy(PacketQueue *q);
int   pq_put(PacketQueue *q, AVPacket *pkt);
int   pq_get(PacketQueue *q, AVPacket *pkt, int block);
void  pq_flush(PacketQueue *q);
void  pq_prune_stale(PacketQueue *q, int64_t min_pts, int max_bytes);
int   natural_casecmp(const char *a, const char *b);
int   pq_peek_pts(PacketQueue *q, int64_t *pts_out);

/* ── Player API (player.c) ────────────────────────────────────────── */

int   player_open(PlayerState *ps, const char *filename);
void  player_close(PlayerState *ps);
int   demux_thread_func(void *arg);
int   decode_thread_func(void *arg);
void  video_display(PlayerState *ps);
void  video_reblit(PlayerState *ps);
void  player_seek(PlayerState *ps, double incr);
void  player_build_media_info(PlayerState *ps);
void  player_build_debug_info(PlayerState *ps);
void  player_update_display_rect(PlayerState *ps);

/* ── GPU Init (player.c) ──────────────────────────────────────────── */

int   gpu_create_pipelines(PlayerState *ps);
void  gpu_destroy_pipelines(PlayerState *ps);
void  hdr_output_apply(PlayerState *ps);
void  hdr_sys_preenable(PlayerState *ps);
int   hdr_sys_display_is_wide_gamut(PlayerState *ps);
void  hdr_output_shutdown(PlayerState *ps);

/* ── Overlay GPU (player.c) ──────────────────────────────────────── */

int   gpu_overlay_ensure(PlayerState *ps, int width, int height);
void  gpu_overlay_upload(PlayerState *ps, const uint8_t *rgba, int width, int height, int y0, int y1);
void  gpu_overlay_copy_cmd(SDL_GPUCommandBuffer *cmd, PlayerState *ps);
void  gpu_overlay_draw(SDL_GPURenderPass *pass, SDL_GPUCommandBuffer *cmd,
                        PlayerState *ps, Uint32 sc_w, Uint32 sc_h);
void  gpu_overlay_destroy(PlayerState *ps);

/* ── Audio API (audio.c) ──────────────────────────────────────────── */

int   audio_open(PlayerState *ps);
void  audio_disable_public(PlayerState *ps, const char *osd_msg);
void  hdr_sys_reconcile_stamp(void);
void  hdr_sys_verify_hold(PlayerState *ps);
void  audio_close(PlayerState *ps);
void  SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream,
                              int additional_amount, int total_amount);
int   audio_decode_frame(PlayerState *ps);
void  audio_find_streams(PlayerState *ps);
void  audio_cycle(PlayerState *ps);
void  hdrwire_log_state(void);  /* hdrwire.c — log the wire HDR infoframe */
void  bitstream_probe(PlayerState *ps);
int   bitstream_hd_blocked(int av_codec_id, int src_rate); /* >48k IEC platform
                                          * blocklist — see audio.c; override
                                          * DSVP_HD_PASSTHROUGH=1 */
int   bitstream_start(PlayerState *ps);  /* spdifenc + pw stream, start feeder */
void  bitstream_stop(PlayerState *ps);   /* close stream + spdifenc, join feeder */
void  bitstream_stop_immediate(PlayerState *ps); /* fast stop, no delays — for async switch */
int   audio_switch_bg_func(void *arg);   /* async-switch signaler thread */

/* ── PipeWire passthrough backend (bitstream_pw.c) ────────────────
 * The only bitstream transport: iec958 encoded stream, server owns
 * the device and channel status (docs/TODO-BITSTREAM.md). */
int    bitstream_pw_probe_caps(BitstreamCaps *caps); /* sink codec union
                                                        via registry; 1 =
                                                        server reachable */
int    bitstream_pw_open(PlayerState *ps, int av_codec_id, int rate,
                         int channels);          /* 1 = negotiated        */
int    bitstream_pw_write(PlayerState *ps, const uint8_t *data, int len);
double bitstream_pw_buffered(PlayerState *ps);   /* seconds ring+queued   */
void   bitstream_pw_pause(PlayerState *ps, int paused);
void   bitstream_pw_seek_reset(PlayerState *ps);
void   bitstream_pw_eof_drain(PlayerState *ps);  /* activate below-threshold tail */
void   bitstream_pw_close(PlayerState *ps);

/* ── Subtitle API (subtitle.c) ───────────────────────────────────── */

void  sub_find_streams(PlayerState *ps);
int   sub_open_codec(PlayerState *ps, int stream_idx);
void  sub_close_codec(PlayerState *ps);
void  sub_cycle(PlayerState *ps);
void  sub_decode_pending(PlayerState *ps);
void  sub_text_cues_clear(PlayerState *ps);
int   sub_init_font(void);
void  sub_close_font(void);
TTF_Font *sub_get_font(void);
TTF_Font *sub_get_outline_font(void);

/* ── Overlay API (overlay.c) ─────────────────────────────────────── */

void  overlay_render(PlayerState *ps);
void  overlay_render_idle(PlayerState *ps);
void  overlay_render_browser(PlayerState *ps);
/* UI scale for a given swapchain height — single source of truth, shared by
 * overlay.c's draw code and main.c's seekbar hit-testing so the two cannot
 * disagree about where the bar is. */
int   ui_scale_for(const PlayerState *ps, int sc_h);
void  overlay_cleanup(void);

/* ── Browser API (browser.c) ────────────────────────────────────── */

void  browser_init(PlayerState *ps);
void  browser_scan(PlayerState *ps);
void  browser_free_entries(PlayerState *ps);
void  browser_navigate(PlayerState *ps, int delta);
void  browser_page(PlayerState *ps, int delta);
int   browser_enter(PlayerState *ps);    /* returns 1 if file selected */
void  browser_back(PlayerState *ps);
int   browser_at_root(PlayerState *ps);
void  browser_save_path(PlayerState *ps);

/* ── Logging API (log.c) ───────────────────────────────────────────── */

void  log_init(void);
void  log_close(void);
void  log_msg(const char *fmt, ...);
int   log_anon_active(void);  /* returns 1 if DSVP_LOG_ANON is set */
const char *log_path(const char *path);  /* path arg for log lines —
                                            "[redacted]" under
                                            DSVP_LOG_ANON. Use for
                                            EVERY path a log_msg
                                            prints. */

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
