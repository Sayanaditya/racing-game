#pragma once
#include "config.h"

struct Camera {
    float x = 0.0f;
    float y = cfg::CAM_HEIGHT;
    float z = 0.0f;         // world; z wrapped into [0, track.length)
    float pitchPx = 0.0f;   // vertical shift of horizon, pixels (+ = horizon lower)
    float shiftX = 0.0f;    // horizontal image shift in pixels (shake, sway); added to W/2 in projection
};
