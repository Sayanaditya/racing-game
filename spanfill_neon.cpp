#include "spanfill.h"
#include "framebuffer.h"
#include <cstdlib>
#include <cstring>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

FillPixelsFn g_fillPixels = fillPixelsScalar;
static bool g_useSIMD = false;

void fillPixelsScalar(uint32_t* d, int n, uint32_t c) {
    std::fill(d, d + n, c);
}

void fillPixelsNEON(uint32_t* d, int n, uint32_t c) {
#if defined(__aarch64__) || defined(_M_ARM64)
    if (n < 16) {
        for (int i = 0; i < n; ++i) d[i] = c;
        return;
    }
    const uint32x4_t v = vdupq_n_u32(c);
    uint32_t mis = (reinterpret_cast<uintptr_t>(d) & 15u) >> 2;
    if (mis) {
        vst1q_u32(d, v);
        int adv = static_cast<int>(4u - mis);
        d += adv;
        n -= adv;
    }
    while (n >= 16) {
        vst1q_u32(d,      v);
        vst1q_u32(d + 4,  v);
        vst1q_u32(d + 8,  v);
        vst1q_u32(d + 12, v);
        d += 16;
        n -= 16;
    }
    while (n >= 4) {
        vst1q_u32(d, v);
        d += 4;
        n -= 4;
    }
    if (n > 0) {
        vst1q_u32(d + n - 4, v); // overlapping tail, same colour
    }
#else
    fillPixelsScalar(d, n, c);
#endif
}

void initSpanFill(bool forceScalar) {
    const char* envScalar = std::getenv("FORCE_SCALAR");
    if (forceScalar || (envScalar && envScalar[0] == '1')) {
        g_fillPixels = fillPixelsScalar;
        g_useSIMD = false;
        return;
    }
#if defined(__aarch64__) || defined(_M_ARM64)
    g_fillPixels = fillPixelsNEON;
    g_useSIMD = true;
#elif defined(__x86_64__) || defined(_M_X64)
    #if defined(__GNUC__) || defined(__clang__)
    if (__builtin_cpu_supports("avx2")) {
        g_fillPixels = fillPixelsAVX2;
        g_useSIMD = true;
    } else {
        g_fillPixels = fillPixelsScalar;
        g_useSIMD = false;
    }
    #else
    g_fillPixels = fillPixelsScalar;
    g_useSIMD = false;
    #endif
#else
    g_fillPixels = fillPixelsScalar;
    g_useSIMD = false;
#endif
}

void fadeRowScalar(uint32_t* d, int n, int level) {
    if (level <= 0) return;
    if (level >= 4) {
        std::fill(d, d + n, 0xFF000000u);
        return;
    }
    for (int i = 0; i < n; ++i) {
        d[i] = fadePixelSWAR(d[i], level);
    }
}

void fadeRowSIMD(uint32_t* d, int n, int level) {
    if (!g_useSIMD || level <= 0) {
        fadeRowScalar(d, n, level);
        return;
    }
    if (level >= 4) {
        g_fillPixels(d, n, 0xFF000000u);
        return;
    }
#if defined(__aarch64__) || defined(_M_ARM64)
    const uint32x4_t alphaMask = vdupq_n_u32(0xFF000000u);
    const uint32x4_t m3f = vdupq_n_u32(0x003F3F3Fu);
    const uint32x4_t m7f = vdupq_n_u32(0x007F7F7Fu);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32x4_t c = vld1q_u32(d + i);
        uint32x4_t out;
        if (level == 1) {
            uint32x4_t sub = vandq_u32(vshrq_n_u32(c, 2), m3f);
            out = vorrq_u32(vsubq_u32(c, sub), alphaMask);
        } else if (level == 2) {
            out = vorrq_u32(vandq_u32(vshrq_n_u32(c, 1), m7f), alphaMask);
        } else {
            out = vorrq_u32(vandq_u32(vshrq_n_u32(c, 2), m3f), alphaMask);
        }
        vst1q_u32(d + i, out);
    }
    for (; i < n; ++i) {
        d[i] = fadePixelSWAR(d[i], level);
    }
#else
    fadeRowScalar(d, n, level);
#endif
}

void fadeRows(Framebuffer& fb, int yBegin, int yEnd, int level) {
    if (level <= 0) return;
    yBegin = std::max(0, yBegin);
    yEnd   = std::min(cfg::SCREEN_H, yEnd);
    for (int y = yBegin; y < yEnd; ++y) {
        fadeRowSIMD(fb.row(y), cfg::SCREEN_W, level);
    }
}

void tintSpanScalar(uint32_t* row, int x0, int x1, int shiftMode) {
    x0 = std::max(0, x0);
    x1 = std::min(cfg::SCREEN_W, x1);
    for (int x = x0; x < x1; ++x) {
        uint32_t c = row[x];
        if (shiftMode == 1) {
            row[x] = 0xFF000000u | (c - ((c >> 2) & 0x003F3F3Fu)); // ~75% brightness
        } else if (shiftMode == 2) {
            row[x] = 0xFF000000u | (c - ((c >> 3) & 0x001F1F1Fu)); // ~88% brightness
        }
    }
}

void tintSpanSIMD(uint32_t* row, int x0, int x1, int shiftMode) {
    if (!g_useSIMD) {
        tintSpanScalar(row, x0, x1, shiftMode);
        return;
    }
#if defined(__aarch64__) || defined(_M_ARM64)
    x0 = std::max(0, x0);
    x1 = std::min(cfg::SCREEN_W, x1);
    int x = x0;
    const uint32x4_t alphaMask = vdupq_n_u32(0xFF000000u);
    const uint32x4_t m3f = vdupq_n_u32(0x003F3F3Fu);
    const uint32x4_t m1f = vdupq_n_u32(0x001F1F1Fu);
    for (; x + 4 <= x1; x += 4) {
        uint32x4_t c = vld1q_u32(row + x);
        uint32x4_t sub = (shiftMode == 1)
            ? vandq_u32(vshrq_n_u32(c, 2), m3f)
            : vandq_u32(vshrq_n_u32(c, 3), m1f);
        vst1q_u32(row + x, vorrq_u32(vsubq_u32(c, sub), alphaMask));
    }
    for (; x < x1; ++x) {
        uint32_t c = row[x];
        row[x] = (shiftMode == 1)
            ? (0xFF000000u | (c - ((c >> 2) & 0x003F3F3Fu)))
            : (0xFF000000u | (c - ((c >> 3) & 0x001F1F1Fu)));
    }
#else
    tintSpanScalar(row, x0, x1, shiftMode);
#endif
}

void copyKeyedSpan(uint32_t* dst, const uint32_t* src, int n) {
    if (n <= 0) return;
#if defined(__aarch64__) || defined(_M_ARM64)
    if (g_useSIMD) {
        int i = 0;
        const uint32x4_t alphaMask = vdupq_n_u32(0xFF000000u);
        for (; i + 4 <= n; i += 4) {
            uint32x4_t s = vld1q_u32(src + i);
            uint32x4_t d = vld1q_u32(dst + i);
            uint32x4_t opaque = vtstq_u32(s, alphaMask);
            vst1q_u32(dst + i, vbslq_u32(opaque, s, d));
        }
        for (; i < n; ++i) {
            if ((src[i] >> 24) != 0) dst[i] = src[i];
        }
        return;
    }
#endif
    for (int i = 0; i < n; ++i) {
        if ((src[i] >> 24) != 0) dst[i] = src[i];
    }
}
