#pragma once
#include <cstdint>

namespace cfg {
inline constexpr int   SCREEN_W         = 320;
inline constexpr int   SCREEN_H         = 240;
inline constexpr int   WORLD_ROWS       = 200;      // rows 0..199 = 3D world; 200..239 hidden under dashboard
inline constexpr int   HORIZON_BASE     = 90;       // horizon row at zero pitch
inline constexpr float FOCAL_PX         = 160.0f;   // (SCREEN_W/2)/tan(45deg) -> 90 deg horizontal FOV
inline constexpr float SEG_LEN          = 200.0f;   // world units (wu) per road segment
inline constexpr int   DRAW_DIST        = 200;      // segments projected per frame
inline constexpr float ROAD_HALF_W      = 1200.0f;  // wu, centre to edge
inline constexpr int   LANES            = 3;
inline constexpr float CAM_HEIGHT       = 1000.0f;  // wu above road surface
inline constexpr int   RUMBLE_LEN       = 3;        // segments per stripe band
inline constexpr int   MAX_SEGMENTS     = 4096;
inline constexpr int   MAX_SCENERY      = 4096;
inline constexpr int   MAX_TRAFFIC      = 24;
inline constexpr int   MAX_DRAW_SPRITES = 512;
inline constexpr float STEP             = 1.0f / 60.0f;
inline constexpr float MAX_SPEED        = SEG_LEN / STEP;   // 12000 wu/s = exactly 1 segment per tick
inline constexpr float KMH_PER_WU       = 0.025f;           // 12000 wu/s displays as 300 km/h
inline constexpr int   NUM_SLICES       = 8;                // row slices for Phase 8 (25 rows per slice)
} // namespace cfg

constexpr uint32_t RGB(uint32_t r, uint32_t g, uint32_t b) {
    return 0xFF000000u | ((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) | (b & 0xFFu);
}

constexpr uint32_t ARGB(uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
    return ((a & 0xFFu) << 24) | ((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) | (b & 0xFFu);
}
