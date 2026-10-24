#pragma once
#include <cstdint>
#include <algorithm>
#include "config.h"

struct Framebuffer;

using FillPixelsFn = void (*)(uint32_t* d, int n, uint32_t c);
extern FillPixelsFn g_fillPixels;

void initSpanFill(bool forceScalar = false);
void fillPixelsScalar(uint32_t* d, int n, uint32_t c);
void fillPixelsNEON(uint32_t* d, int n, uint32_t c);
void fillPixelsAVX2(uint32_t* d, int n, uint32_t c);

inline void fillSpan(uint32_t* row, int x0, int x1, uint32_t c) {
    x0 = std::max(x0, 0);
    x1 = std::min(x1, cfg::SCREEN_W);
    int n = x1 - x0;
    if (n <= 0) return;
    uint32_t* d = row + x0;
    if (n < 16) {
        for (int i = 0; i < n; ++i) d[i] = c;
        return;
    }
    g_fillPixels(d, n, c);
}

// SWAR / SIMD fade & visor tint helpers (Phase 5.5, Phase 6.5, Phase 8.3)
inline uint32_t fadePixelSWAR(uint32_t c, int level) {
    switch (level) {
        case 0: return c;
        case 1: return 0xFF000000u | (c - ((c >> 2) & 0x003F3F3Fu)); // 75%
        case 2: return 0xFF000000u | ((c >> 1) & 0x007F7F7Fu);       // 50%
        case 3: return 0xFF000000u | ((c >> 2) & 0x003F3F3Fu);       // 25%
        default: return 0xFF000000u;                                 // 0% (black)
    }
}

void fadeRowScalar(uint32_t* d, int n, int level);
void fadeRowSIMD(uint32_t* d, int n, int level);
void fadeRows(Framebuffer& fb, int yBegin, int yEnd, int level);

void tintSpanScalar(uint32_t* row, int x0, int x1, int shiftMode); // shiftMode 1 = 75%, 2 = 88%
void tintSpanSIMD(uint32_t* row, int x0, int x1, int shiftMode);
void copyKeyedSpan(uint32_t* dst, const uint32_t* src, int n);
