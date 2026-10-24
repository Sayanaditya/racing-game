#pragma once
#include <cstdint>
#include "config.h"
#include "track.h"
#include "camera.h"
#include "framebuffer.h"

struct RoadPoint {
    float cz = 0.0f;
    float wx = 0.0f;
    float wy = 0.0f;
    float sx = 0.0f;
    float sy = 0.0f;
    float sw = 0.0f;
};

struct RowInfo {                                 // 8 bytes
    int16_t  cx;          // road centre x (pixels, may be off-screen)
    int16_t  hw;          // road half-width (pixels)
    uint16_t seg;         // absolute segment index, or ROW_INVALID
    uint8_t  fog;         // 0..FOG_LEVELS-1
    uint8_t  band;        // 0/1 stripe parity
};
static_assert(sizeof(RowInfo) == 8, "RowInfo must be 8 bytes");

inline constexpr uint16_t ROW_INVALID = 0xFFFFu;
inline constexpr int      FOG_LEVELS  = 32;

struct RoadPalette {
    uint32_t grass;
    uint32_t rumble;
    uint32_t road;
    uint32_t lane;
};

struct RenderContext {
    alignas(64) RowInfo   rows[cfg::WORLD_ROWS];
    alignas(64) RoadPoint pts[cfg::DRAW_DIST + 1];
    int16_t     clipY[cfg::DRAW_DIST];   // rows >= clipY[n] are covered by nearer geometry
    int         topRoadY = cfg::WORLD_ROWS;
    int         horizonY = cfg::HORIZON_BASE;
    float       shiftX   = 0.0f;         // camera horizontal shift in pixels
    int         baseSeg  = 0;            // absolute index of segment under the camera
    float       basePct  = 0.0f;         // 0..1 progress inside it
    RoadPalette pal[FOG_LEVELS][2];      // built at init (or theme switch)
    uint32_t    skyLut[256];
    uint32_t    fogColor = 0;
    uint8_t     fogLut[cfg::DRAW_DIST];
    uint16_t    fogAlpha[FOG_LEVELS];
    alignas(64) uint32_t mountainTex[512 * 48];
    float       bgX = 0.0f;              // mountain parallax horizontal offset (0..512)
    int         theme = 0;               // 0: Classic Daylight, 1: Synthwave Sunset, 2: Neon Night, 3: Alpine
};

void initRenderContext(RenderContext& ctx, int theme = 0);
void projectRoad(const Track& track, const Camera& cam, RenderContext& ctx);
void fillRows(const RenderContext& ctx, Framebuffer& fb, int yBegin, int yEnd);
