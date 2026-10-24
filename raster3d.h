#pragma once
#include <cstdint>
#include <cmath>
#include "config.h"
#include "framebuffer.h"
#include "track.h"
#include "camera.h"
#include "roadrender.h"
#include "sprites.h"
#include "cockpit.h"
#include "vehicle.h"

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    constexpr Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
    constexpr Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    constexpr Vec3 operator*(float s) const       { return { x * s, y * s, z * s }; }
};

inline constexpr float dot3(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline constexpr Vec3 cross3(const Vec3& a, const Vec3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

inline Vec3 normalize3(const Vec3& v) {
    float len2 = dot3(v, v);
    if (len2 <= 1e-12f) return { 0.0f, 1.0f, 0.0f };
    float inv = 1.0f / std::sqrt(len2);
    return { v.x * inv, v.y * inv, v.z * inv };
}

struct Vertex3D {
    Vec3     pos;       // 3D position (camera space or world space)
    uint32_t color;     // base surface color (0xFFRRGGBB)
    float    light = 1.0f; // Gouraud per-vertex light multiplier
};

// Modular Software 3D / Pseudo-3D Depth-Buffered Raster Pipeline
// Operates at native 960x720 (DisplayBuffer) or 320x240 (Framebuffer) with zero heap allocations
struct RasterPipeline {
    static constexpr int MAX_W = DisplayBuffer::W; // 960
    static constexpr int MAX_H = DisplayBuffer::H; // 720

    int       width      = MAX_W;
    int       height     = MAX_H;
    uint32_t* colorBuf   = nullptr;
    alignas(64) float depthBuffer[MAX_W * MAX_H];  // Per-pixel Z-buffer (camera-space depth Z in world units)

    // Camera intrinsics & view parameters
    float fx      = 480.0f;   // horizontal focal length in pixels
    float fy      = 480.0f;   // vertical focal length in pixels
    float cx      = 480.0f;   // optical center X
    float cy      = 270.0f;   // horizon optical center Y
    float zNear   = 120.0f;   // near clipping plane distance (wu)
    float camRoll = 0.0f;     // camera banking / roll angle (radians)

    // Atmospheric & lighting parameters
    Vec3     lightDir   = { -0.42f, 0.78f, -0.46f }; // normalized sun/sky directional light
    float    ambient    = 0.38f;
    float    diffuse    = 0.68f;
    float    fogDensity = 0.000072f;                 // exponential fog density
    uint32_t fogColor   = RGB(170, 200, 230);
    int      theme      = 0;
    uint64_t frameTick  = 0;

    void bind(uint32_t* targetPixels, int w, int h, float horizonY, float shiftXPx);
    void clearDepth(float infVal = 1e30f);

    // Exponential distance fog: factor = exp(-fogDensity * z), lerp(fogColor, surfaceColor, factor)
    uint32_t applyExponentialFog(uint32_t surfaceColor, float z) const;

    // Lambertian + Specular surface shading: I = ambient + diffuse * max(0, N . L)
    uint32_t shadeLambert(uint32_t baseColor, const Vec3& normal, float specStrength = 0.15f) const;

    // Perspective projection of a camera-space 3D point (X, Y, Z) -> (screenX, screenY)
    // X_screen = (X / Z) * fx + cx,  Y_screen = cy - (Y / Z) * fy
    inline bool projectCameraPoint(const Vec3& camPt, float& outSx, float& outSy) const {
        if (camPt.z <= 1e-3f) return false;
        float invZ = 1.0f / camPt.z;
        outSx = (camPt.x * invZ) * fx + cx;
        outSy = cy - (camPt.y * invZ) * fy;
        return true;
    }
};

// Renders a 3D road trapezoid segment with road banking/camber, per-pixel Z-buffer writes,
// analytical box-filtered anti-aliased striping (preventing horizon moire shimmer), and exponential fog
void DrawTrapezoid(RasterPipeline& pipe,
                   const Vec3& nearCenter, float nearHalfW, float nearBankSlope,
                   const Vec3& farCenter,  float farHalfW,  float farBankSlope,
                   const RoadPalette& palBand0, const RoadPalette& palBand1,
                   int absSegIndex, uint8_t segFlags, int clipMaxY);

// Renders a 3D triangle in camera space with near-plane (Z > zNear) clipping,
// perspective-correct 1/Z interpolation, Z-buffer depth test (z < depthBuffer[y*W+x]),
// Lambert flat/Gouraud shading (N . L), and exponential distance fog
void DrawTriangle3D(RasterPipeline& pipe,
                    const Vertex3D& v0, const Vertex3D& v1, const Vertex3D& v2,
                    bool doubleSided = false, float specStrength = 0.18f);

// Convenience quad wrapper splitting into two 3D depth-tested triangles
void DrawQuad3D(RasterPipeline& pipe,
                const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3,
                uint32_t baseColor, bool doubleSided = false, float specStrength = 0.18f);

// Procedural Low-Poly 3D Mesh Renderers (Opponent Cars, Trucks, Trees, Guardrails, Lamps, Signs, Arches)
void DrawOpponentCarMesh3D(RasterPipeline& pipe, const Vec3& baseCamPos, float yawRad, float bankRad,
                           uint16_t spriteId, float brakeGlow);
void DrawTruckMesh3D(RasterPipeline& pipe, const Vec3& baseCamPos, float yawRad, float bankRad);
void DrawSceneryMesh3D(RasterPipeline& pipe, const Vec3& baseCamPos, float yawRad,
                       uint16_t spriteId, bool flipSide);
void DrawCheckpointArchMesh3D(RasterPipeline& pipe, const Vec3& centerCamPos, float roadHalfW,
                              float bankSlope, bool isStartFinish);

// Forward-pass 3D-shaded First-Person Cockpit & Steering Wheel overlay (depth testing disabled / near-plane)
void RenderCockpit3D(RasterPipeline& pipe, const Vehicle& vehicle, const Cockpit& cockpit,
                     const CarSpec& carSpec, bool wipersOn, float wiperPhase, int weatherMode);

// Clean top-level RenderFrame() hook connecting Vehicle, Camera, Track, and Traffic into the 3D RasterPipeline
void RenderFrame(RasterPipeline& pipe,
                 const Track& track,
                 const Vehicle& vehicle,
                 const Traffic& traffic,
                 const Camera& cam,
                 const RenderContext& ctx,
                 const Cockpit& cockpit,
                 DisplayBuffer& outDisplay);
