#include "roadrender.h"
#include "spanfill.h"
#include "mathutil.h"
#include <cmath>
#include <algorithm>
#include <cassert>

namespace {
inline constexpr int RUMBLE_NUM = 20; // ~8% of half-width (20 / 256)
inline constexpr int LANE_NUM   = 6;  // ~2.3% of half-width (6 / 256)
} // namespace

void initRenderContext(RenderContext& ctx, int theme) {
    ctx.theme = theme;
    ctx.horizonY = cfg::HORIZON_BASE;
    ctx.shiftX   = 0.0f;
    ctx.baseSeg  = 0;
    ctx.basePct  = 0.0f;
    ctx.topRoadY = cfg::WORLD_ROWS;
    ctx.bgX      = 0.0f;

    for (int y = 0; y < cfg::WORLD_ROWS; ++y) {
        ctx.rows[y] = { 160, 0, ROW_INVALID, 0, 0 };
    }
    for (int n = 0; n < cfg::DRAW_DIST; ++n) {
        ctx.clipY[n] = cfg::WORLD_ROWS;
        int f = (n * n * FOG_LEVELS) / (cfg::DRAW_DIST * cfg::DRAW_DIST);
        ctx.fogLut[n] = static_cast<uint8_t>(std::min(FOG_LEVELS - 1, f));
    }
    for (int f = 0; f < FOG_LEVELS; ++f) {
        ctx.fogAlpha[f] = static_cast<uint16_t>((f * 256) / FOG_LEVELS);
    }

    // Theme palettes (Theme 0 is exact Phase 2.6 retro palette)
    RoadPalette base[2];
    uint32_t skyTop, skyBot;
    uint32_t mtnTop, mtnBot;

    if (theme == 1) {
        // Synthwave Sunset
        base[0] = { RGB(36, 18, 64),  RGB(255, 40, 140), RGB(48, 38, 72),  RGB(0, 240, 255) };
        base[1] = { RGB(26, 12, 50),  RGB(0, 230, 255),  RGB(40, 32, 62),  RGB(0, 240, 255) };
        ctx.fogColor = RGB(180, 70, 140);
        skyTop = RGB(20, 10, 56);
        skyBot = RGB(255, 110, 130);
        mtnTop = RGB(90, 30, 120);
        mtnBot = RGB(45, 15, 75);
    } else if (theme == 2) {
        // Cyber Neon Night
        base[0] = { RGB(10, 28, 38),  RGB(0, 220, 180),  RGB(34, 38, 48),  RGB(255, 220, 60) };
        base[1] = { RGB(8, 20, 28),   RGB(240, 240, 255),RGB(28, 32, 42),  RGB(255, 220, 60) };
        ctx.fogColor = RGB(18, 38, 64);
        skyTop = RGB(4, 8, 22);
        skyBot = RGB(24, 52, 92);
        mtnTop = RGB(28, 68, 110);
        mtnBot = RGB(12, 28, 52);
    } else if (theme == 3) {
        // Alpine Snow
        base[0] = { RGB(215, 230, 245), RGB(210, 45, 45),  RGB(96, 104, 116), RGB(245, 248, 255) };
        base[1] = { RGB(195, 212, 232), RGB(245, 248, 255),RGB(84, 92, 104),  RGB(245, 248, 255) };
        ctx.fogColor = RGB(210, 225, 242);
        skyTop = RGB(58, 110, 195);
        skyBot = RGB(200, 225, 248);
        mtnTop = RGB(150, 175, 205);
        mtnBot = RGB(95, 120, 155);
    } else if (theme == 4) {
        // Desert Canyon Dusk
        base[0] = { RGB(186, 98, 52),   RGB(240, 180, 40), RGB(82, 74, 72),   RGB(255, 220, 120) };
        base[1] = { RGB(164, 82, 42),   RGB(190, 45, 35),  RGB(70, 62, 60),   RGB(255, 220, 120) };
        ctx.fogColor = RGB(225, 145, 95);
        skyTop = RGB(72, 28, 68);
        skyBot = RGB(245, 150, 75);
        mtnTop = RGB(155, 65, 48);
        mtnBot = RGB(95, 38, 32);
    } else {
        // Theme 0: Authoritative Phase 2.6 Palette
        base[0] = { RGB(16, 170, 16), RGB(210, 30, 30),  RGB(118, 118, 124), RGB(232, 232, 232) };
        base[1] = { RGB(0, 148, 0),   RGB(240, 240, 240),RGB(104, 104, 110), RGB(104, 104, 110) };
        ctx.fogColor = RGB(170, 200, 230);
        skyTop = RGB(24, 84, 200);
        skyBot = RGB(150, 210, 255);
        mtnTop = RGB(92, 126, 172);
        mtnBot = RGB(60, 92, 136);
    }

    for (int f = 0; f < FOG_LEVELS; ++f) {
        float t = static_cast<float>(f) / static_cast<float>(FOG_LEVELS - 1);
        for (int b = 0; b < 2; ++b) {
            ctx.pal[f][b].grass  = mathutil::lerpColor(base[b].grass,  ctx.fogColor, t);
            ctx.pal[f][b].rumble = mathutil::lerpColor(base[b].rumble, ctx.fogColor, t);
            ctx.pal[f][b].road   = mathutil::lerpColor(base[b].road,   ctx.fogColor, t);
            ctx.pal[f][b].lane   = mathutil::lerpColor(base[b].lane,   ctx.fogColor, t);
        }
    }

    // Sky gradient quantised to 12 steps
    for (int i = 0; i < 256; ++i) {
        int step = (i * 12) / 256;
        float t = static_cast<float>(step) / 11.0f;
        ctx.skyLut[i] = mathutil::lerpColor(skyTop, skyBot, t);
    }

    // Mountain parallax strip (512x48 ARGB, sum of three sines, alpha 0 above ridge line)
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 512; ++x) {
            float fx = static_cast<float>(x) * mathutil::TWO_PI / 512.0f;
            float h = 20.0f
                    + 11.0f * std::sin(fx * 2.0f + 0.3f)
                    + 6.0f  * std::sin(fx * 5.0f + 1.7f)
                    + 3.5f  * std::sin(fx * 11.0f + 0.9f);
            int ridgeY = 48 - static_cast<int>(std::lround(h));
            if (y < ridgeY) {
                ctx.mountainTex[y * 512 + x] = 0x00000000u; // transparent above ridge
            } else {
                float ty = static_cast<float>(y - ridgeY) / static_cast<float>(std::max(1, 48 - ridgeY));
                uint32_t col = mathutil::lerpColor(mtnTop, mtnBot, ty);
                if (y - ridgeY <= 1) {
                    col = mathutil::lerpColor(col, RGB(230, 240, 255), 0.35f);
                }
                ctx.mountainTex[y * 512 + x] = col;
            }
        }
    }
}

void projectRoad(const Track& track, const Camera& cam, RenderContext& ctx) {
    assert(track.count > 0);
    float zNorm = mathutil::wrap(cam.z, track.length) / cfg::SEG_LEN;
    ctx.baseSeg  = static_cast<int>(std::floor(zNorm)) % track.count;
    ctx.basePct  = zNorm - std::floor(zNorm);
    ctx.horizonY = cfg::HORIZON_BASE + static_cast<int>(std::lround(cam.pitchPx));
    ctx.shiftX   = cam.shiftX;

    for (int y = 0; y < cfg::WORLD_ROWS; ++y) {
        ctx.rows[y].seg = ROW_INVALID;
    }

    const Segment& seg0 = track.segs[ctx.baseSeg];
    float dx = -seg0.curve * ctx.basePct;
    float x  = -ctx.basePct * dx;
    const float centerX = (cfg::SCREEN_W * 0.5f) + cam.shiftX;
    const float zNear   = cfg::SEG_LEN;

    for (int n = 0; n <= cfg::DRAW_DIST; ++n) {
        const Segment& s_ = track.segs[(ctx.baseSeg + n) % track.count];
        float cz = std::max((static_cast<float>(n) - ctx.basePct) * cfg::SEG_LEN, zNear);
        float sc = cfg::FOCAL_PX / cz;
        float wx = x - cam.x;
        float wy = s_.y0 - cam.y;

        ctx.pts[n].cz = cz;
        ctx.pts[n].wx = wx;
        ctx.pts[n].wy = wy;
        ctx.pts[n].sx = centerX + sc * wx;
        ctx.pts[n].sy = static_cast<float>(ctx.horizonY) - sc * wy;
        ctx.pts[n].sw = sc * cfg::ROAD_HALF_W;

        x  += dx;
        dx += s_.curve;
    }

    int maxY = cfg::WORLD_ROWS;
    ctx.topRoadY = cfg::WORLD_ROWS;

    for (int n = 0; n < cfg::DRAW_DIST; ++n) {
        ctx.clipY[n] = static_cast<int16_t>(maxY);
        float yBot = ctx.pts[n].sy;
        float yTop = ctx.pts[n + 1].sy;
        if (yBot <= yTop) continue;                         // back-facing (underside of rise)
        if (yTop >= static_cast<float>(maxY)) continue;     // hidden behind nearer crest

        int rBeg = std::max(0, static_cast<int>(std::ceil(yTop)));
        int rEnd = std::min(maxY, static_cast<int>(std::ceil(yBot)));
        if (rBeg >= rEnd) continue;

        float inv = 1.0f / (yTop - yBot);                   // ONE division per segment
        float dX  = (ctx.pts[n + 1].sx - ctx.pts[n].sx) * inv;
        float dW  = (ctx.pts[n + 1].sw - ctx.pts[n].sw) * inv;
        float t0  = static_cast<float>(rBeg) - yBot;
        float rx  = ctx.pts[n].sx + dX * t0;
        float rw  = ctx.pts[n].sw + dW * t0;

        uint16_t absSeg = static_cast<uint16_t>((ctx.baseSeg + n) % track.count);
        uint8_t  fog    = ctx.fogLut[n];
        uint8_t  band   = static_cast<uint8_t>((absSeg / cfg::RUMBLE_LEN) & 1u);

        for (int y = rBeg; y < rEnd; ++y) {
            int16_t hwPx = static_cast<int16_t>(std::max(1.0f, std::floor(rw + 0.5f)));
            ctx.rows[y] = {
                static_cast<int16_t>(std::floor(rx + 0.5f)),
                hwPx,
                absSeg,
                fog,
                band
            };
            rx += dX;
            rw += dW;
        }
        maxY = rBeg;
        ctx.topRoadY = std::min(ctx.topRoadY, rBeg);
    }
}

void fillRows(const RenderContext& ctx, Framebuffer& fb, int yBegin, int yEnd) {
    yBegin = std::max(0, yBegin);
    yEnd   = std::min(cfg::WORLD_ROWS, yEnd);
    const int W = cfg::SCREEN_W;

    int bgOffset = static_cast<int>(std::floor(ctx.bgX)) & 511;
    if (bgOffset < 0) bgOffset += 512;

    for (int y = yBegin; y < yEnd; ++y) {
        uint32_t* row = fb.row(y);
        const RowInfo& r = ctx.rows[y];

        if (r.seg != ROW_INVALID) {
            const RoadPalette& P = ctx.pal[r.fog][r.band];
            int rw    = std::max(1, (static_cast<int>(r.hw) * RUMBLE_NUM) >> 8);
            int roadL = static_cast<int>(r.cx) - static_cast<int>(r.hw);
            int roadR = static_cast<int>(r.cx) + static_cast<int>(r.hw);
            int rumL  = roadL - rw;
            int rumR  = roadR + rw;

            fillSpan(row, 0,     rumL,  P.grass);
            fillSpan(row, rumL,  roadL, P.rumble);
            fillSpan(row, roadL, roadR, P.road);
            fillSpan(row, roadR, rumR,  P.rumble);
            fillSpan(row, rumR,  W,     P.grass);

            if (r.band == 0) {
                int lw = std::max(1, (static_cast<int>(r.hw) * LANE_NUM) >> 8);
                for (int k = 1; k < cfg::LANES; ++k) {
                    int lx = roadL + (2 * static_cast<int>(r.hw) * k) / cfg::LANES;
                    fillSpan(row, lx - lw / 2, lx - lw / 2 + lw, P.lane);
                }
            }
        } else if (y < ctx.horizonY) {
            int lutIdx = (y * 255) / std::max(ctx.horizonY, 1);
            lutIdx = mathutil::clamp(lutIdx, 0, 255);
            fillSpan(row, 0, W, ctx.skyLut[lutIdx]);

            int mtnY = y - (ctx.horizonY - 48);
            if (mtnY >= 0 && mtnY < 48) {
                const uint32_t* mtnRow = ctx.mountainTex + mtnY * 512;
                int firstRun = std::min(W, 512 - bgOffset);
                copyKeyedSpan(row, mtnRow + bgOffset, firstRun);
                if (firstRun < W) {
                    copyKeyedSpan(row + firstRun, mtnRow, W - firstRun);
                }
            }
        } else {
            fillSpan(row, 0, W, ctx.fogColor);
        }
    }
}
