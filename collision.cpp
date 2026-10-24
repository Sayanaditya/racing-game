#include "collision.h"
#include "mathutil.h"
#include <cmath>
#include <algorithm>

namespace {

inline constexpr bool SOLID_SPRITE[SPR_COUNT] = {
    true,  // SPR_TREE
    true,  // SPR_PINE
    false, // SPR_BUSH (not solid)
    true,  // SPR_LAMP
    true,  // SPR_SIGN
    true,  // SPR_ROCK
    true,  // SPR_CAR_RED
    true,  // SPR_CAR_BLUE
    true,  // SPR_CAR_YELLOW
    true   // SPR_TRUCK
};

inline constexpr float TRUNK_FRAC[SPR_COUNT] = {
    0.30f, // SPR_TREE
    0.25f, // SPR_PINE
    0.00f, // SPR_BUSH
    0.50f, // SPR_LAMP
    0.15f, // SPR_SIGN
    0.80f, // SPR_ROCK
    1.00f, // SPR_CAR_RED
    1.00f, // SPR_CAR_BLUE
    1.00f, // SPR_CAR_YELLOW
    1.00f  // SPR_TRUCK
};

inline float signNonZero(float x) {
    return (x >= 0.0f) ? +1.0f : -1.0f;
}

// Swept 2D segment-vs-AABB test in (x, r) relative space
bool sweptBoxOverlap(float x0, float r0, float x1, float r1,
                     float cx, float lim, float rMin, float rMax, float& midXOut) {
    float t0 = 0.0f, t1 = 1.0f;
    float dr = r1 - r0;
    float dx = x1 - x0;

    auto clipAxis = [&](float p, float q) -> bool {
        if (std::fabs(p) < 1e-7f) {
            return q >= 0.0f;
        }
        float r = q / p;
        if (p < 0.0f) {
            if (r > t1) return false;
            if (r > t0) t0 = r;
        } else {
            if (r < t0) return false;
            if (r < t1) t1 = r;
        }
        return true;
    };

    const float eps = 1e-5f;
    if (!clipAxis(-dr, (r0 - rMin) - eps)) return false;
    if (!clipAxis( dr, (rMax - r0) - eps)) return false;
    if (!clipAxis(-dx, (x0 - (cx - lim)) - eps)) return false;
    if (!clipAxis( dx, ((cx + lim) - x0) - eps)) return false;

    if (t0 > t1) return false;
    float tMid = 0.5f * (t0 + t1);
    midXOut = x0 + dx * tMid;
    return true;
}

} // namespace

CollisionEvent detectCollision(const Vehicle& v, float prevZ, float prevX,
                               const Track& track, const SpriteAtlas& atlas, const Traffic& traffic) {
    CollisionEvent ev;
    const float L = track.length;
    if (L <= 0.0f) return ev;

    const float hbScale  = (v.hitboxScale > 0.1f) ? v.hitboxScale : 1.0f;
    const float playerHW = vphys::PLAYER_HALF_W * hbScale;
    const float playerLen = vphys::PLAYER_LEN * (v.realisticMode ? 0.62f : 1.0f);

    // 1. Traffic: relative sweep in (x, r) space
    for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) {
        const Car& car = traffic.cars[i];
        if (!car.active) continue;

        const SpriteImage& img = atlas.img[car.spriteId];
        float otherLen = img.worldH * (v.realisticMode ? 0.42f : 0.60f);
        float r0  = mathutil::wrapDist(traffic.prevZ[i], prevZ, L);
        float r1  = mathutil::wrapDist(car.z, v.z, L);
        float carHalfW = (img.worldW / (2.0f * cfg::ROAD_HALF_W)) * hbScale;
        float lim = playerHW + carHalfW;

        float hitX = v.x;
        if (!sweptBoxOverlap(prevX, r0, v.x, r1, car.offset, lim, -otherLen, playerLen, hitX)) {
            continue;
        }

        float dx   = std::fabs(hitX - car.offset);
        float vrel = std::max(800.0f, std::fabs(v.speed - car.speed));
        float f    = mathutil::clamp((lim - dx) / lim, 0.0f, 1.0f);
        ev.kind     = CollisionEvent::Traffic;
        ev.severity = mathutil::clamp((vrel / (0.6f * cfg::MAX_SPEED)) * (0.4f + 0.6f * f), 0.05f, 1.0f);
        ev.dir      = signNonZero(hitX - car.offset);
        ev.carIndex = i;
        return ev;
    }

    // 2. Scenery: only segments crossed this tick
    float zUnwrapped = v.z;
    if (v.z < prevZ && (prevZ - v.z) > 0.5f * L) {
        zUnwrapped += L;
    }
    int sA = static_cast<int>(std::floor(prevZ / cfg::SEG_LEN));
    int sB = static_cast<int>(std::floor(zUnwrapped / cfg::SEG_LEN)) + 1;
    if (sB < sA) std::swap(sA, sB);
    sB = std::min(sB, sA + 4);

    float speedFrac = mathutil::clamp(v.speed / cfg::MAX_SPEED, 0.0f, 1.0f);

    for (int s = sA; s <= sB; ++s) {
        int segIdx = ((s % track.count) + track.count) % track.count;
        const Segment& seg = track.segs[segIdx];
        float segZ = static_cast<float>(segIdx) * cfg::SEG_LEN;
        float rPrev = mathutil::wrapDist(segZ, prevZ, L);
        float rNow  = mathutil::wrapDist(segZ, v.z, L);

        for (int k = 0; k < seg.sceneryCount; ++k) {
            const Scenery& item = track.scenery[seg.sceneryBegin + k];
            if (item.spriteId >= SPR_COUNT || !SOLID_SPRITE[item.spriteId]) continue;

            const SpriteImage& img = atlas.img[item.spriteId];
            float halfN = ((img.worldW * TRUNK_FRAC[item.spriteId]) / (2.0f * cfg::ROAD_HALF_W)) * hbScale;
            float lim   = playerHW + halfN;

            float hitX = v.x;
            if (!sweptBoxOverlap(prevX, rPrev, v.x, rNow, item.offset, lim, -120.0f, playerLen, hitX)) {
                continue;
            }

            float dx = std::fabs(hitX - item.offset);
            float f  = mathutil::clamp((lim - dx) / lim, 0.0f, 1.0f);
            ev.kind     = CollisionEvent::Scenery;
            ev.severity = mathutil::clamp(speedFrac * (0.4f + 0.6f * f), 0.05f, 1.0f);
            ev.dir      = signNonZero(hitX - item.offset);
            return ev;
        }
    }

    // 3. Wall
    if (std::fabs(v.x) >= vphys::WALL_X) {
        ev.kind     = CollisionEvent::Wall;
        ev.severity = speedFrac * 0.5f;
        ev.dir      = -signNonZero(v.x);
        return ev;
    }

    return ev;
}
