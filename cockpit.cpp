#include "cockpit.h"
#include "spanfill.h"
#include "sprites.h"
#include "mathutil.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace {

// Compact 5x7 ASCII bitmap font (columns encoded as 7-bit masks, LSB = top row)
const uint8_t FONT_5X7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x00,0x00,0x5F,0x00,0x00}, // '!'
    {0x00,0x07,0x00,0x07,0x00}, // '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // '$'
    {0x23,0x13,0x08,0x64,0x62}, // '%'
    {0x36,0x49,0x55,0x22,0x50}, // '&'
    {0x00,0x05,0x03,0x00,0x00}, // '''
    {0x00,0x1C,0x22,0x41,0x00}, // '('
    {0x00,0x41,0x22,0x1C,0x00}, // ')'
    {0x14,0x08,0x3E,0x08,0x14}, // '*'
    {0x08,0x08,0x3E,0x08,0x08}, // '+'
    {0x00,0x50,0x30,0x00,0x00}, // ','
    {0x08,0x08,0x08,0x08,0x08}, // '-'
    {0x00,0x60,0x60,0x00,0x00}, // '.'
    {0x20,0x10,0x08,0x04,0x02}, // '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // '0'
    {0x00,0x42,0x7F,0x40,0x00}, // '1'
    {0x42,0x61,0x51,0x49,0x46}, // '2'
    {0x21,0x41,0x45,0x4B,0x31}, // '3'
    {0x18,0x14,0x12,0x7F,0x10}, // '4'
    {0x27,0x45,0x45,0x45,0x39}, // '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // '6'
    {0x01,0x71,0x09,0x05,0x03}, // '7'
    {0x36,0x49,0x49,0x49,0x36}, // '8'
    {0x06,0x49,0x49,0x29,0x1E}, // '9'
    {0x00,0x36,0x36,0x00,0x00}, // ':'
    {0x00,0x56,0x36,0x00,0x00}, // ';'
    {0x08,0x14,0x22,0x41,0x00}, // '<'
    {0x14,0x14,0x14,0x14,0x14}, // '='
    {0x00,0x41,0x22,0x14,0x08}, // '>'
    {0x02,0x01,0x51,0x09,0x06}, // '?'
    {0x32,0x49,0x79,0x41,0x3E}, // '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 'E'
    {0x7F,0x09,0x09,0x09,0x01}, // 'F'
    {0x3E,0x41,0x49,0x49,0x7A}, // 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 'L'
    {0x7F,0x02,0x0C,0x02,0x7F}, // 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 'V'
    {0x3F,0x40,0x38,0x40,0x3F}, // 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 'X'
    {0x07,0x08,0x70,0x08,0x07}, // 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 'Z'
    {0x00,0x7F,0x41,0x41,0x00}, // '['
    {0x02,0x04,0x08,0x10,0x20}, // '\'
    {0x00,0x41,0x41,0x7F,0x00}, // ']'
    {0x04,0x02,0x01,0x02,0x04}, // '^'
    {0x40,0x40,0x40,0x40,0x40}, // '_'
};

void buildDialDecal(uint32_t* buf, bool isTach) {
    std::fill(buf, buf + 56 * 56, 0x00000000u);
    const int cx = 28, cy = 28, rOuter = 26, rInner = 24;
    for (int y = 0; y < 56; ++y) {
        for (int x = 0; x < 56; ++x) {
            int dx = x - cx, dy = y - cy;
            int d2 = dx * dx + dy * dy;
            if (d2 <= rOuter * rOuter) {
                if (d2 >= rInner * rInner) {
                    buf[y * 56 + x] = (dy < 0) ? RGB(150, 158, 172) : RGB(80, 86, 98);
                } else {
                    buf[y * 56 + x] = RGB(16, 18, 24);
                }
            }
        }
    }
    // Tick marks from 225 deg down to -45 deg
    const int numTicks = isTach ? 10 : 13;
    for (int i = 0; i < numTicks; ++i) {
        float frac = static_cast<float>(i) / static_cast<float>(numTicks - 1);
        float deg  = 225.0f - 270.0f * frac;
        float rad  = deg * mathutil::PI / 180.0f;
        uint32_t col = (isTach && frac >= 0.80f) ? RGB(245, 45, 45) : RGB(220, 228, 240);
        for (int r = 19; r <= 23; ++r) {
            int tx = cx + static_cast<int>(std::lround(static_cast<float>(r) * std::cos(rad)));
            int ty = cy - static_cast<int>(std::lround(static_cast<float>(r) * std::sin(rad)));
            if (tx >= 0 && tx < 56 && ty >= 0 && ty < 56) {
                buf[ty * 56 + tx] = col;
            }
        }
    }
    // Redline arc on tachometer (7500..9000 RPM => frac 0.833..1.0)
    if (isTach) {
        for (int s = 0; s <= 24; ++s) {
            float frac = 0.833f + (1.0f - 0.833f) * (static_cast<float>(s) / 24.0f);
            float rad  = (225.0f - 270.0f * frac) * mathutil::PI / 180.0f;
            for (int r = 21; r <= 23; ++r) {
                int tx = cx + static_cast<int>(std::lround(static_cast<float>(r) * std::cos(rad)));
                int ty = cy - static_cast<int>(std::lround(static_cast<float>(r) * std::sin(rad)));
                if (tx >= 0 && tx < 56 && ty >= 0 && ty < 56) {
                    buf[ty * 56 + tx] = RGB(235, 35, 35);
                }
            }
        }
    }
}

void drawSevenSegDigit(Framebuffer& fb, int x0, int y0, int digit, uint32_t colOn, uint32_t colOff) {
    // 8x14 px digit
    // Segments: 0=top, 1=top-left, 2=top-right, 3=mid, 4=bot-left, 5=bot-right, 6=bot
    static const uint8_t MASKS[10] = {
        0b1110111, // 0
        0b0100100, // 1
        0b1011101, // 2
        0b1101101, // 3
        0b0101110, // 4
        0b1101011, // 5
        0b1111011, // 6
        0b0100101, // 7
        0b1111111, // 8
        0b1101111  // 9
    };
    uint8_t m = (digit >= 0 && digit <= 9) ? MASKS[digit] : 0;
    auto hSeg = [&](int y, uint8_t bit) {
        uint32_t c = (m & bit) ? colOn : colOff;
        if (y >= 0 && y + 1 < cfg::SCREEN_H) {
            fillSpan(fb.row(y),     x0 + 1, x0 + 7, c);
            fillSpan(fb.row(y + 1), x0 + 1, x0 + 7, c);
        }
    };
    auto vSeg = [&](int x, int ya, int yb, uint8_t bit) {
        uint32_t c = (m & bit) ? colOn : colOff;
        for (int y = ya; y < yb; ++y) {
            if (y >= 0 && y < cfg::SCREEN_H) {
                fillSpan(fb.row(y), x, x + 2, c);
            }
        }
    };
    hSeg(y0,      1 << 0);
    vSeg(x0,      y0 + 1, y0 + 7,  1 << 1);
    vSeg(x0 + 6,  y0 + 1, y0 + 7,  1 << 2);
    hSeg(y0 + 6,  1 << 3);
    vSeg(x0,      y0 + 7, y0 + 13, 1 << 4);
    vSeg(x0 + 6,  y0 + 7, y0 + 13, 1 << 5);
    hSeg(y0 + 12, 1 << 6);
}

} // namespace

void CameraFx::impact(float ampPx) {
    impactAmp = std::max(impactAmp, ampPx);
    impactT   = 0.0f;
}

void CameraFx::update(float speedFrac, float rpmFrac, float accelFrac, float offroad) {
    phase   += cfg::STEP;
    impactT += cfg::STEP;

    const float kPitch = 1.0f - std::exp(-cfg::STEP / 0.10f);
    float targetPitch  = 3.0f * mathutil::clamp(accelFrac, -1.0f, 1.0f);
    accelPitch += (targetPitch - accelPitch) * kPitch;

    float decay = (impactAmp > 0.0f) ? (impactAmp * std::exp(-impactT / 0.4f)) : 0.0f;
    if (decay < 0.01f && impactT > 0.4f) {
        impactAmp = 0.0f;
        decay     = 0.0f;
    }

    shakeX = decay * std::sin(47.0f * impactT + 0.7f)
           + 0.3f * rpmFrac * std::sin(mathutil::TWO_PI * 24.0f * phase);

    shakeY = decay * std::sin(61.0f * impactT + 1.9f)
           + 0.4f * speedFrac * std::sin(mathutil::TWO_PI * 31.0f * phase)
           + accelPitch
           + (offroad > 0.0f ? (1.2f * speedFrac * offroad * std::sin(mathutil::TWO_PI * 37.0f * phase)) : 0.0f);
}

void Cockpit::init() {
    dispSpeed_ = 0.0f;
    dispRpm_   = 900.0f;
    dispSteer_ = 0.0f;
    ox_ = 0;
    oy_ = 0;
    cracks_.count = 0;
    crackProgress_ = 0.0f;
    st_ = CockpitState{};

    const int W = cfg::SCREEN_W;
    const uint32_t edgeHighlight = RGB(96, 104, 118);

    auto roofColor = [](int y) -> uint32_t {
        int v = 22 + y;
        return RGB(v, v + 2, v + 6);
    };
    auto pillarColor = [](int y) -> uint32_t {
        int v = 30 + (y - 14) / 8;
        return RGB(v, v + 2, v + 6);
    };
    auto dashColor = [](int y) -> uint32_t {
        int v = (y < 192) ? (56 - (y - 176)) : (36 - (y - 192) / 3);
        v = std::max(18, v);
        return RGB(v, v + 3, v + 8);
    };

    for (int y = 0; y < cfg::SCREEN_H; ++y) {
        RowRuns& rr = rows_[y];
        rr.n = 0;
        auto addRun = [&](int x0, int x1, uint32_t col) {
            if (x0 < x1 && rr.n < 6) {
                rr.r[rr.n++] = { static_cast<int16_t>(x0), static_cast<int16_t>(x1), col };
            }
        };

        if (y < 14) {
            addRun(-8, W + 8, roofColor(y));
        } else if (y < 192) {
            float t = mathutil::clamp(static_cast<float>(y - 14) / static_cast<float>(176 - 14), 0.0f, 1.0f);
            int xl = static_cast<int>(std::lround(mathutil::lerp(36.0f, 20.0f, t)));
            int xr = static_cast<int>(std::lround(mathutil::lerp(284.0f, 300.0f, t)));
            addRun(-8, xl - 1, pillarColor(y));
            addRun(xl - 1, xl, edgeHighlight);
            addRun(xr, xr + 1, edgeHighlight);
            addRun(xr + 1, W + 8, pillarColor(y));

            if (y >= 176) {
                float g = 160.0f * std::sqrt(static_cast<float>(y - 176) / 16.0f);
                int dl = std::max(static_cast<int>(std::lround(160.0f - g)), xl);
                int dr = std::min(static_cast<int>(std::lround(160.0f + g)), xr);
                if (dl < dr) {
                    addRun(dl, dr, dashColor(y));
                }
            }
        } else {
            addRun(-8, W + 8, dashColor(y));
        }
    }

    buildDialDecal(tachDecal_, true);
    buildDialDecal(speedDecal_, false);

    // Console decal (80x40 anchored around centre dashboard)
    std::fill(consoleDecal_, consoleDecal_ + 80 * 40, 0x00000000u);
    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 80; ++x) {
            bool border = (x == 0 || x == 79 || y == 0 || y == 39);
            consoleDecal_[y * 80 + x] = border ? RGB(74, 80, 92) : RGB(20, 24, 30);
        }
    }
}

void Cockpit::update(const CockpitState& s, const CameraFx& fx) {
    st_ = s;
    dispSpeed_ += (s.speedKmh - dispSpeed_) * 0.25f;
    dispRpm_   += (s.rpm      - dispRpm_)   * 0.35f;
    dispSteer_ += (s.steer    - dispSteer_) * 0.20f;
    crackProgress_ = mathutil::clamp(s.crack, 0.0f, 1.0f);

    ox_ = mathutil::clamp(static_cast<int>(std::lround(0.3f * fx.shakeX)), -4, 4);
    oy_ = mathutil::clamp(static_cast<int>(std::lround(0.3f * fx.shakeY)), -4, 4);
}

void Cockpit::tintRows(Framebuffer& fb, int yBegin, int yEnd) {
    int r0 = std::max(14, yBegin);
    int r1 = std::min(38, yEnd);
    for (int y = r0; y < r1; ++y) {
        int mode = (y <= 25) ? 1 : 2;
        tintSpanSIMD(fb.row(y), 0, cfg::SCREEN_W, mode);
    }
}

void Cockpit::tint(Framebuffer& fb) {
    tintRows(fb, 14, 38);
}

void Cockpit::clearCracks() {
    cracks_.count = 0;
    crackProgress_ = 0.0f;
}

void Cockpit::triggerCrack(int impactX, int impactY, uint32_t seed) {
    uint32_t rng = seed ? seed : 0x9E3779B9u;
    cracks_.count = 0;
    crackProgress_ = std::max(crackProgress_, 1.0f / 12.0f);

    auto addSeg = [&](int x0, int y0, int x1, int y1) {
        if (cracks_.count < 160) {
            cracks_.s[cracks_.count++] = {
                static_cast<int16_t>(x0), static_cast<int16_t>(y0),
                static_cast<int16_t>(x1), static_cast<int16_t>(y1)
            };
        }
    };

    const int N = 10 + mathutil::randRange(rng, 5);
    int ringPtsX[3][16];
    int ringPtsY[3][16];
    int ringCount[3] = { 0, 0, 0 };
    const float ringRadii[3] = { 22.0f, 48.0f, 80.0f };

    for (int k = 0; k < N && cracks_.count < 160; ++k) {
        float baseTheta = mathutil::TWO_PI * static_cast<float>(k) / static_cast<float>(N)
                        + (mathutil::rand01(rng) * 0.50f - 0.25f);
        float len   = 40.0f + static_cast<float>(mathutil::randRange(rng, 120));
        float walked = 0.0f;
        float cx = static_cast<float>(impactX);
        float cy = static_cast<float>(impactY);
        float dir = baseTheta;
        bool recordedRing[3] = { false, false, false };

        while (walked < len && cracks_.count < 160) {
            float step = 10.0f + static_cast<float>(mathutil::randRange(rng, 15));
            dir += (mathutil::rand01(rng) * 0.70f - 0.35f);
            float nx = cx + step * std::cos(dir);
            float ny = cy + step * std::sin(dir);
            addSeg(static_cast<int>(std::lround(cx)), static_cast<int>(std::lround(cy)),
                   static_cast<int>(std::lround(nx)), static_cast<int>(std::lround(ny)));
            walked += step;

            for (int rIdx = 0; rIdx < 3; ++rIdx) {
                if (!recordedRing[rIdx] && walked >= ringRadii[rIdx] && ringCount[rIdx] < 16) {
                    recordedRing[rIdx] = true;
                    ringPtsX[rIdx][ringCount[rIdx]] = static_cast<int>(std::lround(nx));
                    ringPtsY[rIdx][ringCount[rIdx]] = static_cast<int>(std::lround(ny));
                    ringCount[rIdx]++;
                }
            }

            // Spawn child ray with probability 0.3
            if (mathutil::rand01(rng) < 0.30f && cracks_.count < 160) {
                float cDir = dir + ((mathutil::xorshift32(rng) & 1u) ? +0.85f : -0.85f);
                float cLen = (len - walked) * 0.40f;
                float bx = nx, by = ny, bWalk = 0.0f;
                while (bWalk < cLen && cracks_.count < 160) {
                    float bStep = 10.0f + static_cast<float>(mathutil::randRange(rng, 12));
                    float bnx = bx + bStep * std::cos(cDir);
                    float bny = by + bStep * std::sin(cDir);
                    addSeg(static_cast<int>(std::lround(bx)), static_cast<int>(std::lround(by)),
                           static_cast<int>(std::lround(bnx)), static_cast<int>(std::lround(bny)));
                    bx = bnx;
                    by = bny;
                    bWalk += bStep;
                }
            }
            cx = nx;
            cy = ny;
        }
    }

    // Concentric rings at radii 22, 48, 80
    for (int rIdx = 0; rIdx < 3 && cracks_.count < 160; ++rIdx) {
        int m = ringCount[rIdx];
        for (int i = 0; i < m && cracks_.count < 160; ++i) {
            int j = (i + 1) % m;
            addSeg(ringPtsX[rIdx][i], ringPtsY[rIdx][i], ringPtsX[rIdx][j], ringPtsY[rIdx][j]);
        }
    }
}

void Cockpit::thickLine(Framebuffer& fb, int x0, int y0, int x1, int y1, int halfW, uint32_t color,
                        int minY, int maxY) {
    int dx = std::abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    while (true) {
        if (y0 >= minY && y0 < maxY) {
            fillSpan(fb.row(y0), x0 - halfW, x0 + halfW + 1, color);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void Cockpit::drawCracks(Framebuffer& fb) {
    if (cracks_.count <= 0 || crackProgress_ <= 0.0f) return;
    int nDraw = std::min(cracks_.count, static_cast<int>(std::ceil(cracks_.count * crackProgress_)));
    const uint32_t colLight = RGB(230, 235, 240);
    const uint32_t colDark  = RGB(60, 60, 70);

    for (int i = 0; i < nDraw; ++i) {
        const CrackSeg& s = cracks_.s[i];
        thickLine(fb, s.x0 + ox_ + 1, s.y0 + oy_ + 1, s.x1 + ox_ + 1, s.y1 + oy_ + 1, 0, colDark,  14, 192);
        thickLine(fb, s.x0 + ox_,     s.y0 + oy_,     s.x1 + ox_,     s.y1 + oy_,     0, colLight, 14, 192);
    }
}

void Cockpit::drawFrame(Framebuffer& fb) {
    // 1. Impact white flash on windshield rows before frame masks edges
    if (st_.flash > 0.15f) {
        for (int y = 14; y < 192; ++y) {
            uint32_t* r = fb.row(y);
            if (st_.flash > 0.5f) {
                for (int x = 0; x < cfg::SCREEN_W; ++x) {
                    uint32_t c = r[x] & 0x00FFFFFFu;
                    r[x] = 0xFF000000u | (((c >> 1) & 0x007F7F7Fu) + 0x00808080u);
                }
            } else {
                for (int x = 0; x < cfg::SCREEN_W; ++x) {
                    uint32_t c = r[x] & 0x00FFFFFFu;
                    r[x] = 0xFF000000u | (c + (((0x00FFFFFFu - c) >> 2) & 0x003F3F3Fu));
                }
            }
        }
    }

    // 2. Procedural cockpit runs with interior shake offset (ox_, oy_)
    for (int y = 0; y < cfg::SCREEN_H; ++y) {
        int srcRow = mathutil::clamp(y - oy_, 0, cfg::SCREEN_H - 1);
        const RowRuns& rr = rows_[srcRow];
        uint32_t* dst = fb.row(y);
        for (int i = 0; i < rr.n; ++i) {
            fillSpan(dst, rr.r[i].x0 + ox_, rr.r[i].x1 + ox_, rr.r[i].color);
        }
    }

    // 3. Keyed decals (tachometer, speedometer, centre console)
    blitUnscaledKeyed(tachDecal_,    56, 56, fb, 70  - 28 + ox_, 214 - 28 + oy_);
    blitUnscaledKeyed(speedDecal_,   56, 56, fb, 250 - 28 + ox_, 214 - 28 + oy_);
    blitUnscaledKeyed(consoleDecal_, 80, 40, fb, 160 - 40 + ox_, 196 + oy_);
}

void Cockpit::drawGauges(Framebuffer& fb) {
    // 1. Tachometer needle (centre 70, 214, r = 22, v_max = 9000)
    {
        float rpmNorm = mathutil::clamp(dispRpm_ / 9000.0f, 0.0f, 1.0f);
        float rad = (225.0f - 270.0f * rpmNorm) * mathutil::PI / 180.0f;
        int cx = 70 + ox_, cy = 214 + oy_;
        int tx = cx + static_cast<int>(std::lround(22.0f * std::cos(rad)));
        int ty = cy - static_cast<int>(std::lround(22.0f * std::sin(rad)));
        thickLine(fb, cx, cy, tx, ty, 0, RGB(255, 70, 50));
        drawRect(fb, cx - 1, cy - 1, 3, 3, RGB(220, 225, 235));
    }

    // 2. Speedometer needle (centre 250, 214, r = 22, v_max = 300 km/h)
    {
        float spdNorm = mathutil::clamp(dispSpeed_ / 300.0f, 0.0f, 1.0f);
        float rad = (225.0f - 270.0f * spdNorm) * mathutil::PI / 180.0f;
        int cx = 250 + ox_, cy = 214 + oy_;
        int tx = cx + static_cast<int>(std::lround(22.0f * std::cos(rad)));
        int ty = cy - static_cast<int>(std::lround(22.0f * std::sin(rad)));
        thickLine(fb, cx, cy, tx, ty, 0, RGB(255, 210, 60));
        drawRect(fb, cx - 1, cy - 1, 3, 3, RGB(220, 225, 235));
    }

    // 3. Digital speed readout: 3 seven-segment digits at (141, 206), each 8x14 px
    {
        int spdInt = mathutil::clamp(static_cast<int>(std::lround(dispSpeed_)), 0, 999);
        int d0 = (spdInt / 100) % 10;
        int d1 = (spdInt / 10)  % 10;
        int d2 = spdInt % 10;
        uint32_t colOn  = (st_.damage > 0.6f) ? RGB(255, 85, 45) : RGB(0, 245, 180);
        uint32_t colOff = RGB(14, 34, 32);
        drawSevenSegDigit(fb, 141 + ox_,      206 + oy_, d0, colOn, colOff);
        drawSevenSegDigit(fb, 141 + 10 + ox_, 206 + oy_, d1, colOn, colOff);
        drawSevenSegDigit(fb, 141 + 20 + ox_, 206 + oy_, d2, colOn, colOff);
    }

    // 4. Tilting Steering Wheel (centre 160, 232, rx = 42, ry = 32, 48 rim segments + 3 spokes)
    {
        int cx = 160 + ox_;
        int cy = 232 + oy_;
        const float rx = 42.0f, ry = 32.0f;
        float a = dispSteer_ * (100.0f * mathutil::PI / 180.0f);

        int px[48], py[48];
        for (int k = 0; k < 48; ++k) {
            float theta = mathutil::TWO_PI * static_cast<float>(k) / 48.0f + a;
            px[k] = cx + static_cast<int>(std::lround(rx * std::cos(theta)));
            py[k] = cy + static_cast<int>(std::lround(ry * std::sin(theta)));
        }

        // Three spokes at angles a + {180, 0, 270} deg (i.e. +pi, 0, +1.5*pi)
        const float spokeAngles[3] = { a + mathutil::PI, a, a + 0.5f * mathutil::PI };
        for (int s = 0; s < 3; ++s) {
            int sx = cx + static_cast<int>(std::lround((rx - 3.0f) * std::cos(spokeAngles[s])));
            int sy = cy + static_cast<int>(std::lround((ry - 3.0f) * std::sin(spokeAngles[s])));
            thickLine(fb, cx, cy, sx, sy, 2, RGB(58, 64, 74));
        }

        // Outer rim (48 segments) + top-centre racing stripe at k = 36 (-90 deg)
        for (int k = 0; k < 48; ++k) {
            int kn = (k + 1) % 48;
            uint32_t col = (k == 35 || k == 36) ? RGB(235, 45, 45) : RGB(24, 28, 34);
            thickLine(fb, px[k], py[k], px[kn], py[kn], 2, col);
        }

        // Steering wheel hub
        drawRect(fb, cx - 7, cy - 5, 15, 10, RGB(38, 42, 50));
        drawRect(fb, cx - 2, cy - 2, 5,  4,  RGB(210, 40, 40));
    }
}

void Cockpit::drawRect(Framebuffer& fb, int x, int y, int w, int h, uint32_t color) {
    int y0 = std::max(0, y);
    int y1 = std::min(cfg::SCREEN_H, y + h);
    for (int ry = y0; ry < y1; ++ry) {
        fillSpan(fb.row(ry), x, x + w, color);
    }
}

void Cockpit::drawText(Framebuffer& fb, int x, int y, const char* text, uint32_t color, int scale) {
    if (!text || scale < 1) return;
    int curX = x;
    for (const char* p = text; *p != '\0'; ++p) {
        char ch = *p;
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        if (ch < 32 || ch > 95) ch = ' ';
        const uint8_t* glyph = FONT_5X7[ch - 32];
        for (int col = 0; col < 5; ++col) {
            uint8_t bits = glyph[col];
            for (int row = 0; row < 7; ++row) {
                if ((bits >> row) & 1u) {
                    int py = y + row * scale;
                    int px = curX + col * scale;
                    for (int dy = 0; dy < scale; ++dy) {
                        if (py + dy >= 0 && py + dy < cfg::SCREEN_H) {
                            fillSpan(fb.row(py + dy), px, px + scale, color);
                        }
                    }
                }
            }
        }
        curX += 6 * scale;
    }
}
