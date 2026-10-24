#pragma once
#include <cstdint>
#include <cstring>
#include <algorithm>
#include "config.h"

struct Framebuffer {
    static constexpr int W = cfg::SCREEN_W;
    static constexpr int H = cfg::SCREEN_H;
    static constexpr int STRIDE = W;                 // in pixels
    alignas(64) uint32_t px[W * H];                  // 0xFFRRGGBB

    uint32_t*       row(int y)       { return px + y * STRIDE; }
    const uint32_t* row(int y) const { return px + y * STRIDE; }
    void clear(uint32_t c)           { std::fill(px, px + W * H, c); }

    void drawTestPattern(uint64_t tickCount);
};

static_assert(sizeof(Framebuffer) == 320 * 240 * 4, "no padding allowed");
static_assert(Framebuffer::STRIDE * 4 % 64 == 0, "row must be a cache-line multiple");
static_assert((cfg::WORLD_ROWS / cfg::NUM_SLICES) * Framebuffer::STRIDE * 4 % 64 == 0,
              "slice byte size must be a multiple of 64-byte cache line");

// High-resolution 960x720 software presentation buffer supporting variable pixel size (1px..8px)
struct DisplayBuffer {
    static constexpr int W = 960;
    static constexpr int H = 720;
    static constexpr int STRIDE = W;
    alignas(64) uint32_t px[W * H];

    uint32_t*       row(int y)       { return px + y * STRIDE; }
    const uint32_t* row(int y) const { return px + y * STRIDE; }
    void clear(uint32_t c)           { std::fill(px, px + W * H, c); }

    // Edge-preserving Scale3x upscale from 320x240 base Framebuffer into 960x720 DisplayBuffer
    void upscaleFromBase(const Framebuffer& src, int pixelSize);
    // Applies variable pixel-block quantization (pixelSize > 1) and optional CRT scanlines
    void applyPostProcess(int pixelSize, bool crtScanlines);

    // High-resolution pure software raster primitives (960x720 coordinate space)
    void fillSpan(int y, int x0, int x1, uint32_t color);
    void drawRect(int x, int y, int w, int h, uint32_t color);
    void drawRectAlpha(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    void drawBorder(int x, int y, int w, int h, int thickness, uint32_t color);
    void drawLine(int x0, int y0, int x1, int y1, int halfW, uint32_t color);
    void drawCircle(int cx, int cy, int r, uint32_t color, bool filled = true);
    void drawText(int x, int y, const char* text, uint32_t color, int scale = 2, bool shadow = true);
    static int textWidth(const char* text, int scale = 2);
};

struct FrameStats {
    static constexpr int N = 128;
    float ms[N] = {};
    int head = 0;
    int count = 0;

    void push(float v) {
        ms[head] = v;
        head = (head + 1) % N;
        if (count < N) ++count;
    }
    float avg() const;
    float p99() const;
};
