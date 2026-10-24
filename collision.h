#pragma once
#include <cstdint>
#include "vehicle.h"

struct CollisionEvent {
    enum Kind : uint8_t {
        None = 0,
        Scenery,
        Traffic,
        Wall
    } kind = None;
    float severity = 0.0f;   // 0..1
    float dir      = 0.0f;   // +-1: which way to push the player laterally
    int   carIndex = -1;     // index of traffic car hit (if kind == Traffic)
};

CollisionEvent detectCollision(const Vehicle& v, float prevZ, float prevX,
                               const Track& track, const SpriteAtlas& atlas, const Traffic& traffic);
