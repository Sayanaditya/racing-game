#include "framebuffer.h"
#include "spanfill.h"
#include "mathutil.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// 5x7 ASCII bitmap font table (shared with high-res DisplayBuffer::drawText)
const uint8_t FONT_5X7_HI[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00}, {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40}
};

inline uint32_t blendAvg(uint32_t a, uint32_t b) {
    return 0xFF000000u | ((((a ^ b) & 0x00FEFEFEu) >> 1) + (a & b));
}

} // namespace

void Framebuffer::drawTestPattern(uint64_t tickCount) {
    const uint32_t greyA = RGB(48, 48, 54);
    const uint32_t greyB = RGB(76, 76, 84);
    const uint32_t red   = RGB(255, 0, 0);
    const uint32_t white = RGB(255, 255, 255);
    const int barX = static_cast<int>((tickCount * 2) % static_cast<uint64_t>(W));

    for (int y = 0; y < H; ++y) {
        uint32_t* r = row(y);
        int cellY = (y >> 4) & 1;
        for (int x = 0; x < W; ++x) {
            int cellX = (x >> 4) & 1;
            r[x] = ((cellX ^ cellY) == 0) ? greyA : greyB;
        }
        for (int dx = 0; dx < 4; ++dx) {
            int bx = barX + dx;
            if (bx >= 0 && bx < W) r[bx] = white;
        }
        if (y == 0 || y == H - 1) {
            for (int x = 0; x < W; ++x) r[x] = red;
        } else {
            r[0] = red;
            r[W - 1] = red;
        }
    }
}

void DisplayBuffer::upscaleFromBase(const Framebuffer& src, int pixelSize) {
    if (pixelSize >= 3) {
        // Fast 3x nearest-neighbour replication (each 320x240 pixel -> 3x3 block in 960x720)
        for (int y = 0; y < Framebuffer::H; ++y) {
            const uint32_t* sRow = src.row(y);
            uint32_t* d0 = row(y * 3);
            uint32_t* d1 = row(y * 3 + 1);
            uint32_t* d2 = row(y * 3 + 2);
            for (int x = 0; x < Framebuffer::W; ++x) {
                uint32_t c = sRow[x];
                int dx = x * 3;
                d0[dx] = d0[dx + 1] = d0[dx + 2] = c;
                d1[dx] = d1[dx + 1] = d1[dx + 2] = c;
                d2[dx] = d2[dx + 1] = d2[dx + 2] = c;
            }
        }
        return;
    }

    // Scale3x / EPX edge-preserving 3x upscaler with diagonal anti-aliasing for 1px (Ultra) and 2px (High) modes
    for (int y = 0; y < Framebuffer::H; ++y) {
        const uint32_t* rUp  = src.row(std::max(0, y - 1));
        const uint32_t* rMid = src.row(y);
        const uint32_t* rDn  = src.row(std::min(Framebuffer::H - 1, y + 1));

        uint32_t* d0 = row(y * 3);
        uint32_t* d1 = row(y * 3 + 1);
        uint32_t* d2 = row(y * 3 + 2);

        for (int x = 0; x < Framebuffer::W; ++x) {
            int xm = std::max(0, x - 1);
            int xp = std::min(Framebuffer::W - 1, x + 1);

            uint32_t B = rUp[x];
            uint32_t D = rMid[xm];
            uint32_t E = rMid[x];
            uint32_t F = rMid[xp];
            uint32_t H = rDn[x];

            uint32_t E0 = E, E1 = E, E2 = E;
            uint32_t E3 = E, E4 = E, E5 = E;
            uint32_t E6 = E, E7 = E, E8 = E;

            if (B != H && D != F) {
                if (D == B) E0 = D;
                else        E0 = blendAvg(E, blendAvg(D, B));

                if (B == F) E2 = F;
                else        E2 = blendAvg(E, blendAvg(B, F));

                if (D == H) E6 = D;
                else        E6 = blendAvg(E, blendAvg(D, H));

                if (H == F) E8 = F;
                else        E8 = blendAvg(E, blendAvg(H, F));

                if ((D == B && E != rUp[xp]) || (B == F && E != rUp[xm])) E1 = B;
                if ((D == B && E != rDn[xm]) || (D == H && E != rUp[xm])) E3 = D;
                if ((B == F && E != rDn[xp]) || (H == F && E != rUp[xp])) E5 = F;
                if ((D == H && E != rDn[xp]) || (H == F && E != rDn[xm])) E7 = H;
            }

            int dx = x * 3;
            d0[dx] = E0; d0[dx + 1] = E1; d0[dx + 2] = E2;
            d1[dx] = E3; d1[dx + 1] = E4; d1[dx + 2] = E5;
            d2[dx] = E6; d2[dx + 1] = E7; d2[dx + 2] = E8;
        }
    }
}

void DisplayBuffer::applyPostProcess(int pixelSize, bool crtScanlines) {
    // Quantize 960x720 buffer into blocks of pixelSize x pixelSize when pixelSize > 1
    if (pixelSize > 1) {
        int step = std::clamp(pixelSize, 2, 12);
        for (int y = 0; y < H; y += step) {
            int yEnd = std::min(H, y + step);
            const uint32_t* srcRow = row(y);
            for (int x = 0; x < W; x += step) {
                uint32_t c = srcRow[x];
                int xEnd = std::min(W, x + step);
                for (int by = y; by < yEnd; ++by) {
                    uint32_t* dstRow = row(by);
                    for (int bx = x; bx < xEnd; ++bx) {
                        dstRow[bx] = c;
                    }
                }
            }
        }
    }

    if (crtScanlines) {
        for (int y = 1; y < H; y += 3) {
            uint32_t* r = row(y);
            for (int x = 0; x < W; ++x) {
                uint32_t c = r[x];
                r[x] = 0xFF000000u | (c - ((c >> 2) & 0x003F3F3Fu)); // 75% scanline dimming
            }
        }
    }
}

void DisplayBuffer::fillSpan(int y, int x0, int x1, uint32_t color) {
    if (y < 0 || y >= H) return;
    x0 = std::max(0, x0);
    x1 = std::min(W, x1);
    if (x0 < x1) {
        uint32_t* r = row(y);
        std::fill(r + x0, r + x1, color);
    }
}

void DisplayBuffer::drawRect(int x, int y, int w, int h, uint32_t color) {
    int y0 = std::max(0, y);
    int y1 = std::min(H, y + h);
    int x0 = std::max(0, x);
    int x1 = std::min(W, x + w);
    if (x0 >= x1 || y0 >= y1) return;
    for (int ry = y0; ry < y1; ++ry) {
        uint32_t* r = row(ry);
        std::fill(r + x0, r + x1, color);
    }
}

void DisplayBuffer::drawRectAlpha(int x, int y, int w, int h, uint32_t color, uint8_t alpha) {
    int y0 = std::max(0, y);
    int y1 = std::min(H, y + h);
    int x0 = std::max(0, x);
    int x1 = std::min(W, x + w);
    if (x0 >= x1 || y0 >= y1) return;

    uint32_t a   = alpha;
    uint32_t inv = 256u - a;
    uint32_t sRB = (color & 0x00FF00FFu) * a;
    uint32_t sG  = (color & 0x0000FF00u) * a;

    for (int ry = y0; ry < y1; ++ry) {
        uint32_t* r = row(ry);
        for (int rx = x0; rx < x1; ++rx) {
            uint32_t d = r[rx];
            uint32_t rb = ((d & 0x00FF00FFu) * inv + sRB) >> 8;
            uint32_t g  = ((d & 0x0000FF00u) * inv + sG)  >> 8;
            r[rx] = 0xFF000000u | (rb & 0x00FF00FFu) | (g & 0x0000FF00u);
        }
    }
}

void DisplayBuffer::drawBorder(int x, int y, int w, int h, int t, uint32_t color) {
    drawRect(x, y, w, t, color);
    drawRect(x, y + h - t, w, t, color);
    drawRect(x, y, t, h, color);
    drawRect(x + w - t, y, t, h, color);
}

void DisplayBuffer::drawLine(int x0, int y0, int x1, int y1, int halfW, uint32_t color) {
    int dx = std::abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    while (true) {
        for (int wy = -halfW; wy <= halfW; ++wy) {
            fillSpan(y0 + wy, x0 - halfW, x0 + halfW + 1, color);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void DisplayBuffer::drawCircle(int cx, int cy, int r, uint32_t color, bool filled) {
    if (r <= 0) return;
    int r2 = r * r;
    int inner2 = (r - 2) * (r - 2);
    for (int dy = -r; dy <= r; ++dy) {
        int y = cy + dy;
        if (y < 0 || y >= H) continue;
        int maxDx = static_cast<int>(std::sqrt(static_cast<float>(r2 - dy * dy)));
        if (filled) {
            fillSpan(y, cx - maxDx, cx + maxDx + 1, color);
        } else {
            for (int dx = -maxDx; dx <= maxDx; ++dx) {
                int d2 = dx * dx + dy * dy;
                if (d2 >= inner2) {
                    int x = cx + dx;
                    if (x >= 0 && x < W) row(y)[x] = color;
                }
            }
        }
    }
}

int DisplayBuffer::textWidth(const char* text, int scale) {
    if (!text || scale < 1) return 0;
    return static_cast<int>(std::strlen(text)) * 6 * scale - scale;
}

void DisplayBuffer::drawText(int x, int y, const char* text, uint32_t color, int scale, bool shadow) {
    if (!text || scale < 1) return;
    auto renderPass = [&](int ox, int oy, uint32_t col) {
        int curX = x + ox;
        for (const char* p = text; *p != '\0'; ++p) {
            char ch = *p;
            if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
            if (ch < 32 || ch > 95) ch = ' ';
            const uint8_t* glyph = FONT_5X7_HI[ch - 32];
            for (int colIdx = 0; colIdx < 5; ++colIdx) {
                uint8_t bits = glyph[colIdx];
                for (int rowIdx = 0; rowIdx < 7; ++rowIdx) {
                    if ((bits >> rowIdx) & 1u) {
                        int py = y + oy + rowIdx * scale;
                        int px = curX + colIdx * scale;
                        for (int dy = 0; dy < scale; ++dy) {
                            fillSpan(py + dy, px, px + scale, col);
                        }
                    }
                }
            }
            curX += 6 * scale;
        }
    };
    if (shadow) {
        renderPass(std::max(1, scale / 2), std::max(1, scale / 2), RGB(10, 12, 18));
    }
    renderPass(0, 0, color);
}

float FrameStats::avg() const {
    int n = count > 0 ? count : N;
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) sum += ms[i];
    return sum / static_cast<float>(n);
}

float FrameStats::p99() const {
    float tmp[N];
    for (int i = 0; i < N; ++i) tmp[i] = ms[i];
    std::nth_element(tmp, tmp + 126, tmp + N);
    return tmp[126];
}
