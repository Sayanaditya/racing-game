#include "sprites.h"
#include "spanfill.h"
#include "mathutil.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cassert>

namespace {

void computeRowBounds(SpriteImage& im) {
    for (int y = 0; y < 128; ++y) {
        im.xMin[y] = 1;
        im.xMax[y] = 0;
    }
    for (int y = 0; y < im.h; ++y) {
        int first = im.w;
        int last  = -1;
        const uint32_t* row = im.px + y * im.w;
        for (int x = 0; x < im.w; ++x) {
            if ((row[x] >> 24) != 0) {
                if (x < first) first = x;
                last = x;
            }
        }
        if (first <= last) {
            im.xMin[y] = static_cast<uint8_t>(first);
            im.xMax[y] = static_cast<uint8_t>(last + 1);
        } else {
            im.xMin[y] = 1;
            im.xMax[y] = 0;
        }
    }
}

void drawCarSprite(uint32_t* buf, int w, int h, uint32_t bodyMain, uint32_t bodyDark, uint32_t bodyLight) {
    std::fill(buf, buf + w * h, 0x00000000u);
    auto setPx = [&](int x, int y, uint32_t c) {
        if (x >= 0 && x < w && y >= 0 && y < h) buf[y * w + x] = c;
    };

    // Tyres (bottom left and right)
    for (int y = h - 9; y < h; ++y) {
        for (int x = 3; x < 10; ++x) setPx(x, y, RGB(24, 24, 28));
        for (int x = w - 10; x < w - 3; ++x) setPx(x, y, RGB(24, 24, 28));
    }
    // Lower chassis & diffuser
    for (int y = h - 14; y < h - 3; ++y) {
        for (int x = 2; x < w - 2; ++x) {
            setPx(x, y, (y > h - 7) ? bodyDark : bodyMain);
        }
    }
    // Upper cabin / roof tapering inward
    for (int y = 4; y < h - 14; ++y) {
        int inset = 4 + (h - 14 - y) / 2;
        for (int x = inset; x < w - inset; ++x) {
            setPx(x, y, (y < 8) ? bodyLight : bodyMain);
        }
    }
    // Rear window
    for (int y = 7; y < 15; ++y) {
        int inset = 7 + (15 - y) / 2;
        for (int x = inset; x < w - inset; ++x) {
            setPx(x, y, (x + y) % 3 == 0 ? RGB(70, 95, 130) : RGB(38, 52, 74));
        }
    }
    // Tail lights (glowing red/orange)
    for (int y = h - 12; y < h - 8; ++y) {
        for (int x = 4; x < 11; ++x) setPx(x, y, RGB(255, 35, 35));
        for (int x = w - 11; x < w - 4; ++x) setPx(x, y, RGB(255, 35, 35));
        setPx(6, y, RGB(255, 180, 80));
        setPx(w - 7, y, RGB(255, 180, 80));
    }
    // License plate & twin exhausts
    for (int y = h - 9; y < h - 6; ++y) {
        for (int x = w / 2 - 4; x < w / 2 + 4; ++x) setPx(x, y, RGB(240, 220, 90));
    }
    setPx(11, h - 4, RGB(180, 185, 195));
    setPx(12, h - 4, RGB(180, 185, 195));
    setPx(w - 13, h - 4, RGB(180, 185, 195));
    setPx(w - 12, h - 4, RGB(180, 185, 195));
}

} // namespace

void buildSpriteAtlas(SpriteAtlas& atlas) {
    std::memset(atlas.pool, 0, sizeof(atlas.pool));
    uint32_t* cursor = atlas.pool;

    auto allocSprite = [&](SpriteId id, uint16_t w, uint16_t h, float worldW, float worldH) -> uint32_t* {
        SpriteImage& im = atlas.img[id];
        im.px = cursor;
        im.w = w;
        im.h = h;
        im.worldW = worldW;
        im.worldH = worldH;
        uint32_t* out = cursor;
        cursor += static_cast<size_t>(w) * static_cast<size_t>(h);
        assert(cursor <= atlas.pool + (96 * 1024));
        return out;
    };

    // 1. SPR_TREE (40 x 64, 700 x 1100 wu)
    {
        int w = 40, h = 64;
        uint32_t* px = allocSprite(SPR_TREE, w, h, 700.0f, 1100.0f);
        // Trunk
        for (int y = 40; y < h; ++y) {
            int halfT = 3 + (y - 40) / 8;
            for (int x = w / 2 - halfT; x <= w / 2 + halfT; ++x) {
                px[y * w + x] = (x < w / 2) ? RGB(115, 72, 38) : RGB(82, 48, 24);
            }
        }
        // Leafy canopy (3 overlapping circles)
        const int cx[3] = { 20, 13, 27 };
        const int cy[3] = { 20, 29, 29 };
        const int cr[3] = { 16, 12, 12 };
        for (int y = 2; y < 46; ++y) {
            for (int x = 1; x < w - 1; ++x) {
                bool inside = false;
                for (int k = 0; k < 3; ++k) {
                    int dx = x - cx[k], dy = y - cy[k];
                    if (dx * dx + dy * dy <= cr[k] * cr[k]) inside = true;
                }
                if (inside) {
                    uint32_t col = (y < 16) ? RGB(46, 195, 52)
                                 : (y < 30) ? RGB(26, 156, 34)
                                            : RGB(14, 112, 22);
                    if (((x + y) & 3) == 0) col = mathutil::lerpColor(col, RGB(90, 230, 80), 0.25f);
                    px[y * w + x] = col;
                }
            }
        }
        computeRowBounds(atlas.img[SPR_TREE]);
    }

    // 2. SPR_PINE (32 x 72, 500 x 1200 wu)
    {
        int w = 32, h = 72;
        uint32_t* px = allocSprite(SPR_PINE, w, h, 500.0f, 1200.0f);
        for (int y = 54; y < h; ++y) {
            for (int x = w / 2 - 3; x <= w / 2 + 2; ++x) {
                px[y * w + x] = (x < w / 2) ? RGB(104, 64, 34) : RGB(74, 44, 22);
            }
        }
        for (int tier = 0; tier < 3; ++tier) {
            int yTop = 4 + tier * 14;
            int yBot = yTop + 26;
            for (int y = yTop; y < yBot && y < 58; ++y) {
                int halfW = ((y - yTop) * (10 + tier * 2)) / 24 + 1;
                for (int x = w / 2 - halfW; x <= w / 2 + halfW; ++x) {
                    if (x >= 0 && x < w) {
                        uint32_t col = (x < w / 2) ? RGB(20, 138, 52) : RGB(12, 98, 36);
                        if (y == yBot - 1) col = RGB(8, 72, 26);
                        px[y * w + x] = col;
                    }
                }
            }
        }
        computeRowBounds(atlas.img[SPR_PINE]);
    }

    // 3. SPR_BUSH (24 x 16, 400 x 270 wu)
    {
        int w = 24, h = 16;
        uint32_t* px = allocSprite(SPR_BUSH, w, h, 400.0f, 270.0f);
        for (int y = 2; y < h; ++y) {
            for (int x = 1; x < w - 1; ++x) {
                float nx = (x - 12.0f) / 10.5f;
                float ny = (y - 10.0f) / 7.5f;
                if (nx * nx + ny * ny <= 1.0f) {
                    px[y * w + x] = (y < 8) ? RGB(42, 184, 46) : RGB(22, 132, 26);
                }
            }
        }
        computeRowBounds(atlas.img[SPR_BUSH]);
    }

    // 4. SPR_LAMP (16 x 56, 200 x 1500 wu)
    {
        int w = 16, h = 56;
        uint32_t* px = allocSprite(SPR_LAMP, w, h, 200.0f, 1500.0f);
        // Vertical post anchored near x = 7..8
        for (int y = 6; y < h; ++y) {
            px[y * w + 7] = RGB(160, 168, 178);
            px[y * w + 8] = RGB(110, 116, 126);
        }
        // Horizontal arm extending toward road (left side of sprite, x = 1..8, flipped for left roadside)
        for (int x = 1; x <= 8; ++x) {
            px[5 * w + x] = RGB(175, 182, 192);
            px[6 * w + x] = RGB(130, 136, 146);
        }
        // Glowing lamp head
        for (int y = 6; y <= 9; ++y) {
            for (int x = 1; x <= 5; ++x) {
                px[y * w + x] = RGB(255, 245, 160);
            }
        }
        computeRowBounds(atlas.img[SPR_LAMP]);
    }

    // 5. SPR_SIGN (48 x 40, 900 x 750 wu)
    {
        int w = 48, h = 40;
        uint32_t* px = allocSprite(SPR_SIGN, w, h, 900.0f, 750.0f);
        // Two support posts
        for (int y = 26; y < h; ++y) {
            for (int dx = 0; dx < 3; ++dx) {
                px[y * w + 10 + dx] = RGB(120, 124, 132);
                px[y * w + w - 13 + dx] = RGB(120, 124, 132);
            }
        }
        // Billboard frame and vibrant racing graphic
        for (int y = 2; y < 27; ++y) {
            for (int x = 2; x < w - 2; ++x) {
                bool border = (y <= 3 || y >= 25 || x <= 3 || x >= w - 4);
                if (border) {
                    px[y * w + x] = RGB(240, 240, 245);
                } else {
                    bool stripe = ((x + y * 2) / 6) % 2 == 0;
                    px[y * w + x] = stripe ? RGB(220, 36, 48) : RGB(28, 72, 190);
                }
            }
        }
        computeRowBounds(atlas.img[SPR_SIGN]);
    }

    // 6. SPR_ROCK (24 x 16, 400 x 270 wu)
    {
        int w = 24, h = 16;
        uint32_t* px = allocSprite(SPR_ROCK, w, h, 400.0f, 270.0f);
        for (int y = 3; y < h; ++y) {
            for (int x = 2; x < w - 2; ++x) {
                float nx = (x - 12.0f) / 9.5f;
                float ny = (y - 11.0f) / 7.0f;
                if (nx * nx + ny * ny <= 1.0f) {
                    px[y * w + x] = (y < 9 && x < 14) ? RGB(156, 160, 168) : RGB(108, 112, 120);
                }
            }
        }
        computeRowBounds(atlas.img[SPR_ROCK]);
    }

    // 7..9. Cars (40 x 32, 520 x 416 wu)
    {
        uint32_t* pxRed = allocSprite(SPR_CAR_RED, 40, 32, 520.0f, 416.0f);
        drawCarSprite(pxRed, 40, 32, RGB(220, 32, 40), RGB(140, 18, 24), RGB(255, 95, 95));
        computeRowBounds(atlas.img[SPR_CAR_RED]);

        uint32_t* pxBlue = allocSprite(SPR_CAR_BLUE, 40, 32, 520.0f, 416.0f);
        drawCarSprite(pxBlue, 40, 32, RGB(36, 108, 230), RGB(20, 62, 145), RGB(105, 168, 255));
        computeRowBounds(atlas.img[SPR_CAR_BLUE]);

        uint32_t* pxYellow = allocSprite(SPR_CAR_YELLOW, 40, 32, 520.0f, 416.0f);
        drawCarSprite(pxYellow, 40, 32, RGB(238, 190, 24), RGB(165, 124, 12), RGB(255, 230, 100));
        computeRowBounds(atlas.img[SPR_CAR_YELLOW]);
    }

    // 10. SPR_TRUCK (48 x 56, 640 x 750 wu)
    {
        int w = 48, h = 56;
        uint32_t* px = allocSprite(SPR_TRUCK, w, h, 640.0f, 750.0f);
        // Dual tyres
        for (int y = h - 10; y < h; ++y) {
            for (int x = 4; x < 13; ++x) px[y * w + x] = RGB(24, 24, 28);
            for (int x = w - 13; x < w - 4; ++x) px[y * w + x] = RGB(24, 24, 28);
        }
        // Box trailer container
        for (int y = 3; y < h - 8; ++y) {
            for (int x = 3; x < w - 3; ++x) {
                bool frame = (x == 3 || x == w - 4 || y == 3 || y == h - 9 || x == w / 2);
                px[y * w + x] = frame ? RGB(150, 158, 170) : RGB(212, 218, 226);
            }
        }
        // Warning chevron bumper & tail lights
        for (int y = h - 13; y < h - 9; ++y) {
            for (int x = 5; x < w - 5; ++x) {
                px[y * w + x] = (((x + y) / 3) & 1) ? RGB(230, 50, 40) : RGB(245, 245, 245);
            }
        }
        computeRowBounds(atlas.img[SPR_TRUCK]);
    }
}

void buildDrawList(const Track& track, const Car* cars, int carCount,
                   const RenderContext& ctx, const SpriteAtlas& atlas, DrawList& list) {
    int16_t headOf[SPRITE_DIST];
    int16_t nextCar[cfg::MAX_TRAFFIC];
    std::fill(headOf, headOf + SPRITE_DIST, static_cast<int16_t>(-1));
    std::fill(nextCar, nextCar + cfg::MAX_TRAFFIC, static_cast<int16_t>(-1));

    const int count = track.count;
    const int maxCars = std::min(carCount, cfg::MAX_TRAFFIC);

    for (int i = 0; i < maxCars; ++i) {
        if (!cars[i].active) continue;
        float zNorm = mathutil::wrap(cars[i].z, track.length) / cfg::SEG_LEN;
        int segIdx  = static_cast<int>(std::floor(zNorm)) % count;
        int n       = (segIdx - ctx.baseSeg + count) % count;
        if (n >= 0 && n < SPRITE_DIST) {
            nextCar[i] = headOf[n];
            headOf[n]  = static_cast<int16_t>(i);
        }
    }

    list.count = 0;
    const float centerX = (cfg::SCREEN_W * 0.5f) + ctx.shiftX;

    auto emitSprite = [&](float offset, uint16_t spriteId, uint8_t flip, int n, float t) {
        if (list.count >= cfg::MAX_DRAW_SPRITES) return;
        const SpriteImage& img = atlas.img[spriteId];
        const RoadPoint& p0 = ctx.pts[n];
        const RoadPoint& p1 = ctx.pts[n + 1];

        float cz = (1.0f - t) * p0.cz + t * p1.cz;
        if (cz < Z_SPRITE_NEAR) return;

        float wx = (1.0f - t) * p0.wx + t * p1.wx + offset * cfg::ROAD_HALF_W;
        float wy = (1.0f - t) * p0.wy + t * p1.wy;
        float s  = cfg::FOCAL_PX / cz;

        int sx = static_cast<int>(std::lround(centerX + s * wx));
        int sy = static_cast<int>(std::lround(static_cast<float>(ctx.horizonY) - s * wy));
        int wPx = static_cast<int>(std::lround(s * img.worldW));
        int hPx = static_cast<int>(std::lround(s * img.worldH));

        if (hPx < 1 || hPx > 1024 || wPx < 1) return;
        if (sx + wPx / 2 < 0 || sx - wPx / 2 >= cfg::SCREEN_W) return;

        int clip = ctx.clipY[n];
        if (sy - hPx >= clip || sy <= 0) return;

        DrawSprite& ds = list.items[list.count++];
        ds.sx    = static_cast<int16_t>(sx);
        ds.sy    = static_cast<int16_t>(sy);
        ds.wPx   = static_cast<uint16_t>(wPx);
        ds.hPx   = static_cast<uint16_t>(hPx);
        ds.clipY = static_cast<int16_t>(clip);
        ds.img   = spriteId;
        ds.fog   = ctx.fogLut[n];
        ds.flip  = flip;
    };

    // Far -> near iteration over segments
    for (int n = SPRITE_DIST - 1; n >= 0; --n) {
        const Segment& seg = track.segs[(ctx.baseSeg + n) % count];
        for (int k = 0; k < seg.sceneryCount; ++k) {
            const Scenery& sc = track.scenery[seg.sceneryBegin + k];
            emitSprite(sc.offset, sc.spriteId, sc.flip, n, 0.0f);
        }
        for (int i = headOf[n]; i != -1; i = nextCar[i]) {
            float t = mathutil::frac(mathutil::wrap(cars[i].z, track.length) / cfg::SEG_LEN);
            emitSprite(cars[i].offset, cars[i].spriteId, 0, n, t);
        }
    }
    assert(list.count <= cfg::MAX_DRAW_SPRITES);
}

void drawSprites(const DrawList& list, const SpriteAtlas& atlas, const RenderContext& ctx,
                 Framebuffer& fb, int yBegin, int yEnd) {
    const int W = cfg::SCREEN_W;
    for (int i = 0; i < list.count; ++i) {
        const DrawSprite& s = list.items[i];
        const SpriteImage& img = atlas.img[s.img];

        int dstL = static_cast<int>(s.sx) - static_cast<int>(s.wPx) / 2;
        int dstT = static_cast<int>(s.sy) - static_cast<int>(s.hPx);

        int y0 = std::max({ dstT, yBegin, 0 });
        int y1 = std::min({ static_cast<int>(s.sy), static_cast<int>(s.clipY), yEnd, cfg::WORLD_ROWS });
        int x0 = std::max(dstL, 0);
        int x1 = std::min(dstL + static_cast<int>(s.wPx), W);
        if (y0 >= y1 || x0 >= x1) continue;

        int64_t stepX = (static_cast<int64_t>(img.w) << 16) / static_cast<int64_t>(s.wPx);
        int64_t stepY = (static_cast<int64_t>(img.h) << 16) / static_cast<int64_t>(s.hPx);
        int64_t v     = static_cast<int64_t>(y0 - dstT) * stepY;

        uint32_t fogAlpha = ctx.fogAlpha[s.fog];
        uint32_t fogCol   = ctx.fogColor;

        for (int y = y0; y < y1; ++y) {
            int sr = static_cast<int>(v >> 16);
            v += stepY;
            if (sr < 0 || sr >= img.h) continue;
            if (img.xMin[sr] >= img.xMax[sr]) continue;

            const uint32_t* src = img.px + sr * img.w;
            uint32_t* dst = fb.row(y);

            int srcMin = s.flip ? (img.w - img.xMax[sr]) : img.xMin[sr];
            int srcMax = s.flip ? (img.w - img.xMin[sr]) : img.xMax[sr];

            int dxa = std::max(x0, dstL + (srcMin * static_cast<int>(s.wPx)) / static_cast<int>(img.w));
            int dxb = std::min(x1, dstL + (srcMax * static_cast<int>(s.wPx) + static_cast<int>(img.w) - 1) / static_cast<int>(img.w));
            if (dxa >= dxb) continue;

            int64_t u = static_cast<int64_t>(dxa - dstL) * stepX;
            for (int x = dxa; x < dxb; ++x) {
                int sc = static_cast<int>(u >> 16);
                u += stepX;
                if (sc < 0 || sc >= img.w) continue;
                if (s.flip) sc = img.w - 1 - sc;
                uint32_t c = src[sc];
                if ((c >> 24) != 0) {
                    dst[x] = (s.fog == 0) ? c : fogBlend(c, fogCol, fogAlpha);
                }
            }
        }
    }
}

void blitUnscaledKeyed(const uint32_t* src, int w, int h, Framebuffer& fb, int dstX, int dstY) {
    int y0 = std::max(dstY, 0);
    int y1 = std::min(dstY + h, cfg::SCREEN_H);
    int x0 = std::max(dstX, 0);
    int x1 = std::min(dstX + w, cfg::SCREEN_W);
    if (y0 >= y1 || x0 >= x1) return;

    for (int y = y0; y < y1; ++y) {
        const uint32_t* sRow = src + (y - dstY) * w + (x0 - dstX);
        uint32_t* dRow = fb.row(y) + x0;
        copyKeyedSpan(dRow, sRow, x1 - x0);
    }
}
