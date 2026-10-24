#pragma once
#include <cstdint>
#include "config.h"

namespace curve {
inline constexpr float NONE = 0.0f;
inline constexpr float EASY = 0.6f;
inline constexpr float MED  = 1.5f;
inline constexpr float HARD = 3.0f;
} // namespace curve

namespace hill {
inline constexpr float NONE = 0.0f;
inline constexpr float LOW  = 600.0f;
inline constexpr float MED  = 1200.0f;
inline constexpr float HIGH = 2400.0f;
} // namespace hill

inline constexpr int NUM_TRACK_PRESETS = 5;

struct TrackMetadata {
    const char* name;
    const char* subtitle;
    const char* difficulty;
    int         defaultTheme;
    int         defaultWeather; // 0 = Clear, 1 = Rain, 2 = Snow
    float       trafficSpeedMult;
};

const TrackMetadata& getTrackMetadata(int presetIdx);

struct alignas(16) Segment {            // 16 bytes: 4 per cache line
    float    y0 = 0.0f;                 // world elevation at start
    float    y1 = 0.0f;                 // world elevation at end
    float    curve = 0.0f;              // lateral acceleration
    uint16_t sceneryBegin = 0;          // index into Track::scenery
    uint8_t  sceneryCount = 0;
    uint8_t  flags = 0;                 // bit 0: checkpoint, bit 1: start/finish banner
};
static_assert(sizeof(Segment) == 16, "Segment must be exactly 16 bytes");

struct Scenery {                        // 8 bytes, stored sorted by segment in Track::scenery[]
    float    offset = 0.0f;             // normalised lateral: +-1 = road edge
    uint16_t spriteId = 0;
    uint8_t  flip = 0;
    uint8_t  pad = 0;
};
static_assert(sizeof(Scenery) == 8, "Scenery must be 8 bytes");

struct Track {
    alignas(64) Segment segs[cfg::MAX_SEGMENTS];
    alignas(64) Scenery scenery[cfg::MAX_SCENERY];
    // Precomputed 2D GPS spline coordinates (normalized in [-1, 1]) & heading (radians)
    alignas(64) float   mapX[cfg::MAX_SEGMENTS];
    alignas(64) float   mapY[cfg::MAX_SEGMENTS];
    alignas(64) float   mapHeading[cfg::MAX_SEGMENTS];
    int   count = 0;                    // must be a multiple of 2 * RUMBLE_LEN
    float length = 0.0f;                // count * SEG_LEN
    int   sceneryTotal = 0;
    int   presetId = 0;
};

class TrackBuilder {
public:
    explicit TrackBuilder(Track& t);
    void straight(int n);
    void road(int enter, int hold, int leave, float curveVal, float dy);
    void sCurve(float c);
    void rollingHills(int n, float amp);
    void finish();
private:
    Track& t_;
    float  lastY_ = 0.0f;
    void add(float curveVal, float y1);
};

void buildStraightTrack(Track& t, int count);
void buildDemoTrack(Track& t);
void buildTrackPreset(Track& t, int presetIdx);
void populateScenery(Track& t, uint32_t seed = 0xC0FFEE01u);
void buildTrackGpsSpline(Track& t);
void scanUpcomingTurn(const Track& t, int baseSeg, float& outPeakCurve, int& outDistSegs, bool& outCrest);
