#include "track.h"
#include "sprites.h"
#include "mathutil.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace {

const TrackMetadata TRACK_META[NUM_TRACK_PRESETS] = {
    { "EMERALD COAST",   "SUNLIT COASTAL HIGHWAY",    "NORMAL",   0, 0, 1.00f },
    { "NEON MIRAGE",     "SYNTHWAVE SUNSET FREEWAY",  "FAST",     1, 0, 1.08f },
    { "CYBER TOKYO",     "MIDNIGHT EXPRESSWAY",       "HARD",     2, 1, 1.12f },
    { "ALPINE GLACIER",  "SNOWY MOUNTAIN SWITCHBACKS","EXPERT",   3, 2, 1.05f },
    { "CRIMSON CANYON",  "DESERT ENDURANCE GAUNTLET", "EXTREME",  4, 0, 1.18f }
};

} // namespace

const TrackMetadata& getTrackMetadata(int presetIdx) {
    int idx = ((presetIdx % NUM_TRACK_PRESETS) + NUM_TRACK_PRESETS) % NUM_TRACK_PRESETS;
    return TRACK_META[idx];
}

TrackBuilder::TrackBuilder(Track& t) : t_(t), lastY_(0.0f) {
    std::memset(&t_, 0, sizeof(Track));
}

void TrackBuilder::add(float curveVal, float y1) {
    if (t_.count >= cfg::MAX_SEGMENTS) return;
    Segment& s = t_.segs[t_.count++];
    s.y0 = lastY_;
    s.y1 = y1;
    s.curve = curveVal;
    s.sceneryBegin = 0;
    s.sceneryCount = 0;
    s.flags = 0;
    lastY_ = y1;
    t_.length = static_cast<float>(t_.count) * cfg::SEG_LEN;
}

void TrackBuilder::straight(int n) {
    road(n, 0, 0, 0.0f, 0.0f);
}

void TrackBuilder::road(int enter, int hold, int leave, float curveVal, float dy) {
    int total = enter + hold + leave;
    if (total <= 0) return;
    float startY = lastY_;
    float endY   = startY + dy;

    for (int i = 0; i < total; ++i) {
        float c = 0.0f;
        if (i < enter) {
            float p = (enter > 0) ? (static_cast<float>(i) / static_cast<float>(enter)) : 1.0f;
            c = mathutil::easeIn(0.0f, curveVal, p);
        } else if (i < enter + hold) {
            c = curveVal;
        } else {
            float p = (leave > 0) ? (static_cast<float>(i - enter - hold) / static_cast<float>(leave)) : 1.0f;
            c = mathutil::easeInOut(curveVal, 0.0f, p);
        }
        float py = static_cast<float>(i + 1) / static_cast<float>(total);
        float y1 = mathutil::easeInOut(startY, endY, py);
        add(c, y1);
    }
}

void TrackBuilder::sCurve(float c) {
    road(20, 20, 20, +c, 0.0f);
    road(20, 20, 20, -c, 0.0f);
}

void TrackBuilder::rollingHills(int n, float amp) {
    if (n <= 0) return;
    float startY = lastY_;
    for (int i = 0; i < n; ++i) {
        float phase = mathutil::TWO_PI * static_cast<float>(i + 1) / static_cast<float>(n);
        float y1 = startY + amp * std::sin(phase);
        add(0.0f, y1);
    }
    if (t_.count > 0) {
        t_.segs[t_.count - 1].y1 = startY;
        lastY_ = startY;
    }
}

void TrackBuilder::finish() {
    float startY = (t_.count > 0) ? t_.segs[0].y0 : 0.0f;
    if (std::fabs(lastY_ - startY) > 1e-3f) {
        int closeSegs = 24;
        road(closeSegs / 3, closeSegs / 3, closeSegs - 2 * (closeSegs / 3), 0.0f, startY - lastY_);
    }
    const int mult = 2 * cfg::RUMBLE_LEN;
    int rem = t_.count % mult;
    if (rem != 0) {
        int pad = mult - rem;
        for (int i = 0; i < pad; ++i) {
            add(0.0f, startY);
        }
    }
    if (t_.count > 0) {
        t_.segs[t_.count - 1].y1 = startY;
        lastY_ = startY;
        // Start/Finish banner at segment 0, checkpoints at 1/3 and 2/3 of track
        t_.segs[0].flags |= 0x03;
        if (t_.count >= 180) {
            t_.segs[t_.count / 3].flags |= 0x01;
            t_.segs[(2 * t_.count) / 3].flags |= 0x01;
        }
    }
    t_.length = static_cast<float>(t_.count) * cfg::SEG_LEN;
    buildTrackGpsSpline(t_);
}

void buildTrackGpsSpline(Track& t) {
    if (t.count <= 0) return;
    const double baseStep = mathutil::TWO_PI / static_cast<double>(t.count);
    const double curveScale = 0.0085;

    double angle = -0.5 * mathutil::PI; // start pointing North (Up)
    double curX = 0.0, curY = 0.0;

    for (int i = 0; i < t.count; ++i) {
        t.mapX[i] = static_cast<float>(curX);
        t.mapY[i] = static_cast<float>(curY);
        t.mapHeading[i] = static_cast<float>(angle);
        double dTheta = baseStep + static_cast<double>(t.segs[i].curve) * curveScale;
        angle += dTheta;
        curX  += std::cos(angle);
        curY  += std::sin(angle);
    }

    // Distribute endpoint residual linearly so the 2D loop closes seamlessly at segment 0
    double errX = curX;
    double errY = curY;
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;

    for (int i = 0; i < t.count; ++i) {
        double frac = static_cast<double>(i) / static_cast<double>(t.count);
        t.mapX[i] -= static_cast<float>(errX * frac);
        t.mapY[i] -= static_cast<float>(errY * frac);
        minX = std::min(minX, t.mapX[i]);
        maxX = std::max(maxX, t.mapX[i]);
        minY = std::min(minY, t.mapY[i]);
        maxY = std::max(maxY, t.mapY[i]);
    }

    float cx = 0.5f * (minX + maxX);
    float cy = 0.5f * (minY + maxY);
    float span = std::max({ maxX - minX, maxY - minY, 1.0f }) * 0.5f;

    for (int i = 0; i < t.count; ++i) {
        t.mapX[i] = (t.mapX[i] - cx) / span;
        t.mapY[i] = (t.mapY[i] - cy) / span;
    }
    // Recompute exact tangent heading from closed spline points
    for (int i = 0; i < t.count; ++i) {
        int next = (i + 1) % t.count;
        t.mapHeading[i] = std::atan2(t.mapY[next] - t.mapY[i], t.mapX[next] - t.mapX[i]);
    }
}

void scanUpcomingTurn(const Track& t, int baseSeg, float& outPeakCurve, int& outDistSegs, bool& outCrest) {
    outPeakCurve = 0.0f;
    outDistSegs  = 0;
    outCrest     = false;
    if (t.count <= 0) return;

    float maxAbs = 0.0f;
    for (int d = 4; d <= 55; ++d) {
        const Segment& s = t.segs[(baseSeg + d) % t.count];
        if (std::fabs(s.curve) > maxAbs) {
            maxAbs       = std::fabs(s.curve);
            outPeakCurve = s.curve;
            outDistSegs  = d;
        }
        if (d <= 30) {
            const Segment& sNext = t.segs[(baseSeg + d + 1) % t.count];
            float slope0 = s.y1 - s.y0;
            float slope1 = sNext.y1 - sNext.y0;
            if (slope0 > 12.0f && slope1 < -5.0f) {
                outCrest = true;
            }
        }
    }
}

void buildStraightTrack(Track& t, int count) {
    assert(count % (2 * cfg::RUMBLE_LEN) == 0);
    TrackBuilder b(t);
    b.straight(count);
    b.finish();
}

void buildDemoTrack(Track& t) {
    buildTrackPreset(t, 0);
}

void buildTrackPreset(Track& t, int presetIdx) {
    int idx = ((presetIdx % NUM_TRACK_PRESETS) + NUM_TRACK_PRESETS) % NUM_TRACK_PRESETS;
    TrackBuilder b(t);
    t.presetId = idx;

    if (idx == 0) {
        // Track 0: Emerald Coast Highway (Authoritative Demo Track so unit tests match)
        b.straight(30);
        b.road(25, 35, 25, +curve::EASY, hill::NONE);
        b.rollingHills(60, hill::LOW);
        b.sCurve(curve::MED);
        b.road(30, 40, 30, -curve::MED, +hill::MED);
        b.road(20, 30, 20, +curve::HARD, -hill::MED);
        b.straight(36);
        b.road(30, 30, 30, +curve::MED, +hill::HIGH);
        b.road(30, 30, 30, -curve::EASY, -hill::HIGH);
        b.sCurve(curve::HARD);
        b.road(25, 25, 25, -curve::MED, +hill::LOW);
        b.road(20, 14, 20, 0.0f, -hill::LOW);
        b.finish();
        populateScenery(t, 0xC0FFEE01u);
    } else if (idx == 1) {
        // Track 1: Neon Mirage Speedway (High-speed Synthwave Sunset freeway)
        b.straight(45);
        b.road(20, 40, 20, +curve::EASY, +hill::LOW);
        b.straight(35);
        b.sCurve(curve::MED);
        b.sCurve(curve::MED);
        b.road(25, 50, 25, -curve::HARD, -hill::LOW);
        b.rollingHills(72, hill::MED);
        b.straight(40);
        b.road(30, 45, 30, +curve::MED, +hill::HIGH);
        b.road(30, 45, 30, -curve::MED, -hill::HIGH);
        b.sCurve(curve::EASY);
        b.straight(30);
        b.finish();
        populateScenery(t, 0xCAFE2026u);
    } else if (idx == 2) {
        // Track 2: Cyber Tokyo Expressway (Tight technical night circuit)
        b.straight(24);
        b.sCurve(curve::HARD);
        b.road(15, 30, 15, +curve::HARD, +hill::MED);
        b.road(15, 25, 15, -curve::HARD, -hill::MED);
        b.straight(20);
        b.sCurve(curve::MED);
        b.road(20, 40, 20, -curve::HARD, +hill::HIGH);
        b.road(20, 35, 20, +curve::MED, -hill::HIGH);
        b.rollingHills(48, hill::LOW);
        b.sCurve(curve::HARD);
        b.road(18, 30, 18, +curve::HARD, 0.0f);
        b.finish();
        populateScenery(t, 0x708090A0u);
    } else if (idx == 3) {
        // Track 3: Alpine Glacier Pass (Steep mountain switchbacks & crests)
        b.straight(24);
        b.road(30, 50, 30, +curve::MED, +hill::HIGH);
        b.road(20, 35, 20, -curve::HARD, +hill::MED);
        b.rollingHills(60, hill::MED);
        b.road(20, 40, 20, +curve::HARD, -hill::HIGH);
        b.sCurve(curve::HARD);
        b.road(25, 40, 25, -curve::MED, -hill::MED);
        b.rollingHills(48, hill::LOW);
        b.road(20, 30, 20, +curve::HARD, +hill::LOW);
        b.road(20, 20, 20, -curve::EASY, -hill::LOW);
        b.finish();
        populateScenery(t, 0x5100ACE5u);
    } else {
        // Track 4: Crimson Canyon Gauntlet (Extreme high-speed endurance)
        b.straight(36);
        b.road(20, 45, 20, -curve::HARD, +hill::HIGH);
        b.road(15, 30, 15, +curve::HARD, -hill::HIGH);
        b.sCurve(curve::HARD);
        b.rollingHills(72, hill::MED);
        b.road(25, 50, 25, +curve::HARD, +hill::HIGH);
        b.sCurve(curve::MED);
        b.road(25, 50, 25, -curve::HARD, -hill::HIGH);
        b.sCurve(curve::HARD);
        b.straight(24);
        b.finish();
        populateScenery(t, 0xDEAD10CCu);
    }
}

void populateScenery(Track& t, uint32_t seed) {
    uint32_t rng = seed ? seed : 0xC0FFEE01u;
    t.sceneryTotal = 0;

    for (int i = 0; i < t.count; ++i) {
        Segment& seg = t.segs[i];
        seg.sceneryBegin = static_cast<uint16_t>(t.sceneryTotal);
        seg.sceneryCount = 0;

        auto emitScenery = [&](float offset, uint16_t spriteId, uint8_t flip) {
            if (seg.sceneryCount >= 3) return;
            if (t.sceneryTotal >= cfg::MAX_SCENERY) return;
            Scenery& sc = t.scenery[t.sceneryTotal++];
            sc.offset = offset;
            sc.spriteId = spriteId;
            sc.flip = flip;
            sc.pad = 0;
            seg.sceneryCount++;
        };

        // 1. Lamp posts: both sides at offset = +-1.15, every 12 segments (flip = 1 for left side)
        if (i % 12 == 0) {
            emitScenery(-1.15f, SPR_LAMP, 1);
            emitScenery(+1.15f, SPR_LAMP, 0);
        }

        // 2. Billboard signs: every 60 segments at offset = +-1.35 (plus checkpoint pillars)
        if (i % 60 == 30) {
            float side = ((i / 60) & 1) ? +1.35f : -1.35f;
            emitScenery(side, SPR_SIGN, (side < 0.0f) ? 1 : 0);
        }

        // 3. Trees / pines: probability 0.18, offset = sign * (1.5 + 2.0 * rand01); at most 3 items/seg
        if (mathutil::rand01(rng) < 0.18f) {
            float sign = (mathutil::xorshift32(rng) & 1u) ? +1.0f : -1.0f;
            float off  = sign * (1.5f + 2.0f * mathutil::rand01(rng));
            uint16_t spr = (mathutil::xorshift32(rng) & 1u) ? SPR_TREE : SPR_PINE;
            uint8_t flip = static_cast<uint8_t>(mathutil::xorshift32(rng) & 1u);
            emitScenery(off, spr, flip);
        }

        // 4. Bushes / rocks: probability 0.08, offset = sign * (1.15 + 0.6 * rand01)
        if (mathutil::rand01(rng) < 0.08f) {
            float sign = (mathutil::xorshift32(rng) & 1u) ? +1.0f : -1.0f;
            float off  = sign * (1.15f + 0.6f * mathutil::rand01(rng));
            uint16_t spr = (mathutil::xorshift32(rng) & 1u) ? SPR_BUSH : SPR_ROCK;
            uint8_t flip = static_cast<uint8_t>(mathutil::xorshift32(rng) & 1u);
            emitScenery(off, spr, flip);
        }
    }
}
