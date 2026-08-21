/*
 * DSVP — Dead Simple Video Player
 * player.c — Demux, video decode, display, seeking, media info
 *
 * Threading model:
 *   - Demux thread: reads packets from the container, pushes to queues
 *   - Main thread:  pops video packets, decodes, scales, renders
 *   - SDL audio thread: calls audio_callback() which decodes audio
 *
 * A/V sync strategy:
 *   Audio is the master clock. Video frame display timing is adjusted
 *   to match the audio clock. This is the standard approach (same as
 *   ffplay) because audio glitches are far more perceptible than
 *   dropped/delayed video frames.
 *
 * Rendering (v0.1.4 — SDL_GPU):
 *   Video frames are uploaded to GPU textures (R8_UNORM per plane for
 *   8-bit, R16_UNORM/R16G16_UNORM for 10-bit passthrough) and converted
 *   YUV→RGB by custom HLSL fragment shaders compiled at runtime via
 *   SDL3_shadercross. This replaces SDL_Renderer and unlocks HDR10
 *   and further GPU-side processing.
 */

#include "dsvp.h"

#include <sys/wait.h>   /* waitpid */
#include <spawn.h>      /* posix_spawnp */
#include <fcntl.h>      /* O_WRONLY */
#include <signal.h>     /* kill(pid, 0) — stamp liveness probe */
#include <unistd.h>     /* getpid */


/* ═══════════════════════════════════════════════════════════════════
 * Blue Noise Dither Texture (64×64, void-and-cluster algorithm)
 * ═══════════════════════════════════════════════════════════════════
 *
 * Generated via the void-and-cluster algorithm (Ulichney 1993).
 * All spectral energy is concentrated in high frequencies — the human
 * visual system is least sensitive to high-frequency noise, making
 * blue noise dither perceptually invisible even at ±0.5 LSB amplitude.
 *
 * Uploaded once at startup as R8_UNORM (0-255 → 0.0-1.0 in shader).
 * Fragment shader tiles with frac(screen_pos / 64.0) for seamless wrap.
 */
static const uint8_t blue_noise_64[4096] = {
     53, 245,  22, 216, 180,  41, 124, 223, 111,  64, 205, 102, 177, 251,  54, 193, 123,  86, 154,  50,  78,  23,  94, 128,  70, 243,  86, 215,  26, 204,  64, 120,  47,  20, 236,  41, 192, 216, 132,  10,  76, 140,  18,  69, 225,  96, 242,  62, 179,  47, 163,  72, 200, 178,  79, 205,  50,  98, 138, 176,  35,  77, 215, 172,
     84, 185, 137,  58, 146, 198,  75, 173,   8, 185,  42, 227,   2, 153,  92, 218,  35, 233,  12, 210, 195, 169, 249, 188,  40, 165,   3, 109, 159,  96,  18, 187,  85, 201, 142, 173,  87, 107,  51, 222, 161, 112, 207, 176, 117,  25, 171, 131, 216,  84, 246, 138, 103,   2, 130, 157,  30, 245,  65, 223, 151, 193,  27, 108,
    221,   8, 112, 239,  87,  15, 236,  48, 156, 233, 120, 141,  79, 211,  21, 137,  71, 171, 102, 132,  63, 116,   7, 149, 220, 121, 192, 228,  52, 253, 129, 231, 160, 108,  63,   3, 230, 155,  30, 181,  93, 252,  37,  55, 139, 210,  78,  36, 109,   7, 187,  33, 231, 212,  25, 236, 117, 166,  15, 104,  52, 128, 233, 147,
     39, 160, 210,  31, 167, 106, 207, 130,  94,  69,  22, 171,  46, 182, 105, 240, 189,  47, 253, 162,  37, 229,  82,  51, 100,  28,  65, 141,  35, 174,  72,   7, 212,  33, 251, 122, 205,  71, 244, 119,  61,   2, 187,  87, 238,  10, 151, 251, 202, 159,  95, 127,  64, 152,  87,  48, 192,  89, 211, 182, 247,   2,  93,  62,
    253,  99,  73, 129, 189,  54, 148,  32, 243, 188, 213,  97, 247, 127,  60, 156,   5, 126,  89,  17, 213, 180, 137, 238, 205, 155, 245,  85, 116, 197, 149, 103,  53, 138, 177,  95,  45, 137,  15, 193, 149, 230, 130, 163, 104, 196,  48, 120,  65,  24, 237,  49, 196, 110, 180, 217,  71, 143,  39, 122,  80, 163, 205, 179,
    132, 200,  47, 227,   2, 251,  72, 175,  10, 117,  50, 147,   9, 192,  33, 228,  80, 219, 201, 150,  66,  97,  23, 168,  74,   8, 178, 211,  13, 223,  29, 244, 193,  80, 224,  24, 195, 167, 104, 217,  34,  76, 203,  20,  66, 176, 231,  86, 185, 134, 210, 170,  17, 253,  31, 135,   5, 241, 161,  19, 219,  45, 112,  21,
     76,  13, 175, 145, 113,  90, 210, 125, 223,  85, 164, 232,  75, 207,  95, 118, 179,  56,  31, 110, 246, 129, 198,  41, 115, 133,  54, 102, 146,  60,  90, 169, 115,  11, 156, 126,  76, 235,  53,  88, 175, 113,  48, 245, 126,  31, 143,   3, 224,  42,  80, 103, 144,  76, 164,  99, 224,  62, 103, 195, 136,  67, 239, 152,
    224, 105, 240,  67, 201,  21, 163,  38,  64, 194,  18, 110,  39, 136, 160,  21, 242, 140, 167, 188,  48,   2, 221,  89, 251, 188, 230,  22, 166, 236, 132,  44, 221,  66, 242,  40, 210,   4, 145, 251,  16, 141, 220, 158,  80, 212,  98, 166, 111, 155, 246,   6, 203, 228,  53, 127, 201, 171,  43, 250,  88, 173,  32, 190,
     54, 128,  28, 165,  44, 135, 234, 105, 151, 246, 133, 215, 179, 251,  52, 212,  73,  10,  95, 228,  79, 144, 175,  62, 157,  29,  81, 199, 118,  74, 189,  23, 144, 187,  94, 176, 107, 163, 124, 197,  66, 184,  93,   8, 191,  44, 252,  59, 206,  29, 130,  66, 117,  35, 180,  11,  85,  23, 119, 148,   7, 229, 119,  92,
    208, 150, 217,  97, 248, 186,  76,   5, 181,  54,  32,  98,  68,   2,  90, 173, 114, 203, 128,  27, 206, 240, 100,  17, 216, 111, 148,  43, 216,   2, 249, 102, 210,  53, 131,  26, 232,  47,  82,  29, 228, 121,  39, 241, 109, 131, 182,  15,  78, 172, 231, 187, 156,  90, 244, 209, 151, 237, 184,  75, 212,  57, 160,  11,
     38, 180,  79,   7, 118,  57, 221, 127, 209,  84, 231, 169, 202, 130, 235, 150,  40, 249,  53, 160,  67, 120,  37, 137, 185,  58, 238, 175,  98,  57, 153, 171,  82,   7, 253, 150,  71, 183, 209, 104, 164,  58, 213, 150,  27,  69, 145, 224, 122,  95,  45,  20, 221,  58, 136, 106,  67,  46, 220,  29, 107, 192, 134, 249,
    109,  59, 231, 142, 173,  32, 155,  99,  21, 147, 117,  17, 156,  45, 108,  24, 190,  86, 142, 182,  16, 196, 166, 232,  77,   9, 126,  25, 138, 230, 122,  38, 224, 116, 198,  90, 221,  12, 141, 248,   7, 134,  79, 199, 170, 237,  93,  37, 199, 243, 149, 107, 197,  13, 164,  28, 179, 128,  91, 141, 170,  41,  78, 201,
    164, 126,  27, 206,  71, 197, 238,  49, 192, 252,  61, 213,  78, 226, 182,  65, 229,   5, 216, 105, 244,  87,  51, 106, 198, 253,  92, 212, 189,  80,  16, 194,  65, 141,  21, 168,  60, 125,  42,  89, 180, 235,  34, 102,   2,  51, 210, 156,  11,  60, 185,  74, 125, 253,  80, 213, 239,  15, 197, 252,   2, 230, 100,  20,
    239, 185,  90, 246, 107,  15, 133,  81, 172,  39, 103, 184, 126,   8, 144,  98, 165, 121,  72,  37, 135, 209,   3, 157,  33, 144, 167,  67,  35, 159, 245,  93, 179, 238,  45, 111, 206, 242, 158, 196,  52, 113, 155, 226, 138, 186, 114,  75, 179, 132,  29, 229,  42, 176, 101,  53, 117, 154,  70,  49, 123, 204, 149,  66,
    138,   4,  52, 149,  39, 167, 217, 113,   3, 225, 139,  30, 242,  57, 197, 253,  42, 207, 154, 234,  62, 170, 121, 223,  59, 117,  18, 223, 112,  55, 204, 132,  10, 158,  77, 188,  30, 100,  16,  73, 214,  19, 175,  59,  84, 253,  19, 218, 100, 247,  86, 208, 147,   2, 137, 191,  36, 224, 106, 184,  85, 166,  48, 219,
     81, 195, 115, 222, 190,  93,  55, 240, 156,  73, 206,  90, 166, 116,  83,  18, 134,  93,  12, 186,  24,  99, 249,  75, 182, 206,  85, 178, 238, 146,  27, 109,  60, 212, 124, 229, 146, 178, 225, 120, 143, 245,  95, 201,  37, 123, 161,  44, 144,   7, 167, 110,  64, 202, 233,  74, 163,  11, 208, 145,  25, 235,  12, 111,
     36, 253, 170,  68,   9, 137, 202,  20, 127,  50, 178,  12, 219,  37, 231, 180,  56, 171, 217,  82, 144, 194,  36, 136,  11, 240,  41, 129,   2,  89, 218, 177, 252,  34,  91,   2,  68,  48,  87, 167,  38,  63, 132,  10, 223, 182,  68, 230, 196,  57, 222,  21, 161,  95,  27, 127, 240,  91,  41, 247,  61, 119, 180, 207,
    159,  98,  22, 124, 243,  78, 162, 102, 188, 249, 112, 151,  63, 131, 155, 206, 108, 244,  36, 120, 239,  55, 215,  90, 161, 104, 148,  70, 199, 165,  46,  74, 142, 196, 160, 241, 134, 193, 253,   6, 208, 184, 230, 109, 146,  89,  21, 106, 129,  81, 182, 121, 249,  48, 215, 185,  55, 115, 172, 130, 193,  94, 144,  69,
    214,  52, 227, 154,  45, 208,  27, 227,  40,  86,  24, 234, 189,  97,   2,  72,  22, 141,  67, 165,   4, 105, 173,  23, 230,  54, 183, 251,  33, 118, 237, 102,  13, 114,  52, 208,  98,  29, 117, 152, 103,  83,  30, 170,  52, 197, 245, 173,  27, 238,  41, 144,  71, 171, 106,   8, 153, 203,  75,   3, 226,  33, 238,  16,
    106, 134, 194,  89, 177, 107, 147,  67, 171, 215, 138,  75,  46, 211, 250, 124, 195, 223,  95, 191, 227, 149,  68, 193, 116, 207,  10,  97, 217, 145,  23, 189, 226, 173,  80,  17, 170, 218,  71, 235,  49, 137, 248,  73, 212,   3, 135,  49, 153, 205,  96, 192,  18, 234, 135,  84, 250,  29, 219, 103, 162,  54,  85, 186,
    244,   2,  74,  31, 216,  12, 252, 131,  97,   4, 196, 118, 176,  26, 160,  84,  34, 153,  51,  17, 127,  43, 253, 138,  38,  79, 133, 173,  52,  83, 162,  63, 131,  36, 235, 122, 147,  44, 189,  13, 161, 200,  22, 120, 158, 101,  76, 216, 110,  64,   4, 225, 115,  39, 197,  62, 177, 141,  46, 184, 134, 206, 121, 152,
     43, 168, 231, 142, 119,  63, 189,  47, 231, 158,  37, 246, 143, 105,  55, 233, 178, 107, 247, 208,  76, 185,  92,  18, 236, 156, 225,  25, 124, 202, 248,   4, 216,  97, 197,  61, 249,  86, 132, 221, 113,  62, 180, 218,  38, 233, 186,  15, 247, 177, 130, 155,  83, 164, 213,  22, 120,  93, 239,  68,  14, 250,  25,  66,
    126, 204, 101,  54, 240, 153,  91,  21, 201, 110,  59,  89,  11, 224, 193, 131,  10,  64, 136, 168,  33, 112, 218, 171,  61, 105, 193,  72, 232,  37, 105, 140,  74, 157,  14, 181, 106,  20, 175,  39,  94, 241,   9,  85, 143,  59, 163, 125,  85,  45, 202,  31, 253,  59, 104, 149, 233,   6, 199, 115, 157,  93, 174, 224,
     81,  19, 157, 195,   7, 179, 218, 125,  70, 177, 237, 208, 154,  70,  38,  97, 200, 218,  25,  89, 237, 151,  10, 126, 204,  44,   6, 146, 180,  88, 166, 194,  49, 240, 125,  39, 206, 152, 228,  74, 208, 165, 131, 107, 254,  22, 204,  35, 147, 234,  70, 109, 186,   9, 226,  42, 190,  80, 165,  34, 230,  50, 110, 190,
     38, 254,  88,  42, 114,  76,  34, 247, 151,  14, 133,  29, 186, 117, 171, 252,  78, 161, 119, 191,  52, 201,  71, 243,  88, 167, 251, 116,  57,  15, 243,  29, 112, 214, 169,  83, 234,  58, 123,   2, 143,  30,  49, 197, 172,  80, 115, 228,  96, 169,  19, 221, 148, 123,  70, 173, 129,  55, 213, 142,  77, 202,   5, 142,
    217, 172, 134, 212, 234, 139, 169, 101,  52, 225,  78, 100,  51, 235,   3, 134,  33,  56, 244,   5, 139, 100,  38, 156,  27, 132, 211,  96, 198, 219, 125, 152,  85,   9,  64, 143,  26, 100, 198, 245,  97, 182, 237,  69,  14, 219, 159,  65,   6, 196, 136,  82,  39, 208,  95, 246,  13, 109, 240,  18, 125, 170, 243,  64,
    115,  11,  59, 184,  17,  62, 208,   1, 199, 115, 170, 204, 145, 215,  83, 157, 225, 108, 176,  82, 227, 168, 213, 113, 189,  75,  49,  19, 158,  78,  46, 228, 174, 196, 254, 110, 185, 154,  43, 168,  56, 118, 215, 149, 101, 133,  47, 187, 250, 105,  52, 239, 182, 158,  26, 140, 203, 163,  92,  62, 226,  40,  94, 158,
     77, 232, 125,  95, 155, 109, 241,  84, 157,  24, 254,  41,  17, 108,  60, 187,  19, 205, 147,  37, 123,  18,  59, 235,   1, 220, 173, 236, 136, 187,   4, 102,  57, 128,  41, 210,  11, 238,  86,  19, 207,  78,   6,  42, 176, 244,  28, 121, 150,  25, 213, 120,   4,  62, 218,  84,  51,  33, 195, 147, 186, 112,  23, 207,
     49, 191,  31, 248,  45, 192,  29, 126, 185,  55, 137,  75, 192, 163, 243, 121,  48,  95,  65, 236, 199,  77, 177, 134,  90, 147, 110,  31,  62, 113, 203, 247, 148,  24,  91, 165,  72, 126, 193, 140, 252, 161, 129, 233,  87, 195,  67, 223,  86, 177,  68, 165,  98, 249, 114, 181, 229, 122, 244,   7,  82, 215, 136, 178,
    106, 146, 214, 167,  82, 222, 145,  68, 235,  97, 215, 118, 229,  88,  11, 211, 140, 250, 181,  25, 154, 101, 254,  41, 202,  54, 248,  86, 213, 165,  35,  81, 177, 215, 235, 139,  50, 226, 106,  64,  29, 102, 190,  59,  17, 112, 144,   8, 205,  43, 231, 135, 195,  42, 153,  13,  70, 159, 103,  57, 168,  44, 250,   3,
    234,  87,  18,  57, 116,   8, 176,  38, 207,   6, 174,  22,  56, 150,  40, 172,  75,   1, 111, 130,  49, 212,  10, 117, 161,  19, 188, 130,  12, 240, 142, 119,  18,  61, 110, 190,  30, 178,   9, 218, 170,  45, 227, 152, 214, 169, 240,  97, 163, 116,  12,  83,  27, 211,  90, 134, 194,  28, 209, 139, 228, 125,  73, 159,
     60, 129, 188, 140, 205, 243, 101, 121, 158,  79, 142, 247, 188, 129, 202, 105, 230, 160, 221, 194,  72, 169, 136, 229,  84, 221,  68, 153,  98,  50,  71, 228, 198, 159,   1,  82, 249,  97, 148, 119, 200,  91,  23, 121,  73,  33,  50, 133,  62, 254, 183, 144, 241,  61, 174, 234,  47, 254,  92,  12, 191,  30, 100, 201,
    224,  38, 254,  75, 162,  25,  61, 196, 239,  49, 111,  31,  96,  70, 242,  28,  57,  91,  42,  18, 232,  94,  34,  60, 195, 114,  29, 233, 168, 208, 183,  28,  90, 243, 135, 204, 162,  58,  36, 241,  67, 138, 178, 250,  95, 207, 177, 225,  28, 199, 105,  48, 161, 119,   1, 108,  77, 149, 179,  64, 114, 242, 176,  16,
    116, 171,   9, 109,  44, 224,  92, 135,  17, 183, 213, 162, 222,  13, 154, 121, 183, 208, 152, 108, 183, 143, 246, 172,  13, 145, 181,  44, 123,   4, 109, 153, 122,  41,  70,  26, 124, 219, 185,  86,   3, 231,  54,  16, 148, 125,   1,  88, 155,  75,  16, 226,  92, 193, 221,  34, 214, 127,  20, 225, 160,  81,  50, 148,
     66,  97, 217, 196, 127, 181, 155,  36, 219,  87,  66, 123,  45, 193,  85, 235,   8, 134,  68, 243,  53,   5,  77, 110, 209,  93, 252,  75, 217,  87, 248,  58, 191, 222, 172, 237, 104,  13, 155, 128, 204, 158, 108, 219, 198,  64, 246, 190, 118, 211, 129, 175,  23,  71, 136, 159, 184,  54, 101, 202,  37, 138, 219, 198,
    237, 143,  54,  83, 241,   1,  70, 251, 107, 150,   5, 178, 249, 139,  59, 171, 102, 220,  28, 164, 122, 197, 218, 155,  32,  56, 132,  17, 149, 198,  35, 163,  19,  98, 143,  50, 189,  73, 254,  47,  99,  30, 185,  80,  40, 166, 104,  52,  33, 242,  60, 146, 232,  44, 249,  88,   9, 238, 152,  75, 250,   1,  92,  28,
    187,  13, 164,  30, 148,  99, 206, 172,  54, 201, 229,  94,  22, 109, 216,  31,  50, 190,  79, 227,  96,  43, 133,  66, 238, 188, 222, 105, 179,  63, 127, 232,  72, 202,  11,  86, 216, 119,  21, 174, 223,  66, 242, 118, 139,  11, 233, 151, 182,  85,   6, 201,  95, 124, 203,  59, 118, 196,  24, 125, 183, 112, 168, 124,
     74, 248, 113, 191, 230,  41, 120,  16, 139,  32, 125,  62, 161, 200,  76, 130, 254, 111, 149,  20, 185, 249,  23, 175, 101,   1, 162,  45, 243,  20, 101, 211, 139, 112, 246, 169,  35, 150, 202,  83, 131, 151,  19, 173, 225,  73, 204,  24, 135, 225, 107, 160,  30, 179,  14, 147, 227,  69, 174,  43, 213,  61, 230,  42,
    138,  93, 220,  63, 132,  77, 186, 237,  82, 167, 241, 189,  42, 226,   1, 157, 179,  12, 208,  57, 142,  85, 115, 225, 148,  81, 123, 204,  90, 144, 168,   5,  43, 183,  61, 123, 233, 100,  59, 244,   8, 214,  56,  96,  34, 187,  91, 116,  67, 168,  44, 252,  70, 217, 103, 166,  34,  96, 240, 146,  84,  22, 154, 206,
    173,  49,  24, 178,  10, 216, 157,  58, 211, 101,   9,  72, 113, 141,  88, 236,  64,  92, 124, 237, 166,   6, 210,  51,  33, 197,  61,  26, 220,  68, 194, 254,  84, 227,  28, 160,   1, 191,  39, 122, 166, 105, 194, 251, 156, 127,  47, 247, 209,  14, 190, 119, 143,  48, 242,  74, 212, 133,   5, 110, 192, 247, 103,   7,
    119, 202, 158, 108, 242,  94,  26, 116,  40, 136, 203, 154, 250,  29, 187, 119,  43, 220, 194,  38, 106,  65, 189, 137, 252, 166, 229, 132, 174,  35, 118,  56, 130, 150,  96, 200,  79, 141, 226, 183,  68,  26, 139,  78,   1, 214, 166,  20, 149,  97, 222,  81,   1, 199, 128,  17, 189,  55, 171, 220,  51, 136,  69, 227,
     34, 252,  76, 139,  53, 196, 143, 254, 181, 224,  22,  91, 170,  57, 207,  17, 162, 133,  25,  77, 175, 244, 122,  96,  18,  85, 110,   8, 242,  98, 160, 210,  24, 178, 219,  45, 241, 108,  23,  91, 211, 237,  42, 111, 226,  65, 102, 192,  53, 131,  35, 157, 234, 175,  88, 156, 106, 254,  80,  30, 160,  14, 186,  89,
     59, 181,   5, 212,  32, 169,  72,   1,  88,  61, 127,  44, 220, 130,  77, 228, 101, 251, 184, 152, 217,  15,  40, 161, 207,  55, 184, 152,  72, 199,  14, 239, 108,  71,  12, 124,  63, 177, 152,  51, 127, 162, 197, 146, 176,  33, 136, 239,  76, 231, 184,  64, 104,  32,  58, 229,  24, 139, 205, 123, 234, 107, 212, 146,
    123, 154,  98, 236, 130, 112, 209, 232, 153, 174, 245, 194, 107,   4, 181, 143,  50,  70,   5, 116,  90, 139, 228,  70, 239, 131,  31, 216,  46, 141,  87,  41, 185, 143, 250, 168, 213,  15, 200, 254,   5,  83,  61,  15, 248,  88, 204,   9, 169, 117,  21, 215, 137, 249, 123, 202, 164,  41,  66,  92, 175,  55,  27, 238,
     19, 222,  46,  80, 186,  58,  18, 100,  37, 117,  13,  74, 161, 233,  93,  29, 170, 205, 235,  43, 197,  58, 172, 113,   8, 192,  99, 250, 114, 170, 227, 133,  55, 201,  81,  37,  96, 137,  78, 111, 186, 231, 102, 209, 118,  52, 155, 106,  40, 205,  91, 163,  13, 191,  83,   4,  99, 182, 241,  10, 200, 140,  83, 192,
     72, 109, 199, 163,  25, 245, 135, 178, 201,  83, 213, 142,  37,  62, 208, 245, 108, 129,  86, 146, 222,  22,  93, 203, 147,  79, 163,  63,   1, 209,  25, 104, 232,   4, 114, 160, 234,  49, 221,  26, 145,  36, 131, 164,  27, 191, 228,  69, 252, 147,  53, 232,  73,  43, 151, 210,  53, 222, 127, 156,  36, 251, 116, 164,
     41, 247,  10, 121, 218, 154,  73, 229,  50, 129,  30, 240, 185, 126, 150,  48,   9, 192,  27,  65, 176, 123, 253,  35,  52, 234,  26, 198, 127,  81, 187,  68, 150, 174, 209,  26, 128, 189, 157,  63, 176, 218,  56, 244,  76, 140,  17, 175, 123,   6, 198, 110, 131, 177, 227, 112, 140,  27,  77, 104, 218,  65,   3, 211,
    129, 177, 144,  65,  92,  38, 111,  12, 150, 221, 169, 104,  85,  14, 196,  77, 166, 230, 153, 244, 101,   1, 154, 215, 133, 176, 104, 152, 241,  49, 161, 248,  36,  94,  60, 243,  75,   6,  99, 247, 114,  86,  10, 183,  99, 217,  50,  94, 210,  79, 180,  24, 245,  89,  15,  63, 251, 174, 197,  48, 170, 134, 186,  99,
    220,  82,  34, 241, 206, 165, 193, 254,  94,  67,   3, 201,  55, 252, 115, 221,  98,  58, 117,  40, 195,  78, 182,  63,  88,  13, 220,  35,  91, 218, 133,  20, 121, 221, 138, 181, 109, 229,  40, 199,  20, 135, 211, 154,  40, 128, 238, 145,  31, 241, 138,  65, 157,  38, 199, 162,  94,   8, 118, 239,  16,  89, 234,  54,
     14, 198, 159, 103,   0,  56, 126,  24, 182, 119, 156, 233, 138, 175,  42,  19, 137, 189,  14, 213, 128, 229,  26, 115, 242, 189, 124,  71, 172,  11, 102, 203,  78, 188,  11,  44, 164, 206, 142,  79, 164, 236,  66, 107, 200,   0, 187,  66, 167, 103,  47, 217, 188, 108, 230, 128,  40, 211, 142,  67, 214, 160,  31, 149,
    119,  61, 226, 132, 180, 235,  74, 211,  46, 226,  34,  76,  21,  96, 153, 204, 246,  86, 173,  69, 159,  49, 140, 205,  43, 150,  54, 238, 141, 184,  45, 237, 165,  56, 253,  91, 128,  23,  58, 187, 122,  44, 179,  23, 254,  83, 113, 220,  17, 200, 120,   3,  81, 144,  18,  72, 237, 183,  84,  22, 187, 110,  75, 250,
     94, 188,  22,  78,  42, 152, 102, 137, 167,  91, 193, 131, 214, 186,  67, 121,  51,  31, 234,  96,   8, 248,  81, 167,   5, 101, 200,  24, 108, 214,  67, 145,  16, 114, 151, 212,  68, 225, 104, 250,   7,  95, 222, 140,  57, 152, 174,  43,  90, 148, 227, 169, 255,  50, 218, 170, 107,  52, 157, 243, 130,  47, 204, 174,
     42, 151, 243, 114, 223, 202,  29, 248,   7,  63, 238, 106,  45, 242,   6, 224, 163, 109, 142, 219, 122, 193, 108, 222,  74, 255, 159,  84, 228,   7, 123, 196,  97, 223,  35, 190,   0, 171, 147,  37, 211, 155,  77, 118, 205,  30, 231, 133, 247,  60,  26,  69, 131, 100, 195,  32, 138,   0, 199,  95,  28, 223, 140,   4,
    124, 209,  56, 172,   9,  90,  60, 187, 116, 149, 177,  16, 161,  79, 139,  94, 198,  18, 181,  57,  36, 172,  21,  56, 126, 184,  36, 136,  55, 164,  81, 246,  28, 175,  77, 135,  99, 238,  81, 130, 195,  54, 244,  10, 183, 100,  70,   6, 190, 162, 114, 210, 179,   9, 153,  87, 248, 121, 221,  65, 170, 104,  69, 238,
     84,  19, 100, 142, 195, 125, 159, 214,  82,  32, 221,  57, 125, 209, 174,  35,  63, 253,  80, 209, 151,  92, 239, 143, 215,  14, 113, 235, 179, 207,  39, 153,  60, 121, 243,  47, 205,  26,  57, 181,  24, 111, 168,  41, 236, 158, 124, 208,  98,  46, 236,  88,  39, 242,  64, 209, 184,  76,  36, 146, 252,  20, 199, 156,
    220, 181, 255,  34,  71, 239,  15,  46, 244, 132, 197,  91, 248,  21, 112, 232, 155, 116, 135,  14, 230,  66, 196,  39, 165,  93, 203,  73,  22,  94, 133, 219, 190,  19, 149, 180, 112, 157, 216, 120, 232,  71, 214, 128,  87,  51, 224,  28, 180, 139,  12, 125, 201, 145, 113,  46,  16, 168, 103, 194,  55, 119, 178,  49,
    112, 134,  65, 160, 212,  96, 141, 174, 106,  69,   0, 144, 183,  71,  48, 203,   4, 192,  46, 170, 105, 130,   3, 111,  68, 245,  48, 157, 119, 251,   3, 107,  73, 228,  92,  10,  67, 255,  84,   5, 153,  98,  19, 144, 200,  12, 172,  85, 251,  72, 217, 168,  76,  23, 232, 162, 129, 239, 215,   6, 136, 230,  79,   9,
    236,  39, 200,   0, 120,  51, 232,  21, 194, 162, 236,  39, 118, 226, 148,  99,  74, 242,  89, 217,  33, 246, 158, 225, 181, 134,   8, 190, 224,  60, 186, 166,  47, 200, 131, 235, 191,  32, 137, 200,  51, 240, 192,  59, 246, 111, 148,  58, 117, 154,  33,  55, 188,  96, 217,  69,  92,  32,  63, 159,  89,  34, 206, 150,
    169,  82, 103, 226, 184,  77, 204, 127,  88,  51, 210, 102, 166,  13, 215, 176, 138,  27, 183, 145,  73, 190,  47,  83,  28, 210,  99, 143,  82,  34, 137, 241,  89,  26, 162,  43, 122, 171, 102, 224, 179,  36, 117, 173,  78,  34, 207, 233,   0, 194, 225, 109, 248, 140,   3, 175, 202, 147, 115, 191, 246, 172, 102,  62,
     21, 247, 145,  32, 167,  17, 155,  35, 223, 146,  25,  65, 255,  85,  36,  58, 113, 223,  53, 120,  15, 100, 206, 151, 115,  56, 237,  23, 169, 214, 115,  15, 146, 212, 107,  79, 219,  59,  20,  73, 129,  87, 156,  14, 227, 136,  92, 184,  47,  98, 135,  14, 159,  43, 122,  53, 255,  14, 219,  45,  71,  11, 128, 214,
    116, 185,  60, 126, 239,  93, 114, 245,  73, 178, 122, 204, 141, 187, 128, 236, 191,  17, 156, 250, 167, 231, 132,   7, 255, 171, 124,  70, 194,  97,  51, 227,  68, 173, 250,   0, 142, 198, 236, 165,   8, 248, 203, 105,  53, 177,  20, 124, 162, 245,  80, 180,  68, 213, 194, 100, 133,  80, 167, 105, 140, 229, 193,  51,
    222,  13,  87, 198,  48, 214,  60, 186,   4, 106, 234,  16,  50,  99,   6, 163,  82, 104, 204,  66,  90,  32,  61, 188,  79,  30, 203, 151,  11, 249, 158, 191, 129,  31,  53, 182,  93,  37, 115, 145, 195,  64,  33, 216, 146, 255,  69, 220,  31,  58, 209,  25, 237,  88,  30, 228, 180,  40, 241,  23, 183,  91,  32, 151,
     99, 165, 232, 152,   7, 164, 134, 207,  46, 165,  83, 195, 158, 229, 206,  67, 245,  44, 136,   0, 178, 216, 111, 159, 226, 103,  45, 233, 113,  40,  81,   6,  95, 205, 111, 222, 154, 246,  82,  28, 101, 174, 126,  84,   4, 114, 199,  89, 140, 191, 101, 150, 120, 168, 141,  12,  69, 153, 120, 203,  56, 161, 252,  69,
    201,  38, 122,  74, 103, 255,  25,  87, 148, 247,  27, 132,  72,  32, 114, 148,  20, 182, 222, 113, 240, 145,  46, 207,  16, 141, 181,  61, 135, 176, 223, 147, 244, 164,  74, 126,  16,  62, 169, 203, 231,  46, 240, 158, 190,  44, 153,  12, 233, 117,  15, 225,  38,  56, 249, 110, 188, 220,   5,  86, 235, 118,   9, 135
};


/* ═══════════════════════════════════════════════════════════════════
 * HLSL Shader Sources (compiled at runtime via shadercross)
 * ═══════════════════════════════════════════════════════════════════
 *
 * SDL_GPU binding convention (CRITICAL — wrong spaces = black screen):
 *   Fragment textures/samplers: space2 (SPIR-V set 2)
 *   Fragment uniform buffers:   space3 (SPIR-V set 3)
 *   Vertex textures/samplers:   space0 (SPIR-V set 0)
 *   Vertex uniform buffers:     space1 (SPIR-V set 1)
 *
 * CRITICAL: One SamplerState per Texture2D, always. SDL_GPU / SPIRV-Cross
 * counts "samplers" as texture+sampler pairs. Sharing one SamplerState
 * across multiple textures causes unpaired textures to be misclassified
 * as storage textures and bound to wrong slots.
 */

/* Fullscreen quad — generates 4 vertices from SV_VertexID, no vertex buffer.
 * Draw with SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0) as triangle-strip. */
static const char hlsl_fullscreen_vert[] =
    "struct VSOutput {\n"
    "    float4 pos : SV_Position;\n"
    "    float2 uv  : TEXCOORD0;\n"
    "};\n"
    "\n"
    "VSOutput main(uint id : SV_VertexID) {\n"
    "    VSOutput o;\n"
    "    o.uv  = float2((id & 1), (id >> 1));\n"
    "    o.pos = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);\n"
    "    o.uv.y = 1.0 - o.uv.y;\n"  /* flip Y: video is top-left origin */
    "    return o;\n"
    "}\n";

/* Planar YUV420P fragment shader — Lanczos-2 luma, Catmull-Rom chroma, blue noise dither.
 *
 * Luma (Y): Lanczos-2 windowed sinc, 4×4 texel kernel (16 taps).
 *           Preserves sharp detail during downscaling.
 *
 * Chroma (U, V): Catmull-Rom bicubic, 4×4 texel kernel (16 taps).
 *           Smoother than Lanczos without ringing at chroma block
 *           boundaries. Standard for chroma in quality video players.
 *
 * Output: Blue noise dither (±0.5 LSB) from 64×64 void-and-cluster
 *         texture before 8-bit quantization. All spectral energy in
 *         high frequencies — perceptually invisible, superior to IGN.
 *
 * SampleLevel(s, uv, 0) forces mip level 0. */
static const char hlsl_yuv_planar_frag[] =
    "Texture2D<float> texY : register(t0, space2);\n"
    "Texture2D<float2> texU : register(t1, space2);\n"
    "Texture2D<float2> texV : register(t2, space2);\n"
    "Texture2D<float> texNoise : register(t3, space2);\n"
    "SamplerState sampY : register(s0, space2);\n"
    "SamplerState sampU : register(s1, space2);\n"
    "SamplerState sampV : register(s2, space2);\n"
    "SamplerState sampNoise : register(s3, space2);\n"
    "#if DSVP_PQ_LUT\n"
    "/* SDR-in-PQ encode LUTs (REVIEW-PERF §3 design, re-corrected): the\n"
    " * 9-pow/pixel encode chain measured as the whole 60fps drop cost\n"
    " * (DSVP_PQ_NOMATH discriminator, 2026-08-20). BOTH LUTs live in\n"
    " * sqrt-ish domains: texLutLin stores x^1.2 (squared here to get\n"
    " * x^2.4) because an R16-stored straight x^2.4 quantises shadows\n"
    " * to ~1.5e-5 steps that PQ's near-black slope amplifies to 5-17\n"
    " * ten-bit codes (simulated); this shape keeps the whole chain\n"
    " * under 0.4 codes at any nits, below the dither floor. texLutPq\n"
    " * is the PQ OETF indexed by sqrt(linear); the out_pq nits scale\n"
    " * is BAKED IN at build time. saturate() clamps filter overshoot\n"
    " * the pow path propagated — eye-test note in the review doc. */\n"
    "Texture2D<float> texLutLin : register(t4, space2);\n"
    "Texture2D<float> texLutPq : register(t5, space2);\n"
    "SamplerState sampLutLin : register(s4, space2);\n"
    "SamplerState sampLutPq : register(s5, space2);\n"
    "float lut_coord(float x) { return x * (1023.0/1024.0) + (0.5/1024.0); }\n"
    "float lut_lin(float x) {\n"
    "    float t = texLutLin.SampleLevel(sampLutLin,\n"
    "        float2(lut_coord(saturate(x)), 0.5), 0);\n"
    "    return t * t;  /* stored x^1.2 -> x^2.4 */\n"
    "}\n"
    "float lut_pq(float y) {\n"
    "    return texLutPq.SampleLevel(sampLutPq,\n"
    "        float2(lut_coord(sqrt(saturate(y))), 0.5), 0);\n"
    "}\n"
    "#endif\n"
    "\n"
    "cbuffer Params : register(b0, space3) {\n"
    "    row_major float4x4 colorMatrix;\n"
    "    float2 rangeY;\n"
    "    float2 rangeUV;\n"
    "    float2 texSizeY;\n"
    "    float2 texSizeUV;\n"
    "    float2 chromaOffset;\n"
    "    float frameCount;\n"
    "    float is_hdr;\n"
    "    float hdr_peak_nits;\n"
    "    float hdr_gamut;\n"
    "    float hdr_debug;\n"
    "    float hdr_target_nits;\n"
    "    float hdr_midtone_gain;\n"
    "    float is_dovi;\n"
    "    float is_semiplanar;\n"
    "    float out_gamma;\n"
    "    float is_hlg;\n"
    "    float hdr_pass;\n"
    "    float out_gamut;\n"
    "    float out_pq;\n"
    "    float4 dovi_num_pieces;\n"
    "    float4 dovi_pivots[9];\n"
    "    float4 dovi_c0[8];\n"
    "    float4 dovi_c1[8];\n"
    "    float4 dovi_c2[8];\n"
    "    float4 dovi_ycc_r0;\n"
    "    float4 dovi_ycc_r1;\n"
    "    float4 dovi_ycc_r2;\n"
    "    float4 dovi_out_r0;\n"
    "    float4 dovi_out_r1;\n"
    "    float4 dovi_out_r2;\n"
    "    float4 dovi_mmr_meta;\n"
    "    float4 dovi_mmr_ct[6];\n"
    "    float4 dovi_mmr_cp[6];\n"
    "};\n"
    "\n"
    "#define PI 3.14159265358979\n"
    "\n"
    "float lanczos2(float x) {\n"
    "    x = abs(x);\n"
    "    if (x < 1e-6) return 1.0;\n"
    "    if (x >= 2.0) return 0.0;\n"
    "    float pix = PI * x;\n"
    "    return (sin(pix) * sin(pix * 0.5)) / (pix * pix * 0.5);\n"
    "}\n"
    "\n"
    "float sample_lanczos(Texture2D<float> tex, SamplerState samp,\n"
    "                     float2 uv, float2 tex_size) {\n"
    "#if DSVP_DIRECT\n"
    "    /* Exact-1:1 variant: viewport == source, so pixel centers sit\n"
    "     * on texel centers and the whole Lanczos kernel collapses to\n"
    "     * the identity — one fetch replaces 16. Only ever BOUND when\n"
    "     * the selection site measures an exact match; upscale and\n"
    "     * downscale keep their kernels. Chroma is NOT direct: its 2x\n"
    "     * upsample keeps the full Catmull-Rom kernel — bilinear chroma\n"
    "     * was rejected on sight-unseen principle (Holden 2026-08-20);\n"
    "     * the picture is bit-identical to the fixed path. */\n"
    "    return tex.SampleLevel(samp, uv, 0).r;\n"
    "#elif DSVP_DILATE\n"
    "    /* Downscale dilation: when one output pixel spans df > 1 source\n"
    "     * texels, a fixed 4-tap kernel undersamples high frequencies and\n"
    "     * aliases (moire on 4K content in a small window / the internal\n"
    "     * panel). Stretch the kernel by the pixel footprint, measured\n"
    "     * from screen-space derivatives — exact under letterboxing and\n"
    "     * aspect scaling. This variant is only BOUND when downscaling:\n"
    "     * its dynamic [loop] bounds cost real GPU time even at df == 1\n"
    "     * (field-measured: broke 4K60 under software-decode UMA\n"
    "     * contention), so 1:1/upscale binds the fixed variant below. */\n"
    "    float2 df = float2(max(1.0, abs(ddx(uv.x)) * tex_size.x),\n"
    "                       max(1.0, abs(ddy(uv.y)) * tex_size.y));\n"
    "    df = min(df, 4.0);  /* 16 taps/axis cap — total work stays\n"
    "                           bounded because output pixels shrink as\n"
    "                           fast as taps grow */\n"
    "    float2 pos  = uv * tex_size - 0.5;\n"
    "    float2 base = floor(pos);\n"
    "    float2 fr   = pos - base;\n"
    "\n"
    "    int2 jmin = int2(floor(fr - 2.0 * df)) + 1;\n"
    "    int2 jmax = int2(floor(fr + 2.0 * df));\n"
    "\n"
    "    float result  = 0.0;\n"
    "    float wsum    = 0.0;\n"
    "    float tap_min = 1e9;\n"
    "    float tap_max = -1e9;\n"
    "\n"
    "    [loop] for (int j = jmin.y; j <= jmax.y; j++) {\n"
    "        float wy = lanczos2((float(j) - fr.y) / df.y);\n"
    "        [loop] for (int i = jmin.x; i <= jmax.x; i++) {\n"
    "            float w  = lanczos2((float(i) - fr.x) / df.x) * wy;\n"
    "            float2 tc = (base + float2(float(i), float(j)) + 0.5)\n"
    "                        / tex_size;\n"
    "            float s  = tex.SampleLevel(samp, tc, 0).r;\n"
    "            tap_min  = min(tap_min, s);\n"
    "            tap_max  = max(tap_max, s);\n"
    "            result  += s * w;\n"
    "            wsum    += w;\n"
    "        }\n"
    "    }\n"
    "#else\n"
    "    /* Fixed 4x4 — the proven-fast unrolled path (84ffc54). */\n"
    "    float2 pos  = uv * tex_size - 0.5;\n"
    "    float2 base = floor(pos);\n"
    "    float2 f    = pos - base;\n"
    "\n"
    "    float result  = 0.0;\n"
    "    float wsum    = 0.0;\n"
    "    float tap_min = 1e9;\n"
    "    float tap_max = -1e9;\n"
    "\n"
    "#if DSVP_SCALE2X\n"
    "    /* Exact 2.0x upscale: output phases per axis are only 0.25\n"
    "     * and 0.75, so the Lanczos-2 weights are two constant vectors\n"
    "     * (one the mirror of the other). Same 16 taps, same anti-ring,\n"
    "     * same wsum normalisation — identical arithmetic, minus the\n"
    "     * ~16 sin() per pixel the general path burns recomputing\n"
    "     * these constants 8.3M times a frame. */\n"
    "    const float4 W25 = float4(-0.084724804, 0.877354071,\n"
    "                              0.235346678, -0.017905185);\n"
    "    float4 wxv = (f.x < 0.5) ? W25 : W25.wzyx;\n"
    "    float4 wyv = (f.y < 0.5) ? W25 : W25.wzyx;\n"
    "#endif\n"
    "    [unroll] for (int j = -1; j <= 2; j++) {\n"
    "#if DSVP_SCALE2X\n"
    "        float wy = wyv[j + 1];\n"
    "#else\n"
    "        float wy = lanczos2(float(j) - f.y);\n"
    "#endif\n"
    "        [unroll] for (int i = -1; i <= 2; i++) {\n"
    "#if DSVP_SCALE2X\n"
    "            float wx = wxv[i + 1];\n"
    "#else\n"
    "            float wx = lanczos2(float(i) - f.x);\n"
    "#endif\n"
    "            float w  = wx * wy;\n"
    "            float2 tc = (base + float2(float(i), float(j)) + 0.5)\n"
    "                        / tex_size;\n"
    "            float s  = tex.SampleLevel(samp, tc, 0).r;\n"
    "            tap_min  = min(tap_min, s);\n"
    "            tap_max  = max(tap_max, s);\n"
    "            result  += s * w;\n"
    "            wsum    += w;\n"
    "        }\n"
    "    }\n"
    "#endif\n"
    "\n"
    "#if !DSVP_DIRECT\n"
    "    /* Shared tail of the kernel branches — the DIRECT build\n"
    "     * returned above and declares none of these (leaving it\n"
    "     * unguarded was the shared-source trap: undeclared wsum et\n"
    "     * al. failed the whole compile and the WARN fell back to\n"
    "     * fixed, silently). */\n"
    "    float filtered = (wsum > 0.0) ? result / wsum : 0.0;\n"
    "    /* Anti-ringing: clamp to local tap range. Strength 0.8 per\n"
    "     * Artoriuz's scaler benchmarks (mpv community). */\n"
    "    float clamped  = clamp(filtered, tap_min, tap_max);\n"
    "    return lerp(filtered, clamped, 0.8);\n"
    "#endif\n"
    "}\n"
    "\n"
    "/* Catmull-Rom weight for |t| in [0,2]. The dilated loop bounds\n"
    " * guarantee |t| <= 2 by construction, so no outside-range clamp\n"
    " * is needed. */\n"
    "float catmull_w(float t) {\n"
    "    t = abs(t);\n"
    "    return (t <= 1.0)\n"
    "        ? (1.5 * t * t * t - 2.5 * t * t + 1.0)\n"
    "        : (-0.5 * t * t * t + 2.5 * t * t - 4.0 * t + 2.0);\n"
    "}\n"
    "\n"
    "/* Catmull-Rom (bicubic) tap filter for chroma planes.\n"
    " * Smoother than bilinear without the ringing of Lanczos.\n"
    " * Standard for chroma upscaling in quality video players (mpv). */\n"
    "float sample_catmull(Texture2D<float2> tex, SamplerState samp,\n"
    "                     float2 uv, float2 tex_size) {\n"
    "#if DSVP_DILATE\n"
    "    /* Same footprint dilation as sample_lanczos — chroma aliases on\n"
    "     * downscale just like luma. df == 1 reproduces the original. */\n"
    "    float2 df = float2(max(1.0, abs(ddx(uv.x)) * tex_size.x),\n"
    "                       max(1.0, abs(ddy(uv.y)) * tex_size.y));\n"
    "    df = min(df, 4.0);\n"
    "    float2 pos  = uv * tex_size - 0.5;\n"
    "    float2 base = floor(pos);\n"
    "    float2 fr   = pos - base;\n"
    "\n"
    "    int2 jmin = int2(floor(fr - 2.0 * df)) + 1;\n"
    "    int2 jmax = int2(floor(fr + 2.0 * df));\n"
    "\n"
    "    float result = 0.0;\n"
    "    float wsum   = 0.0;\n"
    "    float tap_min = 1e9;\n"
    "    float tap_max = -1e9;\n"
    "\n"
    "    [loop] for (int j = jmin.y; j <= jmax.y; j++) {\n"
    "        float wy = catmull_w((float(j) - fr.y) / df.y);\n"
    "        [loop] for (int i = jmin.x; i <= jmax.x; i++) {\n"
    "            float w = catmull_w((float(i) - fr.x) / df.x) * wy;\n"
    "            float2 tc = (base + float2(float(i), float(j)) + 0.5)\n"
    "                        / tex_size;\n"
    "            float sm = tex.SampleLevel(samp, tc, 0).r;\n"
    "            tap_min  = min(tap_min, sm);\n"
    "            tap_max  = max(tap_max, sm);\n"
    "            result  += sm * w;\n"
    "            wsum   += w;\n"
    "        }\n"
    "    }\n"
    "\n"
    "    float out_c = (wsum > 0.0) ? result / wsum : 0.0;\n"
    "    /* Anti-ring: clamp to tap range, strength 0.8 — parity with\n"
    "     * luma (Artoriuz benchmarks). Rationale: PQ + wide gamut make\n"
    "     * chroma overshoot visible as edge color bleed. Since the\n"
    "     * SDR-in-PQ arc, SDR content rides a PQ/BT.2020 container on\n"
    "     * wide-gamut displays — the stated condition — so the clamp\n"
    "     * follows the CONTAINER (out_pq), not just the content\n"
    "     * (is_hdr). tap_min/max already accumulate unconditionally in\n"
    "     * the tap loop; the old is_hdr-only gate threw them away on\n"
    "     * every SDR frame (Knot gains #1, 2026-08-20).\n"
    "     * DSVP_CHROMA_AR=hdr restores the old predicate for A/B. */\n"
    "#ifdef DSVP_CHROMA_AR_HDR\n"
    "    if (is_hdr > 0.5)\n"
    "#else\n"
    "    if (is_hdr > 0.5 || out_pq > 0.5)\n"
    "#endif\n"
    "        out_c = lerp(out_c, clamp(out_c, tap_min, tap_max), 0.8);\n"
    "    return out_c;\n"
    "#else\n"
    "    /* Fixed 4x4 — the proven-fast unrolled path (84ffc54). */\n"
    "    float2 pos  = uv * tex_size - 0.5;\n"
    "    float2 base = floor(pos);\n"
    "    float2 f    = pos - base;\n"
    "\n"
    "    float result = 0.0;\n"
    "    float wsum   = 0.0;\n"
    "    float tap_min = 1e9;\n"
    "    float tap_max = -1e9;\n"
    "\n"
    "    [unroll] for (int j = -1; j <= 2; j++) {\n"
    "        float wy = catmull_w(float(j) - f.y);\n"
    "        [unroll] for (int i = -1; i <= 2; i++) {\n"
    "            float w = catmull_w(float(i) - f.x) * wy;\n"
    "            float2 tc = (base + float2(float(i), float(j)) + 0.5)\n"
    "                        / tex_size;\n"
    "            float sm = tex.SampleLevel(samp, tc, 0).r;\n"
    "            tap_min  = min(tap_min, sm);\n"
    "            tap_max  = max(tap_max, sm);\n"
    "            result  += sm * w;\n"
    "            wsum   += w;\n"
    "        }\n"
    "    }\n"
    "\n"
    "    float out_c = (wsum > 0.0) ? result / wsum : 0.0;\n"
    "    /* Anti-ring: clamp to tap range, strength 0.8 — parity with\n"
    "     * luma (Artoriuz benchmarks). Rationale: PQ + wide gamut make\n"
    "     * chroma overshoot visible as edge color bleed. Since the\n"
    "     * SDR-in-PQ arc, SDR content rides a PQ/BT.2020 container on\n"
    "     * wide-gamut displays — the stated condition — so the clamp\n"
    "     * follows the CONTAINER (out_pq), not just the content\n"
    "     * (is_hdr). tap_min/max already accumulate unconditionally in\n"
    "     * the tap loop; the old is_hdr-only gate threw them away on\n"
    "     * every SDR frame (Knot gains #1, 2026-08-20).\n"
    "     * DSVP_CHROMA_AR=hdr restores the old predicate for A/B. */\n"
    "#ifdef DSVP_CHROMA_AR_HDR\n"
    "    if (is_hdr > 0.5)\n"
    "#else\n"
    "    if (is_hdr > 0.5 || out_pq > 0.5)\n"
    "#endif\n"
    "        out_c = lerp(out_c, clamp(out_c, tap_min, tap_max), 0.8);\n"
    "    return out_c;\n"
    "#endif\n"
    "}\n"
    "\n"
    "/* Catmull-Rom returning float2 — for semi-planar UV (R16G16_UNORM).\n"
    " * Reads .rg from each tap: .r = U (Cb), .g = V (Cr). Same dilated\n"
    " * weights as sample_catmull — this is the sampler the zero-copy\n"
    " * P010 path uses, i.e. exactly what runs when 4K DV/HDR content is\n"
    " * downscaled on the internal panel. */\n"
    "float2 sample_catmull_rg(Texture2D<float2> tex, SamplerState samp,\n"
    "                         float2 uv, float2 tex_size) {\n"
    "#if DSVP_DILATE\n"
    "    float2 df = float2(max(1.0, abs(ddx(uv.x)) * tex_size.x),\n"
    "                       max(1.0, abs(ddy(uv.y)) * tex_size.y));\n"
    "    df = min(df, 4.0);\n"
    "    float2 pos  = uv * tex_size - 0.5;\n"
    "    float2 base = floor(pos);\n"
    "    float2 fr   = pos - base;\n"
    "\n"
    "    int2 jmin = int2(floor(fr - 2.0 * df)) + 1;\n"
    "    int2 jmax = int2(floor(fr + 2.0 * df));\n"
    "\n"
    "    float2 result = float2(0.0, 0.0);\n"
    "    float wsum   = 0.0;\n"
    "    float2 tap_min = float2(1e9, 1e9);\n"
    "    float2 tap_max = float2(-1e9, -1e9);\n"
    "\n"
    "    [loop] for (int j = jmin.y; j <= jmax.y; j++) {\n"
    "        float wy = catmull_w((float(j) - fr.y) / df.y);\n"
    "        [loop] for (int i = jmin.x; i <= jmax.x; i++) {\n"
    "            float w = catmull_w((float(i) - fr.x) / df.x) * wy;\n"
    "            float2 tc = (base + float2(float(i), float(j)) + 0.5)\n"
    "                        / tex_size;\n"
    "            float2 sm = tex.SampleLevel(samp, tc, 0).rg;\n"
    "            tap_min   = min(tap_min, sm);\n"
    "            tap_max   = max(tap_max, sm);\n"
    "            result   += sm * w;\n"
    "            wsum   += w;\n"
    "        }\n"
    "    }\n"
    "\n"
    "    float2 out_c = (wsum > 0.0) ? result / wsum : float2(0.0, 0.0);\n"
    "    /* Anti-ring, container-keyed — see sample_catmull. (This\n"
    "     * function binds only on the zero-copy P010 route, HDR-class\n"
    "     * by construction; updated for consistency.) */\n"
    "#ifdef DSVP_CHROMA_AR_HDR\n"
    "    if (is_hdr > 0.5)\n"
    "#else\n"
    "    if (is_hdr > 0.5 || out_pq > 0.5)\n"
    "#endif\n"
    "        out_c = lerp(out_c, clamp(out_c, tap_min, tap_max), 0.8);\n"
    "    return out_c;\n"
    "#else\n"
    "    /* Fixed 4x4 — the proven-fast unrolled path (84ffc54). */\n"
    "    float2 pos  = uv * tex_size - 0.5;\n"
    "    float2 base = floor(pos);\n"
    "    float2 f    = pos - base;\n"
    "\n"
    "    float2 result = float2(0.0, 0.0);\n"
    "    float wsum   = 0.0;\n"
    "    float2 tap_min = float2(1e9, 1e9);\n"
    "    float2 tap_max = float2(-1e9, -1e9);\n"
    "\n"
    "    [unroll] for (int j = -1; j <= 2; j++) {\n"
    "        float wy = catmull_w(float(j) - f.y);\n"
    "        [unroll] for (int i = -1; i <= 2; i++) {\n"
    "            float w = catmull_w(float(i) - f.x) * wy;\n"
    "            float2 tc = (base + float2(float(i), float(j)) + 0.5)\n"
    "                        / tex_size;\n"
    "            float2 sm = tex.SampleLevel(samp, tc, 0).rg;\n"
    "            tap_min   = min(tap_min, sm);\n"
    "            tap_max   = max(tap_max, sm);\n"
    "            result   += sm * w;\n"
    "            wsum   += w;\n"
    "        }\n"
    "    }\n"
    "\n"
    "    float2 out_c = (wsum > 0.0) ? result / wsum : float2(0.0, 0.0);\n"
    "    /* Anti-ring, container-keyed — see sample_catmull. (This\n"
    "     * function binds only on the zero-copy P010 route, HDR-class\n"
    "     * by construction; updated for consistency.) */\n"
    "#ifdef DSVP_CHROMA_AR_HDR\n"
    "    if (is_hdr > 0.5)\n"
    "#else\n"
    "    if (is_hdr > 0.5 || out_pq > 0.5)\n"
    "#endif\n"
    "        out_c = lerp(out_c, clamp(out_c, tap_min, tap_max), 0.8);\n"
    "    return out_c;\n"
    "#endif\n"
    "}\n"
    "\n"
    "/* PQ EOTF (SMPTE ST 2084 inverse): PQ code values [0,1] → linear\n"
    " * light [0, 10000] nits. Constants from ITU-R BT.2100. */\n"
    "float3 pq_eotf(float3 pq) {\n"
    "    float m1 = 0.1593017578125;\n"      /* 2610/16384 */
    "    float m2 = 78.84375;\n"              /* 2523/32 * 128 */
    "    float c1 = 0.8359375;\n"             /* 3424/4096 */
    "    float c2 = 18.8515625;\n"            /* 2413/128 */
    "    float c3 = 18.6875;\n"               /* 2392/128 */
    "    float3 Np = pow(max(pq, 0.0), 1.0 / m2);\n"
    "    float3 num = max(Np - c1, 0.0);\n"
    "    float3 den = c2 - c3 * Np;\n"
    "    return 10000.0 * pow(max(num / den, 0.0), 1.0 / m1);\n"
    "}\n"
    "\n"
    "/* PQ OETF (exact inverse of pq_eotf): linear light as a fraction\n"
    " * of 10,000 nits → PQ code values. Used by the passthrough paths\n"
    " * that re-encode display-linear results into the HDR10 container\n"
    " * (DV-after-reshape, HLG-after-OOTF). */\n"
    "float3 pq_oetf(float3 lin) {\n"
    "    float m1 = 0.1593017578125;\n"
    "    float m2 = 78.84375;\n"
    "    float c1 = 0.8359375;\n"
    "    float c2 = 18.8515625;\n"
    "    float c3 = 18.6875;\n"
    "    float3 Np = pow(max(lin, 0.0), m1);\n"
    "    return pow((c1 + c2 * Np) / (1.0 + c3 * Np), m2);\n"
    "}\n"
    "\n"
    "/* BT.2390 EETF: Hermite spline shoulder rolloff for tone mapping.\n"
    " * Maps normalized luminance [0,1] through a soft knee at ks,\n"
    " * compressing highlights to maxLum. Below ks is linear passthrough. */\n"
    "float bt2390_eetf(float e, float ks, float maxLum) {\n"
    "    if (e <= ks) return e;\n"
    /* Clamp to the normalized range before the spline. Pixels brighter
     * than the detected/declared peak gave t > 1, and the cubic then
     * EXTRAPOLATES above maxLum instead of rolling off to it — the
     * final saturate was left to hide it, which clips rather than
     * tone maps. */
    "    e = min(e, 1.0);\n"
    "    float t = (e - ks) / (1.0 - ks);\n"
    "    float t2 = t * t;\n"
    "    float t3 = t2 * t;\n"
    "    return (2.0*t3 - 3.0*t2 + 1.0) * ks\n"
    "         + (t3 - 2.0*t2 + t) * (1.0 - ks)\n"
    "         + (-2.0*t3 + 3.0*t2) * maxLum;\n"
    "}\n"
    "\n"
    "/* Encode tone-mapped display-linear output for the display's EOTF.\n"
    " * out_gamma == 0 selects the sRGB piecewise curve; otherwise a pure\n"
    " * power law (2.2 default, 2.4 = BT.1886 dark-room). The sRGB linear\n"
    " * toe lifts shadows slightly on a display decoding ~2.2, so power is\n"
    " * the reference-faithful default (DSVP_OUTPUT_GAMMA overrides). */\n"
    "float3 encode_output(float3 lin) {\n"
    "    lin = max(lin, 0.0);\n"
    "    /* PQ surface: encode instead of gamma. Only tone-mapped HDR and\n"
    "     * DV reach here, and their reference white is the TONE-MAP\n"
    "     * TARGET (T key) — rgb_tm is normalised so 1.0 means exactly\n"
    "     * that. out_pq is the SDR reference white and must not be used\n"
    "     * here, or lowering it would dim HDR content too. */\n"
    "    if (out_pq > 0.5) return pq_oetf(lin * hdr_target_nits / 10000.0);\n"
    "    if (out_gamma < 0.5) {\n"
    "        return float3(\n"
    "            lin.r <= 0.0031308 ? 12.92*lin.r : 1.055*pow(lin.r, 1.0/2.4) - 0.055,\n"
    "            lin.g <= 0.0031308 ? 12.92*lin.g : 1.055*pow(lin.g, 1.0/2.4) - 0.055,\n"
    "            lin.b <= 0.0031308 ? 12.92*lin.b : 1.055*pow(lin.b, 1.0/2.4) - 0.055);\n"
    "    }\n"
    "    return pow(lin, 1.0 / out_gamma);\n"
    "}\n"
    "\n"
    "/* HLG (ARIB STD-B67): inverse OETF to scene-linear, then the\n"
    " * BT.2100 OOTF at nominal Lw=1000 (system gamma 1.2) to display\n"
    " * light in nits — from there the shared BT.2390 path takes over.\n"
    " * (DSVP main fdbb489.) */\n"
    "float3 hlg_oetf_inv(float3 e) {\n"
    "    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;\n"
    "    float3 lo = (e * e) / 3.0;\n"
    "    float3 hi = (exp((e - c) / a) + b) / 12.0;\n"
    "    return float3(e.r <= 0.5 ? lo.r : hi.r,\n"
    "                  e.g <= 0.5 ? lo.g : hi.g,\n"
    "                  e.b <= 0.5 ? lo.b : hi.b);\n"
    "}\n"
    "\n"
    "float3 hlg_to_nits(float3 sig) {\n"
    "    float3 s = hlg_oetf_inv(saturate(sig));\n"
    "    float ys = dot(s, float3(0.2627, 0.6780, 0.0593));\n"
    "    return 1000.0 * pow(max(ys, 1e-6), 0.2) * s;\n"
    "}\n"
    "\n"
    "/* Select component from float4: 0=x(I), 1=y(Ct), 2=z(Cp) */\n"
    "float sel3(float4 v, int c) {\n"
    "    if (c == 0) return v.x;\n"
    "    if (c == 1) return v.y;\n"
    "    return v.z;\n"
    "}\n"
    "\n"
    "/* DV MMR (Multivariate Multiple Regression) reshape for one chroma\n"
    " * component: a cross-channel polynomial over the coded (I,Ct,Cp)\n"
    " * triple. Terms per order o (1..3): I, Ct, Cp, I*Ct, I*Cp, Ct*Cp,\n"
    " * I*Ct*Cp — each raised to the o-th power, 7 coefficients per\n"
    " * order plus one constant. comp: 1 = Ct, 2 = Cp. */\n"
    "float dovi_mmr_eval(float3 sig, int comp) {\n"
    "    int   order = (int)((comp == 1) ? dovi_mmr_meta.x : dovi_mmr_meta.y);\n"
    "    float s     =        (comp == 1) ? dovi_mmr_meta.z : dovi_mmr_meta.w;\n"
    "    float t[7];\n"
    "    t[0] = sig.x; t[1] = sig.y; t[2] = sig.z;\n"
    "    t[3] = sig.x * sig.y;\n"
    "    t[4] = sig.x * sig.z;\n"
    "    t[5] = sig.y * sig.z;\n"
    "    t[6] = t[3] * sig.z;\n"
    "    float p[7];\n"
    "    [unroll] for (int k = 0; k < 7; k++) p[k] = t[k];\n"
    "    [loop] for (int o = 0; o < 3; o++) {\n"
    "        if (o >= order) break;\n"
    "        [unroll] for (int i = 0; i < 7; i++) {\n"
    "            int idx = o * 7 + i;\n"
    "            float4 bank = (comp == 1) ? dovi_mmr_ct[idx >> 2]\n"
    "                                      : dovi_mmr_cp[idx >> 2];\n"
    "            s += bank[idx & 3] * p[i];\n"
    "        }\n"
    "        [unroll] for (int k2 = 0; k2 < 7; k2++) p[k2] *= t[k2];\n"
    "    }\n"
    "    return s;\n"
    "}\n"
    "\n"
    "/* Reshape one DV component: pivot search selects the active piece,\n"
    " * THEN the piece's method dispatches — polynomial c0 + c1*x + c2*x*x,\n"
    " * or MMR for a chroma piece 0 flagged in dovi_mmr_meta (the CPU side\n"
    " * only ever packs MMR for piece 0). Dispatching the method before\n"
    " * the pivot search routed EVERY pixel of a multi-piece curve through\n"
    " * piece 0's MMR coefficients; for the universal single-piece case\n"
    " * the two orders are identical. Falls back to identity if no\n"
    " * pieces. */\n"
    "float dovi_reshape(float3 sig, int comp) {\n"
    "    float x = sel3(float4(sig, 0.0), comp);\n"
    "    int n = (int)sel3(dovi_num_pieces, comp);\n"
    "    if (n <= 0) return x;\n"
    "    for (int p = 0; p < 8; p++) {\n"
    "        if (p >= n) break;\n"
    "        float hi = sel3(dovi_pivots[p + 1], comp);\n"
    "        if (x < hi || p == n - 1) {\n"
    "            if (p == 0 && comp != 0 &&\n"
    "                ((comp == 1) ? dovi_mmr_meta.x : dovi_mmr_meta.y) > 0.5)\n"
    "                return dovi_mmr_eval(sig, comp);\n"
    "            float c0v = sel3(dovi_c0[p], comp);\n"
    "            float c1v = sel3(dovi_c1[p], comp);\n"
    "            float c2v = sel3(dovi_c2[p], comp);\n"
    "            return c0v + c1v * x + c2v * x * x;\n"
    "        }\n"
    "    }\n"
    "    return x;\n"
    "}\n"
    "\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {\n"
    "    /* Chroma siting: shift UV to actual sample position */\n"
    "    float2 uv_chroma = uv + chromaOffset / texSizeUV;\n"
    "\n"
    "    /* Lanczos-2 for luma, Catmull-Rom for chroma */\n"
    "    float y  = sample_lanczos(texY, sampY, uv, texSizeY);\n"
    "    float cb, cr;\n"
    "    if (is_semiplanar > 0.5) {\n"
    "        /* Semi-planar: texU is R16G16_UNORM with interleaved UV.\n"
    "         * .r = Cb (U), .g = Cr (V). Single texture, one sample. */\n"
    "        float2 uv_val = sample_catmull_rg(texU, sampU, uv_chroma, texSizeUV);\n"
    "        cb = uv_val.r;\n"
    "        cr = uv_val.g;\n"
    "    } else {\n"
    "        /* Planar: separate R16/R8 textures for U and V */\n"
    "        cb = sample_catmull(texU, sampU, uv_chroma, texSizeUV);\n"
    "        cr = sample_catmull(texV, sampV, uv_chroma, texSizeUV);\n"
    "    }\n"
    "\n"
    "    y  = (y  - rangeY.x)  * rangeY.y;\n"
    "    cb = (cb - rangeUV.x) * rangeUV.y;\n"
    "    cr = (cr - rangeUV.x) * rangeUV.y;\n"
    "\n"
    "    float3 rgb;\n"
    "\n"
    "    if (is_dovi > 0.5) {\n"
    "        /* ── Dolby Vision decode chain ──\n"
    "         * Planes contain I/Ct/Cp (IPTPQc2), not standard YCbCr.\n"
    "         * 1. Reshape: piecewise polynomial (from RPU pivot/coef arrays)\n"
    "         * 2. ycc_to_rgb matrix: IPT → PQ-encoded signal\n"
    "         * 3. PQ EOTF → linear light (nits)\n"
    "         * 4. Output matrix → BT.2020 linear RGB\n"
    "         * 5. BT.2390 tone mapping (shared with HDR10 path) */\n"
    "        float3 sig = float3(y, cb, cr);\n"
    "        float3 ipt;\n"
    "        ipt.x = dovi_reshape(sig, 0);\n"
    "        ipt.y = dovi_reshape(sig, 1);\n"
    "        ipt.z = dovi_reshape(sig, 2);\n"
    "        ipt = saturate(ipt);\n"
    "\n"
    "        float3 centered = ipt - float3(dovi_ycc_r0.w, dovi_ycc_r1.w, dovi_ycc_r2.w);\n"
    "        float3 pq_sig;\n"
    "        pq_sig.r = dot(dovi_ycc_r0.xyz, centered);\n"
    "        pq_sig.g = dot(dovi_ycc_r1.xyz, centered);\n"
    "        pq_sig.b = dot(dovi_ycc_r2.xyz, centered);\n"
    "        pq_sig = saturate(pq_sig);\n"
    "\n"
    "        float3 lin = pq_eotf(pq_sig);\n"
    "        float3 bt2020;\n"
    "        bt2020.r = dot(dovi_out_r0.xyz, lin);\n"
    "        bt2020.g = dot(dovi_out_r1.xyz, lin);\n"
    "        bt2020.b = dot(dovi_out_r2.xyz, lin);\n"
    "        bt2020 = max(bt2020, 0.0);\n"
    "\n"
    "        if (hdr_pass > 0.5) {\n"
    "            /* DV-as-HDR10 (docs/TODO-HDR.md item 5): the per-frame\n"
    "             * RPU reshape above IS Dolby Vision's dynamic metadata,\n"
    "             * applied by us — the same processing an LLDV player\n"
    "             * does internally. Re-encode the display-linear BT.2020\n"
    "             * result (nits, from pq_eotf) into the PQ container and\n"
    "             * let the display tone-map. */\n"
    "            rgb = pq_oetf(bt2020 / 10000.0);\n"
    "        } else {\n"
    "        /* BT.2390 tone mapping — always BT.2020 gamut for DV */\n"
    "        float3 E = bt2020 / hdr_peak_nits;\n"
    "        float target = (hdr_debug > 0.5 && hdr_debug < 1.5)\n"
    "            ? hdr_target_nits + 100.0 : hdr_target_nits;\n"
    "        float maxLum = target / hdr_peak_nits;\n"
    "        float ks = max(1.5 * maxLum - 0.5, 0.0);\n"
    "        float3 lc = float3(0.2627, 0.6780, 0.0593);\n"
    "        float Y_l = dot(E, lc);\n"
    "        float Yt = bt2390_eetf(Y_l, ks, maxLum);\n"
    "        float3 rgb_tm = (Y_l > 0.0) ? E * (Yt / Y_l) : float3(0,0,0);\n"
    "        rgb_tm = rgb_tm / max(maxLum, 0.001);\n"
    "\n"
    "        /* BT.2020→BT.709 gamut matrix. Skipped when the display\n"
    "         * itself is BT.2020 (out_gamut): DV decode already produced\n"
    "         * BT.2020, so squeezing to 709 only to have the display\n"
    "         * stretch it back is two lossy steps for nothing. */\n"
    "        if (out_gamut < 0.5) {\n"
    "            float3 r2 = rgb_tm;\n"
    "            rgb_tm = float3(\n"
    "                 1.6605*r2.r - 0.5877*r2.g - 0.0728*r2.b,\n"
    "                -0.1246*r2.r + 1.1330*r2.g - 0.0084*r2.b,\n"
    "                -0.0182*r2.r - 0.1006*r2.g + 1.1187*r2.b);\n"
    "            rgb_tm = max(rgb_tm, 0.0);\n"
    "        }\n"
    "\n"
    "        if (hdr_midtone_gain > 1.001) {\n"
    "            float inv = 1.0 / hdr_midtone_gain;\n"
    "            rgb_tm = float3(pow(rgb_tm.r, inv), pow(rgb_tm.g, inv), pow(rgb_tm.b, inv));\n"
    "        }\n"
    "        rgb = encode_output(rgb_tm);\n"
    "        } /* end DV SDR tone map */\n"
    "\n"
    "    } else {\n"
    "        /* Standard path (SDR + HDR10) */\n"
    "        float4 yuv = float4(y, cb - 0.5, cr - 0.5, 1.0);\n"
    "        rgb = mul(colorMatrix, yuv).rgb;\n"
    "\n"
    "    /* ── HDR→SDR Tone Mapping (BT.2390 EETF) ──\n"
    "     * Debug modes (H key): 0=normal, 1=target 300, 2=PQ bypass, 3=luma viz */\n"
    "    if (is_hdr > 0.5) {\n"
    "\n"
    "        /* HDR passthrough (docs/TODO-HDR.md): the swapchain is\n"
    "         * HDR10/ST2084 — rgb already holds range-expanded PQ code\n"
    "         * values in BT.2020, which is exactly the payload the\n"
    "         * display wants. No tone map, no gamut squeeze, no output\n"
    "         * encode: the most correct path is the code we skip. The\n"
    "         * debug modes and T/G/E controls are SDR-render tools. */\n"
    "        if (hdr_pass > 0.5) {\n"
    "            /* PQ content ships as-is. HLG rides the same HDR10\n"
    "             * container (item 4): inverse OETF + BT.2100 OOTF to\n"
    "             * display light at the 1000-nit nominal, then\n"
    "             * PQ-encode — the display tone-maps from there. */\n"
    "            if (is_hlg > 0.5)\n"
    "                rgb = pq_oetf(hlg_to_nits(rgb) / 10000.0);\n"
    "        }\n"
    "        /* Mode 2: PQ bypass — raw PQ code values straight to display.\n"
    "         * Shows what the stream actually contains. If this looks\n"
    "         * reasonably bright, PQ values are valid and the issue\n"
    "         * is in tone mapping. If dark, values themselves are wrong. */\n"
    "        else if (hdr_debug > 1.5 && hdr_debug < 2.5) {\n"
    "            /* rgb already holds PQ code values [0,1] — skip everything */\n"
    "        }\n"
    "        /* Mode 3: luminance visualization — EOTF output with sRGB gamma.\n"
    "         * Grayscale showing actual nit distribution in the frame. */\n"
    "        else if (hdr_debug > 2.5) {\n"
    "            float3 lin = (is_hlg > 0.5) ? hlg_to_nits(rgb) : pq_eotf(rgb);\n"
    "            float lum = lin.r * 0.2627 + lin.g * 0.6780 + lin.b * 0.0593;\n"
    "            float v = lum / hdr_peak_nits;\n"
    "            v = (v <= 0.0031308) ? 12.92*v : 1.055*pow(v, 1.0/2.4) - 0.055;\n"
    "            rgb = float3(v, v, v);\n"
    "        }\n"
    "        else {\n"
    "            float3 lin = (is_hlg > 0.5) ? hlg_to_nits(rgb) : pq_eotf(rgb);\n"
    "            float3 E = lin / hdr_peak_nits;\n"
    "\n"
    "            /* Target comes from T-key toggle (203/300/400 nits).\n"
    "             * Debug mode 1: override to target+100 for comparison. */\n"
    "            float target = (hdr_debug > 0.5 && hdr_debug < 1.5)\n"
    "                ? hdr_target_nits + 100.0 : hdr_target_nits;\n"
    "            float maxLum = target / hdr_peak_nits;\n"
    "            float ks = max(1.5 * maxLum - 0.5, 0.0);\n"
    "\n"
    "            float3 lc = (hdr_gamut > 0.5)\n"
    "                ? float3(0.2627, 0.6780, 0.0593)\n"
    "                : float3(0.2126, 0.7152, 0.0722);\n"
    "            float Y = dot(E, lc);\n"
    "            float Yt = bt2390_eetf(Y, ks, maxLum);\n"
    "            float3 rgb_tm = (Y > 0.0) ? E * (Yt / Y) : float3(0,0,0);\n"
    "\n"
    "            rgb_tm = rgb_tm / max(maxLum, 0.001);\n"
    "\n"
    "            /* Gamut reconcile: source primaries (hdr_gamut) vs the\n"
    "             * display's (out_gamut). Matching pairs convert nothing.\n"
    "             * Both directions run here in LINEAR light — rgb_tm is\n"
    "             * pre-encode_output — which is where a primaries change\n"
    "             * belongs. */\n"
    "            if (hdr_gamut > 0.5 && out_gamut < 0.5) {\n"
    "                float3 r2 = rgb_tm;\n"
    "                rgb_tm = float3(\n"
    "                     1.6605*r2.r - 0.5877*r2.g - 0.0728*r2.b,\n"
    "                    -0.1246*r2.r + 1.1330*r2.g - 0.0084*r2.b,\n"
    "                    -0.0182*r2.r - 0.1006*r2.g + 1.1187*r2.b);\n"
    "                rgb_tm = max(rgb_tm, 0.0);\n"
    "            }\n"
    "            else if (hdr_gamut < 0.5 && out_gamut > 0.5) {\n"
    "                float3 r7 = rgb_tm;\n"
    "                rgb_tm = float3(\n"
    "                     0.6274*r7.r + 0.3293*r7.g + 0.0433*r7.b,\n"
    "                     0.0691*r7.r + 0.9195*r7.g + 0.0114*r7.b,\n"
    "                     0.0164*r7.r + 0.0880*r7.g + 0.8956*r7.b);\n"
    "                rgb_tm = max(rgb_tm, 0.0);\n"
    "            }\n"
    "\n"
    "\n"
    "            rgb_tm = max(rgb_tm, 0.0);\n"
    "            if (hdr_midtone_gain > 1.001) {\n"
    "                float inv = 1.0 / hdr_midtone_gain;\n"
    "                rgb_tm = float3(pow(rgb_tm.r, inv), pow(rgb_tm.g, inv), pow(rgb_tm.b, inv));\n"
    "            }\n"
    "            rgb = encode_output(rgb_tm);\n"
    "        }\n"
    "    }\n"
    "    else if (hdr_gamut > 0.5 && out_gamut < 0.5 && out_pq < 0.5) {\n"
    "        /* SDR tagged BT.2020 (rare but legal) on a 709 display: the\n"
    "         * 2020 YCbCr matrix above produced BT.2020 RGB, and showing\n"
    "         * it unconverted is visibly desaturated. Convert primaries in\n"
    "         * LINEAR light (BT.1886-ish 2.4 for SDR video), then re-encode\n"
    "         * with the same curve (transfer unchanged — this is a gamut\n"
    "         * conversion, not a tone map). */\n"
    "        float3 lin = pow(max(rgb, 0.0), 2.4);\n"
    "        float3 l7 = float3(\n"
    "             1.6605*lin.r - 0.5877*lin.g - 0.0728*lin.b,\n"
    "            -0.1246*lin.r + 1.1330*lin.g - 0.0084*lin.b,\n"
    "            -0.0182*lin.r - 0.1006*lin.g + 1.1187*lin.b);\n"
    "        rgb = pow(max(l7, 0.0), 1.0/2.4);\n"
    "    }\n"
    "    else if (hdr_gamut < 0.5 && out_gamut > 0.5 && out_pq < 0.5) {\n"
    "        /* The common case this whole uniform exists for: ordinary\n"
    "         * BT.709 SDR on a display running BT.2020 primaries. Sending\n"
    "         * 709 code values to a 2020 display means every colour is\n"
    "         * read against wider primaries and stretched outward —\n"
    "         * oversaturated, and worst on skin tones. Same linear-light\n"
    "         * treatment as the branch above, opposite direction (the\n"
    "         * matrices are numerically verified inverses). */\n"
    "        float3 lin = pow(max(rgb, 0.0), 2.4);\n"
    "        float3 l20 = float3(\n"
    "             0.6274*lin.r + 0.3293*lin.g + 0.0433*lin.b,\n"
    "             0.0691*lin.r + 0.9195*lin.g + 0.0114*lin.b,\n"
    "             0.0164*lin.r + 0.0880*lin.g + 0.8956*lin.b);\n"
    "        rgb = pow(max(l20, 0.0), 1.0/2.4);\n"
    "    }\n"
    "    } /* end else (standard path) */\n"
    "\n"
    "    /* SDR content keeps its own transfer through the paths above\n"
    "     * (they change primaries, never the curve), so it is the one\n"
    "     * case still holding gamma-encoded values by here. Take it to\n"
    "     * display-linear with the BT.1886 2.4 the gamut branches also\n"
    "     * assume, convert primaries into the BT.2020 the HDR10\n"
    "     * container is defined with, scale so 1.0 lands on the SDR\n"
    "     * reference white, and PQ-encode. Tone-mapped and DV output\n"
    "     * left through encode_output, which PQ-encodes on its own and\n"
    "     * against its own reference white. */\n"
    "    if (out_pq > 0.5 && is_hdr < 0.5 && is_dovi < 0.5) {\n"
    "        /* Primaries AND encode in ONE trip through linear light.\n"
    "         * The gamut branches above are skipped in this mode: doing\n"
    "         * it there meant pow(2.4) -> matrix -> pow(1/2.4) and then\n"
    "         * pow(2.4) again here, six wasted pow() per pixel — 8.3M\n"
    "         * pixels at 60fps makes that a measurable frame cost on the\n"
    "         * Deck iGPU, and it showed up as drops on 4K59.94. */\n"
    "#if DSVP_PQ_LUT\n"
    "        float3 lin = float3(lut_lin(rgb.r), lut_lin(rgb.g),\n"
    "                            lut_lin(rgb.b));\n"
    "#else\n"
    "        float3 lin = pow(max(rgb, 0.0), 2.4);\n"
    "#endif\n"
    "        if (hdr_gamut < 0.5) {\n"
    "            float3 l7 = lin;\n"
    "            lin = float3(\n"
    "                 0.6274*l7.r + 0.3293*l7.g + 0.0433*l7.b,\n"
    "                 0.0691*l7.r + 0.9195*l7.g + 0.0114*l7.b,\n"
    "                 0.0164*l7.r + 0.0880*l7.g + 0.8956*l7.b);\n"
    "        }\n"
    "#if DSVP_PQ_LUT\n"
    "        /* nits scale baked into the LUT — see the decl comment. */\n"
    "        rgb = float3(lut_pq(lin.r), lut_pq(lin.g), lut_pq(lin.b));\n"
    "#else\n"
    "        rgb = pq_oetf(max(lin, 0.0) * out_pq / 10000.0);\n"
    "#endif\n"
    "    }\n"
    "\n"
    "    /* Blue noise dither: ±0.5 LSB in 8-bit (±1/510 in [0,1]).\n"
    "     * 64x64 void-and-cluster texture, tiled via frac(). Temporal\n"
    "     * offset shifts the pattern each frame so quantization error\n"
    "     * averages out over ~4 frames — perceived bit depth increases. */\n"
    "    uint fc = (uint)frameCount;\n"
    "    float2 ditherCoord = pos.xy + float2(fc % 4u, (fc / 4u) % 4u);\n"
    "    /* LSB matches the output surface: 10-bit in HDR passthrough. */\n"
    "    float dith_lsb = (hdr_pass > 0.5 || out_pq > 0.5) ? 1023.0 : 255.0;\n"
    "    float d = (texNoise.SampleLevel(sampNoise, frac(ditherCoord / 64.0), 0).r - 0.5) / dith_lsb;\n"
    "    rgb += float3(d, d, d);\n"
    "\n"
    "    return float4(saturate(rgb), 1.0);\n"
    "}\n";

/* Frame-blit fragment shader: copies the intermediate frame texture
 * to the swapchain 1:1. Pure fetch — the video shader already did
 * all colour math and dither into the UNORM16 intermediate; the
 * store into the swapchain quantises by rounding exactly as the
 * direct path's float→UNORM store did. */
static const char hlsl_blit_frag[] =
    "Texture2D<float4> texFrame : register(t0, space2);\n"
    "SamplerState sampFrame : register(s0, space2);\n"
    "\n"
    "float4 main(float2 uv : TEXCOORD0) : SV_Target0 {\n"
    "    return texFrame.Sample(sampFrame, uv);\n"
    "}\n";

/* RGBA overlay fragment shader — compositing debug overlays, seek
 * bar, subtitles, etc. over the video frame. One texture, one
 * sampler, one small uniform: when the swapchain is HDR10/PQ, the
 * overlay's SDR-authored pixels must be re-encoded to PQ at
 * reference white (203 nits, BT.2408) — drawn raw, "white" would
 * mean 10,000 nits and every subtitle would be a flashbang. */
static const char hlsl_overlay_frag[] =
    "Texture2D<float4> texOverlay : register(t0, space2);\n"
    "SamplerState sampOverlay : register(s0, space2);\n"
    "\n"
    "cbuffer OvParams : register(b0, space3) {\n"
    "    float ov_pq;\n"
    "    float ov_nits;\n"
    "    float2 _pad;\n"
    "};\n"
    "\n"
    "float4 main(float2 uv : TEXCOORD0) : SV_Target0 {\n"
    "    float4 c = texOverlay.Sample(sampOverlay, uv);\n"
    "    if (ov_pq > 0.5) {\n"
    "        /* SDR (2.2) → linear → reference-white nits → PQ OETF.\n"
    "         * ov_nits: 203 (BT.2408 graphics white) over passthrough\n"
    "         * HDR; the SDR reference white in SDR-in-PQ mode, so\n"
    "         * overlay white matches video white. */\n"
    "        float3 lin = pow(max(c.rgb, 0.0), 2.2) * (ov_nits / 10000.0);\n"
    "        float m1 = 0.1593017578125, m2 = 78.84375;\n"
    "        float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;\n"
    "        float3 Np = pow(lin, m1);\n"
    "        c.rgb = pow((c1 + c2 * Np) / (1.0 + c3 * Np), m2);\n"
    "    }\n"
    "    return c;\n"
    "}\n";


/* ═══════════════════════════════════════════════════════════════════
 * Shader Compilation Helper
 * ═══════════════════════════════════════════════════════════════════
 *
 * Three-step pipeline for shadercross 3.0.0:
 *   1. HLSL → SPIRV  (CompileSPIRVFromHLSL)
 *   2. SPIRV → metadata (ReflectGraphicsSPIRV) — resource counts
 *   3. SPIRV → native  (CompileGraphicsShaderFromSPIRV) — D3D12/Vulkan/Metal
 *
 * Note: CompileGraphicsShaderFromHLSL does NOT exist in 3.0.0.
 */

static SDL_GPUShader *compile_shader_pfx(
    SDL_GPUDevice *device,
    const char *source,
    const char *entrypoint,
    SDL_ShaderCross_ShaderStage stage,
    const char *prefix);

static SDL_GPUShader *compile_shader(
    SDL_GPUDevice *device,
    const char *source,
    const char *entrypoint,
    SDL_ShaderCross_ShaderStage stage)
{
    return compile_shader_pfx(device, source, entrypoint, stage, NULL);
}

/* Chroma anti-ring predicate switch (Knot gains #1, 2026-08-20): the
 * clamp follows the CONTAINER (is_hdr || out_pq) by default;
 * DSVP_CHROMA_AR=hdr restores the pre-change HDR-content-only gate
 * for a one-binary A/B. Applied to every variant prefix so the
 * shared-source sites stay in lockstep. */
static const char *chroma_ar_defs(void) {
    const char *v = SDL_getenv("DSVP_CHROMA_AR");
    return (v && strcmp(v, "hdr") == 0)
        ? "#define DSVP_CHROMA_AR_HDR 1\n" : "";
}

/* Compile with an optional #define prefix prepended to the source —
 * the shader-variant mechanism (e.g. "#define DSVP_DILATE 1\n"). */
static SDL_GPUShader *compile_shader_pfx(
    SDL_GPUDevice *device,
    const char *source,
    const char *entrypoint,
    SDL_ShaderCross_ShaderStage stage,
    const char *prefix)
{
    char *combined = NULL;
    if (prefix) {
        size_t pl = strlen(prefix), sl = strlen(source);
        combined = malloc(pl + sl + 1);
        if (!combined) return NULL;
        memcpy(combined, prefix, pl);
        memcpy(combined + pl, source, sl + 1);
        source = combined;
    }

    const char *stage_name =
        (stage == SDL_SHADERCROSS_SHADERSTAGE_VERTEX) ? "vert" : "frag";

    /* Step 1: HLSL → SPIRV */
    SDL_ShaderCross_HLSL_Info hlsl_info;
    SDL_zero(hlsl_info);
    hlsl_info.source       = source;
    hlsl_info.entrypoint   = entrypoint;
    hlsl_info.include_dir  = NULL;
    hlsl_info.defines      = NULL;
    hlsl_info.shader_stage = stage;
    hlsl_info.props        = 0;

    size_t spirv_size = 0;
    void *spirv = SDL_ShaderCross_CompileSPIRVFromHLSL(&hlsl_info, &spirv_size);
    free(combined);   /* HLSL consumed — prefix copy no longer needed */
    if (!spirv) {
        log_msg("ERROR: HLSL->SPIRV failed (%s): %s", stage_name, SDL_GetError());
        return NULL;
    }
    log_msg("Shader: HLSL->SPIRV OK (%s, %zu bytes)", stage_name, spirv_size);

    /* Step 2: Reflect SPIRV for resource counts.
     * Returns a malloc'd struct — must SDL_free when done. */
    SDL_ShaderCross_GraphicsShaderMetadata *metadata =
        SDL_ShaderCross_ReflectGraphicsSPIRV(spirv, spirv_size, 0);
    if (!metadata) {
        log_msg("ERROR: SPIRV reflection failed: %s", SDL_GetError());
        SDL_free(spirv);
        return NULL;
    }
    log_msg("Shader: reflect OK (samplers=%u storage_tex=%u uniforms=%u)",
            metadata->resource_info.num_samplers,
            metadata->resource_info.num_storage_textures,
            metadata->resource_info.num_uniform_buffers);

    /* Step 3: SPIRV → native GPU shader.
     * CompileGraphicsShaderFromSPIRV takes:
     *   (device, SPIRV_Info*, GraphicsShaderResourceInfo*, props) */
    SDL_ShaderCross_SPIRV_Info spirv_info;
    SDL_zero(spirv_info);
    spirv_info.bytecode     = spirv;
    spirv_info.bytecode_size = spirv_size;
    spirv_info.entrypoint   = entrypoint;
    spirv_info.shader_stage = stage;
    spirv_info.props        = 0;

    SDL_GPUShader *shader = SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(
        device, &spirv_info, &metadata->resource_info, 0);

    SDL_free(metadata);
    SDL_free(spirv);

    if (!shader) {
        log_msg("ERROR: SPIRV->native failed: %s", SDL_GetError());
        return NULL;
    }
    log_msg("Shader: native compile OK (%s)", stage_name);
    return shader;
}


/* ═══════════════════════════════════════════════════════════════════
 * GPU Pipeline Setup / Teardown
 * ═══════════════════════════════════════════════════════════════════
 *
 * Called once at startup (from main.c after creating the GPU device
 * and claiming the window). Compiles shaders and creates the
 * graphics pipelines and sampler.
 *
 * Two pipelines:
 *   - gpu_pipeline_yuv:     planar YUV420P (3 textures, 3 samplers)
 *   - gpu_pipeline_overlay: RGBA + alpha blend (1 texture, 1 sampler)
 */

/* Build + upload the two SDR-in-PQ encode LUTs (1024×1 R16_UNORM,
 * sampled with the linear/clamp video sampler). lut_lin stores x^1.2
 * — the shader squares it to x^2.4, because a straight x^2.4 table in
 * R16 quantises shadows into steps PQ amplifies to 5-17 ten-bit codes
 * (found by simulating the full chain; §3's R16 note missed the
 * output-quantisation cascade). lut_pq = PQ OETF of (u² · nits/10000),
 * sqrt-domain index, nits baked in (out_pq_nits is fixed for the
 * session). Whole-chain simulation incl. R16 rounding + bilinear PWL:
 * worst error 0.11 (100 nits) / 0.37 (1000 nits) ten-bit codes —
 * under the ±0.5-LSB dither floor. Returns 0 on success; any failure
 * releases both and the caller compiles the pow-path shader instead. */
#define PQ_LUT_N 1024
static int gpu_pq_luts_create(PlayerState *ps) {
    Uint16 lin_tab[PQ_LUT_N], pq_tab[PQ_LUT_N];
    const double m1 = 0.1593017578125, m2 = 78.84375;
    const double c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    for (int i = 0; i < PQ_LUT_N; i++) {
        double u = (double)i / (PQ_LUT_N - 1);
        lin_tab[i] = (Uint16)lround(pow(u, 1.2) * 65535.0);
        double Y = u * u * (double)ps->out_pq_nits / 10000.0;
        double p = pow(Y, m1);
        double v = pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        pq_tab[i] = (Uint16)lround(v * 65535.0);
    }

    SDL_GPUTextureCreateInfo ti;
    SDL_zero(ti);
    ti.type                 = SDL_GPU_TEXTURETYPE_2D;
    ti.format               = SDL_GPU_TEXTUREFORMAT_R16_UNORM;
    ti.width                = PQ_LUT_N;
    ti.height               = 1;
    ti.layer_count_or_depth = 1;
    ti.num_levels           = 1;
    ti.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ps->gpu_tex_lut_lin = SDL_CreateGPUTexture(ps->gpu_device, &ti);
    ps->gpu_tex_lut_pq  = SDL_CreateGPUTexture(ps->gpu_device, &ti);

    SDL_GPUTransferBufferCreateInfo xi;
    SDL_zero(xi);
    xi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    xi.size  = 2 * PQ_LUT_N * sizeof(Uint16);
    SDL_GPUTransferBuffer *xfer =
        SDL_CreateGPUTransferBuffer(ps->gpu_device, &xi);

    int ok = 0;
    if (ps->gpu_tex_lut_lin && ps->gpu_tex_lut_pq && xfer) {
        Uint16 *dst = SDL_MapGPUTransferBuffer(ps->gpu_device, xfer, false);
        if (dst) {
            memcpy(dst, lin_tab, sizeof(lin_tab));
            memcpy(dst + PQ_LUT_N, pq_tab, sizeof(pq_tab));
            SDL_UnmapGPUTransferBuffer(ps->gpu_device, xfer);
            SDL_GPUCommandBuffer *cmd =
                SDL_AcquireGPUCommandBuffer(ps->gpu_device);
            if (cmd) {
                SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
                SDL_GPUTextureTransferInfo si;
                SDL_GPUTextureRegion dr;
                SDL_zero(si);
                SDL_zero(dr);
                si.transfer_buffer = xfer;
                si.pixels_per_row  = PQ_LUT_N;
                si.rows_per_layer  = 1;
                dr.w = PQ_LUT_N;
                dr.h = 1;
                dr.d = 1;
                dr.texture = ps->gpu_tex_lut_lin;
                SDL_UploadToGPUTexture(copy, &si, &dr, false);
                si.offset  = PQ_LUT_N * sizeof(Uint16);
                dr.texture = ps->gpu_tex_lut_pq;
                SDL_UploadToGPUTexture(copy, &si, &dr, false);
                SDL_EndGPUCopyPass(copy);
                SDL_SubmitGPUCommandBuffer(cmd);
                ok = 1;
            }
        }
    }
    if (xfer) SDL_ReleaseGPUTransferBuffer(ps->gpu_device, xfer);
    if (!ok) {
        log_msg("WARN: PQ LUT setup failed (%s) — pow-path shader",
                SDL_GetError());
        if (ps->gpu_tex_lut_lin) {
            SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_tex_lut_lin);
            ps->gpu_tex_lut_lin = NULL;
        }
        if (ps->gpu_tex_lut_pq) {
            SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_tex_lut_pq);
            ps->gpu_tex_lut_pq = NULL;
        }
        return -1;
    }
    log_msg("GPU: PQ encode LUTs created (2x %dx1 R16, %.0f nits baked, "
            "sqrt-domain; DSVP_PQ_LUT=0 opts out)",
            PQ_LUT_N, (double)ps->out_pq_nits);
    return 0;
}

int gpu_create_pipelines(PlayerState *ps) {
    if (!ps->gpu_device || !ps->window) return -1;

    log_msg("GPU: compiling shaders...");

    /* ── SDR-in-PQ encode LUTs (decide BEFORE the frag compiles: the
     * DSVP_PQ_LUT define must match whether the textures exist) ──
     * Only for PQ-container sessions; DSVP_PQ_LUT=0 is the in-app A/B
     * back to the pow path. Textures are (re)created here because
     * gpu_destroy_pipelines releases them alongside the noise texture
     * on every HDR-flip rebuild. */
    ps->pq_lut_active = 0;
    {
        const char *le = SDL_getenv("DSVP_PQ_LUT");
        if (ps->out_pq_nits > 0.0f && !(le && strcmp(le, "0") == 0)
                && gpu_pq_luts_create(ps) == 0)
            ps->pq_lut_active = 1;
    }
    char frag_defs[192];
    snprintf(frag_defs, sizeof(frag_defs),
             "#define DSVP_DILATE 0\n#define DSVP_PQ_LUT %d\n"
             "#define DSVP_DIRECT 0\n#define DSVP_SCALE2X 0\n%s",
             ps->pq_lut_active, chroma_ar_defs());
    log_msg("GPU: chroma anti-ring predicate: %s",
            chroma_ar_defs()[0] ? "hdr-content only (DSVP_CHROMA_AR=hdr)"
                                : "hdr-or-pq-container");

    /* ── Compile vertex shader (shared by both pipelines) ── */
    SDL_GPUShader *vert = compile_shader(
        ps->gpu_device, hlsl_fullscreen_vert, "main",
        SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
    if (!vert) return -1;

    /* ── Compile planar YUV fragment shader, BOTH sampler variants ──
     * Fixed (unrolled 4x4, DSVP_DILATE=0): bound at 1:1/upscale — the
     * proven-fast path; the dilated kernels' dynamic [loop] bounds
     * cost real GPU time even at df == 1 (field-measured: broke 4K60
     * under software-decode UMA contention). Dilated (DSVP_DILATE=1):
     * bound only when actually downscaling, where it is the correct
     * filter (anti-moire, Tier 3b item 8). At 1:1 both produce
     * identical pixels, so selection never changes the picture. */
    SDL_GPUShader *frag_yuv = compile_shader_pfx(
        ps->gpu_device, hlsl_yuv_planar_frag, "main",
        SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT, frag_defs);
    if (!frag_yuv) {
        SDL_ReleaseGPUShader(ps->gpu_device, vert);
        return -1;
    }

    /* ── Create YUV planar pipelines ── */
    SDL_GPUColorTargetDescription color_desc;
    SDL_zero(color_desc);
    color_desc.format = SDL_GetGPUSwapchainTextureFormat(
        ps->gpu_device, ps->window);

    SDL_GPUGraphicsPipelineCreateInfo pipe_info;
    SDL_zero(pipe_info);
    pipe_info.vertex_shader   = vert;
    pipe_info.fragment_shader = frag_yuv;
    pipe_info.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    pipe_info.target_info.num_color_targets        = 1;
    pipe_info.target_info.color_target_descriptions = &color_desc;

    ps->gpu_pipeline_yuv = SDL_CreateGPUGraphicsPipeline(
        ps->gpu_device, &pipe_info);

    /* Second variant targeting the RGBA16 intermediate: a pipeline's
     * color-target format must match the attachment it renders into,
     * and the default path draws into gpu_tex_frame
     * (R16G16B16A16_UNORM), not the swapchain. Binding the
     * swapchain-format pipeline there was spec-undefined (RADV
     * happened to tolerate it — review P1-6). */
    color_desc.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM;
    ps->gpu_pipeline_yuv_frame = SDL_CreateGPUGraphicsPipeline(
        ps->gpu_device, &pipe_info);
    color_desc.format = SDL_GetGPUSwapchainTextureFormat(
        ps->gpu_device, ps->window);

    /* Shaders are baked into the pipeline — release the objects */
    SDL_ReleaseGPUShader(ps->gpu_device, frag_yuv);

    if (!ps->gpu_pipeline_yuv || !ps->gpu_pipeline_yuv_frame) {
        log_msg("ERROR: Failed to create YUV pipeline: %s", SDL_GetError());
        SDL_ReleaseGPUShader(ps->gpu_device, vert);
        return -1;
    }
    log_msg("GPU: swapchain format = %d", (int)color_desc.format);
    log_msg("GPU: YUV planar pipelines created (fixed 4x4, swapchain + frame)");

    /* Dilated variant — non-fatal: without it, downscale falls back to
     * the fixed kernels (pre-item-8 quality, full speed).
     * DSVP_NO_DILATE=1 skips it entirely — the one-run falsification
     * switch for "is the dilated sampler the problem". */
    if (!SDL_getenv("DSVP_NO_DILATE")) {
        char frag_defs_dil[192];
        snprintf(frag_defs_dil, sizeof(frag_defs_dil),
                 "#define DSVP_DILATE 1\n#define DSVP_PQ_LUT %d\n"
                 "#define DSVP_DIRECT 0\n#define DSVP_SCALE2X 0\n%s",
                 ps->pq_lut_active, chroma_ar_defs());
        SDL_GPUShader *frag_dil = compile_shader_pfx(
            ps->gpu_device, hlsl_yuv_planar_frag, "main",
            SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT, frag_defs_dil);
        if (frag_dil) {
            pipe_info.fragment_shader = frag_dil;
            ps->gpu_pipeline_yuv_dilated = SDL_CreateGPUGraphicsPipeline(
                ps->gpu_device, &pipe_info);
            /* Intermediate-target variant (see fixed-kernel pair above).
             * Non-fatal like the swapchain dilated: a frame-target
             * downscale just falls back to the fixed frame pipeline. */
            color_desc.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM;
            ps->gpu_pipeline_yuv_dilated_frame = SDL_CreateGPUGraphicsPipeline(
                ps->gpu_device, &pipe_info);
            color_desc.format = SDL_GetGPUSwapchainTextureFormat(
                ps->gpu_device, ps->window);
            SDL_ReleaseGPUShader(ps->gpu_device, frag_dil);
        }
        if (ps->gpu_pipeline_yuv_dilated)
            log_msg("GPU: YUV planar pipeline created (dilated, for downscale)");
        else
            log_msg("WARN: dilated YUV pipeline unavailable (%s) — "
                    "fixed kernels for downscale too", SDL_GetError());
    }

    /* Direct variant — exact-1:1 only (4K-on-4K fullscreen). The fixed
     * kernel pays ~16 taps/plane to reproduce the identity there; at
     * the Deck's SUSTAINED (post-boost) GPU clock that surplus is the
     * difference between fitting the 16.7ms budget and missing it by
     * ~1ms forever (clock trace 2026-08-20: LOCKED at 1480-1540MHz,
     * metered drops at 1335-1450MHz). Non-fatal; DSVP_NO_DIRECT=1 is
     * the falsification/eye-test switch. */
    if (!SDL_getenv("DSVP_NO_DIRECT")) {
        char frag_defs_dir[192];
        snprintf(frag_defs_dir, sizeof(frag_defs_dir),
                 "#define DSVP_DILATE 0\n#define DSVP_PQ_LUT %d\n"
                 "#define DSVP_DIRECT 1\n#define DSVP_SCALE2X 0\n%s",
                 ps->pq_lut_active, chroma_ar_defs());
        SDL_GPUShader *frag_dir = compile_shader_pfx(
            ps->gpu_device, hlsl_yuv_planar_frag, "main",
            SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT, frag_defs_dir);
        if (frag_dir) {
            pipe_info.fragment_shader = frag_dir;
            ps->gpu_pipeline_yuv_direct = SDL_CreateGPUGraphicsPipeline(
                ps->gpu_device, &pipe_info);
            color_desc.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM;
            ps->gpu_pipeline_yuv_direct_frame = SDL_CreateGPUGraphicsPipeline(
                ps->gpu_device, &pipe_info);
            color_desc.format = SDL_GetGPUSwapchainTextureFormat(
                ps->gpu_device, ps->window);
            SDL_ReleaseGPUShader(ps->gpu_device, frag_dir);
        }
        if (ps->gpu_pipeline_yuv_direct)
            log_msg("GPU: YUV planar pipeline created (direct, exact 1:1)");
        else
            log_msg("WARN: direct YUV pipeline unavailable (%s) — "
                    "fixed kernels at 1:1 too", SDL_GetError());
    }

    /* Scale2x variant — exact 2.0x upscale (1080p->4K out, and 2x
     * letterboxed sources). Output phases are only 0.25/0.75 per
     * axis, so the Lanczos weights become two constant vectors:
     * identical taps, anti-ring and normalisation — the win is the
     * ~16 sin()/px/plane the general path spends recomputing them
     * (the transcendental-hog class the PQ pow chain belonged to).
     * Non-fatal; DSVP_NO_SCALE2X=1 is the falsification switch. */
    if (!SDL_getenv("DSVP_NO_SCALE2X")) {
        char frag_defs_2x[192];
        snprintf(frag_defs_2x, sizeof(frag_defs_2x),
                 "#define DSVP_DILATE 0\n#define DSVP_PQ_LUT %d\n"
                 "#define DSVP_DIRECT 0\n#define DSVP_SCALE2X 1\n%s",
                 ps->pq_lut_active, chroma_ar_defs());
        SDL_GPUShader *frag_2x = compile_shader_pfx(
            ps->gpu_device, hlsl_yuv_planar_frag, "main",
            SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT, frag_defs_2x);
        if (frag_2x) {
            pipe_info.fragment_shader = frag_2x;
            ps->gpu_pipeline_yuv_scale2x = SDL_CreateGPUGraphicsPipeline(
                ps->gpu_device, &pipe_info);
            color_desc.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM;
            ps->gpu_pipeline_yuv_scale2x_frame = SDL_CreateGPUGraphicsPipeline(
                ps->gpu_device, &pipe_info);
            color_desc.format = SDL_GetGPUSwapchainTextureFormat(
                ps->gpu_device, ps->window);
            SDL_ReleaseGPUShader(ps->gpu_device, frag_2x);
        }
        if (ps->gpu_pipeline_yuv_scale2x)
            log_msg("GPU: YUV planar pipeline created (scale2x, constant weights)");
        else
            log_msg("WARN: scale2x YUV pipeline unavailable (%s) — "
                    "general kernels at 2x too", SDL_GetError());
    }

    /* Vertex shader done — safe to release now */
    SDL_ReleaseGPUShader(ps->gpu_device, vert);

    /* ── Compile overlay RGBA fragment shader ── */
    SDL_GPUShader *frag_overlay = compile_shader(
        ps->gpu_device, hlsl_overlay_frag, "main",
        SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
    if (!frag_overlay) {
        log_msg("ERROR: Overlay shader compile failed");
        return -1;
    }

    /* ── Create overlay pipeline (alpha blending enabled) ──
     *
     * Standard alpha compositing: src.a * src + (1-src.a) * dst.
     * This is the "over" operator — overlay pixels with alpha < 1
     * blend with the video underneath. */
    {
        /* Need a fresh vertex shader since we released vert above */
        SDL_GPUShader *vert_overlay = compile_shader(
            ps->gpu_device, hlsl_fullscreen_vert, "main",
            SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
        if (!vert_overlay) {
            SDL_ReleaseGPUShader(ps->gpu_device, frag_overlay);
            log_msg("ERROR: Overlay vertex shader compile failed");
            return -1;
        }

        SDL_GPUColorTargetDescription overlay_color_desc;
        SDL_zero(overlay_color_desc);
        overlay_color_desc.format = SDL_GetGPUSwapchainTextureFormat(
            ps->gpu_device, ps->window);
        overlay_color_desc.blend_state.enable_blend          = true;
        overlay_color_desc.blend_state.src_color_blendfactor  = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        overlay_color_desc.blend_state.dst_color_blendfactor  = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        overlay_color_desc.blend_state.color_blend_op         = SDL_GPU_BLENDOP_ADD;
        overlay_color_desc.blend_state.src_alpha_blendfactor   = SDL_GPU_BLENDFACTOR_ONE;
        overlay_color_desc.blend_state.dst_alpha_blendfactor   = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        overlay_color_desc.blend_state.alpha_blend_op          = SDL_GPU_BLENDOP_ADD;
        overlay_color_desc.blend_state.color_write_mask        =
            SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
            SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;

        SDL_GPUGraphicsPipelineCreateInfo overlay_pipe;
        SDL_zero(overlay_pipe);
        overlay_pipe.vertex_shader   = vert_overlay;
        overlay_pipe.fragment_shader = frag_overlay;
        overlay_pipe.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
        overlay_pipe.target_info.num_color_targets        = 1;
        overlay_pipe.target_info.color_target_descriptions = &overlay_color_desc;

        ps->gpu_pipeline_overlay = SDL_CreateGPUGraphicsPipeline(
            ps->gpu_device, &overlay_pipe);

        SDL_ReleaseGPUShader(ps->gpu_device, vert_overlay);
        SDL_ReleaseGPUShader(ps->gpu_device, frag_overlay);

        if (!ps->gpu_pipeline_overlay) {
            log_msg("ERROR: Failed to create overlay pipeline: %s", SDL_GetError());
            return -1;
        }
        log_msg("GPU: overlay pipeline created (alpha blend)");
    }

    /* ── Create frame-blit pipeline (no blend — blit covers the target) ── */
    {
        SDL_GPUShader *vert_blit = compile_shader(
            ps->gpu_device, hlsl_fullscreen_vert, "main",
            SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
        SDL_GPUShader *frag_blit = compile_shader(
            ps->gpu_device, hlsl_blit_frag, "main",
            SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
        if (vert_blit && frag_blit) {
            SDL_GPUColorTargetDescription blit_desc;
            SDL_zero(blit_desc);
            blit_desc.format = SDL_GetGPUSwapchainTextureFormat(
                ps->gpu_device, ps->window);

            SDL_GPUGraphicsPipelineCreateInfo blit_pipe;
            SDL_zero(blit_pipe);
            blit_pipe.vertex_shader   = vert_blit;
            blit_pipe.fragment_shader = frag_blit;
            blit_pipe.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
            blit_pipe.target_info.num_color_targets         = 1;
            blit_pipe.target_info.color_target_descriptions = &blit_desc;

            ps->gpu_pipeline_blit = SDL_CreateGPUGraphicsPipeline(
                ps->gpu_device, &blit_pipe);
        }
        if (vert_blit) SDL_ReleaseGPUShader(ps->gpu_device, vert_blit);
        if (frag_blit) SDL_ReleaseGPUShader(ps->gpu_device, frag_blit);
        if (ps->gpu_pipeline_blit) {
            log_msg("GPU: frame-blit pipeline created");
        } else {
            /* Not fatal: the direct render path works without it. */
            log_msg("WARN: frame-blit pipeline unavailable (%s) — "
                    "direct render path", SDL_GetError());
        }
    }

    /* ── Create sampler (linear filtering, no anisotropy) ──
     * The fragment shader does its own Lanczos/Catmull-Rom multi-tap
     * resampling via SampleLevel(..., 0). Hardware anisotropy adds
     * nothing on a flat fullscreen quad — it only helps when texture
     * coordinates are foreshortened by perspective. */
    SDL_GPUSamplerCreateInfo samp_info;
    SDL_zero(samp_info);
    samp_info.min_filter     = SDL_GPU_FILTER_LINEAR;
    samp_info.mag_filter     = SDL_GPU_FILTER_LINEAR;
    samp_info.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samp_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samp_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samp_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

    ps->gpu_sampler = SDL_CreateGPUSampler(ps->gpu_device, &samp_info);
    if (!ps->gpu_sampler) {
        log_msg("ERROR: Failed to create sampler: %s", SDL_GetError());
        return -1;
    }
    log_msg("GPU: sampler created (linear, no anisotropy)");

    /* ── Create nearest-neighbor sampler for overlay ──
     * Bitmap font pixels should be pixel-perfect, not bilinear-blurred. */
    SDL_GPUSamplerCreateInfo nearest_info;
    SDL_zero(nearest_info);
    nearest_info.min_filter     = SDL_GPU_FILTER_NEAREST;
    nearest_info.mag_filter     = SDL_GPU_FILTER_NEAREST;
    nearest_info.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    nearest_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    nearest_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    nearest_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

    ps->gpu_sampler_nearest = SDL_CreateGPUSampler(ps->gpu_device, &nearest_info);
    if (!ps->gpu_sampler_nearest) {
        log_msg("ERROR: Failed to create nearest sampler: %s", SDL_GetError());
        return -1;
    }
    log_msg("GPU: nearest sampler created (overlay)");

    /* ── Create and upload blue noise dither texture (64×64, R8_UNORM) ──
     * Uploaded once at startup. Lives for the entire application lifetime.
     * Nearest-neighbor sampling preserves exact noise values — bilinear
     * would low-pass the texture and destroy its blue spectral character. */
    {
        SDL_GPUTextureCreateInfo noise_tex_info;
        SDL_zero(noise_tex_info);
        noise_tex_info.type                 = SDL_GPU_TEXTURETYPE_2D;
        noise_tex_info.format               = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        noise_tex_info.width                = 64;
        noise_tex_info.height               = 64;
        noise_tex_info.layer_count_or_depth = 1;
        noise_tex_info.num_levels           = 1;
        noise_tex_info.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;

        ps->gpu_tex_noise = SDL_CreateGPUTexture(ps->gpu_device, &noise_tex_info);
        if (!ps->gpu_tex_noise) {
            log_msg("ERROR: Failed to create blue noise texture: %s", SDL_GetError());
            return -1;
        }

        /* Upload via transfer buffer */
        SDL_GPUTransferBufferCreateInfo xfer_info;
        SDL_zero(xfer_info);
        xfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        xfer_info.size  = 64 * 64;  /* R8 = 1 byte per texel */

        SDL_GPUTransferBuffer *xfer = SDL_CreateGPUTransferBuffer(
            ps->gpu_device, &xfer_info);
        if (!xfer) {
            log_msg("ERROR: Failed to create blue noise transfer buffer: %s",
                    SDL_GetError());
            return -1;
        }

        uint8_t *dst = SDL_MapGPUTransferBuffer(ps->gpu_device, xfer, false);
        if (dst) {
            memcpy(dst, blue_noise_64, 64 * 64);
            SDL_UnmapGPUTransferBuffer(ps->gpu_device, xfer);
        }

        /* Upload to GPU texture */
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(ps->gpu_device);
        if (cmd) {
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureTransferInfo src_info;
            SDL_GPUTextureRegion dst_region;
            SDL_zero(src_info);
            SDL_zero(dst_region);
            src_info.transfer_buffer = xfer;
            src_info.pixels_per_row  = 64;
            src_info.rows_per_layer  = 64;
            dst_region.texture = ps->gpu_tex_noise;
            dst_region.w = 64;
            dst_region.h = 64;
            dst_region.d = 1;
            SDL_UploadToGPUTexture(copy, &src_info, &dst_region, false);
            SDL_EndGPUCopyPass(copy);
            SDL_SubmitGPUCommandBuffer(cmd);
        }

        SDL_ReleaseGPUTransferBuffer(ps->gpu_device, xfer);
        log_msg("GPU: blue noise dither texture created (64x64 R8_UNORM)");
    }

    return 0;
}

/* ── System-level display HDR (docs/TODO-HDR.md) ──
 *
 * KWin's Wayland color management ACCEPTS a PQ surface without ever
 * switching the DISPLAY into HDR — it tone-maps to SDR itself
 * (verified 2026-08-07: no HDR infoframe, colorspace=Default on the
 * DRM connector). The per-output HDR switch is compositor policy
 * with no client protocol — but it IS scriptable: kscreen-doctor
 * output.<name>.hdr.enable flips the same property as the KDE GUI,
 * instantly, and the kernel confirms with colorspace=BT2020_RGB +
 * an HDR_OUTPUT_METADATA blob on the wire. So DSVP orchestrates it:
 * enable while an HDR file plays fullscreen (the desktop is covered
 * the whole time), restore on close/quit. DSVP_NO_SYS_HDR=1 opts
 * out; every failure path falls back to the compositor-rendered
 * behaviour, which is today's baseline. */

/* kscreen-doctor colours its output with ANSI escapes even into a
 * pipe — the first field-test parse failed on exactly that (matched
 * "Output:" against "\x1b[1mOutput:"). Strip CSI sequences in place
 * before any matching. */
static void strip_ansi(char *s) {
    char *w = s;
    for (char *r = s; *r; ) {
        if (*r == 0x1b && r[1] == '[') {
            r += 2;
            while (*r && !(*r >= '@' && *r <= '~')) r++;
            if (*r) r++;   /* consume the final byte */
        } else *w++ = *r++;
    }
    *w = '\0';
}

/* Read a kscreen-doctor property line's value.
 * 1 = enabled, 0 = disabled, -1 = incapable or unrecognised.
 * ("disabled" contains no "enabled" substring, so the order is safe.) */
static int hdr_prop_state(const char *s) {
    if (strstr(s, "incapable")) return -1;
    if (strstr(s, "enabled"))   return  1;
    if (strstr(s, "disabled"))  return  0;
    return -1;
}

/* Is this the wide-colour-gamut property line? The label has been
 * spelled several ways across Plasma releases, so match the key part
 * case-insensitively rather than pinning one string — an unmatched
 * label leaves WCG at "unknown", which is the safe state (we then
 * never write it). */
static int hdr_line_is_wcg(const char *s) {
    char key[64];
    size_t i = 0;
    for (; i < sizeof(key) - 1 && s[i] && s[i] != ':'; i++)
        key[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
    key[i] = '\0';
    return strstr(key, "wide color gamut") != NULL
        || strstr(key, "wide colour gamut") != NULL
        || strstr(key, "wcg") != NULL;
}

/* Adopt this output if it is the one we can drive. Called at each
 * block boundary because the properties we need can appear in any
 * order within an output's block. */
static int hdr_sys_adopt(PlayerState *ps, const char *name,
                         int enabled, int hdr, int wcg) {
    if (!enabled || !name[0] || hdr < 0) return 0;
    /* Sanitize before this ever reaches an exec argument */
    for (const char *c = name; *c; c++)
        if (!((*c>='A'&&*c<='Z')||(*c>='a'&&*c<='z')||
              (*c>='0'&&*c<='9')||*c=='-')) return 0;
    snprintf(ps->hdr_sys_output, sizeof(ps->hdr_sys_output), "%s", name);
    ps->hdr_sys_prior_hdr = hdr;
    ps->hdr_sys_prior_wcg = wcg;
    return 1;
}

/* Find the connected, enabled output whose HDR capability is real
 * ("HDR: enabled|disabled" — the internal panel says "incapable").
 * Records the output name and the CURRENT state of every property we
 * are going to write, so revert restores rather than assumes.
 *
 * WCG is read here because hdr_sys_set writes it. It used to be
 * written but never read: revert issued wcg.disable unconditionally,
 * on the assumption that WCG tracked HDR. On a display where the user
 * had wide gamut on with HDR off, that silently turned it off and
 * LEFT it off — kscreen persists output properties, so the change
 * outlived the process, the session, and reboots. Anything we write,
 * we read first.
 *
 * Parser is host-unit-tested against coloured, plain, and
 * prior-enabled fixtures built from real kscreen-doctor output. */
static void hdr_sys_wait(void);

static int hdr_sys_detect(PlayerState *ps) {
    /* Serialize behind any in-flight kscreen-doctor child first. The
     * close path fires a fire-and-forget disable; a playlist advance
     * reopens within milliseconds and a probe here would read the
     * PRE-disable state — latching prior_hdr=1, skipping the engage,
     * and inverting the restore baseline for the whole session (review
     * 2026-08-20 finding 4 — the F4 class of persistent display-state
     * damage, resurrected through a race). */
    hdr_sys_wait();
    ps->hdr_sys_output[0] = '\0';
    ps->hdr_sys_prior_hdr = 0;
    ps->hdr_sys_prior_wcg = -1;
    if (SDL_getenv("DSVP_NO_SYS_HDR")) return 0;

    FILE *fp = popen("kscreen-doctor -o 2>/dev/null", "r");
    if (!fp) {
        log_msg("HDR sys: popen(kscreen-doctor) failed");
        return 0;
    }

    char line[512];
    char cur_name[32] = "";
    int  cur_enabled = 0, cur_hdr = -1, cur_wcg = -1;
    int  lines_seen = 0, outputs_seen = 0, found = 0;

    while (fgets(line, sizeof(line), fp)) {
        lines_seen++;
        strip_ansi(line);
        const char *s = line;
        while (*s == ' ' || *s == '\t') s++;

        if (strncmp(s, "Output:", 7) == 0) {
            /* Block boundary: decide on the block that just ended */
            if (!found)
                found = hdr_sys_adopt(ps, cur_name, cur_enabled,
                                      cur_hdr, cur_wcg);
            /* "Output: 2 DP-1 <uuid>" — name is the third token */
            outputs_seen++;
            cur_name[0] = '\0';
            cur_enabled = 0;
            cur_hdr = -1;
            cur_wcg = -1;
            char idx[16];
            if (sscanf(s, "Output: %15s %31s", idx, cur_name) != 2)
                cur_name[0] = '\0';
        } else if (strncmp(s, "enabled", 7) == 0 &&
                   (s[7]=='\n' || s[7]=='\0' || s[7]=='\r' || s[7]==' ')) {
            cur_enabled = 1;
        } else if (strncmp(s, "HDR:", 4) == 0) {
            cur_hdr = hdr_prop_state(s);
        } else if (hdr_line_is_wcg(s)) {
            cur_wcg = hdr_prop_state(s);
        }
    }
    if (!found)
        found = hdr_sys_adopt(ps, cur_name, cur_enabled, cur_hdr, cur_wcg);
    pclose(fp);

    if (found) {
        log_msg("HDR sys: output %s is HDR-capable — baseline hdr=%s wcg=%s",
                ps->hdr_sys_output,
                ps->hdr_sys_prior_hdr ? "enabled" : "disabled",
                ps->hdr_sys_prior_wcg < 0 ? "unknown"
                    : (ps->hdr_sys_prior_wcg ? "enabled" : "disabled"));
        return 1;
    }
    log_msg("HDR sys: no HDR-capable enabled output found "
            "(read %d lines, %d outputs) — compositor-rendered fallback",
            lines_seen, outputs_seen);
    return 0;
}

/* ── Async kscreen-doctor execution ──
 *
 * kscreen-doctor takes ~2s to flip a display output's HDR/WCG state.
 * The old synchronous system() call blocked the main loop for the
 * entire duration — no events, no presents — and KWin dimmed the
 * window as unresponsive.
 *
 * posix_spawnp runs the command without blocking.  hdr_sys_wait()
 * serializes: each new spawn waits for the previous child (normally
 * already exited, so instant; only blocks on rapid toggles, which is
 * correct serialization).  Shutdown calls hdr_sys_wait() explicitly
 * so the process doesn't exit with HDR still enabled on the display. */

static pid_t s_hdr_sys_pid = 0;

/* Exit code of the most recently reaped kscreen-doctor child. The
 * status used to be discarded (waitpid(..., NULL, 0)) — a
 * spawn-succeeds/command-fails enable was indistinguishable from
 * success (review 2026-08-20 finding 14, PLAUSIBLE half, promoted
 * by the field: a latched-but-never-landed launch enable is the
 * exact "no badge, no pop, correct colour" phenotype). */
static int s_hdr_sys_last_rc = 0;

static void hdr_sys_wait(void) {
    if (s_hdr_sys_pid > 0) {
        int st = 0;
        waitpid(s_hdr_sys_pid, &st, 0);
        s_hdr_sys_pid = 0;
        s_hdr_sys_last_rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (s_hdr_sys_last_rc != 0)
            log_msg("HDR sys: kscreen-doctor exited rc=%d — the last "
                    "display switch may NOT have landed",
                    s_hdr_sys_last_rc);
    }
}

/* ── Crash-restore stamp (review 2026-08-20 finding 18, Holden-
 * approved design) ──
 * kscreen persists HDR across sessions and reboots; a SIGKILL, SEGV
 * or driver death while we hold display HDR strands the desktop
 * there with no handler able to run — the F4 lesson: persistent
 * display state is the damage that outlives the process. So: a
 * stamp written when we flip the display, cleared when we restore,
 * reconciled synchronously at the next launch before any probe. */
/* Returns 0 (buf empty) when HOME is unset. The old fallback was the
 * fixed world-writable path /tmp/.dsvp-hdr-restore — any local user
 * could pre-seed it (its contents reach a popen'd command line) or
 * symlink it for the fopen("w") to clobber (Knot audit finding 7).
 * The stamp is crash insurance, not a feature worth a predictable
 * path in /tmp: with no HOME we simply run without it. */
static int hdr_stamp_path(char *buf, size_t n) {
    const char *h = SDL_getenv("HOME");
    if (!h || !h[0]) { buf[0] = '\0'; return 0; }
    snprintf(buf, n, "%s/.dsvp-hdr-restore", h);
    return 1;
}

static void hdr_stamp_update(PlayerState *ps, int held) {
    char path[512];
    if (!hdr_stamp_path(path, sizeof(path))) return;
    if (!held) { unlink(path); return; }
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s %d %d %d\n", ps->hdr_sys_output,
            ps->hdr_sys_prior_hdr, ps->hdr_sys_prior_wcg,
            (int)getpid());
    fclose(f);
}

/* True when the pid recorded in a stamp belongs to a LIVE dsvp
 * process. Without this a second concurrent instance (shim daemon
 * spawn beside a desktop session, or a plain double launch) read the
 * first instance's stamp as "previous session died", restored the
 * display baseline under its feet mid-passthrough, and unlinked its
 * crash protection. The comm check keeps a recycled pid from
 * counting as ours. */
static int hdr_stamp_owner_alive(int pid) {
    if (pid <= 0 || kill((pid_t)pid, 0) != 0) return 0;
    char cpath[64], comm[32] = "";
    snprintf(cpath, sizeof(cpath), "/proc/%d/comm", pid);
    FILE *cf = fopen(cpath, "r");
    if (!cf) return 0;
    if (!fgets(comm, sizeof(comm), cf)) comm[0] = '\0';
    fclose(cf);
    comm[strcspn(comm, "\n")] = '\0';
    /* Exact match: a prefix test would also accept the long-running
     * dsvp-shim daemon, the likeliest home for a recycled pid. */
    return strcmp(comm, "dsvp") == 0;
}

void hdr_sys_reconcile_stamp(void) {
    char path[512];
    if (!hdr_stamp_path(path, sizeof(path))) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char out[64] = "";
    int phdr = 0, pwcg = -1, spid = 0;
    int n = fscanf(f, "%63s %d %d %d", out, &phdr, &pwcg, &spid);
    fclose(f);
    /* Re-apply hdr_sys_adopt's character whitelist on the READ side:
     * out reaches a popen'd command line below, and the write-side
     * sanitisation does not protect a stamp file we did not write.
     * The rule this tree already earned — anything we write to shared
     * state we read first — has a mirror: anything read back from
     * shared state is re-validated (Knot audit finding 7). */
    for (const char *c = out; *c; c++) {
        if (!((*c>='A'&&*c<='Z')||(*c>='a'&&*c<='z')||
              (*c>='0'&&*c<='9')||*c=='-')) {
            log_msg("HDR sys: stamp output name rejected — not restoring");
            unlink(path);
            return;
        }
    }
    if (n == 4 && hdr_stamp_owner_alive(spid)) {
        /* The session that wrote this is still running and still owns
         * the display — leave both the display and the stamp alone.
         * (n == 3 is a pre-pid stamp: its writer predates this check,
         * so it reconciles exactly as before.) */
        log_msg("HDR sys: stamp belongs to live dsvp pid %d — "
                "not reconciling", spid);
        return;
    }
    if (n >= 3 && out[0]) {
        /* Mirror hdr_sys_set's restore semantics: hdr back to the
         * recorded baseline; wcg only if the dead session had turned
         * it on itself (prior_wcg == 0). Synchronous — the session
         * that follows must probe the TRUE baseline. */
        char wcg_part[128] = "";
        if (pwcg == 0)
            snprintf(wcg_part, sizeof(wcg_part),
                     " output.%s.wcg.disable", out);
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "kscreen-doctor output.%s.hdr.%s%s 2>/dev/null",
                 out, phdr ? "enable" : "disable", wcg_part);
        log_msg("HDR sys: previous session died holding display HDR — "
                "restoring baseline on %s (stamp file)", out);
        FILE *p = popen(cmd, "r");
        if (p) pclose(p);
    }
    unlink(path);
}

static void hdr_sys_set(PlayerState *ps, int on) {
    if (!ps->hdr_sys_output[0]) return;

    hdr_sys_wait();

    /* HDR is ours to drive: on the way out it goes back to the state
     * detect recorded, not to a hardcoded "disable". */
    char hdr_arg[64], wcg_arg[64] = "";
    snprintf(hdr_arg, sizeof(hdr_arg), "output.%s.hdr.%s",
             ps->hdr_sys_output,
             (on || ps->hdr_sys_prior_hdr) ? "enable" : "disable");

    /* WCG is only ever written when detect actually read it AND it was
     * off — i.e. only when engaging HDR requires turning it on, and
     * then only to put it back exactly as found. Unknown (-1) or
     * already-on means we never touch it: kscreen persists these
     * properties, so a write we can't reverse is a permanent change to
     * the user's display for a setting they didn't ask us to manage. */
    int write_wcg = (ps->hdr_sys_prior_wcg == 0);
    if (write_wcg)
        snprintf(wcg_arg, sizeof(wcg_arg), "output.%s.wcg.%s",
                 ps->hdr_sys_output, on ? "enable" : "disable");

    extern char **environ;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                     "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO,
                                     STDERR_FILENO);

    char *argv[4];
    int argn = 0;
    argv[argn++] = "kscreen-doctor";
    argv[argn++] = hdr_arg;
    if (write_wcg) argv[argn++] = wcg_arg;
    argv[argn] = NULL;

    pid_t pid;
    int rc = posix_spawnp(&pid, "kscreen-doctor", &actions, NULL,
                          argv, environ);
    posix_spawn_file_actions_destroy(&actions);

    if (rc == 0) {
        s_hdr_sys_pid = pid;
        hdr_stamp_update(ps, on);
        log_msg("HDR sys: display HDR %s on %s (wcg %s, async, pid=%d)",
                on ? "ENABLED" : "restored to baseline",
                ps->hdr_sys_output,
                write_wcg ? (on ? "enabled" : "restored to disabled")
                          : "untouched",
                (int)pid);
    } else {
        log_msg("HDR sys: posix_spawnp failed (rc=%d)", rc);
        /* No child to reap, so hdr_sys_wait will never record this
         * failure — a stale rc of 0 let verify_hold certify a launch
         * hold that was never even spawned and skip its retry. */
        s_hdr_sys_last_rc = rc ? rc : -1;
    }
}

/* Auto-selection input for PQ output: is the display we can drive
 * currently running wide gamut? That is the exact condition under
 * which plain BT.709 output is wrong — the display reads our 709 code
 * values against BT.2020 primaries and stretches every colour outward.
 * Returns 0 when there is no drivable HDR output at all, or when the
 * display is an ordinary BT.709 one and the existing SDR path is
 * already correct. Honours DSVP_NO_SYS_HDR through hdr_sys_detect. */
int hdr_sys_display_is_wide_gamut(PlayerState *ps) {
    if (!hdr_sys_detect(ps)) return 0;
    return ps->hdr_sys_prior_wcg == 1;
}

/* Pre-enable display HDR at startup so fullscreen works immediately.
 * The SDR fullscreen path on certain displays (LG C4 via DP dock)
 * produces frozen glass — all app instruments healthy, below-compositor,
 * windowed unaffected. Field-established 2026-08-17: the freeze tracks
 * the DISPLAY's HDR state, not our launch path or any app state, and it
 * survives reboot and TV wall-unplug. Holding the output in HDR for the
 * whole session sidesteps it.
 *
 * COST, not yet eye-verified: the swapchain stays SDR until an HDR file
 * opens, so SDR content is mapped up to a PQ/BT.2020 output by the
 * compositor. That is NOT the DSVP_NO_SYS_HDR=1 baseline — that path
 * leaves the display in SDR and shows SDR content natively. Whether the
 * mapped-up picture is reference-accurate on this panel is an open
 * question for the eye, and the reason this is a workaround for a
 * below-app fault rather than an architecture we chose.
 *
 * OPT-IN (DSVP_FS_HDR_FALLBACK=1), because the cost is real: SDR
 * content mapped up to a PQ output was judged clearly worse by eye
 * (2026-08-17), and this fires for every HDR-capable display whether
 * or not it has the fault. It stays in the tree as the fallback for
 * a display that freezes in SDR fullscreen and has no better fix.
 * DSVP_NO_SYS_HDR=1 still opts out of everything. */
/* True when display HDR is a SESSION hold (PQ container, or the
 * fullscreen-freeze fallback) rather than a per-content engage. The
 * hold must survive HDR-file closes AND the failure branches — the
 * fallback hold used to die on the first HDR close, resurrecting the
 * exact freeze it shipped to prevent (review 2026-08-20 finding 9). */
static int hdr_sys_session_hold(PlayerState *ps) {
    return ps->out_pq_nits > 0.0f
        || SDL_getenv("DSVP_FS_HDR_FALLBACK") != NULL;
}

/* DIAG (DSVP_PQ_NOMATH=1): feed the shader out_pq=0 while the
 * swapchain stays HDR10/PQ. Colours are WRONG on purpose (gamma 2.2
 * values PQ-decoded by the compositor) — this run isolates OUR PQ
 * encode math from KWin's ingest of a PQ surface, the unmeasured
 * split inside attribution v3's A−C (TODO-PACING item 0). Every
 * uniform write goes through here so the probe can't miss a site. */
static float out_pq_uniform(PlayerState *ps) {
    static int nomath = -1;
    if (nomath < 0) {
        nomath = SDL_getenv("DSVP_PQ_NOMATH") != NULL;
        if (nomath)
            log_msg("DIAG: DSVP_PQ_NOMATH — shader PQ encode OFF on a "
                    "PQ swapchain; colours wrong BY DESIGN (perf probe)");
    }
    return nomath ? 0.0f : ps->out_pq_nits;
}

void hdr_sys_preenable(PlayerState *ps) {
    /* PQ output needs the display in HDR for the whole session, not just
     * while an HDR file is open — the container is HDR10 even when the
     * content is SDR. */
    if (!hdr_sys_session_hold(ps))
        return;
    /* Reuse the startup wide-gamut probe when it ran (finding 25: the
     * duplicate blocking kscreen-doctor popen showed as a double
     * "HDR-capable — baseline" line in every field log). */
    if ((ps->hdr_sys_output[0] != '\0' || hdr_sys_detect(ps))
            && !ps->hdr_sys_prior_hdr) {
        hdr_sys_set(ps, 1);
        ps->hdr_sys_enabled_by_us = 1;
        log_msg("HDR sys: pre-enabled on %s (%s)", ps->hdr_sys_output,
                ps->out_pq_nits > 0.0f
                    ? "PQ output — the container is HDR10 even for SDR, "
                      "so the display stays in HDR for the session"
                    : "DSVP_FS_HDR_FALLBACK — SDR picture is compromised "
                      "while this is on");
    }
}

/* One-shot, a few seconds after a session-hold launch: the pre-enable
 * is async so the display's mode switch can overlap shader compile,
 * which also means nothing ever CONFIRMED it landed. Reap the child
 * with status, retry once on failure, and log the actual DRM wire
 * state either way — every session's log then answers "was the
 * display really in HDR while SDR content showed", which is exactly
 * the question the no-badge/no-pop field reports could not settle
 * (2026-08-20). */
void hdr_sys_verify_hold(PlayerState *ps) {
    if (!ps->hdr_sys_enabled_by_us) return;
    hdr_sys_wait();
    if (s_hdr_sys_last_rc != 0) {
        log_msg("HDR sys: launch pre-enable FAILED (rc=%d) — retrying",
                s_hdr_sys_last_rc);
        hdr_sys_set(ps, 1);
        hdr_sys_wait();
        if (s_hdr_sys_last_rc != 0)
            log_msg("HDR sys: retry also failed — display likely NOT "
                    "in HDR; KWin will tone-map the PQ surface "
                    "(flat highlights, no badge)");
    }
    hdrwire_log_state();
}

/* Backstop: restore the display if we exit while it is still ours.
 * The normal path is hdr_output_apply on file close, which hands the
 * display back the moment HDR content stops and clears the flag —
 * so by the time we get here there is usually nothing to do. This
 * catches the paths that skip it: quit mid-file, window close, and
 * the signal handlers. The blocking wait is acceptable here and
 * nowhere else: the process must not exit before kscreen-doctor has
 * actually put the output back. */
void hdr_output_shutdown(PlayerState *ps) {
    if (ps->hdr_sys_enabled_by_us) {
        hdr_sys_set(ps, 0);
        hdr_sys_wait();
        ps->hdr_sys_enabled_by_us = 0;
    }
}

/* ── HDR output switching (docs/TODO-HDR.md) ──
 * Reconcile the swapchain with (mode, content, display support).
 * The DESKTOP is never touched: we request an HDR10/ST2084 surface
 * for our window only; the compositor engages display HDR while we
 * present and reverts when we stop. Pipelines are format-bound to
 * the swapchain, so a switch recreates them (the ~400 ms shader
 * recompile hides behind the TV's own 1-2 s HDR mode-switch blank;
 * the recreated overlay texture is safe by the fresh-texture
 * full-upload rule). Called at file open, file close, and Z. */
void hdr_output_apply(PlayerState *ps) {
    if (!ps->gpu_device || !ps->window) return;

    int want = ps->hdr_out_mode == 1
            && ps->hdr_pass_content
            && SDL_WindowSupportsGPUSwapchainComposition(ps->gpu_device,
                   ps->window, SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084);

    if (want == ps->hdr_out_active) {
        ps->gpu_uniforms.hdr_pass = want ? 1.0f : 0.0f;
        return;
    }

    /* Engage the DISPLAY first: without this, KWin accepts the PQ
     * surface but tone-maps it to an SDR output itself. The TV's own
     * mode switch overlaps our pipeline recreation below.
     *
     * The ordering is load-bearing, so this is the one site that waits
     * for kscreen-doctor: hdr_sys_set is fire-and-forget everywhere
     * else, but returning before the output is actually in HDR would
     * hand KWin a PQ surface on an SDR output and silently lose
     * passthrough. The wait costs nothing in practice — the TV blanks
     * for its own 1-2 s mode switch either way.
     *
     * hdr_sys_enabled_by_us is the guard, not a fresh probe: when
     * startup pre-enable already asked for HDR, its kscreen-doctor
     * child may still be in flight, and a probe here would read the
     * pre-flip state, report "not HDR", and fire a duplicate enable. */
    if (want && !ps->hdr_sys_enabled_by_us) {
        /* Reuse the session baseline when one exists: with the desktop
         * already in HDR (prior_hdr=1) this block used to run a
         * BLOCKING kscreen-doctor probe on every toggle into
         * passthrough — 30-60ms drops per Z press in the 2026-08-20
         * baseline-hdr-on field log. Same cached-probe pattern as
         * hdr_sys_preenable; the trade is that a mid-session manual
         * HDR change by the user is not re-read, which the fresh
         * probe raced anyway. */
        if ((ps->hdr_sys_output[0] != '\0' || hdr_sys_detect(ps))
                && !ps->hdr_sys_prior_hdr) {
            hdr_sys_set(ps, 1);
            hdr_sys_wait();
            ps->hdr_sys_enabled_by_us = 1;
        }
    }

    /* Non-passthrough output can also ride the HDR10 container: BT.2020
     * needs 10 bits by spec and the SDR swapchain is 8, so converting
     * primaries into a narrower code range and quantising there is what
     * made the wide-gamut picture look blocky and soft. A2R10G10B10
     * gives the missing bits, and we PQ-encode SDR at a reference white
     * ourselves rather than leaving the mapping to the compositor —
     * which is what Windows and macOS do for SDR on an HDR display.
     * (The extended-linear/scRGB route was tried first and changed
     * nothing visible: KWin appears not to colour-manage that surface
     * either. Removed rather than left as dead weight — 932e34a.) */
    SDL_GPUSwapchainComposition comp =
        (want || ps->out_pq_nits > 0.0f)
            ? SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084
            : SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    int comp_hdr10 = (comp == SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084);

    /* ── Recreate-skip (TODO-PACING carried item, field-witnessed
     * 2026-08-20) ── In PQ mode both sides of an HDR↔SDR passthrough
     * flip ride the HDR10 composition, yet this function still tore
     * down and recompiled every pipeline (~400 ms) behind a full
     * WaitForGPUIdle — the log showed it as 731/761 ms freezes with
     * drop cascades on every mid-playback Z toggle. When the
     * composition is not actually changing, the entire transition is
     * a uniform update. The display-handback tail is kept identical
     * to the full path below; it is unreachable in PQ mode
     * (out_pq_nits > 0 is what makes the composition stick), and
     * outside PQ mode a want-flip always changes the composition, so
     * this branch never skips a real rebuild. */
    if (comp_hdr10 == ps->swapchain_hdr10) {
        ps->hdr_out_active = want;
        ps->gpu_uniforms.hdr_pass = want ? 1.0f : 0.0f;
        ps->gpu_uniforms.out_pq   = want ? 0.0f : out_pq_uniform(ps);
        ps->frame_render_dirty = 1;
        log_msg("HDR out: %s (composition unchanged — no pipeline "
                "rebuild)", want
                ? "PASSTHROUGH — HDR10/ST2084 swapchain, display tone-maps"
                : "SDR — tone-mapped output");
        if (!want && ps->hdr_sys_enabled_by_us && !hdr_sys_session_hold(ps)) {
            hdr_sys_set(ps, 0);
            ps->hdr_sys_enabled_by_us = 0;
        }
        return;
    }

    SDL_WaitForGPUIdle(ps->gpu_device);
    if (!SDL_SetGPUSwapchainParameters(ps->gpu_device, ps->window,
            comp, SDL_GPU_PRESENTMODE_VSYNC)) {
        log_msg("HDR out: swapchain switch failed (%s) — staying %s",
                SDL_GetError(), ps->hdr_out_active ? "HDR" : "SDR");
        ps->gpu_uniforms.hdr_pass = ps->hdr_out_active ? 1.0f : 0.0f;
        ps->gpu_uniforms.out_pq =
            ps->hdr_out_active ? 0.0f : out_pq_uniform(ps);
        if (want && ps->hdr_sys_enabled_by_us
                && !hdr_sys_session_hold(ps)) {
            hdr_sys_set(ps, 0);   /* don't leave the TV in HDR for SDR */
            ps->hdr_sys_enabled_by_us = 0;
        }
        return;
    }

    gpu_destroy_pipelines(ps);
    if (gpu_create_pipelines(ps) < 0) {
        /* Limp back to SDR — an SDR-format pipeline set is the known-
         * good configuration from startup. */
        log_msg("ERROR: pipeline recreation after HDR switch failed — "
                "reverting to SDR");
        SDL_SetGPUSwapchainParameters(ps->gpu_device, ps->window,
            SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);
        ps->swapchain_hdr10 = 0;
        /* The failed attempt has no self-cleanup — destroy whatever it
         * did create before retrying, or the retry's unconditional
         * assignments orphan those GPU objects (review P2-26). */
        gpu_destroy_pipelines(ps);
        if (gpu_create_pipelines(ps) < 0)
            log_msg("FATAL: SDR pipeline recreation also failed");
        ps->hdr_out_active = 0;
        ps->gpu_uniforms.hdr_pass = 0.0f;
        if (ps->hdr_sys_enabled_by_us && !hdr_sys_session_hold(ps)) {
            hdr_sys_set(ps, 0);
            ps->hdr_sys_enabled_by_us = 0;
        }
        return;
    }

    ps->hdr_out_active = want;
    ps->swapchain_hdr10 = comp_hdr10;
    ps->gpu_uniforms.hdr_pass = want ? 1.0f : 0.0f;
    /* PQ encode applies to OUR SDR output only. Passthrough already
     * ships PQ code values straight from the stream and must not be
     * re-encoded. */
    ps->gpu_uniforms.out_pq = want ? 0.0f : out_pq_uniform(ps);
    ps->frame_render_dirty = 1;   /* render state changed */
    log_msg("HDR out: %s", want
            ? "PASSTHROUGH — HDR10/ST2084 swapchain, display tone-maps"
            : "SDR — tone-mapped output");

    /* Leaving passthrough: hand the display back immediately. The
     * whole point of the smart switch is that HDR is on ONLY while
     * HDR content is on screen — the desktop, the browser and every
     * SDR file that follows get the user's own configuration back.
     *
     * This was deferred to app exit for one day (194054c) as a
     * workaround while the fullscreen freeze was misdiagnosed as
     * "SDR output is the broken state". It wasn't: the freeze tracked
     * WIDE GAMUT, which we were disabling here without ever having
     * read it (fixed in aeed00c — hdr_sys_set now restores what
     * detect actually found and never touches WCG it did not turn
     * on). With the real cause fixed, deferring is just a bug: one
     * HDR file left the display in HDR for the rest of the session
     * and past app exit, which is the opposite of the feature.
     *
     * hdr_sys_set is fire-and-forget, and no ordering constraint
     * applies on the way out — the swapchain is already back to SDR
     * above, so the display can follow whenever it gets there.
     *
     * EXCEPT in PQ mode, where "leaving passthrough" does not mean
     * leaving HDR: our SDR output is itself PQ/BT.2020 in an HDR10
     * container, so the display has to stay in HDR for the whole
     * session. Handing it back here left every SDR file after an HDR
     * one sending PQ code values to a display that had returned to
     * SDR — which is exactly the "colour fix never kicked in" case,
     * and only after an HDR file, because a fresh launch never
     * reached this branch. hdr_output_shutdown restores it at exit. */
    if (!want && ps->hdr_sys_enabled_by_us && !hdr_sys_session_hold(ps)) {
        hdr_sys_set(ps, 0);
        ps->hdr_sys_enabled_by_us = 0;
    }
}

void gpu_destroy_pipelines(PlayerState *ps) {
    if (!ps->gpu_device) return;

    gpu_overlay_destroy(ps);

    if (ps->gpu_sampler) {
        SDL_ReleaseGPUSampler(ps->gpu_device, ps->gpu_sampler);
        ps->gpu_sampler = NULL;
    }
    if (ps->gpu_sampler_nearest) {
        SDL_ReleaseGPUSampler(ps->gpu_device, ps->gpu_sampler_nearest);
        ps->gpu_sampler_nearest = NULL;
    }
    if (ps->gpu_tex_noise) {
        SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_tex_noise);
        ps->gpu_tex_noise = NULL;
    }
    if (ps->gpu_tex_lut_lin) {
        SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_tex_lut_lin);
        ps->gpu_tex_lut_lin = NULL;
    }
    if (ps->gpu_tex_lut_pq) {
        SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_tex_lut_pq);
        ps->gpu_tex_lut_pq = NULL;
    }
    ps->pq_lut_active = 0;
    if (ps->gpu_pipeline_yuv) {
        SDL_ReleaseGPUGraphicsPipeline(ps->gpu_device, ps->gpu_pipeline_yuv);
        ps->gpu_pipeline_yuv = NULL;
    }
    if (ps->gpu_pipeline_yuv_dilated) {
        SDL_ReleaseGPUGraphicsPipeline(ps->gpu_device, ps->gpu_pipeline_yuv_dilated);
        ps->gpu_pipeline_yuv_dilated = NULL;
    }
    if (ps->gpu_pipeline_yuv_frame) {
        SDL_ReleaseGPUGraphicsPipeline(ps->gpu_device, ps->gpu_pipeline_yuv_frame);
        ps->gpu_pipeline_yuv_frame = NULL;
    }
    if (ps->gpu_pipeline_yuv_dilated_frame) {
        SDL_ReleaseGPUGraphicsPipeline(ps->gpu_device, ps->gpu_pipeline_yuv_dilated_frame);
        ps->gpu_pipeline_yuv_dilated_frame = NULL;
    }
    if (ps->gpu_pipeline_yuv_direct) {
        SDL_ReleaseGPUGraphicsPipeline(ps->gpu_device, ps->gpu_pipeline_yuv_direct);
        ps->gpu_pipeline_yuv_direct = NULL;
    }
    if (ps->gpu_pipeline_yuv_direct_frame) {
        SDL_ReleaseGPUGraphicsPipeline(ps->gpu_device, ps->gpu_pipeline_yuv_direct_frame);
        ps->gpu_pipeline_yuv_direct_frame = NULL;
    }
    if (ps->gpu_pipeline_yuv_scale2x) {
        SDL_ReleaseGPUGraphicsPipeline(ps->gpu_device, ps->gpu_pipeline_yuv_scale2x);
        ps->gpu_pipeline_yuv_scale2x = NULL;
    }
    if (ps->gpu_pipeline_yuv_scale2x_frame) {
        SDL_ReleaseGPUGraphicsPipeline(ps->gpu_device, ps->gpu_pipeline_yuv_scale2x_frame);
        ps->gpu_pipeline_yuv_scale2x_frame = NULL;
    }
    if (ps->gpu_pipeline_overlay) {
        SDL_ReleaseGPUGraphicsPipeline(ps->gpu_device, ps->gpu_pipeline_overlay);
        ps->gpu_pipeline_overlay = NULL;
    }
    if (ps->gpu_pipeline_blit) {
        SDL_ReleaseGPUGraphicsPipeline(ps->gpu_device, ps->gpu_pipeline_blit);
        ps->gpu_pipeline_blit = NULL;
    }
    if (ps->gpu_tex_frame) {
        SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_tex_frame);
        ps->gpu_tex_frame = NULL;
        ps->frame_tex_w = ps->frame_tex_h = 0;
        ps->frame_tex_valid = 0;
        ps->frame_render_dirty = 1;
    }
}

/* Ensure the intermediate frame texture matches the swapchain size.
 * Recreation invalidates contents — the caller re-renders before any
 * blit (same fresh-texture rule as the overlay). Returns 0 on
 * success, -1 if unavailable (caller falls back to direct render). */
static int gpu_frame_tex_ensure(PlayerState *ps, int w, int h) {
    if (!ps->gpu_pipeline_blit || w <= 0 || h <= 0) return -1;
    if (ps->gpu_tex_frame && ps->frame_tex_w == w && ps->frame_tex_h == h)
        return 0;

    if (ps->gpu_tex_frame) {
        SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_tex_frame);
        ps->gpu_tex_frame = NULL;
    }

    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type                  = SDL_GPU_TEXTURETYPE_2D;
    info.format                = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM;
    info.width                 = (Uint32)w;
    info.height                = (Uint32)h;
    info.layer_count_or_depth  = 1;
    info.num_levels            = 1;
    info.usage                 = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
                               | SDL_GPU_TEXTUREUSAGE_SAMPLER;

    ps->gpu_tex_frame = SDL_CreateGPUTexture(ps->gpu_device, &info);
    if (!ps->gpu_tex_frame) {
        /* Latch the fallback: retrying per frame would spam the log
         * and re-fail; the direct path is the known-good baseline. */
        log_msg("WARN: frame texture create failed (%s) — direct render "
                "path for this session", SDL_GetError());
        ps->frame_tex_w = ps->frame_tex_h = 0;
        ps->no_intermediate = 1;
        return -1;
    }
    ps->frame_tex_w = w;
    ps->frame_tex_h = h;
    ps->frame_tex_valid = 0;
    ps->frame_render_dirty = 1;
    log_msg("GPU: frame texture created (%dx%d UNORM16)", w, h);
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════
 * GPU Texture & Transfer Buffer Helpers (per-file lifetime)
 * ═══════════════════════════════════════════════════════════════════ */

/* Create GPU textures and transfer buffers for the current video.
 *
 * Two paths:
 *   8-bit (YUV420P):      3 × R8_UNORM planar textures (Y, U, V)
 *   10-bit (YUV420P10LE): 3 × R16_UNORM planar textures (Y, U, V)
 *
 * Both paths use the same YUV planar shader — Texture2D<float> reads
 * the .r channel from either format. The 10-bit path bypasses swscale
 * entirely; raw frame data goes straight to GPU. */
/* get_buffer2 pool — defined after gpu_destroy_video_textures, which
 * calls the destroy; declared here so the call site compiles. */
static void xfer_pool_destroy(PlayerState *ps);

static int gpu_create_video_textures(PlayerState *ps) {
    int w = ps->vid_w;
    int h = ps->vid_h;
    /* ceil — FFmpeg allocates ceil(w/2) chroma for odd dimensions;
     * truncating dropped the last chroma column/row and skewed the
     * chroma texture geometry by half a texel across the frame
     * (DSVP main fdbb489). */
    int cw = (w + 1) / 2;  /* chroma width  (4:2:0) */
    int ch = (h + 1) / 2;  /* chroma height (4:2:0) */

    /* Key on the UPLOAD format, not the codec format: deep sources on
     * the swscale path land in yuv420p10le and need R16 too. */
    int is_10bit = (ps->video_codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P10LE
                    && !ps->sws_ctx)
                   || (ps->vaapi_active && !ps->vaapi_nv12)
                   || (ps->sws_ctx && ps->sws_out_10bit);

    SDL_GPUTextureFormat fmt = is_10bit
        ? SDL_GPU_TEXTUREFORMAT_R16_UNORM
        : SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    int bpp = is_10bit ? 2 : 1;  /* bytes per sample */

    SDL_GPUTextureCreateInfo tex_info;
    SDL_zero(tex_info);
    tex_info.type                  = SDL_GPU_TEXTURETYPE_2D;
    tex_info.format                = fmt;
    tex_info.layer_count_or_depth  = 1;
    tex_info.num_levels            = 1;
    tex_info.usage                 = SDL_GPU_TEXTUREUSAGE_SAMPLER;

    /* Y plane (full resolution) */
    tex_info.width  = w;
    tex_info.height = h;
    ps->gpu_tex_y = SDL_CreateGPUTexture(ps->gpu_device, &tex_info);
    if (!ps->gpu_tex_y) {
        log_msg("ERROR: Failed to create Y texture: %s", SDL_GetError());
        return -1;
    }

    /* U plane (half resolution) */
    tex_info.width  = cw;
    tex_info.height = ch;
    ps->gpu_tex_u = SDL_CreateGPUTexture(ps->gpu_device, &tex_info);
    if (!ps->gpu_tex_u) {
        log_msg("ERROR: Failed to create U texture: %s", SDL_GetError());
        return -1;
    }

    /* V plane (half resolution) */
    ps->gpu_tex_v = SDL_CreateGPUTexture(ps->gpu_device, &tex_info);
    if (!ps->gpu_tex_v) {
        log_msg("ERROR: Failed to create V texture: %s", SDL_GetError());
        return -1;
    }

    /* UV interleaved texture for zero-copy (R16G16_UNORM, half res).
     * Only created when VAAPI zero-copy is active for P010 content.
     * The shader reads .r = U, .g = V from this single texture. */
    if (ps->vaapi_zerocopy && is_10bit) {
        SDL_GPUTextureCreateInfo uv_info;
        SDL_zero(uv_info);
        uv_info.type   = SDL_GPU_TEXTURETYPE_2D;
        uv_info.format = SDL_GPU_TEXTUREFORMAT_R16G16_UNORM;
        uv_info.width  = cw;
        uv_info.height = ch;
        uv_info.layer_count_or_depth = 1;
        uv_info.num_levels = 1;
        uv_info.usage  = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ps->gpu_tex_uv = SDL_CreateGPUTexture(ps->gpu_device, &uv_info);
        if (!ps->gpu_tex_uv) {
            log_msg("ZEROCOPY: failed to create UV texture — readback fallback");
            ps->vaapi_zerocopy = 0;
        } else {
            log_msg("GPU: zero-copy UV texture created (R16G16_UNORM %dx%d)", cw, ch);
        }
    }

    /* Transfer buffers (CPU→GPU staging) — three sets, see dsvp.h:
     * 0/1 decode-thread ping-pong, 2 main-thread fallback. */
    SDL_GPUTransferBufferCreateInfo xfer_info;
    SDL_zero(xfer_info);
    xfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;

    for (int s = 0; s < 3; s++) {
        xfer_info.size = (Uint32)w * h * bpp;
        ps->gpu_xfer_y[s] = SDL_CreateGPUTransferBuffer(ps->gpu_device, &xfer_info);

        xfer_info.size = (Uint32)cw * ch * bpp;
        ps->gpu_xfer_u[s] = SDL_CreateGPUTransferBuffer(ps->gpu_device, &xfer_info);
        ps->gpu_xfer_v[s] = SDL_CreateGPUTransferBuffer(ps->gpu_device, &xfer_info);

        if (!ps->gpu_xfer_y[s] || !ps->gpu_xfer_u[s] || !ps->gpu_xfer_v[s]) {
            log_msg("ERROR: Failed to create transfer buffers: %s", SDL_GetError());
            return -1;
        }
    }
    ps->xfer_fill          = 0;
    ps->decoded_frame_xfer = -1;
    ps->video_frame_xfer   = -1;

    log_msg("GPU: textures created (Y=%dx%d, UV=%dx%d, %s planar)",
            w, h, cw, ch,
            is_10bit ? "R16_UNORM 10-bit" : "R8_UNORM");
    return 0;
}

/* Destroy per-file GPU resources. */
static void gpu_destroy_video_textures(PlayerState *ps) {
    if (!ps->gpu_device) return;

    if (ps->gpu_tex_y)  { SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_tex_y);  ps->gpu_tex_y  = NULL; }
    if (ps->gpu_tex_u)  { SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_tex_u);  ps->gpu_tex_u  = NULL; }
    if (ps->gpu_tex_v)  { SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_tex_v);  ps->gpu_tex_v  = NULL; }
    if (ps->gpu_tex_uv) { SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_tex_uv); ps->gpu_tex_uv = NULL; }
    for (int s = 0; s < 3; s++) {
        if (ps->gpu_xfer_y[s]) { SDL_ReleaseGPUTransferBuffer(ps->gpu_device, ps->gpu_xfer_y[s]); ps->gpu_xfer_y[s] = NULL; }
        if (ps->gpu_xfer_u[s]) { SDL_ReleaseGPUTransferBuffer(ps->gpu_device, ps->gpu_xfer_u[s]); ps->gpu_xfer_u[s] = NULL; }
        if (ps->gpu_xfer_v[s]) { SDL_ReleaseGPUTransferBuffer(ps->gpu_device, ps->gpu_xfer_v[s]); ps->gpu_xfer_v[s] = NULL; }
    }
    ps->decoded_frame_xfer = -1;
    ps->video_frame_xfer   = -1;

    xfer_pool_destroy(ps);
}


/* ═══════════════════════════════════════════════════════════════════
 * get_buffer2 Zero-Copy Decode Pool (TODO-PACING open item 1)
 *
 * The decoder writes frames directly into persistently-mapped SDL
 * transfer buffers. See the XferSlot comment in dsvp.h for the
 * ownership/cooling model. Deck is Vulkan-only, where SDL transfer
 * buffers are persistently mapped (Unmap is a no-op), which is what
 * makes holding the mapping for the file's lifetime legal.
 * ═══════════════════════════════════════════════════════════════════ */

/* Free callback for one plane's AVBufferRef. Runs on whatever thread
 * drops the last frame ref (decoder workers, decode thread, main
 * thread at consume, demux thread at seek flush) — hence the mutex
 * and the atomic. When the last of the three planes goes, the slot
 * starts cooling; it may not be handed to the decoder again until
 * DSVP_XFER_POOL_COOL presents later, by which point the GPU has
 * provably executed the copy pass that read it (queue order + the
 * swapchain acquire throttle bound GPU lag to less than that). */
static void xfer_pool_plane_free(void *opaque, uint8_t *data)
{
    XferSlot *slot = (XferSlot *)opaque;
    (void)data;
    if (SDL_AtomicDecRef(&slot->plane_refs)) {
        PlayerState *psl = (PlayerState *)slot->ps;
        SDL_LockMutex(psl->xfer_pool_mutex);
        slot->state = XFER_SLOT_COOLING;
        /* Cross-thread read of a long the main thread increments —
         * staleness only delays reuse, which is the safe direction. */
        slot->cool_stamp = (int)psl->presents;
        SDL_UnlockMutex(psl->xfer_pool_mutex);
    }
}

/* AVCodecContext.get_buffer2 — called by FFmpeg (from frame-thread
 * workers too; must be thread-safe) whenever the decoder needs a
 * frame buffer. Hands out a pool slot when one fits; anything else
 * falls back to FFmpeg's own allocator, and such frames simply take
 * the existing prestage path. A pool miss is a slow frame, never a
 * wrong frame. */
static int xfer_pool_get_buffer2(AVCodecContext *avctx, AVFrame *frame,
                                 int flags)
{
    PlayerState *ps = (PlayerState *)avctx->opaque;

    if (!ps || ps->xfer_pool_n <= 0
            || frame->format != AV_PIX_FMT_YUV420P
            || frame->width  > ps->xfer_pool_pitch_y
            || frame->height > ps->xfer_pool_h)
        return avcodec_default_get_buffer2(avctx, frame, flags);

    XferSlot *slot = NULL;
    SDL_LockMutex(ps->xfer_pool_mutex);
    for (int i = 0; i < ps->xfer_pool_n; i++) {
        XferSlot *s = &ps->xfer_pool[i];
        if (s->state == XFER_SLOT_FREE ||
            (s->state == XFER_SLOT_COOLING &&
             (int)ps->presents - s->cool_stamp >= DSVP_XFER_POOL_COOL)) {
            s->state = XFER_SLOT_BUSY;
            SDL_SetAtomicInt(&s->plane_refs, 3);
            slot = s;
            break;
        }
    }
    SDL_UnlockMutex(ps->xfer_pool_mutex);

    if (!slot) {
        ps->xfer_pool_misses++;   /* benign racy counter, diag only */
        return avcodec_default_get_buffer2(avctx, frame, flags);
    }

    size_t sz_y = (size_t)ps->xfer_pool_pitch_y  * (ps->xfer_pool_h  + 1) + 64;
    size_t sz_c = (size_t)ps->xfer_pool_pitch_uv * (ps->xfer_pool_ch + 1) + 64;

    frame->buf[0] = av_buffer_create(slot->my, sz_y,
                                     xfer_pool_plane_free, slot, 0);
    frame->buf[1] = av_buffer_create(slot->mu, sz_c,
                                     xfer_pool_plane_free, slot, 0);
    frame->buf[2] = av_buffer_create(slot->mv, sz_c,
                                     xfer_pool_plane_free, slot, 0);
    if (!frame->buf[0] || !frame->buf[1] || !frame->buf[2]) {
        /* Unref what exists (each unref fires the plane free cb);
         * decrement manually for the ones never created so the slot
         * still reaches zero and cools. */
        for (int p = 0; p < 3; p++) {
            if (frame->buf[p]) av_buffer_unref(&frame->buf[p]);
            else               xfer_pool_plane_free(slot, NULL);
        }
        return AVERROR(ENOMEM);
    }

    frame->data[0] = slot->my;
    frame->data[1] = slot->mu;
    frame->data[2] = slot->mv;
    frame->linesize[0] = ps->xfer_pool_pitch_y;
    frame->linesize[1] = ps->xfer_pool_pitch_uv;
    frame->linesize[2] = ps->xfer_pool_pitch_uv;
    frame->extended_data = frame->data;
    return 0;
}

/* Build the pool for the current file, or leave it off (pool_n = 0)
 * when the file's decode path can't use it — every consumer then
 * takes the prestage/upload fallbacks unchanged. Called from
 * player_open after the codec is open and the textures exist,
 * before the decode thread starts. */
static void xfer_pool_create(PlayerState *ps)
{
    ps->xfer_pool_n        = 0;
    ps->xfer_pool_misses   = 0;
    ps->xfer_pool_served   = 0;
    ps->decoded_frame_slot = -1;
    ps->video_frame_slot   = -1;

    if (ps->no_pool || ps->vaapi_active || ps->sws_ctx) return;
    if (ps->video_codec_ctx->pix_fmt != AV_PIX_FMT_YUV420P) return;
    /* Direct rendering must be supported for get_buffer2 frames. */
    if (!ps->video_codec_ctx->codec ||
        !(ps->video_codec_ctx->codec->capabilities & AV_CODEC_CAP_DR1))
        return;

    /* Size planes the way FFmpeg's own allocator would: aligned
     * dimensions from the codec, 64-byte row pitch (covers every
     * SIMD alignment FFmpeg requests), one spare row + 64 bytes of
     * tail slack per plane for decoder over-read. */
    int aw = ps->video_codec_ctx->width;
    int ah = ps->video_codec_ctx->height;
    {
        int ls_align[AV_NUM_DATA_POINTERS];
        avcodec_align_dimensions2(ps->video_codec_ctx, &aw, &ah, ls_align);
    }
    ps->xfer_pool_pitch_y  = (aw + 63) & ~63;
    ps->xfer_pool_pitch_uv = (((aw + 1) / 2) + 63) & ~63;
    ps->xfer_pool_h        = ah;
    ps->xfer_pool_ch       = (ah + 1) / 2;

    size_t sz_y = (size_t)ps->xfer_pool_pitch_y  * (ps->xfer_pool_h  + 1) + 64;
    size_t sz_c = (size_t)ps->xfer_pool_pitch_uv * (ps->xfer_pool_ch + 1) + 64;

    SDL_GPUTransferBufferCreateInfo ci;
    SDL_zero(ci);
    ci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;

    int made = 0;
    for (int i = 0; i < DSVP_XFER_POOL_SLOTS; i++) {
        XferSlot *s = &ps->xfer_pool[i];
        SDL_zerop(s);
        s->ps = ps;
        ci.size = (Uint32)sz_y;
        s->xy = SDL_CreateGPUTransferBuffer(ps->gpu_device, &ci);
        ci.size = (Uint32)sz_c;
        s->xu = SDL_CreateGPUTransferBuffer(ps->gpu_device, &ci);
        s->xv = SDL_CreateGPUTransferBuffer(ps->gpu_device, &ci);
        if (!s->xy || !s->xu || !s->xv) break;
        /* Map once, keep forever (cycle=false: fresh buffer, and we
         * must never rotate the backing under FFmpeg's pointers). */
        s->my = SDL_MapGPUTransferBuffer(ps->gpu_device, s->xy, false);
        s->mu = SDL_MapGPUTransferBuffer(ps->gpu_device, s->xu, false);
        s->mv = SDL_MapGPUTransferBuffer(ps->gpu_device, s->xv, false);
        if (!s->my || !s->mu || !s->mv) break;
        s->state = XFER_SLOT_FREE;
        made = i + 1;
    }

    if (made < DSVP_XFER_POOL_SLOTS) {
        /* Partial pool = none: releasing keeps the accounting simple
         * and the fallback path is fully functional. */
        for (int i = 0; i < DSVP_XFER_POOL_SLOTS; i++) {
            XferSlot *s = &ps->xfer_pool[i];
            if (s->xy) SDL_ReleaseGPUTransferBuffer(ps->gpu_device, s->xy);
            if (s->xu) SDL_ReleaseGPUTransferBuffer(ps->gpu_device, s->xu);
            if (s->xv) SDL_ReleaseGPUTransferBuffer(ps->gpu_device, s->xv);
            SDL_zerop(s);
        }
        log_msg("WARN: zero-copy decode pool allocation failed at slot %d "
                "— falling back to staged uploads", made);
        return;
    }

    ps->xfer_pool_mutex = SDL_CreateMutex();
    if (!ps->xfer_pool_mutex) {
        for (int i = 0; i < DSVP_XFER_POOL_SLOTS; i++) {
            XferSlot *s = &ps->xfer_pool[i];
            SDL_ReleaseGPUTransferBuffer(ps->gpu_device, s->xy);
            SDL_ReleaseGPUTransferBuffer(ps->gpu_device, s->xu);
            SDL_ReleaseGPUTransferBuffer(ps->gpu_device, s->xv);
            SDL_zerop(s);
        }
        return;
    }

    ps->xfer_pool_n = DSVP_XFER_POOL_SLOTS;
    ps->video_codec_ctx->opaque      = ps;
    ps->video_codec_ctx->get_buffer2 = xfer_pool_get_buffer2;
    log_msg("GPU: zero-copy decode pool — %d slots, Y pitch %d, "
            "%.0f MB (decoder writes straight into transfer buffers; "
            "DSVP_NO_POOL=1 opts out)",
            ps->xfer_pool_n, ps->xfer_pool_pitch_y,
            (double)ps->xfer_pool_n * (sz_y + 2 * sz_c) / (1024.0 * 1024.0));
}

/* Called from gpu_destroy_video_textures, which player_close reaches
 * only AFTER the decode/demux threads are joined, video_frame /
 * decoded_frame are freed, and avcodec_free_context has flushed the
 * decoder — i.e. every plane ref is gone and no callback can fire
 * concurrently with this teardown. */
static void xfer_pool_destroy(PlayerState *ps)
{
    if (ps->xfer_pool_n <= 0) return;
    int busy = 0;
    for (int i = 0; i < ps->xfer_pool_n; i++) {
        XferSlot *s = &ps->xfer_pool[i];
        if (s->state == XFER_SLOT_BUSY) busy++;
        if (s->xy) { SDL_UnmapGPUTransferBuffer(ps->gpu_device, s->xy);
                     SDL_ReleaseGPUTransferBuffer(ps->gpu_device, s->xy); }
        if (s->xu) { SDL_UnmapGPUTransferBuffer(ps->gpu_device, s->xu);
                     SDL_ReleaseGPUTransferBuffer(ps->gpu_device, s->xu); }
        if (s->xv) { SDL_UnmapGPUTransferBuffer(ps->gpu_device, s->xv);
                     SDL_ReleaseGPUTransferBuffer(ps->gpu_device, s->xv); }
        SDL_zerop(s);
    }
    if (busy)
        log_msg("WARN: zero-copy pool destroyed with %d slot(s) still "
                "busy — a frame ref outlived the codec (should not "
                "happen; see teardown ordering in player_close)", busy);
    if (ps->xfer_pool_served || ps->xfer_pool_misses)
        log_msg("Pool: %d frame(s) served zero-copy%s",
                ps->xfer_pool_served,
                ps->xfer_pool_misses ? "" : ", no fallbacks");
    if (ps->xfer_pool_misses)
        log_msg("Pool: %d frame(s) fell back to FFmpeg's allocator "
                "(pool exhausted — staged-upload path served them)",
                ps->xfer_pool_misses);
    if (ps->xfer_pool_mutex) {
        SDL_DestroyMutex(ps->xfer_pool_mutex);
        ps->xfer_pool_mutex = NULL;
    }
    ps->xfer_pool_n = 0;
    ps->decoded_frame_slot = -1;
    ps->video_frame_slot   = -1;
}


/* ═══════════════════════════════════════════════════════════════════
 * GPU Uniform Setup
 * ═══════════════════════════════════════════════════════════════════
 *
 * Sets the YUV→RGB color matrix and range parameters based on the
 * video's colorspace metadata. Called once per file in player_open().
 *
 * Three modes:
 *   10-bit passthrough (yuv420p10le): range expansion in shader (R16_UNORM)
 *   8-bit passthrough  (yuv420p):     range expansion in shader (R8_UNORM)
 *   swscale fallback:                 swscale does range → identity uniforms
 */

static void gpu_setup_uniforms(PlayerState *ps) {
    /* Determine YCbCr matrix from metadata or resolution heuristic.
     * Three standards: BT.601 (SD), BT.709 (HD), BT.2020 NCL (UHD/HDR).
     * color_space tag is authoritative; resolution heuristic is fallback. */
    int colorspace = (ps->vid_h >= 720) ? 709 : 601;
    if (ps->fmt_ctx) {
        AVCodecParameters *par =
            ps->fmt_ctx->streams[ps->video_stream_idx]->codecpar;
        if (par->color_space == AVCOL_SPC_BT709)
            colorspace = 709;
        else if (par->color_space == AVCOL_SPC_BT470BG ||
                 par->color_space == AVCOL_SPC_SMPTE170M)
            colorspace = 601;
        else if (par->color_space == AVCOL_SPC_BT2020_NCL)
            colorspace = 2020;
    }

    const char *cs_name = (colorspace == 2020) ? "BT.2020"
                        : (colorspace == 709)  ? "BT.709" : "BT.601";

    /* ── Range parameters ──
     *
     * Three passthrough modes, all handling range expansion in shader:
     *
     * 10-bit passthrough (yuv420p10le → R16_UNORM):
     *   GPU reads uint16 V as V/65535.
     *   Limited: Y 64-940, UV 64-960
     *   Full:    Y/UV 0-1023
     *
     * 8-bit passthrough (yuv420p → R8_UNORM):
     *   GPU reads uint8 V as V/255.
     *   Limited: Y 16-235, UV 16-240
     *   Full:    identity {0, 1}
     *
     * swscale fallback (other formats → yuv420p full-range):
     *   swscale outputs full-range → identity {0, 1}.
     */
    int is_10bit_passthrough =
        (ps->video_codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P10LE
         && !ps->sws_ctx);
    int is_8bit_passthrough =
        (ps->video_codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P
         && !ps->sws_ctx);

    /* Read color range from metadata */
    int is_full_range = 0;
    if (ps->fmt_ctx) {
        AVCodecParameters *par =
            ps->fmt_ctx->streams[ps->video_stream_idx]->codecpar;
        is_full_range = (par->color_range == AVCOL_RANGE_JPEG);
    }

    if (ps->vaapi_active && !ps->vaapi_nv12) {
        /* ── P010 from VAAPI — 10-bit values left-shifted by 6 in uint16 ──
         *
         * P010 stores 10-bit code V as (V << 6) in a uint16.
         * R16_UNORM reads uint16 as value/65535.
         *
         * Limited range:
         *   Y  codes 64-940   → stored as 4096-60160  → R16 = 4096/65535 to 60160/65535
         *   UV codes 64-960   → stored as 4096-61440  → R16 = 4096/65535 to 61440/65535
         *
         * Full range:
         *   codes 0-1023 → stored as 0-65472 → R16 = 0 to 65472/65535
         */
        if (is_full_range) {
            ps->gpu_uniforms.rangeY[0]  = 0.0f;
            ps->gpu_uniforms.rangeY[1]  = 65535.0f / 65472.0f;
            /* Half-LSB chroma-neutral offset, same correction as every
             * other full-range branch below: neutral code 512 is stored
             * as 32768, and without the offset the shader lands at
             * +32/65472 above true neutral — a constant color cast
             * (review Q-4). 32 = half of one 10-bit LSB in the <<6
             * P010 representation. */
            ps->gpu_uniforms.rangeUV[0] = 32.0f / 65535.0f;
            ps->gpu_uniforms.rangeUV[1] = 65535.0f / 65472.0f;
        } else {
            ps->gpu_uniforms.rangeY[0]  = 4096.0f / 65535.0f;
            ps->gpu_uniforms.rangeY[1]  = 65535.0f / (60160.0f - 4096.0f);
            ps->gpu_uniforms.rangeUV[0] = 4096.0f / 65535.0f;
            ps->gpu_uniforms.rangeUV[1] = 65535.0f / (61440.0f - 4096.0f);
        }

        log_msg("GPU: uniforms set (%s, P010 %s range → shader)",
                cs_name,
                is_full_range ? "full" : "limited");

    } else if (ps->vaapi_active && ps->vaapi_nv12) {
        /* ── NV12 from VAAPI — 8-bit uint8 samples ──
         *
         * R8_UNORM reads uint8 V as V/255.
         * Same range math as yuv420p passthrough.
         */
        if (is_full_range) {
            ps->gpu_uniforms.rangeY[0]  = 0.0f;
            ps->gpu_uniforms.rangeY[1]  = 1.0f;
            /* Full-range chroma neutral is (2^n)/2, i.e. code 128 of 0..255,
             * but the shader computes code/(2^n - 1) - 0.5, which puts the
             * neutral half a code low (H.273: E_Cb = (code - 2^(n-1))/(2^n - 1)).
             * Offsetting by half an LSB removes a constant colour cast on
             * full-range (JPEG-range) content. Limited range is already exact. */
            ps->gpu_uniforms.rangeUV[0] = 0.5f / 255.0f;
            ps->gpu_uniforms.rangeUV[1] = 1.0f;
        } else {
            ps->gpu_uniforms.rangeY[0]  = 16.0f / 255.0f;
            ps->gpu_uniforms.rangeY[1]  = 255.0f / (235.0f - 16.0f);
            ps->gpu_uniforms.rangeUV[0] = 16.0f / 255.0f;
            ps->gpu_uniforms.rangeUV[1] = 255.0f / (240.0f - 16.0f);
        }

        log_msg("GPU: uniforms set (%s, NV12 %s range → shader)",
                cs_name,
                is_full_range ? "full" : "limited");

    } else if (is_10bit_passthrough) {
        /* 10-bit passthrough — range correction in shader */
        if (is_full_range) {
            ps->gpu_uniforms.rangeY[0]  = 0.0f;
            ps->gpu_uniforms.rangeY[1]  = 65535.0f / 1023.0f;
            /* half-LSB full-range chroma neutral, see note above */
            ps->gpu_uniforms.rangeUV[0] = 0.5f / 65535.0f;
            ps->gpu_uniforms.rangeUV[1] = 65535.0f / 1023.0f;
        } else {
            ps->gpu_uniforms.rangeY[0]  = 64.0f / 65535.0f;
            ps->gpu_uniforms.rangeY[1]  = 65535.0f / (940.0f - 64.0f);
            ps->gpu_uniforms.rangeUV[0] = 64.0f / 65535.0f;
            ps->gpu_uniforms.rangeUV[1] = 65535.0f / (960.0f - 64.0f);
        }

        log_msg("GPU: uniforms set (%s, 10-bit %s range → shader)",
                cs_name, is_full_range ? "full" : "limited");

    } else if (is_8bit_passthrough) {
        /* 8-bit YUV420P passthrough — range correction in shader.
         * R8_UNORM reads uint8 V as V/255. */
        if (is_full_range) {
            ps->gpu_uniforms.rangeY[0]  = 0.0f;
            ps->gpu_uniforms.rangeY[1]  = 1.0f;
            /* Full-range chroma neutral is (2^n)/2, i.e. code 128 of 0..255,
             * but the shader computes code/(2^n - 1) - 0.5, which puts the
             * neutral half a code low (H.273: E_Cb = (code - 2^(n-1))/(2^n - 1)).
             * Offsetting by half an LSB removes a constant colour cast on
             * full-range (JPEG-range) content. Limited range is already exact. */
            ps->gpu_uniforms.rangeUV[0] = 0.5f / 255.0f;
            ps->gpu_uniforms.rangeUV[1] = 1.0f;
        } else {
            ps->gpu_uniforms.rangeY[0]  = 16.0f / 255.0f;
            ps->gpu_uniforms.rangeY[1]  = 255.0f / (235.0f - 16.0f);
            ps->gpu_uniforms.rangeUV[0] = 16.0f / 255.0f;
            ps->gpu_uniforms.rangeUV[1] = 255.0f / (240.0f - 16.0f);
        }

        log_msg("GPU: uniforms set (%s, 8-bit %s range → shader)",
                cs_name, is_full_range ? "full" : "limited");

    } else if (ps->sws_ctx && ps->sws_out_10bit) {
        /* swscale fallback, 10-bit destination — full-range yuv420p10le
         * through the R16 path. Same math as 10-bit full-range
         * passthrough: codes 0..1023 in 16-bit words, half-LSB chroma
         * neutral (see note above). */
        ps->gpu_uniforms.rangeY[0]  = 0.0f;
        ps->gpu_uniforms.rangeY[1]  = 65535.0f / 1023.0f;
        ps->gpu_uniforms.rangeUV[0] = 0.5f / 65535.0f;
        ps->gpu_uniforms.rangeUV[1] = 65535.0f / 1023.0f;

        log_msg("GPU: uniforms set (%s, full range 10-bit via swscale)",
                cs_name);

    } else {
        /* swscale fallback, 8-bit destination — since 2026-08-20 sws
         * PRESERVES the source range (dst_range = src_range, Knot
         * audit finding 9) and the shader expands, exactly like the
         * 8-bit passthrough branch above. Full-range sources keep the
         * identity-with-half-LSB-neutral math. */
        if (is_full_range) {
            ps->gpu_uniforms.rangeY[0]  = 0.0f;
            ps->gpu_uniforms.rangeY[1]  = 1.0f;
            ps->gpu_uniforms.rangeUV[0] = 0.5f / 255.0f;
            ps->gpu_uniforms.rangeUV[1] = 1.0f;
        } else {
            ps->gpu_uniforms.rangeY[0]  = 16.0f / 255.0f;
            ps->gpu_uniforms.rangeY[1]  = 255.0f / (235.0f - 16.0f);
            ps->gpu_uniforms.rangeUV[0] = 16.0f / 255.0f;
            ps->gpu_uniforms.rangeUV[1] = 255.0f / (240.0f - 16.0f);
        }

        log_msg("GPU: uniforms set (%s, 8-bit %s range via swscale → shader)",
                cs_name, is_full_range ? "full" : "limited");
    }

    /* Color matrix: row-major (matches HLSL row_major qualifier).
     *
     * Standard YUV→RGB for full-range input where Cb,Cr are centered:
     *   R = Y + 0     * (Cb-0.5) + Cr_coeff * (Cr-0.5)
     *   G = Y + Cb_g  * (Cb-0.5) + Cr_g    * (Cr-0.5)
     *   B = Y + Cb_b  * (Cb-0.5) + 0       * (Cr-0.5)
     */
    float *m = ps->gpu_uniforms.colorMatrix;
    memset(m, 0, 16 * sizeof(float));

    if (colorspace == 2020) {
        /* BT.2020 NCL: Kr=0.2627, Kb=0.0593 */
        m[ 0] = 1.0f;  m[ 1] =  0.0f;     m[ 2] =  1.4746f;  /* R */
        m[ 4] = 1.0f;  m[ 5] = -0.1646f;  m[ 6] = -0.5714f;  /* G */
        m[ 8] = 1.0f;  m[ 9] =  1.8814f;  m[10] =  0.0f;     /* B */
    } else if (colorspace == 709) {
        /* BT.709: Kr=0.2126, Kb=0.0722 */
        m[ 0] = 1.0f;  m[ 1] =  0.0f;     m[ 2] =  1.5748f;  /* R */
        m[ 4] = 1.0f;  m[ 5] = -0.1873f;  m[ 6] = -0.4681f;  /* G */
        m[ 8] = 1.0f;  m[ 9] =  1.8556f;  m[10] =  0.0f;     /* B */
    } else {
        /* BT.601: Kr=0.299, Kb=0.114 */
        m[ 0] = 1.0f;  m[ 1] =  0.0f;     m[ 2] =  1.402f;   /* R */
        m[ 4] = 1.0f;  m[ 5] = -0.3441f;  m[ 6] = -0.7141f;  /* G */
        m[ 8] = 1.0f;  m[ 9] =  1.772f;   m[10] =  0.0f;     /* B */
    }
    m[15] = 1.0f;  /* A passthrough */

    /* ── Texture dimensions for Lanczos resampling ──
     *
     * The fragment shader needs texel size to compute sample positions
     * for the Lanczos-2 4×4 kernel. Y plane is full resolution;
     * UV planes are half (4:2:0 chroma subsampling). */
    ps->gpu_uniforms.texSizeY[0]  = (float)ps->vid_w;
    ps->gpu_uniforms.texSizeY[1]  = (float)ps->vid_h;
    ps->gpu_uniforms.texSizeUV[0] = (float)((ps->vid_w + 1) / 2);
    ps->gpu_uniforms.texSizeUV[1] = (float)((ps->vid_h + 1) / 2);

    /* ── Chroma siting correction ──
     *
     * 4:2:0 chroma samples may be co-sited with luma at different sub-texel
     * positions depending on the codec. The Catmull-Rom kernel assumes samples
     * are at texel centers (CENTER siting). For other sitings, we offset the
     * chroma UV coordinate so the kernel reconstructs at the correct position.
     *
     * Math: in 4:2:0, each chroma texel spans 2 luma pixels. CENTER places the
     * sample at the midpoint of this span (texel center — no correction).
     * LEFT co-sites with the left luma column, which is 0.5 luma pixels = 0.25
     * chroma texels away from center. Shader applies: uv + offset / texSizeUV.
     */
    enum AVChromaLocation chroma_loc = AVCHROMA_LOC_LEFT; /* safe default */
    if (ps->sws_ctx) {
        /* swscale path: the OUTPUT siting was pinned explicitly via
         * dst_chr_pos at context creation (source siting for 4:2:0
         * inputs — same-geometry conversions don't move chroma — LEFT
         * for genuinely resampled 422/444/RGB inputs). Reconstruct at
         * that siting. Replaces the old blanket zero-offset, whose
         * "sws re-sites to center" premise is false for unscaled depth
         * conversions (DSVP main df16dc8). */
        chroma_loc = (enum AVChromaLocation)ps->sws_dst_siting;
    } else if (ps->fmt_ctx) {
        AVCodecParameters *par =
            ps->fmt_ctx->streams[ps->video_stream_idx]->codecpar;
        if (par->chroma_location != AVCHROMA_LOC_UNSPECIFIED)
            chroma_loc = par->chroma_location;
        else if (par->color_primaries == AVCOL_PRI_BT2020)
            /* BT.2020 sites 4:2:0 chroma TOP-LEFT by spec; the LEFT
             * default is the BT.709-era convention. Re-encodes often
             * strip the VUI siting flag, so unspecified BT.2020 gets
             * the spec default — a quarter-texel VERTICAL correction
             * that SDR files never see. */
            chroma_loc = AVCHROMA_LOC_TOPLEFT;
    }
    ps->chroma_location = (int)chroma_loc;

    /* SIGN NOTE (fixed 2026-07-31): these were all negated, which moved
     * chroma a full luma pixel the WRONG way instead of correcting a half
     * pixel — and since LEFT is both the default and the siting of nearly
     * all H.264/HEVC content, it was active on essentially every file.
     *
     * Derivation for LEFT at 1:1 (W=4 luma, CW=2 chroma): chroma sample 0
     * is co-sited with luma column 0. Output pixel 0 samples at uv=0.125,
     * so in chroma space pos = 0.125*2 - 0.5 = -0.25 — the kernel lands a
     * quarter texel left of sample 0. To center it on the sample the
     * lookup must move +0.25 texels, not -0.25. The -0.25 figure is the
     * swscale FILTER-PHASE convention, which is the negative of a
     * coordinate offset; the shader applies it as a coordinate.
     * CENTER=0 is self-consistent either way, which is why this survived.
     *
     * Vertical: LEFT/CENTER are vertically centered (0). The vertical
     * signs below follow the same derivation in the vertex shader's
     * top-left-origin uv space, but only apply to the rare TOP and BOTTOM
     * sitings - worth a synthetic-pattern check if such a file turns up. */
    switch (chroma_loc) {
        case AVCHROMA_LOC_CENTER:
            ps->gpu_uniforms.chromaOffset[0] =  0.0f;
            ps->gpu_uniforms.chromaOffset[1] =  0.0f;
            break;
        case AVCHROMA_LOC_TOPLEFT:
            ps->gpu_uniforms.chromaOffset[0] =  0.25f;
            ps->gpu_uniforms.chromaOffset[1] =  0.25f;
            break;
        case AVCHROMA_LOC_TOP:
            ps->gpu_uniforms.chromaOffset[0] =  0.0f;
            ps->gpu_uniforms.chromaOffset[1] =  0.25f;
            break;
        case AVCHROMA_LOC_BOTTOMLEFT:
            ps->gpu_uniforms.chromaOffset[0] =  0.25f;
            ps->gpu_uniforms.chromaOffset[1] = -0.25f;
            break;
        case AVCHROMA_LOC_BOTTOM:
            ps->gpu_uniforms.chromaOffset[0] =  0.0f;
            ps->gpu_uniforms.chromaOffset[1] = -0.25f;
            break;
        default: /* LEFT and fallback */
            ps->gpu_uniforms.chromaOffset[0] =  0.25f;
            ps->gpu_uniforms.chromaOffset[1] =  0.0f;
            break;
    }

    /* (No sws zero-out here anymore: the shader offset above is derived
     * from the explicitly pinned sws OUTPUT siting when sws is active —
     * see the chroma_loc selection.) */

    ps->gpu_uniforms.frameCount = 0.0f;

    /* ── HDR Detection & Metadata ──
     *
     * HDR detection priority (per industry consensus — mpv, MPC, VLC):
     *   1. color_trc == SMPTE2084 (PQ) — catches all HDR10 content
     *   2. DOVI_CONF in coded_side_data — catches DV P5 where color_trc
     *      is often UNSPECIFIED
     *   3. color_trc == ARIB_STD_B67 (HLG) — inverse OETF + BT.2100
     *      OOTF in the shader, then the shared BT.2390 tone map
     *
     * Primaries classification (separate from HDR detection):
     *   - color_primaries == BT2020 → true BT.2020, needs gamut mapping
     *   - DV P5 with UNSPECIFIED primaries → base layer is BT.709 PQ
     *     (RPU would transform to BT.2020, but we don't process RPU)
     *
     * Peak luminance priority:
     *   1. MaxCLL from content light level metadata
     *   2. max_luminance from mastering display metadata
     *   3. 1000 nit fallback (standard for most HDR10 content)
     */
    int is_hdr = 0;
    int is_hlg = 0;
    int is_dolby_vision = 0;
    int has_pq_transfer = 0;
    float peak_nits = 0.0f;
    int has_bt2020_primaries = 0;

    if (ps->fmt_ctx) {
        AVCodecParameters *par =
            ps->fmt_ctx->streams[ps->video_stream_idx]->codecpar;

        /* --- Transfer function check --- */
        if (par->color_trc == AVCOL_TRC_SMPTE2084) {
            is_hdr = 1;
            has_pq_transfer = 1;
            log_msg("HDR: detected PQ transfer (SMPTE ST 2084)");
        } else if (par->color_trc == AVCOL_TRC_ARIB_STD_B67) {
            /* HLG: the shader converts inverse-OETF + BT.2100 OOTF
             * (Lw=1000) to display light, then the shared BT.2390 path
             * tone-maps. Previously detected-but-rendered-as-SDR:
             * washed out and desaturated on every HLG file
             * (DSVP main fdbb489). */
            is_hdr = 1;
            is_hlg = 1;
            log_msg("HDR: detected HLG transfer (ARIB STD-B67)");
        }

        /* --- Dolby Vision fallback (DV P5 often has UNSPECIFIED trc) --- */
        int dv_profile = -1;
        const AVPacketSideData *dovi_sd = av_packet_side_data_get(
            par->coded_side_data, par->nb_coded_side_data,
            AV_PKT_DATA_DOVI_CONF);
        if (dovi_sd) {
            const AVDOVIDecoderConfigurationRecord *cfg =
                (const AVDOVIDecoderConfigurationRecord *)dovi_sd->data;
            dv_profile = cfg->dv_profile;
            is_dolby_vision = 1;
            if (!is_hdr) {
                is_hdr = 1;
                log_msg("HDR: detected Dolby Vision Profile %d (DOVI conf in stream)",
                        dv_profile);
            } else {
                log_msg("HDR: Dolby Vision Profile %d metadata also present",
                        dv_profile);
            }
        }

        /* --- Primaries classification --- */
        if (par->color_primaries == AVCOL_PRI_BT2020) {
            has_bt2020_primaries = 1;
        }

        /* --- Static metadata: peak luminance --- */
        const AVPacketSideData *cll_sd = av_packet_side_data_get(
            par->coded_side_data, par->nb_coded_side_data,
            AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
        if (cll_sd && cll_sd->size >= (int)sizeof(AVContentLightMetadata)) {
            const AVContentLightMetadata *cll =
                (const AVContentLightMetadata *)cll_sd->data;
            if (cll->MaxCLL > 0) {
                peak_nits = (float)cll->MaxCLL;
                log_msg("HDR: MaxCLL=%u nits, MaxFALL=%u nits",
                        cll->MaxCLL, cll->MaxFALL);
            }
        }

        if (peak_nits == 0.0f) {
            const AVPacketSideData *mdm_sd = av_packet_side_data_get(
                par->coded_side_data, par->nb_coded_side_data,
                AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
            if (mdm_sd && mdm_sd->size >= (int)sizeof(AVMasteringDisplayMetadata)) {
                const AVMasteringDisplayMetadata *mdm =
                    (const AVMasteringDisplayMetadata *)mdm_sd->data;
                if (mdm->has_luminance) {
                    double max_lum = av_q2d(mdm->max_luminance);
                    if (max_lum > 0.0) {
                        peak_nits = (float)max_lum;
                        log_msg("HDR: mastering display max=%.0f nits, min=%.4f nits",
                                max_lum, av_q2d(mdm->min_luminance));
                    }
                }
                if (mdm->has_primaries) {
                    log_msg("HDR: mastering primaries: "
                            "R(%.4f,%.4f) G(%.4f,%.4f) B(%.4f,%.4f) WP(%.4f,%.4f)",
                            av_q2d(mdm->display_primaries[0][0]),
                            av_q2d(mdm->display_primaries[0][1]),
                            av_q2d(mdm->display_primaries[1][0]),
                            av_q2d(mdm->display_primaries[1][1]),
                            av_q2d(mdm->display_primaries[2][0]),
                            av_q2d(mdm->display_primaries[2][1]),
                            av_q2d(mdm->white_point[0]),
                            av_q2d(mdm->white_point[1]));
                }
            }
        }

        /* ── HDR10 static metadata → SDL window properties ──
         * Consumed by the local SDL patch (tools/sdl-patches/
         * sdl-3.4.14-hdr-metadata.patch): on the next HDR10 swapchain
         * (re)creation SDL calls vkSetHdrMetadataEXT and Mesa's WSI
         * hands the compositor a fully populated parametric image
         * description — KWin stops tone-mapping our surface blind.
         * Stock (unpatched) SDL ignores the properties entirely.
         * Forward only when BOTH primaries and luminance are present
         * (a zeroed VkHdrMetadataEXT is worse than none); CLL rides
         * along when available (0 = unknown, per CTA-861). Properties
         * are re-staged every open, with max_luminance=0 as the
         * "nothing to forward" state, so SDR files and metadata-less
         * HDR never inherit stale values. DSVP_NO_HDR_META=1 disables
         * for A/B. */
        {
            SDL_PropertiesID wp = SDL_GetWindowProperties(ps->window);
            float fw_maxlum = 0.0f;
            const AVPacketSideData *fw_mdm = av_packet_side_data_get(
                par->coded_side_data, par->nb_coded_side_data,
                AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
            if (is_hdr && !SDL_getenv("DSVP_NO_HDR_META")
                    && fw_mdm
                    && fw_mdm->size >= (int)sizeof(AVMasteringDisplayMetadata)) {
                const AVMasteringDisplayMetadata *m =
                    (const AVMasteringDisplayMetadata *)fw_mdm->data;
                if (m->has_primaries && m->has_luminance
                        && av_q2d(m->max_luminance) > 0.0) {
                    fw_maxlum = (float)av_q2d(m->max_luminance);
                    /* AVMasteringDisplayMetadata primaries order: R,G,B */
                    SDL_SetFloatProperty(wp, "dsvp.hdr.red_x",
                        (float)av_q2d(m->display_primaries[0][0]));
                    SDL_SetFloatProperty(wp, "dsvp.hdr.red_y",
                        (float)av_q2d(m->display_primaries[0][1]));
                    SDL_SetFloatProperty(wp, "dsvp.hdr.green_x",
                        (float)av_q2d(m->display_primaries[1][0]));
                    SDL_SetFloatProperty(wp, "dsvp.hdr.green_y",
                        (float)av_q2d(m->display_primaries[1][1]));
                    SDL_SetFloatProperty(wp, "dsvp.hdr.blue_x",
                        (float)av_q2d(m->display_primaries[2][0]));
                    SDL_SetFloatProperty(wp, "dsvp.hdr.blue_y",
                        (float)av_q2d(m->display_primaries[2][1]));
                    SDL_SetFloatProperty(wp, "dsvp.hdr.white_x",
                        (float)av_q2d(m->white_point[0]));
                    SDL_SetFloatProperty(wp, "dsvp.hdr.white_y",
                        (float)av_q2d(m->white_point[1]));
                    SDL_SetFloatProperty(wp, "dsvp.hdr.min_luminance",
                        (float)av_q2d(m->min_luminance));
                    float fw_cll = 0.0f, fw_fall = 0.0f;
                    const AVPacketSideData *fw_cll_sd =
                        av_packet_side_data_get(
                            par->coded_side_data, par->nb_coded_side_data,
                            AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
                    if (fw_cll_sd && fw_cll_sd->size >=
                            (int)sizeof(AVContentLightMetadata)) {
                        const AVContentLightMetadata *c =
                            (const AVContentLightMetadata *)fw_cll_sd->data;
                        fw_cll  = (float)c->MaxCLL;
                        fw_fall = (float)c->MaxFALL;
                    }
                    /* Sanitize to the protocol-legal envelope. Disc
                     * metadata is routinely self-inconsistent (field
                     * case: a famous 4000-nit-mastered title declaring
                     * MaxCLL 9918 — authoring-tool artifact), and
                     * Mesa's Wayland WSI validates hard
                     * (is_hdr_metadata_legal: max_cll ≤ max_luminance,
                     * max_fall ≤ max_cll ≤ ...) and silently DROPS all
                     * metadata on any violation ("MESA: warning: Not
                     * using HDR metadata to avoid protocol errors").
                     * Trust the mastering block (measured at the
                     * facility) over CLL (computed by tools). */
                    if (fw_cll > fw_maxlum) {
                        log_msg("HDR: MaxCLL %.0f exceeds mastering "
                                "peak %.0f — clamped (disc metadata "
                                "inconsistency; protocol requires "
                                "CLL ≤ mastering max)",
                                fw_cll, fw_maxlum);
                        fw_cll = fw_maxlum;
                    }
                    if (fw_fall > 0.0f && fw_cll > 0.0f
                            && fw_fall > fw_cll) {
                        log_msg("HDR: MaxFALL %.0f exceeds MaxCLL %.0f "
                                "— clamped", fw_fall, fw_cll);
                        fw_fall = fw_cll;
                    }
                    SDL_SetFloatProperty(wp, "dsvp.hdr.max_cll",  fw_cll);
                    SDL_SetFloatProperty(wp, "dsvp.hdr.max_fall", fw_fall);
                    log_msg("HDR: static metadata staged for swapchain "
                            "(maxLum=%.0f minLum=%.4f CLL=%.0f FALL=%.0f — "
                            "patched SDL consumes, stock SDL ignores)",
                            fw_maxlum, av_q2d(m->min_luminance),
                            fw_cll, fw_fall);
                }
            }
            SDL_SetFloatProperty(wp, "dsvp.hdr.max_luminance", fw_maxlum);
        }

        /* Fallback: no metadata → 1000 nits (standard HDR10 assumption) */
        if (is_hdr && peak_nits == 0.0f) {
            peak_nits = 1000.0f;
            log_msg("HDR: no luminance metadata — using 1000 nit fallback");
        }

        /* DV P5 base layer is full-range by spec (IPTPQc2).
         * Range override applied after HDR detection completes. */
        if (dv_profile == 5) {
            log_msg("HDR: DV Profile 5 detected — full-range override pending");
        }
    }

    /* Gamut classification for the shader:
     * - DV P5: output after DV reshaping is BT.2020 (always)
     * - HDR10 with BT.2020 primaries: needs gamut mapping in tone map.
     * - DV P5 without explicit BT.2020 primaries: DV decode handles gamut. */
    float hdr_gamut = 0.0f; /* 0.0 = BT.709 primaries */
    int is_dovi_active = 0;
    if (is_hdr && is_dolby_vision && !has_pq_transfer) {
        /* DV-only (no PQ transfer tag, e.g. Profile 5):
         * Base layer is IPTPQc2 — needs DV reshaping pipeline.
         * The DV decode chain outputs BT.2020, so set gamut accordingly.
         * DV uniforms will be populated from first decoded frame's RPU.
         *
         * Spec-conforming P5 is always 10-bit 4:2:0; the full-range
         * override below hardwires 10-bit scale factors, so a
         * mistagged/nonconforming stream on any other upload path would
         * get a 64x range scale — white garbage with no diagnostic
         * (DSVP main 7f09ae0). Guard on the deck's actual 10-bit
         * arrival paths: VAAPI P010 or software yuv420p10le — the same
         * predicate gpu_create_video_textures uses to pick R16. */
        int is_10bit_path =
            (ps->video_codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P10LE
             || (ps->vaapi_active && !ps->vaapi_nv12));
        if (is_10bit_path) {
            is_dovi_active = 1;
            hdr_gamut = 1.0f;
            log_msg("HDR: Dolby Vision Profile 5 — DV reshape pipeline active");
        } else {
            log_msg("WARN: DV P5 tagged but not on a 10-bit path "
                    "(VAAPI P010 / yuv420p10le) — DV pipeline disabled "
                    "for this file");
        }
    } else if (is_hdr && has_bt2020_primaries) {
        hdr_gamut = 1.0f;   /* 1.0 = BT.2020 primaries */
    } else if (!is_hdr && has_bt2020_primaries) {
        /* SDR tagged BT.2020: shader converts primaries to 709 in
         * linear light (was displayed unconverted — visibly
         * desaturated). (DSVP main fdbb489.) */
        hdr_gamut = 1.0f;
        log_msg("SDR BT.2020 content — gamut conversion to 709 active");
    }

    /* HLG carries no mastering metadata worth trusting; the OOTF is a
     * fixed 1000-nit nominal, so pin the peak — a stray MaxCLL from
     * the container must not stretch the tone map. The PQ scene-peak
     * histogram is likewise gated off in video_display: its bin→nits
     * conversion decodes PQ and is meaningless for HLG's relative
     * signal. */
    if (is_hlg)
        peak_nits = 1000.0f;

    ps->gpu_uniforms.is_hdr        = is_hdr ? 1.0f : 0.0f;
    ps->gpu_uniforms.is_hlg        = is_hlg ? 1.0f : 0.0f;
    ps->gpu_uniforms.hdr_pass      = 0.0f;  /* hdr_output_apply() decides */
    ps->gpu_uniforms.hdr_peak_nits = peak_nits;

    /* HDR passthrough eligibility (docs/TODO-HDR.md): every HDR class
     * now rides the HDR10 container — HDR10/DV-P8 ship PQ as-is, DV
     * P5 re-encodes to PQ after the per-frame RPU reshape (item 5,
     * DV-as-HDR10 — what an LLDV player does internally), HLG
     * converts via OOTF + PQ OETF in-shader (item 4). */
    ps->hdr_pass_content = is_hdr;
    ps->gpu_uniforms.hdr_gamut     = hdr_gamut;
    ps->gpu_uniforms.hdr_debug     = 0.0f;
    ps->gpu_uniforms.is_dovi       = is_dovi_active ? 1.0f : 0.0f;
    ps->gpu_uniforms.is_semiplanar = 0.0f;

    /* Output transfer for tone-mapped content. Displays decode ~2.2
     * regardless of "sRGB support" — the sRGB piecewise toe lifts
     * shadows on a calibrated screen, so pure power 2.2 is the
     * reference-faithful default. DSVP_OUTPUT_GAMMA=srgb|2.2|2.4
     * sets the startup value; the E key cycles live. Deck note: the
     * internal panel decodes ~2.2; docked to an OLED TV, 2.4 is worth
     * an eye test both ways. SDR passthrough is untouched (gamma-in =
     * gamma-out was already faithful). (DSVP main fdbb489.)
     *
     * out_gamma_pref survives file opens like hdr_target_nits does:
     * env parsed only while unset (0), so an E-key choice is not
     * clobbered by the next open. Pref 1.0 = sRGB (uniform 0.0). */
    if (ps->out_gamma_pref == 0.0f) {
        float pref = 2.2f;
        const char *og = SDL_getenv("DSVP_OUTPUT_GAMMA");
        if (og) {
            if (SDL_strcasecmp(og, "srgb") == 0) pref = 1.0f;
            else {
                double v = SDL_atof(og);
                if (v >= 1.0 && v <= 3.0) pref = (float)v;
                else log_msg("WARN: DSVP_OUTPUT_GAMMA='%s' ignored", og);
            }
        }
        ps->out_gamma_pref = pref;
    }
    /* Output gamut, like the transfer above: a property of the DISPLAY,
     * not of the file, so it survives file opens and is re-applied per
     * file rather than re-derived. DSVP_OUT_GAMUT=2020 sets it at
     * startup; the M key toggles it live for A/B against the same
     * frame, which is the only way a primaries change can honestly be
     * judged. Default stays BT.709 — flipping the default to follow
     * the display's own wide-gamut state is a separate change, after
     * the eye has ruled on this one. */
    /* The HDR10 container is BT.2020 by definition, so PQ output forces
     * the primaries conversion on regardless of the M-key preference —
     * emitting 709 code values into a PQ/BT.2020 surface would be the
     * original oversaturation bug wearing a different hat. */
    int gamut_2020 = ps->out_gamut_pref || ps->out_pq_nits > 0.0f;
    ps->gpu_uniforms.out_gamut = gamut_2020 ? 1.0f : 0.0f;
    ps->gpu_uniforms.out_pq    = out_pq_uniform(ps);
    log_msg("Output gamut: %s%s",
            gamut_2020 ? "BT.2020 (wide-gamut display)"
                       : "BT.709 (default)",
            ps->out_pq_nits > 0.0f ? " — PQ/HDR10 container" : "");

    ps->gpu_uniforms.out_gamma =
        (ps->out_gamma_pref == 1.0f) ? 0.0f : ps->out_gamma_pref;
    log_msg("Output transfer: %s",
            ps->out_gamma_pref == 1.0f ? "sRGB piecewise" :
            ps->out_gamma_pref == 2.2f ? "gamma 2.2 (default)" :
                                         "custom gamma");

    /* DV P5 range override: container says limited but IPTPQc2 is full-range.
     * Must happen after normal range setup since it overrides those values.
     *
     * Two storage formats:
     *   - VAAPI P010: 10-bit left-shifted by 6 → max 65472 in uint16.
     *     R16_UNORM reads 65472/65535 ≈ 0.999. Scale = 65535/65472.
     *   - Software YUV420P10LE: raw 10-bit → max 1023 in uint16.
     *     R16_UNORM reads 1023/65535 ≈ 0.0156. Scale = 65535/1023. */
    if (is_dovi_active) {
        ps->gpu_uniforms.rangeY[0]  = 0.0f;
        ps->gpu_uniforms.rangeUV[0] = 0.0f;
        if (ps->vaapi_active && !ps->vaapi_nv12) {
            /* P010: (V << 6) storage, near-unity scale */
            ps->gpu_uniforms.rangeY[1]  = 65535.0f / 65472.0f;
            ps->gpu_uniforms.rangeUV[1] = 65535.0f / 65472.0f;
        } else {
            /* Software decode: raw 10-bit in uint16 */
            ps->gpu_uniforms.rangeY[1]  = 65535.0f / 1023.0f;
            ps->gpu_uniforms.rangeUV[1] = 65535.0f / 1023.0f;
        }
        log_msg("GPU: DV P5 — range overridden to full-range 10-bit%s",
                (ps->vaapi_active && !ps->vaapi_nv12) ? " (P010)" : "");
    }

    /* Initialize DV uniforms to identity (populated from first frame RPU) */
    if (is_dovi_active) {
        /* Identity reshape: 1 piece per component, pivots [0,1], out = x */
        memset(ps->gpu_uniforms.dovi_num_pieces, 0, sizeof(ps->gpu_uniforms.dovi_num_pieces));
        memset(ps->gpu_uniforms.dovi_pivots, 0, sizeof(ps->gpu_uniforms.dovi_pivots));
        memset(ps->gpu_uniforms.dovi_c0, 0, sizeof(ps->gpu_uniforms.dovi_c0));
        memset(ps->gpu_uniforms.dovi_c1, 0, sizeof(ps->gpu_uniforms.dovi_c1));
        memset(ps->gpu_uniforms.dovi_c2, 0, sizeof(ps->gpu_uniforms.dovi_c2));
        for (int c = 0; c < 3; c++) {
            ps->gpu_uniforms.dovi_num_pieces[c] = 1.0f;
            ps->gpu_uniforms.dovi_pivots[0][c] = 0.0f;
            ps->gpu_uniforms.dovi_pivots[1][c] = 1.0f;
            ps->gpu_uniforms.dovi_c0[0][c] = 0.0f;  /* c0 = 0 */
            ps->gpu_uniforms.dovi_c1[0][c] = 1.0f;  /* c1 = 1 → out = x */
            ps->gpu_uniforms.dovi_c2[0][c] = 0.0f;
        }
        /* Identity matrices (will be overwritten by first frame) */
        /* MMR off until the first RPU says otherwise */
        memset(ps->gpu_uniforms.dovi_mmr_meta, 0,
               sizeof(ps->gpu_uniforms.dovi_mmr_meta));
        memset(ps->gpu_uniforms.dovi_mmr_ct, 0,
               sizeof(ps->gpu_uniforms.dovi_mmr_ct));
        memset(ps->gpu_uniforms.dovi_mmr_cp, 0,
               sizeof(ps->gpu_uniforms.dovi_mmr_cp));
        memset(ps->gpu_uniforms.dovi_ycc_r0, 0, 4 * sizeof(float));
        memset(ps->gpu_uniforms.dovi_ycc_r1, 0, 4 * sizeof(float));
        memset(ps->gpu_uniforms.dovi_ycc_r2, 0, 4 * sizeof(float));
        ps->gpu_uniforms.dovi_ycc_r0[0] = 1.0f;
        ps->gpu_uniforms.dovi_ycc_r1[1] = 1.0f;
        ps->gpu_uniforms.dovi_ycc_r2[2] = 1.0f;
        memset(ps->gpu_uniforms.dovi_out_r0, 0, 4 * sizeof(float));
        memset(ps->gpu_uniforms.dovi_out_r1, 0, 4 * sizeof(float));
        memset(ps->gpu_uniforms.dovi_out_r2, 0, 4 * sizeof(float));
        ps->gpu_uniforms.dovi_out_r0[0] = 1.0f;
        ps->gpu_uniforms.dovi_out_r1[1] = 1.0f;
        ps->gpu_uniforms.dovi_out_r2[2] = 1.0f;
    }

    /* SDR target nits — preserved across file opens (N key cycles).
     * Only initialize to default if not already set by a previous file. */
    if (ps->gpu_uniforms.hdr_target_nits < 1.0f)
        ps->gpu_uniforms.hdr_target_nits = 203.0f;

    /* Save static peak as ceiling for dynamic detection.
     * Initialize smoothing state — first frame will set the actual peak. */
    ps->hdr_static_peak      = peak_nits;
    ps->hdr_smoothed_peak    = 0.0f;   /* 0 = uninitialized, first frame jumps */
    ps->hdr_prev_frame_peak  = 0.0f;
    ps->dovi_metadata_logged = 0;

    if (is_hdr) {
        float target = ps->gpu_uniforms.hdr_target_nits;
        float maxLum = target / peak_nits;
        float ks = 1.5f * maxLum - 0.5f;
        if (ks < 0.0f) ks = 0.0f;
        log_msg("GPU: HDR→SDR tone mapping active (peak=%.0f nits, target=%.0f nits, gamut=%s%s)",
                peak_nits, target,
                has_bt2020_primaries ? "BT.2020" : "BT.709",
                is_dolby_vision ? ", Dolby Vision" : "");
        log_msg("HDR: BT.2390 EETF (target=%.0f nits, KS=%.3f, maxLum=%.4f)",
                target, ks, maxLum);
    }

    static const char *chroma_names[] = {
        "unspecified", "left", "center", "top-left",
        "top", "bottom-left", "bottom"
    };
    const char *cn = (chroma_loc >= 0 && chroma_loc <= 6)
        ? chroma_names[chroma_loc] : "unknown";
    log_msg("GPU: chroma siting=%s (offset %.2f, %.2f texels)",
            cn, ps->gpu_uniforms.chromaOffset[0],
            ps->gpu_uniforms.chromaOffset[1]);
}


/* ═══════════════════════════════════════════════════════════════════
 * Overlay GPU Resources
 * ═══════════════════════════════════════════════════════════════════
 *
 * The overlay system composites debug info, seek bar, subtitles, and
 * other UI elements as a single RGBA texture drawn over the video
 * with alpha blending. The texture is recreated when the window is
 * resized. Upload happens once per frame when overlay_dirty is set.
 */

/* Ensure overlay texture and transfer buffer exist at the given size.
 * Recreates if dimensions changed. Returns 1 if the texture was
 * (re)created — its contents are undefined VRAM and the caller must
 * upload the FULL height before it is drawn — 0 if already the right
 * size, -1 on error. */
int gpu_overlay_ensure(PlayerState *ps, int width, int height) {
    if (!ps->gpu_device || width <= 0 || height <= 0) return -1;

    /* Already the right size? */
    if (ps->gpu_overlay_tex &&
        ps->overlay_tex_w == width && ps->overlay_tex_h == height) {
        return 0;
    }

    /* Destroy old resources */
    gpu_overlay_destroy(ps);

    /* No audio pause around this allocation, deliberately. The old
     * pause/resume pair (insurance against a 200-350ms alloc stall
     * that the exclusive-fullscreen fix reduced to 1-11ms) had both
     * failure returns skip the resume — audio silent for the rest of
     * the file, recoverable only by pausing twice (Knot audit finding
     * 4). Deleting the mechanism deletes the bug. Likewise never reset
     * frame_timer or audio clocks here — lies-to-the-clock accumulate
     * drift on every overlay alloc / resize. */
    double alloc_start = get_time_sec();

    /* Create RGBA8888 texture */
    SDL_GPUTextureCreateInfo tex_info;
    SDL_zero(tex_info);
    tex_info.type                 = SDL_GPU_TEXTURETYPE_2D;
    tex_info.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tex_info.width                = width;
    tex_info.height               = height;
    tex_info.layer_count_or_depth = 1;
    tex_info.num_levels           = 1;
    tex_info.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;

    ps->gpu_overlay_tex = SDL_CreateGPUTexture(ps->gpu_device, &tex_info);
    if (!ps->gpu_overlay_tex) {
        log_msg("ERROR: Failed to create overlay texture: %s", SDL_GetError());
        return -1;
    }

    /* Create transfer buffer (RGBA = 4 bytes per pixel) */
    SDL_GPUTransferBufferCreateInfo xfer_info;
    SDL_zero(xfer_info);
    xfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    xfer_info.size  = (Uint32)width * height * 4;

    ps->gpu_overlay_xfer = SDL_CreateGPUTransferBuffer(ps->gpu_device, &xfer_info);
    if (!ps->gpu_overlay_xfer) {
        log_msg("ERROR: Failed to create overlay transfer buffer: %s", SDL_GetError());
        SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_overlay_tex);
        ps->gpu_overlay_tex = NULL;
        return -1;
    }

    ps->overlay_tex_w = width;
    ps->overlay_tex_h = height;
    ps->overlay_dirty = 0;
    /* Contents are undefined VRAM until a full-height upload lands.
     * Callers that use the return value handle this themselves; the
     * player_open pre-alloc discards it — gpu_overlay_upload widens
     * the first band to full height when this is set (review P1-5). */
    ps->overlay_tex_undefined = 1;

    log_msg("GPU: overlay texture created (%dx%d RGBA, alloc %.0fms)",
            width, height, (get_time_sec() - alloc_start) * 1000.0);

    return 1;
}

/* Upload RGBA pixel data to the overlay GPU texture.
 * `rgba` must be width×height×4 bytes, tightly packed. */
void gpu_overlay_upload(PlayerState *ps, const uint8_t *rgba,
                        int width, int height, int y0, int y1) {
    if (!ps->gpu_overlay_xfer || !ps->gpu_overlay_tex) return;
    if (width != ps->overlay_tex_w || height != ps->overlay_tex_h) return;

    /* Fresh texture with no full-height upload yet: widen this band to
     * cover every row, or the fullscreen composite samples undefined
     * VRAM in the rows nobody drew (review P1-5; reachable via the
     * player_open pre-alloc when its size matches the window). The
     * CPU pixel buffer is always fully initialized, so full-height is
     * safe from any caller. */
    if (ps->overlay_tex_undefined) {
        y0 = 0;
        y1 = height;
        ps->overlay_tex_undefined = 0;
    }

    /* Bound the copy to the changed rows (DSVP main 135914f). A
     * full-frame upload moved the whole overlay through the transfer
     * buffer AND the GPU copy every frame just to show a seekbar;
     * overlay.c already tracks the cleared/drawn row bands. */
    if (y0 < 0) y0 = 0;
    if (y1 > height) y1 = height;
    if (y1 <= y0) return;

    /* Union with any pending not-yet-copied window FIRST: map with
     * cycle=true may hand a fresh buffer, so every row the pending GPU
     * region will read must be rewritten from the persistent CPU-side
     * pixel buffer. */
    if (ps->overlay_dirty) {
        if (ps->overlay_up_y0 < y0) y0 = ps->overlay_up_y0;
        if (ps->overlay_up_y1 > y1) y1 = ps->overlay_up_y1;
    }

    uint8_t *dst = SDL_MapGPUTransferBuffer(ps->gpu_device,
                                             ps->gpu_overlay_xfer, true);
    if (!dst) {
        log_msg("ERROR: overlay transfer map failed: %s", SDL_GetError());
        return;
    }
    size_t off = (size_t)y0 * width * 4;
    memcpy(dst + off, rgba + off, (size_t)(y1 - y0) * width * 4);
    SDL_UnmapGPUTransferBuffer(ps->gpu_device, ps->gpu_overlay_xfer);

    ps->overlay_up_y0 = y0;
    ps->overlay_up_y1 = y1;
    ps->overlay_dirty = 1;
}

/* Issue the GPU copy pass to transfer overlay data to the texture.
 * Call this inside an existing command buffer, BEFORE the render pass.
 * Returns the copy pass so the caller can end it, or does it inline. */
void gpu_overlay_copy_cmd(SDL_GPUCommandBuffer *cmd, PlayerState *ps) {
    if (!ps->overlay_dirty || !ps->gpu_overlay_tex) return;

    int cy0 = ps->overlay_up_y0;
    int cy1 = ps->overlay_up_y1;
    /* gpu_overlay_upload only sets overlay_dirty for a clamped non-empty
     * band on the current texture, so this never fires — but the old
     * "repair" fallback (cy1 = overlay_tex_h) could pair a stale cy0
     * with it, wrap rows_per_layer negative through Uint32, and issue a
     * massive over-read. Treat an invalid band as a skip, not a fix. */
    if (cy0 < 0 || cy1 <= cy0 || cy1 > ps->overlay_tex_h) {
        ps->overlay_dirty = 0;
        return;
    }

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    {
        SDL_GPUTextureTransferInfo src_info;
        SDL_GPUTextureRegion dst_region;

        SDL_zero(src_info);
        SDL_zero(dst_region);
        src_info.transfer_buffer = ps->gpu_overlay_xfer;
        src_info.offset          = (Uint32)((size_t)cy0
                                       * ps->overlay_tex_w * 4);
        src_info.pixels_per_row  = ps->overlay_tex_w;
        src_info.rows_per_layer  = cy1 - cy0;
        dst_region.texture = ps->gpu_overlay_tex;
        dst_region.y = (Uint32)cy0;
        dst_region.w = ps->overlay_tex_w;
        dst_region.h = (Uint32)(cy1 - cy0);
        dst_region.d = 1;
        /* cycle MUST be false for a partial upload (DSVP main 56f7739):
         * cycling lets SDL hand back a different backing allocation, so
         * rows outside this region would hold undefined recycled
         * content — it strobed the elements not re-uploaded that tick. */
        SDL_UploadToGPUTexture(copy, &src_info, &dst_region, false);
    }
    SDL_EndGPUCopyPass(copy);

    ps->overlay_dirty = 0;
}

/* Draw the overlay quad within an existing render pass.
 * Uses the overlay pipeline (alpha blend) and a fullscreen viewport.
 * Call AFTER the video quad has been drawn. */
void gpu_overlay_draw(SDL_GPURenderPass *pass, SDL_GPUCommandBuffer *cmd,
                      PlayerState *ps, Uint32 sc_w, Uint32 sc_h) {
    if (!ps->gpu_overlay_tex || !ps->gpu_pipeline_overlay || !ps->overlay_active)
        return;

    /* SDR-authored overlay pixels need PQ re-encode whenever the
     * SWAPCHAIN is HDR10 — which in PQ mode is the whole session,
     * SDR content, browser and idle screen included. Keying this on
     * hdr_out_active alone left every overlay un-encoded in PQ mode:
     * white = PQ code 1.0 = 10,000 nits, the exact flashbang the
     * shader comment warns about (review 2026-08-20 finding 1).
     * Reference white: 203 nits over passthrough HDR (BT.2408
     * graphics white); the SDR reference white in SDR-in-PQ mode so
     * overlay white sits AT video white, not 2x above it. */
    float ov_params[4] = {
        ps->swapchain_hdr10 ? 1.0f : 0.0f,
        (!ps->hdr_out_active && ps->out_pq_nits > 0.0f)
            ? ps->out_pq_nits : 203.0f,
        0, 0
    };
    SDL_PushGPUFragmentUniformData(cmd, 0, ov_params, sizeof(ov_params));

    SDL_BindGPUGraphicsPipeline(pass, ps->gpu_pipeline_overlay);

    /* Fullscreen viewport — overlay covers entire window, not just
     * the letterboxed video area. This lets us draw seek bars,
     * debug info, etc. in the black bar regions too. */
    SDL_GPUViewport viewport;
    viewport.x = 0;
    viewport.y = 0;
    viewport.w = (float)sc_w;
    viewport.h = (float)sc_h;
    viewport.min_depth = 0.0f;
    viewport.max_depth = 1.0f;
    SDL_SetGPUViewport(pass, &viewport);

    SDL_GPUTextureSamplerBinding binding = {
        .texture = ps->gpu_overlay_tex,
        .sampler = ps->gpu_sampler_nearest  /* nearest = pixel-perfect bitmap font */
    };
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

    SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
}

/* Destroy overlay GPU resources (texture + transfer buffer). */
void gpu_overlay_destroy(PlayerState *ps) {
    if (!ps->gpu_device) return;

    if (ps->gpu_overlay_tex) {
        SDL_ReleaseGPUTexture(ps->gpu_device, ps->gpu_overlay_tex);
        ps->gpu_overlay_tex = NULL;
    }
    if (ps->gpu_overlay_xfer) {
        SDL_ReleaseGPUTransferBuffer(ps->gpu_device, ps->gpu_overlay_xfer);
        ps->gpu_overlay_xfer = NULL;
    }
    ps->overlay_tex_w = 0;
    ps->overlay_tex_h = 0;
    ps->overlay_dirty = 0;
}


/* ═══════════════════════════════════════════════════════════════════
 * Packet Queue — thread-safe FIFO for AVPackets
 * ═══════════════════════════════════════════════════════════════════ */

void pq_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond  = SDL_CreateCondition();
}

void pq_destroy(PacketQueue *q) {
    pq_flush(q);
    /* NULL the handles: player_open() can fail BEFORE pq_init() runs on
     * a re-open, and player_close() then calls pq_destroy() again on
     * the previous file's already-destroyed (but non-NULL) handles —
     * a use-after-free without this. SDL_DestroyMutex/Condition and
     * SDL_LockMutex are NULL-safe, so double-destroy becomes a no-op. */
    if (q->mutex) { SDL_DestroyMutex(q->mutex);    q->mutex = NULL; }
    if (q->cond)  { SDL_DestroyCondition(q->cond); q->cond  = NULL; }
}

/* Push a packet onto the queue. Caller still owns pkt after this call
 * returns — we move the packet data into a new AVPacket internally. */
int pq_put(PacketQueue *q, AVPacket *pkt) {
    PacketNode *node = av_malloc(sizeof(PacketNode));
    if (!node) return -1;

    node->pkt = av_packet_alloc();
    if (!node->pkt) {
        av_free(node);
        return -1;
    }
    av_packet_move_ref(node->pkt, pkt);
    node->next = NULL;

    SDL_LockMutex(q->mutex);

    if (!q->last) {
        q->first = node;
    } else {
        q->last->next = node;
    }
    q->last = node;
    q->nb_packets++;
    q->size += node->pkt->size;

    SDL_SignalCondition(q->cond);
    SDL_UnlockMutex(q->mutex);
    return 0;
}

/* Pop a packet from the queue. If block=1, waits until data arrives
 * or abort_request is set. Returns 1 on success, 0 if non-blocking
 * and empty, -1 if aborted. */
/* Unlink and free the head node, keeping the queue accounting exact.
 * Moves the packet into *out when given, discards it otherwise.
 * Caller must hold q->mutex and have checked q->first != NULL.
 * (This five-line unlink used to exist as three inline copies.) */
static void pq_unlink_head(PacketQueue *q, AVPacket *out) {
    PacketNode *node = q->first;
    q->first = node->next;
    if (!q->first) q->last = NULL;
    q->nb_packets--;
    q->size -= node->pkt->size;
    if (out) av_packet_move_ref(out, node->pkt);
    av_packet_free(&node->pkt);
    av_free(node);
}

int pq_get(PacketQueue *q, AVPacket *pkt, int block) {
    int ret = -1;

    SDL_LockMutex(q->mutex);
    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        if (q->first) {
            pq_unlink_head(q, pkt);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_WaitCondition(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

/* Flush all packets from the queue. Called on seek or close. */
/* Drop head packets older than min_pts (stream time base). Keeps the
 * subtitle queues as rolling windows: every track stays queued so an
 * S-press has the current moment's packets on hand (an empty queue
 * only fills from the demux read position, ~10s ahead of playback —
 * the user-visible "subtitles don't work" delay), while memory stays
 * bounded instead of accumulating for the whole file. Stops at a
 * NOPTS head (can't judge it), so max_bytes backstops the tracks the
 * PTS walk can't police: a NOPTS-emitting track (DVB teletext in TS)
 * halted pruning permanently, and unselected tracks are never drained
 * by decode — the queue grew unbounded for the whole file.
 * Ported from DSVP main 55834d4; byte backstop added in review. */
void pq_prune_stale(PacketQueue *q, int64_t min_pts, int max_bytes) {
    SDL_LockMutex(q->mutex);
    while (q->first && q->first->pkt->pts != AV_NOPTS_VALUE
           && q->first->pkt->pts < min_pts)
        pq_unlink_head(q, NULL);
    while (q->first && q->size > max_bytes)
        pq_unlink_head(q, NULL);
    SDL_UnlockMutex(q->mutex);
}

/* Peek the PTS of the head packet without consuming it. Returns 1 with
 * *pts_out set when a packet is queued, 0 when empty. Used by the
 * subtitle drain to avoid consuming display sets before their time. */
int pq_peek_pts(PacketQueue *q, int64_t *pts_out) {
    SDL_LockMutex(q->mutex);
    int have = (q->first != NULL);
    if (have) *pts_out = q->first->pkt->pts;
    SDL_UnlockMutex(q->mutex);
    return have;
}

void pq_flush(PacketQueue *q) {
    SDL_LockMutex(q->mutex);
    PacketNode *node = q->first;
    while (node) {
        PacketNode *next = node->next;
        av_packet_free(&node->pkt);
        av_free(node);
        node = next;
    }
    q->first = NULL;
    q->last  = NULL;
    q->nb_packets = 0;
    q->size = 0;
    SDL_UnlockMutex(q->mutex);
}


/* ═══════════════════════════════════════════════════════════════════
 * VAAPI Hardware Decode (Linux only, HEVC)
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef __linux__
/* Callback for FFmpeg to select hardware pixel format.
 * If VAAPI is in the list, select it; otherwise fall back. */
static enum AVPixelFormat vaapi_get_format(AVCodecContext *ctx,
                                           const enum AVPixelFormat *pix_fmts)
{
    for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VAAPI)
            return AV_PIX_FMT_VAAPI;
    }
    /* Unsupported profile (HEVC 4:2:2/Rext etc.) — VAAPI not offered.
     * This fires at first decode, after the whole VAAPI upload
     * pipeline is already configured, so a mid-stream format switch
     * is not safe; flag it and let the hard-error escalation abort,
     * after which main.c reopens the file in software (review P2-17). */
    PlayerState *ps = (PlayerState *)ctx->opaque;
    if (ps) ps->vaapi_unsupported = 1;
    log_msg("VAAPI: hardware format not offered by decoder — "
            "will reopen in software decode");
    return AV_PIX_FMT_NONE;
}
#endif /* __linux__ */


/* ═══════════════════════════════════════════════════════════════════
 * VAAPI Zero-Copy Interop (Linux only)
 * ═══════════════════════════════════════════════════════════════════
 *
 * Eliminates the 35-42ms av_hwframe_transfer_data GPU→CPU readback
 * by importing the VAAPI decoded surface directly into Vulkan via
 * DMA-BUF file descriptors.
 *
 * Flow per frame:
 *   1. vaSyncSurface (ensure decode complete)
 *   2. vaExportSurfaceHandle → DMA-BUF fds + DRM format info
 *   3. Import as VkImage via VkImportMemoryFdInfoKHR
 *   4. vkCmdCopyImage → existing SDL_GPU textures
 *   5. SDL_GPU render pass samples the textures (same queue = ordered)
 *
 * Requires SDL3 ≥ 3.4.0 for SDL_GPUVulkanOptions.
 * Requires Vulkan extensions: VK_KHR_external_memory_fd,
 *   VK_EXT_external_memory_dma_buf, VK_EXT_image_drm_format_modifier.
 */


/* ── SDL_GPU Internal Struct Offsets (x86_64 Linux) ──
 * Validated against SDL3 3.4.2 (session 11) and 3.4.14 (field, the
 * deployed deck build). vaapi_zerocopy_init refuses the offset path
 * on any OTHER SDL version (readback fallback) — extend its version
 * whitelist and this comment together when a new build is validated. */

/* SDL_GPUDevice[+664] → VulkanRenderer* */
#define DSVP_VK_WRAPPER_OFFSET     664
#define DSVP_VK_INSTANCE_OFFSET    0
#define DSVP_VK_PHYSDEV_OFFSET     8
#define DSVP_VK_DEVICE_OFFSET      1392
#define DSVP_VK_QFI_OFFSET         1984
#define DSVP_VK_QUEUE_OFFSET       1992

/* SDL_GPUTexture → VkImage extraction
 *
 * SDL_GPUTexture* is actually VulkanTextureContainer* (internal to SDL3).
 * We need VkImage for DMA-BUF copy targets.
 *
 * Struct layout from SDL3 3.4.2 source (src/gpu/vulkan/SDL_gpu_vulkan.c):
 *
 *   VulkanTextureContainer {
 *       TextureCommonHeader header;         // 36B (SDL_GPUTextureCreateInfo)
 *       // 4B padding (align ptr to 8)
 *       VulkanTexture *activeTexture;       // offset 40
 *       ...
 *   };
 *
 *   VulkanTexture {
 *       VulkanTextureContainer *container;  // offset 0
 *       Uint32 containerIndex;              // offset 8, +4B padding
 *       VulkanMemoryUsedRegion *usedRegion; // offset 16
 *       VkImage image;                      // offset 24  ← target
 *       ...
 *   };
 *
 * Chain: container[+40] → activeTexture[+24] → VkImage
 * Validated at init with two test textures of different sizes. */

#define DSVP_TEX_CONTAINER_TO_ACTIVE  40  /* VulkanTextureContainer → VulkanTexture* */
#define DSVP_TEX_ACTIVE_TO_VKIMAGE    24  /* VulkanTexture → VkImage */

static int s_tex_offsets_valid = 0;  /* 1 after successful validation */


/* Extract VkImage from SDL_GPUTexture using known struct offsets. */
static VkImage sdl_texture_to_vkimage(SDL_GPUTexture *tex)
{
    uint8_t *container = (uint8_t *)tex;
    uint8_t *active = *(uint8_t **)(container + DSVP_TEX_CONTAINER_TO_ACTIVE);
    return *(VkImage *)(active + DSVP_TEX_ACTIVE_TO_VKIMAGE);
}


/* Validate VkImage extraction by checking memory requirements.
 * Creates two test textures (4×4 and 16×16), extracts VkImage from each
 * using the hardcoded offsets, and verifies both produce sane memory
 * requirements with the larger texture needing more memory.
 * Returns 0 on success, -1 on failure. */
static int vaapi_zerocopy_validate_vkimage(PlayerState *ps)
{
    SDL_GPUTextureCreateInfo ti;
    SDL_zero(ti);
    ti.type   = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R16_UNORM;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    ti.usage  = SDL_GPU_TEXTUREUSAGE_SAMPLER;

    ti.width = 16; ti.height = 16;
    SDL_GPUTexture *tex_small = SDL_CreateGPUTexture(ps->gpu_device, &ti);

    ti.width = 256; ti.height = 256;
    SDL_GPUTexture *tex_large = SDL_CreateGPUTexture(ps->gpu_device, &ti);

    if (!tex_small || !tex_large) {
        log_msg("ZEROCOPY validate: test texture creation failed");
        if (tex_small) SDL_ReleaseGPUTexture(ps->gpu_device, tex_small);
        if (tex_large) SDL_ReleaseGPUTexture(ps->gpu_device, tex_large);
        return -1;
    }

    /* Verify the activeTexture pointer at offset 40 looks valid */
    uint8_t *active_s = *(uint8_t **)((uint8_t *)tex_small + DSVP_TEX_CONTAINER_TO_ACTIVE);
    uint8_t *active_l = *(uint8_t **)((uint8_t *)tex_large + DSVP_TEX_CONTAINER_TO_ACTIVE);
    if (!active_s || !active_l) {
        log_msg("ZEROCOPY validate: activeTexture is NULL — readback fallback");
        SDL_ReleaseGPUTexture(ps->gpu_device, tex_small);
        SDL_ReleaseGPUTexture(ps->gpu_device, tex_large);
        return -1;
    }

    /* Extract VkImages at offset 24 within VulkanTexture */
    VkImage img_s = *(VkImage *)(active_s + DSVP_TEX_ACTIVE_TO_VKIMAGE);
    VkImage img_l = *(VkImage *)(active_l + DSVP_TEX_ACTIVE_TO_VKIMAGE);

    /* Validate via vkGetImageMemoryRequirements */
    VkMemoryRequirements req_s, req_l;
    vkGetImageMemoryRequirements(ps->vk_device, img_s, &req_s);
    vkGetImageMemoryRequirements(ps->vk_device, img_l, &req_l);

    int ok = 1;

    /* Sanity: sizes should be small (< 1MB for tiny textures) and non-zero */
    if (req_s.size == 0 || req_s.size > 1048576) ok = 0;
    if (req_l.size == 0 || req_l.size > 1048576) ok = 0;
    /* Larger texture must need more memory */
    if (req_l.size <= req_s.size) ok = 0;
    /* Alignment must be power of 2 */
    if (req_s.alignment == 0 || (req_s.alignment & (req_s.alignment - 1)) != 0) ok = 0;
    /* Must have compatible memory types */
    if (req_s.memoryTypeBits == 0 || req_l.memoryTypeBits == 0) ok = 0;

    SDL_ReleaseGPUTexture(ps->gpu_device, tex_small);
    SDL_ReleaseGPUTexture(ps->gpu_device, tex_large);

    if (ok) {
        s_tex_offsets_valid = 1;
        log_msg("ZEROCOPY validate: VkImage at container[+%d][+%d] confirmed "
                "(16x16=%zu, 256x256=%zu bytes)",
                DSVP_TEX_CONTAINER_TO_ACTIVE, DSVP_TEX_ACTIVE_TO_VKIMAGE,
                (size_t)req_s.size, (size_t)req_l.size);
        return 0;
    } else {
        log_msg("ZEROCOPY validate: VkImage extraction failed "
                "(16x16: size=%zu align=%zu bits=0x%x, "
                "256x256: size=%zu align=%zu bits=0x%x) — readback fallback",
                (size_t)req_s.size, (size_t)req_s.alignment, req_s.memoryTypeBits,
                (size_t)req_l.size, (size_t)req_l.alignment, req_l.memoryTypeBits);
        return -1;
    }
}


/* ── Initialize zero-copy: extract Vulkan handles, create cmd pool ── */
static int zc_sync_mode(void);   /* defined with the upload path below */

int vaapi_zerocopy_init(PlayerState *ps)
{
    ps->vaapi_zerocopy = 0;

    if (!ps->hw_device_ctx) return -1;
    if (!ps->gpu_device) return -1;

    /* The renderer offsets below are hand-validated against SPECIFIC
     * SDL builds (3.4.2 session 11; 3.4.14 field-verified on the
     * deployed deck build). Unlike the texture offsets, which get a
     * behavioural validation in vaapi_zerocopy_validate_vkimage, a
     * wrong renderer offset is a segfault inside the Vulkan loader at
     * the vkDeviceWaitIdle below — it dereferences the very pointer
     * the offsets produced. So refuse the offset path on any other
     * SDL version and take the readback fallback: a SteamOS SDL bump
     * must degrade to slow-and-correct, not crash on the first
     * decoded frame (Knot audit finding 5). When a new SDL version is
     * validated, add it here AND update the offset comments. */
    int sdl_ver = SDL_GetVersion();
    if (sdl_ver != SDL_VERSIONNUM(3, 4, 2) &&
        sdl_ver != SDL_VERSIONNUM(3, 4, 14)) {
        log_msg("ZEROCOPY: SDL %d.%d.%d not offset-validated "
                "(3.4.2/3.4.14 are) — readback fallback",
                SDL_VERSIONNUM_MAJOR(sdl_ver),
                SDL_VERSIONNUM_MINOR(sdl_ver),
                SDL_VERSIONNUM_MICRO(sdl_ver));
        return -1;
    }

    /* Extract VkDevice, VkQueue from SDL_GPU VulkanRenderer */
    uint8_t *renderer = *(uint8_t **)((uint8_t *)ps->gpu_device + DSVP_VK_WRAPPER_OFFSET);
    if (!renderer) {
        log_msg("ZEROCOPY: failed to get VulkanRenderer");
        return -1;
    }

    ps->vk_device       = *(VkDevice *)(renderer + DSVP_VK_DEVICE_OFFSET);
    ps->vk_queue_family  = *(uint32_t *)(renderer + DSVP_VK_QFI_OFFSET);
    ps->vk_queue         = *(VkQueue *)(renderer + DSVP_VK_QUEUE_OFFSET);

    /* Cheap non-dereferencing sanity before vkDeviceWaitIdle takes
     * the first dereference: dispatchable Vulkan handles are pointers
     * — NULL or misaligned means the offsets read garbage. A queue
     * family index in the thousands means the same. */
    if (!ps->vk_device || ((uintptr_t)ps->vk_device & 7) ||
        !ps->vk_queue  || ((uintptr_t)ps->vk_queue & 7) ||
        ps->vk_queue_family > 255) {
        log_msg("ZEROCOPY: extracted Vulkan handles fail sanity "
                "(dev=%p queue=%p qfi=%u) — readback fallback",
                (void *)ps->vk_device, (void *)ps->vk_queue,
                ps->vk_queue_family);
        return -1;
    }

    /* Validate VkDevice */
    if (vkDeviceWaitIdle(ps->vk_device) != VK_SUCCESS) {
        log_msg("ZEROCOPY: VkDevice validation failed");
        return -1;
    }

    /* Validate VkImage extraction from SDL_GPUTexture (source-verified offsets) */
    if (vaapi_zerocopy_validate_vkimage(ps) < 0)
        return -1;

    /* Get VADisplay from FFmpeg's hw_device_ctx */
    AVHWDeviceContext *hw_ctx = (AVHWDeviceContext *)ps->hw_device_ctx->data;
    AVVAAPIDeviceContext *va_ctx = (AVVAAPIDeviceContext *)hw_ctx->hwctx;
    ps->va_display = va_ctx->display;

    /* Create Vulkan command pool + command buffer for DMA-BUF copies */
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = ps->vk_queue_family,
    };
    if (vkCreateCommandPool(ps->vk_device, &pool_info, NULL, &ps->vk_cmd_pool) != VK_SUCCESS) {
        log_msg("ZEROCOPY: vkCreateCommandPool failed");
        return -1;
    }

    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = ps->vk_cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(ps->vk_device, &alloc_info, &ps->vk_cmd_buf) != VK_SUCCESS) {
        log_msg("ZEROCOPY: vkAllocateCommandBuffers failed");
        vkDestroyCommandPool(ps->vk_device, ps->vk_cmd_pool, NULL);
        return -1;
    }

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    if (vkCreateFence(ps->vk_device, &fence_info, NULL,
                      &ps->vk_copy_fence) != VK_SUCCESS) {
        log_msg("ZEROCOPY: vkCreateFence failed");
        vkDestroyCommandPool(ps->vk_device, ps->vk_cmd_pool, NULL);
        ps->vk_cmd_pool = VK_NULL_HANDLE;
        ps->vk_cmd_buf  = VK_NULL_HANDLE;
        return -1;
    }
    ps->vk_copy_pending = 0;

    ps->vaapi_zerocopy = 1;
    log_msg("ZEROCOPY: initialized (VkDevice=%p, VADisplay=%p, qfi=%u)",
            (void *)ps->vk_device, ps->va_display, ps->vk_queue_family);
    log_msg("ZEROCOPY: import cache %s",
            SDL_getenv("DSVP_ZC_NOCACHE")
                ? "DISABLED (DSVP_ZC_NOCACHE)" : "enabled");
    log_msg("ZEROCOPY: copy sync mode: %s",
            zc_sync_mode() == 2 ? "none (DSVP_ZC_NOSYNC)"
          : zc_sync_mode() == 0 ? "own-submit fence"
          : SDL_getenv("DSVP_ZC_SYNC") ? "queue drain (DSVP_ZC_SYNC)"
          :        "queue drain (implied by DSVP_ZC_NOCACHE)");
    return 0;
}


/* ── Cleanup zero-copy resources ── */
void vaapi_zerocopy_cleanup(PlayerState *ps)
{
    /* Drain the import cache. The per-frame wait-idle means no copy
     * is in flight by the time close reaches here; the wait below
     * covers the paths that skip it (submit failure, early close). */
    int n = (int)(sizeof(ps->zc_imports) / sizeof(ps->zc_imports[0]));
    int live = 0;
    for (int i = 0; i < n; i++)
        if (ps->zc_imports[i].valid) live++;
    if ((live || ps->vk_copy_pending) && ps->vk_device) {
        vkDeviceWaitIdle(ps->vk_device);
        ps->vk_copy_pending = 0;
        for (int i = 0; i < n; i++) {
            if (!ps->zc_imports[i].valid) continue;
            vkDestroyImage(ps->vk_device, ps->zc_imports[i].img_y, NULL);
            vkDestroyImage(ps->vk_device, ps->zc_imports[i].img_uv, NULL);
            vkFreeMemory(ps->vk_device, ps->zc_imports[i].mem_y, NULL);
            vkFreeMemory(ps->vk_device, ps->zc_imports[i].mem_uv, NULL);
            ps->zc_imports[i].valid = 0;
        }
    }
    if (ps->zc_cache_hits || ps->zc_cache_misses)
        log_msg("ZEROCOPY cache: %d hits, %d misses (%d surfaces), "
                "%d rebuilds",
                ps->zc_cache_hits, ps->zc_cache_misses, live,
                ps->zc_cache_rebuilds);
    ps->zc_cache_hits = ps->zc_cache_misses = ps->zc_cache_rebuilds = 0;

    if (ps->vk_cmd_pool && ps->vk_device) {
        vkDestroyCommandPool(ps->vk_device, ps->vk_cmd_pool, NULL);
        ps->vk_cmd_pool = VK_NULL_HANDLE;
        ps->vk_cmd_buf  = VK_NULL_HANDLE;
    }
    if (ps->vk_copy_fence && ps->vk_device) {
        vkDestroyFence(ps->vk_device, ps->vk_copy_fence, NULL);
        ps->vk_copy_fence = VK_NULL_HANDLE;
    }
    ps->vaapi_zerocopy = 0;
}


/* ── Import a single DMA-BUF plane as a VkImage ──
 *
 * Creates a VkImage backed by the DMA-BUF memory at the given offset.
 * Uses VkImageDrmFormatModifierExplicitCreateInfoEXT to specify the
 * tiling layout. The caller is responsible for destroying the returned
 * VkImage and VkDeviceMemory after use.
 *
 * Returns 0 on success, -1 on failure. */
static int import_dmabuf_plane(
    VkDevice device,
    int fd,                   /* DMA-BUF fd (will be dup'd, caller keeps original) */
    uint64_t modifier,        /* DRM format modifier */
    VkFormat format,          /* VK_FORMAT_R16_UNORM or VK_FORMAT_R16G16_UNORM */
    uint32_t width,
    uint32_t height,
    uint32_t offset,          /* byte offset of this plane within the DMA-BUF */
    uint32_t pitch,           /* row pitch in bytes */
    size_t   mem_size,        /* total DMA-BUF object size */
    VkImage *out_image,
    VkDeviceMemory *out_mem)
{
    /* Plane layout within the image (single-plane image) */
    VkSubresourceLayout plane_layout = {
        .offset     = offset,
        .size       = 0,    /* ignored for import */
        .rowPitch   = pitch,
        .arrayPitch = 0,
        .depthPitch = 0,
    };

    VkImageDrmFormatModifierExplicitCreateInfoEXT drm_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .pNext = NULL,
        .drmFormatModifier       = modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts           = &plane_layout,
    };

    VkExternalMemoryImageCreateInfo ext_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &drm_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };

    VkImageCreateInfo img_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &ext_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    if (vkCreateImage(device, &img_info, NULL, out_image) != VK_SUCCESS) {
        log_msg("ZEROCOPY: vkCreateImage failed for %ux%u", width, height);
        return -1;
    }

    /* Import DMA-BUF memory */
    int dup_fd = dup(fd);
    if (dup_fd < 0) {
        log_msg("ZEROCOPY: dup(fd) failed");
        vkDestroyImage(device, *out_image, NULL);
        return -1;
    }

    VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = dup_fd,   /* Vulkan takes ownership */
    };

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(device, *out_image, &mem_req);

    /* Find a memory type that supports the image */
    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        .allocationSize = mem_size > mem_req.size ? mem_size : mem_req.size,
        .memoryTypeIndex = 0, /* DMA-BUF import: driver picks the right type */
    };

    /* For DMA-BUF imports, the memoryTypeIndex needs to be compatible.
     * Use vkGetMemoryFdPropertiesKHR if available, otherwise try type 0. */
    VkResult vr = vkAllocateMemory(device, &alloc_info, NULL, out_mem);
    if (vr != VK_SUCCESS) {
        /* Try type index 1 (some drivers need this) */
        alloc_info.memoryTypeIndex = 1;
        vr = vkAllocateMemory(device, &alloc_info, NULL, out_mem);
    }
    if (vr != VK_SUCCESS) {
        log_msg("ZEROCOPY: vkAllocateMemory failed (DMA-BUF import): %d", vr);
        vkDestroyImage(device, *out_image, NULL);
        /* VK_KHR_external_memory_fd: the implementation takes ownership of
         * the fd ONLY on success. On failure it is still ours and must be
         * closed — the previous comment talked itself out of this and leaked
         * one fd per failed import. */
        close(dup_fd);
        return -1;
    }

    if (vkBindImageMemory(device, *out_image, *out_mem, 0) != VK_SUCCESS) {
        log_msg("ZEROCOPY: vkBindImageMemory failed");
        vkFreeMemory(device, *out_mem, NULL);
        vkDestroyImage(device, *out_image, NULL);
        return -1;
    }

    return 0;
}


/* ── Per-frame zero-copy upload ──
 *
 * Exports the VAAPI surface as DMA-BUF, imports into Vulkan,
 * and copies to the existing SDL_GPU textures via vkCmdCopyImage.
 *
 * Returns 0 on success, -1 on failure (caller falls back to readback). */
/* ── Copy-synchronisation ladder (Knot gains #3) ──
 * The old per-frame vkQueueWaitIdle waited on the ENTIRE shared
 * queue — including the previous frame's SDL render and present — a
 * hard serialisation point once per frame on the path TODO-RAMHACKS
 * argues is serialisation-limited. Our command buffer already ends
 * with a barrier to SHADER_READ_ONLY/FRAGMENT_SHADER, and submission
 * order on the single queue makes that barrier binding for SDL_GPU's
 * subsequent render pass.
 *   default        — fence on OUR OWN submit: same guarantee for our
 *                    copy, no waiting on SDL's work (ladder step 1).
 *   DSVP_ZC_SYNC=1 — restore the full queue drain (old behaviour).
 *   DSVP_ZC_NOSYNC=1 — no post-submit wait at all; the previous
 *                    frame's fence is collected at the top of the
 *                    next upload (ladder step 2 — needs the run; any
 *                    visual difference refutes it, per the doc).
 * Refutation risk is SDL_GPU's private layout tracking, not the
 * Vulkan semantics — hence the one-binary A/B switches. */
static int zc_sync_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        if (SDL_getenv("DSVP_ZC_SYNC"))        mode = 1;
        else if (SDL_getenv("DSVP_ZC_NOSYNC")) mode = 2;
        else if (SDL_getenv("DSVP_ZC_NOCACHE")) mode = 1;
        /* NOCACHE implies the drain: the falsification switch must
         * restore the PRE-CHANGE configuration, which was per-frame
         * import + queue drain. Field 2026-08-21: per-frame import
         * with fence-only sync is a config that never existed and it
         * is pathological — 33% drops with visible jitter on 4K24
         * HDR, vs 12% cache+drain and 3% cache+fence in the same
         * seek-heavy session. The per-frame import path NEEDS the
         * drain's throttling. Explicit DSVP_ZC_SYNC/NOSYNC still
         * override for deliberate experiments. */
        else                                    mode = 0;
    }
    return mode;
}

static void zc_fence_collect(PlayerState *ps) {
    if (!ps->vk_copy_pending)
        return;
    VkResult r = vkWaitForFences(ps->vk_device, 1, &ps->vk_copy_fence,
                                 VK_TRUE, 1000000000ull /* 1s */);
    if (r != VK_SUCCESS)
        log_msg("ZEROCOPY: copy fence wait returned %d", r);
    vkResetFences(ps->vk_device, 1, &ps->vk_copy_fence);
    ps->vk_copy_pending = 0;
}

static int vaapi_zerocopy_upload(PlayerState *ps)
{
    AVFrame *frame = ps->video_frame;

    /* The VAAPI surface ID is stored in data[3] */
    VASurfaceID surface = (VASurfaceID)(uintptr_t)frame->data[3];

    /* Wait for decode to complete */
    VAStatus va_st = vaSyncSurface(ps->va_display, surface);
    if (va_st != VA_STATUS_SUCCESS) {
        log_msg("ZEROCOPY: vaSyncSurface failed: %d", va_st);
        return -1;
    }

    /* Export as DRM_PRIME_2 (gives us DMA-BUF fds + layout info) */
    VADRMPRIMESurfaceDescriptor desc;
    va_st = vaExportSurfaceHandle(ps->va_display, surface,
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &desc);
    if (va_st != VA_STATUS_SUCCESS) {
        log_msg("ZEROCOPY: vaExportSurfaceHandle failed: %d", va_st);
        return -1;
    }

    /* Expect 2 layers: Y (R16) and UV (R16G16) for P010 */
    if (desc.num_layers < 2) {
        log_msg("ZEROCOPY: unexpected layer count: %u", desc.num_layers);
        goto fail_close_fds;
    }

    int w  = ps->vid_w;
    int h  = ps->vid_h;
    /* ceil, matching gpu_tex_uv's creation dims, texSizeUV, and the
     * readback deinterleave (the fdbb489 lesson): floor left the last
     * chroma column/row of the destination unwritten on odd dims.
     * Unreachable today (HEVC 4:2:0 crops in even units and zero-copy
     * is HEVC-only) but kept consistent so it stays a non-bug. */
    int cw = (w + 1) / 2;
    int ch = (h + 1) / 2;

    /* Collect the previous frame's copy fence BEFORE anything that
     * resets the command buffer or destroys images it may still read
     * (the rebuild path below, vkResetCommandBuffer). Under the
     * default sync mode this was already collected after submit and
     * returns instantly; under DSVP_ZC_NOSYNC this is the only wait,
     * and at 24fps the sub-ms copy finished long ago. */
    zc_fence_collect(ps);

    /* ── Import cache lookup (Knot gains #2, Tier A) ──
     * The export above still runs every frame and is the authority on
     * layout: an entry is reused ONLY when surface id, modifiers,
     * offsets, pitches, and object sizes all match; any mismatch
     * destroys and rebuilds. DSVP_ZC_NOCACHE=1 bypasses. */
    int oy = desc.layers[0].object_index[0];
    int ou = desc.layers[1].object_index[0];
    uint64_t mod_y  = desc.objects[oy].drm_format_modifier;
    uint64_t mod_uv = desc.objects[ou].drm_format_modifier;

    static int zc_nocache = -1;
    if (zc_nocache < 0)
        zc_nocache = SDL_getenv("DSVP_ZC_NOCACHE") != NULL;

    struct ZCImportEntry *ent = NULL;
    int cached = 0;
    if (!zc_nocache) {
        struct ZCImportEntry *free_slot = NULL;
        int n = (int)(sizeof(ps->zc_imports) / sizeof(ps->zc_imports[0]));
        for (int i = 0; i < n; i++) {
            if (ps->zc_imports[i].valid &&
                ps->zc_imports[i].surface == (uint32_t)surface) {
                ent = &ps->zc_imports[i];
                break;
            }
            if (!ps->zc_imports[i].valid && !free_slot)
                free_slot = &ps->zc_imports[i];
        }
        if (ent) {
            if (ent->mod_y    == mod_y &&
                ent->mod_uv   == mod_uv &&
                ent->off_y    == desc.layers[0].offset[0] &&
                ent->pitch_y  == desc.layers[0].pitch[0] &&
                ent->off_uv   == desc.layers[1].offset[0] &&
                ent->pitch_uv == desc.layers[1].pitch[0] &&
                ent->size_y   == (size_t)desc.objects[oy].size &&
                ent->size_uv  == (size_t)desc.objects[ou].size) {
                cached = 1;
                ps->zc_cache_hits++;
            } else {
                /* Layout changed under a live surface id — rebuild.
                 * Safe to destroy: zc_fence_collect above guarantees
                 * the previous frame's copy has retired. */
                vkDestroyImage(ps->vk_device, ent->img_y, NULL);
                vkDestroyImage(ps->vk_device, ent->img_uv, NULL);
                vkFreeMemory(ps->vk_device, ent->mem_y, NULL);
                vkFreeMemory(ps->vk_device, ent->mem_uv, NULL);
                ent->valid = 0;
                ps->zc_cache_rebuilds++;
            }
        } else if (free_slot) {
            ent = free_slot;
        }
        /* No slot free (DPB larger than the cache): this frame takes
         * the uncached path below; ent stays NULL. */
    }

    VkImage vk_img_y = VK_NULL_HANDLE;
    VkDeviceMemory vk_mem_y = VK_NULL_HANDLE;
    VkImage vk_img_uv = VK_NULL_HANDLE;
    VkDeviceMemory vk_mem_uv = VK_NULL_HANDLE;

    if (cached) {
        vk_img_y  = ent->img_y;
        vk_mem_y  = ent->mem_y;
        vk_img_uv = ent->img_uv;
        vk_mem_uv = ent->mem_uv;
    } else {
        /* Import Y plane */
        if (import_dmabuf_plane(ps->vk_device,
                desc.objects[oy].fd,
                mod_y,
                VK_FORMAT_R16_UNORM,
                w, h,
                desc.layers[0].offset[0],
                desc.layers[0].pitch[0],
                desc.objects[oy].size,
                &vk_img_y, &vk_mem_y) < 0)
            goto fail_close_fds;

        /* Import UV plane */
        if (import_dmabuf_plane(ps->vk_device,
                desc.objects[ou].fd,
                mod_uv,
                VK_FORMAT_R16G16_UNORM,
                cw, ch,
                desc.layers[1].offset[0],
                desc.layers[1].pitch[0],
                desc.objects[ou].size,
                &vk_img_uv, &vk_mem_uv) < 0) {
            vkDestroyImage(ps->vk_device, vk_img_y, NULL);
            vkFreeMemory(ps->vk_device, vk_mem_y, NULL);
            goto fail_close_fds;
        }

        if (ent) {
            ent->surface  = (uint32_t)surface;
            ent->mod_y    = mod_y;
            ent->mod_uv   = mod_uv;
            ent->off_y    = desc.layers[0].offset[0];
            ent->pitch_y  = desc.layers[0].pitch[0];
            ent->off_uv   = desc.layers[1].offset[0];
            ent->pitch_uv = desc.layers[1].pitch[0];
            ent->size_y   = (size_t)desc.objects[oy].size;
            ent->size_uv  = (size_t)desc.objects[ou].size;
            ent->img_y    = vk_img_y;
            ent->img_uv   = vk_img_uv;
            ent->mem_y    = vk_mem_y;
            ent->mem_uv   = vk_mem_uv;
            ent->valid    = 1;
            ps->zc_cache_misses++;
        }
    }

    /* ── Record copy commands ── */
    VkDevice dev = ps->vk_device;

    vkResetCommandBuffer(ps->vk_cmd_buf, 0);
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(ps->vk_cmd_buf, &begin_info);

    /* Get destination VkImages from SDL_GPU textures */
    VkImage dst_y  = sdl_texture_to_vkimage(ps->gpu_tex_y);
    VkImage dst_uv = sdl_texture_to_vkimage(ps->gpu_tex_uv);

    /* ── Pre-copy barriers ──
     *
     * Source (imported): UNDEFINED → TRANSFER_SRC_OPTIMAL
     *   - Content is valid (DMA-BUF data from VAAPI) but Vulkan needs
     *     the layout transition for cache coherency.
     *   - Also correct for a REUSED cached image: UNDEFINED discards
     *     prior VULKAN contents only; the pixel data lives in the
     *     DMA-BUF and is rewritten by VAAPI outside Vulkan each
     *     decode. Same transition the uncached path performs on every
     *     first use. (Gains #2 acceptance: verify explicitly on a
     *     DRM-modifier tiled surface — that is where "discard" would
     *     have teeth.)
     *
     * Destination (SDL texture): UNDEFINED → TRANSFER_DST_OPTIMAL
     *   - Using UNDEFINED as oldLayout because we're overwriting the
     *     entire image. This avoids needing to know SDL's internal
     *     layout tracking state. */
    VkImageMemoryBarrier pre_barriers[4] = {
        /* Source Y */
        { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = 0,
          .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = vk_img_y,
          .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
        /* Source UV */
        { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = 0,
          .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = vk_img_uv,
          .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
        /* Dest Y */
        { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
          .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = dst_y,
          .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
        /* Dest UV */
        { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
          .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = dst_uv,
          .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
    };
    vkCmdPipelineBarrier(ps->vk_cmd_buf,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 4, pre_barriers);

    /* ── Copy Y plane ── */
    VkImageCopy copy_y = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffset = { 0, 0, 0 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffset = { 0, 0, 0 },
        .extent = { w, h, 1 },
    };
    vkCmdCopyImage(ps->vk_cmd_buf,
        vk_img_y,  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst_y,     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &copy_y);

    /* ── Copy UV plane ── */
    VkImageCopy copy_uv = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffset = { 0, 0, 0 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffset = { 0, 0, 0 },
        .extent = { cw, ch, 1 },
    };
    vkCmdCopyImage(ps->vk_cmd_buf,
        vk_img_uv, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst_uv,    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &copy_uv);

    /* ── Post-copy barriers: transition destinations back to shader read ── */
    VkImageMemoryBarrier post_barriers[2] = {
        { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = dst_y,
          .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
        { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = dst_uv,
          .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
    };
    vkCmdPipelineBarrier(ps->vk_cmd_buf,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL, 2, post_barriers);

    vkEndCommandBuffer(ps->vk_cmd_buf);

    /* ── Submit copy to same queue as SDL_GPU (ordered by queue) ── */
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &ps->vk_cmd_buf,
    };
    VkResult vr = vkQueueSubmit(ps->vk_queue, 1, &submit_info,
                                ps->vk_copy_fence);
    if (vr == VK_SUCCESS)
        ps->vk_copy_pending = 1;

    /* ── Copy-completion sync, per the ladder (see zc_sync_mode) ── */
    if (vr == VK_SUCCESS) {
        int mode = zc_sync_mode();
        if (mode == 1)
            vkQueueWaitIdle(ps->vk_queue);   /* old full drain (A/B) */
        else if (mode == 0)
            zc_fence_collect(ps);            /* our copy only        */
        /* mode 2 (NOSYNC): collected at the top of the next upload */
    }

    /* ── Cleanup imported resources ──
     * Cache-owned imports stay alive for the next frame with this
     * surface; only uncached (slotless) imports are destroyed here. */
    int owned = (ent && ent->valid && ent->img_y == vk_img_y);
    if (!owned) {
        /* This frame's copy reads these images — under NOSYNC the
         * fence has not been collected yet, so collect before the
         * destroy. Rare by construction (slotless frame). */
        zc_fence_collect(ps);
        vkDestroyImage(dev, vk_img_y, NULL);
        vkDestroyImage(dev, vk_img_uv, NULL);
        vkFreeMemory(dev, vk_mem_y, NULL);
        vkFreeMemory(dev, vk_mem_uv, NULL);
    }

    /* Close DMA-BUF fds (vaExportSurfaceHandle opened them; the cache
     * keeps its own references via the dup'd fds Vulkan owns) */
    for (uint32_t i = 0; i < desc.num_objects; i++)
        close(desc.objects[i].fd);

    if (vr != VK_SUCCESS) {
        log_msg("ZEROCOPY: vkQueueSubmit failed: %d", vr);
        return -1;
    }

    return 0;

fail_close_fds:
    for (uint32_t i = 0; i < desc.num_objects; i++)
        close(desc.objects[i].fd);
    return -1;
}



/* ═══════════════════════════════════════════════════════════════════
 * Open / Close
 * ═══════════════════════════════════════════════════════════════════ */

/* FFmpeg interrupt callback — allows aborting blocked I/O.
 * Called periodically by FFmpeg during av_read_frame, av_seek_frame,
 * and avformat_open_input. Returns 1 to abort, 0 to continue.
 * Without this, reads from stale NFS mounts block indefinitely. */
static int io_interrupt_cb(void *opaque) {
    PlayerState *ps = (PlayerState *)opaque;
    if (ps->quit || ps->closing) return 1;
    if (ps->io_deadline > 0.0 && get_time_sec() > ps->io_deadline) {
        log_msg("I/O timeout — aborting blocked read");
        return 1;
    }
    return 0;
}

/* Open a media file: probe format, find best streams, init decoders,
 * set up scaling context, create GPU textures, start demux thread. */
int player_open(PlayerState *ps, const char *filename) {
    int ret;

    strncpy(ps->filepath, filename, sizeof(ps->filepath) - 1);
    ps->io_error = 0;
    ps->vaapi_unsupported = 0;
    ps->res_change_logged = 0;
    ps->frame_tex_valid = 0;      /* stale content from previous file */
    ps->filepath[sizeof(ps->filepath) - 1] = '\0';
    if (log_anon_active()) {
        /* Redact file path — show only codec-relevant info for public logs */
        const char *ext = strrchr(filename, '.');
        log_msg("player_open: [redacted]%s", ext ? ext : "");
    } else {
        log_msg("player_open: %s", filename);
    }

    /* ── Open container ── */
    ps->fmt_ctx = avformat_alloc_context();
    if (!ps->fmt_ctx) {
        log_msg("ERROR: avformat_alloc_context failed");
        return -1;
    }
    ps->fmt_ctx->interrupt_callback.callback = io_interrupt_cb;
    ps->fmt_ctx->interrupt_callback.opaque   = ps;

    /* "No networking whatsoever" is enforced, not just claimed: the
     * bundled FFmpeg has network protocols compiled in, so without this
     * whitelist a URL argument would happily demux over the network
     * (DSVP main fdbb489). A shim session (DSVP_SHIM=1, set only by the
     * shim daemon that launched us) opens the one HTTP stream it was
     * handed; the cold-start default stays file-only. */
    AVDictionary *open_opts = NULL;
    av_dict_set(&open_opts, "protocol_whitelist",
                getenv("DSVP_SHIM") ? "file,http,tcp" : "file", 0);
    /* The protocol whitelist alone leaves the full demuxer set
     * reachable — including hls and concat, which take sub-resource
     * references from the file's OWN CONTENT. is_media_file() filters
     * by extension but avformat probes by content, so a hostile file
     * named .mkv whose bytes begin #EXTM3U opened as HLS, and with
     * file whitelisted its playlist entries could name arbitrary
     * local paths (Knot audit finding 8). Whitelist exactly the
     * demuxers behind video_extensions[] in main.c — keep the two
     * lists in step. This also makes the no-networking claim true at
     * the demuxer layer, not just the protocol layer. */
    av_dict_set(&open_opts, "format_whitelist",
                "matroska,webm,mov,mp4,m4a,3gp,3g2,mj2,avi,asf,flv,"
                "mpegts,mpeg", 0);
    /* Stream auth rides an HTTP header the daemon provides, never the
     * URL — a token in the URL is a token in this very log file. */
    const char *shim_hdrs = getenv("DSVP_SHIM_HEADERS");
    if (getenv("DSVP_SHIM") && shim_hdrs && shim_hdrs[0])
        av_dict_set(&open_opts, "headers", shim_hdrs, 0);
    ps->io_deadline = get_time_sec() + 10.0;
    ret = avformat_open_input(&ps->fmt_ctx, filename, NULL, &open_opts);
    ps->io_deadline = 0.0;
    av_dict_free(&open_opts);
    if (ret < 0) {
        log_msg("ERROR: avformat_open_input failed: %s", av_err2str(ret));
        return -1;
    }

    ps->io_deadline = get_time_sec() + 10.0;
    ret = avformat_find_stream_info(ps->fmt_ctx, NULL);
    ps->io_deadline = 0.0;
    if (ret < 0) {
        log_msg("ERROR: avformat_find_stream_info failed: %s", av_err2str(ret));
        avformat_close_input(&ps->fmt_ctx);
        return -1;
    }
    log_msg("Container: %s (%s), streams=%d",
        ps->fmt_ctx->iformat->name, ps->fmt_ctx->iformat->long_name,
        ps->fmt_ctx->nb_streams);

    /* ── Find best video stream ── */
    ps->video_stream_idx = av_find_best_stream(ps->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    ps->audio_stream_idx = av_find_best_stream(ps->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, ps->video_stream_idx, NULL, 0);

    /* ── Skip TrueHD audio when not in bitstream passthrough ──
     *
     * TrueHD Atmos 7.1 MLP decode is extremely CPU-heavy and starves the
     * video pipeline on complex files (4K HEVC 10-bit + 29 streams).
     * Without an AVR/soundbar via HDMI, it just gets crushed to S16 stereo
     * anyway — pointless pain. Every Blu-ray with TrueHD ships an AC3 or
     * EAC3 compatibility track. Pick that instead.
     * When bitstream passthrough is active, TrueHD packets go straight to
     * the HDMI wire without decoding — no CPU cost, lossless output. */
    /* Probe the sink BEFORE the TrueHD-selection decision: on the
     * first file of a session the caps cache is empty, support_truehd
     * reads false, and the Atmos track gets silently demoted to the
     * compatibility track (field case 2026-08-09: Civil War never
     * attempted TrueHD passthrough; the second file of the session
     * worked because the first had populated the cache). */
    if (ps->audio_mode != AUDIO_MODE_PCM && !ps->bitstream_caps.probed)
        bitstream_probe(ps);
    if (ps->audio_stream_idx >= 0) {
        AVStream *as = ps->fmt_ctx->streams[ps->audio_stream_idx];
        if (as->codecpar->codec_id == AV_CODEC_ID_TRUEHD &&
            (ps->audio_mode == AUDIO_MODE_PCM ||
             !ps->bitstream_caps.support_truehd ||
             bitstream_hd_blocked(AV_CODEC_ID_TRUEHD,
                                  as->codecpar->sample_rate))) {
            log_msg("Audio: default stream is TrueHD — skipping (PCM mode or passthrough inactive)");
            int fallback = -1;
            for (unsigned i = 0; i < ps->fmt_ctx->nb_streams; i++) {
                AVStream *st = ps->fmt_ctx->streams[i];
                if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
                if ((int)i == ps->audio_stream_idx) continue;
                if (st->codecpar->codec_id == AV_CODEC_ID_TRUEHD) continue;
                fallback = (int)i;
                break;
            }
            if (fallback >= 0) {
                const AVCodec *fc = avcodec_find_decoder(
                    ps->fmt_ctx->streams[fallback]->codecpar->codec_id);
                log_msg("Audio: fallback to stream %d (%s)",
                    fallback, fc ? fc->name : "unknown");
                ps->audio_stream_idx = fallback;
            } else {
                log_msg("Audio: no non-TrueHD fallback found — playing without audio");
                ps->audio_stream_idx = -1;
            }
        }
    }

    if (ps->video_stream_idx < 0) {
        log_msg("ERROR: No video stream found");
        avformat_close_input(&ps->fmt_ctx);
        return -1;
    }
    log_msg("Video stream: idx=%d, Audio stream: idx=%d",
        ps->video_stream_idx, ps->audio_stream_idx);

    /* ── Open video decoder ── */
    {
        AVStream *vs = ps->fmt_ctx->streams[ps->video_stream_idx];
        const AVCodec *codec = NULL;

        /* FFmpeg 8.1's generic 'av1' decoder probes for hardware accel
         * first and fails catastrophically on systems without AV1 HW
         * decode (spams "Failed to get pixel format", zero frames output).
         * Force libdav1d — it's pure software, always works, and is the
         * reference AV1 decoder. */
        if (vs->codecpar->codec_id == AV_CODEC_ID_AV1) {
            codec = avcodec_find_decoder_by_name("libdav1d");
            if (codec)
                log_msg("Video codec: libdav1d forced for AV1 (avoiding hw probe)");
        }
        if (!codec)
            codec = avcodec_find_decoder(vs->codecpar->codec_id);
        if (!codec) {
            log_msg("ERROR: Unsupported video codec id=%d", vs->codecpar->codec_id);
            avformat_close_input(&ps->fmt_ctx);
            return -1;
        }
        if (vs->codecpar->codec_id != AV_CODEC_ID_AV1)
            log_msg("Video codec: %s (%s)", codec->name, codec->long_name);

        ps->video_codec_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(ps->video_codec_ctx, vs->codecpar);

        /* ── VAAPI hardware decode (Linux, HEVC only) ──
         *
         * VAAPI is used exclusively for HEVC because software decode can't
         * sustain 4K HEVC 10-bit at 24fps on the Steam Deck's Zen 2.
         * H.264 4K 60fps plays perfectly with 4-thread software decode.
         *
         * VAAPI decode is bit-exact (same output as software) — no fidelity
         * compromise. Output is P010LE (10-bit) for 10-bit sources or
         * NV12 (8-bit) for 8-bit sources. Both are semi-planar, which we
         * deinterleave on CPU and feed the existing R16_UNORM/R8_UNORM pipeline.
         *
         * DSVP_HWDEC=0 disables hardware decode (for testing/comparison).
         */
        int use_vaapi = 0;
#ifdef __linux__
        {
            const char *hwdec_env = getenv("DSVP_HWDEC");
            int hwdec_disabled = (hwdec_env && hwdec_env[0] == '0')
                              || ps->force_swdec;  /* P2-17 retry */
            enum AVCodecID cid = vs->codecpar->codec_id;

            if (cid == AV_CODEC_ID_HEVC && !hwdec_disabled) {
                /* Try to create VAAPI device context */
                if (!ps->hw_device_ctx) {
                    ret = av_hwdevice_ctx_create(&ps->hw_device_ctx,
                        AV_HWDEVICE_TYPE_VAAPI,
                        "/dev/dri/renderD128", NULL, 0);
                    if (ret < 0) {
                        log_msg("VAAPI: device init failed (%s) — software fallback",
                                av_err2str(ret));
                        ps->hw_device_ctx = NULL;
                    }
                }

                if (ps->hw_device_ctx) {
                    ps->video_codec_ctx->hw_device_ctx =
                        av_buffer_ref(ps->hw_device_ctx);
                    ps->video_codec_ctx->get_format = vaapi_get_format;
                    ps->video_codec_ctx->opaque = ps;  /* for get_format */
                    use_vaapi = 1;
                    log_msg("VAAPI: attempting hardware decode for HEVC");
                }
            } else if (hwdec_disabled && cid == AV_CODEC_ID_HEVC) {
                log_msg("VAAPI: disabled by DSVP_HWDEC=0");

            }
        }
#endif /* __linux__ */

        if (!use_vaapi) {
            /* Software decode — adaptive thread count for Deck
             *
             * Heuristic derived from Steam Deck (Zen 2 4C/8T) OLED benchmarks:
             *   HEVC ≤30fps → 1 thread  (Dogma 4K HEVC 10-bit: 1.05% at 1T)
             *   H.264 ≥50fps → 4 threads (French Kiss 4K 60fps: clean)
             *   Everything else → 2 threads (safe default)
             *
             * DSVP_THREADS env var overrides the heuristic (0 = FFmpeg auto).
             */
            int tcount;
            const char *tenv = getenv("DSVP_THREADS");
            if (tenv && tenv[0] != '\0') {
                tcount = atoi(tenv);
                log_msg("Thread selection: DSVP_THREADS=%d (env override)", tcount);
            } else {
                double fps = 0.0;
                if (vs->avg_frame_rate.den > 0)
                    fps = av_q2d(vs->avg_frame_rate);

                enum AVCodecID cid = vs->codecpar->codec_id;

                if (cid == AV_CODEC_ID_HEVC) {
                    tcount = 1;  /* software fallback only (VAAPI handles normal path) */
                } else {
                    /* 8 threads, full stop. A >40fps→6T gate was tried
                     * and FALSIFIED 2026-08-20 (same 1080p60 file, same
                     * session: 6T=7.49%, 8T=3.45% — ce4c466 A/B).
                     * Race-to-idle wins on the power-shared APU:
                     * finishing decode fast and sleeping beats
                     * trickling on fewer cores. DSVP_THREADS=N above
                     * remains the experiment knob. */
                    tcount = 8;
                }
                log_msg("Thread selection: codec=%s fps=%.2f → %d threads (adaptive)",
                        avcodec_get_name(cid), fps, tcount);
            }
            ps->video_codec_ctx->thread_count = tcount;
            ps->video_codec_ctx->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;
        }


        ret = avcodec_open2(ps->video_codec_ctx, codec, NULL);
        if (ret < 0) {
            if (use_vaapi) {
                /* VAAPI open failed — retry with software decode */
                log_msg("VAAPI: avcodec_open2 failed (%s) — retrying software",
                        av_err2str(ret));
                avcodec_free_context(&ps->video_codec_ctx);
                ps->video_codec_ctx = avcodec_alloc_context3(codec);
                avcodec_parameters_to_context(ps->video_codec_ctx, vs->codecpar);
                ps->video_codec_ctx->thread_count = 1;
                ps->video_codec_ctx->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;
                use_vaapi = 0;
                ret = avcodec_open2(ps->video_codec_ctx, codec, NULL);
            }
            if (ret < 0) {
                fprintf(stderr, "[DSVP] Cannot open video codec: %s\n", av_err2str(ret));
                /* Free the codec context here: player_close's
                 * (!playing && !fmt_ctx) guard makes it unreachable
                 * after this return, and the next open would orphan
                 * it (the audio path below already does this). */
                avcodec_free_context(&ps->video_codec_ctx);
                avformat_close_input(&ps->fmt_ctx);
                return -1;
            }
        }

        ps->vaapi_active = use_vaapi;
        ps->vid_w = ps->video_codec_ctx->width;
        ps->vid_h = ps->video_codec_ctx->height;

        if (use_vaapi) {
            /* Determine VAAPI output format from source bit depth.
             * VAAPI outputs NV12 (8-bit uint8) for 8-bit HEVC,
             * P010 (10-bit uint16) for 10-bit HEVC. The stream's
             * codecpar->format is the original pixel format before
             * hardware acceleration — reliable and available now. */
            enum AVPixelFormat stream_fmt = vs->codecpar->format;
            ps->vaapi_nv12 = (stream_fmt != AV_PIX_FMT_YUV420P10LE
                           && stream_fmt != AV_PIX_FMT_P010LE);
            log_msg("VAAPI: active — HEVC hardware decode, %s output",
                    ps->vaapi_nv12 ? "NV12 (8-bit)" : "P010 (10-bit)");
            log_msg("Video: %dx%d, pix_fmt=%s (sw=%s), stream_fmt=%s",
                ps->vid_w, ps->vid_h,
                av_get_pix_fmt_name(ps->video_codec_ctx->pix_fmt),
                av_get_pix_fmt_name(ps->video_codec_ctx->sw_pix_fmt),
                av_get_pix_fmt_name(stream_fmt));

            /* ── VAAPI zero-copy init (P010 10-bit only) ──
             * NV12 (8-bit) is fast enough with readback (~2ms).
             * P010 (10-bit 4K) is the bottleneck at 35-42ms. */
            if (!ps->vaapi_nv12) {
                if (vaapi_zerocopy_init(ps) == 0) {
                    log_msg("VAAPI: zero-copy enabled for P010");
                } else {
                    log_msg("VAAPI: zero-copy unavailable — using readback");
                }
            }
        } else {
            log_msg("Video: %dx%d, pix_fmt=%s, threads=%d",
                ps->vid_w, ps->vid_h,
                av_get_pix_fmt_name(ps->video_codec_ctx->pix_fmt),
                ps->video_codec_ctx->thread_count);
        }
    }

    /* ── Open audio decoder ── */
    if (ps->audio_stream_idx >= 0) {
        AVStream *as = ps->fmt_ctx->streams[ps->audio_stream_idx];
        const AVCodec *codec = avcodec_find_decoder(as->codecpar->codec_id);
        if (codec) {
            ps->audio_codec_ctx = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(ps->audio_codec_ctx, as->codecpar);
            ps->audio_codec_ctx->thread_count = 0;
            ret = avcodec_open2(ps->audio_codec_ctx, codec, NULL);
            if (ret < 0) {
                fprintf(stderr, "[DSVP] Cannot open audio codec: %s\n", av_err2str(ret));
                avcodec_free_context(&ps->audio_codec_ctx);
                ps->audio_stream_idx = -1;
            }
        } else {
            ps->audio_stream_idx = -1;
        }
    }

    /* ── Find subtitle streams ── */
    sub_find_streams(ps);

    /* ── Find audio streams ── */
    audio_find_streams(ps);

    /* ── Discard unused streams (reduces demux I/O) ──
     *
     * Tell the demuxer to skip packets for streams we won't decode.
     * Saves I/O on files with many streams (e.g. Dogma: 29 streams).
     * Also eliminates DV dual-layer enhancement layer overhead if present.
     */
    {
        int discarded = 0;
        int dv_el_found = 0;

        for (unsigned i = 0; i < ps->fmt_ctx->nb_streams; i++) {
            AVStream *st = ps->fmt_ctx->streams[i];
            int dominated_by_stream = (int)i;

            /* Keep the selected video and audio streams */
            if (dominated_by_stream == ps->video_stream_idx) continue;
            if (dominated_by_stream == ps->audio_stream_idx) continue;

            /* Keep all cataloged subtitle streams */
            int is_sub = 0;
            for (int s = 0; s < ps->sub_count; s++) {
                if (dominated_by_stream == ps->sub_stream_indices[s]) {
                    is_sub = 1;
                    break;
                }
            }
            if (is_sub) continue;

            /* Keep all cataloged audio streams (for audio cycling) */
            int is_aud = 0;
            for (int a = 0; a < ps->aud_count; a++) {
                if (dominated_by_stream == ps->aud_stream_indices[a]) {
                    is_aud = 1;
                    break;
                }
            }
            if (is_aud) continue;

            /* Check if this is a DV enhancement layer video stream */
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                dominated_by_stream != ps->video_stream_idx) {
                dv_el_found = 1;
                log_msg("Stream %d: DV enhancement layer video — discarding", dominated_by_stream);
            }

            st->discard = AVDISCARD_ALL;
            discarded++;
        }

        if (discarded > 0)
            log_msg("Demux: discarding %d unused stream(s) to reduce I/O", discarded);

        /* Log DV detection from codec tag (dvhe/dvh1 = DV HEVC single-layer) */
        if (ps->video_stream_idx >= 0) {
            AVStream *vs = ps->fmt_ctx->streams[ps->video_stream_idx];
            unsigned int tag = vs->codecpar->codec_tag;
            if (tag == MKTAG('d','v','h','e') || tag == MKTAG('d','v','h','1') ||
                tag == MKTAG('d','v','a','v') || tag == MKTAG('d','v','a','1')) {
                log_msg("Dolby Vision detected (tag=%.4s) — base layer decode only (no HDR pipeline)",
                        (const char *)&tag);
            } else if (dv_el_found) {
                log_msg("Dolby Vision dual-layer detected — enhancement layer discarded");
            }
        }
    }

    /* ── Allocate decode frames ── */
    ps->video_frame = av_frame_alloc();
    ps->rgb_frame   = av_frame_alloc();
    ps->audio_frame = av_frame_alloc();

    /* ── VAAPI: allocate hw→sw transfer frame and UV deinterleave buffers ── */
    if (ps->vaapi_active) {
        ps->hw_frame = av_frame_alloc();
        if (!ps->hw_frame) {
            log_msg("ERROR: Failed to allocate hw_frame");
            player_close(ps);
            return -1;
        }
        /* Semi-planar UV plane: interleaved sample pairs → split into U and V.
         * NV12: uint8 pairs (1 byte each), P010: uint16 pairs (2 bytes each).
         * ceil — matches texture + FFmpeg chroma allocation for odd dims. */
        int cw = (ps->vid_w + 1) / 2;
        int ch = (ps->vid_h + 1) / 2;
        int sample_bytes = ps->vaapi_nv12 ? 1 : 2;
        ps->p010_u_plane = (uint8_t *)av_malloc((size_t)cw * ch * sample_bytes);
        ps->p010_v_plane = (uint8_t *)av_malloc((size_t)cw * ch * sample_bytes);
        if (!ps->p010_u_plane || !ps->p010_v_plane) {
            log_msg("ERROR: Failed to allocate deinterleave buffers");
            player_close(ps);
            return -1;
        }
        log_msg("VAAPI: allocated %s deinterleave buffers (%dx%d chroma)",
                ps->vaapi_nv12 ? "NV12" : "P010", cw, ch);
    }

    /* ── Set up swscale (or skip for GPU passthrough) ──
     *
     * VAAPI NV12:    bypass swscale. CPU deinterleave UV → R8_UNORM textures.
     * VAAPI P010:    bypass swscale. CPU deinterleave UV → R16_UNORM textures.
     * yuv420p10le:   bypass swscale. Raw 10-bit planes → R16_UNORM textures.
     * yuv420p:       bypass swscale. Raw 8-bit planes → R8_UNORM textures.
     *                Range expansion (limited→full) done in fragment shader.
     *
     * All other pixel formats need swscale conversion to YUV420P first.
     * Shader handles the color matrix and any remaining range work.
     */
    {
        enum AVPixelFormat src_fmt = ps->video_codec_ctx->pix_fmt;
        int is_10bit  = (src_fmt == AV_PIX_FMT_YUV420P10LE);
        int is_yuv420p = (src_fmt == AV_PIX_FMT_YUV420P);

        if (ps->vaapi_active) {
            /* ── VAAPI — deinterleave on CPU, range in shader ── */
            ps->sws_ctx    = NULL;
            ps->rgb_buffer = NULL;
            log_msg("swscale: bypassed (VAAPI %s → CPU deinterleave → %s)",
                    ps->vaapi_nv12 ? "NV12" : "P010",
                    ps->vaapi_nv12 ? "R8_UNORM" : "R16_UNORM");

        } else if (is_10bit) {
            /* ── 10-bit GPU passthrough — no swscale needed ── */
            ps->sws_ctx    = NULL;
            ps->rgb_buffer = NULL;
            log_msg("swscale: bypassed (10-bit GPU passthrough)");

        } else if (is_yuv420p) {
            /* ── 8-bit YUV420P passthrough — range in shader ── */
            ps->sws_ctx    = NULL;
            ps->rgb_buffer = NULL;
            log_msg("swscale: bypassed (8-bit YUV420P, range in shader)");

        } else {
            /* ── swscale path for all other formats ──
             * Deep sources (12-bit HEVC, 10-bit AV1/VP9, 10-bit
             * 4:2:2/4:4:4) convert to yuv420p10le and ride the R16
             * upload path — converting to 8-bit here quantized the PQ
             * signal to 256 codes BEFORE the EETF stretched it,
             * guaranteeing shadow banding that no output dither can
             * repair. 8-bit sources keep the 8-bit destination.
             * (DSVP main df16dc8.) */
            const AVPixFmtDescriptor *src_desc = av_pix_fmt_desc_get(src_fmt);
            int src_depth = src_desc ? src_desc->comp[0].depth : 8;
            ps->sws_out_10bit = (src_depth > 8);
            enum AVPixelFormat dst_fmt = ps->sws_out_10bit
                ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
            int dst_w = ps->vid_w;
            int dst_h = ps->vid_h;

            int sws_flags = SWS_LANCZOS | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
            const char *sws_mode = ps->sws_out_10bit
                ? "format convert to 10-bit (SWS_LANCZOS + ED dither)"
                : "format convert (SWS_LANCZOS + ED dither)";

            ps->sws_ctx = sws_getContext(
                ps->vid_w, ps->vid_h, src_fmt,
                dst_w, dst_h, dst_fmt,
                sws_flags,
                NULL, NULL, NULL
            );

            if (!ps->sws_ctx) {
                log_msg("ERROR: Cannot create swscale context");
                player_close(ps);
                return -1;
            }

            /* Error-diffusion dithering for format conversions.
             * Was av_opt_set_int("dithering", 1, 0) — value 1 is
             * SWS_DITHER_AUTO, not ED (ED is 3), so the startup log
             * and debug overlay claimed a dither the code never asked
             * for (Knot audit finding 12). Set by option/constant
             * name, with the enum value as fallback, and say so if
             * neither lands rather than claiming ED anyway. */
            if (av_opt_set(ps->sws_ctx, "sws_dither", "ed", 0) < 0 &&
                av_opt_set_int(ps->sws_ctx, "dithering", 3, 0) < 0)
                log_msg("WARN: swscale ED dither not set — "
                        "library default in effect");

            /* ── Colorspace and range ── */
            {
                AVCodecParameters *par = ps->fmt_ctx->streams[ps->video_stream_idx]->codecpar;

                int src_cs;
                if (par->color_space != AVCOL_SPC_UNSPECIFIED) {
                    src_cs = (par->color_space == AVCOL_SPC_BT709)
                        ? SWS_CS_ITU709 : SWS_CS_ITU601;
                } else {
                    src_cs = (ps->vid_h >= 720) ? SWS_CS_ITU709 : SWS_CS_ITU601;
                }

                int dst_cs = src_cs;

                int src_range;
                if (par->color_range == AVCOL_RANGE_JPEG) {
                    src_range = 1;
                } else if (par->color_range == AVCOL_RANGE_MPEG) {
                    src_range = 0;
                } else {
                    src_range = 0;
                }
                /* 8-bit destination: PRESERVE the source range and
                 * let the shader's exact 8-bit branch expand, like
                 * every passthrough path. dst_range=1 here stretched
                 * 219 luma levels onto 256 in integer space — a
                 * non-invertible rounding applied before all the
                 * careful downstream work, on the one path with the
                 * least eye time (Knot audit finding 9). The 10-bit
                 * destination keeps the full-range stretch: it happens
                 * with 4 bits of headroom, where it is harmless. */
                int dst_range = ps->sws_out_10bit ? 1 : src_range;

                int *inv_table, *table;
                int cur_src_range, cur_dst_range, brightness, contrast, saturation;
                sws_getColorspaceDetails(ps->sws_ctx,
                    &inv_table, &cur_src_range, &table, &cur_dst_range,
                    &brightness, &contrast, &saturation);

                sws_setColorspaceDetails(ps->sws_ctx,
                    sws_getCoefficients(src_cs), src_range,
                    sws_getCoefficients(dst_cs), dst_range,
                    brightness, contrast, saturation);

                log_msg("swscale: colorspace=%s range=%s->%s",
                    (src_cs == SWS_CS_ITU709) ? "BT.709" : "BT.601",
                    src_range ? "full" : "limited",
                    dst_range ? "full" : "limited");
            }

            /* ── Chroma siting ──
             * Pin BOTH sides explicitly and remember the output siting.
             * The old pin-dst-to-center approach assumed sws always
             * re-sites chroma, but same-geometry conversions (420 depth
             * changes — the common case once deep sources keep 10-bit)
             * take unscaled per-plane converters that move nothing: the
             * output keeps the SOURCE siting. Rule: 4:2:0 sources keep
             * their siting (no resample happens or is needed); formats
             * that genuinely resample chroma (422/444/RGB → 420) are
             * pinned to LEFT, the H.264/HEVC convention. The shader
             * offset is then derived from sws_dst_siting instead of
             * being zeroed. (DSVP main df16dc8.) */
            {
                AVCodecParameters *par = ps->fmt_ctx->streams[ps->video_stream_idx]->codecpar;
                enum AVChromaLocation src_loc = par->chroma_location;
                if (src_loc == AVCHROMA_LOC_UNSPECIFIED)
                    src_loc = AVCHROMA_LOC_LEFT;

                /* AVChromaLocation → swscale chr_pos (1/256 luma units):
                 * h: left=0 center=128; v: top=0 center=128 bottom=256 */
                static const struct { int h, v; } chr_pos[] = {
                    [AVCHROMA_LOC_LEFT]       = {   0, 128 },
                    [AVCHROMA_LOC_CENTER]     = { 128, 128 },
                    [AVCHROMA_LOC_TOPLEFT]    = {   0,   0 },
                    [AVCHROMA_LOC_TOP]        = { 128,   0 },
                    [AVCHROMA_LOC_BOTTOMLEFT] = {   0, 256 },
                    [AVCHROMA_LOC_BOTTOM]     = { 128, 256 },
                };
                int src_is_420 = src_desc
                    && src_desc->log2_chroma_w == 1
                    && src_desc->log2_chroma_h == 1;
                enum AVChromaLocation dst_loc =
                    src_is_420 ? src_loc : AVCHROMA_LOC_LEFT;

                av_opt_set_int(ps->sws_ctx, "src_h_chr_pos", chr_pos[src_loc].h, 0);
                av_opt_set_int(ps->sws_ctx, "src_v_chr_pos", chr_pos[src_loc].v, 0);
                av_opt_set_int(ps->sws_ctx, "dst_h_chr_pos", chr_pos[dst_loc].h, 0);
                av_opt_set_int(ps->sws_ctx, "dst_v_chr_pos", chr_pos[dst_loc].v, 0);
                ps->sws_dst_siting = (int)dst_loc;

                log_msg("swscale: chroma siting src=%s dst=%s",
                        av_chroma_location_name(src_loc),
                        av_chroma_location_name(dst_loc));
            }

            log_msg("swscale: mode=%s", sws_mode);

            /* Allocate buffer for the converted frame */
            int buf_size = av_image_get_buffer_size(dst_fmt, dst_w, dst_h, 32);
            ps->rgb_buffer = av_malloc(buf_size);
            av_image_fill_arrays(ps->rgb_frame->data, ps->rgb_frame->linesize,
                                 ps->rgb_buffer, dst_fmt, dst_w, dst_h, 32);
        }
    }

    /* ── Resize window to video dimensions ── */
    {
        if (!ps->fullscreen) {
            /* Cap to 80% of screen, maintain aspect ratio */
            const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(
                SDL_GetPrimaryDisplay());
            int max_w = dm ? (int)(dm->w * 0.8) : 1920;
            int max_h = dm ? (int)(dm->h * 0.8) : 1080;

            int w = ps->vid_w;
            int h = ps->vid_h;

            if (w > max_w || h > max_h) {
                double scale = fmin((double)max_w / w, (double)max_h / h);
                w = (int)(w * scale);
                h = (int)(h * scale);
            }

            ps->win_w = w;
            ps->win_h = h;

            SDL_SetWindowSize(ps->window, w, h);
            SDL_SyncWindow(ps->window);  /* let WM process resize before centering */
            SDL_SetWindowPosition(ps->window,
                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        } else {
            /* Fullscreen: don't resize, just read actual dimensions */
            int fw, fh;
            SDL_GetWindowSize(ps->window, &fw, &fh);
            ps->win_w = fw;
            ps->win_h = fh;
        }

        /* Update window title with filename */
        const char *basename = strrchr(filename, '/');
        if (!basename) basename = strrchr(filename, '\\');
        basename = basename ? basename + 1 : filename;
        char title[512];
        snprintf(title, sizeof(title), "DSVP — %s", basename);
        SDL_SetWindowTitle(ps->window, title);
    }

    /* ── Create GPU textures and transfer buffers ── */
    if (gpu_create_video_textures(ps) < 0) {
        log_msg("ERROR: GPU texture creation failed");
        player_close(ps);
        return -1;
    }

    /* ── get_buffer2 zero-copy pool (8-bit yuv420p SW decode only;
     * installs the callback on the codec when it activates, before
     * the decode thread exists) ── */
    xfer_pool_create(ps);

    /* ── Set up GPU color uniforms ── */
    /* ── Intermediate-texture aptness (per file) ──
     * The render-at-content-rate intermediate only pays when content
     * rate sits below display refresh — that's when redundant
     * re-renders exist to skip (24p on 60Hz: 36 of 60). At content ==
     * refresh (4K60 SDR) it saves zero renders and its ~6GB/s blit
     * bandwidth pushed the saturated UMA over the edge: present fell
     * to ~40. Rule: intermediate iff content fps < 75% of refresh —
     * 24/25/30 qualify, 48/50/60 render direct. Keyed on frame rate,
     * NOT the HDR flag: 24p SDR film (the bulk of the library) keeps
     * the benefit, and any 60fps HDR renders direct like it must. */
    {
        double content_fps = 0.0;
        AVStream *vs = ps->fmt_ctx->streams[ps->video_stream_idx];
        if (vs->avg_frame_rate.den > 0)
            content_fps = av_q2d(vs->avg_frame_rate);

        double refresh = 60.0;
        /* The WINDOW's display, not the primary: on a docked deck the
         * primary can be the (disabled) internal panel, and its refresh
         * would drive the wrong intermediate/direct decision for the
         * TV. (Known gap: decided once per file open; a mid-file
         * display change keeps the old verdict.) */
        SDL_DisplayID disp = SDL_GetDisplayForWindow(ps->window);
        if (!disp) disp = SDL_GetPrimaryDisplay();
        const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(disp);
        if (dm && dm->refresh_rate > 0.0f)
            refresh = (double)dm->refresh_rate;

        ps->intermediate_apt =
            (content_fps > 0.0 && content_fps < refresh * 0.75);
        log_msg("Render: content %.2f fps vs %.0f Hz refresh — %s",
                content_fps, refresh,
                ps->intermediate_apt
                    ? "intermediate (render at content rate)"
                    : "direct (content at/near refresh — nothing to save)");
    }

    gpu_setup_uniforms(ps);
    hdr_output_apply(ps);   /* engage/revert HDR swapchain per content */

    /* ── Init packet queues ── */
    pq_init(&ps->video_pq);
    pq_init(&ps->audio_pq);
    for (int i = 0; i < ps->sub_count; i++)
        pq_init(&ps->sub_pqs[i]);

    /* ── Seek mutex (protects codec flush vs decode) ── */
    ps->seek_mutex = SDL_CreateMutex();
    ps->seeking    = 0;

    /* ── Decode thread setup ── */
    ps->decoded_frame = av_frame_alloc();
    if (!ps->decoded_frame) {
        log_msg("ERROR: Failed to allocate decoded_frame");
        player_close(ps);
        return -1;
    }
    ps->decode_mutex = SDL_CreateMutex();
    ps->decode_cond  = SDL_CreateCondition();
    ps->decode_frame_ready = 0;
    ps->decode_eof = 0;

    /* ── Init timing ── */
    ps->frame_timer      = get_time_sec();
    ps->frame_last_delay = 0.04;   /* assume ~25fps initially */
    ps->frame_last_pts   = 0.0;
    ps->audio_clock      = 0.0;
    ps->audio_clock_sync = 0.0;
    ps->av_bias = 0.0;
    ps->av_bias_samples = 0;
    /* Pacing state is per-file: a playlist transition must not
     * inherit the previous file's display cadence, drift, or drop
     * history. */
    ps->drift_resync_ticks = 0;
    ps->pace_ring_n        = 0;
    ps->pace_ring_pos      = 0;
    ps->pace_median        = 0.0;
    ps->pace_last_present  = 0.0;
    ps->pace_content_ema   = 0.0;
    ps->pace_mode          = PACE_SCHEDULED;
    ps->pace_enter_streak  = 0;
    ps->pace_exit_streak   = 0;
    ps->pace_drift_streak  = 0;
    ps->last_av_diff       = 0.0;
    ps->sched_off          = 0.0;
    ps->sched_off_valid    = 0;
    ps->sched_chain        = 0;
    ps->sched_chain_start  = 0.0;
    ps->pace_bias_ref      = 0.0;
    ps->hdrwire_logged     = 0;
    ps->video_clock      = 0.0;

    /* Suppress frame drops until the first frame is displayed.
     * Adapts automatically to any codec's keyframe recovery time. */
    ps->seek_recovering = 1;
    ps->video_ready = 0;
    ps->last_frame_wall  = 0.0;
    ps->audio_stalled    = 0;
    ps->bitstream_failed = 0;   /* stale cross-file flag (finding 3) */

    /* ── Reset diagnostics ── */
    ps->diag_frames_displayed = 0;
    ps->diag_frames_decoded   = 0;
    ps->diag_frames_dropped   = 0;
    ps->diag_multi_decodes    = 0;
    ps->diag_timer_snaps      = 0;
    ps->diag_max_av_drift     = 0.0;
    ps->diag_last_report      = get_time_sec();

    /* Real-time FPS window */
    ps->fps_window_start   = 0.0;
    ps->fps_window_frames  = 0;
    ps->rfps_window_frames = 0;
    ps->fps_content        = 0.0;
    ps->fps_render         = 0.0;
    ps->active_variant     = -1;   /* no render pass yet this file */

    /* ── Probe HDMI sink for bitstream capabilities (once per session) ── */
    if (!ps->bitstream_caps.probed)
        bitstream_probe(ps);

    /* ── Open audio output ──
     * If passthrough mode is requested and the sink supports the codec,
     * use ALSA direct (bitstream_start). Otherwise fall back to SDL3 PCM. */
    if (ps->audio_codec_ctx) {
        int use_bitstream = 0;
        if (ps->audio_mode != AUDIO_MODE_PCM && ps->bitstream_caps.probed)
            use_bitstream = bitstream_start(ps);
        if (!use_bitstream && audio_open(ps) < 0) {
            /* Checked like every sibling site (the finding-2 freeze
             * class, sixth site — the widest path in the program): a
             * codec with no device fills audio_pq with nothing
             * draining it and the demux throttle wedges all playback.
             * Continue video-only instead. */
            log_msg("Audio: device open failed at file open — "
                    "continuing video-only");
            avcodec_free_context(&ps->audio_codec_ctx);
            audio_disable_public(ps, "Audio: device error, audio off");
        }
    }

    /* ── Pre-allocate overlay at max display resolution ──
     * The overlay texture (up to 33MB at 4K RGBA) causes a GPU stall on
     * shared-memory APUs when allocated mid-playback. Pre-allocating here
     * moves the stall to init time, before the first frame is decoded. */
    {
        const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(
            SDL_GetPrimaryDisplay());
        if (dm) {
            int ow, oh;
            SDL_GetWindowSizeInPixels(ps->window, &ow, &oh);
            int max_w = (dm->w > ow) ? dm->w : ow;
            int max_h = (dm->h > oh) ? dm->h : oh;
            gpu_overlay_ensure(ps, max_w, max_h);
        }
    }

    /* ── Start demux thread ── */
    ps->eof     = 0;
    ps->playing = 1;
    ps->paused  = 0;
    ps->demux_thread = SDL_CreateThread(demux_thread_func, "demux", ps);

    /* ── Start decode thread ── */
    ps->decode_thread = SDL_CreateThread(decode_thread_func, "decode", ps);

    /* Build media info string */
    player_build_media_info(ps);

    return 0;
}

/* Close playback: stop threads, free all resources. */
void player_close(PlayerState *ps) {
    if (!ps->playing && !ps->fmt_ctx) return;
    log_msg("player_close: stopping playback");

    /* ── Playback diagnostics summary ── */
    if (ps->diag_frames_decoded > 0) {
        double drop_pct = (ps->diag_frames_decoded > 0)
            ? (100.0 * ps->diag_frames_dropped / ps->diag_frames_decoded)
            : 0.0;
        log_msg("DIAG: === Playback Summary ===");
        log_msg("DIAG:   Frames decoded:   %d", ps->diag_frames_decoded);
        log_msg("DIAG:   Frames displayed:  %d", ps->diag_frames_displayed);
        log_msg("DIAG:   Frames dropped:    %d (%.2f%%)",
                ps->diag_frames_dropped, drop_pct);
        log_msg("DIAG:   Multi-decode ticks: %d", ps->diag_multi_decodes);
        log_msg("DIAG:   Timer snap-forwards: %d", ps->diag_timer_snaps);
        log_msg("DIAG:   Peak A/V drift:    %.1fms",
                ps->diag_max_av_drift * 1000.0);
        log_msg("DIAG:   A/V bias:          %.1fms",
                ps->av_bias * 1000.0);
    }

    /* Stop this file's threads via the dedicated close flag. This was
     * ps->quit = 1 until 2026-08-20: the reset block below then zeroed
     * quit, silently undoing shim_session_end()'s exit request on the
     * Q/O/gamepad-B paths — the appliance parked on the idle screen
     * while dsvp-shim blocked in waitpid (Knot audit finding 1, a
     * regression of range-review 6a/6b by its own fix). */
    ps->closing = 1;

    /* Join async audio switch thread if running */
    if (ps->audio_switch_thread) {
        SDL_WaitThread(ps->audio_switch_thread, NULL);
        ps->audio_switch_thread = NULL;
    }
    ps->audio_switch_phase = 0;

    /* Signal queues to unblock any waiting threads */
    ps->video_pq.abort_request = 1;
    ps->audio_pq.abort_request = 1;
    SDL_SignalCondition(ps->video_pq.cond);
    SDL_SignalCondition(ps->audio_pq.cond);

    /* Wake and wait for decode thread (must exit before codec free).
     * The signal must be sent under decode_mutex: the decode thread
     * checks its predicate and enters SDL_WaitCondition while holding
     * the mutex — an unlocked signal can land in that window and be
     * lost, leaving SDL_WaitThread below blocked forever (the other
     * two signal sites, main.c consume and the demux seek flush, both
     * hold the mutex; only this path skipped it). */
    if (ps->decode_cond) {
        SDL_LockMutex(ps->decode_mutex);
        SDL_SignalCondition(ps->decode_cond);
        SDL_UnlockMutex(ps->decode_mutex);
    }
    if (ps->decode_thread) {
        SDL_WaitThread(ps->decode_thread, NULL);
        ps->decode_thread = NULL;
    }

    /* Wait for demux thread */
    if (ps->demux_thread) {
        SDL_WaitThread(ps->demux_thread, NULL);
        ps->demux_thread = NULL;
    }

    /* Close audio */
    bitstream_stop(ps);  /* no-op if not active */
    audio_close(ps);

    /* Force re-probe on next file open. ELD index and IEC958 mixer
     * index can shift when HDMI sinks come and go between files;
     * a fresh probe avoids stale state. */
    ps->bitstream_caps.probed = 0;

    /* Close subtitles */
    sub_close_codec(ps);

    /* Flush queues */
    pq_destroy(&ps->video_pq);
    pq_destroy(&ps->audio_pq);
    for (int i = 0; i < ps->sub_count; i++)
        pq_destroy(&ps->sub_pqs[i]);

    /* Destroy seek mutex */
    if (ps->seek_mutex) { SDL_DestroyMutex(ps->seek_mutex); ps->seek_mutex = NULL; }

    /* Destroy decode thread resources */
    if (ps->decode_mutex) { SDL_DestroyMutex(ps->decode_mutex); ps->decode_mutex = NULL; }
    if (ps->decode_cond)  { SDL_DestroyCondition(ps->decode_cond); ps->decode_cond = NULL; }
    if (ps->decoded_frame) av_frame_free(&ps->decoded_frame);

    /* Free frames */
    if (ps->video_frame)  av_frame_free(&ps->video_frame);
    if (ps->rgb_frame)    av_frame_free(&ps->rgb_frame);
    if (ps->audio_frame)  av_frame_free(&ps->audio_frame);
    if (ps->hw_frame)     av_frame_free(&ps->hw_frame);

    /* Free buffers */
    if (ps->rgb_buffer)    { av_free(ps->rgb_buffer);    ps->rgb_buffer    = NULL; }
    if (ps->audio_buf)     { av_free(ps->audio_buf);     ps->audio_buf     = NULL; ps->audio_buf_cap = 0; }
    if (ps->p010_u_plane)  { av_free(ps->p010_u_plane);  ps->p010_u_plane  = NULL; }
    if (ps->p010_v_plane)  { av_free(ps->p010_v_plane);  ps->p010_v_plane  = NULL; }

    /* Free scale/resample contexts */
    if (ps->sws_ctx)      { sws_freeContext(ps->sws_ctx); ps->sws_ctx = NULL; }
    ps->sws_out_10bit = 0;
    ps->sws_dst_siting = 0;
    if (ps->swr_ctx)      { swr_free(&ps->swr_ctx); ps->swr_ctx = NULL; }
    av_channel_layout_uninit(&ps->swr_in_layout);

    /* Free codecs */
    if (ps->video_codec_ctx) avcodec_free_context(&ps->video_codec_ctx);
    if (ps->audio_codec_ctx) avcodec_free_context(&ps->audio_codec_ctx);

    /* Close format */
    if (ps->fmt_ctx) avformat_close_input(&ps->fmt_ctx);

    /* ── Destroy GPU video textures and transfer buffers ── */
    gpu_destroy_video_textures(ps);

    /* ── VAAPI zero-copy cleanup ── */
    vaapi_zerocopy_cleanup(ps);

    /* Reset state */
    ps->playing            = 0;
    ps->paused             = 0;
    ps->eof                = 0;
    ps->closing            = 0;   /* ps->quit deliberately NOT touched */
    ps->vaapi_active       = 0;
    ps->vaapi_nv12         = 0;
    ps->video_stream_idx   = -1;
    ps->audio_stream_idx   = -1;
    ps->audio_buf_size     = 0;
    ps->audio_buf_index    = 0;
    ps->seek_request       = 0;
    ps->seeking            = 0;
    ps->seek_recovering    = 0;
    ps->decode_frame_ready = 0;
    ps->decode_eof         = 0;
    ps->audio_pts_floor    = 0.0;
    ps->video_ready        = 0;
    ps->show_debug         = 0;
    ps->show_info          = 0;
    ps->show_seekbar       = 0;
    ps->seekbar_hide_time  = 0.0;
    ps->overlay_active     = 0;
    ps->aud_count          = 0;
    ps->aud_selection      = 0;
    ps->aud_osd[0]         = '\0';
    ps->sub_count          = 0;
    ps->sub_selection      = 0;
    ps->sub_active_idx     = -1;
    ps->sub_valid          = 0;
    ps->sub_is_bitmap      = 0;
    ps->sub_bitmap_count   = 0;
    ps->sub_text[0]        = '\0';
    sub_text_cues_clear(ps);
    ps->sub_osd[0]         = '\0';

    /* Revert to the SDR swapchain: the browser/idle screen is SDR
     * content, and the design rule is explicit — desktop-facing state
     * never stays in HDR past file close. */
    ps->hdr_pass_content = 0;
    hdr_output_apply(ps);

    /* Reset window (skip resize if fullscreen — actual size is monitor) */
    SDL_SetWindowTitle(ps->window, DSVP_WINDOW_TITLE);
    if (!ps->fullscreen) {
        SDL_SetWindowSize(ps->window, DEFAULT_WIN_W, DEFAULT_WIN_H);
        SDL_SetWindowPosition(ps->window,
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}


/* ═══════════════════════════════════════════════════════════════════
 * Demux Thread
 * ═══════════════════════════════════════════════════════════════════
 *
 * Reads packets from the container file and distributes them to
 * the video and audio packet queues.
 */

int demux_thread_func(void *arg) {
    PlayerState *ps = (PlayerState *)arg;
    AVPacket *pkt = av_packet_alloc();
    log_msg("Demux thread started");

    while (!ps->quit && !ps->closing) {
        /* ── Handle seek requests ── */
        if (ps->seek_request) {
            /* Clear the request BEFORE latching the target (P2-8): a
             * player_seek() landing during av_seek_frame re-sets the
             * flag and the next loop iteration serves it. The old
             * post-processing clear erased such requests — held-key
             * seeks on slow media dropped presses. Worst-case
             * interleaving now duplicates a seek; it never drops one. */
            ps->seek_request = 0;
            int64_t target = ps->seek_target;
            log_msg("Demux: seeking to %.3f s", (double)target / AV_TIME_BASE);

            /* CRITICAL: Lock the seek mutex. This prevents the main thread
             * from calling avcodec_send_packet/receive_frame on the video
             * codec while we flush it. The audio callback is also paused. */
            SDL_LockMutex(ps->seek_mutex);
            ps->seeking = 1;

            /* Pause audio device so callback can't touch audio codec.
             * BARRIER: pause does not wait for an in-flight callback —
             * SDL holds the stream lock while running the get-callback,
             * so lock+unlock returns only after any in-flight
             * audio_decode_frame() has finished. Only then is the codec
             * safe to flush. */
            if (ps->audio_stream) {
                SDL_PauseAudioStreamDevice(ps->audio_stream);
                SDL_LockAudioStream(ps->audio_stream);
                SDL_UnlockAudioStream(ps->audio_stream);
            }

            ps->io_deadline = get_time_sec() + 10.0;
            int ret = av_seek_frame(ps->fmt_ctx, -1, target, ps->seek_flags);
            ps->io_deadline = 0.0;
            if (ret < 0) {
                /* Failure epilogue (P2-9): the demuxer never moved.
                 * Do NOT reset clocks to the phantom target, discard
                 * the decoded frame, clear the audio pipeline, or
                 * signal a bitstream reset — just resume the audio we
                 * paused above and carry on at the old position. */
                log_msg("ERROR: Seek failed: %s — continuing at current "
                        "position", av_err2str(ret));
                ps->seeking = 0;
                SDL_UnlockMutex(ps->seek_mutex);
                if (ps->audio_stream && !ps->paused)
                    SDL_ResumeAudioStreamDevice(ps->audio_stream);
                continue;
            } else {
                log_msg("Demux: av_seek_frame OK, flushing queues");
                pq_flush(&ps->video_pq);
                pq_flush(&ps->audio_pq);
                for (int i = 0; i < ps->sub_count; i++)
                    pq_flush(&ps->sub_pqs[i]);
                log_msg("Demux: queues flushed, flushing video codec");
                if (ps->video_codec_ctx)
                    avcodec_flush_buffers(ps->video_codec_ctx);
                log_msg("Demux: video codec flushed, flushing audio codec");
                if (ps->audio_codec_ctx)
                    avcodec_flush_buffers(ps->audio_codec_ctx);
                if (ps->sub_codec_ctx)
                    avcodec_flush_buffers(ps->sub_codec_ctx);
                ps->sub_valid = 0;
                ps->sub_text[0] = '\0';
                sub_text_cues_clear(ps);  /* seek = clear-all (P2-16) */
                log_msg("Demux: all codecs flushed");
            }
            /* seek_request was cleared before processing (P2-8) */
            ps->eof = 0;

            /* Reset audio decode buffer (safe — callback is paused) */
            ps->audio_buf_size  = 0;
            ps->audio_buf_index = 0;

            /* Reset both clocks to the seek target. Without this,
             * video_clock retains the old position until the first
             * frame is decoded, causing a phantom drift spike equal
             * to the entire seek distance (e.g. 895 seconds). */
            {
                double seek_pos = (double)target / AV_TIME_BASE;
                ps->audio_clock = seek_pos;
                ps->audio_clock_sync = seek_pos;
                ps->video_clock = seek_pos;
            }

            /* Flush the decode thread's frame buffer.
             * Safe: seek_mutex is held, so decode thread can't be mid-decode.
             * Signal the cond to wake decode thread if it was waiting. */
            SDL_LockMutex(ps->decode_mutex);
            av_frame_unref(ps->decoded_frame);
            ps->decode_frame_ready = 0;
            ps->decoded_frame_xfer = -1;   /* staged planes die with the frame */
            ps->decoded_frame_slot = -1;   /* pool ref freed by the unref above */
            ps->decode_eof = 0;
            SDL_SignalCondition(ps->decode_cond);
            SDL_UnlockMutex(ps->decode_mutex);

            ps->seeking = 0;
            SDL_UnlockMutex(ps->seek_mutex);

            /* Suppress frame drops until the first frame is displayed
             * post-seek. Adapts to any codec — H.264 recovers in
             * ~100ms, HEVC with long GOPs may take 5–10 seconds.
             * Cleared in main.c when a frame is actually shown. */
            ps->seek_recovering = 1;
            /* av_bias is NOT reset here. It models the systematic latency of
             * the audio pipeline (SDL + device buffering that audio_clock_sync
             * under-counts), which is a property of the output path, not of
             * playback position — it is identical either side of a seek.
             * Zeroing it meant the ~-70ms steady-state offset was measured
             * raw again after every seek, crossing the -50ms drop threshold
             * and discarding 2-5 good frames until the EMA re-converged.
             * That is the entire "frame dropped ... A/V drift: -57..-97ms"
             * burst that follows every seek in the logs. */

            /* Flush any pre-seek audio still queued in SDL pipeline.
             * Audio stays PAUSED until the first video frame is displayed
             * (main.c seek_recovering clear) — prevents audio clock from
             * running ahead while VAAPI rebuilds its DPB after a seek. */
            if (ps->audio_stream)
                SDL_ClearAudioStream(ps->audio_stream);

            /* ── Signal bitstream thread to reset clock state on seek ──
             * The bitstream thread computes audio_clock_sync from wall-clock
             * math: submitted_frames / rate - (now - wall_start). After a
             * seek, both counters are stale. Setting this flag tells the
             * bitstream thread to drop the ALSA buffer, re-prepare, and
             * zero the counters on its own thread — avoiding a data race
             * with the concurrent read/increment in the ALSA write loop. */
            if (ps->bitstream_active) {
                ps->bitstream_seek_pending = 1;
            }

            log_msg("Demux: seek complete");
        }

        /* ── Throttle if queues are full ──
         * In bitstream mode, only throttle on video queue — TrueHD generates
         * ~1200 audio packets/sec vs ~24 video. Blocking on audio fullness
         * starves the video pipeline within ~200ms. The bitstream thread
         * drains audio at wire rate; letting audio queue grow is safe. */
        int video_full = ps->video_pq.nb_packets > PACKET_QUEUE_MAX;
        int audio_full = ps->audio_pq.nb_packets > PACKET_QUEUE_MAX;
        if (video_full || (audio_full && !ps->bitstream_active)) {
            SDL_Delay(10);
            continue;
        }

        /* ── Read next packet ── */
        ps->io_deadline = get_time_sec() + 10.0;
        double t_read0 = get_time_sec();
        int ret = av_read_frame(ps->fmt_ctx, pkt);
        double t_read1 = get_time_sec();
        ps->io_deadline = 0.0;

        /* Log slow reads (>100ms) — helps diagnose post-seek stalls */
        if ((t_read1 - t_read0) > 0.100)
            log_msg("DEMUX DIAG: av_read_frame took %.0fms (stream=%d vpq=%d)",
                    (t_read1 - t_read0) * 1000.0,
                    (ret >= 0) ? pkt->stream_index : -1,
                    ps->video_pq.nb_packets);
        if (ret < 0) {
            if (ret == AVERROR_EOF || avio_feof(ps->fmt_ctx->pb)) {
                if (!ps->eof) log_msg("Demux: reached end of file");
                ps->eof = 1;
                SDL_Delay(100);
                continue;
            }
            if (ret == AVERROR_EXIT) {
                log_msg("Demux: I/O aborted (network loss or timeout)");
            } else {
                log_msg("ERROR: av_read_frame failed: %s", av_err2str(ret));
            }
            ps->io_error = 1;
            break; /* real error — exit demux loop, player_close will clean up */
        }

        /* Route packet to the correct queue.
         * pq_put only takes ownership on success; on node-alloc failure the
         * packet is still ours and must be unreffed or its payload leaks. */
        if (pkt->stream_index == ps->video_stream_idx) {
            if (pq_put(&ps->video_pq, pkt) < 0) av_packet_unref(pkt);
        } else if (pkt->stream_index == ps->audio_stream_idx) {
            if (pq_put(&ps->audio_pq, pkt) < 0) av_packet_unref(pkt);
        } else {
            /* Check subtitle streams.
             * Only the SELECTED track is queued. Every catalogued track used
             * to be queued while only the selected one ever drained, so on a
             * long remux with many PGS tracks the unread queues grew for the
             * whole session, and switching to one of them at minute 90 then
             * decoded its entire backlog synchronously on the main thread.
             * (Seeks flushed them, so this only bit an uninterrupted watch.)
             * sub_selection: 0 = off, 1..sub_count = track. Read unsynchronised
             * from the main thread; the worst case during a switch is one
             * packet landing in a queue that sub_cycle flushes immediately. */
            /* REVISED (from DSVP main 55834d4): queue EVERY track, pruned
             * to a rolling window just behind the playback clock.
             *
             * Selected-only queueing (the previous behaviour) made S feel
             * broken: the fresh queue only fills from the demux read
             * position, which runs ~10s ahead of playback, so the packets
             * covering the moment on screen had already been read and
             * discarded — subtitles appeared only once playback caught up.
             * Queueing everything unpruned grew unbounded on multi-track
             * remuxes. The rolling window keeps switching instant AND
             * memory bounded (SUB_PRUNE_WINDOW_SEC per track, sized just
             * over the decoder's SUB_STALE_CAP_SEC stale-skip so nothing
             * prunable would have been decoded anyway; SUB_PQ_MAX_BYTES
             * backstops NOPTS tracks the PTS walk can't judge). */
            int routed = 0;
            for (int i = 0; i < ps->sub_count; i++) {
                if (pkt->stream_index == ps->sub_stream_indices[i]) {
                    if (pq_put(&ps->sub_pqs[i], pkt) < 0)
                        av_packet_unref(pkt);
                    double now = player_clock(ps);
                    AVStream *st =
                        ps->fmt_ctx->streams[ps->sub_stream_indices[i]];
                    double tb = av_q2d(st->time_base);
                    if (now > SUB_PRUNE_WINDOW_SEC && tb > 0.0)
                        pq_prune_stale(&ps->sub_pqs[i],
                                       (int64_t)((now - SUB_PRUNE_WINDOW_SEC) / tb),
                                       SUB_PQ_MAX_BYTES);
                    else
                        pq_prune_stale(&ps->sub_pqs[i], INT64_MIN,
                                       SUB_PQ_MAX_BYTES);
                    routed = 1;
                    break;
                }
            }
            if (!routed) {
                av_packet_unref(pkt);
            }
        }
    }

    av_packet_free(&pkt);
    log_msg("Demux thread exiting");
    return 0;
}


/* Defined in the GPU upload section below; the decode thread calls it
 * for the pre-stage copy (review M1), which sits earlier in the file. */
static int upload_plane(SDL_GPUDevice *device, SDL_GPUTransferBuffer *xfer,
                        const uint8_t *src, int src_stride,
                        int width, int height);

/* ═══════════════════════════════════════════════════════════════════
 * Decode Thread
 * ═══════════════════════════════════════════════════════════════════
 *
 * Decodes video frames in a background thread, feeding one decoded
 * frame at a time to the main loop via decoded_frame.
 *
 * The main loop consumes the frame when frame_timer permits, then
 * signals decode_cond so this thread can decode the next one.
 * This decouples the 22-37ms VAAPI decode+readback from the
 * VSync-driven main loop, allowing continuous reblits at display
 * refresh rate (60Hz, 120Hz, 144Hz, etc.).
 */

int decode_thread_func(void *arg) {
    PlayerState *ps = (PlayerState *)arg;
    log_msg("Decode thread started");

    int hard_err_streak = 0;  /* consecutive non-EAGAIN receive_frame errors */

    /* ── Stall diagnostic state ── */
    double diag_last_frame_time = 0.0;  /* wall time of last decoded frame */
    int    diag_stall_logged    = 0;    /* avoid re-logging same stall */
    int    diag_eagain_count    = 0;    /* EAGAIN hits since last frame */
    int    diag_empty_count     = 0;    /* empty-queue hits since last frame */
    int    diag_gate_count      = 0;    /* decode_frame_ready waits */
    int    diag_seekmtx_count   = 0;    /* seek_mutex contention hits */

    while (!ps->quit && !ps->closing) {
        /* ── Wait until main loop consumed the previous frame ── */
        SDL_LockMutex(ps->decode_mutex);
        if (ps->decode_frame_ready && !ps->quit && !ps->closing
            && !ps->seeking) {
            diag_gate_count++;
        }
        while (ps->decode_frame_ready && !ps->quit && !ps->closing
               && !ps->seeking)
            SDL_WaitCondition(ps->decode_cond, ps->decode_mutex);
        SDL_UnlockMutex(ps->decode_mutex);

        if (ps->quit || ps->closing) break;

        /* Skip decode when paused, seeking, or not playing */
        if (ps->seeking || !ps->playing || ps->paused) {
            if (ps->seeking) {
                diag_last_frame_time = 0.0;  /* reset stall timer on seek */
                diag_stall_logged = 0;
                diag_eagain_count = 0;
                diag_empty_count  = 0;
                diag_gate_count   = 0;
                diag_seekmtx_count = 0;
            }
            SDL_Delay(1);
            continue;
        }

        /* ── Lock seek_mutex to prevent codec flush mid-decode ──
         * TryLock: if the demux thread is seeking (holds the mutex),
         * we yield instead of blocking — same pattern the old
         * synchronous main-loop decode used. */
        if (!SDL_TryLockMutex(ps->seek_mutex)) {
            diag_seekmtx_count++;
            SDL_Delay(1);
            continue;
        }

        int got_frame = 0;
        int staged_set = -1;   /* transfer set pre-filled for this frame */
#ifdef DSVP_PROFILE
        double t_dec0 = get_time_sec();
#endif

        for (;;) {
            /* Try to receive a decoded frame.
             * VAAPI path: receive into hw_frame, then transfer to
             * decoded_frame (GPU→CPU readback — the expensive part). */
            AVFrame *recv_frame = ps->vaapi_active
                                  ? ps->hw_frame : ps->decoded_frame;
            double t_recv0 = get_time_sec();
            int ret = avcodec_receive_frame(ps->video_codec_ctx, recv_frame);
            double t_recv1 = get_time_sec();

            /* Log if receive_frame itself took a long time */
            if ((t_recv1 - t_recv0) > 0.100)
                log_msg("DECODE DIAG: receive_frame took %.0fms (ret=%d)",
                        (t_recv1 - t_recv0) * 1000.0, ret);

            if (ret == 0) {
                if (ps->vaapi_active) {
                    if (ps->vaapi_zerocopy) {
                        /* Zero-copy: keep raw VAAPI surface in decoded_frame.
                         * No GPU→CPU readback. data[3] = VASurfaceID.
                         * The main loop will hand this to vaapi_zerocopy_upload
                         * which exports the surface via DMA-BUF. */
                        av_frame_unref(ps->decoded_frame);
                        av_frame_move_ref(ps->decoded_frame, ps->hw_frame);
                    } else
                    {
                        /* Readback path: GPU→CPU transfer (35-42ms at 4K) */
                        av_frame_unref(ps->decoded_frame);
                        ret = av_hwframe_transfer_data(
                                  ps->decoded_frame, ps->hw_frame, 0);
                        if (ret < 0) {
                            log_msg("ERROR: av_hwframe_transfer_data: %s",
                                    av_err2str(ret));
                            av_frame_unref(ps->hw_frame);
                            SDL_UnlockMutex(ps->seek_mutex);
                            SDL_Delay(1);
                            goto next_iter;
                        }
                        av_frame_copy_props(ps->decoded_frame, ps->hw_frame);
                        av_frame_unref(ps->hw_frame);
                    }
                }

                /* Compute PTS */
                AVStream *vs = ps->fmt_ctx->streams[ps->video_stream_idx];
                int64_t frame_pts = ps->decoded_frame->best_effort_timestamp;
                if (frame_pts == AV_NOPTS_VALUE)
                    frame_pts = ps->decoded_frame->pts;
                /* A PTS-less frame used to be timestamped 0.0, which made the
                 * clock jump to the start of the file for one frame: a phantom
                 * multi-second A/V drift, a spurious drop, and a poisoned EMA.
                 * Extrapolate from the previous frame instead. */
                double pts;
                if (frame_pts != AV_NOPTS_VALUE) {
                    pts = (double)frame_pts * av_q2d(vs->time_base);
                } else {
                    pts = ps->decoded_pts + (ps->frame_last_delay > 0.0
                                             ? ps->frame_last_delay : 0.0);
                }

                ps->decoded_pts = pts;
                got_frame = 1;
                break;
            }
            if (ret != AVERROR(EAGAIN)) {
                /* AVERROR_EOF = decoder fully drained */
                if (ret == AVERROR_EOF) {
                    ps->decode_eof = 1;
                    hard_err_streak = 0;
                } else {
                    /* A persistent hard error used to be retried forever with
                     * no log and no flag: video froze on the last frame, the
                     * stall watchdog paused audio, and there was no route back
                     * to the browser. vaapi_get_format returning NONE (HEVC
                     * 4:2:2, unsupported profile) lands here, because it fires
                     * at first decode rather than at avcodec_open2 where the
                     * software-retry path lives. Escalate to io_error, which
                     * main.c already tears down cleanly. */
                    if (++hard_err_streak >= 30) {
                        log_msg("ERROR: video decode failing persistently (%s)"
                                " - aborting playback", av_err2str(ret));
                        ps->io_error = 1;
                        hard_err_streak = 0;
                    }
                }
                break;
            }
            hard_err_streak = 0;

            diag_eagain_count++;

            /* Need more packets from the queue */
            AVPacket pkt;
            ret = pq_get(&ps->video_pq, &pkt, 0);
            if (ret <= 0) {
                diag_empty_count++;
                /* No packets available. If demuxer hit EOF, flush the
                 * decoder by sending a NULL packet. This triggers drain
                 * mode: receive_frame will return any buffered frames,
                 * then AVERROR_EOF when fully drained. */
                if (ps->eof) {
                    avcodec_send_packet(ps->video_codec_ctx, NULL);
                    continue;  /* loop back to receive_frame */
                }
                break;  /* not EOF — yield and try again later */
            }
            avcodec_send_packet(ps->video_codec_ctx, &pkt);
            av_packet_unref(&pkt);
        }

        /* ── Pre-stage planes into a GPU transfer set (review M1) ──
         * The Y+U+V staging memcpy (12.4MB/frame at 4K) used to run on
         * the vsync-gated main thread inside video_display, serial
         * with present — the biggest recoverable chunk of the frame
         * tick. Do it HERE instead, into the ping-pong set the main
         * thread is not reading. Placement is load-bearing: seek_mutex
         * is still held, and the seek flush (which unrefs
         * decoded_frame) runs with seek_mutex held, so the frame
         * cannot vanish mid-copy. Map(cycle=true) rotates off any
         * backing the GPU still reads; the ping-pong plus the
         * single-slot handoff guarantees the main thread has already
         * recorded its copy pass from a set before this thread can
         * come back around to refill it. Any frame this cannot serve
         * (VAAPI, swscale, format/size mismatch, map failure,
         * DSVP_NO_PRESTAGE=1) leaves staged_set at -1 and the main
         * thread uploads into its own set 2 exactly as before. */
        /* Pool-backed frame? Its planes ALREADY live in a transfer
         * buffer — the decoder wrote them there via get_buffer2. No
         * prestage memcpy at all; just carry the slot index. */
        int pool_slot = -1;
        if (got_frame && ps->xfer_pool_n > 0
                && ps->decoded_frame->buf[0]) {
            void *op = av_buffer_get_opaque(ps->decoded_frame->buf[0]);
            if (op >= (void *)&ps->xfer_pool[0]
                    && op < (void *)&ps->xfer_pool[ps->xfer_pool_n])
                pool_slot = (int)((XferSlot *)op - &ps->xfer_pool[0]);
            /* FFmpeg applies container cropping by SHIFTING the
             * frame's data pointers inside the buffer we handed it.
             * The pool copy pass binds the slot BASE, so a shifted
             * frame would display offset by the crop — while the
             * staged path copies from the shifted pointers and is
             * correct for free (review 2026-08-20 finding 19).
             * Cropped frames (rare: crop_top/left content) fall back
             * to staging. */
            if (pool_slot >= 0 &&
                (ps->decoded_frame->data[0] != ps->xfer_pool[pool_slot].my ||
                 ps->decoded_frame->data[1] != ps->xfer_pool[pool_slot].mu ||
                 ps->decoded_frame->data[2] != ps->xfer_pool[pool_slot].mv))
                pool_slot = -1;
            if (pool_slot >= 0)
                ps->xfer_pool_served++;   /* decode-thread-only counter */
        }

        if (got_frame && pool_slot < 0 && !ps->no_prestage
                && !ps->vaapi_active && !ps->sws_ctx
                && (ps->decoded_frame->format == AV_PIX_FMT_YUV420P ||
                    ps->decoded_frame->format == AV_PIX_FMT_YUV420P10LE)
                && ps->decoded_frame->width  == ps->vid_w
                && ps->decoded_frame->height == ps->vid_h
                && ps->gpu_xfer_y[0] && ps->gpu_xfer_y[1]) {
            const AVFrame *df = ps->decoded_frame;
            int bpp = (df->format == AV_PIX_FMT_YUV420P10LE) ? 2 : 1;
            int cw  = (ps->vid_w + 1) / 2;
            int chh = (ps->vid_h + 1) / 2;
            int set = ps->xfer_fill;
            if (upload_plane(ps->gpu_device, ps->gpu_xfer_y[set],
                             df->data[0], df->linesize[0],
                             ps->vid_w * bpp, ps->vid_h) == 0 &&
                upload_plane(ps->gpu_device, ps->gpu_xfer_u[set],
                             df->data[1], df->linesize[1],
                             cw * bpp, chh) == 0 &&
                upload_plane(ps->gpu_device, ps->gpu_xfer_v[set],
                             df->data[2], df->linesize[2],
                             cw * bpp, chh) == 0) {
                staged_set   = set;
                ps->xfer_fill = set ^ 1;
            }
        }

        SDL_UnlockMutex(ps->seek_mutex);

        /* ── Stall diagnostic: report when decode gap exceeds 200ms ── */
        {
            double now_d = get_time_sec();
            if (got_frame) {
                double gap = (diag_last_frame_time > 0.0)
                    ? (now_d - diag_last_frame_time) * 1000.0 : 0.0;
                if (gap > 200.0 && diag_last_frame_time > 0.0) {
                    log_msg("DECODE DIAG: %.0fms gap between frames — "
                            "gate=%d eagain=%d empty_q=%d seekmtx=%d "
                            "vpq=%d apq=%d",
                            gap, diag_gate_count, diag_eagain_count,
                            diag_empty_count, diag_seekmtx_count,
                            ps->video_pq.nb_packets,
                            ps->audio_pq.nb_packets);
                }
                diag_last_frame_time = now_d;
                diag_stall_logged    = 0;
                diag_eagain_count    = 0;
                diag_empty_count     = 0;
                diag_gate_count      = 0;
                diag_seekmtx_count   = 0;
            } else if (!diag_stall_logged && diag_last_frame_time > 0.0
                       && (now_d - diag_last_frame_time) > 1.0) {
                /* Log once when decode has been stuck for >1 second */
                log_msg("DECODE DIAG: stalled >1s — "
                        "gate=%d eagain=%d empty_q=%d seekmtx=%d "
                        "vpq=%d apq=%d seeking=%d eof=%d",
                        diag_gate_count, diag_eagain_count,
                        diag_empty_count, diag_seekmtx_count,
                        ps->video_pq.nb_packets,
                        ps->audio_pq.nb_packets,
                        ps->seeking, ps->eof);
                diag_stall_logged = 1;
            }
        }

#ifdef DSVP_PROFILE
        if (got_frame) {
            double dec_ms = (get_time_sec() - t_dec0) * 1000.0;
            ps->prof_decode_ms = dec_ms;
            ps->prof_sum_decode += dec_ms;
            if (dec_ms > ps->prof_max_decode)
                ps->prof_max_decode = dec_ms;
            /* Log decode spikes that approach frame budget.
             * Threshold: 80% of content frame period.
             * 24fps → 33ms, 60fps → 13ms. */
            double dec_thr = ps->frame_last_delay > 0.001
                ? ps->frame_last_delay * 800.0 : 13.0;
            if (dec_ms > dec_thr) {
                /* Same 1/s rate limit as the display-side spike line:
                 * a steadily overloaded decode would otherwise log
                 * per frame through the unbuffered log. */
                static double s_dspike_last = 0.0;
                static int    s_dspike_supp = 0;
                double now_dsp = get_time_sec();
                if (now_dsp - s_dspike_last >= 1.0) {
                    log_msg("PROF SPIKE: decode=%.1fms (+%d suppressed)",
                            dec_ms, s_dspike_supp);
                    s_dspike_last = now_dsp;
                    s_dspike_supp = 0;
                } else {
                    s_dspike_supp++;
                }
            }
        }
#endif

        if (got_frame) {
            /* Hand off to main loop. A seek can complete in the gap
             * since seek_mutex was released above: its flush unref'd
             * decoded_frame under decode_mutex. Re-asserting ready
             * would hand main an empty frame with a stale pre-seek
             * decoded_pts — video_clock briefly wrong, one-frame
             * hiccup (review P2-6). Only publish a frame that
             * survived. */
            SDL_LockMutex(ps->decode_mutex);
            if (ps->decoded_frame->buf[0]) {
                ps->decode_frame_ready = 1;
                /* The staged-set / pool-slot index travels with the
                 * frame it belongs to, published under the mutex. */
                ps->decoded_frame_xfer = staged_set;
                ps->decoded_frame_slot = pool_slot;
            }
            SDL_UnlockMutex(ps->decode_mutex);
        } else {
            SDL_Delay(1); /* no packets or error — yield */
        }

next_iter:
        (void)0;  /* label requires a statement */
    }

    log_msg("Decode thread exiting");
    return 0;
}


/* Compute the letterboxed display rectangle for the video.
 * Maintains aspect ratio within the current window, centering with
 * black bars on the shorter axis. Call after window resize or video open. */
void player_update_display_rect(PlayerState *ps) {
    if (ps->vid_w <= 0 || ps->vid_h <= 0 || ps->win_w <= 0 || ps->win_h <= 0) {
        ps->display_rect = (SDL_Rect){ 0, 0, ps->win_w, ps->win_h };
        return;
    }

    double video_aspect = (double)ps->vid_w / ps->vid_h;
    double win_aspect   = (double)ps->win_w / ps->win_h;

    int disp_w, disp_h;

    /* Game Mode 16:10 fill: crop-to-fill instead of letterbox.
     * On the Deck's 1280×800 screen, 16:9 content would normally
     * get 80px of black bars. Fill mode fits to height and crops
     * ~5% per side — negligible loss, much better screen use.
     * Only activates when aspect ratios are close (within 15%). */
    if (ps->game_mode && video_aspect > win_aspect &&
            video_aspect / win_aspect < 1.15) {
        /* Fill to height, allow horizontal overflow (crop) */
        disp_h = ps->win_h;
        disp_w = (int)(ps->win_h * video_aspect);
    } else if (video_aspect > win_aspect) {
        /* Video is wider than window — pillarbox (bars top/bottom) */
        disp_w = ps->win_w;
        disp_h = (int)(ps->win_w / video_aspect);
    } else {
        /* Video is taller than window — letterbox (bars left/right) */
        disp_h = ps->win_h;
        disp_w = (int)(ps->win_h * video_aspect);
    }

    ps->display_rect.x = (ps->win_w - disp_w) / 2;
    ps->display_rect.y = (ps->win_h - disp_h) / 2;
    ps->display_rect.w = disp_w;
    ps->display_rect.h = disp_h;
}


/* ── Upload one YUV plane from AVFrame to GPU transfer buffer ──
 *
 * Handles stride mismatch: FFmpeg's linesize may be wider than the
 * actual pixel width (alignment padding). The transfer buffer is
 * tightly packed at the target width. */
static int upload_plane(
    SDL_GPUDevice *device,
    SDL_GPUTransferBuffer *xfer,
    const uint8_t *src, int src_stride,
    int width, int height)
{
    uint8_t *dst = SDL_MapGPUTransferBuffer(device, xfer, true);
    if (!dst) return -1;

    if (src_stride == width) {
        /* Tightly packed — single memcpy */
        memcpy(dst, src, (size_t)width * height);
    } else {
        /* Stride mismatch — copy row by row */
        for (int row = 0; row < height; row++) {
            memcpy(dst + row * width, src + row * src_stride, width);
        }
    }

    SDL_UnmapGPUTransferBuffer(device, xfer);
    return 0;
}


/* ═══════════════════════════════════════════════════════════════════
 * HDR Dynamic Peak Detection (Layer 1 — CPU histogram scan)
 *
 * Builds a 256-bin histogram of Y plane values per frame, reads off
 * the 99.875th percentile, converts to nits via PQ EOTF, and applies
 * temporal smoothing. Using a percentile instead of max avoids
 * specular highlights (sun glints, lamp reflections) inflating the
 * peak, which would cause BT.2390 to over-compress midtones.
 *
 * 99.875th percentile (skip top 0.125%) matches the spirit of
 * libplacebo/mpv's approach (default 99.995, user-tunable).
 * We use a slightly more aggressive value to better handle older
 * film content with occasional bright hotspots.
 *
 * Layer 2 will move this to a GPU compute shader for zero CPU cost.
 * ═══════════════════════════════════════════════════════════════════ */

/* PQ EOTF (SMPTE ST 2084): PQ code value [0,1] → linear nits [0,10000].
 * Scalar version of the shader's pq_eotf() for CPU-side use. */
static float pq_eotf_scalar(float pq) {
    const float m1 = 0.1593017578125f;   /* 2610/16384 */
    const float m2 = 78.84375f;          /* 2523/32 * 128 */
    const float c1 = 0.8359375f;         /* 3424/4096 */
    const float c2 = 18.8515625f;        /* 2413/128 */
    const float c3 = 18.6875f;           /* 2392/128 */

    float Np  = powf(fmaxf(pq, 0.0f), 1.0f / m2);
    float num = fmaxf(Np - c1, 0.0f);
    float den = c2 - c3 * Np;
    return 10000.0f * powf(fmaxf(num / den, 0.0f), 1.0f / m1);
}

/* Temporal smoothing parameters.
 * Fast attack (bright → brighter): adapt quickly so highlights aren't clipped.
 * Slow decay (bright → darker): prevent flickering from fading highlights.
 * Scene cut: jump immediately on large changes. */
/* ── Dolby Vision RPU Metadata Extraction ──
 *
 * FFmpeg's HEVC decoder parses DV RPU NALs and attaches parsed metadata
 * as AV_FRAME_DATA_DOVI_METADATA side data on each decoded frame.
 * This function extracts and logs that metadata so we can understand
 * the reshaping curves and color matrices needed for shader implementation.
 *
 * DV Profile 5 stores data in IPTPQc2 color space, not standard YCbCr.
 * The RPU contains per-component piecewise polynomial (or MMR) reshaping
 * curves that transform from the encoded IPTPQc2 signal back to standard
 * PQ-encoded BT.2020 RGB, plus color matrices for the conversion chain. */
static void dovi_log_frame_metadata(PlayerState *ps, const AVFrame *frame)
{
    /* Only log once per file open (first frame with DV metadata) */
    if (ps->dovi_metadata_logged) return;

    /* Check for raw RPU buffer first (always present if DV) */
    const AVFrameSideData *rpu_sd =
        av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_RPU_BUFFER);
    if (rpu_sd) {
        log_msg("DOVI: raw RPU buffer present (%d bytes)", rpu_sd->size);
    }

    /* Check for parsed metadata (what we actually need) */
    const AVFrameSideData *sd =
        av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA);
    if (!sd) {
        if (rpu_sd) {
            log_msg("DOVI: WARNING — raw RPU present but parsed "
                    "AV_FRAME_DATA_DOVI_METADATA missing! "
                    "FFmpeg may not be parsing this profile.");
        }
        /* No DV metadata on this frame — not a DV file or decoder
         * doesn't expose it. Will retry next frame. */
        return;
    }

    ps->dovi_metadata_logged = 1;
    const AVDOVIMetadata *dovi = (const AVDOVIMetadata *)sd->data;

    /* ── RPU Header ── */
    const AVDOVIRpuDataHeader *hdr = av_dovi_get_header(dovi);
    log_msg("DOVI RPU header: rpu_type=%u, rpu_format=%u, "
            "vdr_rpu_profile=%u, vdr_rpu_level=%u",
            hdr->rpu_type, hdr->rpu_format,
            hdr->vdr_rpu_profile, hdr->vdr_rpu_level);
    log_msg("DOVI RPU header: coef_data_type=%u, coef_log2_denom=%u, "
            "bl_video_full_range=%u, bl_bit_depth=%u, el_bit_depth=%u, "
            "vdr_bit_depth=%u",
            hdr->coef_data_type, hdr->coef_log2_denom,
            hdr->bl_video_full_range_flag, hdr->bl_bit_depth,
            hdr->el_bit_depth, hdr->vdr_bit_depth);
    log_msg("DOVI RPU header: disable_residual=%u, "
            "spatial_resampling=%u, el_spatial_resampling=%u",
            hdr->disable_residual_flag,
            hdr->spatial_resampling_filter_flag,
            hdr->el_spatial_resampling_filter_flag);

    /* ── Data Mapping (reshaping curves) ── */
    const AVDOVIDataMapping *mapping = av_dovi_get_mapping(dovi);
    log_msg("DOVI mapping: vdr_rpu_id=%u, mapping_color_space=%u, "
            "mapping_chroma_format=%u, nlq_method=%d",
            mapping->vdr_rpu_id, mapping->mapping_color_space,
            mapping->mapping_chroma_format_idc, mapping->nlq_method_idc);

    double coef_scale = (double)(1LL << hdr->coef_log2_denom);
    const char *comp_names[] = { "I/Y", "Ct/Cb", "Cp/Cr" };

    for (int c = 0; c < 3; c++) {
        const AVDOVIReshapingCurve *curve = &mapping->curves[c];
        log_msg("DOVI reshape [%s]: num_pivots=%u",
                comp_names[c], curve->num_pivots);

        /* Log pivot values */
        char pivot_str[256] = "";
        int pos = 0;
        for (int i = 0; i < curve->num_pivots && i < AV_DOVI_MAX_PIECES + 1; i++) {
            pos += snprintf(pivot_str + pos, sizeof(pivot_str) - pos,
                           "%s%u", i ? "," : "", curve->pivots[i]);
        }
        log_msg("DOVI reshape [%s]: pivots=[%s]", comp_names[c], pivot_str);

        /* Log each piece */
        int num_pieces = curve->num_pivots - 1;
        for (int p = 0; p < num_pieces && p < AV_DOVI_MAX_PIECES; p++) {
            if (curve->mapping_idc[p] == AV_DOVI_MAPPING_POLYNOMIAL) {
                int order = curve->poly_order[p];
                double c0 = (double)curve->poly_coef[p][0] / coef_scale;
                double c1 = (double)curve->poly_coef[p][1] / coef_scale;
                double c2 = (order >= 2)
                    ? (double)curve->poly_coef[p][2] / coef_scale : 0.0;
                log_msg("DOVI reshape [%s] piece %d: POLY order=%d "
                        "range=[%u,%u] coef=[%.6f, %.6f, %.6f]",
                        comp_names[c], p, order,
                        curve->pivots[p], curve->pivots[p + 1],
                        c0, c1, c2);
            } else if (curve->mapping_idc[p] == AV_DOVI_MAPPING_MMR) {
                log_msg("DOVI reshape [%s] piece %d: MMR order=%d "
                        "range=[%u,%u] constant=%.6f",
                        comp_names[c], p, curve->mmr_order[p],
                        curve->pivots[p], curve->pivots[p + 1],
                        (double)curve->mmr_constant[p] / coef_scale);
                /* Log MMR coefficient matrix for each order */
                for (int o = 0; o < curve->mmr_order[p] && o < 3; o++) {
                    log_msg("DOVI reshape [%s] piece %d: MMR[%d] "
                            "coef=[%.6f, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f]",
                            comp_names[c], p, o + 1,
                            (double)curve->mmr_coef[p][o][0] / coef_scale,
                            (double)curve->mmr_coef[p][o][1] / coef_scale,
                            (double)curve->mmr_coef[p][o][2] / coef_scale,
                            (double)curve->mmr_coef[p][o][3] / coef_scale,
                            (double)curve->mmr_coef[p][o][4] / coef_scale,
                            (double)curve->mmr_coef[p][o][5] / coef_scale,
                            (double)curve->mmr_coef[p][o][6] / coef_scale);
                }
            }
        }
    }

    /* ── NLQ parameters (if present) ── */
    if (mapping->nlq_method_idc != AV_DOVI_NLQ_NONE) {
        for (int c = 0; c < 3; c++) {
            log_msg("DOVI NLQ [%s]: offset=%u, vdr_in_max=%llu, "
                    "dz_slope=%llu, dz_threshold=%llu",
                    comp_names[c],
                    mapping->nlq[c].nlq_offset,
                    (unsigned long long)mapping->nlq[c].vdr_in_max,
                    (unsigned long long)mapping->nlq[c].linear_deadzone_slope,
                    (unsigned long long)mapping->nlq[c].linear_deadzone_threshold);
        }
    }

    /* ── Color Metadata ── */
    const AVDOVIColorMetadata *color = av_dovi_get_color(dovi);
    log_msg("DOVI color: dm_metadata_id=%u, scene_refresh=%u, "
            "signal_eotf=%u, signal_bit_depth=%u, signal_color_space=%u, "
            "signal_full_range=%u",
            color->dm_metadata_id, color->scene_refresh_flag,
            color->signal_eotf, color->signal_bit_depth,
            color->signal_color_space, color->signal_full_range_flag);
    log_msg("DOVI color: source_min_pq=%u, source_max_pq=%u, "
            "source_diagonal=%u",
            color->source_min_pq, color->source_max_pq,
            color->source_diagonal);

    /* YCC→RGB matrix (applied before PQ linearization) */
    log_msg("DOVI ycc_to_rgb_matrix:");
    for (int row = 0; row < 3; row++) {
        log_msg("  [%.6f  %.6f  %.6f]  offset=%.6f",
                av_q2d(color->ycc_to_rgb_matrix[row * 3 + 0]),
                av_q2d(color->ycc_to_rgb_matrix[row * 3 + 1]),
                av_q2d(color->ycc_to_rgb_matrix[row * 3 + 2]),
                av_q2d(color->ycc_to_rgb_offset[row]));
    }

    /* RGB→LMS matrix (applied after PQ linearization) */
    log_msg("DOVI rgb_to_lms_matrix:");
    for (int row = 0; row < 3; row++) {
        log_msg("  [%.6f  %.6f  %.6f]",
                av_q2d(color->rgb_to_lms_matrix[row * 3 + 0]),
                av_q2d(color->rgb_to_lms_matrix[row * 3 + 1]),
                av_q2d(color->rgb_to_lms_matrix[row * 3 + 2]));
    }
}

/* ── Dolby Vision Uniform Population ──
 *
 * Extracts reshape coefficients and color matrices from the DV RPU
 * metadata on each decoded frame and populates the GPU uniforms.
 * Called every frame; skips frames with no RPU side data.
 * Verbose logging only on first populate (state 1 → 2).
 *
 * The DV decode chain in the shader is:
 *   1. Reshape: affine per-component (poly_coef from RPU)
 *   2. ycc_to_rgb_matrix: ICtCp → PQ-encoded signal (with offsets)
 *   3. PQ EOTF → linear light
 *   4. Output matrix: precomputed (cone_inv × rgb_to_lms) → BT.2020 linear
 *
 * The ICtCp "cone" matrix (BT.2020 RGB → LMS, from ITU-R BT.2100):
 *   [1688/4096  2146/4096   262/4096]
 *   [ 683/4096  2951/4096   462/4096]
 *   [  99/4096   309/4096  3688/4096]
 *
 * Its inverse (LMS → BT.2020 linear RGB) is precomputed and multiplied
 * with rgb_to_lms on the CPU to save a shader matrix multiply. */

/* BT.2100 ICtCp inverse cone matrix (LMS → BT.2020 linear RGB) */
/* Crosstalk-FREE HPE inverse (DSVP main df16dc8). Dolby Vision outputs
 * BT.2020-referred HPE LMS; inverting the full BT.2100 cone matrix (which
 * carries the 4% crosstalk term) conjugated an uncompensated crosstalk
 * through every DV P5 pixel. Constants match libplacebo's dovi_lms2rgb. */
static const double ictcp_lms_to_bt2020[3][3] = {
    {  3.06441879, -2.16597676,  0.10155818 },
    { -0.65612108,  1.78554118, -0.12943749 },
    {  0.01736321, -0.04725154,  1.03004253 },
};

static void dovi_populate_uniforms(PlayerState *ps, const AVFrame *frame)
{
    if (ps->gpu_uniforms.is_dovi < 0.5f) return;
    if (ps->dovi_metadata_logged < 1) return; /* wait for logging pass */

    /* Per-frame RPU read — re-extract uniforms from each frame's side data */
    const AVFrameSideData *sd =
        av_frame_get_side_data(frame, AV_FRAME_DATA_DOVI_METADATA);
    if (!sd) return;

    const AVDOVIMetadata *dovi = (const AVDOVIMetadata *)sd->data;
    const AVDOVIRpuDataHeader *hdr = av_dovi_get_header(dovi);
    const AVDOVIDataMapping *mapping = av_dovi_get_mapping(dovi);
    const AVDOVIColorMetadata *color = av_dovi_get_color(dovi);

    double coef_scale = (double)(1LL << hdr->coef_log2_denom);

    /* ── Piecewise reshape coefficients (all pieces, all components) ──
     * DV spec allows up to 8 pieces per component with independent
     * polynomial coefficients. Pivots are normalized to [0,1] by
     * dividing by (2^bl_bit_depth - 1). Coefficients are packed into
     * float4 arrays indexed [piece][component] for GPU access. */
    float pivot_scale = (float)((1 << hdr->bl_bit_depth) - 1);
    if (pivot_scale < 1.0f) pivot_scale = 1023.0f; /* safety fallback */

    /* MMR banks default to off (orders 0) — set below if the RPU uses
     * MMR for a chroma component. Re-zeroed per frame: RPUs can switch
     * mapping method between scenes. */
    memset(ps->gpu_uniforms.dovi_mmr_meta, 0,
           sizeof(ps->gpu_uniforms.dovi_mmr_meta));
    memset(ps->gpu_uniforms.dovi_mmr_ct, 0,
           sizeof(ps->gpu_uniforms.dovi_mmr_ct));
    memset(ps->gpu_uniforms.dovi_mmr_cp, 0,
           sizeof(ps->gpu_uniforms.dovi_mmr_cp));

    for (int c = 0; c < 3; c++) {
        const AVDOVIReshapingCurve *curve = &mapping->curves[c];
        int num_pieces = 0;
        if (curve->num_pivots >= 2)
            num_pieces = curve->num_pivots - 1;
        if (num_pieces > 8) num_pieces = 8;
        ps->gpu_uniforms.dovi_num_pieces[c] = (float)num_pieces;

        /* Pack normalized pivots — [pivot_idx][component] */
        for (int i = 0; i < (int)curve->num_pivots && i < 9; i++)
            ps->gpu_uniforms.dovi_pivots[i][c] =
                (float)curve->pivots[i] / pivot_scale;

        /* Pack per-piece polynomial coefficients */
        for (int p = 0; p < num_pieces; p++) {
            if (curve->mapping_idc[p] == AV_DOVI_MAPPING_POLYNOMIAL) {
                int order = curve->poly_order[p];
                ps->gpu_uniforms.dovi_c0[p][c] =
                    (float)((double)curve->poly_coef[p][0] / coef_scale);
                ps->gpu_uniforms.dovi_c1[p][c] =
                    (order >= 1)
                    ? (float)((double)curve->poly_coef[p][1] / coef_scale)
                    : 0.0f;
                ps->gpu_uniforms.dovi_c2[p][c] =
                    (order >= 2)
                    ? (float)((double)curve->poly_coef[p][2] / coef_scale)
                    : 0.0f;
            } else if (curve->mapping_idc[p] == AV_DOVI_MAPPING_MMR
                       && (c == 1 || c == 2) && p == 0) {
                /* MMR chroma reshaping — the cross-channel polynomial
                 * nearly all real P5 content uses for Ct/Cp. Uniform
                 * budget carries ONE MMR bank per chroma component, so
                 * only piece 0 may be MMR; the shader dispatches the
                 * method after its pivot search, so on a multi-piece
                 * curve the remaining pieces still evaluate their own
                 * (polynomial) coefficients. The poly slot gets
                 * identity so a stray evaluation is harmless. */
                int order = curve->mmr_order[p];
                if (order < 1) order = 1;
                if (order > 3) order = 3;
                float *meta = ps->gpu_uniforms.dovi_mmr_meta;
                float (*bank)[4] = (c == 1) ? ps->gpu_uniforms.dovi_mmr_ct
                                            : ps->gpu_uniforms.dovi_mmr_cp;
                meta[c - 1] = (float)order;               /* x=Ct, y=Cp */
                meta[c + 1] =                             /* z=Ct, w=Cp */
                    (float)((double)curve->mmr_constant[p] / coef_scale);
                for (int o = 0; o < order; o++)
                    for (int t = 0; t < 7; t++) {
                        int idx = o * 7 + t;
                        bank[idx / 4][idx % 4] = (float)
                            ((double)curve->mmr_coef[p][o][t] / coef_scale);
                    }
                ps->gpu_uniforms.dovi_c0[p][c] = 0.0f;
                ps->gpu_uniforms.dovi_c1[p][c] = 1.0f;
                ps->gpu_uniforms.dovi_c2[p][c] = 0.0f;
            } else {
                /* MMR on I, or MMR on a piece > 0 (no uniform bank for
                 * it) — unseen in real content; identity passthrough
                 * for that piece's range only. */
                ps->gpu_uniforms.dovi_c0[p][c] = 0.0f;
                ps->gpu_uniforms.dovi_c1[p][c] = 1.0f;
                ps->gpu_uniforms.dovi_c2[p][c] = 0.0f;
                if (ps->dovi_metadata_logged < 2) {
                    const char *comp_names[] = {"I", "Ct", "Cp"};
                    log_msg("DOVI: WARNING — piece %d comp %s uses an "
                            "unsupported mapping shape, identity fallback",
                            p, comp_names[c]);
                }
            }
        }
    }
    ps->gpu_uniforms.dovi_num_pieces[3] = 0.0f; /* w component unused */

    /* ── ycc_to_rgb matrix + offsets → packed as float4 rows ──
     * Row format: [m0, m1, m2, offset] */
    for (int row = 0; row < 3; row++) {
        float *dst;
        switch (row) {
            case 0: dst = ps->gpu_uniforms.dovi_ycc_r0; break;
            case 1: dst = ps->gpu_uniforms.dovi_ycc_r1; break;
            default: dst = ps->gpu_uniforms.dovi_ycc_r2; break;
        }
        dst[0] = (float)av_q2d(color->ycc_to_rgb_matrix[row * 3 + 0]);
        dst[1] = (float)av_q2d(color->ycc_to_rgb_matrix[row * 3 + 1]);
        dst[2] = (float)av_q2d(color->ycc_to_rgb_matrix[row * 3 + 2]);
        dst[3] = (float)av_q2d(color->ycc_to_rgb_offset[row]);
    }

    /* ── Output matrix: precompute cone_inv × rgb_to_lms ──
     * Saves one 3×3 matmul per pixel in the shader. */
    double lms[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            lms[r][c] = av_q2d(color->rgb_to_lms_matrix[r * 3 + c]);

    for (int i = 0; i < 3; i++) {
        float *dst;
        switch (i) {
            case 0: dst = ps->gpu_uniforms.dovi_out_r0; break;
            case 1: dst = ps->gpu_uniforms.dovi_out_r1; break;
            default: dst = ps->gpu_uniforms.dovi_out_r2; break;
        }
        for (int j = 0; j < 3; j++) {
            double sum = 0.0;
            for (int k = 0; k < 3; k++)
                sum += ictcp_lms_to_bt2020[i][k] * lms[k][j];
            dst[j] = (float)sum;
        }
        dst[3] = 0.0f;
    }

    /* ── Peak nits from DV source_max_pq ──
     * More accurate than the 1000 nit fallback — DV RPU knows the actual
     * mastering peak. PQ code in 12-bit domain [0, 4095].
     * Updated per-frame; only logs when the peak changes. */
    if (color->source_max_pq > 0) {
        float pq_norm = (float)color->source_max_pq / 4095.0f;
        float dv_peak = pq_eotf_scalar(pq_norm);
        if (dv_peak > 100.0f) {
            float prev_peak = ps->hdr_static_peak;
            ps->gpu_uniforms.hdr_peak_nits = dv_peak;
            ps->hdr_static_peak = dv_peak;
            if (fabsf(dv_peak - prev_peak) > 0.5f) {
                log_msg("DOVI: source_max_pq=%u → peak=%.0f nits%s",
                        color->source_max_pq, dv_peak,
                        prev_peak < 1.0f ? " (initial)" : " (scene change)");
            }
        }
    }

    /* Log on first populate only (avoid per-frame log spam) */
    if (ps->dovi_metadata_logged < 2) {
        log_msg("DOVI: uniforms populated — pieces I=%d Ct=%d Cp=%d",
                (int)ps->gpu_uniforms.dovi_num_pieces[0],
                (int)ps->gpu_uniforms.dovi_num_pieces[1],
                (int)ps->gpu_uniforms.dovi_num_pieces[2]);
        /* Log piece-0 coefficients as representative sample */
        log_msg("DOVI: piece 0 — c0=[%.4f,%.4f,%.4f] c1=[%.4f,%.4f,%.4f] "
                "c2=[%.4f,%.4f,%.4f]",
                ps->gpu_uniforms.dovi_c0[0][0], ps->gpu_uniforms.dovi_c0[0][1],
                ps->gpu_uniforms.dovi_c0[0][2],
                ps->gpu_uniforms.dovi_c1[0][0], ps->gpu_uniforms.dovi_c1[0][1],
                ps->gpu_uniforms.dovi_c1[0][2],
                ps->gpu_uniforms.dovi_c2[0][0], ps->gpu_uniforms.dovi_c2[0][1],
                ps->gpu_uniforms.dovi_c2[0][2]);
        log_msg("DOVI: output matrix (cone_inv × rgb_to_lms):");
        log_msg("  [%.6f  %.6f  %.6f]", ps->gpu_uniforms.dovi_out_r0[0],
                ps->gpu_uniforms.dovi_out_r0[1], ps->gpu_uniforms.dovi_out_r0[2]);
        log_msg("  [%.6f  %.6f  %.6f]", ps->gpu_uniforms.dovi_out_r1[0],
                ps->gpu_uniforms.dovi_out_r1[1], ps->gpu_uniforms.dovi_out_r1[2]);
        log_msg("  [%.6f  %.6f  %.6f]", ps->gpu_uniforms.dovi_out_r2[0],
                ps->gpu_uniforms.dovi_out_r2[1], ps->gpu_uniforms.dovi_out_r2[2]);
    }

    /* Mark as populated at least once (allows log pass gate, peak shortcut) */
    ps->dovi_metadata_logged = 2;
}

#define PEAK_ATTACK_RATE    0.3f     /* rise towards new peak per frame   */
#define PEAK_DECAY_RATE     0.03f    /* decay towards new peak per frame  */
#define PEAK_SCENE_CUT_THR  0.5f     /* 50% increase = scene cut, jump up */
#define PEAK_MIN_NITS       100.0f   /* floor to prevent near-zero peaks   */
#define PEAK_PERCENTILE     99.875f  /* skip top 0.125% (specular hotspots) */

/* Scan the Y plane, build histogram, extract percentile peak,
 * convert to nits, smooth, and update the uniform.
 * Called once per frame from video_display() for HDR content only. */
/* is_p010: 10-bit samples stored shifted left by 6 (VAAPI P010). Raw
 * yuv420p10le holds unshifted 0-1023 codes and needs a different shift to
 * reach the top 8 bits — using >>8 on raw 10-bit collapsed the whole
 * 256-bin histogram into bins 0-3, quantising the detected scene peak to
 * four possible values and making the tone curve lurch between them.
 * Reached by 10-bit AV1 HDR (always software-decoded) and DSVP_HWDEC=0. */
static void hdr_compute_scene_peak(PlayerState *ps, const AVFrame *frame,
                                   int is_10bit, int is_p010)
{
    /* Skip if not HDR or in PQ bypass debug mode */
    if (ps->gpu_uniforms.is_hdr < 0.5f) return;
    if (ps->gpu_uniforms.hdr_debug > 1.5f && ps->gpu_uniforms.hdr_debug < 2.5f)
        return;  /* mode 2: PQ bypass, use static peak */

    /* DV Profile 5: skip CPU histogram — I-plane is IPTPQc2, not PQ luma.
     * Histogram reads garbage, stuck at PEAK_MIN_NITS floor.  Use the
     * static peak from source_max_pq (updated per-frame by dovi_populate_uniforms). */
    if (ps->gpu_uniforms.is_dovi > 0.5f) {
        ps->hdr_smoothed_peak = ps->hdr_static_peak;
        ps->hdr_prev_frame_peak = ps->hdr_static_peak;
        ps->gpu_uniforms.hdr_peak_nits = ps->hdr_static_peak;
        return;
    }

    /* ── Frame skip: run histogram every 4th frame ──
     * Temporal smoothing handles gradual changes; skipped frames
     * retain the previous smoothed peak (already in the uniform).
     * Cuts CPU cost by 75% with no visible quality loss.
     * Frame 0 always runs (initial peak acquisition). */
    if (ps->diag_frames_displayed > 0
            && (ps->diag_frames_displayed % 4) != 0) {
        return;
    }

    const uint8_t *data = frame->data[0];
    int stride = frame->linesize[0];
    int w = ps->vid_w;
    int h = ps->vid_h;

    /* ── Build 256-bin histogram of Y plane ──
     * Subsample 8× in each dimension to reduce work.
     * At 4K: (3840/8) × (2160/8) = 129,600 samples — still robust
     * for a 256-bin histogram with 99.875th percentile readout.
     * For 10-bit: bin = uint16 >> 8 (top 8 bits → 256 bins).
     * For 8-bit:  bin = uint8 value directly. */
    int histogram[256];
    memset(histogram, 0, sizeof(histogram));
    int total_samples = 0;

    if (is_10bit) {
        for (int y = 0; y < h; y += 8) {
            const uint16_t *row = (const uint16_t *)(data + y * stride);
            for (int x = 0; x < w; x += 8) {
                histogram[row[x] >> (is_p010 ? 8 : 2)]++;
                total_samples++;
            }
        }
    } else {
        for (int y = 0; y < h; y += 8) {
            const uint8_t *row = data + y * stride;
            for (int x = 0; x < w; x += 8) {
                histogram[row[x]]++;
                total_samples++;
            }
        }
    }

    /* ── Find the 99.875th percentile bin ──
     * Walk from the top bin downward, accumulating counts until
     * we've passed (100 - PEAK_PERCENTILE)% of total samples. */
    int skip_count = (int)((100.0f - PEAK_PERCENTILE) / 100.0f * total_samples);
    if (skip_count < 1) skip_count = 1;

    int accumulated = 0;
    int percentile_bin = 255;
    for (int i = 255; i >= 0; i--) {
        accumulated += histogram[i];
        if (accumulated >= skip_count) {
            percentile_bin = i;
            break;
        }
    }

    /* ── Convert bin to normalized value [0,1] ──
     * Use bin center: (bin + 0.5) / 256 for 10-bit (maps back to uint16 space).
     * For 8-bit: (bin + 0.5) / 256 ≈ bin / 255 (close enough). */
    float raw_max_norm;
    if (is_10bit) {
        /* Invert the binning shift above: P010 stores code<<6 and bins
         * with >>8 (one bin = 256 uint16 steps); raw 10-bit stores the
         * bare code and bins with >>2 (one bin = 4 steps). Using the
         * P010 factor for raw 10-bit read 64x hot — pq_code clamped to
         * 1.0 every frame and the dynamic peak pinned at the static
         * ceiling on all software 10-bit HDR (review P1-1). */
        float bin_step = is_p010 ? 256.0f : 4.0f;
        raw_max_norm = ((float)percentile_bin + 0.5f) * bin_step / 65535.0f;
    } else {
        raw_max_norm = ((float)percentile_bin + 0.5f) / 256.0f;
    }

    /* ── Apply range expansion (same math as shader) ──
     * Convert from texture-space to PQ code [0,1] */
    float pq_code = (raw_max_norm - ps->gpu_uniforms.rangeY[0])
                  * ps->gpu_uniforms.rangeY[1];
    if (pq_code < 0.0f) pq_code = 0.0f;
    if (pq_code > 1.0f) pq_code = 1.0f;

    /* ── PQ → linear nits ── */
    float raw_peak_nits = pq_eotf_scalar(pq_code);

    /* ── Temporal smoothing ── */
    float smoothed = ps->hdr_smoothed_peak;
    float prev     = ps->hdr_prev_frame_peak;

    if (smoothed < 1.0f) {
        /* First frame — initialize directly */
        smoothed = raw_peak_nits;
    } else {
        /* Scene cut detection: large *increase* from previous frame → jump up.
         * Downward changes always use smooth decay to prevent strobe flicker
         * when dark frames temporarily depress the peak. */
        float change = (raw_peak_nits - prev) / fmaxf(prev, 1.0f);
        if (change > PEAK_SCENE_CUT_THR) {
            /* Bright scene cut — jump to avoid highlight clipping */
            smoothed = raw_peak_nits;
        } else if (raw_peak_nits > smoothed) {
            /* Attack: scene getting brighter — rise quickly */
            smoothed += PEAK_ATTACK_RATE * (raw_peak_nits - smoothed);
        } else {
            /* Decay: scene getting darker — fade slowly */
            smoothed += PEAK_DECAY_RATE * (raw_peak_nits - smoothed);
        }
    }

    /* Clamp: floor at PEAK_MIN_NITS, ceiling at static metadata peak */
    if (smoothed < PEAK_MIN_NITS) smoothed = PEAK_MIN_NITS;
    if (ps->hdr_static_peak > 0.0f && smoothed > ps->hdr_static_peak)
        smoothed = ps->hdr_static_peak;

    /* Update state */
    ps->hdr_smoothed_peak   = smoothed;
    ps->hdr_prev_frame_peak = raw_peak_nits;

    /* Feed dynamic peak to the tone mapper */
    ps->gpu_uniforms.hdr_peak_nits = smoothed;

    /* Periodic log (every 120 frames ≈ 5s at 24fps) */
    if (ps->diag_frames_displayed % 120 == 0) {
        float target = ps->gpu_uniforms.hdr_target_nits;
        float maxLum = target / smoothed;
        float ks = 1.5f * maxLum - 0.5f;
        if (ks < 0.0f) ks = 0.0f;
        log_msg("HDR peak: raw=%.0f nits, smoothed=%.0f nits "
                "(p%.1f, target=%.0f, static=%.0f, KS=%.3f, maxLum=%.4f)",
                raw_peak_nits, smoothed, PEAK_PERCENTILE,
                target, ps->hdr_static_peak, ks, maxLum);
    }
}


/* Display the current video frame: upload to GPU → shader draw.
 *
 * This is the hot path. Called once per new frame from main.c.
 *
 * Three source modes, all using the YUV planar pipeline (3 textures):
 *   1. 10-bit passthrough: direct upload, 2 bytes/sample (R16_UNORM)
 *   2. 8-bit YUV420P passthrough: direct upload, 1 byte/sample (R8_UNORM)
 *      Range expansion (limited→full) done in fragment shader.
 *   3. All other formats: swscale → upload, 1 byte/sample (R8_UNORM)
 */

/* Defined below video_reblit — shared by both render entry points. */
static void render_video_pass(PlayerState *ps, SDL_GPUCommandBuffer *cmd,
                              SDL_GPUTexture *target,
                              Uint32 sc_w, Uint32 sc_h,
                              int use_zc_uv, int draw_overlay);
static void blit_and_overlay(PlayerState *ps, SDL_GPUCommandBuffer *cmd,
                             SDL_GPUTexture *swapchain_tex,
                             Uint32 sc_w, Uint32 sc_h);

void video_display(PlayerState *ps) {
    if (!ps->gpu_tex_y || !ps->video_frame) return;
    /* In zero-copy mode, video_frame is a raw VAAPI surface: data[3] = VASurfaceID,
     * data[0] may be NULL. In readback mode, data[0] has CPU pixel data. */
    if (!ps->vaapi_zerocopy && !ps->video_frame->data[0]) return;
    if (ps->vaapi_zerocopy && !ps->video_frame->data[3]) return;
    if (ps->seeking) return;

    /* ── Mid-stream resolution change guard ──
     * Every upload path below derives row counts and byte widths from
     * the open-time vid_w/vid_h: a frame decoded at a different size
     * would be OVER-READ by the readback deinterleave loops and
     * sws_scale (heap over-read, not just a wrong picture), or bind
     * mismatched in the zero-copy import. Skip such frames. Latched
     * per file, not per process — a static here would silence every
     * later file's warning (DSVP main 7f09ae0). */
    if (ps->video_frame->width > 0 && ps->video_frame->height > 0 &&
        (ps->video_frame->width  != ps->vid_w ||
         ps->video_frame->height != ps->vid_h)) {
        if (!ps->res_change_logged) {
            log_msg("WARN: mid-stream resolution change %dx%d -> %dx%d — "
                    "frames at the new size are skipped",
                    ps->vid_w, ps->vid_h,
                    ps->video_frame->width, ps->video_frame->height);
            ps->res_change_logged = 1;
        }
        return;
    }

    int zerocopy_ok = 0;  /* 1 = zero-copy upload succeeded this frame */

#ifdef DSVP_PROFILE
    double t_enter = get_time_sec();
#endif

    int w  = ps->vid_w;
    int h  = ps->vid_h;
    int cw = (w + 1) / 2;   /* ceil — matches texture + FFmpeg alloc */
    int ch = (h + 1) / 2;

    /* ── Determine source frame and byte width ── */
    AVFrame *src_frame = NULL;
    int bpp = 0;  /* bytes per sample for upload_plane */

    int is_10bit_passthrough =
        (ps->video_codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P10LE
         && !ps->sws_ctx);

    if (ps->vaapi_active) {
        if (ps->vaapi_zerocopy && ps->gpu_tex_uv) {
            /* ── VAAPI zero-copy path ──
             *
             * DMA-BUF export → Vulkan import → vkCmdCopyImage to SDL textures.
             * Eliminates 35-42ms GPU→CPU readback entirely. */
            if (vaapi_zerocopy_upload(ps) == 0) {
                ps->gpu_uniforms.is_semiplanar = 1.0f;
                zerocopy_ok = 1;
            } else {
                /* Zero-copy failed — disable permanently, fall through to readback.
                 * This frame will be dropped (no CPU data available). Log and continue. */
                log_msg("ZEROCOPY: upload failed — disabling, readback fallback next frame");
                ps->vaapi_zerocopy = 0;
                ps->gpu_uniforms.is_semiplanar = 0.0f;
                /* Stop reblitting until real planar data lands. The binding
                 * rule just flipped from semi-planar (gpu_tex_uv) to planar
                 * (gpu_tex_u/gpu_tex_v), but those two textures have never
                 * been uploaded in zero-copy mode — every reblit between here
                 * and the next decoded frame would sample their creation-time
                 * contents and paint garbage chroma over a correct picture.
                 * video_display sets this back to 1 after a real upload. */
                ps->video_ready = 0;
                /* Can't recover this frame (no readback data), so return early.
                 * The decode thread will provide readback data on the next frame. */
                return;
            }
        }
        if (!zerocopy_ok)
        {
        /* ── VAAPI semi-planar path ──
         *
         * Both NV12 and P010 are semi-planar: Y plane + interleaved UV plane.
         * NV12: uint8 samples (1 byte each).  P010: uint16 samples (2 bytes each).
         * Deinterleave UV into separate U and V planes on CPU, then upload
         * all three to the matching texture format (R8_UNORM or R16_UNORM).
         *
         * This is a memory shuffle, not a math operation — every sample
         * value is preserved bit-exact. */
        AVFrame *f = ps->video_frame;
        int uv_stride = f->linesize[1];

        if (ps->vaapi_nv12) {
            /* NV12: uint8 interleaved UV → separate U[], V[] */
            for (int row = 0; row < ch; row++) {
                const uint8_t *uv_row = f->data[1] + row * uv_stride;
                uint8_t *u_row = ps->p010_u_plane + row * cw;
                uint8_t *v_row = ps->p010_v_plane + row * cw;
                for (int x = 0; x < cw; x++) {
                    u_row[x] = uv_row[x * 2];
                    v_row[x] = uv_row[x * 2 + 1];
                }
            }

            /* Upload Y plane (1 byte/sample) — set 2, the main
             * thread's own (decode thread never stages VAAPI). */
            upload_plane(ps->gpu_device, ps->gpu_xfer_y[2],
                         f->data[0], f->linesize[0], w, h);
            /* Upload deinterleaved U and V planes (1 byte/sample) */
            upload_plane(ps->gpu_device, ps->gpu_xfer_u[2],
                         ps->p010_u_plane, cw, cw, ch);
            upload_plane(ps->gpu_device, ps->gpu_xfer_v[2],
                         ps->p010_v_plane, cw, cw, ch);
        } else {
            /* P010: uint16 interleaved UV → separate U[], V[] */
            for (int row = 0; row < ch; row++) {
                const uint16_t *uv_row = (const uint16_t *)(f->data[1] + row * uv_stride);
                uint16_t *u_row = (uint16_t *)(ps->p010_u_plane + row * cw * 2);
                uint16_t *v_row = (uint16_t *)(ps->p010_v_plane + row * cw * 2);
                for (int x = 0; x < cw; x++) {
                    u_row[x] = uv_row[x * 2];
                    v_row[x] = uv_row[x * 2 + 1];
                }
            }

            /* Upload Y plane (2 bytes/sample) — set 2, main-thread own */
            upload_plane(ps->gpu_device, ps->gpu_xfer_y[2],
                         f->data[0], f->linesize[0], w * 2, h);
            /* Upload deinterleaved U and V planes (2 bytes/sample) */
            upload_plane(ps->gpu_device, ps->gpu_xfer_u[2],
                         ps->p010_u_plane, cw * 2, cw * 2, ch);
            upload_plane(ps->gpu_device, ps->gpu_xfer_v[2],
                         ps->p010_v_plane, cw * 2, cw * 2, ch);
        }
        } /* end if (!zerocopy_ok) */

    } else if (is_10bit_passthrough) {
        /* 10-bit passthrough — raw frame directly to R16_UNORM textures */
        src_frame = ps->video_frame;
        bpp = 2;
    } else if (!ps->sws_ctx) {
        /* 8-bit YUV420P passthrough — direct upload, range in shader */
        src_frame = ps->video_frame;
        bpp = 1;
    } else {
        /* swscale path — format conversion to yuv420p / yuv420p10le */
        sws_scale(ps->sws_ctx,
            (const uint8_t *const *)ps->video_frame->data,
            ps->video_frame->linesize,
            0, ps->vid_h,
            ps->rgb_frame->data,
            ps->rgb_frame->linesize);
        src_frame = ps->rgb_frame;
        bpp = ps->sws_out_10bit ? 2 : 1;
    }

    /* ── Dolby Vision RPU metadata extraction ──
     * Extract and log reshaping curves from first DV frame.
     * Uses original decoded frame (side data not on swscale output). */
    dovi_log_frame_metadata(ps, ps->video_frame);
    dovi_populate_uniforms(ps, ps->video_frame);

    /* ── HDR dynamic peak detection (CPU scan) ──
     * Scan luma plane to find actual scene peak before uploading.
     * Updates hdr_peak_nits uniform with temporally smoothed value.
     *
     * For VAAPI: video_frame has the hw-transferred data (P010 or NV12).
     * P010 Y plane is uint16 (10-bit), NV12 Y plane is uint8 (8-bit).
     * For software decode: src_frame has the data, is_10bit_passthrough
     * indicates whether it's 10-bit YUV420P10LE. */
    {
        AVFrame *peak_frame = ps->vaapi_active ? ps->video_frame : src_frame;
        /* Software side: 10-bit whenever the upload is 10-bit — the
         * sws-10le path scans the converted frame, not the source. */
        int peak_is_10bit = ps->vaapi_active ? !ps->vaapi_nv12 : (bpp == 2);
#ifdef DSVP_PROFILE
        double t_before_peak = get_time_sec();
#endif
        if (ps->gpu_uniforms.is_hlg > 0.5f) {
            /* HLG: fixed 1000-nit OOTF, peak pinned at open. The
             * histogram converts bins through the PQ EOTF — meaningless
             * for HLG's relative signal — so it must not run
             * (DSVP main fdbb489). */
        } else if (zerocopy_ok) {
            /* Zero-copy: no CPU pixel data for luma histogram.
             * Use static metadata peak (from container or DV RPU).
             * DV P5 already uses static peak regardless. */
            ps->gpu_uniforms.hdr_peak_nits = ps->hdr_static_peak;
        } else
        {
            /* P010 storage exactly when VAAPI produced the frame in 10-bit */
            hdr_compute_scene_peak(ps, peak_frame, peak_is_10bit,
                                   ps->vaapi_active && !ps->vaapi_nv12);
        }
#ifdef DSVP_PROFILE
        double t_after_peak = get_time_sec();
        ps->prof_peak_ms = (t_after_peak - t_before_peak) * 1000.0;
#endif
    }

    /* ── Upload plane data to GPU transfer buffers ──
     * (VAAPI readback path does its own upload above.
     *  Zero-copy did vkCmdCopyImage — skip everything.)
     * When the decode thread pre-staged this frame (review M1,
     * video_frame_xfer >= 0), the planes already sit in that set and
     * this 12.4MB main-thread memcpy — formerly the largest single
     * CPU cost on the vsync-gated thread — is skipped entirely; the
     * copy pass below binds the staged set instead. */
    if (!ps->vaapi_active && !zerocopy_ok && ps->video_frame_xfer < 0
            && ps->video_frame_slot < 0) {
        upload_plane(ps->gpu_device, ps->gpu_xfer_y[2],
                     src_frame->data[0], src_frame->linesize[0], w * bpp, h);
        upload_plane(ps->gpu_device, ps->gpu_xfer_u[2],
                     src_frame->data[1], src_frame->linesize[1], cw * bpp, ch);
        upload_plane(ps->gpu_device, ps->gpu_xfer_v[2],
                     src_frame->data[2], src_frame->linesize[2], cw * bpp, ch);
    }

    /* ── GPU command buffer ── */
#ifdef DSVP_PROFILE
    double t_before_gpu = get_time_sec();
    ps->prof_upload_ms = (t_before_gpu - t_enter) * 1000.0 - ps->prof_peak_ms;
#endif
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(ps->gpu_device);
    if (!cmd) {
        log_msg("ERROR: SDL_AcquireGPUCommandBuffer failed: %s", SDL_GetError());
        return;
    }

    /* ── Copy pass: transfer buffers → GPU textures ──
     * Skipped in zero-copy mode (data already in textures via Vulkan).
     * xs = the set holding this frame's planes: the decode thread's
     * staged set when prestaged, the main thread's set 2 otherwise. */
    if (!zerocopy_ok) {
    int xs = (ps->video_frame_xfer >= 0 && ps->video_frame_xfer <= 1)
             ? ps->video_frame_xfer : 2;
    /* Pool-backed frame: bind the slot the DECODER wrote directly
     * (get_buffer2 — no staging copy ever happened), with its padded
     * pitch. pixels_per_row carries the pitch, so the padded layout
     * uploads without repacking; pool is 8-bit, pixels == bytes. */
    int psl = (ps->video_frame_slot >= 0
               && ps->video_frame_slot < ps->xfer_pool_n)
              ? ps->video_frame_slot : -1;
    SDL_GPUTransferBuffer *tb_y, *tb_u, *tb_v;
    Uint32 ppr_y, ppr_c;
    if (psl >= 0) {
        tb_y  = ps->xfer_pool[psl].xy;
        tb_u  = ps->xfer_pool[psl].xu;
        tb_v  = ps->xfer_pool[psl].xv;
        ppr_y = (Uint32)ps->xfer_pool_pitch_y;
        ppr_c = (Uint32)ps->xfer_pool_pitch_uv;
    } else {
        tb_y  = ps->gpu_xfer_y[xs];
        tb_u  = ps->gpu_xfer_u[xs];
        tb_v  = ps->gpu_xfer_v[xs];
        ppr_y = (Uint32)w;
        ppr_c = (Uint32)cw;
    }
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    {
        SDL_GPUTextureTransferInfo src_info;
        SDL_GPUTextureRegion dst_region;

        /* Y plane */
        SDL_zero(src_info);
        SDL_zero(dst_region);
        src_info.transfer_buffer = tb_y;
        src_info.pixels_per_row  = ppr_y;
        src_info.rows_per_layer  = h;
        dst_region.texture = ps->gpu_tex_y;
        dst_region.w = w;
        dst_region.h = h;
        dst_region.d = 1;
        SDL_UploadToGPUTexture(copy, &src_info, &dst_region, true);

        /* U plane */
        SDL_zero(src_info);
        SDL_zero(dst_region);
        src_info.transfer_buffer = tb_u;
        src_info.pixels_per_row  = ppr_c;
        src_info.rows_per_layer  = ch;
        dst_region.texture = ps->gpu_tex_u;
        dst_region.w = cw;
        dst_region.h = ch;
        dst_region.d = 1;
        SDL_UploadToGPUTexture(copy, &src_info, &dst_region, true);

        /* V plane */
        SDL_zero(src_info);
        SDL_zero(dst_region);
        src_info.transfer_buffer = tb_v;
        src_info.pixels_per_row  = ppr_c;
        src_info.rows_per_layer  = ch;
        dst_region.texture = ps->gpu_tex_v;
        dst_region.w = cw;
        dst_region.h = ch;
        dst_region.d = 1;
        SDL_UploadToGPUTexture(copy, &src_info, &dst_region, true);
    }
    SDL_EndGPUCopyPass(copy);
    } /* end if (!zerocopy_ok) copy pass */

    /* ── Overlay copy pass (if dirty) ── */
    gpu_overlay_copy_cmd(cmd, ps);

    /* ── Acquire swapchain texture ── */
    SDL_GPUTexture *swapchain_tex = NULL;
    Uint32 sc_w, sc_h;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, ps->window,
            &swapchain_tex, &sc_w, &sc_h)) {
        log_msg("ERROR: Swapchain acquire failed: %s", SDL_GetError());
        SDL_CancelGPUCommandBuffer(cmd);
        return;
    }
    if (!swapchain_tex) {
        SDL_CancelGPUCommandBuffer(cmd);
        return;
    }

    /* Cache physical pixel dimensions for DPI-correct overlay sizing */
    ps->sc_w = (int)sc_w;
    ps->sc_h = (int)sc_h;

#ifdef DSVP_PROFILE
    double t_after_vsync = get_time_sec();
    ps->prof_vsync_ms = (t_after_vsync - t_before_gpu) * 1000.0;
#endif

    /* ── Render: video shader at content rate, blit at present rate ── */
    if (!ps->no_intermediate && ps->intermediate_apt
            && gpu_frame_tex_ensure(ps, (int)sc_w, (int)sc_h) == 0) {
        render_video_pass(ps, cmd, ps->gpu_tex_frame, sc_w, sc_h,
                          zerocopy_ok && ps->gpu_tex_uv != NULL, 0);
        ps->frame_tex_valid = 1;
        ps->frame_render_dirty = 0;
        blit_and_overlay(ps, cmd, swapchain_tex, sc_w, sc_h);
    } else {
        /* Direct path (fallback / DSVP_NO_INTERMEDIATE) — identical to
         * the pre-intermediate renderer. */
        render_video_pass(ps, cmd, swapchain_tex, sc_w, sc_h,
                          zerocopy_ok && ps->gpu_tex_uv != NULL, 1);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    ps->presents++;

#ifdef DSVP_PROFILE
    {
        double t_exit = get_time_sec();
        ps->prof_render_ms  = (t_exit - t_after_vsync) * 1000.0;
        ps->prof_display_ms = (t_exit - t_enter) * 1000.0;

        /* Spike log: flag individual frames that exceed budget.
         * At 60fps the VSync period is 16.67ms, so total includes
         * ~14ms of normal VSync wait. Only log genuine anomalies.
         * 20ms total = missed VSync boundary by ~3ms. */
        if (ps->prof_peak_ms > 3.0 || ps->prof_upload_ms > 8.0
                || ps->prof_display_ms > 20.0) {
            /* Rate-limited to 1/s: with the loop ticking 19-21ms the
             * 20ms line fires on ~every other frame, and each line is
             * 3 unbuffered write syscalls — the profiler was
             * perturbing the loop it measures. Suppressed spikes are
             * counted onto the next emitted line; the 10s DIAG means
             * are unaffected either way. */
            static double s_spike_last = 0.0;
            static int    s_spike_supp = 0;
            double now_sp = get_time_sec();
            if (now_sp - s_spike_last >= 1.0) {
                log_msg("PROF SPIKE: upload=%.1f peak=%.1f vsync=%.1f "
                        "render=%.1f total=%.1fms (frame %d, +%d suppressed)",
                        ps->prof_upload_ms, ps->prof_peak_ms,
                        ps->prof_vsync_ms, ps->prof_render_ms,
                        ps->prof_display_ms, ps->diag_frames_displayed,
                        s_spike_supp);
                s_spike_last = now_sp;
                s_spike_supp = 0;
            } else {
                s_spike_supp++;
            }
        }

        /* Running stats (reset by main.c every 10s DIAG) */
        ps->prof_n++;
        ps->prof_sum_upload += ps->prof_upload_ms;
        ps->prof_sum_peak   += ps->prof_peak_ms;
        ps->prof_sum_vsync  += ps->prof_vsync_ms;
        ps->prof_sum_total  += ps->prof_display_ms;
        if (ps->prof_upload_ms > ps->prof_max_upload)
            ps->prof_max_upload = ps->prof_upload_ms;
        if (ps->prof_peak_ms > ps->prof_max_peak)
            ps->prof_max_peak = ps->prof_peak_ms;
        if (ps->prof_vsync_ms > ps->prof_max_vsync)
            ps->prof_max_vsync = ps->prof_vsync_ms;
        if (ps->prof_display_ms > ps->prof_max_total)
            ps->prof_max_total = ps->prof_display_ms;
    }
#endif

    ps->video_ready = 1;
}


/* ── Shared video render pass ──
 * Runs the full YUV/DV/HDR shader into `target` (the intermediate
 * frame texture, or the swapchain directly in fallback mode). When
 * draw_overlay is set (direct-to-swapchain mode) the overlay quad is
 * composited inside the same pass, exactly as the pre-intermediate
 * code did. */
static void render_video_pass(PlayerState *ps, SDL_GPUCommandBuffer *cmd,
                              SDL_GPUTexture *target,
                              Uint32 sc_w, Uint32 sc_h,
                              int use_zc_uv, int draw_overlay) {
    SDL_GPUColorTargetInfo color_target;
    SDL_zero(color_target);
    color_target.texture     = target;
    color_target.clear_color = (SDL_FColor){ 0.0f, 0.0f, 0.0f, 1.0f };
    color_target.load_op     = SDL_GPU_LOADOP_CLEAR;
    color_target.store_op    = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, NULL);
    {
        player_update_display_rect(ps);
        float scale_x = (sc_w > 0) ? (float)sc_w / ps->win_w : 1.0f;
        float scale_y = (sc_h > 0) ? (float)sc_h / ps->win_h : 1.0f;

        SDL_GPUViewport viewport;
        viewport.x = ps->display_rect.x * scale_x;
        viewport.y = ps->display_rect.y * scale_y;
        viewport.w = ps->display_rect.w * scale_x;
        viewport.h = ps->display_rect.h * scale_y;
        viewport.min_depth = 0.0f;
        viewport.max_depth = 1.0f;

        /* Sampler variant: dilated kernels only when the viewport is
         * actually smaller than the video — the only case where the
         * two variants differ in output. At 1:1/upscale the fixed
         * unrolled path runs (proven fast under UMA contention). */
        int downscale = (viewport.w + 0.5f < (float)ps->vid_w)
                     || (viewport.h + 0.5f < (float)ps->vid_h);
        /* Pipeline set must match the attachment format: frame-target
         * passes use the RGBA16 variants, swapchain passes the
         * swapchain variants (review P1-6). A missing dilated-frame
         * variant falls back to the fixed FRAME pipeline — never to a
         * swapchain-format pipeline on a frame target. */
        int to_frame = (target == ps->gpu_tex_frame && target != NULL);
        /* Exact 1:1 (4K-on-4K fullscreen): the kernels reproduce the
         * identity at ~16 taps/plane; the direct variant fetches once.
         * Exactness matters — a half-pixel mismatch would bilinear-blur
         * the whole frame, so anything not an exact match keeps its
         * kernel. */
        /* Size alone is not exactness: with a fractional viewport
         * ORIGIN every fragment lands between texel centres, so the
         * LINEAR sampler blends — bilinear across the whole frame on
         * the one path that exists to be exact — and scale2x's
         * constant weights are valid only at phases {0.25, 0.75},
         * which hold only for integer origins (Knot audit finding 6).
         * Benign today (fullscreen/letterboxed origins are 0 or
         * integer), fires under fractional display scaling or any
         * future inset/PiP rect. The transition log reports the
         * honest fallback to the fixed kernel. */
        int origin_integer = viewport.x == floorf(viewport.x)
                          && viewport.y == floorf(viewport.y);
        int one2one = fabsf(viewport.w - (float)ps->vid_w) < 0.5f
                   && fabsf(viewport.h - (float)ps->vid_h) < 0.5f
                   && origin_integer;
        /* Exact 2.0x on both axes (the HiDPI-2.0 fullscreen shape for
         * 1080p-class sources): constant-weight Lanczos applies. */
        int two_x = fabsf(viewport.w - 2.0f * (float)ps->vid_w) < 0.5f
                 && fabsf(viewport.h - 2.0f * (float)ps->vid_h) < 0.5f
                 && origin_integer;
        SDL_GPUGraphicsPipeline *pipe_fixed = to_frame
            ? ps->gpu_pipeline_yuv_frame : ps->gpu_pipeline_yuv;
        SDL_GPUGraphicsPipeline *pipe_dil = to_frame
            ? ps->gpu_pipeline_yuv_dilated_frame : ps->gpu_pipeline_yuv_dilated;
        SDL_GPUGraphicsPipeline *pipe_dir = to_frame
            ? ps->gpu_pipeline_yuv_direct_frame : ps->gpu_pipeline_yuv_direct;
        SDL_GPUGraphicsPipeline *pipe_2x = to_frame
            ? ps->gpu_pipeline_yuv_scale2x_frame : ps->gpu_pipeline_yuv_scale2x;
        SDL_GPUGraphicsPipeline *pipe =
            (downscale && pipe_dil) ? pipe_dil
          : (one2one && pipe_dir)   ? pipe_dir
          : (two_x && pipe_2x)      ? pipe_2x
          : pipe_fixed;
        {
            /* State lives in PlayerState (not a local static) so the
             * debug panel reports the variant actually bound. */
            int now_variant = (downscale && pipe_dil) ? 1
                            : (one2one && pipe_dir)   ? 2
                            : (two_x && pipe_2x)      ? 3 : 0;
            if (now_variant != ps->active_variant) {
                ps->active_variant = now_variant;
                log_msg("GPU: sampler variant → %s (vp %.0fx%.0f vs src %dx%d)",
                        now_variant == 1 ? "dilated (downscale)"
                      : now_variant == 2 ? "direct (exact 1:1)"
                      : now_variant == 3 ? "scale2x (constant weights)"
                                         : "fixed 4x4",
                        viewport.w, viewport.h, ps->vid_w, ps->vid_h);
            }
        }
        SDL_BindGPUGraphicsPipeline(pass, pipe);
        SDL_SetGPUViewport(pass, &viewport);

        /* GEOM DIAG - log the whole geometry chain whenever it changes, so the
         * shape actually being scanned out can be compared between windowed and
         * exclusive fullscreen numerically instead of judged by eye. If the
         * viewport aspect matches the source aspect, any remaining distortion
         * is downstream of this process. */
        {
            static float lvx = -1, lvy = -1, lvw = -1, lvh = -1;
            if (viewport.x != lvx || viewport.y != lvy ||
                viewport.w != lvw || viewport.h != lvh) {
                lvx = viewport.x; lvy = viewport.y;
                lvw = viewport.w; lvh = viewport.h;
                log_msg("GEOM: src=%dx%d (%.4f) win=%dx%d sc=%ux%u "
                        "rect=%d,%d %dx%d scale=%.3f,%.3f -> vp=%.1f,%.1f %.1fx%.1f (%.4f)%s",
                        ps->vid_w, ps->vid_h,
                        ps->vid_h ? (double)ps->vid_w / ps->vid_h : 0.0,
                        ps->win_w, ps->win_h, sc_w, sc_h,
                        ps->display_rect.x, ps->display_rect.y,
                        ps->display_rect.w, ps->display_rect.h,
                        scale_x, scale_y,
                        viewport.x, viewport.y, viewport.w, viewport.h,
                        viewport.h > 0 ? (double)viewport.w / viewport.h : 0.0,
                        ps->fullscreen ? " [FS]" : " [windowed]");
            }
        }

        ps->gpu_uniforms.frameCount = (float)ps->diag_frames_displayed;

        SDL_PushGPUFragmentUniformData(cmd, 0,
            &ps->gpu_uniforms, sizeof(ps->gpu_uniforms));

        SDL_GPUTextureSamplerBinding bindings[6] = {
            { .texture = ps->gpu_tex_y,     .sampler = ps->gpu_sampler },
            { .texture = (use_zc_uv && ps->gpu_tex_uv)
                         ? ps->gpu_tex_uv : ps->gpu_tex_u,
                                            .sampler = ps->gpu_sampler },
            { .texture = ps->gpu_tex_v,     .sampler = ps->gpu_sampler },
            { .texture = ps->gpu_tex_noise, .sampler = ps->gpu_sampler_nearest },
        };
        Uint32 nbind = 4;
        if (ps->pq_lut_active) {
            /* LUT-variant shaders declare t4/t5 — bind count must
             * match the compiled variant (linear+clamp sampler is
             * exactly what a 1D LUT wants). */
            bindings[4].texture = ps->gpu_tex_lut_lin;
            bindings[4].sampler = ps->gpu_sampler;
            bindings[5].texture = ps->gpu_tex_lut_pq;
            bindings[5].sampler = ps->gpu_sampler;
            nbind = 6;
        }
        SDL_BindGPUFragmentSamplers(pass, 0, bindings, nbind);

        SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);

        if (draw_overlay)
            gpu_overlay_draw(pass, cmd, ps, sc_w, sc_h);
    }
    SDL_EndGPURenderPass(pass);
}

/* ── Swapchain pass for intermediate mode: blit + overlay ──
 * The frame texture is swapchain-sized, so the blit is a 1:1 copy
 * covering every pixel — LOADOP_DONT_CARE skips a clear. */
static void blit_and_overlay(PlayerState *ps, SDL_GPUCommandBuffer *cmd,
                             SDL_GPUTexture *swapchain_tex,
                             Uint32 sc_w, Uint32 sc_h) {
    SDL_GPUColorTargetInfo color_target;
    SDL_zero(color_target);
    color_target.texture  = swapchain_tex;
    color_target.load_op  = SDL_GPU_LOADOP_DONT_CARE;
    color_target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, NULL);
    {
        SDL_BindGPUGraphicsPipeline(pass, ps->gpu_pipeline_blit);
        SDL_GPUTextureSamplerBinding binding = {
            .texture = ps->gpu_tex_frame,
            .sampler = ps->gpu_sampler_nearest,   /* 1:1 — exact */
        };
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);

        gpu_overlay_draw(pass, cmd, ps, sc_w, sc_h);
    }
    SDL_EndGPURenderPass(pass);
}

/* Re-draw the last frame without uploading new data.
 * Called from main.c on ticks where no new frame was decoded
 * (GPU double-buffering requires explicit re-blit each frame).
 * Also used for paused state rendering. */
void video_reblit(PlayerState *ps) {
    if (!ps->gpu_tex_y) return;

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(ps->gpu_device);
    if (!cmd) return;

    /* ── Overlay copy pass (if dirty — e.g. first reblit after overlay update) ── */
    gpu_overlay_copy_cmd(cmd, ps);

    SDL_GPUTexture *swapchain_tex = NULL;
    Uint32 sc_w, sc_h;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, ps->window,
            &swapchain_tex, &sc_w, &sc_h)) {
        SDL_CancelGPUCommandBuffer(cmd);
        /* Was silent — a freeze living entirely on reblit ticks left
         * zero log evidence (2026-08-14 FS investigation). Throttled:
         * first failure and every 60th thereafter. */
        static int s_reblit_acq_fail = 0;
        if (s_reblit_acq_fail++ % 60 == 0)
            log_msg("WARN: reblit swapchain acquire failed (x%d): %s",
                    s_reblit_acq_fail, SDL_GetError());
        return;
    }
    if (!swapchain_tex) {
        SDL_CancelGPUCommandBuffer(cmd);
        static int s_reblit_null_tex = 0;
        if (s_reblit_null_tex++ % 60 == 0)
            log_msg("WARN: reblit acquire returned no texture (x%d)",
                    s_reblit_null_tex);
        return;
    }

    /* Cache physical pixel dimensions for DPI-correct overlay sizing */
    ps->sc_w = (int)sc_w;
    ps->sc_h = (int)sc_h;

    /* ── Reblit: video shader only when state moved; blit every tick ── */
    if (!ps->no_intermediate && ps->intermediate_apt
            && gpu_frame_tex_ensure(ps, (int)sc_w, (int)sc_h) == 0) {
        if (!ps->frame_tex_valid || ps->frame_render_dirty) {
            render_video_pass(ps, cmd, ps->gpu_tex_frame, sc_w, sc_h,
                              ps->vaapi_zerocopy && ps->gpu_tex_uv != NULL, 0);
            ps->frame_tex_valid = 1;
            ps->frame_render_dirty = 0;
        }
        blit_and_overlay(ps, cmd, swapchain_tex, sc_w, sc_h);
    } else {
        render_video_pass(ps, cmd, swapchain_tex, sc_w, sc_h,
                          ps->vaapi_zerocopy && ps->gpu_tex_uv != NULL, 1);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    ps->presents++;
}


/* ═══════════════════════════════════════════════════════════════════
 * Seeking
 * ═══════════════════════════════════════════════════════════════════ */

/* Seek by `incr` seconds relative to current position. */
void player_seek(PlayerState *ps, double incr) {
    if (!ps->playing) return;

    double pos = ps->video_clock + incr;
    if (pos < 0.0) pos = 0.0;

    ps->seek_target  = (int64_t)(pos * AV_TIME_BASE);
    ps->seek_flags   = (incr < 0) ? AVSEEK_FLAG_BACKWARD : 0;
    ps->seek_request = 1;

    /* Reset video timing after seek */
    ps->frame_timer      = get_time_sec();
    ps->frame_last_delay = 0.04;
}


/* ═══════════════════════════════════════════════════════════════════
 * Media Info / Debug
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Bounded append for the info/debug string builders ──
 *
 * HEAP-OVERFLOW FIX: the old `INFO_APPEND(...)`
 * pattern is unsafe on truncation — snprintf returns the WOULD-BE length,
 * so off can exceed sz, after which (sz - off) is negative and wraps to a
 * huge size_t on the next call, writing past the buffer. Reachable via
 * tag-heavy files (the metadata loop is unbounded) or long filenames.
 * This macro clamps off and skips appends once the buffer is full.
 *
 * Expects locals: char *buf; int sz; int off; */
#define INFO_APPEND(...)                                                    \
    do {                                                                    \
        if (off < sz - 1) {                                                 \
            int _n = snprintf(buf + off, (size_t)(sz - off), __VA_ARGS__);  \
            if (_n > 0) off += _n;                                          \
            if (off > sz - 1) off = sz - 1;                                 \
        }                                                                   \
    } while (0)

void player_build_media_info(PlayerState *ps) {
    if (!ps->fmt_ctx) return;

    char *buf = ps->media_info;
    int   sz  = sizeof(ps->media_info);
    int   off = 0;

    INFO_APPEND("=== MEDIA INFO ===\n");
    INFO_APPEND("File: %s\n", ps->filepath);
    INFO_APPEND("Format: %s (%s)\n",
        ps->fmt_ctx->iformat->name, ps->fmt_ctx->iformat->long_name);

    double duration = (ps->fmt_ctx->duration != AV_NOPTS_VALUE)
        ? (double)ps->fmt_ctx->duration / AV_TIME_BASE : 0.0;
    int hrs = (int)duration / 3600;
    int min = ((int)duration % 3600) / 60;
    int sec = (int)duration % 60;
    INFO_APPEND("Duration: %02d:%02d:%02d\n", hrs, min, sec);

    if (ps->fmt_ctx->bit_rate > 0) {
        INFO_APPEND("Bitrate: %"PRId64" kb/s\n",
            ps->fmt_ctx->bit_rate / 1000);
    }

    /* Video stream info */
    if (ps->video_stream_idx >= 0) {
        AVStream *vs = ps->fmt_ctx->streams[ps->video_stream_idx];
        AVCodecParameters *par = vs->codecpar;
        INFO_APPEND("\n--- Video ---\n");
        INFO_APPEND("Codec: %s\n",
            avcodec_get_name(par->codec_id));
        INFO_APPEND("Resolution: %dx%d\n",
            par->width, par->height);
        INFO_APPEND("Pixel Format: %s\n",
            av_get_pix_fmt_name(par->format));
        INFO_APPEND("Decode: %s\n",
            ps->vaapi_active ? "VAAPI hardware" : "software");

        if (vs->avg_frame_rate.den > 0) {
            INFO_APPEND("Frame Rate: %.3f fps\n",
                av_q2d(vs->avg_frame_rate));
        }
        if (vs->r_frame_rate.den > 0) {
            INFO_APPEND("Real Frame Rate: %.3f fps\n",
                av_q2d(vs->r_frame_rate));
        }
        if (par->bit_rate > 0) {
            INFO_APPEND("Video Bitrate: %"PRId64" kb/s\n",
                par->bit_rate / 1000);
        }

        /* Color info — show tagged values, or infer with "(assumed)" */
        {
            int is_hd = (par->height >= 720);

            if (par->color_space != AVCOL_SPC_UNSPECIFIED) {
                INFO_APPEND("Color Space: %s\n",
                    av_color_space_name(par->color_space));
            } else {
                INFO_APPEND("Color Space: %s (assumed)\n",
                    is_hd ? "bt709" : "bt601");
            }

            if (par->color_range != AVCOL_RANGE_UNSPECIFIED) {
                INFO_APPEND("Color Range: %s\n",
                    av_color_range_name(par->color_range));
            } else {
                INFO_APPEND("Color Range: tv (assumed)\n");
            }

            if (par->color_primaries != AVCOL_PRI_UNSPECIFIED) {
                INFO_APPEND("Color Primaries: %s\n",
                    av_color_primaries_name(par->color_primaries));
            } else {
                INFO_APPEND("Color Primaries: %s (assumed)\n",
                    is_hd ? "bt709" : "bt601");
            }

            if (par->color_trc != AVCOL_TRC_UNSPECIFIED) {
                INFO_APPEND("Color TRC: %s\n",
                    av_color_transfer_name(par->color_trc));
            } else {
                INFO_APPEND("Color TRC: %s (assumed)\n",
                    is_hd ? "bt709" : "bt601");
            }

            /* HDR info from uniforms (already detected at open time) */
            if (ps->gpu_uniforms.is_hdr > 0.0f) {
                INFO_APPEND(
                    "HDR: Yes (peak %.0f nits, %s gamut)\n",
                    ps->gpu_uniforms.hdr_peak_nits,
                    ps->gpu_uniforms.hdr_gamut > 0.5f ? "BT.2020" : "BT.709");
            }
        }
    }

    /* Audio stream info */
    if (ps->audio_stream_idx >= 0) {
        AVStream *as = ps->fmt_ctx->streams[ps->audio_stream_idx];
        AVCodecParameters *par = as->codecpar;
        INFO_APPEND("\n--- Audio ---\n");
        INFO_APPEND("Codec: %s\n",
            avcodec_get_name(par->codec_id));
        INFO_APPEND("Sample Rate: %d Hz\n",
            par->sample_rate);
        INFO_APPEND("Channels: %d\n",
            par->ch_layout.nb_channels);

        char ch_layout_str[128];
        av_channel_layout_describe(&par->ch_layout, ch_layout_str, sizeof(ch_layout_str));
        INFO_APPEND("Channel Layout: %s\n", ch_layout_str);

        INFO_APPEND("Sample Format: %s\n",
            av_get_sample_fmt_name(par->format));
        if (par->bit_rate > 0) {
            INFO_APPEND("Audio Bitrate: %"PRId64" kb/s\n",
                par->bit_rate / 1000);
        }
    }

    /* Metadata */
    AVDictionaryEntry *tag = NULL;
    int first = 1;
    while ((tag = av_dict_get(ps->fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        if (first) {
            INFO_APPEND("\n--- Metadata ---\n");
            first = 0;
        }
        INFO_APPEND("%s: %s\n", tag->key, tag->value);
    }
}

void player_build_debug_info(PlayerState *ps) {
    if (!ps->playing) return;

    char *buf = ps->debug_info;
    int   sz  = sizeof(ps->debug_info);
    int   off = 0;

    INFO_APPEND("=== DEBUG ===\n");
    INFO_APPEND("Build:       %s\n", DSVP_GIT_COMMIT);
    INFO_APPEND("Output:      %dx%d (%s)\n",
        ps->sc_w, ps->sc_h,
        ps->fullscreen ? "borderless" : "windowed");

    /* Real-time FPS + scaler resolution (video streams only) */
    if (ps->video_stream_idx >= 0) {
        if (ps->paused)
            INFO_APPEND("FPS:         paused\n");
        else
            INFO_APPEND("FPS:         %.2f (render %.0f)\n",
                ps->fps_content, ps->fps_render);

        /* Resolution = source -> the physical-pixel area the scaler
         * actually fills (display_rect is in logical window coords;
         * convert to swapchain pixels, matching the viewport math in
         * video_display). In Game Mode crop-to-fill, out_w can exceed
         * sc_w — that's the crop overflow, shown honestly. */
        int out_w = ps->display_rect.w;
        int out_h = ps->display_rect.h;
        if (ps->win_w > 0 && ps->sc_w > 0)
            out_w = (int)((double)ps->display_rect.w * ps->sc_w / ps->win_w + 0.5);
        if (ps->win_h > 0 && ps->sc_h > 0)
            out_h = (int)((double)ps->display_rect.h * ps->sc_h / ps->win_h + 0.5);
        INFO_APPEND("Resolution:  %dx%d -> %dx%d\n",
            ps->vid_w, ps->vid_h, out_w, out_h);
    }

    INFO_APPEND("Renderer: SDL_GPU\n");
    INFO_APPEND("Video Queue: %d pkts (%d KB)\n",
        ps->video_pq.nb_packets, ps->video_pq.size / 1024);
    INFO_APPEND("Audio Queue: %d pkts (%d KB)\n",
        ps->audio_pq.nb_packets, ps->audio_pq.size / 1024);
    INFO_APPEND("Volume:      %.0f%%\n", ps->volume * 100.0);

    if (ps->video_codec_ctx) {
        if (ps->vaapi_active) {
            INFO_APPEND("Decode: VAAPI (hardware)\n");
        } else {
            INFO_APPEND("Decoder Threads: %d\n",
                ps->video_codec_ctx->thread_count);
        }

        int is_yuv420p = (ps->video_codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P);
        int is_10bit = (ps->video_codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P10LE);
        int is_full_range = (ps->fmt_ctx &&
            ps->fmt_ctx->streams[ps->video_stream_idx]->codecpar->color_range == AVCOL_RANGE_JPEG);

        if (ps->vaapi_active) {
            INFO_APPEND(
                "SWS: bypassed (VAAPI %s → deinterleave → %s, %s)\n",
                ps->vaapi_nv12 ? "NV12" : "P010",
                ps->vaapi_nv12 ? "R8_UNORM" : "R16_UNORM",
                is_full_range ? "full" : "limited");
        } else if (is_10bit && !ps->sws_ctx) {
            INFO_APPEND(
                "SWS: bypassed (10-bit passthrough, %s->full in shader)\n",
                is_full_range ? "full" : "limited");
        } else if (is_yuv420p && !ps->sws_ctx) {
            INFO_APPEND(
                "SWS: bypassed (8-bit passthrough, %s->full in shader)\n",
                is_full_range ? "full" : "limited");
        } else {
            INFO_APPEND(
                "SWS: format convert%s (SWS_LANCZOS + ED dither)\n",
                ps->sws_out_10bit ? " to 10-bit" : "");
        }
        /* Report the kernel actually BOUND — the old static "Lanczos-2
         * luma" line lied on three of the four variants. */
        {
            /* "exact-2x", not "scale2x": same Lanczos-2 kernel, weights
             * precomputed (identical phase at exactly 2.0x — output is
             * bit-comparable to the sin path, verified at landing).
             * The old label read like the pixel-art Scale2x algorithm
             * and got flagged as a fidelity downgrade in the field. */
            static const char *variant_names[] = {
                "fixed 4x4 Lanczos-2",
                "dilated Lanczos",
                "direct 1:1 (single fetch)",
                "exact-2x Lanczos-2 (const weights)",
            };
            int v = ps->active_variant;
            INFO_APPEND("Sampler: %s luma, Catmull-Rom chroma\n",
                (v >= 0 && v <= 3) ? variant_names[v] : "(no frame yet)");
            INFO_APPEND("Dither:  blue noise 64x64\n");
        }

        {
            static const char *chroma_names[] = {
                "unspecified", "left", "center", "top-left",
                "top", "bottom-left", "bottom"
            };
            int cl = ps->chroma_location;
            const char *cn = (cl >= 0 && cl <= 6) ? chroma_names[cl] : "unknown";
            INFO_APPEND("Chroma Siting: %s\n", cn);
        }
    }

    /* Audio track info */
    if (ps->aud_count > 1) {
        INFO_APPEND("Audio Track: %s (%d/%d)\n",
            ps->aud_stream_names[ps->aud_selection],
            ps->aud_selection + 1, ps->aud_count);
    }

    /* Subtitle info */
    if (ps->sub_count > 0) {
        if (ps->sub_selection == 0) {
            INFO_APPEND("Subtitles: off (%d available)\n",
                ps->sub_count);
        } else {
            INFO_APPEND("Subtitles: %s\n",
                ps->sub_stream_names[ps->sub_selection - 1]);
        }
    } else {
        INFO_APPEND("Subtitles: none found\n");
    }

    double duration = (ps->fmt_ctx && ps->fmt_ctx->duration != AV_NOPTS_VALUE)
        ? (double)ps->fmt_ctx->duration / AV_TIME_BASE : 0.0;
    double pos = ps->video_clock;
    INFO_APPEND("Position:    %.1f / %.1f s\n", pos, duration);

    /* Audio status */
    INFO_APPEND("\n--- Audio ---\n");
    if (ps->audio_codec_ctx) {
        const char *acodec = avcodec_get_name(ps->audio_codec_ctx->codec_id);
        const char *afmt   = av_get_sample_fmt_name(ps->audio_codec_ctx->sample_fmt);
        int src_rate = ps->audio_codec_ctx->sample_rate;
        int src_ch   = ps->audio_codec_ctx->ch_layout.nb_channels;

        char layout_desc[64] = {0};
        av_channel_layout_describe(&ps->audio_codec_ctx->ch_layout,
                                   layout_desc, sizeof(layout_desc));

        INFO_APPEND("Source:  %s %s %dHz %dch (%s)\n",
            acodec, afmt ? afmt : "?", src_rate, src_ch, layout_desc);

        /* Passthrough truth: with bitstream active the SDL PCM spec
         * below describes a device that is NOT running — say what the
         * audio path actually is instead. */
        if (ps->bitstream_active) {
            INFO_APPEND("Output:  IEC 61937 passthrough (PipeWire)\n");
            INFO_APPEND("Pipeline: compressed bits to the sink — no "
                        "decode, no resample\n");
            goto audio_done;
        }

        /* Determine output format name from SDL spec */
        const char *out_fmt = (ps->audio_spec.format == SDL_AUDIO_F32) ? "F32" :
                              (ps->audio_spec.format == SDL_AUDIO_S16) ? "S16" : "???";
        int out_rate = ps->audio_spec.freq;
        int out_ch   = ps->audio_spec.channels;
        INFO_APPEND("Output:  %s %dHz %dch (stereo)\n",
            out_fmt, out_rate, out_ch);

        int resampling = (src_rate != out_rate);
        int downmixing = (src_ch != out_ch);

        if (resampling && downmixing)
            INFO_APPEND(
                "Pipeline: resample %d->%dHz + downmix %dch->%dch + %s\n",
                src_rate, out_rate, src_ch, out_ch, out_fmt);
        else if (resampling)
            INFO_APPEND(
                "Pipeline: resample %d->%dHz + %s\n", src_rate, out_rate, out_fmt);
        else if (downmixing)
            INFO_APPEND(
                "Pipeline: downmix %dch->%dch + %s\n", src_ch, out_ch, out_fmt);
        else if (afmt && strcmp(afmt, "flt") == 0 &&
                 ps->audio_spec.format == SDL_AUDIO_F32)
            INFO_APPEND("Pipeline: direct (no conversion)\n");
        else
            INFO_APPEND(
                "Pipeline: format convert %s->%s\n", afmt ? afmt : "?", out_fmt);
audio_done:;
    } else {
        INFO_APPEND("No audio\n");
    }

    /* Bitstream status */
    INFO_APPEND("\n--- Bitstream ---\n");
    {
        static const char *mode_names[] = { "PCM", "AUTO", "PASSTHROUGH" };
        INFO_APPEND("Mode:    %s\n",
            mode_names[ps->audio_mode]);
        INFO_APPEND("Active:  %s\n",
            ps->bitstream_active ? "yes" : "no");
        if (ps->bitstream_caps.probed) {
            INFO_APPEND("Via:     PipeWire (sink chosen at start)\n");
            INFO_APPEND("Codecs:  %s%s%s%s%s\n",
                ps->bitstream_caps.support_ac3    ? "AC3 " : "",
                ps->bitstream_caps.support_eac3   ? "EAC3 " : "",
                ps->bitstream_caps.support_truehd  ? "TrueHD " : "",
                ps->bitstream_caps.support_dts     ? "DTS " : "",
                ps->bitstream_caps.support_dtshd   ? "DTS-HD " : "");
        } else {
            INFO_APPEND("Sink:    not probed\n");
        }
    }

    /* Pacing — the live contract state, same numbers as PACE-DIAG */
    INFO_APPEND("\n--- Pacing ---\n");
    INFO_APPEND("Mode:    %s\n",
        ps->pace_mode == PACE_LOCKED ? "LOCKED" : "SCHEDULED");
    if (ps->pace_median > 0.0)
        INFO_APPEND("Cadence: median %.2f ms vs content %.2f ms\n",
            ps->pace_median * 1000.0, ps->pace_content_ema * 1000.0);
    else
        INFO_APPEND("Cadence: sensor filling (content %.2f ms)\n",
            ps->pace_content_ema * 1000.0);

    /* Zero-copy (VAAPI DMA-BUF route only) */
    if (ps->vaapi_zerocopy) {
        INFO_APPEND("\n--- Zero-copy ---\n");
        INFO_APPEND("Cache:   %d hit / %d miss / %d rebuild\n",
            ps->zc_cache_hits, ps->zc_cache_misses, ps->zc_cache_rebuilds);
        INFO_APPEND("Sync:    %s\n",
            zc_sync_mode() == 1 ? "queue drain"
          : zc_sync_mode() == 2 ? "none (NOSYNC)" : "own-submit fence");
    }

    /* Playback diagnostics */
    INFO_APPEND("\n--- Diagnostics ---\n");
    INFO_APPEND("Decoded:     %d\n", ps->diag_frames_decoded);
    INFO_APPEND("Displayed:   %d\n", ps->diag_frames_displayed);
    /* Same formula as the close-out summary (dropped/decoded) — the
     * panel and the log must never disagree about the same number. */
    INFO_APPEND("Dropped:     %d (%.2f%%)\n", ps->diag_frames_dropped,
        ps->diag_frames_decoded > 0
            ? 100.0 * ps->diag_frames_dropped / ps->diag_frames_decoded
            : 0.0);
    if (ps->xfer_pool_served || ps->xfer_pool_misses)
        INFO_APPEND("Pool:        %d zero-copy, %d fallback\n",
            ps->xfer_pool_served, ps->xfer_pool_misses);
    INFO_APPEND("Multi-ticks: %d\n", ps->diag_multi_decodes);
    INFO_APPEND("Stall snaps: %d\n", ps->diag_timer_snaps);
    INFO_APPEND("Peak drift:  %.1f ms\n",
        ps->diag_max_av_drift * 1000.0);
    INFO_APPEND("A/V bias:    %.1f ms\n",
        ps->av_bias * 1000.0);
}
