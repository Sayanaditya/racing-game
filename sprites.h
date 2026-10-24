#pragma once
#include <cstdint>
#include "config.h"
#include "track.h"
#include "roadrender.h"
#include "framebuffer.h"

enum SpriteId : uint16_t {
    SPR_TREE = 0,
    SPR_PINE,
    SPR_BUSH,
    SPR_LAMP,
    SPR_SIGN,
    SPR_ROCK,
    SPR_CAR_RED,
    SPR_CAR_BLUE,
    SPR_CAR_YELLOW,
    SPR_TRUCK,
    SPR_COUNT
};

struct SpriteImage {                       // pixels live in SpriteAtlas::pool
    const uint32_t* px = nullptr;          // ARGB32, alpha == 0 -> transparent (colour key)
    uint16_t w = 0, h = 0;                 // source pixels; anchor is bottom-centre (w/2, h)
    float    worldW = 0.0f, worldH = 0.0f; // size in wu (also the collision box in Phase 6)
    uint8_t  xMin[128] = {};               // per source row: first opaque pixel
    uint8_t  xMax[128] = {};               // per source row: one-past-last opaque pixel (xMin>=xMax: empty)
};

struct SpriteAtlas {
    alignas(64) uint32_t pool[96 * 1024];  // 384 KB total, all sprites <= 64x128
    SpriteImage img[SPR_COUNT];
};

void buildSpriteAtlas(SpriteAtlas& atlas);

struct Car {                               // traffic, pooled
    float    z = 0.0f;                     // world z, wrapped
    float    offset = 0.0f;                // normalised lateral
    float    speed = 0.0f;                 // wu/s
    uint16_t spriteId = SPR_CAR_RED;
    uint8_t  active = 0;
    uint8_t  pad = 0;
};

struct alignas(16) DrawSprite {            // 16 bytes
    int16_t  sx;                           // screen anchor x (bottom-centre)
    int16_t  sy;                           // screen anchor y (bottom-centre)
    uint16_t wPx;                          // destination width
    uint16_t hPx;                          // destination height
    int16_t  clipY;                        // visible rows are < clipY
    uint16_t img;                          // SpriteId
    uint8_t  fog;                          // 0..FOG_LEVELS-1
    uint8_t  flip;                         // 0 or 1
    uint16_t pad = 0;
};
static_assert(sizeof(DrawSprite) == 16, "DrawSprite must be 16 bytes");

struct DrawList {
    DrawSprite items[cfg::MAX_DRAW_SPRITES];
    int count = 0;
};

inline constexpr int   SPRITE_DIST   = 140;
inline constexpr float Z_SPRITE_NEAR = 400.0f;

// 2-multiply fog blend; a = fog * 256 / FOG_LEVELS, result alpha forced to 0xFF
inline uint32_t fogBlend(uint32_t c, uint32_t f, uint32_t a) {
    uint32_t rb = ((((c & 0x00FF00FFu) * (256u - a)) + ((f & 0x00FF00FFu) * a)) >> 8) & 0x00FF00FFu;
    uint32_t g  = ((((c & 0x0000FF00u) * (256u - a)) + ((f & 0x0000FF00u) * a)) >> 8) & 0x0000FF00u;
    return 0xFF000000u | rb | g;
}

void buildDrawList(const Track& track, const Car* cars, int carCount,
                   const RenderContext& ctx, const SpriteAtlas& atlas, DrawList& list);

void drawSprites(const DrawList& list, const SpriteAtlas& atlas, const RenderContext& ctx,
                 Framebuffer& fb, int yBegin, int yEnd);

void blitUnscaledKeyed(const uint32_t* src, int w, int h, Framebuffer& fb, int dstX, int dstY);
