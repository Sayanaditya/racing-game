#include "raster3d.h"
#include "spanfill.h"
#include "mathutil.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// Local-to-Camera 3D transform helper (rotates local model point by yaw & bank, then translates to baseCamPos)
inline Vec3 transformLocalToCam(const Vec3& localPt, const Vec3& baseCamPos,
                                float cosYaw, float sinYaw, float cosBank, float sinBank) {
    // 1. Roll / bank around Z axis
    float bx = localPt.x * cosBank - localPt.y * sinBank;
    float by = localPt.x * sinBank + localPt.y * cosBank;
    float bz = localPt.z;
    // 2. Yaw around Y axis
    float rx = bx * cosYaw + bz * sinYaw;
    float ry = by;
    float rz = -bx * sinYaw + bz * cosYaw;
    return { baseCamPos.x + rx, baseCamPos.y + ry, baseCamPos.z + rz };
}

inline uint32_t scaleColorBrightness(uint32_t c, float factor) {
    factor = mathutil::clamp(factor, 0.0f, 2.2f);
    uint32_t r = static_cast<uint32_t>(mathutil::clamp(((c >> 16) & 0xFFu) * factor, 0.0f, 255.0f));
    uint32_t g = static_cast<uint32_t>(mathutil::clamp(((c >> 8)  & 0xFFu) * factor, 0.0f, 255.0f));
    uint32_t b = static_cast<uint32_t>(mathutil::clamp(( c        & 0xFFu) * factor, 0.0f, 255.0f));
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// Emits a 3D box (axis-aligned in local model space, transformed by yaw & bank into camera space)
void drawBoxMesh3D(RasterPipeline& pipe, const Vec3& baseCamPos,
                   float cosYaw, float sinYaw, float cosBank, float sinBank,
                   float x0, float y0, float z0, float x1, float y1, float z1,
                   uint32_t colTop, uint32_t colSide, uint32_t colRear, uint32_t colFront,
                   float specStrength = 0.20f) {
    Vec3 p000 = transformLocalToCam({ x0, y0, z0 }, baseCamPos, cosYaw, sinYaw, cosBank, sinBank);
    Vec3 p100 = transformLocalToCam({ x1, y0, z0 }, baseCamPos, cosYaw, sinYaw, cosBank, sinBank);
    Vec3 p110 = transformLocalToCam({ x1, y1, z0 }, baseCamPos, cosYaw, sinYaw, cosBank, sinBank);
    Vec3 p010 = transformLocalToCam({ x0, y1, z0 }, baseCamPos, cosYaw, sinYaw, cosBank, sinBank);

    Vec3 p001 = transformLocalToCam({ x0, y0, z1 }, baseCamPos, cosYaw, sinYaw, cosBank, sinBank);
    Vec3 p101 = transformLocalToCam({ x1, y0, z1 }, baseCamPos, cosYaw, sinYaw, cosBank, sinBank);
    Vec3 p111 = transformLocalToCam({ x1, y1, z1 }, baseCamPos, cosYaw, sinYaw, cosBank, sinBank);
    Vec3 p011 = transformLocalToCam({ x0, y1, z1 }, baseCamPos, cosYaw, sinYaw, cosBank, sinBank);

    // Rear face (z = z0, facing camera when car is ahead)
    DrawQuad3D(pipe, p000, p100, p110, p010, colRear, true, specStrength);
    // Top face (y = y1)
    DrawQuad3D(pipe, p010, p110, p111, p011, colTop, true, specStrength);
    // Left face (x = x0)
    DrawQuad3D(pipe, p001, p000, p010, p011, colSide, true, specStrength);
    // Right face (x = x1)
    DrawQuad3D(pipe, p100, p101, p111, p110, colSide, true, specStrength);
    // Front face (z = z1)
    DrawQuad3D(pipe, p101, p001, p011, p111, colFront, true, specStrength);
}

} // namespace

void RasterPipeline::bind(uint32_t* targetPixels, int w, int h, float horizonY, float shiftXPx) {
    colorBuf = targetPixels;
    width    = std::clamp(w, 1, MAX_W);
    height   = std::clamp(h, 1, MAX_H);
    float scaleX = static_cast<float>(width)  / static_cast<float>(cfg::SCREEN_W);
    float scaleY = static_cast<float>(height) / static_cast<float>(cfg::SCREEN_H);
    fx = cfg::FOCAL_PX * scaleX;
    fy = cfg::FOCAL_PX * scaleY;
    cx = (static_cast<float>(width) * 0.5f) + shiftXPx * scaleX;
    cy = horizonY * scaleY;
    lightDir = normalize3({ -0.40f, 0.80f, -0.44f });
}

void RasterPipeline::clearDepth(float infVal) {
    const int total = width * height;
    std::fill(depthBuffer, depthBuffer + total, infVal);
}

uint32_t RasterPipeline::applyExponentialFog(uint32_t surfaceColor, float z) const {
    if (z <= zNear) return surfaceColor;
    float factor = std::exp(-fogDensity * (z - zNear));
    factor = mathutil::clamp(factor, 0.0f, 1.0f);
    return mathutil::lerpColor(fogColor, surfaceColor, factor);
}

uint32_t RasterPipeline::shadeLambert(uint32_t baseColor, const Vec3& normal, float specStrength) const {
    Vec3 N = normalize3(normal);
    float ndotl = std::max(0.0f, dot3(N, lightDir));
    float intensity = ambient + diffuse * ndotl;

    uint32_t lit = scaleColorBrightness(baseColor, intensity);
    if (specStrength > 0.0f && ndotl > 0.65f) {
        float spec = specStrength * std::pow((ndotl - 0.65f) / 0.35f, 2.0f);
        lit = mathutil::lerpColor(lit, RGB(255, 255, 255), mathutil::clamp(spec, 0.0f, 0.65f));
    }
    return lit;
}

static void rasterizeClippedTriangle(RasterPipeline& pipe,
                                     const Vertex3D& v0, const Vertex3D& v1, const Vertex3D& v2,
                                     uint32_t flatLitColor) {
    float sx0, sy0, sx1, sy1, sx2, sy2;
    if (!pipe.projectCameraPoint(v0.pos, sx0, sy0)) return;
    if (!pipe.projectCameraPoint(v1.pos, sx1, sy1)) return;
    if (!pipe.projectCameraPoint(v2.pos, sx2, sy2)) return;

    // 2D signed area (twice screen area)
    float area = (sx1 - sx0) * (sy2 - sy0) - (sy1 - sy0) * (sx2 - sx0);
    if (std::fabs(area) < 0.25f) return;

    // Ensure consistent winding for edge functions
    Vertex3D a = v0, b = v1, c = v2;
    if (area < 0.0f) {
        std::swap(b, c);
        std::swap(sx1, sx2);
        std::swap(sy1, sy2);
        area = -area;
    }
    const float invArea = 1.0f / area;

    int minX = std::max(0, static_cast<int>(std::floor(std::min({ sx0, sx1, sx2 }))));
    int maxX = std::min(pipe.width  - 1, static_cast<int>(std::ceil(std::max({ sx0, sx1, sx2 }))));
    int minY = std::max(0, static_cast<int>(std::floor(std::min({ sy0, sy1, sy2 }))));
    int maxY = std::min(pipe.height - 1, static_cast<int>(std::ceil(std::max({ sy0, sy1, sy2 }))));
    if (minX > maxX || minY > maxY) return;

    const float invZ0 = 1.0f / a.pos.z;
    const float invZ1 = 1.0f / b.pos.z;
    const float invZ2 = 1.0f / c.pos.z;

    // Edge step increments per pixel
    const float A01 = sy0 - sy1, B01 = sx1 - sx0;
    const float A12 = sy1 - sy2, B12 = sx2 - sx1;
    const float A20 = sy2 - sy0, B20 = sx0 - sx2;

    float px0 = static_cast<float>(minX) + 0.5f;
    float py0 = static_cast<float>(minY) + 0.5f;

    float w0_row = (sx2 - sx1) * (py0 - sy1) - (sy2 - sy1) * (px0 - sx1);
    float w1_row = (sx0 - sx2) * (py0 - sy2) - (sy0 - sy2) * (px0 - sx2);
    float w2_row = (sx1 - sx0) * (py0 - sy0) - (sy1 - sy0) * (px0 - sx0);

    // Average Z for fog lookup across small triangle (or per-pixel for large triangles)
    bool uniformLight = (std::fabs(a.light - 1.0f) < 0.02f &&
                         std::fabs(b.light - 1.0f) < 0.02f &&
                         std::fabs(c.light - 1.0f) < 0.02f);

    for (int y = minY; y <= maxY; ++y) {
        float w0 = w0_row;
        float w1 = w1_row;
        float w2 = w2_row;
        uint32_t* dstColor = pipe.colorBuf    + y * pipe.width;
        float*    dstDepth = pipe.depthBuffer + y * pipe.width;

        for (int x = minX; x <= maxX; ++x) {
            if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                float l0 = w0 * invArea;
                float l1 = w1 * invArea;
                float l2 = 1.0f - l0 - l1;
                float invZ = l0 * invZ0 + l1 * invZ1 + l2 * invZ2;
                if (invZ > 1e-6f) {
                    float z = 1.0f / invZ;
                    if (z < dstDepth[x]) {
                        dstDepth[x] = z;
                        uint32_t col = flatLitColor;
                        if (!uniformLight) {
                            float g = l0 * a.light + l1 * b.light + l2 * c.light;
                            col = scaleColorBrightness(col, g);
                        }
                        dstColor[x] = pipe.applyExponentialFog(col, z);
                    }
                }
            }
            w0 += A12;
            w1 += A20;
            w2 += A01;
        }
        w0_row += B12;
        w1_row += B20;
        w2_row += B01;
    }
}

void DrawTriangle3D(RasterPipeline& pipe,
                    const Vertex3D& v0, const Vertex3D& v1, const Vertex3D& v2,
                    bool doubleSided, float specStrength) {
    if (!pipe.colorBuf) return;

    // Compute camera-space surface normal N = normalize((v1 - v0) x (v2 - v0))
    Vec3 e1 = v1.pos - v0.pos;
    Vec3 e2 = v2.pos - v0.pos;
    Vec3 N  = cross3(e1, e2);
    float len2 = dot3(N, N);
    if (len2 <= 1e-8f) return;
    N = N * (1.0f / std::sqrt(len2));

    // Back-face test against camera ray v0.pos
    if (dot3(N, v0.pos) > 0.0f) {
        if (!doubleSided) return;
        N = N * -1.0f;
    }

    uint32_t litColor = pipe.shadeLambert(v0.color, N, specStrength);

    // Sutherland-Hodgman Near-Plane Clipping against Z >= pipe.zNear
    const float zNear = pipe.zNear;
    const Vertex3D inVerts[3] = { v0, v1, v2 };
    Vertex3D outVerts[4];
    int outCount = 0;

    auto lerpVertex = [&](const Vertex3D& a, const Vertex3D& b) -> Vertex3D {
        float t = (zNear - a.pos.z) / (b.pos.z - a.pos.z);
        t = mathutil::clamp(t, 0.0f, 1.0f);
        Vertex3D r;
        r.pos   = a.pos + (b.pos - a.pos) * t;
        r.pos.z = zNear;
        r.color = a.color;
        r.light = mathutil::lerp(a.light, b.light, t);
        return r;
    };

    for (int i = 0; i < 3; ++i) {
        const Vertex3D& cur = inVerts[i];
        const Vertex3D& nxt = inVerts[(i + 1) % 3];
        bool curIn = (cur.pos.z >= zNear);
        bool nxtIn = (nxt.pos.z >= zNear);

        if (curIn && nxtIn) {
            outVerts[outCount++] = nxt;
        } else if (curIn && !nxtIn) {
            outVerts[outCount++] = lerpVertex(cur, nxt);
        } else if (!curIn && nxtIn) {
            outVerts[outCount++] = lerpVertex(cur, nxt);
            outVerts[outCount++] = nxt;
        }
    }

    if (outCount < 3) return;
    rasterizeClippedTriangle(pipe, outVerts[0], outVerts[1], outVerts[2], litColor);
    if (outCount == 4) {
        rasterizeClippedTriangle(pipe, outVerts[0], outVerts[2], outVerts[3], litColor);
    }
}

void DrawQuad3D(RasterPipeline& pipe,
                const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3,
                uint32_t baseColor, bool doubleSided, float specStrength) {
    Vertex3D v0{ p0, baseColor, 1.0f };
    Vertex3D v1{ p1, baseColor, 1.0f };
    Vertex3D v2{ p2, baseColor, 1.0f };
    Vertex3D v3{ p3, baseColor, 1.0f };
    DrawTriangle3D(pipe, v0, v1, v2, doubleSided, specStrength);
    DrawTriangle3D(pipe, v0, v2, v3, doubleSided, specStrength);
}

void DrawTrapezoid(RasterPipeline& pipe,
                   const Vec3& nearCenter, float nearHalfW, float nearBankSlope,
                   const Vec3& farCenter,  float farHalfW,  float farBankSlope,
                   const RoadPalette& palBand0, const RoadPalette& palBand1,
                   int absSegIndex, uint8_t segFlags, int clipMaxY) {
    if (!pipe.colorBuf) return;
    if (nearCenter.z < pipe.zNear || farCenter.z < pipe.zNear) return;

    float sx0, sy0, sx1, sy1;
    if (!pipe.projectCameraPoint(nearCenter, sx0, sy0)) return;
    if (!pipe.projectCameraPoint(farCenter,  sx1, sy1)) return;

    float sw0 = (nearHalfW / nearCenter.z) * pipe.fx;
    float sw1 = (farHalfW  / farCenter.z)  * pipe.fx;

    if (sy0 <= sy1) return; // Back-facing slope
    int maxY = std::min(pipe.height, clipMaxY);
    if (sy1 >= static_cast<float>(maxY)) return;

    int rBeg = std::max(0, static_cast<int>(std::ceil(sy1)));
    int rEnd = std::min(maxY, static_cast<int>(std::ceil(sy0)));
    if (rBeg >= rEnd) return;

    const float invDy = 1.0f / (sy1 - sy0);
    const float dX    = (sx1 - sx0) * invDy;
    const float dW    = (sw1 - sw0) * invDy;
    const float invZ0 = 1.0f / nearCenter.z;
    const float invZ1 = 1.0f / farCenter.z;
    const float dInvZ = (invZ1 - invZ0) * invDy;
    const float dBank = (farBankSlope - nearBankSlope) * invDy;

    float t0   = static_cast<float>(rBeg) - sy0;
    float rx   = sx0   + dX    * t0;
    float rw   = sw0   + dW    * t0;
    float invZ = invZ0 + dInvZ * t0;
    float bank = nearBankSlope + dBank * t0;

    // Stripe band parity & analytical box-filter anti-aliasing to eliminate horizon moire shimmer
    const int band = (absSegIndex / cfg::RUMBLE_LEN) & 1;
    const RoadPalette& P0 = (band == 0) ? palBand0 : palBand1;
    const RoadPalette& P1 = (band == 0) ? palBand1 : palBand0;

    for (int y = rBeg; y < rEnd; ++y) {
        float zRow = (invZ > 1e-6f) ? (1.0f / invZ) : farCenter.z;

        // Mip-mapping / analytical box filter blend between alternating stripe bands at far distance
        float mipBlend = 0.5f * (1.0f - std::exp(-0.000095f * zRow));
        uint32_t grassCol  = mathutil::lerpColor(P0.grass,  P1.grass,  mipBlend);
        uint32_t rumbleCol = mathutil::lerpColor(P0.rumble, P1.rumble, mipBlend);
        uint32_t roadCol   = mathutil::lerpColor(P0.road,   P1.road,   mipBlend);
        uint32_t laneCol   = mathutil::lerpColor(P0.lane,   roadCol,   mipBlend * 1.4f);

        // Apply subtle banking shade across road surface
        if (std::fabs(bank) > 0.005f) {
            roadCol = scaleColorBrightness(roadCol, 1.0f + bank * 0.25f);
        }

        // Apply exponential distance fog per scanline
        grassCol  = pipe.applyExponentialFog(grassCol,  zRow);
        rumbleCol = pipe.applyExponentialFog(rumbleCol, zRow);
        roadCol   = pipe.applyExponentialFog(roadCol,   zRow);
        laneCol   = pipe.applyExponentialFog(laneCol,   zRow);
        uint32_t tireWearCol = scaleColorBrightness(roadCol, 0.91f);
        uint32_t curbBevelCol = scaleColorBrightness(rumbleCol, 1.15f);

        float hw   = std::max(2.0f, rw);
        float rumW = std::max(2.0f, hw * 0.085f);
        float roadL = rx - hw;
        float roadR = rx + hw;
        float rumL  = roadL - rumW;
        float rumR  = roadR + rumW;

        uint32_t* dstCol = pipe.colorBuf    + y * pipe.width;
        float*    dstZ   = pipe.depthBuffer + y * pipe.width;

        int iRumL  = std::clamp(static_cast<int>(std::floor(rumL)),  0, pipe.width);
        int iRoadL = std::clamp(static_cast<int>(std::floor(roadL)), 0, pipe.width);
        int iRoadR = std::clamp(static_cast<int>(std::ceil(roadR)),  0, pipe.width);
        int iRumR  = std::clamp(static_cast<int>(std::ceil(rumR)),   0, pipe.width);

        // Fill terrain & road spans and write Z-buffer depth
        for (int x = 0; x < iRumL; ++x)       { dstCol[x] = grassCol;  dstZ[x] = zRow; }
        for (int x = iRumL; x < iRoadL; ++x)  { dstCol[x] = (x == iRumL) ? curbBevelCol : rumbleCol; dstZ[x] = zRow; }
        for (int x = iRoadL; x < iRoadR; ++x) { dstCol[x] = roadCol;   dstZ[x] = zRow; }
        for (int x = iRoadR; x < iRumR; ++x)  { dstCol[x] = (x == iRumR - 1) ? curbBevelCol : rumbleCol; dstZ[x] = zRow; }
        for (int x = iRumR; x < pipe.width; ++x) { dstCol[x] = grassCol; dstZ[x] = zRow; }

        // Analytical 1-px anti-aliased edge smoothing at grass/rumble and rumble/road transitions
        auto smoothEdge = [&](float edgeX, uint32_t colLeft, uint32_t colRight) {
            int ix = static_cast<int>(std::floor(edgeX));
            if (ix >= 0 && ix < pipe.width) {
                float frac = edgeX - static_cast<float>(ix);
                dstCol[ix] = mathutil::lerpColor(colRight, colLeft, frac);
            }
        };
        smoothEdge(rumL,  grassCol,  rumbleCol);
        smoothEdge(roadL, rumbleCol, roadCol);
        smoothEdge(roadR, roadCol,   rumbleCol);
        smoothEdge(rumR,  rumbleCol, grassCol);

        // Subtle dual tire-wear tracks along each of the 3 lanes
        if (hw > 18.0f) {
            for (int lane = 0; lane < cfg::LANES; ++lane) {
                float laneCenter = roadL + (2.0f * hw * (static_cast<float>(lane) + 0.5f)) / static_cast<float>(cfg::LANES);
                float trackOff   = hw * 0.11f;
                int twW          = std::max(1, static_cast<int>(hw * 0.035f));
                int t1 = static_cast<int>(laneCenter - trackOff);
                int t2 = static_cast<int>(laneCenter + trackOff);
                for (int dx = -twW; dx <= twW; ++dx) {
                    if (t1 + dx >= iRoadL && t1 + dx < iRoadR) dstCol[t1 + dx] = tireWearCol;
                    if (t2 + dx >= iRoadL && t2 + dx < iRoadR) dstCol[t2 + dx] = tireWearCol;
                }
            }
        }

        // Start/Finish checkerboard or Anti-Aliased Lane Dashes
        if (segFlags & 0x02) {
            int checkW = std::max(2, static_cast<int>(hw / 6.0f));
            for (int x = iRoadL; x < iRoadR; ++x) {
                bool whiteSq = (((x - iRoadL) / checkW) + band) & 1;
                dstCol[x] = pipe.applyExponentialFog(whiteSq ? RGB(245, 245, 250) : RGB(28, 30, 36), zRow);
            }
        } else if (band == 0) {
            float lw = std::max(1.5f, hw * 0.025f);
            for (int k = 1; k < cfg::LANES; ++k) {
                float lx = roadL + (2.0f * hw * static_cast<float>(k)) / static_cast<float>(cfg::LANES);
                float l0 = lx - lw * 0.5f;
                float l1 = lx + lw * 0.5f;
                int il0 = std::max(iRoadL, static_cast<int>(std::floor(l0)));
                int il1 = std::min(iRoadR, static_cast<int>(std::ceil(l1)));
                for (int x = il0; x < il1; ++x) {
                    dstCol[x] = laneCol;
                }
                smoothEdge(l0, roadCol, laneCol);
                smoothEdge(l1, laneCol, roadCol);
            }
        }

        rx   += dX;
        rw   += dW;
        invZ += dInvZ;
        bank += dBank;
    }
}

void DrawOpponentCarMesh3D(RasterPipeline& pipe, const Vec3& baseCamPos, float yawRad, float bankRad,
                           uint16_t spriteId, float brakeGlow) {
    if (baseCamPos.z < pipe.zNear || baseCamPos.z > 32000.0f) return;

    float cosY = std::cos(yawRad), sinY = std::sin(yawRad);
    float cosB = std::cos(bankRad), sinB = std::sin(bankRad);

    uint32_t bodyMain  = RGB(225, 36, 48);
    uint32_t bodySide  = RGB(175, 24, 34);
    uint32_t bodyTop   = RGB(255, 95, 95);
    if (spriteId == SPR_CAR_BLUE) {
        bodyMain = RGB(36, 120, 245);
        bodySide = RGB(22, 82, 180);
        bodyTop  = RGB(110, 175, 255);
    } else if (spriteId == SPR_CAR_YELLOW) {
        bodyMain = RGB(245, 195, 25);
        bodySide = RGB(195, 145, 14);
        bodyTop  = RGB(255, 230, 105);
    }

    // Car dimensions in world units (width ~ 520 wu, height ~ 380 wu, length ~ 760 wu)
    const float hw = 250.0f;
    const float hl = 360.0f;

    // 1. Ground shadow quad just above road surface (y = 6 wu)
    {
        Vec3 s0 = transformLocalToCam({ -hw * 1.06f, 6.0f, -hl * 1.02f }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 s1 = transformLocalToCam({ +hw * 1.06f, 6.0f, -hl * 1.02f }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 s2 = transformLocalToCam({ +hw * 1.06f, 6.0f, +hl * 1.02f }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 s3 = transformLocalToCam({ -hw * 1.06f, 6.0f, +hl * 1.02f }, baseCamPos, cosY, sinY, cosB, sinB);
        DrawQuad3D(pipe, s0, s1, s2, s3, RGB(18, 20, 26), true, 0.0f);
    }

    // 2. Four 3D Wheels (front-left, front-right, rear-left, rear-right)
    const uint32_t tireCol = RGB(24, 26, 32);
    const uint32_t rimCol  = RGB(150, 160, 175);
    drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB, -hw - 12.0f, 0.0f, -hl * 0.72f, -hw + 55.0f, 115.0f, -hl * 0.28f, tireCol, rimCol, tireCol, tireCol, 0.05f);
    drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB, +hw - 55.0f, 0.0f, -hl * 0.72f, +hw + 12.0f, 115.0f, -hl * 0.28f, tireCol, rimCol, tireCol, tireCol, 0.05f);
    drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB, -hw - 8.0f,  0.0f, +hl * 0.28f, -hw + 50.0f, 110.0f, +hl * 0.72f, tireCol, rimCol, tireCol, tireCol, 0.05f);
    drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB, +hw - 50.0f, 0.0f, +hl * 0.28f, +hw + 8.0f,  110.0f, +hl * 0.72f, tireCol, rimCol, tireCol, tireCol, 0.05f);

    // 3. Lower Widebody Chassis & Aerodynamic Side Skirts (y = 42..185 wu)
    drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB,
                  -hw, 42.0f, -hl, +hw, 185.0f, +hl,
                  bodyTop, bodySide, bodyMain, bodySide, 0.32f);

    // 4. Sculpted Fastback Cabin / Greenhouse (tapered inward, y = 185..335 wu)
    {
        float cw0 = hw * 0.84f, cw1 = hw * 0.68f;
        Vec3 b0 = transformLocalToCam({ -cw0, 185.0f, -hl * 0.65f }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 b1 = transformLocalToCam({ +cw0, 185.0f, -hl * 0.65f }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 b2 = transformLocalToCam({ +cw0, 185.0f, +hl * 0.45f }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 b3 = transformLocalToCam({ -cw0, 185.0f, +hl * 0.45f }, baseCamPos, cosY, sinY, cosB, sinB);

        Vec3 t0 = transformLocalToCam({ -cw1, 335.0f, -hl * 0.25f }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 t1 = transformLocalToCam({ +cw1, 335.0f, -hl * 0.25f }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 t2 = transformLocalToCam({ +cw1, 335.0f, +hl * 0.18f }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 t3 = transformLocalToCam({ -cw1, 335.0f, +hl * 0.18f }, baseCamPos, cosY, sinY, cosB, sinB);

        uint32_t glassCol = RGB(42, 62, 92);
        // Rear fastback glass window
        DrawQuad3D(pipe, b0, b1, t1, t0, glassCol, true, 0.45f);
        // Roof panel
        DrawQuad3D(pipe, t0, t1, t2, t3, bodyTop, true, 0.38f);
        // Left & right side windows
        DrawQuad3D(pipe, b3, b0, t0, t3, glassCol, true, 0.40f);
        DrawQuad3D(pipe, b1, b2, t2, t1, glassCol, true, 0.40f);
        // Front windshield
        DrawQuad3D(pipe, b2, b3, t3, t2, glassCol, true, 0.45f);
    }

    // 5. 3D Raised Carbon-Fibre GT Rear Wing & Twin Pylons
    drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB,
                  -hw * 0.92f, 235.0f, -hl * 0.95f, +hw * 0.92f, 255.0f, -hl * 0.72f,
                  RGB(45, 50, 62), RGB(30, 34, 42), RGB(35, 40, 50), RGB(35, 40, 50), 0.25f);
    drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB,
                  -hw * 0.55f, 185.0f, -hl * 0.88f, -hw * 0.45f, 235.0f, -hl * 0.76f,
                  RGB(35, 38, 46), RGB(35, 38, 46), RGB(35, 38, 46), RGB(35, 38, 46), 0.1f);
    drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB,
                  +hw * 0.45f, 185.0f, -hl * 0.88f, +hw * 0.55f, 235.0f, -hl * 0.76f,
                  RGB(35, 38, 46), RGB(35, 38, 46), RGB(35, 38, 46), RGB(35, 38, 46), 0.1f);

    // 6. Emissive 3D LED Tail-Light Bar & Brake Clusters on Rear Fascia (z = -hl - 3 wu)
    {
        uint32_t tailRed  = (brakeGlow > 0.3f) ? RGB(255, 80, 60)  : RGB(240, 30, 40);
        uint32_t tailCore = (brakeGlow > 0.3f) ? RGB(255, 235, 160): RGB(255, 140, 80);
        float zRear = -hl - 3.0f;

        Vec3 l0 = transformLocalToCam({ -hw * 0.88f, 132.0f, zRear }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 l1 = transformLocalToCam({ +hw * 0.88f, 132.0f, zRear }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 l2 = transformLocalToCam({ +hw * 0.88f, 162.0f, zRear }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 l3 = transformLocalToCam({ -hw * 0.88f, 162.0f, zRear }, baseCamPos, cosY, sinY, cosB, sinB);
        DrawQuad3D(pipe, l0, l1, l2, l3, tailRed, true, 0.5f);

        // Left & right bright brake LED pods
        Vec3 bl0 = transformLocalToCam({ -hw * 0.82f, 138.0f, zRear - 2.0f }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 bl1 = transformLocalToCam({ -hw * 0.42f, 138.0f, zRear - 2.0f }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 bl2 = transformLocalToCam({ -hw * 0.42f, 156.0f, zRear - 2.0f }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 bl3 = transformLocalToCam({ -hw * 0.82f, 156.0f, zRear - 2.0f }, baseCamPos, cosY, sinY, cosB, sinB);
        DrawQuad3D(pipe, bl0, bl1, bl2, bl3, tailCore, true, 0.6f);

        Vec3 br0 = transformLocalToCam({ +hw * 0.42f, 138.0f, zRear - 2.0f }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 br1 = transformLocalToCam({ +hw * 0.82f, 138.0f, zRear - 2.0f }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 br2 = transformLocalToCam({ +hw * 0.82f, 156.0f, zRear - 2.0f }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 br3 = transformLocalToCam({ +hw * 0.42f, 156.0f, zRear - 2.0f }, baseCamPos, cosY, sinY, cosB, sinB);
        DrawQuad3D(pipe, br0, br1, br2, br3, tailCore, true, 0.6f);

        // Rear Carbon Diffuser & License Plate
        Vec3 df0 = transformLocalToCam({ -hw * 0.65f, 45.0f, zRear }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 df1 = transformLocalToCam({ +hw * 0.65f, 45.0f, zRear }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 df2 = transformLocalToCam({ +hw * 0.65f, 92.0f, zRear }, baseCamPos, cosY, sinY, cosB, sinB);
        Vec3 df3 = transformLocalToCam({ -hw * 0.65f, 92.0f, zRear }, baseCamPos, cosY, sinY, cosB, sinB);
        DrawQuad3D(pipe, df0, df1, df2, df3, RGB(26, 28, 34), true, 0.1f);
    }
}

void DrawTruckMesh3D(RasterPipeline& pipe, const Vec3& baseCamPos, float yawRad, float bankRad) {
    if (baseCamPos.z < pipe.zNear || baseCamPos.z > 34000.0f) return;

    float cosY = std::cos(yawRad), sinY = std::sin(yawRad);
    float cosB = std::cos(bankRad), sinB = std::sin(bankRad);

    const float hw = 310.0f;
    const float hl = 560.0f;

    // 1. Ground shadow
    Vec3 s0 = transformLocalToCam({ -hw, 6.0f, -hl }, baseCamPos, cosY, sinY, cosB, sinB);
    Vec3 s1 = transformLocalToCam({ +hw, 6.0f, -hl }, baseCamPos, cosY, sinY, cosB, sinB);
    Vec3 s2 = transformLocalToCam({ +hw, 6.0f, +hl }, baseCamPos, cosY, sinY, cosB, sinB);
    Vec3 s3 = transformLocalToCam({ -hw, 6.0f, +hl }, baseCamPos, cosY, sinY, cosB, sinB);
    DrawQuad3D(pipe, s0, s1, s2, s3, RGB(16, 18, 24), true, 0.0f);

    // 2. Dual axle wheels
    drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB, -hw, 0.0f, -hl * 0.85f, -hw + 75.0f, 140.0f, +hl * 0.85f, RGB(24, 26, 32), RGB(40, 44, 52), RGB(24, 26, 32), RGB(24, 26, 32), 0.05f);
    drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB, +hw - 75.0f, 0.0f, -hl * 0.85f, +hw, 140.0f, +hl * 0.85f, RGB(24, 26, 32), RGB(40, 44, 52), RGB(24, 26, 32), RGB(24, 26, 32), 0.05f);

    // 3. Large 3D Box Trailer (y = 110..730 wu)
    drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB,
                  -hw, 110.0f, -hl, +hw, 730.0f, +hl * 0.55f,
                  RGB(235, 240, 248), RGB(188, 198, 212), RGB(215, 222, 234), RGB(175, 185, 200), 0.18f);

    // 4. Tractor Cab ahead of trailer
    drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB,
                  -hw * 0.88f, 95.0f, +hl * 0.56f, +hw * 0.88f, 560.0f, +hl,
                  RGB(55, 115, 210), RGB(42, 90, 175), RGB(42, 90, 175), RGB(65, 130, 230), 0.25f);

    // 5. Rear Hazard Bumper & Tail Lights
    float zRear = -hl - 3.0f;
    Vec3 b0 = transformLocalToCam({ -hw * 0.92f, 95.0f,  zRear }, baseCamPos, cosY, sinY, cosB, sinB);
    Vec3 b1 = transformLocalToCam({ +hw * 0.92f, 95.0f,  zRear }, baseCamPos, cosY, sinY, cosB, sinB);
    Vec3 b2 = transformLocalToCam({ +hw * 0.92f, 155.0f, zRear }, baseCamPos, cosY, sinY, cosB, sinB);
    Vec3 b3 = transformLocalToCam({ -hw * 0.92f, 155.0f, zRear }, baseCamPos, cosY, sinY, cosB, sinB);
    DrawQuad3D(pipe, b0, b1, b2, b3, RGB(235, 50, 45), true, 0.35f);
}

void DrawSceneryMesh3D(RasterPipeline& pipe, const Vec3& baseCamPos, float yawRad,
                       uint16_t spriteId, bool flipSide) {
    if (baseCamPos.z < pipe.zNear || baseCamPos.z > 32000.0f) return;

    float cosY = std::cos(yawRad), sinY = std::sin(yawRad);
    const float cosB = 1.0f, sinB = 0.0f;

    if (spriteId == SPR_PINE || spriteId == SPR_TREE) {
        // 3D Trunk
        float tw = (spriteId == SPR_PINE) ? 45.0f : 65.0f;
        float th = (spriteId == SPR_PINE) ? 320.0f : 420.0f;
        drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB,
                      -tw, 0.0f, -tw, +tw, th, +tw,
                      RGB(105, 68, 36), RGB(92, 58, 30), RGB(112, 72, 38), RGB(80, 50, 26), 0.0f);

        // 3D Multi-Tier Pyramidal Canopy
        int tiers = (spriteId == SPR_PINE) ? 3 : 2;
        uint32_t leafA = (pipe.theme == 3) ? RGB(205, 225, 245)
                       : (spriteId == SPR_PINE) ? RGB(22, 138, 52) : RGB(38, 175, 48);
        uint32_t leafB = (pipe.theme == 3) ? RGB(165, 190, 220)
                       : (spriteId == SPR_PINE) ? RGB(14, 102, 38) : RGB(24, 135, 34);

        for (int t = 0; t < tiers; ++t) {
            float yBot = 240.0f + t * 280.0f;
            float yTop = yBot + ((spriteId == SPR_PINE) ? 460.0f : 560.0f);
            float rad  = ((spriteId == SPR_PINE) ? 280.0f : 360.0f) * (1.0f - 0.22f * t);

            Vec3 apex = transformLocalToCam({ 0.0f, yTop, 0.0f }, baseCamPos, cosY, sinY, cosB, sinB);
            Vec3 c0   = transformLocalToCam({ -rad, yBot, -rad }, baseCamPos, cosY, sinY, cosB, sinB);
            Vec3 c1   = transformLocalToCam({ +rad, yBot, -rad }, baseCamPos, cosY, sinY, cosB, sinB);
            Vec3 c2   = transformLocalToCam({ +rad, yBot, +rad }, baseCamPos, cosY, sinY, cosB, sinB);
            Vec3 c3   = transformLocalToCam({ -rad, yBot, +rad }, baseCamPos, cosY, sinY, cosB, sinB);

            DrawTriangle3D(pipe, { c0, leafA, 1.0f }, { c1, leafA, 1.0f }, { apex, leafA, 1.15f }, true, 0.05f);
            DrawTriangle3D(pipe, { c1, leafB, 1.0f }, { c2, leafB, 1.0f }, { apex, leafB, 1.10f }, true, 0.05f);
            DrawTriangle3D(pipe, { c2, leafB, 1.0f }, { c3, leafB, 1.0f }, { apex, leafB, 1.05f }, true, 0.05f);
            DrawTriangle3D(pipe, { c3, leafA, 1.0f }, { c0, leafA, 1.0f }, { apex, leafA, 1.12f }, true, 0.05f);
        }
    } else if (spriteId == SPR_LAMP) {
        // 3D Streetlamp Pole + Cantilever Arm + Glowing Luminaire
        float armDir = flipSide ? +1.0f : -1.0f;
        drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB,
                      -18.0f, 0.0f, -18.0f, +18.0f, 1420.0f, +18.0f,
                      RGB(170, 180, 195), RGB(130, 140, 155), RGB(155, 165, 180), RGB(120, 130, 145), 0.25f);
        float xA = (armDir > 0.0f) ? -18.0f : -260.0f;
        float xB = (armDir > 0.0f) ? +260.0f : +18.0f;
        drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB,
                      xA, 1380.0f, -16.0f, xB, 1425.0f, +16.0f,
                      RGB(180, 190, 205), RGB(145, 155, 170), RGB(165, 175, 190), RGB(165, 175, 190), 0.25f);
        float lx0 = (armDir > 0.0f) ? 160.0f : -260.0f;
        float lx1 = (armDir > 0.0f) ? 260.0f : -160.0f;
        drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB,
                      lx0, 1340.0f, -28.0f, lx1, 1380.0f, +28.0f,
                      RGB(255, 250, 180), RGB(255, 245, 160), RGB(255, 250, 190), RGB(255, 245, 160), 0.6f);
    } else if (spriteId == SPR_SIGN) {
        // 3D Twin-Pylon Roadside Billboard
        drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB,
                      -320.0f, 0.0f, -20.0f, -270.0f, 420.0f, +20.0f,
                      RGB(130, 138, 150), RGB(110, 118, 130), RGB(125, 132, 145), RGB(110, 118, 130), 0.2f);
        drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB,
                      +270.0f, 0.0f, -20.0f, +320.0f, 420.0f, +20.0f,
                      RGB(130, 138, 150), RGB(110, 118, 130), RGB(125, 132, 145), RGB(110, 118, 130), 0.2f);
        drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB,
                      -440.0f, 340.0f, -28.0f, +440.0f, 760.0f, +28.0f,
                      RGB(240, 245, 255), RGB(180, 190, 210), RGB(230, 45, 70), RGB(30, 90, 215), 0.3f);
    } else {
        // 3D Faceted Rock / Bush Mound
        uint32_t colA = (spriteId == SPR_BUSH) ? RGB(38, 175, 48) : RGB(150, 156, 168);
        uint32_t colB = (spriteId == SPR_BUSH) ? RGB(22, 128, 32) : RGB(105, 112, 124);
        drawBoxMesh3D(pipe, baseCamPos, cosY, sinY, cosB, sinB,
                      -180.0f, 0.0f, -140.0f, +180.0f, 240.0f, +140.0f,
                      colA, colB, colA, colB, 0.1f);
    }
}

void DrawCheckpointArchMesh3D(RasterPipeline& pipe, const Vec3& centerCamPos, float roadHalfW,
                              float bankSlope, bool isStartFinish) {
    if (centerCamPos.z < pipe.zNear || centerCamPos.z > 36000.0f) return;

    float cosB = 1.0f, sinB = bankSlope;
    uint32_t pillarCol = isStartFinish ? RGB(235, 240, 250) : RGB(0, 225, 255);
    uint32_t beamCol   = isStartFinish ? RGB(240, 45, 60)   : RGB(22, 95, 230);

    float xL = -roadHalfW * 1.08f;
    float xR = +roadHalfW * 1.08f;
    float archH = 1480.0f;

    // Left 3D Tower
    drawBoxMesh3D(pipe, centerCamPos, 1.0f, 0.0f, cosB, sinB,
                  xL - 90.0f, -60.0f, -45.0f, xL, archH, +45.0f,
                  pillarCol, pillarCol, pillarCol, pillarCol, 0.35f);
    // Right 3D Tower
    drawBoxMesh3D(pipe, centerCamPos, 1.0f, 0.0f, cosB, sinB,
                  xR, -60.0f, -45.0f, xR + 90.0f, archH, +45.0f,
                  pillarCol, pillarCol, pillarCol, pillarCol, 0.35f);
    // Overhead 3D Gantry Span
    drawBoxMesh3D(pipe, centerCamPos, 1.0f, 0.0f, cosB, sinB,
                  xL - 90.0f, archH - 220.0f, -55.0f, xR + 90.0f, archH, +55.0f,
                  pillarCol, beamCol, beamCol, beamCol, 0.40f);
}

void RenderCockpit3D(RasterPipeline& pipe, const Vehicle& vehicle, const Cockpit& cockpit,
                     const CarSpec& carSpec, bool /*wipersOn*/, float /*wiperPhase*/, int /*weatherMode*/) {
    if (!pipe.colorBuf || pipe.width < 960 || pipe.height < 720) return;

    DisplayBuffer& db = *reinterpret_cast<DisplayBuffer*>(pipe.colorBuf);
    const int ox = cockpit.ox() * 3;
    const int oy = cockpit.oy() * 3;

    // 1. 3D Sculpted Instrument Binnacle Hood & Stitched Leather Dash Bevel (forward pass over 960x720)
    int hoodL = 135 + ox, hoodR = 825 + ox, hoodTop = 532 + oy;
    db.drawLine(hoodL, 576 + oy, hoodL + 85, hoodTop, 3, RGB(68, 76, 92));
    db.drawLine(hoodL + 85, hoodTop, hoodR - 85, hoodTop, 3, RGB(88, 98, 118));
    db.drawLine(hoodR - 85, hoodTop, hoodR, 576 + oy, 3, RGB(68, 76, 92));
    // Stitched accent seam along binnacle crest
    db.drawLine(hoodL + 88, hoodTop + 5, hoodR - 88, hoodTop + 5, 1, carSpec.accentColor);

    // 2. High-Resolution 3D Beveled Tachometer (210, 642) & Speedometer (750, 642)
    const int tachCx = 210 + ox, tachCy = 642 + oy;
    const int spdCx  = 750 + ox, spdCy  = 642 + oy;
    db.drawCircle(tachCx, tachCy, 78, RGB(150, 162, 182), false);
    db.drawCircle(tachCx, tachCy, 75, carSpec.accentColor, false);
    db.drawCircle(spdCx,  spdCy,  78, RGB(150, 162, 182), false);
    db.drawCircle(spdCx,  spdCy,  75, carSpec.accentColor, false);

    // High-res illuminated needles
    {
        float rpmNorm = mathutil::clamp(cockpit.dispRpm() / 9000.0f, 0.0f, 1.0f);
        float rad = (225.0f - 270.0f * rpmNorm) * mathutil::PI / 180.0f;
        int tx = tachCx + static_cast<int>(std::lround(66.0f * std::cos(rad)));
        int ty = tachCy - static_cast<int>(std::lround(66.0f * std::sin(rad)));
        db.drawLine(tachCx, tachCy, tx, ty, 2, RGB(255, 65, 55));
        db.drawCircle(tachCx, tachCy, 8, RGB(210, 220, 235), true);
    }
    {
        float spdNorm = mathutil::clamp(cockpit.dispSpeed() / 300.0f, 0.0f, 1.0f);
        float rad = (225.0f - 270.0f * spdNorm) * mathutil::PI / 180.0f;
        int tx = spdCx + static_cast<int>(std::lround(66.0f * std::cos(rad)));
        int ty = spdCy - static_cast<int>(std::lround(66.0f * std::sin(rad)));
        db.drawLine(spdCx, spdCy, tx, ty, 2, carSpec.accentColor);
        db.drawCircle(spdCx, spdCy, 8, RGB(210, 220, 235), true);
    }

    // 3. High-Resolution 3D-Shaded Flat-Bottom GT Steering Wheel Rim & Specular Highlight
    {
        int wCx = 480 + ox;
        int wCy = 696 + oy;
        float rx = 126.0f, ry = 96.0f;
        float a  = cockpit.dispSteer() * (100.0f * mathutil::PI / 180.0f);

        int px[48], py[48];
        for (int k = 0; k < 48; ++k) {
            float theta = mathutil::TWO_PI * static_cast<float>(k) / 48.0f + a;
            // Flat-bottom GT profile when local angle is near +90 deg (bottom of wheel)
            float localAng = mathutil::TWO_PI * static_cast<float>(k) / 48.0f;
            float rScale = (std::sin(localAng) > 0.72f) ? 0.84f : 1.0f;
            px[k] = wCx + static_cast<int>(std::lround(rx * rScale * std::cos(theta)));
            py[k] = wCy + static_cast<int>(std::lround(ry * rScale * std::sin(theta)));
        }

        for (int k = 0; k < 48; ++k) {
            int kn = (k + 1) % 48;
            float theta = mathutil::TWO_PI * static_cast<float>(k) / 48.0f + a;
            // 3D normal lighting on torus rim based on angle
            float rimLight = 0.75f + 0.35f * std::max(0.0f, -std::sin(theta - 0.4f));
            bool centerMark = (k == 35 || k == 36);
            uint32_t baseCol = centerMark ? carSpec.accentColor : RGB(34, 40, 50);
            db.drawLine(px[k], py[k], px[kn], py[kn], 5, scaleColorBrightness(baseCol, rimLight));
        }
    }

    // 4. Gear & RPM Digital Sub-Readout inside Tachometer/Speedometer
    char buf[32];
    std::snprintf(buf, sizeof(buf), "G%d", vehicle.gear);
    db.drawText(tachCx - 12, tachCy + 24, buf, carSpec.accentColor, 2, true);
    std::snprintf(buf, sizeof(buf), "KM/H");
    db.drawText(spdCx - 22, spdCy + 26, buf, RGB(180, 205, 235), 1, true);
}

void RenderFrame(RasterPipeline& pipe,
                 const Track& track,
                 const Vehicle& vehicle,
                 const Traffic& traffic,
                 const Camera& cam,
                 const RenderContext& ctx,
                 const Cockpit& /*cockpit*/,
                 DisplayBuffer& outDisplay) {
    if (track.count <= 0) return;

    pipe.bind(outDisplay.px, DisplayBuffer::W, DisplayBuffer::H,
              static_cast<float>(ctx.horizonY), cam.shiftX);
    pipe.fogColor   = ctx.fogColor;
    pipe.theme      = ctx.theme;
    pipe.fogDensity = 0.000072f;
    pipe.clearDepth(1e30f);

    const int W = pipe.width;
    const int horizonY720 = std::clamp(static_cast<int>(std::lround(pipe.cy)), 0, pipe.height);

    // 1. Sky Gradient & Mountain Parallax Backdrop (rows 0..horizonY720)
    int bgOffset = static_cast<int>(std::floor(ctx.bgX)) & 511;
    if (bgOffset < 0) bgOffset += 512;

    for (int y = 0; y < pipe.height; ++y) {
        uint32_t* row = outDisplay.row(y);
        if (y < horizonY720) {
            int lutIdx = std::clamp((y * 255) / std::max(1, horizonY720), 0, 255);
            std::fill(row, row + W, ctx.skyLut[lutIdx]);

            int mtnY = (y - (horizonY720 - 144)) / 3;
            if (mtnY >= 0 && mtnY < 48) {
                const uint32_t* mtnRow = ctx.mountainTex + mtnY * 512;
                for (int x = 0; x < W; ++x) {
                    int mx = (bgOffset + (x / 3)) & 511;
                    uint32_t mc = mtnRow[mx];
                    if ((mc >> 24) != 0) row[x] = mc;
                }
            }
        } else {
            std::fill(row, row + W, ctx.fogColor);
        }
    }

    // 2. Pass A & B: 3D Road Trapezoid Projection with Camber/Banking, Z-Buffer Writes & AA Striping
    const RoadPalette& pal0 = ctx.pal[0][0];
    const RoadPalette& pal1 = ctx.pal[0][1];
    int maxY720 = 600; // world rows 0..599 (rows 600..719 covered by dashboard)

    // Precompute segment banking angles & tangent headings
    float segBank[cfg::DRAW_DIST + 1];
    float segYaw[cfg::DRAW_DIST + 1];
    float curYaw = 0.0f;
    for (int n = 0; n <= cfg::DRAW_DIST; ++n) {
        const Segment& s_ = track.segs[(ctx.baseSeg + n) % track.count];
        segBank[n] = mathutil::clamp(s_.curve * 0.028f, -0.10f, +0.10f);
        segYaw[n]  = curYaw;
        curYaw    += s_.curve * 0.0045f;
    }

    for (int n = 0; n < cfg::DRAW_DIST; ++n) {
        const RoadPoint& p0 = ctx.pts[n];
        const RoadPoint& p1 = ctx.pts[n + 1];
        Vec3 c0{ p0.wx, p0.wy, p0.cz };
        Vec3 c1{ p1.wx, p1.wy, p1.cz };

        int absSeg = (ctx.baseSeg + n) % track.count;
        uint8_t flags = track.segs[absSeg].flags;

        float syTop = pipe.cy - (c1.y / c1.z) * pipe.fy;
        DrawTrapezoid(pipe, c0, cfg::ROAD_HALF_W, segBank[n],
                      c1, cfg::ROAD_HALF_W, segBank[n + 1],
                      pal0, pal1, absSeg, flags, maxY720);

        if (syTop < static_cast<float>(maxY720)) {
            maxY720 = std::max(0, static_cast<int>(std::ceil(syTop)));
        }
    }

    // 3. Render 3D Polygonal Meshes into Z-Buffer (Scenery, Checkpoint Arches, Guardrails & Traffic Cars)
    const int maxMeshDist = 115;
    for (int n = maxMeshDist - 1; n >= 1; --n) {
        int absSeg = (ctx.baseSeg + n) % track.count;
        const Segment& seg = track.segs[absSeg];
        const RoadPoint& p0 = ctx.pts[n];

        // 3a. 3D Overhead Checkpoint & Start/Finish Arches
        if (seg.flags & 0x03) {
            Vec3 archPos{ p0.wx, p0.wy, p0.cz };
            DrawCheckpointArchMesh3D(pipe, archPos, cfg::ROAD_HALF_W, segBank[n], (seg.flags & 0x02) != 0);
        }

        // 3b. 3D Roadside Guardrail Posts on Hard Curves
        if (std::fabs(seg.curve) >= 1.4f && (n % 2 == 0)) {
            float side = (seg.curve > 0.0f) ? -1.08f : +1.08f; // outside of curve
            Vec3 postPos{ p0.wx + side * cfg::ROAD_HALF_W, p0.wy, p0.cz };
            drawBoxMesh3D(pipe, postPos, 1.0f, 0.0f, 1.0f, 0.0f,
                          -22.0f, 0.0f, -90.0f, +22.0f, 190.0f, +90.0f,
                          RGB(220, 225, 235), RGB(180, 190, 205), RGB(255, 55, 50), RGB(220, 225, 235), 0.3f);
        }

        // 3c. 3D Roadside Scenery Meshes (Trees, Pines, Lamps, Signs, Rocks)
        for (int k = 0; k < seg.sceneryCount; ++k) {
            const Scenery& sc = track.scenery[seg.sceneryBegin + k];
            Vec3 scPos{ p0.wx + sc.offset * cfg::ROAD_HALF_W, p0.wy, p0.cz };
            DrawSceneryMesh3D(pipe, scPos, segYaw[n], sc.spriteId, sc.flip != 0);
        }
    }

    // 4. Render 3D Opponent Cars & Semi-Trucks with Depth Testing & Lambertian/Specular Shading
    for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) {
        const Car& car = traffic.cars[i];
        if (!car.active) continue;

        float zNorm = mathutil::wrap(car.z, track.length) / cfg::SEG_LEN;
        int segIdx  = static_cast<int>(std::floor(zNorm)) % track.count;
        int n       = (segIdx - ctx.baseSeg + track.count) % track.count;
        if (n < 1 || n >= maxMeshDist) continue;

        float t = zNorm - std::floor(zNorm);
        const RoadPoint& p0 = ctx.pts[n];
        const RoadPoint& p1 = ctx.pts[n + 1];

        float cz = (1.0f - t) * p0.cz + t * p1.cz;
        if (cz < pipe.zNear + 80.0f) continue;

        float wx = (1.0f - t) * p0.wx + t * p1.wx + car.offset * cfg::ROAD_HALF_W;
        float wy = (1.0f - t) * p0.wy + t * p1.wy;
        float yaw = mathutil::lerp(segYaw[n], segYaw[n + 1], t);
        float bnk = mathutil::lerp(segBank[n], segBank[n + 1], t);

        float dz = mathutil::wrapDist(car.z, vehicle.z, track.length);
        float brakeGlow = (dz > 0.0f && dz < 1800.0f) ? 1.0f : 0.0f;

        Vec3 carCamPos{ wx, wy, cz };
        if (car.spriteId == SPR_TRUCK) {
            DrawTruckMesh3D(pipe, carCamPos, yaw, bnk);
        } else {
            DrawOpponentCarMesh3D(pipe, carCamPos, yaw, bnk, car.spriteId, brakeGlow);
        }
    }
}
