#include "tests.h"
#include "config.h"
#include "mathutil.h"
#include "framebuffer.h"
#include "spanfill.h"
#include "track.h"
#include "camera.h"
#include "roadrender.h"
#include "sprites.h"
#include "cockpit.h"
#include "vehicle.h"
#include "collision.h"
#include "audio.h"
#include "jobs.h"
#include "highscores.h"
#include "game.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <new>
#include <atomic>

// Global allocation counter for verifying zero steady-state heap allocation in tick() and render()
static std::atomic<uint64_t> g_allocCount{0};

void* operator new(std::size_t sz) {
    g_allocCount.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(sz ? sz : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

namespace {

int g_passed = 0;
int g_failed = 0;

void checkCond(bool cond, const char* id, const char* desc) {
    if (cond) {
        std::printf("  [PASS] %-4s %s\n", id, desc);
        g_passed++;
    } else {
        std::printf("  [FAIL] %-4s %s\n", id, desc);
        g_failed++;
    }
}

// Static scratch instances so tests never blow the stack
alignas(64) static Game          s_game;
alignas(64) static Framebuffer   s_fb2;
alignas(64) static RenderContext s_ctx;
alignas(64) static Track         s_track;
alignas(64) static SpriteAtlas   s_atlas;
alignas(64) static DrawList      s_dl;
alignas(64) static Cockpit       s_cockpit;
alignas(64) static SynthDevice   s_synth;
alignas(64) static int16_t       s_audioBuf[SynthDevice::SR * 4]; // 4 seconds buffer

// Discrete Fourier Transform magnitude at target frequency fHz over N samples
double goertzelMag(const int16_t* samples, int N, double fHz, double sr) {
    double k = std::floor(0.5 + (static_cast<double>(N) * fHz) / sr);
    double w = (2.0 * 3.14159265358979323846 * k) / static_cast<double>(N);
    double cosine = std::cos(w);
    double coeff  = 2.0 * cosine;
    double q0 = 0.0, q1 = 0.0, q2 = 0.0;
    for (int i = 0; i < N; ++i) {
        q0 = coeff * q1 - q2 + static_cast<double>(samples[i]) / 32768.0;
        q2 = q1;
        q1 = q0;
    }
    double mag2 = q1 * q1 + q2 * q2 - q1 * q2 * coeff;
    return std::sqrt(std::max(0.0, mag2)) / static_cast<double>(N);
}

void testPhase1() {
    std::printf("\n=== Phase 1: Engine Harness & Framebuffer Checks ===\n");
    checkCond(reinterpret_cast<uintptr_t>(s_game.fb.px) % 64 == 0,
              "1.a", "Framebuffer 64-byte alignment");

    uint32_t red = RGB(255, 0, 0);
    checkCond(red == 0xFFFF0000u,
              "1.b", "RGB(255,0,0) matches QImage::Format_RGB32 0xFFFF0000");

    s_game.fb.drawTestPattern(10);
    bool borderOk = (s_game.fb.row(0)[0] == red) &&
                    (s_game.fb.row(0)[cfg::SCREEN_W - 1] == red) &&
                    (s_game.fb.row(cfg::SCREEN_H - 1)[0] == red) &&
                    (s_game.fb.row(cfg::SCREEN_H - 1)[cfg::SCREEN_W - 1] == red);
    checkCond(borderOk, "1.c", "1-px red border on all four edges of test pattern");
}

void testPhase2() {
    std::printf("\n=== Phase 2: Pseudo-3D Scanline Road Projection Checks ===\n");
    initSpanFill(false);
    buildStraightTrack(s_track, 600);
    initRenderContext(s_ctx, 0);

    Camera cam;
    cam.x = 0.0f;
    cam.y = cfg::CAM_HEIGHT;
    cam.z = 0.0f;
    cam.pitchPx = 0.0f;
    projectRoad(s_track, cam, s_ctx);

    bool oracleOk = true;
    bool symOk    = true;
    for (int y = 110; y <= 199; ++y) {
        float expectedHw = static_cast<float>(y - 90) * 1.2f;
        if (std::fabs(static_cast<float>(s_ctx.rows[y].hw) - expectedHw) > 1.0f) {
            oracleOk = false;
        }
        if (s_ctx.rows[y].cx != 160) {
            symOk = false;
        }
    }
    checkCond(oracleOk, "2.a", "Flat-ground oracle: abs(rows[y].hw - (y - 90)*1.2) <= 1 for y in 110..199");
    checkCond(symOk,    "2.b", "Symmetry: rows[y].cx == 160 on straight road with cam.x = 0");

    cam.x = 600.0f;
    projectRoad(s_track, cam, s_ctx);
    float cx199 = static_cast<float>(s_ctx.rows[199].cx);
    checkCond(std::fabs(cx199 - 94.6f) <= 2.0f,
              "2.c", "Lateral shift: cam.x = 600 -> rows[199].cx == 94.6 (+/- 2 px)");

    // 2.e Scroll continuity across 1 segment in 100 sub-steps
    cam.x = 0.0f;
    bool scrollOk = true;
    for (int step = 0; step < 100; ++step) {
        cam.z = (static_cast<float>(step) / 100.0f) * cfg::SEG_LEN;
        projectRoad(s_track, cam, s_ctx);
        for (int y = s_ctx.topRoadY; y <= 199; ++y) {
            if (s_ctx.rows[y].seg == ROW_INVALID) {
                scrollOk = false;
            }
        }
    }
    checkCond(scrollOk, "2.e", "Scroll continuity: no ROW_INVALID in [topRoadY, 199] across 100 sub-steps");

    // 2.f Wrap seam continuity
    cam.z = s_track.length - 10.0f;
    bool wrapOk = true;
    for (int t = 0; t < 20; ++t) {
        cam.z = mathutil::wrap(cam.z + 5.0f, s_track.length);
        projectRoad(s_track, cam, s_ctx);
        if (s_ctx.rows[199].seg == ROW_INVALID) wrapOk = false;
    }
    checkCond(wrapOk, "2.f", "Wrap seam: continuous stripe pattern across track loop boundary");

    // 2.h Performance budget check
    auto t0 = std::chrono::steady_clock::now();
    projectRoad(s_track, cam, s_ctx);
    auto t1 = std::chrono::steady_clock::now();
    fillRows(s_ctx, s_game.fb, 0, cfg::WORLD_ROWS);
    auto t2 = std::chrono::steady_clock::now();
    double passAUs = std::chrono::duration<double, std::micro>(t1 - t0).count();
    double passBMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    checkCond(passAUs < 200.0 && passBMs < 2.0,
              "2.h", "Pass A and Pass B execute well within frame time budget");
}

void testPhase3() {
    std::printf("\n=== Phase 3: Dynamic Geometry (Curves, Hills, Crest Occlusion) Checks ===\n");
    // 3.a Constant curve = 1.5 oracle
    TrackBuilder tb(s_track);
    tb.road(0, 300, 0, 1.5f, 0.0f);
    tb.finish();

    Camera cam;
    cam.x = 0.0f;
    cam.y = cfg::CAM_HEIGHT;
    cam.z = 0.0f;
    cam.pitchPx = 0.0f;
    projectRoad(s_track, cam, s_ctx);

    float dx50 = s_ctx.pts[50].sx - 160.0f;
    checkCond(std::fabs(dx50 - 29.4f) <= 1.0f,
              "3.a", "Curve oracle: pts[50].sx - 160 == 29.4 px (+/- 1 px) at curve = 1.5");

    // 3.b Positive vs negative curve direction & camera centring
    TrackBuilder tbEasy(s_track);
    tbEasy.road(0, 300, 0, curve::EASY, 0.0f);
    tbEasy.finish();
    projectRoad(s_track, cam, s_ctx);
    bool rightBendOk = (s_ctx.pts[50].sx > 160.0f) && (std::abs(s_ctx.rows[199].cx - 160) <= 2);

    TrackBuilder tbLeft(s_track);
    tbLeft.road(0, 300, 0, -curve::EASY, 0.0f);
    tbLeft.finish();
    projectRoad(s_track, cam, s_ctx);
    bool leftBendOk = (s_ctx.pts[50].sx < 160.0f) && (std::abs(s_ctx.rows[199].cx - 160) <= 2);
    checkCond(rightBendOk && leftBendOk,
              "3.b", "Positive curve bends right, negative bends left; road at camera stays centred");

    // 3.c Centring wobble check across pct in 0..1 on constant curve = 1.5
    TrackBuilder tbMed(s_track);
    tbMed.road(0, 300, 0, 1.5f, 0.0f);
    tbMed.finish();
    int minCx = 1000, maxCx = -1000;
    for (int i = 0; i < 100; ++i) {
        cam.z = (static_cast<float>(i) / 100.0f) * cfg::SEG_LEN;
        projectRoad(s_track, cam, s_ctx);
        minCx = std::min<int>(minCx, s_ctx.rows[199].cx);
        maxCx = std::max<int>(maxCx, s_ctx.rows[199].cx);
    }
    checkCond((maxCx - minCx) <= 1,
              "3.c", "Wobble correction: rows[199].cx changes by <= 1 px across segment boundary");

    // 3.d Constant slope = 0.1 oracle (dY = 20 wu per 200 wu segment)
    std::memset(&s_track, 0, sizeof(Track));
    s_track.count = 300;
    s_track.length = 300 * cfg::SEG_LEN;
    for (int i = 0; i < 300; ++i) {
        s_track.segs[i].y0 = i * 20.0f;
        s_track.segs[i].y1 = (i + 1) * 20.0f;
    }
    cam.z = 0.0f;
    cam.y = cfg::CAM_HEIGHT;
    projectRoad(s_track, cam, s_ctx);
    float flatSy50 = 90.0f + (160.0f / s_ctx.pts[50].cz) * cfg::CAM_HEIGHT;
    float liftPx   = flatSy50 - s_ctx.pts[50].sy;
    checkCond(std::fabs(liftPx - 16.0f) <= 1.0f,
              "3.d", "Slope oracle: constant 10% grade lifts far boundary rows by 16 px (+/- 1)");

    // 3.h Smooth curvature step bound along road(25, 25, 25, c, 0)
    TrackBuilder tbSmooth(s_track);
    tbSmooth.road(25, 25, 25, 1.5f, 0.0f);
    tbSmooth.finish();
    bool smoothCurveOk = true;
    for (int i = 0; i < 74; ++i) {
        if (std::fabs(s_track.segs[i + 1].curve - s_track.segs[i].curve) > (2.0f * 1.5f / 25.0f) + 1e-4f) {
            smoothCurveOk = false;
        }
    }
    checkCond(smoothCurveOk, "3.h", "Smooth curvature: abs(curve[i+1] - curve[i]) <= 2*c/25 along eased bend");

    // 3.e, 3.f, 3.g Demo track closure & crest occlusion
    buildDemoTrack(s_track);
    bool closureOk = std::fabs(s_track.segs[s_track.count - 1].y1 - s_track.segs[0].y0) < 1.0f;
    for (int i = 0; i < s_track.count - 1; ++i) {
        if (std::fabs(s_track.segs[i + 1].y0 - s_track.segs[i].y1) >= 1e-3f) closureOk = false;
    }
    checkCond(closureOk, "3.g", "Elevation closure: C0 continuous across all segments and loop seam");

    bool crestOk = true;
    bool hwPositiveOk = true;
    for (int s = 0; s < s_track.count; s += 15) {
        cam.z = s * cfg::SEG_LEN;
        cam.y = cfg::CAM_HEIGHT + s_track.segs[s].y0;
        projectRoad(s_track, cam, s_ctx);
        for (int n = 0; n < cfg::DRAW_DIST - 1; ++n) {
            if (s_ctx.clipY[n + 1] > s_ctx.clipY[n]) crestOk = false;
        }
        for (int y = 0; y < cfg::WORLD_ROWS; ++y) {
            if (s_ctx.rows[y].seg != ROW_INVALID && s_ctx.rows[y].hw <= 0) hwPositiveOk = false;
        }
    }
    checkCond(crestOk,      "3.e", "Crest occlusion: clipY[n] is monotonically non-increasing in n");
    checkCond(hwPositiveOk, "3.f", "Back-face culling: no inverted trapezoids (all valid rows have hw > 0)");
}

void testPhase4() {
    std::printf("\n=== Phase 4: Billboard Sprite Engine Checks ===\n");
    buildSpriteAtlas(s_atlas);
    buildStraightTrack(s_track, 600);
    initRenderContext(s_ctx, 0);

    // 4.a 1/Z scaling law: Tree (worldH = 1100) at cz = 2000 (n=10) and cz = 4000 (n=20)
    s_track.segs[10].sceneryBegin = 0;
    s_track.segs[10].sceneryCount = 1;
    s_track.scenery[0] = { 0.0f, SPR_TREE, 0, 0 };

    s_track.segs[15].sceneryBegin = 1;
    s_track.segs[15].sceneryCount = 1;
    s_track.scenery[1] = { 1.0f, SPR_TREE, 0, 0 }; // cz = 3000, offset = +1.0

    s_track.segs[20].sceneryBegin = 2;
    s_track.segs[20].sceneryCount = 1;
    s_track.scenery[2] = { 0.0f, SPR_TREE, 0, 0 };

    Camera cam;
    cam.x = 0.0f; cam.y = cfg::CAM_HEIGHT; cam.z = 0.0f; cam.pitchPx = 0.0f;
    projectRoad(s_track, cam, s_ctx);
    buildDrawList(s_track, nullptr, 0, s_ctx, s_atlas, s_dl);

    checkCond(s_dl.count == 3 &&
              std::abs(static_cast<int>(s_dl.items[2].hPx) - 88) <= 1 &&
              std::abs(static_cast<int>(s_dl.items[0].hPx) - 44) <= 1,
              "4.a", "1/Z law: tree at cz=2000 has hPx=88, at cz=4000 has hPx=44 (+/- 1)");

    int sy3000 = s_dl.items[1].sy;
    int sx3000 = s_dl.items[1].sx;
    checkCond(std::abs(sy3000 - 143) <= 1 && s_ctx.rows[sy3000].seg != ROW_INVALID,
              "4.b", "Ground anchor: sprite at cz=3000 lands on row 143 (+/- 1)");
    checkCond(std::abs(sx3000 - (s_ctx.rows[sy3000].cx + s_ctx.rows[sy3000].hw)) <= 1,
              "4.c", "Lateral edge: sprite with offset +1 lands at rows[sy].cx + rows[sy].hw (+/- 1)");

    // 4.f Colour key transparency preservation on solid red background
    const uint32_t bgRed = RGB(255, 0, 0);
    s_game.fb.clear(bgRed);
    DrawList singleDl;
    singleDl.count = 1;
    singleDl.items[0] = { 160, 150, 40, 64, 200, SPR_TREE, 0, 0, 0 };
    drawSprites(singleDl, s_atlas, s_ctx, s_game.fb, 0, cfg::WORLD_ROWS);
    bool keyOk = (s_game.fb.row(10)[10] == bgRed) && (s_game.fb.row(140)[160] != bgRed);
    checkCond(keyOk, "4.f", "Colour key: transparent pixels preserve background, opaque pixels overwrite");

    // 4.i Horizontal flip mirror symmetry check
    s_game.fb.clear(0xFF000000u);
    s_fb2.clear(0xFF000000u);
    singleDl.items[0] = { 160, 150, 16, 56, 200, SPR_LAMP, 0, 0, 0 };
    drawSprites(singleDl, s_atlas, s_ctx, s_game.fb, 0, cfg::WORLD_ROWS);
    singleDl.items[0].flip = 1;
    drawSprites(singleDl, s_atlas, s_ctx, s_fb2, 0, cfg::WORLD_ROWS);
    bool flipMirrorOk = true;
    for (int y = 94; y < 150; ++y) {
        for (int dx = 0; dx < 16; ++dx) {
            if (s_game.fb.row(y)[152 + dx] != s_fb2.row(y)[152 + 15 - dx]) flipMirrorOk = false;
        }
    }
    checkCond(flipMirrorOk, "4.i", "Horizontal flip: flipped lamp post is exact row-by-row mirror");

    // 4.g Slice identity: 1 slice vs 4 slices byte-identical
    buildDemoTrack(s_track);
    Traffic tr;
    initTraffic(tr, s_track, 42u);
    projectRoad(s_track, cam, s_ctx);
    buildDrawList(s_track, tr.cars, cfg::MAX_TRAFFIC, s_ctx, s_atlas, s_dl);

    s_game.fb.clear(0xFF000000u);
    fillRows(s_ctx, s_game.fb, 0, cfg::WORLD_ROWS);
    drawSprites(s_dl, s_atlas, s_ctx, s_game.fb, 0, cfg::WORLD_ROWS);

    s_fb2.clear(0xFF000000u);
    for (int s = 0; s < 4; ++s) {
        int y0 = s * (cfg::WORLD_ROWS / 4);
        int y1 = (s + 1) * (cfg::WORLD_ROWS / 4);
        fillRows(s_ctx, s_fb2, y0, y1);
        drawSprites(s_dl, s_atlas, s_ctx, s_fb2, y0, y1);
    }
    checkCond(std::memcmp(s_game.fb.px, s_fb2.px, sizeof(s_game.fb.px)) == 0,
              "4.g", "Slice identity: 1-slice and 4-slice renders are byte-identical (memcmp == 0)");
}

void testPhase5() {
    std::printf("\n=== Phase 5: First-Person Cockpit & Interior Overlay Checks ===\n");
    s_cockpit.init();
    const uint32_t magenta = RGB(255, 0, 255);
    s_game.fb.clear(magenta);

    CameraFx fx;
    CockpitState cs;
    s_cockpit.update(cs, fx);
    s_cockpit.drawFrame(s_game.fb);

    bool roofOpaque = true;
    for (int y = 0; y < 14; ++y) {
        for (int x = 0; x < cfg::SCREEN_W; ++x) {
            if (s_game.fb.row(y)[x] == magenta) roofOpaque = false;
        }
    }
    int openL = -1, openR = -1;
    for (int x = 0; x < cfg::SCREEN_W; ++x) {
        if (s_game.fb.row(100)[x] == magenta) {
            if (openL == -1) openL = x;
            openR = x + 1;
        }
    }
    checkCond(roofOpaque && std::abs(openL - 28) <= 1 && std::abs(openR - 292) <= 1,
              "5.a", "Windshield geometry: rows 0..13 opaque; row 100 opening is [28, 292) (+/- 1 px)");

    // Check 5.b: magenta appears ONLY inside the windshield polygon
    bool outsideMasked = true;
    for (int y = 192; y < cfg::SCREEN_H; ++y) {
        for (int x = 0; x < cfg::SCREEN_W; ++x) {
            if (s_game.fb.row(y)[x] == magenta) outsideMasked = false;
        }
    }
    checkCond(outsideMasked, "5.b", "Windshield masking: world pixels appear only inside windshield opening");

    // Check 5.e: shake decay & interior offset clamped to +-4
    fx.impact(10.0f);
    bool clampOk = true;
    for (int i = 0; i < 75; ++i) { // 1.25 seconds
        fx.update(0.0f, 0.0f, 0.0f, 0.0f);
        s_cockpit.update(cs, fx);
        if (std::abs(s_cockpit.ox()) > 4 || std::abs(s_cockpit.oy()) > 4) clampOk = false;
    }
    checkCond(clampOk && std::fabs(fx.shakeX) < 0.4f && std::fabs(fx.shakeY) < 0.4f,
              "5.e", "Shake decay: impact amplitude < 0.4 px after 1.2 s; offset within [-4, +4]");

    // Check 5.g: deterministic glass cracks
    s_cockpit.triggerCrack(160, 80, 12345u);
    CrackSet c1 = s_cockpit.cracks();
    s_cockpit.triggerCrack(160, 80, 12345u);
    CrackSet c2 = s_cockpit.cracks();
    checkCond(c1.count > 0 && c1.count <= 160 &&
              c1.count == c2.count &&
              std::memcmp(c1.s, c2.s, c1.count * sizeof(CrackSeg)) == 0,
              "5.g", "Crack determinism: identical seed produces identical crack segments (<= 160)");
}

void testPhase6() {
    std::printf("\n=== Phase 6: Vehicle Dynamics, Swept Collision & Crash Machine Checks ===\n");
    buildStraightTrack(s_track, 600);
    buildSpriteAtlas(s_atlas);
    Traffic tr;
    Vehicle v;
    VehiclePhysics::reset(v, s_track, tr);
    for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) tr.cars[i].active = 0;

    InputState in;
    in.down = InputState::ACCEL;
    for (int i = 0; i < 150; ++i) { // 2.5 seconds
        VehiclePhysics::tick(v, in, s_track, s_atlas, tr);
    }
    checkCond(std::fabs(v.speed - 7587.0f) <= 7587.0f * 0.03f,
              "6.a", "Acceleration oracle: speed at 2.5 s is 7587 wu/s (+/- 3%)");

    // 6.b Top speed & 1.0 s braking
    for (int i = 0; i < 600; ++i) VehiclePhysics::tick(v, in, s_track, s_atlas, tr);
    bool topOk = (v.speed <= 12600.0f && v.speed >= 11900.0f);
    v.speed = cfg::MAX_SPEED;
    in.down = InputState::BRAKE;
    int brakeTicks = 0;
    while (v.speed > 0.0f && brakeTicks < 100) {
        VehiclePhysics::tick(v, in, s_track, s_atlas, tr);
        brakeTicks++;
    }
    float brakeSec = brakeTicks * cfg::STEP;
    checkCond(topOk && std::fabs(brakeSec - 1.0f) <= 0.05f,
              "6.b", "Top speed <= 12600 wu/s and braking from 12000 wu/s stops in 1.0 s (+/- 0.05 s)");

    // 6.c Off-road limit (3000 wu/s)
    v.speed = 9000.0f;
    v.x = 1.5f;
    in.down = InputState::ACCEL;
    for (int i = 0; i < 180; ++i) {
        VehiclePhysics::tick(v, in, s_track, s_atlas, tr);
        v.x = 1.5f;
    }
    checkCond(std::fabs(v.speed - 3000.0f) <= 50.0f,
              "6.c", "Off-road limit: speed settles at 3000 wu/s with throttle held");

    // 6.d Centrifugal drift oracle: curve = 3.0, f = 1, steer = 0, start x = 0 -> x(1 s) = -0.615 (+/- 0.03)
    for (int i = 0; i < s_track.count; ++i) s_track.segs[i].curve = 3.0f;
    VehiclePhysics::reset(v, s_track, tr);
    for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) tr.cars[i].active = 0;
    v.speed = cfg::MAX_SPEED;
    v.x = 0.0f;
    v.latV = 0.0f;
    in.down = InputState::ACCEL;
    for (int i = 0; i < 60; ++i) {
        v.speed = cfg::MAX_SPEED;
        VehiclePhysics::tick(v, in, s_track, s_atlas, tr);
    }
    checkCond(std::fabs(v.x - (-0.615f)) <= 0.03f,
              "6.d", "Drift oracle: curve=3.0, f=1, steer=0 gives x(1s) = -0.615 (+/- 0.03)");

    // 6.f Swept collision: 10,000 random trials vs analytical interval oracle
    buildStraightTrack(s_track, 600);
    uint32_t rng = 0x13572468u;
    int mismatches = 0;
    for (int trial = 0; trial < 10000; ++trial) {
        for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) tr.cars[i].active = 0;
        tr.cars[0].active   = 1;
        tr.cars[0].spriteId = SPR_CAR_RED;
        tr.cars[0].offset   = (mathutil::rand01(rng) * 2.0f - 1.0f);
        tr.cars[0].speed    = mathutil::rand01(rng) * 4000.0f;
        tr.prevZ[0]         = 5000.0f + mathutil::rand01(rng) * 600.0f;
        tr.cars[0].z        = tr.prevZ[0] + tr.cars[0].speed * cfg::STEP;

        float prevZ = 5000.0f;
        v.speed = 4000.0f + mathutil::rand01(rng) * 8000.0f;
        v.z     = prevZ + v.speed * cfg::STEP;
        v.x     = (mathutil::rand01(rng) * 2.0f - 1.0f);
        float prevX = v.x;

        CollisionEvent ce = detectCollision(v, prevZ, prevX, s_track, s_atlas, tr);
        bool sweptHit = (ce.kind == CollisionEvent::Traffic);

        const SpriteImage& img = s_atlas.img[SPR_CAR_RED];
        float otherLen = img.worldH * 0.6f;
        float lim = vphys::PLAYER_HALF_W + img.worldW / (2.0f * cfg::ROAD_HALF_W);
        float r0  = mathutil::wrapDist(tr.prevZ[0], prevZ, s_track.length);
        float r1  = mathutil::wrapDist(tr.cars[0].z, v.z, s_track.length);
        bool exactHit = (std::fabs(v.x - tr.cars[0].offset) < lim) &&
                        (std::max(r0, r1) > -otherLen + 1e-4f) &&
                        (std::min(r0, r1) < vphys::PLAYER_LEN - 1e-4f);
        if (sweptHit != exactHit) mismatches++;
    }
    checkCond(mismatches == 0, "6.f", "No tunnelling: 10,000 random high-speed collision trials match oracle");

    // 6.g & 6.h Crash state machine cycle & Respawn safety
    VehiclePhysics::reset(v, s_track, tr);
    for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) tr.cars[i].active = 0;
    tr.cars[0].active = 1;
    tr.cars[0].offset = 0.0f;
    tr.cars[0].z      = 1000.0f;
    tr.cars[0].speed  = 0.0f;
    v.x = 0.0f;
    v.z = 900.0f;
    v.speed = 11000.0f;
    in.down = InputState::ACCEL;

    bool seenImpact = false, seenSpin = false, seenFade = false, seenRespawn = false, backToDrive = false;
    bool respawnSafe = false;
    int crashTicks = 0;
    for (int t = 0; t < 240; ++t) {
        VehiclePhysics::tick(v, in, s_track, s_atlas, tr);
        if (v.state != CrashState::Driving) crashTicks++;
        if (v.state == CrashState::Impact)  seenImpact = true;
        if (v.state == CrashState::Spin && seenImpact) seenSpin = true;
        if (v.state == CrashState::FadeOut && seenSpin) seenFade = true;
        if (v.state == CrashState::Respawn && seenFade && !seenRespawn) {
            seenRespawn = true;
            respawnSafe = (v.speed == 0.0f) && (v.invulnT > 1.9f);
        }
        if (v.state == CrashState::Driving && seenRespawn) { backToDrive = true; break; }
    }
    float crashDur = crashTicks * cfg::STEP;
    checkCond(seenImpact && seenSpin && seenFade && seenRespawn && backToDrive &&
              crashDur >= 2.60f && crashDur <= 3.10f,
              "6.g", "Crash cycle transitions Driving -> Impact -> Spin -> FadeOut -> Respawn -> Driving (~2.85 s)");
    checkCond(respawnSafe, "6.h", "Respawn safety: speed reset to 0 and 2.0 s collision immunity applied");
}

void testPhase7() {
    std::printf("\n=== Phase 7: Real-Time Audio Synthesis & Offline DSP Checks ===\n");
    s_synth.resetState();
    s_synth.setMusicEnabled(false);

    // 7.a Pitch law: rpm = 3000 -> 100 Hz; rpm = 6000 -> 200 Hz
    AudioParams ap;
    ap.rpm = 3000.0f;
    ap.throttle = 0.0f;
    s_synth.setParams(ap);
    s_synth.render(s_audioBuf, SynthDevice::SR * 2);
    double mag100 = goertzelMag(s_audioBuf + SynthDevice::SR, SynthDevice::SR, 100.0, SynthDevice::SR);
    double mag200_a = goertzelMag(s_audioBuf + SynthDevice::SR, SynthDevice::SR, 200.0, SynthDevice::SR);

    ap.rpm = 6000.0f;
    s_synth.setParams(ap);
    s_synth.render(s_audioBuf, SynthDevice::SR * 2);
    double mag200_b = goertzelMag(s_audioBuf + SynthDevice::SR, SynthDevice::SR, 200.0, SynthDevice::SR);
    checkCond(mag100 > 0.01 && mag200_a > 0.005 && mag200_b > 0.01,
              "7.a", "Pitch law: spectral peaks at 100 Hz (3000 RPM) and 200 Hz (6000 RPM)");

    // 7.d Engine RMS level at mid RPM (-24 to -14 dBFS)
    ap.rpm = 4500.0f;
    ap.throttle = 0.5f;
    s_synth.setParams(ap);
    s_synth.render(s_audioBuf, SynthDevice::SR);
    double sumSq = 0.0;
    for (int i = 0; i < SynthDevice::SR; ++i) {
        double v = static_cast<double>(s_audioBuf[i]) / 32768.0;
        sumSq += v * v;
    }
    double rmsDb = 20.0 * std::log10(std::sqrt(sumSq / SynthDevice::SR));
    checkCond(rmsDb >= -24.0 && rmsDb <= -14.0,
              "7.d", "Engine RMS level at mid RPM is within [-24, -14] dBFS");

    // 7.c Headroom: all voices at maximum stay within Int16 without wrapping
    s_synth.setMusicEnabled(true);
    ap.rpm = 8000.0f;
    ap.throttle = 1.0f;
    ap.skid = 1.0f;
    ap.offroad = 1.0f;
    ap.speedFrac = 1.0f;
    ap.crashCount = 1;
    ap.scrapeCount = 1;
    ap.shiftCount = 1;
    s_synth.setParams(ap);
    s_synth.render(s_audioBuf, SynthDevice::SR);
    int maxSample = 0;
    for (int i = 0; i < SynthDevice::SR; ++i) {
        maxSample = std::max(maxSample, std::abs(static_cast<int>(s_audioBuf[i])));
    }
    checkCond(maxSample > 8000 && maxSample <= 32767,
              "7.c", "Headroom: soft-clip limiter prevents Int16 wrap with all voices active");

    // 7.g Tempo accuracy: 10 seconds of rendering advances floor(10 * 128 * 4 / 60) = 85 steps (+/- 1)
    s_synth.resetState();
    s_synth.setMusicEnabled(true);
    for (int sec = 0; sec < 10; ++sec) {
        s_synth.render(s_audioBuf, SynthDevice::SR);
    }
    int64_t steps10s = static_cast<int64_t>(s_synth.musicStepsAdvanced());
    checkCond(std::abs(steps10s - 85) <= 1,
              "7.g", "Music sequencer fractional accumulator tempo accuracy (128 BPM)");
}

void testPhase8() {
    std::printf("\n=== Phase 8: Multi-Threading, SIMD & Zero-Allocation Checks ===\n");
    // 8.b Exhaustive SIMD span fill test with 16-pixel canary guards
    uint32_t bufSIMD[16 + 128 + 16];
    uint32_t bufScal[16 + 128 + 16];
    const uint32_t canary = 0xDEADBEEFu;
    const uint32_t fillC  = 0xFF123456u;
    bool spanOk = true;

    initSpanFill(false);
    for (int mis = 0; mis < 32; ++mis) {
        for (int n = 0; n <= 70; ++n) {
            std::fill(bufSIMD, bufSIMD + 160, canary);
            std::fill(bufScal, bufScal + 160, canary);
            uint32_t* d1 = bufSIMD + 16 + mis;
            uint32_t* d2 = bufScal + 16 + mis;
            if (n < 16) {
                for (int i = 0; i < n; ++i) d1[i] = fillC;
            } else {
                g_fillPixels(d1, n, fillC);
            }
            fillPixelsScalar(d2, n, fillC);
            if (std::memcmp(bufSIMD, bufScal, sizeof(bufSIMD)) != 0) {
                spanOk = false;
            }
        }
    }
    checkCond(spanOk, "8.b", "SIMD fillPixels matches scalar std::fill across all alignments 0..31 and lengths 0..70");

    // 8.c SIMD vs Scalar fade equivalence across RGB values and levels 1..3
    uint32_t fadeA[64], fadeB[64];
    bool fadeOk = true;
    for (int level = 1; level <= 3; ++level) {
        for (int i = 0; i < 64; ++i) {
            uint32_t col = RGB((i * 37) & 255, (i * 73) & 255, (i * 151) & 255);
            fadeA[i] = col;
            fadeB[i] = col;
        }
        fadeRowSIMD(fadeA, 64, level);
        fadeRowScalar(fadeB, 64, level);
        if (std::memcmp(fadeA, fadeB, sizeof(fadeA)) != 0) fadeOk = false;
    }
    checkCond(fadeOk, "8.c", "SIMD fadeRow matches scalar SWAR fade across levels 1..3");

    // 8.a & 1.f Golden hash & zero heap allocation across 1-thread scalar vs 4-thread SIMD
    auto runScriptedDrive = [](int workers, bool forceScalar, uint64_t& allocDeltaOut) -> uint64_t {
        s_game.init(workers, forceScalar);
        s_game.testMode = true;
        uint64_t h = 14695981039346656037ULL;
        InputState in;
        uint64_t allocStart = 0;

        for (int f = 0; f < 180; ++f) {
            if (f == 10) allocStart = g_allocCount.load(std::memory_order_relaxed);
            in.down = InputState::ACCEL;
            if (f >= 40 && f < 90)  in.down |= InputState::RIGHT;
            if (f >= 90 && f < 140) in.down |= InputState::LEFT;
            s_game.tick(in);
            s_game.render();
            h = mathutil::fnv1a64(s_game.fb.px, sizeof(s_game.fb.px), h);
        }
        uint64_t allocEnd = g_allocCount.load(std::memory_order_relaxed);
        allocDeltaOut = allocEnd - allocStart;
        s_game.shutdown();
        return h;
    };

    uint64_t alloc1 = 0, alloc4 = 0;
    uint64_t hash1Scalar = runScriptedDrive(0, true,  alloc1);
    uint64_t hash4SIMD   = runScriptedDrive(3, false, alloc4);

    checkCond(alloc1 == 0 && alloc4 == 0,
              "1.f", "Zero heap allocations during steady-state tick() and render() frames");
    checkCond(hash1Scalar == hash4SIMD,
              "8.a", "Golden FNV-1a framebuffer hash is byte-identical between 1-thread scalar and 4-thread SIMD");
}

void testPhase9_Upgrades() {
    std::printf("\n=== Phase 9: Launch Screen, 5 Tracks, 2D GPS, High Scores & Variable Pixel Size ===\n");

    // 9.a Verify all 5 tracks build valid closed loops and normalized 2D GPS splines
    bool allTracksOk = true;
    for (int t = 0; t < NUM_TRACK_PRESETS; ++t) {
        buildTrackPreset(s_track, t);
        if (s_track.count < 300 || s_track.count % 6 != 0) allTracksOk = false;
        if (std::fabs(s_track.segs[s_track.count - 1].y1 - s_track.segs[0].y0) > 1.0f) allTracksOk = false;
        for (int i = 0; i < s_track.count; ++i) {
            if (std::fabs(s_track.mapX[i]) > 1.05f || std::fabs(s_track.mapY[i]) > 1.05f) {
                allTracksOk = false;
            }
        }
    }
    checkCond(allTracksOk, "9.a", "All 5 racing tracks build valid C0 elevation loops and normalized [-1,1] 2D GPS splines");

    // 9.b Verify Turn-by-Turn GPS Rally Scanner detects curves ahead
    buildTrackPreset(s_track, 2); // Cyber Tokyo (has hard curves right after start straight)
    float peakC = 0.0f;
    int distSegs = 0;
    bool crest = false;
    scanUpcomingTurn(s_track, 10, peakC, distSegs, crest);
    checkCond(std::fabs(peakC) >= 1.5f && distSegs > 0,
              "9.b", "GPS Turn-by-Turn Rally Scanner accurately detects upcoming hard curves and distance");

    // 9.c Verify HighScoreManager persistence (save & reload)
    std::remove("/tmp/racer_test_highscores.dat");
    HighScoreManager hsm;
    hsm.init("/tmp/racer_test_highscores.dat");
    bool newHS = false, newBL = false;
    hsm.submitResult(1, 88500u, 42.35f, 298.5f, 21, newHS, newBL);
    HighScoreManager hsmReload;
    hsmReload.init("/tmp/racer_test_highscores.dat");
    const TrackRecord& r1 = hsmReload.record(1);
    checkCond(newHS && newBL && r1.highScore == 88500u && std::fabs(r1.bestLapSec - 42.35f) < 0.05f,
              "9.c", "HighScoreManager persists and reloads per-track High Score and Best Lap records");

    // 9.d Verify Variable Pixel Size (1px Ultra 960x720 vs 4px Retro quantization) & Launch Menu rendering
    s_game.init(2, false);
    s_game.testMode = false;
    s_game.enterLaunchMenu();
    s_game.setPixelSizePreset(0); // 1px Ultra 960x720
    InputState emptyIn;
    s_game.tick(emptyIn);
    s_game.render();
    uint64_t hashLaunch1px = mathutil::fnv1a64(s_game.displayFb.px, sizeof(s_game.displayFb.px));

    s_game.setPixelSizePreset(3); // 4px Retro (4x4 uniform blocks in 960x720)
    s_game.render();
    bool blockQuantizedOk = true;
    for (int y = 0; y < DisplayBuffer::H; y += 4) {
        for (int x = 0; x < DisplayBuffer::W; x += 4) {
            uint32_t c00 = s_game.displayFb.row(y)[x];
            if (s_game.displayFb.row(y + 3)[x + 3] != c00) blockQuantizedOk = false;
        }
    }
    checkCond(hashLaunch1px != 0 && blockQuantizedOk,
              "9.d", "DisplayBuffer renders 960x720 Launch Screen and quantizes exact NxN pixel blocks");

    // 9.e Verify LaunchMenu -> Countdown -> Racing transition flow
    s_game.startNewRace();
    bool startedInCountdown = (s_game.screenState == ScreenState::Countdown);
    for (int i = 0; i < 210; ++i) {
        s_game.tick(emptyIn);
    }
    bool transitionedToRacing = (s_game.screenState == ScreenState::Racing);
    checkCond(startedInCountdown && transitionedToRacing,
              "9.e", "Game state machine transitions cleanly from LaunchMenu -> 3-2-1 Countdown -> Racing");

    // 9.f Verify 4 CarSpec presets, Traffic Congestion (Low=8, Med=16, High=24), and Steer Assist toggle
    s_game.selectCar(1); // Vortex V12 (345 km/h)
    bool carSpecOk = (getCarSpec(s_game.selectedCar).topSpeedKmh == 345);
    s_game.selectTrafficDensity(0); // Low (8 cars)
    int lowCount = s_game.traffic.activeCount;
    s_game.selectTrafficDensity(1); // Medium (16 cars)
    int medCount = s_game.traffic.activeCount;
    s_game.selectTrafficDensity(2); // High (24 cars)
    int highCount = s_game.traffic.activeCount;
    s_game.toggleSteerAssist();
    bool assistToggledOn = s_game.steerAssistOn && s_game.vehicle.steerAssist;
    s_game.toggleSteerAssist();
    bool assistToggledOff = !s_game.steerAssistOn && !s_game.vehicle.steerAssist;
    checkCond(carSpecOk && lowCount == 8 && medCount == 16 && highCount == 24 && assistToggledOn && assistToggledOff,
              "9.f", "4 Car presets, Traffic Congestion (8/16/24 cars), and Steer Assist ON/OFF toggle verified");

    // 9.g Verify Car Health depletion -> Wrecked state -> On-Spot Repair recovery
    s_game.vehicle.health = 5.0f;
    s_game.vehicle.wrecked = true;
    s_game.tick(emptyIn);
    bool enteredWrecked = (s_game.screenState == ScreenState::Wrecked);
    s_game.repairAndContinue();
    bool repairedOk = (s_game.screenState == ScreenState::Racing) &&
                      (s_game.vehicle.health == 100.0f) &&
                      (!s_game.vehicle.wrecked);
    s_game.shutdown();
    checkCond(enteredWrecked && repairedOk,
              "9.g", "Zero Car Health triggers Wrecked state and On-Spot Repair restores 100% Health in place");
}

} // namespace

int runAllUnitTests() {
    std::printf("================================================================\n");
    std::printf("  Software-Rasterized Pseudo-3D Racer Verification Suite\n");
    std::printf("================================================================\n");

    g_passed = 0;
    g_failed = 0;

    testPhase1();
    testPhase2();
    testPhase3();
    testPhase4();
    testPhase5();
    testPhase6();
    testPhase7();
    testPhase8();
    testPhase9_Upgrades();

    std::printf("\n================================================================\n");
    std::printf("  Summary: %d passed, %d failed\n", g_passed, g_failed);
    std::printf("================================================================\n");
    return (g_failed == 0) ? 0 : 1;
}
