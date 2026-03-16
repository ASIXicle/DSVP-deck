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
    "Texture2D<float> texU : register(t1, space2);\n"
    "Texture2D<float> texV : register(t2, space2);\n"
    "Texture2D<float> texNoise : register(t3, space2);\n"
    "SamplerState sampY : register(s0, space2);\n"
    "SamplerState sampU : register(s1, space2);\n"
    "SamplerState sampV : register(s2, space2);\n"
    "SamplerState sampNoise : register(s3, space2);\n"
    "\n"
    "cbuffer Params : register(b0, space3) {\n"
    "    row_major float4x4 colorMatrix;\n"
    "    float2 rangeY;\n"
    "    float2 rangeUV;\n"
    "    float2 texSizeY;\n"
    "    float2 texSizeUV;\n"
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
    "    float2 pos  = uv * tex_size - 0.5;\n"
    "    float2 base = floor(pos);\n"
    "    float2 f    = pos - base;\n"
    "\n"
    "    float result = 0.0;\n"
    "    float wsum   = 0.0;\n"
    "\n"
    "    [unroll] for (int j = -1; j <= 2; j++) {\n"
    "        float wy = lanczos2(float(j) - f.y);\n"
    "        [unroll] for (int i = -1; i <= 2; i++) {\n"
    "            float wx = lanczos2(float(i) - f.x);\n"
    "            float w  = wx * wy;\n"
    "            float2 tc = (base + float2(float(i), float(j)) + 0.5)\n"
    "                        / tex_size;\n"
    "            result += tex.SampleLevel(samp, tc, 0).r * w;\n"
    "            wsum   += w;\n"
    "        }\n"
    "    }\n"
    "\n"
    "    return (wsum > 0.0) ? result / wsum : 0.0;\n"
    "}\n"
    "\n"
    "/* Catmull-Rom (bicubic) 4x4 tap filter for chroma planes.\n"
    " * Smoother than bilinear without the ringing of Lanczos.\n"
    " * Standard for chroma upscaling in quality video players (mpv). */\n"
    "float sample_catmull(Texture2D<float> tex, SamplerState samp,\n"
    "                     float2 uv, float2 tex_size) {\n"
    "    float2 pos  = uv * tex_size - 0.5;\n"
    "    float2 base = floor(pos);\n"
    "    float2 f    = pos - base;\n"
    "\n"
    "    float result = 0.0;\n"
    "    float wsum   = 0.0;\n"
    "\n"
    "    [unroll] for (int j = -1; j <= 2; j++) {\n"
    "        float t = abs(float(j) - f.y);\n"
    "        float wy = (t <= 1.0)\n"
    "            ? (1.5 * t * t * t - 2.5 * t * t + 1.0)\n"
    "            : (-0.5 * t * t * t + 2.5 * t * t - 4.0 * t + 2.0);\n"
    "        [unroll] for (int i = -1; i <= 2; i++) {\n"
    "            float s = abs(float(i) - f.x);\n"
    "            float wx = (s <= 1.0)\n"
    "                ? (1.5 * s * s * s - 2.5 * s * s + 1.0)\n"
    "                : (-0.5 * s * s * s + 2.5 * s * s - 4.0 * s + 2.0);\n"
    "            float w = wx * wy;\n"
    "            float2 tc = (base + float2(float(i), float(j)) + 0.5)\n"
    "                        / tex_size;\n"
    "            result += tex.SampleLevel(samp, tc, 0).r * w;\n"
    "            wsum   += w;\n"
    "        }\n"
    "    }\n"
    "\n"
    "    return (wsum > 0.0) ? result / wsum : 0.0;\n"
    "}\n"
    "\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {\n"
    "    /* Lanczos-2 for luma, Catmull-Rom for chroma */\n"
    "    float y  = sample_lanczos(texY, sampY, uv, texSizeY);\n"
    "    float cb = sample_catmull(texU, sampU, uv, texSizeUV);\n"
    "    float cr = sample_catmull(texV, sampV, uv, texSizeUV);\n"
    "\n"
    "    y  = (y  - rangeY.x)  * rangeY.y;\n"
    "    cb = (cb - rangeUV.x) * rangeUV.y;\n"
    "    cr = (cr - rangeUV.x) * rangeUV.y;\n"
    "\n"
    "    float4 yuv = float4(y, cb - 0.5, cr - 0.5, 1.0);\n"
    "    float3 rgb = mul(colorMatrix, yuv).rgb;\n"
    "\n"
    "    /* Blue noise dither: ±0.5 LSB in 8-bit (±1/510 in [0,1]).\n"
    "     * 64x64 void-and-cluster texture, tiled via frac(). All spectral\n"
    "     * energy in high frequencies — perceptually invisible. */\n"
    "    float d = (texNoise.SampleLevel(sampNoise, frac(pos.xy / 64.0), 0).r - 0.5) / 255.0;\n"
    "    rgb += float3(d, d, d);\n"
    "\n"
    "    return float4(saturate(rgb), 1.0);\n"
    "}\n";

/* RGBA overlay fragment shader — simple passthrough with alpha.
 * Used for compositing debug overlays, seek bar, subtitles, etc.
 * over the video frame. One texture, one sampler, no uniforms. */
static const char hlsl_overlay_frag[] =
    "Texture2D<float4> texOverlay : register(t0, space2);\n"
    "SamplerState sampOverlay : register(s0, space2);\n"
    "\n"
    "float4 main(float2 uv : TEXCOORD0) : SV_Target0 {\n"
    "    return texOverlay.Sample(sampOverlay, uv);\n"
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

static SDL_GPUShader *compile_shader(
    SDL_GPUDevice *device,
    const char *source,
    const char *entrypoint,
    SDL_ShaderCross_ShaderStage stage)
{
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

int gpu_create_pipelines(PlayerState *ps) {
    if (!ps->gpu_device || !ps->window) return -1;

    log_msg("GPU: compiling shaders...");

    /* ── Compile vertex shader (shared by both pipelines) ── */
    SDL_GPUShader *vert = compile_shader(
        ps->gpu_device, hlsl_fullscreen_vert, "main",
        SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
    if (!vert) return -1;

    /* ── Compile planar YUV fragment shader ── */
    SDL_GPUShader *frag_yuv = compile_shader(
        ps->gpu_device, hlsl_yuv_planar_frag, "main",
        SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
    if (!frag_yuv) {
        SDL_ReleaseGPUShader(ps->gpu_device, vert);
        return -1;
    }

    /* ── Create YUV planar pipeline ── */
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

    /* Shaders are baked into the pipeline — release the objects */
    SDL_ReleaseGPUShader(ps->gpu_device, frag_yuv);

    if (!ps->gpu_pipeline_yuv) {
        log_msg("ERROR: Failed to create YUV pipeline: %s", SDL_GetError());
        SDL_ReleaseGPUShader(ps->gpu_device, vert);
        return -1;
    }
    log_msg("GPU: YUV planar pipeline created");

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
    if (ps->gpu_pipeline_yuv) {
        SDL_ReleaseGPUGraphicsPipeline(ps->gpu_device, ps->gpu_pipeline_yuv);
        ps->gpu_pipeline_yuv = NULL;
    }
    if (ps->gpu_pipeline_overlay) {
        SDL_ReleaseGPUGraphicsPipeline(ps->gpu_device, ps->gpu_pipeline_overlay);
        ps->gpu_pipeline_overlay = NULL;
    }
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
static int gpu_create_video_textures(PlayerState *ps) {
    int w = ps->vid_w;
    int h = ps->vid_h;
    int cw = w / 2;  /* chroma width  (4:2:0) */
    int ch = h / 2;  /* chroma height (4:2:0) */

    int is_10bit = (ps->video_codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P10LE);

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

    /* Transfer buffers (CPU→GPU staging) */
    SDL_GPUTransferBufferCreateInfo xfer_info;
    SDL_zero(xfer_info);
    xfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;

    xfer_info.size = (Uint32)w * h * bpp;
    ps->gpu_xfer_y = SDL_CreateGPUTransferBuffer(ps->gpu_device, &xfer_info);

    xfer_info.size = (Uint32)cw * ch * bpp;
    ps->gpu_xfer_u = SDL_CreateGPUTransferBuffer(ps->gpu_device, &xfer_info);
    ps->gpu_xfer_v = SDL_CreateGPUTransferBuffer(ps->gpu_device, &xfer_info);

    if (!ps->gpu_xfer_y || !ps->gpu_xfer_u || !ps->gpu_xfer_v) {
        log_msg("ERROR: Failed to create transfer buffers: %s", SDL_GetError());
        return -1;
    }

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
    if (ps->gpu_xfer_y) { SDL_ReleaseGPUTransferBuffer(ps->gpu_device, ps->gpu_xfer_y); ps->gpu_xfer_y = NULL; }
    if (ps->gpu_xfer_u) { SDL_ReleaseGPUTransferBuffer(ps->gpu_device, ps->gpu_xfer_u); ps->gpu_xfer_u = NULL; }
    if (ps->gpu_xfer_v) { SDL_ReleaseGPUTransferBuffer(ps->gpu_device, ps->gpu_xfer_v); ps->gpu_xfer_v = NULL; }
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
    /* Determine colorspace from metadata or resolution heuristic */
    int is_bt709 = (ps->vid_h >= 720);
    if (ps->fmt_ctx) {
        AVCodecParameters *par =
            ps->fmt_ctx->streams[ps->video_stream_idx]->codecpar;
        if (par->color_space == AVCOL_SPC_BT709)
            is_bt709 = 1;
        else if (par->color_space == AVCOL_SPC_BT470BG ||
                 par->color_space == AVCOL_SPC_SMPTE170M)
            is_bt709 = 0;
    }

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

    if (is_10bit_passthrough) {
        /* 10-bit passthrough — range correction in shader */
        if (is_full_range) {
            ps->gpu_uniforms.rangeY[0]  = 0.0f;
            ps->gpu_uniforms.rangeY[1]  = 65535.0f / 1023.0f;
            ps->gpu_uniforms.rangeUV[0] = 0.0f;
            ps->gpu_uniforms.rangeUV[1] = 65535.0f / 1023.0f;
        } else {
            ps->gpu_uniforms.rangeY[0]  = 64.0f / 65535.0f;
            ps->gpu_uniforms.rangeY[1]  = 65535.0f / (940.0f - 64.0f);
            ps->gpu_uniforms.rangeUV[0] = 64.0f / 65535.0f;
            ps->gpu_uniforms.rangeUV[1] = 65535.0f / (960.0f - 64.0f);
        }

        log_msg("GPU: uniforms set (%s, 10-bit %s range → shader)",
                is_bt709 ? "BT.709" : "BT.601",
                is_full_range ? "full" : "limited");

    } else if (is_8bit_passthrough) {
        /* 8-bit YUV420P passthrough — range correction in shader.
         * R8_UNORM reads uint8 V as V/255. */
        if (is_full_range) {
            ps->gpu_uniforms.rangeY[0]  = 0.0f;
            ps->gpu_uniforms.rangeY[1]  = 1.0f;
            ps->gpu_uniforms.rangeUV[0] = 0.0f;
            ps->gpu_uniforms.rangeUV[1] = 1.0f;
        } else {
            ps->gpu_uniforms.rangeY[0]  = 16.0f / 255.0f;
            ps->gpu_uniforms.rangeY[1]  = 255.0f / (235.0f - 16.0f);
            ps->gpu_uniforms.rangeUV[0] = 16.0f / 255.0f;
            ps->gpu_uniforms.rangeUV[1] = 255.0f / (240.0f - 16.0f);
        }

        log_msg("GPU: uniforms set (%s, 8-bit %s range → shader)",
                is_bt709 ? "BT.709" : "BT.601",
                is_full_range ? "full" : "limited");

    } else {
        /* swscale fallback — outputs full-range YUV420P, identity range */
        ps->gpu_uniforms.rangeY[0]  = 0.0f;
        ps->gpu_uniforms.rangeY[1]  = 1.0f;
        ps->gpu_uniforms.rangeUV[0] = 0.0f;
        ps->gpu_uniforms.rangeUV[1] = 1.0f;

        log_msg("GPU: uniforms set (%s, full range via swscale)",
                is_bt709 ? "BT.709" : "BT.601");
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

    if (is_bt709) {
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
    ps->gpu_uniforms.texSizeUV[0] = (float)(ps->vid_w / 2);
    ps->gpu_uniforms.texSizeUV[1] = (float)(ps->vid_h / 2);
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
 * Recreates if dimensions changed. Returns 0 on success, -1 on error. */
int gpu_overlay_ensure(PlayerState *ps, int width, int height) {
    if (!ps->gpu_device || width <= 0 || height <= 0) return -1;

    /* Already the right size? */
    if (ps->gpu_overlay_tex &&
        ps->overlay_tex_w == width && ps->overlay_tex_h == height) {
        return 0;
    }

    /* Destroy old resources */
    gpu_overlay_destroy(ps);

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

    log_msg("GPU: overlay texture created (%dx%d RGBA)", width, height);
    return 0;
}

/* Upload RGBA pixel data to the overlay GPU texture.
 * `rgba` must be width×height×4 bytes, tightly packed. */
void gpu_overlay_upload(PlayerState *ps, const uint8_t *rgba,
                        int width, int height) {
    if (!ps->gpu_overlay_xfer || !ps->gpu_overlay_tex) return;
    if (width != ps->overlay_tex_w || height != ps->overlay_tex_h) return;

    /* Map transfer buffer and copy pixel data */
    uint8_t *dst = SDL_MapGPUTransferBuffer(ps->gpu_device,
                                             ps->gpu_overlay_xfer, true);
    if (!dst) return;
    memcpy(dst, rgba, (size_t)width * height * 4);
    SDL_UnmapGPUTransferBuffer(ps->gpu_device, ps->gpu_overlay_xfer);

    ps->overlay_dirty = 1;
}

/* Issue the GPU copy pass to transfer overlay data to the texture.
 * Call this inside an existing command buffer, BEFORE the render pass.
 * Returns the copy pass so the caller can end it, or does it inline. */
void gpu_overlay_copy_cmd(SDL_GPUCommandBuffer *cmd, PlayerState *ps) {
    if (!ps->overlay_dirty || !ps->gpu_overlay_tex) return;

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    {
        SDL_GPUTextureTransferInfo src_info;
        SDL_GPUTextureRegion dst_region;

        SDL_zero(src_info);
        SDL_zero(dst_region);
        src_info.transfer_buffer = ps->gpu_overlay_xfer;
        src_info.pixels_per_row  = ps->overlay_tex_w;
        src_info.rows_per_layer  = ps->overlay_tex_h;
        dst_region.texture = ps->gpu_overlay_tex;
        dst_region.w = ps->overlay_tex_w;
        dst_region.h = ps->overlay_tex_h;
        dst_region.d = 1;
        SDL_UploadToGPUTexture(copy, &src_info, &dst_region, true);
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
    (void)cmd;  /* uniform push would use cmd, but overlay has none */

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
    if (q->mutex) SDL_DestroyMutex(q->mutex);
    if (q->cond)  SDL_DestroyCondition(q->cond);
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
int pq_get(PacketQueue *q, AVPacket *pkt, int block) {
    int ret = -1;

    SDL_LockMutex(q->mutex);
    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        PacketNode *node = q->first;
        if (node) {
            q->first = node->next;
            if (!q->first) q->last = NULL;
            q->nb_packets--;
            q->size -= node->pkt->size;

            av_packet_move_ref(pkt, node->pkt);
            av_packet_free(&node->pkt);
            av_free(node);
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
 * Open / Close
 * ═══════════════════════════════════════════════════════════════════ */

/* Open a media file: probe format, find best streams, init decoders,
 * set up scaling context, create GPU textures, start demux thread. */
int player_open(PlayerState *ps, const char *filename) {
    int ret;

    strncpy(ps->filepath, filename, sizeof(ps->filepath) - 1);
    log_msg("player_open: %s", filename);

    /* ── Open container ── */
    ps->fmt_ctx = NULL;
    ret = avformat_open_input(&ps->fmt_ctx, filename, NULL, NULL);
    if (ret < 0) {
        log_msg("ERROR: avformat_open_input failed: %s", av_err2str(ret));
        return -1;
    }

    ret = avformat_find_stream_info(ps->fmt_ctx, NULL);
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

    if (ps->video_stream_idx < 0) {
        log_msg("ERROR: No video stream found");
        avformat_close_input(&ps->fmt_ctx);
        return -1;
    }
    log_msg("Video stream: idx=%d, Audio stream: idx=%d",
        ps->video_stream_idx, ps->audio_stream_idx);

    /* ── Open video decoder (SOFTWARE ONLY) ── */
    {
        AVStream *vs = ps->fmt_ctx->streams[ps->video_stream_idx];
        const AVCodec *codec = avcodec_find_decoder(vs->codecpar->codec_id);
        if (!codec) {
            log_msg("ERROR: Unsupported video codec id=%d", vs->codecpar->codec_id);
            avformat_close_input(&ps->fmt_ctx);
            return -1;
        }
        log_msg("Video codec: %s (%s)", codec->name, codec->long_name);

        ps->video_codec_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(ps->video_codec_ctx, vs->codecpar);

        /* Force software decode — use all available CPU threads */
        ps->video_codec_ctx->thread_count = 0; /* auto-detect */
        ps->video_codec_ctx->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;

        ret = avcodec_open2(ps->video_codec_ctx, codec, NULL);
        if (ret < 0) {
            fprintf(stderr, "[DSVP] Cannot open video codec: %s\n", av_err2str(ret));
            avformat_close_input(&ps->fmt_ctx);
            return -1;
        }

        ps->vid_w = ps->video_codec_ctx->width;
        ps->vid_h = ps->video_codec_ctx->height;
        log_msg("Video: %dx%d, pix_fmt=%s, threads=%d",
            ps->vid_w, ps->vid_h,
            av_get_pix_fmt_name(ps->video_codec_ctx->pix_fmt),
            ps->video_codec_ctx->thread_count);
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

    /* ── Allocate decode frames ── */
    ps->video_frame = av_frame_alloc();
    ps->rgb_frame   = av_frame_alloc();
    ps->audio_frame = av_frame_alloc();

    /* ── Set up swscale (or skip for GPU passthrough) ──
     *
     * yuv420p10le: bypass swscale. Raw 10-bit planes → R16_UNORM textures.
     * yuv420p:     bypass swscale. Raw 8-bit planes → R8_UNORM textures.
     *              Range expansion (limited→full) done in fragment shader.
     *
     * All other pixel formats need swscale conversion to YUV420P first.
     * Shader handles the color matrix and any remaining range work.
     */
    {
        enum AVPixelFormat src_fmt = ps->video_codec_ctx->pix_fmt;
        int is_10bit  = (src_fmt == AV_PIX_FMT_YUV420P10LE);
        int is_yuv420p = (src_fmt == AV_PIX_FMT_YUV420P);

        if (is_10bit) {
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
            /* ── swscale path for all other formats ── */
            enum AVPixelFormat dst_fmt = AV_PIX_FMT_YUV420P;
            int dst_w = ps->vid_w;
            int dst_h = ps->vid_h;

            int sws_flags = SWS_LANCZOS | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
            const char *sws_mode = "format convert (SWS_LANCZOS + ED dither)";

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

            /* Error-diffusion dithering for format conversions */
            av_opt_set_int(ps->sws_ctx, "dithering", 1, 0);

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
                int dst_range = 1;

                int *inv_table, *table;
                int cur_src_range, cur_dst_range, brightness, contrast, saturation;
                sws_getColorspaceDetails(ps->sws_ctx,
                    &inv_table, &cur_src_range, &table, &cur_dst_range,
                    &brightness, &contrast, &saturation);

                sws_setColorspaceDetails(ps->sws_ctx,
                    sws_getCoefficients(src_cs), src_range,
                    sws_getCoefficients(dst_cs), dst_range,
                    brightness, contrast, saturation);

                log_msg("swscale: colorspace=%s range=%s->full",
                    (src_cs == SWS_CS_ITU709) ? "BT.709" : "BT.601",
                    src_range ? "full" : "limited");
            }

            /* ── Chroma siting ── */
            {
                AVCodecParameters *par = ps->fmt_ctx->streams[ps->video_stream_idx]->codecpar;
                const char *chroma_desc = "default";

                if (par->chroma_location == AVCHROMA_LOC_LEFT) {
                    av_opt_set_int(ps->sws_ctx, "src_h_chr_pos", 0, 0);
                    av_opt_set_int(ps->sws_ctx, "src_v_chr_pos", 128, 0);
                    chroma_desc = "left (MPEG-2)";
                } else if (par->chroma_location == AVCHROMA_LOC_CENTER) {
                    av_opt_set_int(ps->sws_ctx, "src_h_chr_pos", 128, 0);
                    av_opt_set_int(ps->sws_ctx, "src_v_chr_pos", 128, 0);
                    chroma_desc = "center (MPEG-1/JPEG)";
                } else if (par->chroma_location == AVCHROMA_LOC_TOPLEFT) {
                    av_opt_set_int(ps->sws_ctx, "src_h_chr_pos", 0, 0);
                    av_opt_set_int(ps->sws_ctx, "src_v_chr_pos", 0, 0);
                    chroma_desc = "top-left";
                }

                log_msg("swscale: chroma siting=%s", chroma_desc);
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

    /* ── Set up GPU color uniforms ── */
    gpu_setup_uniforms(ps);

    /* ── Init packet queues ── */
    pq_init(&ps->video_pq);
    pq_init(&ps->audio_pq);
    for (int i = 0; i < ps->sub_count; i++)
        pq_init(&ps->sub_pqs[i]);

    /* ── Seek mutex (protects codec flush vs decode) ── */
    ps->seek_mutex = SDL_CreateMutex();
    ps->seeking    = 0;

    /* ── Init timing ── */
    ps->frame_timer      = get_time_sec();
    ps->frame_last_delay = 0.04;   /* assume ~25fps initially */
    ps->frame_last_pts   = 0.0;
    ps->audio_clock      = 0.0;
    ps->audio_clock_sync = 0.0;
    ps->av_bias = 0.0;
    ps->av_bias_samples = 0;
    ps->video_clock      = 0.0;

    /* Suppress frame drops until the first frame is displayed.
     * Adapts automatically to any codec's keyframe recovery time. */
    ps->seek_recovering = 1;
    ps->video_ready = 0;

    /* ── Reset diagnostics ── */
    ps->diag_frames_displayed = 0;
    ps->diag_frames_decoded   = 0;
    ps->diag_frames_dropped   = 0;
    ps->diag_multi_decodes    = 0;
    ps->diag_timer_snaps      = 0;
    ps->diag_max_av_drift     = 0.0;
    ps->diag_last_report      = get_time_sec();

    /* ── Open audio output ── */
    if (ps->audio_codec_ctx) {
        audio_open(ps);
    }

    /* ── Start demux thread ── */
    ps->eof     = 0;
    ps->playing = 1;
    ps->paused  = 0;
    ps->demux_thread = SDL_CreateThread(demux_thread_func, "demux", ps);

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

    ps->quit = 1;

    /* Signal queues to unblock any waiting threads */
    ps->video_pq.abort_request = 1;
    ps->audio_pq.abort_request = 1;
    SDL_SignalCondition(ps->video_pq.cond);
    SDL_SignalCondition(ps->audio_pq.cond);

    /* Wait for demux thread */
    if (ps->demux_thread) {
        SDL_WaitThread(ps->demux_thread, NULL);
        ps->demux_thread = NULL;
    }

    /* Close audio */
    audio_close(ps);

    /* Close subtitles */
    sub_close_codec(ps);

    /* Flush queues */
    pq_destroy(&ps->video_pq);
    pq_destroy(&ps->audio_pq);
    for (int i = 0; i < ps->sub_count; i++)
        pq_destroy(&ps->sub_pqs[i]);

    /* Destroy seek mutex */
    if (ps->seek_mutex) { SDL_DestroyMutex(ps->seek_mutex); ps->seek_mutex = NULL; }

    /* Free frames */
    if (ps->video_frame)  av_frame_free(&ps->video_frame);
    if (ps->rgb_frame)    av_frame_free(&ps->rgb_frame);
    if (ps->audio_frame)  av_frame_free(&ps->audio_frame);

    /* Free buffers */
    if (ps->rgb_buffer)    { av_free(ps->rgb_buffer);    ps->rgb_buffer    = NULL; }
    if (ps->audio_buf)     { av_free(ps->audio_buf);     ps->audio_buf     = NULL; }

    /* Free scale/resample contexts */
    if (ps->sws_ctx)      { sws_freeContext(ps->sws_ctx); ps->sws_ctx = NULL; }
    if (ps->swr_ctx)      { swr_free(&ps->swr_ctx); ps->swr_ctx = NULL; }

    /* Free codecs */
    if (ps->video_codec_ctx) avcodec_free_context(&ps->video_codec_ctx);
    if (ps->audio_codec_ctx) avcodec_free_context(&ps->audio_codec_ctx);

    /* Close format */
    if (ps->fmt_ctx) avformat_close_input(&ps->fmt_ctx);

    /* ── Destroy GPU video textures and transfer buffers ── */
    gpu_destroy_video_textures(ps);

    /* Reset state */
    ps->playing            = 0;
    ps->paused             = 0;
    ps->eof                = 0;
    ps->quit               = 0;
    ps->video_stream_idx   = -1;
    ps->audio_stream_idx   = -1;
    ps->audio_buf_size     = 0;
    ps->audio_buf_index    = 0;
    ps->seek_request       = 0;
    ps->seeking            = 0;
    ps->seek_recovering    = 0;
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
    ps->sub_osd[0]         = '\0';

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

    while (!ps->quit) {
        /* ── Handle seek requests ── */
        if (ps->seek_request) {
            int64_t target = ps->seek_target;
            log_msg("Demux: seeking to %.3f s", (double)target / AV_TIME_BASE);

            /* CRITICAL: Lock the seek mutex. This prevents the main thread
             * from calling avcodec_send_packet/receive_frame on the video
             * codec while we flush it. The audio callback is also paused. */
            SDL_LockMutex(ps->seek_mutex);
            ps->seeking = 1;

            /* Pause audio device so callback can't touch audio codec */
            if (ps->audio_stream)
                SDL_PauseAudioStreamDevice(ps->audio_stream);

            int ret = av_seek_frame(ps->fmt_ctx, -1, target, ps->seek_flags);
            if (ret < 0) {
                log_msg("ERROR: Seek failed: %s", av_err2str(ret));
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
                log_msg("Demux: all codecs flushed");
            }
            ps->seek_request = 0;
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

            ps->seeking = 0;
            SDL_UnlockMutex(ps->seek_mutex);

            /* Suppress frame drops until the first frame is displayed
             * post-seek. Adapts to any codec — H.264 recovers in
             * ~100ms, HEVC with long GOPs may take 5–10 seconds.
             * Cleared in main.c when a frame is actually shown. */
            ps->seek_recovering = 1;
            ps->av_bias = 0.0;
            ps->av_bias_samples = 0;

            /* Resume audio playback */
            if (ps->audio_stream && !ps->paused)
                SDL_ResumeAudioStreamDevice(ps->audio_stream);

            log_msg("Demux: seek complete");
        }

        /* ── Throttle if queues are full ── */
        if (ps->video_pq.nb_packets > PACKET_QUEUE_MAX ||
            ps->audio_pq.nb_packets > PACKET_QUEUE_MAX) {
            SDL_Delay(10);
            continue;
        }

        /* ── Read next packet ── */
        int ret = av_read_frame(ps->fmt_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF || avio_feof(ps->fmt_ctx->pb)) {
                if (!ps->eof) log_msg("Demux: reached end of file");
                ps->eof = 1;
                SDL_Delay(100);
                continue;
            }
            log_msg("ERROR: av_read_frame failed: %s", av_err2str(ret));
            break; /* real error */
        }

        /* Route packet to the correct queue */
        if (pkt->stream_index == ps->video_stream_idx) {
            pq_put(&ps->video_pq, pkt);
        } else if (pkt->stream_index == ps->audio_stream_idx) {
            pq_put(&ps->audio_pq, pkt);
        } else {
            /* Check subtitle streams */
            int routed = 0;
            for (int i = 0; i < ps->sub_count; i++) {
                if (pkt->stream_index == ps->sub_stream_indices[i]) {
                    pq_put(&ps->sub_pqs[i], pkt);
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


/* ═══════════════════════════════════════════════════════════════════
 * Video Decode & Display
 * ═══════════════════════════════════════════════════════════════════ */

/* Decode one video frame from the packet queue.
 * Returns 1 if a frame was decoded, 0 if no packets available, -1 on error. */
int video_decode_frame(PlayerState *ps) {
    AVPacket pkt;
    int ret;

    /* If a seek is in progress, skip decode entirely.
     * The demux thread holds seek_mutex and is flushing codecs. */
    if (ps->seeking) return 0;

    /* Lock to prevent demux thread from flushing codecs mid-decode */
    if (!SDL_TryLockMutex(ps->seek_mutex)) {
        return 0; /* mutex held by seek — skip this frame */
    }

    for (;;) {
        /* Try to receive a decoded frame first (may have buffered frames) */
        ret = avcodec_receive_frame(ps->video_codec_ctx, ps->video_frame);
        if (ret == 0) {
            /* Got a frame — compute its PTS in seconds.
             * best_effort_timestamp is preferred: FFmpeg computes it
             * from DTS/packet timing even when the codec doesn't set
             * frame->pts (required for VC-1, some MPEG-2, etc.). */
            AVStream *vs = ps->fmt_ctx->streams[ps->video_stream_idx];
            double pts = 0.0;
            int64_t frame_pts = ps->video_frame->best_effort_timestamp;
            if (frame_pts == AV_NOPTS_VALUE)
                frame_pts = ps->video_frame->pts;
            if (frame_pts != AV_NOPTS_VALUE) {
                pts = (double)frame_pts * av_q2d(vs->time_base);
            }
            ps->video_clock = pts;
            SDL_UnlockMutex(ps->seek_mutex);
            return 1;
        }
        if (ret != AVERROR(EAGAIN)) {
            log_msg("ERROR: avcodec_receive_frame (video) failed: %s", av_err2str(ret));
            SDL_UnlockMutex(ps->seek_mutex);
            return -1; /* decoder error */
        }

        /* Need to feed more packets to the decoder */
        ret = pq_get(&ps->video_pq, &pkt, 0);
        if (ret <= 0) {
            SDL_UnlockMutex(ps->seek_mutex);
            return 0;  /* no packets available right now */
        }

        avcodec_send_packet(ps->video_codec_ctx, &pkt);
        av_packet_unref(&pkt);
    }
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
    if (video_aspect > win_aspect) {
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
static void upload_plane(
    SDL_GPUDevice *device,
    SDL_GPUTransferBuffer *xfer,
    const uint8_t *src, int src_stride,
    int width, int height)
{
    uint8_t *dst = SDL_MapGPUTransferBuffer(device, xfer, true);
    if (!dst) return;

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
void video_display(PlayerState *ps) {
    if (!ps->gpu_tex_y || !ps->video_frame || !ps->video_frame->data[0]) return;
    if (ps->seeking) return;

    int w  = ps->vid_w;
    int h  = ps->vid_h;
    int cw = w / 2;
    int ch = h / 2;

    /* ── Determine source frame and byte width ── */
    AVFrame *src_frame;
    int bpp;  /* bytes per sample for upload_plane */

    int is_10bit_passthrough =
        (ps->video_codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P10LE
         && !ps->sws_ctx);

    if (is_10bit_passthrough) {
        /* 10-bit passthrough — raw frame directly to R16_UNORM textures */
        src_frame = ps->video_frame;
        bpp = 2;
    } else if (!ps->sws_ctx) {
        /* 8-bit YUV420P passthrough — direct upload, range in shader */
        src_frame = ps->video_frame;
        bpp = 1;
    } else {
        /* swscale path — format conversion to YUV420P */
        sws_scale(ps->sws_ctx,
            (const uint8_t *const *)ps->video_frame->data,
            ps->video_frame->linesize,
            0, ps->vid_h,
            ps->rgb_frame->data,
            ps->rgb_frame->linesize);
        src_frame = ps->rgb_frame;
        bpp = 1;
    }

    /* ── Upload plane data to GPU transfer buffers ── */
    upload_plane(ps->gpu_device, ps->gpu_xfer_y,
                 src_frame->data[0], src_frame->linesize[0], w * bpp, h);
    upload_plane(ps->gpu_device, ps->gpu_xfer_u,
                 src_frame->data[1], src_frame->linesize[1], cw * bpp, ch);
    upload_plane(ps->gpu_device, ps->gpu_xfer_v,
                 src_frame->data[2], src_frame->linesize[2], cw * bpp, ch);

    /* ── GPU command buffer ── */
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(ps->gpu_device);
    if (!cmd) {
        log_msg("ERROR: SDL_AcquireGPUCommandBuffer failed: %s", SDL_GetError());
        return;
    }

    /* ── Copy pass: transfer buffers → GPU textures ── */
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    {
        SDL_GPUTextureTransferInfo src_info;
        SDL_GPUTextureRegion dst_region;

        /* Y plane */
        SDL_zero(src_info);
        SDL_zero(dst_region);
        src_info.transfer_buffer = ps->gpu_xfer_y;
        src_info.pixels_per_row  = w;
        src_info.rows_per_layer  = h;
        dst_region.texture = ps->gpu_tex_y;
        dst_region.w = w;
        dst_region.h = h;
        dst_region.d = 1;
        SDL_UploadToGPUTexture(copy, &src_info, &dst_region, true);

        /* U plane */
        SDL_zero(src_info);
        SDL_zero(dst_region);
        src_info.transfer_buffer = ps->gpu_xfer_u;
        src_info.pixels_per_row  = cw;
        src_info.rows_per_layer  = ch;
        dst_region.texture = ps->gpu_tex_u;
        dst_region.w = cw;
        dst_region.h = ch;
        dst_region.d = 1;
        SDL_UploadToGPUTexture(copy, &src_info, &dst_region, true);

        /* V plane */
        SDL_zero(src_info);
        SDL_zero(dst_region);
        src_info.transfer_buffer = ps->gpu_xfer_v;
        src_info.pixels_per_row  = cw;
        src_info.rows_per_layer  = ch;
        dst_region.texture = ps->gpu_tex_v;
        dst_region.w = cw;
        dst_region.h = ch;
        dst_region.d = 1;
        SDL_UploadToGPUTexture(copy, &src_info, &dst_region, true);
    }
    SDL_EndGPUCopyPass(copy);

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

    /* ── Render pass: YUV planar shader, 3 textures ── */
    SDL_GPUColorTargetInfo color_target;
    SDL_zero(color_target);
    color_target.texture    = swapchain_tex;
    color_target.clear_color = (SDL_FColor){ 0.0f, 0.0f, 0.0f, 1.0f };
    color_target.load_op    = SDL_GPU_LOADOP_CLEAR;
    color_target.store_op   = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, NULL);
    {
        SDL_BindGPUGraphicsPipeline(pass, ps->gpu_pipeline_yuv);

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
        SDL_SetGPUViewport(pass, &viewport);

        SDL_PushGPUFragmentUniformData(cmd, 0,
            &ps->gpu_uniforms, sizeof(ps->gpu_uniforms));

        SDL_GPUTextureSamplerBinding bindings[4] = {
            { .texture = ps->gpu_tex_y,     .sampler = ps->gpu_sampler },
            { .texture = ps->gpu_tex_u,     .sampler = ps->gpu_sampler },
            { .texture = ps->gpu_tex_v,     .sampler = ps->gpu_sampler },
            { .texture = ps->gpu_tex_noise, .sampler = ps->gpu_sampler_nearest },
        };
        SDL_BindGPUFragmentSamplers(pass, 0, bindings, 4);

        SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);

        /* ── Overlay quad (alpha-blended over video) ── */
        gpu_overlay_draw(pass, cmd, ps, sc_w, sc_h);
    }
    SDL_EndGPURenderPass(pass);
    SDL_SubmitGPUCommandBuffer(cmd);

    ps->video_ready = 1;
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
        return;
    }
    if (!swapchain_tex) {
        SDL_CancelGPUCommandBuffer(cmd);
        return;
    }

    /* Cache physical pixel dimensions for DPI-correct overlay sizing */
    ps->sc_w = (int)sc_w;
    ps->sc_h = (int)sc_h;

    SDL_GPUColorTargetInfo color_target;
    SDL_zero(color_target);
    color_target.texture    = swapchain_tex;
    color_target.clear_color = (SDL_FColor){ 0.0f, 0.0f, 0.0f, 1.0f };
    color_target.load_op    = SDL_GPU_LOADOP_CLEAR;
    color_target.store_op   = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, NULL);
    {
        SDL_BindGPUGraphicsPipeline(pass, ps->gpu_pipeline_yuv);

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
        SDL_SetGPUViewport(pass, &viewport);

        SDL_PushGPUFragmentUniformData(cmd, 0,
            &ps->gpu_uniforms, sizeof(ps->gpu_uniforms));

        SDL_GPUTextureSamplerBinding bindings[4] = {
            { .texture = ps->gpu_tex_y,     .sampler = ps->gpu_sampler },
            { .texture = ps->gpu_tex_u,     .sampler = ps->gpu_sampler },
            { .texture = ps->gpu_tex_v,     .sampler = ps->gpu_sampler },
            { .texture = ps->gpu_tex_noise, .sampler = ps->gpu_sampler_nearest },
        };
        SDL_BindGPUFragmentSamplers(pass, 0, bindings, 4);

        SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);

        /* ── Overlay quad (alpha-blended over video) ── */
        gpu_overlay_draw(pass, cmd, ps, sc_w, sc_h);
    }
    SDL_EndGPURenderPass(pass);
    SDL_SubmitGPUCommandBuffer(cmd);
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

void player_build_media_info(PlayerState *ps) {
    if (!ps->fmt_ctx) return;

    char *buf = ps->media_info;
    int   sz  = sizeof(ps->media_info);
    int   off = 0;

    off += snprintf(buf + off, sz - off, "=== MEDIA INFO ===\n");
    off += snprintf(buf + off, sz - off, "File: %s\n", ps->filepath);
    off += snprintf(buf + off, sz - off, "Format: %s (%s)\n",
        ps->fmt_ctx->iformat->name, ps->fmt_ctx->iformat->long_name);

    double duration = (ps->fmt_ctx->duration != AV_NOPTS_VALUE)
        ? (double)ps->fmt_ctx->duration / AV_TIME_BASE : 0.0;
    int hrs = (int)duration / 3600;
    int min = ((int)duration % 3600) / 60;
    int sec = (int)duration % 60;
    off += snprintf(buf + off, sz - off, "Duration: %02d:%02d:%02d\n", hrs, min, sec);

    if (ps->fmt_ctx->bit_rate > 0) {
        off += snprintf(buf + off, sz - off, "Bitrate: %"PRId64" kb/s\n",
            ps->fmt_ctx->bit_rate / 1000);
    }

    /* Video stream info */
    if (ps->video_stream_idx >= 0) {
        AVStream *vs = ps->fmt_ctx->streams[ps->video_stream_idx];
        AVCodecParameters *par = vs->codecpar;
        off += snprintf(buf + off, sz - off, "\n--- Video ---\n");
        off += snprintf(buf + off, sz - off, "Codec: %s\n",
            avcodec_get_name(par->codec_id));
        off += snprintf(buf + off, sz - off, "Resolution: %dx%d\n",
            par->width, par->height);
        off += snprintf(buf + off, sz - off, "Pixel Format: %s\n",
            av_get_pix_fmt_name(par->format));

        if (vs->avg_frame_rate.den > 0) {
            off += snprintf(buf + off, sz - off, "Frame Rate: %.3f fps\n",
                av_q2d(vs->avg_frame_rate));
        }
        if (vs->r_frame_rate.den > 0) {
            off += snprintf(buf + off, sz - off, "Real Frame Rate: %.3f fps\n",
                av_q2d(vs->r_frame_rate));
        }
        if (par->bit_rate > 0) {
            off += snprintf(buf + off, sz - off, "Video Bitrate: %"PRId64" kb/s\n",
                par->bit_rate / 1000);
        }

        /* Color info — show tagged values, or infer with "(assumed)" */
        {
            int is_hd = (par->height >= 720);

            if (par->color_space != AVCOL_SPC_UNSPECIFIED) {
                off += snprintf(buf + off, sz - off, "Color Space: %s\n",
                    av_color_space_name(par->color_space));
            } else {
                off += snprintf(buf + off, sz - off, "Color Space: %s (assumed)\n",
                    is_hd ? "bt709" : "bt601");
            }

            if (par->color_range != AVCOL_RANGE_UNSPECIFIED) {
                off += snprintf(buf + off, sz - off, "Color Range: %s\n",
                    av_color_range_name(par->color_range));
            } else {
                off += snprintf(buf + off, sz - off, "Color Range: tv (assumed)\n");
            }

            if (par->color_primaries != AVCOL_PRI_UNSPECIFIED) {
                off += snprintf(buf + off, sz - off, "Color Primaries: %s\n",
                    av_color_primaries_name(par->color_primaries));
            } else {
                off += snprintf(buf + off, sz - off, "Color Primaries: %s (assumed)\n",
                    is_hd ? "bt709" : "bt601");
            }

            if (par->color_trc != AVCOL_TRC_UNSPECIFIED) {
                off += snprintf(buf + off, sz - off, "Color TRC: %s\n",
                    av_color_transfer_name(par->color_trc));
            } else {
                off += snprintf(buf + off, sz - off, "Color TRC: %s (assumed)\n",
                    is_hd ? "bt709" : "bt601");
            }
        }
    }

    /* Audio stream info */
    if (ps->audio_stream_idx >= 0) {
        AVStream *as = ps->fmt_ctx->streams[ps->audio_stream_idx];
        AVCodecParameters *par = as->codecpar;
        off += snprintf(buf + off, sz - off, "\n--- Audio ---\n");
        off += snprintf(buf + off, sz - off, "Codec: %s\n",
            avcodec_get_name(par->codec_id));
        off += snprintf(buf + off, sz - off, "Sample Rate: %d Hz\n",
            par->sample_rate);
        off += snprintf(buf + off, sz - off, "Channels: %d\n",
            par->ch_layout.nb_channels);

        char ch_layout_str[128];
        av_channel_layout_describe(&par->ch_layout, ch_layout_str, sizeof(ch_layout_str));
        off += snprintf(buf + off, sz - off, "Channel Layout: %s\n", ch_layout_str);

        off += snprintf(buf + off, sz - off, "Sample Format: %s\n",
            av_get_sample_fmt_name(par->format));
        if (par->bit_rate > 0) {
            off += snprintf(buf + off, sz - off, "Audio Bitrate: %"PRId64" kb/s\n",
                par->bit_rate / 1000);
        }
    }

    /* Metadata */
    AVDictionaryEntry *tag = NULL;
    int first = 1;
    while ((tag = av_dict_get(ps->fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        if (first) {
            off += snprintf(buf + off, sz - off, "\n--- Metadata ---\n");
            first = 0;
        }
        off += snprintf(buf + off, sz - off, "%s: %s\n", tag->key, tag->value);
    }
}

void player_build_debug_info(PlayerState *ps) {
    if (!ps->playing) return;

    char *buf = ps->debug_info;
    int   sz  = sizeof(ps->debug_info);
    int   off = 0;

    off += snprintf(buf + off, sz - off, "=== DEBUG ===\n");
    off += snprintf(buf + off, sz - off, "Renderer: SDL_GPU\n");
    off += snprintf(buf + off, sz - off, "Video Clock: %.3f s\n", ps->video_clock);
    off += snprintf(buf + off, sz - off, "Audio Clock: %.3f s\n", ps->audio_clock_sync);
    off += snprintf(buf + off, sz - off, "A/V Diff:    %.3f ms\n",
        (ps->video_clock - ps->audio_clock_sync) * 1000.0);
    off += snprintf(buf + off, sz - off, "A/V Bias:    %.1f ms\n",
        ps->av_bias * 1000.0);
    off += snprintf(buf + off, sz - off, "Video Queue: %d pkts (%d KB)\n",
        ps->video_pq.nb_packets, ps->video_pq.size / 1024);
    off += snprintf(buf + off, sz - off, "Audio Queue: %d pkts (%d KB)\n",
        ps->audio_pq.nb_packets, ps->audio_pq.size / 1024);
    off += snprintf(buf + off, sz - off, "Volume:      %.0f%%\n", ps->volume * 100.0);
    off += snprintf(buf + off, sz - off, "Paused:      %s\n", ps->paused ? "yes" : "no");
    off += snprintf(buf + off, sz - off, "EOF:         %s\n", ps->eof ? "yes" : "no");

    if (ps->video_codec_ctx) {
        off += snprintf(buf + off, sz - off, "Decoder Threads: %d\n",
            ps->video_codec_ctx->thread_count);

        int is_yuv420p = (ps->video_codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P);
        int is_10bit = (ps->video_codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P10LE);
        int is_full_range = (ps->fmt_ctx &&
            ps->fmt_ctx->streams[ps->video_stream_idx]->codecpar->color_range == AVCOL_RANGE_JPEG);

        if (is_10bit && !ps->sws_ctx) {
            off += snprintf(buf + off, sz - off,
                "SWS: bypassed (10-bit passthrough, %s->full in shader)\n",
                is_full_range ? "full" : "limited");
        } else if (is_yuv420p && !ps->sws_ctx) {
            off += snprintf(buf + off, sz - off,
                "SWS: bypassed (8-bit passthrough, %s->full in shader)\n",
                is_full_range ? "full" : "limited");
        } else {
            off += snprintf(buf + off, sz - off,
                "SWS: format convert (SWS_LANCZOS + ED dither)\n");
        }
        off += snprintf(buf + off, sz - off,
            "GPU: Lanczos-2 luma, Catmull-Rom chroma, blue noise dither\n");
    }

    /* Audio track info */
    if (ps->aud_count > 1) {
        off += snprintf(buf + off, sz - off, "Audio Track: %s (%d/%d)\n",
            ps->aud_stream_names[ps->aud_selection],
            ps->aud_selection + 1, ps->aud_count);
    }

    /* Subtitle info */
    if (ps->sub_count > 0) {
        if (ps->sub_selection == 0) {
            off += snprintf(buf + off, sz - off, "Subtitles: off (%d available)\n",
                ps->sub_count);
        } else {
            off += snprintf(buf + off, sz - off, "Subtitles: %s\n",
                ps->sub_stream_names[ps->sub_selection - 1]);
        }
    } else {
        off += snprintf(buf + off, sz - off, "Subtitles: none found\n");
    }

    double duration = (ps->fmt_ctx && ps->fmt_ctx->duration != AV_NOPTS_VALUE)
        ? (double)ps->fmt_ctx->duration / AV_TIME_BASE : 0.0;
    double pos = ps->video_clock;
    off += snprintf(buf + off, sz - off, "Position:    %.1f / %.1f s\n", pos, duration);

    /* Playback diagnostics */
    off += snprintf(buf + off, sz - off, "\n--- Diagnostics ---\n");
    off += snprintf(buf + off, sz - off, "Decoded:     %d\n", ps->diag_frames_decoded);
    off += snprintf(buf + off, sz - off, "Displayed:   %d\n", ps->diag_frames_displayed);
    off += snprintf(buf + off, sz - off, "Dropped:     %d\n", ps->diag_frames_dropped);
    off += snprintf(buf + off, sz - off, "Multi-ticks: %d\n", ps->diag_multi_decodes);
    off += snprintf(buf + off, sz - off, "Stall snaps: %d\n", ps->diag_timer_snaps);
    off += snprintf(buf + off, sz - off, "Peak drift:  %.1f ms\n",
        ps->diag_max_av_drift * 1000.0);
    off += snprintf(buf + off, sz - off, "A/V bias:    %.1f ms\n",
        ps->av_bias * 1000.0);
}
