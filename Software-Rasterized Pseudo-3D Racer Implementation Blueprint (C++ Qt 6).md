# Software-Rasterized Pseudo-3D Racer: Implementation Blueprint (C++ / Qt 6)

Oct 24, 2026 · @Sayanaditya Das

## 0. Global architecture and conventions

Every phase below shares one constants header, one memory model and one frame contract; paste this section plus the single phase you are generating into the downstream model, so struct names and numbers never drift.

### 0.1 Frame contract (fixed 60 Hz)

1. **Tick** (fixed step, `cfg::STEP`): input, vehicle physics, collision, traffic AI, audio parameter update. Never touches pixels.
2. **Prepare** (serial, cheap): compute `Camera`, run Pass A (project segments into the `RowTable`), build the depth-sorted sprite list.
3. **Rasterize** (parallel over row slices): Pass B fills sky, grass, rumble, road, lane marks, then blits sprites clipped to the slice.
4. **Overlay** (serial or sliced): cockpit, HUD, glass cracks, fade.
5. **Present**: `QImage` wrapping the framebuffer, one `drawImage()` in `paintEvent()`.

### 0.2 `config.h` (authoritative constants)

```cpp
#pragma once
#include <cstdint>
namespace cfg {
inline constexpr int   SCREEN_W   = 320;
inline constexpr int   SCREEN_H   = 240;
inline constexpr int   WORLD_ROWS = 200;      // rows 0..199 = 3D world; 200..239 hidden under dashboard
inline constexpr int   HORIZON_BASE = 90;     // horizon row at zero pitch
inline constexpr float FOCAL_PX   = 160.0f;   // (SCREEN_W/2)/tan(45deg) -> 90 deg horizontal FOV
inline constexpr float SEG_LEN    = 200.0f;   // world units (wu) per road segment
inline constexpr int   DRAW_DIST  = 200;      // segments projected per frame
inline constexpr float ROAD_HALF_W = 1200.0f; // wu, centre to edge
inline constexpr int   LANES      = 3;
inline constexpr float CAM_HEIGHT = 1000.0f;  // wu above road surface
inline constexpr int   RUMBLE_LEN = 3;        // segments per stripe band
inline constexpr int   MAX_SEGMENTS = 4096;
inline constexpr int   MAX_SCENERY  = 4096;
inline constexpr int   MAX_TRAFFIC  = 24;
inline constexpr int   MAX_DRAW_SPRITES = 512;
inline constexpr float STEP       = 1.0f / 60.0f;
inline constexpr float MAX_SPEED  = SEG_LEN / STEP;   // 12000 wu/s = exactly 1 segment per tick
inline constexpr float KMH_PER_WU = 0.025f;           // 12000 wu/s displays as 300 km/h
inline constexpr int   NUM_SLICES = 8;                // row slices for Phase 8
}
```

Pixel format is `QImage::Format_RGB32`, i.e. `0xFFRRGGBB` in a `uint32_t`. Sprite atlases use ARGB32 where **alpha == 0 means transparent** (the colour key).

```cpp
constexpr uint32_t RGB(uint32_t r, uint32_t g, uint32_t b) { return 0xFF000000u | (r << 16) | (g << 8) | b; }
```

### 0.3 Coordinate system and units

- World axes: **X right, Y up, Z forward** along the track. Track length is `trackLen = segCount * SEG_LEN`; all Z values wrap modulo `trackLen`.
- Lateral positions of the player, traffic and scenery are **normalised**: `offset = 1.0` is the road edge, so world X = `offset * ROAD_HALF_W`. Off-road starts at `|offset| > 1`.
- Speed is in wu/s. Display speed = `speed * KMH_PER_WU`.
- Screen: origin top-left, Y down, row stride = `SCREEN_W` pixels (1280 bytes, an exact multiple of the 64-byte cache line).

### 0.4 Source layout

| File | Phase | Responsibility |
| --- | --- | --- |
| `config.h`, `mathutil.h` | 0 | constants, lerp/clamp/ease, xorshift RNG |
| `framebuffer.h/.cpp` | 1 | pixel buffer, `RGB()`, clear/test patterns |
| `gamewidget.h/.cpp`, `main.cpp` | 1 | QWidget host, timer, input, present |
| `track.h/.cpp` | 2-3 | `Segment` array, track builder, curve/hill authoring |
| `roadrender.h/.cpp` | 2-3 | Pass A projection, Pass B span fill |
| `sprites.h/.cpp` | 4 | atlas, draw list, scaled blit |
| `cockpit.h/.cpp` | 5 | dashboard, gauges, windshield mask |
| `vehicle.h/.cpp`, `collision.h/.cpp` | 6 | physics, crash state machine |
| `audio.h/.cpp` | 7 | synthesized engine loop, SFX, music |
| `jobs.h/.cpp`, `spanfill_*.cpp` | 8 | slice scheduler, SIMD span fills |

### 0.5 Memory and determinism rules (apply to every phase)

- **No heap allocation in tick or render.** All storage is `static`, `std::array`, or members of one `Game` object allocated at startup. Debug builds override `operator new` with a counter; the unit checks assert it does not move between frames.
- Hot arrays are `alignas(64)`. Per-frame scratch (`RowTable`, projected segments, draw list) lives in a `RenderContext` struct, never in the immutable `Track`.
- `float` everywhere (no `double` in hot paths); no `std::function`, `QString` or `QVariant` per frame.
- Fixed-step simulation with an accumulator, so physics is identical at any render rate. RNG is `xorshift32`, seeded once, so a run is reproducible.
- Compile flags: `-O2 -fno-math-errno` (GCC/Clang) or `/O2 /fp:precise` (MSVC). Do **not** use fast-math: it breaks the determinism check.

### 0.6 Prompt template for downstream models

```text
You are implementing Phase N of a software-rasterized pseudo-3D racer in C++17 + Qt 6.
Hard rules: no OpenGL/GPU, no per-pixel Qt calls, no heap allocation in tick/render.
Inputs: config.h (below), the structs from earlier phases (below), and the Phase N spec.
Output: only the files listed in the spec, compiling standalone, plus the unit-check harness.
[paste section 0.2] [paste prior-phase headers] [paste Phase N]
```

## Phase 1: Engine harness, framebuffer and 60 FPS presentation loop

**Goal:** an empty Qt window that shows a CPU-written 320x240 test pattern, scaled to the window, with a drift-free 60 Hz fixed-step loop and a keyboard bitmask. Nothing in later phases changes this plumbing.

### 1.1 Memory model

The whole 320x240 buffer is 307,200 bytes: it lives in L2, not L1 (32-48 KB). Sequential row-major writes still stream through L1 and keep the hardware prefetcher fed. The row stride (1280 bytes = 20 cache lines) is a multiple of 64, so a row slice never shares a cache line with its neighbour: this is what makes Phase 8 free of false sharing.

```cpp
// framebuffer.h
#pragma once
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <config.h>

struct Framebuffer {
    static constexpr int W = cfg::SCREEN_W;
    static constexpr int H = cfg::SCREEN_H;
    static constexpr int STRIDE = W;                 // in pixels
    alignas(64) uint32_t px[W * H];                  // 0xFFRRGGBB

    uint32_t*       row(int y)       { return px + y * STRIDE; }
    const uint32_t* row(int y) const { return px + y * STRIDE; }
    void clear(uint32_t c)           { std::fill(px, px + W * H, c); }
};
static_assert(sizeof(Framebuffer) == 320 * 240 * 4, "no padding allowed");
static_assert(Framebuffer::STRIDE * 4 % 64 == 0, "row must be a cache-line multiple");
```

The `Game` object that owns the `Framebuffer` is a **static** object (or a member of `main`'s stack object created once), so the buffer is 64-byte aligned for its whole lifetime. Assert `reinterpret_cast<uintptr_t>(fb.px) % 64 == 0` in `Game::init()`.

### 1.2 Interfaces

```cpp
// input.h
struct InputState {
    enum Bit : uint8_t { ACCEL = 1, BRAKE = 2, LEFT = 4, RIGHT = 8, RESET = 16, PAUSE = 32 };
    uint8_t down = 0;        // currently held
    uint8_t pressed = 0;     // went down since last tick (cleared by the widget after each tick)
    bool has(Bit b) const { return (down & b) != 0; }
};

// game.h
class Game {
public:
    void init();                              // one-time setup; may allocate
    void tick(const InputState& in);          // fixed STEP; NO allocation, NO pixel writes
    void render();                            // writes fb; NO allocation
    Framebuffer fb;
    uint64_t    tickCount = 0;
};

// gamewidget.h
class GameWidget : public QWidget {
    Q_OBJECT
public:
    explicit GameWidget(QWidget* parent = nullptr);
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void focusOutEvent(QFocusEvent*) override;     // clear all held keys
private slots:
    void onFrame();
private:
    Game          game_;
    QImage        view_;        // wraps game_.fb.px, no copy
    QTimer        timer_;       // single-shot, re-armed every frame
    QElapsedTimer clock_;
    qint64        accNs_ = 0, lastNs_ = 0, deadlineNs_ = 0;
    InputState    input_;
    QRect         target_;      // letterboxed destination rect
    FrameStats    stats_;       // ring buffer, no allocation
};
```

### 1.3 Construction and presentation

```cpp
GameWidget::GameWidget(QWidget* p) : QWidget(p),
    view_(reinterpret_cast<uchar*>(game_.fb.px), Framebuffer::W, Framebuffer::H,
          Framebuffer::STRIDE * 4, QImage::Format_RGB32)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::StrongFocus);
    game_.init();
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &GameWidget::onFrame);
    clock_.start();
    deadlineNs_ = lastNs_ = clock_.nsecsElapsed();
    timer_.start(0);
}

void GameWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);   // nearest neighbour = crisp pixels
    p.drawImage(target_, view_);                               // the ONLY Qt draw call per frame
}
```

`resizeEvent` computes `target_`: `s = min(width / 320.0, height / 240.0)`; if `s >= 1`, floor it to an integer for pixel-perfect scaling; centre the rect. Bars outside `target_` are painted black **once** in `resizeEvent` via `setAutoFillBackground(false)` plus a single `fillRect` on the first paint after a resize (a `bool barsDirty_` flag), never per frame.

### 1.4 Frame pacing algorithm (drift-free)

A fixed timer interval of 16 ms runs fast (62.5 Hz) and 17 ms runs slow (58.8 Hz). Instead, schedule against an absolute deadline and keep simulation on an accumulator:

```text
onFrame():
    now = clock.nsecsElapsed()
    acc += now - last;  last = now
    if acc > 100 ms: acc = 100 ms              // spiral-of-death guard after a stall
    while acc >= STEP_NS (16'666'667):
        game.tick(input);  input.pressed = 0
        acc -= STEP_NS
    game.render()
    update()                                    // queues paintEvent
    stats.push(now - prevFrameStart)
    deadline += STEP_NS
    if deadline < now: deadline = now           // never try to catch up with extra renders
    timer.start( max(0, (deadline - clock.nsecsElapsed()) / 1'000'000) )   // ms, floor
```

Because the accumulator absorbs rounding, the long-run tick rate is exactly 60.000 Hz even though individual frames land 16 or 17 ms apart.

### 1.5 Input handling

```text
keyPressEvent(e):   if e.isAutoRepeat() return
    bit = map(e.key())      // Up/W=ACCEL, Down/S=BRAKE, Left/A=LEFT, Right/D=RIGHT, R=RESET, P=PAUSE
    if bit and not (input.down & bit): input.pressed |= bit
    input.down |= bit
keyReleaseEvent(e): if e.isAutoRepeat() return;  input.down &= ~map(e.key())
focusOutEvent:      input.down = 0
```

### 1.6 Stats helper (no allocation)

```cpp
struct FrameStats {
    static constexpr int N = 128;
    float ms[N] = {};  int head = 0;
    void  push(float v) { ms[head] = v; head = (head + 1) % N; }
    float avg() const;   // mean over N
    float p99() const;   // copy to a stack array of N, std::nth_element at index 126
};
```

### 1.7 Phase 1 test render

`Game::render()` draws: a 16x16 checkerboard of two greys, a 1-pixel red border on the outermost pixels, and a 4-pixel-wide white vertical bar at `x = (tickCount * 2) % 320`. Row loops are written as `uint32_t* r = fb.row(y); for (x...) r[x] = ...;`, with no `fb.px[y*W+x]` multiplications inside the inner loop.

### 1.8 Unit verification checks

| # | Check | Pass criterion |
| --- | --- | --- |
| 1.a | Alignment | `reinterpret_cast<uintptr_t>(fb.px) % 64 == 0` asserted at startup |
| 1.b | Pixel format | Pure `RGB(255,0,0)` fill shows red (not blue): confirms `Format_RGB32` byte order |
| 1.c | Border | 1-px red border visible on all four edges at 1x, 2x and 3x scale, no half-pixel blur |
| 1.d | Tick rate | Count `tickCount` over 60 s of wall time: 3600 +/- 2 |
| 1.e | Frame time | HUD text (or console) shows avg 16.67 ms and p99 below 20 ms on an idle machine |
| 1.f | No allocation | Debug `operator new` counter is identical on frame 100 and frame 10,000 |
| 1.g | Input | Holding Up then alt-tabbing releases ACCEL (focus-out clears state); no auto-repeat double edges |
| 1.h | Letterbox | Resize window to 700x300: image stays 4:3, centred, black bars, still nearest-neighbour |

## Phase 2: Pseudo-3D scanline road projection

**Goal:** a straight, flat, endless road with horizon, sky, alternating grass / rumble-curb / road bands and lane markings, scrolling toward the camera. The architecture built here (Pass A fills a per-row table, Pass B fills each row left to right) is the one Phases 3, 4 and 8 extend without rewriting.

### 2.1 Why two passes

Pass A walks the segments **near to far** (serial, about 200 iterations) and writes one `RowInfo` per screen row. Pass B reads that table and fills each row independently, so any row range can later be handed to a different thread. All per-pixel work is a contiguous span write; fog and colour banding are resolved per row from a lookup table, never per pixel.

### 2.2 Structs and memory layout

```cpp
// track.h
struct alignas(16) Segment {            // 16 bytes: 4 per cache line
    float    y0, y1;                    // world elevation at start / end (Phase 3; 0 in Phase 2)
    float    curve;                     // lateral acceleration (Phase 3; 0 in Phase 2)
    uint16_t sceneryBegin;              // index into Track::scenery (Phase 4)
    uint8_t  sceneryCount;
    uint8_t  flags;
};
struct Track {
    Segment  segs[cfg::MAX_SEGMENTS];
    int      count = 0;                 // must be a multiple of 2 * RUMBLE_LEN
    float    length = 0;                // count * SEG_LEN
    // scenery array added in Phase 4
};
void buildStraightTrack(Track& t, int count);   // asserts count % (2*RUMBLE_LEN) == 0

// camera.h
struct Camera {
    float x = 0, y = cfg::CAM_HEIGHT, z = 0;   // world; z wrapped into [0, track.length)
    float pitchPx = 0;                         // vertical shift of horizon, pixels (+ = horizon lower)
    float shiftX = 0;                          // horizontal image shift in pixels (shake, sway); added to W/2 in projection
};

// roadrender.h
struct RoadPoint { float cz, sx, sy, sw; };     // camera depth, screen x, screen y, half-width px

struct RowInfo {                                 // 8 bytes
    int16_t  cx;          // road centre x (pixels, may be off-screen)
    int16_t  hw;          // road half-width (pixels)
    uint16_t seg;         // absolute segment index, or ROW_INVALID
    uint8_t  fog;         // 0..FOG_LEVELS-1
    uint8_t  band;        // 0/1 stripe parity
};
inline constexpr uint16_t ROW_INVALID = 0xFFFF;
inline constexpr int FOG_LEVELS = 32;

struct RoadPalette { uint32_t grass, rumble, road, lane; };   // one per (fog, band)

struct RenderContext {
    alignas(64) RowInfo   rows[cfg::WORLD_ROWS];
    alignas(64) RoadPoint pts[cfg::DRAW_DIST + 1];
    int   horizonY = cfg::HORIZON_BASE;
    int   baseSeg  = 0;           // absolute index of segment under the camera
    float basePct  = 0;           // 0..1 progress inside it
    RoadPalette pal[FOG_LEVELS][2];   // built once at init
    uint32_t    skyLut[256];
    uint32_t    fogColor;
};

void initRenderContext(RenderContext&);                                    // builds palettes + sky LUT
void projectRoad(const Track&, const Camera&, RenderContext&);             // Pass A
void fillRows(const RenderContext&, Framebuffer&, int yBegin, int yEnd);   // Pass B, [yBegin, yEnd)
```

### 2.3 Mathematics

Camera-space projection of a world point (X, Y, Z), with focal length F = `FOCAL_PX` and horizon row H0:

```latex
c_x = X - x_{cam},\quad c_y = Y - y_{cam},\quad c_z = Z - z_{cam}\\
s = \frac{F}{c_z}\\
screen_x = \frac{W}{2} + s\,c_x,\qquad screen_y = H_0 - s\,c_y,\qquad halfWidth_{px} = s\,R
```

where R = `ROAD_HALF_W`. Segment boundary *n* (n = 0..DRAW\_DIST) has camera depth, using `basePct` = fractional position inside the camera's segment:

```latex
c_z(n) = \max\big((n - basePct)\cdot SEG\_LEN,\; Z_{near}\big),\qquad Z_{near} = SEG\_LEN
```

This avoids wrap-around bookkeeping: depth is relative, and only the segment index wraps. Boundaries nearer than about 1470 wu project below row 199 on flat ground, so clamping at Z\_near never shows.

**Closed form for flat ground** (used as the correctness oracle in 2.7): the road half-width on screen row y and the depth that row sees are

```latex
z(y) = \frac{F\cdot CAM\_HEIGHT}{y - H_0},\qquad halfWidth_{px}(y) = \frac{(y - H_0)\,R}{CAM\_HEIGHT}
```

Edges of a planar quad project to straight lines, so linearly interpolating `sx` and `sw` in **screen y** between two boundaries is exact, with no perspective correction needed.

### 2.4 Pass A: `projectRoad` pseudocode

```text
projectRoad(track, cam, ctx):
    ctx.baseSeg = int(cam.z / SEG_LEN) % track.count
    ctx.basePct = frac(cam.z / SEG_LEN)
    ctx.horizonY = HORIZON_BASE + round(cam.pitchPx)
    for y in 0..WORLD_ROWS-1: ctx.rows[y].seg = ROW_INVALID

    for n in 0..DRAW_DIST:                                  // project boundaries once, shared by 2 segments
        cz = max((n - basePct) * SEG_LEN, Z_NEAR)
        s  = FOCAL_PX / cz
        pts[n] = { cz, W/2 + s*(0 - cam.x), horizonY + s*cam.y, s*ROAD_HALF_W }   // road centre line is X=0, Y=0

    maxY = WORLD_ROWS                                        // top-most row already written (near segments win)
    for n in 0..DRAW_DIST-1:
        yBot = pts[n].sy;  yTop = pts[n+1].sy               // yTop < yBot on flat road
        if yBot <= yTop:  continue                           // back-facing (Phase 3 hills)
        if yTop >= maxY:  continue                           // fully hidden behind a nearer crest
        rBeg = max(0, ceil(yTop));  rEnd = min(maxY, ceil(yBot))      // rows [rBeg, rEnd)
        if rBeg >= rEnd:  continue
        inv  = 1 / (yTop - yBot)                              // ONE division per segment
        dX   = (pts[n+1].sx - pts[n].sx) * inv                // pixels of x per row
        dW   = (pts[n+1].sw - pts[n].sw) * inv
        t0   = rBeg - yBot
        x    = pts[n].sx + dX*t0;  w = pts[n].sw + dW*t0
        abs  = (baseSeg + n) % track.count
        for y in rBeg..rEnd-1:
            rows[y] = { int16(x+0.5), int16(w+0.5), abs, FOG_LUT[n], (abs / RUMBLE_LEN) & 1 }
            x += dX;  w += dW
        maxY = rBeg
```

`FOG_LUT[n] = min(FOG_LEVELS-1, (n * n * FOG_LEVELS) / (DRAW_DIST * DRAW_DIST))`, a quadratic ramp that keeps near road clean and fades the last third. Rows from `horizonY` down to the farthest written row stay `ROW_INVALID` and are filled with `fogColor`.

### 2.5 Pass B: `fillRows` pseudocode

```text
fillRows(ctx, fb, yBegin, yEnd):
    for y in yBegin..yEnd-1:
        row = fb.row(y)
        if y < ctx.horizonY:                       // sky: stepped gradient
            fillSpan(row, 0, W, ctx.skyLut[ (y*255) / max(ctx.horizonY,1) ]);  continue
        r = ctx.rows[y]
        if r.seg == ROW_INVALID:                   // beyond draw distance
            fillSpan(row, 0, W, ctx.fogColor);  continue
        P  = ctx.pal[r.fog][r.band]
        rw = max(1, (r.hw * RUMBLE_NUM) >> 8)       // curb width ~ 8% of half-width => RUMBLE_NUM = 20
        roadL = r.cx - r.hw;  roadR = r.cx + r.hw
        rumL  = roadL - rw;   rumR  = roadR + rw
        // strictly left-to-right, each span clamped to [0, W]
        fillSpan(row, 0,     rumL,  P.grass)
        fillSpan(row, rumL,  roadL, P.rumble)
        fillSpan(row, roadL, roadR, P.road)
        fillSpan(row, roadR, rumR,  P.rumble)
        fillSpan(row, rumR,  W,     P.grass)
        if r.band == 0:                             // dashed centre/lane lines on alternate bands
            lw = max(1, (r.hw * LANE_NUM) >> 8)     // LANE_NUM = 6 => ~2.3% of half-width
            for k in 1..LANES-1:
                lx = roadL + (2*r.hw*k) / LANES      // integer divide by constant 3 => multiply-shift
                fillSpan(row, lx - lw/2, lx - lw/2 + lw, P.lane)
```

```cpp
// spanfill.h  (Phase 8 swaps the body for SIMD; signature never changes)
inline void fillSpan(uint32_t* row, int x0, int x1, uint32_t c) {
    x0 = std::max(x0, 0);  x1 = std::min(x1, cfg::SCREEN_W);
    if (x0 < x1) std::fill(row + x0, row + x1, c);
}
```

### 2.6 Palette (retro, pre-fogged)

| Slot | Band 0 | Band 1 |
| --- | --- | --- |
| Grass | (16,170,16) | (0,148,0) |
| Rumble | (210,30,30) | (240,240,240) |
| Road | (118,118,124) | (104,104,110) |
| Lane | (232,232,232) | unused |
| Sky | gradient (24,84,200) at top to (150,210,255) at horizon, quantised to 12 steps |  |
| Fog | (170,200,230) |  |

`initRenderContext` fills `pal[f][b]` by `lerp(color, fogColor, f / (FOG_LEVELS-1))` per channel, so no blending happens at draw time.

### 2.7 Phase 2 demo behaviour

`Game::tick`: `cam.z += 6000 * STEP` (wrapped modulo `track.length`); Left/Right change `cam.x` by +/-1500 wu/s clamped to +/-1.4 \* `ROAD_HALF_W`. Track: `buildStraightTrack(track, 600)`.

### 2.8 Unit verification checks

| # | Check | Pass criterion |
| --- | --- | --- |
| 2.a | Flat-ground oracle | `cam.x = 0`, `pitch = 0`: for y in 110..199, `abs(rows[y].hw - (y - 90) * 1.2) <= 1` (e.g. row 199 gives about 131 px) |
| 2.b | Symmetry | `rows[y].cx == 160` for all valid rows; left grass width equals right grass width within 1 px |
| 2.c | Lateral shift | `cam.x = 600`: `rows[199].cx` is about 94.6 (+/- 2 px), because 160 - (160/1468)\*600 |
| 2.d | Stripe scale | Visible stripe height grows smoothly toward the bottom; each colour band is 3 segments deep |
| 2.e | Scroll continuity | Sweep `cam.z` across one segment in 100 sub-steps: no row in `[ceil(farTop), 199]` is ever `ROW_INVALID`, and no pixel row flickers colour |
| 2.f | Wrap seam | Set `cam.z = length - 10` and run 20 ticks: stripe pattern is continuous across the wrap (needs `count % 6 == 0`) |
| 2.g | Lane geometry | 2 lane lines visible, evenly spaced at 1/3 and 2/3 of road width |
| 2.h | Budget | Pass A under 20 us; Pass B (scalar, whole frame) under 0.4 ms on a 3 GHz core |

## Phase 3: Dynamic geometry (curves, hills, perspective shift)

**Goal:** the straight road becomes a looping track of eased bends and crests. Only `buildTrack`, `projectRoad` (Pass A), the sky/row-order logic of Pass B and the camera pitch change; Pass B's span filler is untouched.

### 3.1 Key idea: the track never needs a world X

Curvature is stored as a per-segment **lateral acceleration** and integrated every frame, starting at the camera. The road is therefore defined only *relative to the camera*, so a looping track never has to close geometrically; only elevation must return to its start height (see `finish()`).

### 3.2 Authoring API

```cpp
// track.h additions
namespace curve { inline constexpr float EASY = 0.6f, MED = 1.5f, HARD = 3.0f; }   // wu per segment^2
namespace hill  { inline constexpr float LOW = 600.f, MED = 1200.f, HIGH = 2400.f; } // wu of total rise

class TrackBuilder {
public:
    explicit TrackBuilder(Track& t);
    void straight(int n);
    // enter: segments easing curvature 0 -> curve; hold: constant; leave: easing curve -> 0.
    // dy: net elevation change (wu) spread over enter+hold+leave with cosine ease.
    void road(int enter, int hold, int leave, float curve, float dy);
    void sCurve(float c);                    // road(20,20,20,+c,0) then road(20,20,20,-c,0)
    void rollingHills(int n, float amp);     // n segments of sin-shaped dy, returns to start
    void finish();                           // closes elevation to start, pads count to multiple of 2*RUMBLE_LEN
private:
    Track& t_;  float lastY_ = 0;
    void add(float curve, float y1);         // y0 = lastY_, then lastY_ = y1
};
void buildDemoTrack(Track&);                 // about 900 segments mixing every element
```

**Easing functions** (`mathutil.h`), with p in \[0,1\]:

```latex
easeIn(a,b,p) = a + (b-a)\,p^2\\
easeOut(a,b,p) = a + (b-a)\,\big(1-(1-p)^2\big)\\
easeInOut(a,b,p) = a + (b-a)\Big(\tfrac{1}{2} - \tfrac{1}{2}\cos(\pi p)\Big)
```

`road()` pseudocode (total = enter + hold + leave, startY = `lastY_`, endY = startY + dy):

```text
for i in 0..total-1:
    if i < enter:            c = easeIn   (0,     curve, i/enter)
    elif i < enter + hold:   c = curve
    else:                    c = easeInOut(curve, 0,     (i-enter-hold)/leave)
    add(c, easeInOut(startY, endY, (i+1)/total))      // y1 of this segment; next y0 == this y1 (C0 continuous)
```

### 3.3 Euler integration of lateral displacement

With `curve_k` the curvature of segment k (counting from the camera's segment), `pct` the camera's fraction inside segment 0, and `dx_0 = -curve_0 * pct`:

```latex
x_{n+1} = x_n + dx_n,\qquad dx_{n+1} = dx_n + curve_n,\qquad x_0 = -pct\cdot dx_0
```

The `x_0` term is a centring correction: it makes the interpolated road centre exactly zero **at the camera**, which removes the slight lateral wobble that appears as the camera crosses a segment boundary. For constant curvature c from `pct = 0`, the closed form is `x_n = c * n * (n - 1) / 2`.

The lateral offset enters the projection as an addition to the boundary's world X:

```latex
screen_x(n) = \frac{W}{2} + \frac{F}{c_z(n)}\,\big(x_n - x_{cam}\big)
```

Positive curvature bends the road to the **right** on screen.

### 3.4 Hill elevation and camera height

Boundary n takes the elevation `Y_n = segs[(baseSeg + n) % count].y0`. The camera rides the surface:

```latex
y_{cam} = CAM\_HEIGHT + y_0 + pct\,(y_1 - y_0)\;\text{(of the camera's segment)}\\
screen_y(n) = H_0 - \frac{F}{c_z(n)}\,(Y_n - y_{cam})
```

Oracle for a constant slope `sigma = dY/dZ`: every boundary lands `F * sigma` pixels **higher** than the flat-road case (for example sigma = 0.1 lifts the far road by 16 px).

### 3.5 Perspective shift: camera pitch and parallax

Slope under the camera is `slope = (y1 - y0) / SEG_LEN`. The camera pitches into the slope, which lowers the horizon when climbing:

```latex
pitch_{target} = \text{clamp}\big(F\cdot slope\cdot 0.35,\; -24,\; +24\big)\\
pitch \mathrel{+}= (pitch_{target} - pitch)\,(1 - e^{-\Delta t/\tau}),\quad \tau = 0.15\,s
```

`Camera::pitchPx = pitch`, consumed as `horizonY = HORIZON_BASE + round(pitchPx)`. Precompute `k = 1 - expf(-STEP/0.15f)` once.

**Mountain parallax strip.** A 512x48 ARGB image generated at init (sum of three sines, alpha 0 above the ridge line), drawn on the rows `[horizonY - 48, horizonY)` before the road. Its horizontal scroll advances each tick by `bgX += player.segment.curve * speedFraction * 1.2` (pixels), wrapping at 512. Rows are drawn as at most two sequential keyed span copies (before and after the wrap).

### 3.6 Updated `projectRoad` (replaces the Phase 2 pseudocode)

```cpp
// RenderContext additions
int16_t clipY[cfg::DRAW_DIST];   // rows >= clipY[n] are covered by nearer geometry (used by sprites in Phase 4)
int     topRoadY;                // smallest row written by the road this frame
```

```text
projectRoad(track, cam, ctx):
    baseSeg, basePct, horizonY as before
    seg0 = track.segs[baseSeg]
    dx = -seg0.curve * basePct;   x = -basePct * dx
    for n in 0..DRAW_DIST:
        s_  = track.segs[(baseSeg + n) % count]
        cz  = max((n - basePct) * SEG_LEN, Z_NEAR);  sc = FOCAL_PX / cz
        pts[n].cz = cz
        pts[n].sx = W/2 + sc * (x - cam.x)
        pts[n].sy = horizonY - sc * (s_.y0 - cam.y)
        pts[n].sw = sc * ROAD_HALF_W
        x += dx;  dx += s_.curve
    maxY = WORLD_ROWS;  ctx.topRoadY = WORLD_ROWS
    for n in 0..DRAW_DIST-1:
        ctx.clipY[n] = maxY                           // BEFORE this segment is drawn
        yBot = pts[n].sy;  yTop = pts[n+1].sy
        if yBot <= yTop:  continue                     // looking at the underside of a rise
        if yTop >= maxY:  continue                     // hidden behind a nearer crest
        ... identical row-fill loop as Phase 2 ...
        maxY = rBeg;  ctx.topRoadY = min(ctx.topRoadY, rBeg)
```

### 3.7 Pass B change: row order

Hills put road rows **above** the base horizon, so Pass B must test the road first:

```text
for y in yBegin..yEnd-1:
    if rows[y].seg != ROW_INVALID:   draw grass/rumble/road/lane spans (unchanged)
    elif y < horizonY:               mountains strip + sky gradient
    else:                            fillSpan(row, 0, W, fogColor)      // terrain hidden behind a crest or beyond draw distance
```

### 3.8 Unit verification checks

| # | Check | Pass criterion |
| --- | --- | --- |
| 3.a | Curve oracle | Constant `curve = 1.5`, `pct = 0`, `x_cam = 0`: `pts[50].sx - 160` equals `(160/10000) * 1.5*50*49/2` = 29.4 px, +/- 1 px |
| 3.b | Direction | Positive curve moves the far road right; negative moves it left; road at the camera stays centred (`rows[199].cx` within 2 px of the straight-road value) |
| 3.c | Wobble | Sweep `pct` 0..1 across a constant bend: `rows[199].cx` changes by under 1 px (centring correction works) |
| 3.d | Slope oracle | Constant slope 0.1: far boundary rows are 16 px (+/- 1) above the flat case |
| 3.e | Crest occlusion | Camera before a crest: `clipY[n]` is non-increasing in n; rows between the crest line and the horizon are `ROW_INVALID`; no road is visible beyond the crest |
| 3.f | Back-face | Camera on a crest: segments descending away are drawn, the underside of the near rise is skipped, and there are no inverted trapezoids (assert every written row has `hw > 0`) |
| 3.g | Elevation closure | After `finish()`: `abs(segs[count-1].y1 - segs[0].y0) < 1`, and for all i `abs(segs[i+1].y0 - segs[i].y1) < 1e-3` |
| 3.h | Smooth curvature | Along a `road(25,25,25,c,0)`, `abs(curve[i+1] - curve[i]) <= 2*c/25` |
| 3.i | Pitch | Climb a 10% grade: `pitchPx` settles to 0.35 \* 160 \* 0.1 = 5.6 px within 0.5 s, with no overshoot |
| 3.j | Wrap | Drive 3 laps at constant speed: no visible pop at the loop seam in curvature, elevation or stripe colour |
| 3.k | Budget | Pass A under 40 us with hills and curves; zero allocations |

## Phase 4: Billboard sprite engine

**Goal:** roadside scenery and traffic cars drawn as scaled, colour-keyed billboards, correctly depth-ordered and clipped by hill crests, with output that is bit-identical whether the frame is drawn in one slice or four.

### 4.1 Data model

```cpp
// sprites.h
enum SpriteId : uint16_t { SPR_TREE, SPR_PINE, SPR_BUSH, SPR_LAMP, SPR_SIGN, SPR_ROCK,
                           SPR_CAR_RED, SPR_CAR_BLUE, SPR_CAR_YELLOW, SPR_TRUCK, SPR_COUNT };

struct SpriteImage {                       // pixels live in SpriteAtlas::pool
    const uint32_t* px;                    // ARGB32, alpha == 0 -> transparent (colour key)
    uint16_t w, h;                         // source pixels; anchor is bottom-centre (w/2, h)
    float    worldW, worldH;               // size in wu (also the collision box in Phase 6)
    uint8_t  xMin[128], xMax[128];         // per source row: first / one-past-last opaque pixel (xMin>=xMax: empty row)
};
struct SpriteAtlas {
    alignas(64) uint32_t pool[96 * 1024];  // 384 KB total, all sprites <= 64x128
    SpriteImage img[SPR_COUNT];
};
void buildSpriteAtlas(SpriteAtlas&);       // procedural pixel art, no asset files

struct Scenery {                           // 8 bytes, stored sorted by segment in Track::scenery[]
    float    offset;                       // normalised lateral: +-1 = road edge
    uint16_t spriteId;
    uint8_t  flip, pad;
};                                         // Track gains: Scenery scenery[cfg::MAX_SCENERY];

struct Car {                               // traffic, pooled
    float    z;                            // world z, wrapped
    float    offset;                       // normalised lateral
    float    speed;                        // wu/s
    uint16_t spriteId;
    uint8_t  active, pad;
};

struct DrawSprite {                        // 16 bytes
    int16_t  sx, sy;                       // screen anchor (bottom-centre)
    uint16_t wPx, hPx;                     // destination size
    int16_t  clipY;                        // visible rows are < clipY
    uint16_t img;
    uint8_t  fog;                          // 0..FOG_LEVELS-1
    uint8_t  flip;
};
struct DrawList { DrawSprite items[cfg::MAX_DRAW_SPRITES]; int count = 0; };   // far -> near order
```

World sizes (src px is the generated pixel-art size):

| Sprite | src w x h | worldW x worldH (wu) |
| --- | --- | --- |
| Tree | 40 x 64 | 700 x 1100 |
| Pine | 32 x 72 | 500 x 1200 |
| Bush / Rock | 24 x 16 | 400 x 270 |
| Lamp post | 16 x 56 | 200 x 1500 |
| Billboard sign | 48 x 40 | 900 x 750 |
| Car (rear view) | 40 x 32 | 520 x 416 |
| Truck (rear view) | 48 x 56 | 640 x 750 |

### 4.2 Amendment to `RoadPoint` (Phases 2-3)

Sprites must be placed with the same perspective as the road, so Pass A also records each boundary's **camera-space** lateral offset and height:

```cpp
struct RoadPoint { float cz, wx, wy, sx, sy, sw; };   // wx = x_n - cam.x ; wy = Y_n - cam.y
```

`projectRoad` sets `wx = x - cam.x`, `wy = s_.y0 - cam.y` next to the existing `sx`, `sy`, `sw` assignments. No other Pass A logic changes.

### 4.3 Placement and scaling mathematics

A sprite at fractional position `t` (0..1) inside segment n, with normalised lateral offset `o`, interpolates **camera-space** quantities, which are linear in world Z, and only then projects. This keeps screen y perspective-exact:

```latex
c_z = (1-t)\,c_z(n) + t\,c_z(n+1),\quad w_x = (1-t)\,wx(n) + t\,wx(n+1) + o\,R,\quad w_y = (1-t)\,wy(n) + t\,wy(n+1)\\
s = \frac{F}{c_z},\quad sx = \frac{W}{2} + s\,w_x,\quad sy = H_0 - s\,w_y\\
w_{px} = \text{round}(s\cdot worldW),\qquad h_{px} = \text{round}(s\cdot worldH)
```

Scenery uses `t = 0`; a car with `z` uses `n = floor(z / SEG_LEN) - baseSeg` (mod count) and `t = frac(z / SEG_LEN)`.

### 4.4 Building the draw list (zero allocation, correct order without a sort)

Far-to-near iteration over segments is itself the depth sort. Traffic is bucketed into per-segment linked lists with a counting-sort pass:

```text
buildDrawList(track, traffic, ctx, list):
    headOf[0..SPRITE_DIST-1] = -1                               // int16 stack array, SPRITE_DIST = 140
    for i in 0..MAX_TRAFFIC-1 where cars[i].active:
        n = (segIndex(cars[i].z) - ctx.baseSeg + count) % count
        if n < SPRITE_DIST:  nextCar[i] = headOf[n];  headOf[n] = i
    list.count = 0
    for n in SPRITE_DIST-1 down to 0:                            // FAR -> NEAR
        seg = track.segs[(ctx.baseSeg + n) % count]
        for k in 0..seg.sceneryCount-1:   emit(track.scenery[seg.sceneryBegin + k], n, t=0)
        for i = headOf[n]; i != -1; i = nextCar[i]:   emit(cars[i], n, t = frac(cars[i].z / SEG_LEN))

emit(item, n, t):
    img = atlas.img[item.spriteId]
    compute (cz, sx, sy, s) from 4.3
    if cz < Z_SPRITE_NEAR (= 400):                      return
    wPx = round(s*img.worldW);  hPx = round(s*img.worldH)
    if hPx < 1 or hPx > 1024:                           return
    if sx + wPx/2 < 0 or sx - wPx/2 >= W:               return    // horizontally off-screen
    clip = ctx.clipY[n]
    if sy - hPx >= clip or sy <= 0:                     return    // fully hidden by a nearer crest / above screen
    list.items[list.count++] = { sx, sy, wPx, hPx, clip, spriteId, FOG_LUT[n], item.flip }
```

The scenery generator guarantees at most 3 scenery items per segment, so `140*3 + 24 = 444 < MAX_DRAW_SPRITES (512)`; the builder asserts this.

### 4.5 Scaled, colour-keyed blit (slice-safe)

Signature: `void drawSprites(const DrawList&, const SpriteAtlas&, Framebuffer&, int yBegin, int yEnd);`. Each slice thread calls it with its own row range and all sprites, in list order.

```text
blit(s, img, fb, yBegin, yEnd):
    dstL = s.sx - s.wPx/2;   dstT = s.sy - s.hPx                  // destination top-left (may be negative)
    y0 = max(dstT, yBegin, 0);   y1 = min(s.sy, s.clipY, yEnd, WORLD_ROWS)
    x0 = max(dstL, 0);           x1 = min(dstL + s.wPx, W)
    if y0 >= y1 or x0 >= x1:  return
    stepX = (img.w << 16) / s.wPx;   stepY = (img.h << 16) / s.hPx           // 16.16 fixed point
    v = int64(y0 - dstT) * stepY                                              // ABSOLUTE start: makes slices identical
    for y in y0..y1-1:
        sr = v >> 16;   v += stepY
        if img.xMin[sr] >= img.xMax[sr]:  continue                            // empty source row
        src = img.px + sr * img.w;   dst = fb.row(y)
        // clamp the x loop to the opaque run, scaled into destination space
        dxa = max(x0, dstL + ((img.xMin[sr] * s.wPx) / img.w))
        dxb = min(x1, dstL + ((img.xMax[sr] * s.wPx + img.w - 1) / img.w))
        u = int64(dxa - dstL) * stepX
        for x in dxa..dxb-1:                                                   // left -> right, sequential writes
            sc = flip ? (img.w - 1 - (u >> 16)) : (u >> 16)
            c  = src[sc];  u += stepX
            if (c >> 24) != 0:   dst[x] = (s.fog == 0) ? c : fogBlend(c, ctx.fogColor, FOG_ALPHA[s.fog])
```

```cpp
// 2-multiply fog blend; a = fog * 256 / FOG_LEVELS (precomputed table), result alpha forced to 0xFF
inline uint32_t fogBlend(uint32_t c, uint32_t f, uint32_t a) {
    uint32_t rb = (((c & 0x00FF00FFu) * (256 - a)) + ((f & 0x00FF00FFu) * a)) >> 8 & 0x00FF00FFu;
    uint32_t g  = (((c & 0x0000FF00u) * (256 - a)) + ((f & 0x0000FF00u) * a)) >> 8 & 0x0000FF00u;
    return 0xFF000000u | rb | g;
}
```

The per-row opaque bounds (`xMin`, `xMax`) make the inner loop skip empty margins, which is the span-level benefit of the colour key. Fog blending is paid only by far sprites, which are small by construction.

### 4.6 Procedural scenery placement

```cpp
void populateScenery(Track&, uint32_t seed);   // deterministic xorshift32; fills Track::scenery sorted by segment
```

- Lamp posts: both sides at `offset = +-1.15`, every 12 segments (flip = 1 for the left side).
- Trees/pines: per segment, with probability 0.18, `offset = sign * (1.5 + 2.0 * rand01)`; at most 3 items per segment.
- Billboard signs: every 60 segments at `offset = +-1.35`.
- Bushes/rocks: probability 0.08, `offset = sign * (1.15 + 0.6 * rand01)`.

### 4.7 Unit verification checks

| # | Check | Pass criterion |
| --- | --- | --- |
| 4.a | 1/Z law | Same tree at `cz = 2000` and `cz = 4000`: `hPx` is 88 and 44 (+/- 1) |
| 4.b | Ground anchor | Sprite at offset 0, flat road, `cz = 3000`: `sy` equals `90 + 160*1000/3000` = 143 (+/- 1), and that row is a road row |
| 4.c | Lateral edge | A sprite with offset +1: `sx` equals `rows[sy].cx + rows[sy].hw` (+/- 1) |
| 4.d | Depth order | Two overlapping trees at different depth: the nearer fully covers the farther; shuffling storage order leaves the frame hash unchanged |
| 4.e | Crest clipping | Sprite just behind a crest: only rows `< clipY` are written; fully behind: nothing is emitted |
| 4.f | Colour key | On a solid-red background, red pixels outside each sprite silhouette are unchanged |
| 4.g | Slice identity | Render a 300-sprite scene with 1 slice and with 4 slices: framebuffers are **byte-identical** (memcmp) |
| 4.h | Edge safety | Sprites half off the left and right edges: no write outside `[0, W)`; ASAN/UBSAN clean |
| 4.i | Flip | A flipped lamp post is the exact mirror of the unflipped one (compare row by row) |
| 4.j | Budget | About 150 visible sprites in under 0.35 ms; draw list never exceeds 512; zero allocations |

## Phase 5: First-person cockpit and interior overlay

**Goal:** windshield frame, curved dashboard, animated speedometer and tachometer, tilting steering wheel, camera shake, glass-crack overlay and impact flash, all drawn without a full-screen overlay image.

### 5.1 Design decision: procedural runs, not a 320x240 image

A full ARGB overlay would cost about 300 KB and double the cache footprint. Instead the frame is a **per-row run table** (up to 6 opaque horizontal runs per row, each with a row-dependent colour), filled with `fillSpan`, plus a few small keyed decals (dial faces, centre console) that reuse the Phase 4 blit. Total cockpit memory is about 50 KB, and camera shake of the interior is exact (shift the run coordinates), with no resampling.

### 5.2 Layout (screen pixels, 320x240)

| Element | Geometry |
| --- | --- |
| Roof header | rows 0..13, fully opaque |
| Windshield | trapezoid: top corners (36,14) and (284,14); bottom corners (20,176) and (300,176) |
| Pillars | opaque outside the trapezoid for rows 14..192 |
| Dash top edge | `y_top(x) = 176 + round(16 * ((x-160)/160)^2)`: highest (176) at centre, 192 at the edges |
| Dash body | rows 192..239 opaque across the full width |
| Steering wheel | centre (160, 232), rim rx = 42, ry = 32 |
| Tachometer | dial centre (70, 214), r = 26 (left of wheel) |
| Speedometer | dial centre (250, 214), r = 26 (right of wheel) |
| Digital speed | 3 seven-segment digits at (141, 206), each 8x14 px |

`WORLD_ROWS = 200` is safe because the dash top edge never exceeds row 192.

### 5.3 Structs

```cpp
// cockpit.h
struct Run { int16_t x0, x1; uint32_t color; };           // [x0, x1), x may be < 0 or > W (margin 8 px for shake)
struct RowRuns { uint8_t n; Run r[6]; };                  // 6*8 + 1 -> 56 bytes

struct CockpitState {                                      // filled by Vehicle each tick
    float speedKmh = 0, rpm = 900, steer = 0;              // steer in [-1, 1]
    float crack = 0;                                       // 0..1 growth of glass cracks
    float flash = 0;                                       // 0..1 white impact flash
    float damage = 0;                                      // 0..1, tints the dash lamps
};

struct CrackSeg { int16_t x0, y0, x1, y1; };
struct CrackSet { CrackSeg s[160]; int count = 0; };      // generated once per crash

struct CameraFx {                                          // output feeds Camera (see 5.6)
    float shakeX = 0, shakeY = 0;                          // px
    float impactAmp = 0, impactT = 0, accelPitch = 0, phase = 0;
    void impact(float ampPx);                              // amp 6..12 px for a crash
    void update(float speedFrac, float rpmFrac, float accelFrac);   // once per tick
};

class Cockpit {
public:
    void init();                                           // builds run table, dial decals, LUTs
    void update(const CockpitState& s, const CameraFx& fx);// smoothing only; no pixels
    void tint(Framebuffer&);                               // optional windshield visor gradient
    void drawCracks(Framebuffer&);                         // BEFORE the frame so the frame masks them
    void drawFrame(Framebuffer&);                          // runs + decals
    void drawGauges(Framebuffer&);                         // needles, digits, wheel, flash
    void triggerCrack(int impactX, int impactY, uint32_t seed);
private:
    RowRuns   rows_[cfg::SCREEN_H];
    float     dispSpeed_ = 0, dispRpm_ = 900, dispSteer_ = 0;
    int       ox_ = 0, oy_ = 0;                            // interior shake offset, clamped to +-4
    CrackSet  cracks_;  float crackProgress_ = 0;
    CockpitState st_;
};
```

### 5.4 Building the run table (init)

```text
for y in 0..239:
    n = 0
    if y < 14:                         add(-8, W+8, roofColor(y))
    else if y < 192:
        t  = clamp((y - 14) / (176 - 14), 0, 1)
        xl = round(lerp(36, 20, t));  xr = round(lerp(284, 300, t))      // windshield opening
        add(-8, xl, pillarColor(y));  add(xl-1, xl, edgeHighlight)        // left pillar + 1 px highlight
        add(xr, W+8, pillarColor(y)); add(xr, xr+1, edgeHighlight)
        if y >= 176:                                                      // curved dash rising into the opening
            g  = 160 * sqrt((y - 176) / 16.0)
            dl = max(round(160 - g), xl);  dr = min(round(160 + g), xr)
            if dl < dr:  add(dl, dr, dashColor(y))
    else:                              add(-8, W+8, dashColor(y))
```

Colour LUTs (`roofColor`, `pillarColor`, `dashColor`) are 240-entry vertical gradients computed at init. For y in 176..191 the opening above the dash stays transparent, so the world shows through.

### 5.5 Drawing order each frame

```text
world (Pass B)  ->  tint()  ->  drawCracks()  ->  drawFrame()  ->  drawGauges()
```

`drawFrame` with interior shake offset (ox, oy), for each destination row y:

```text
srcRow = clamp(y - oy, 0, 239)
for run in rows_[srcRow]:  fillSpan(fb.row(y), run.x0 + ox, run.x1 + ox, run.color)
```

The 8 px margin guarantees no world pixel leaks at the screen edge when `|ox|, |oy| <= 4`. Decals (dial faces 56x56, console 80x40) are keyed-blitted at their anchor plus (ox, oy) by the Phase 4 `blitUnscaled` helper.

`tint()` is a cheap SWAR visor gradient on the top 24 windshield rows: rows 14..25 use `c - ((c >> 2) & 0x3F3F3F)` (about 75% brightness), rows 26..37 use `c - ((c >> 3) & 0x1F1F1F)` (about 88%).

### 5.6 Mathematics

**Dial needle.** Both gauges sweep 270 degrees clockwise from 225 degrees (zero) to -45 degrees (full scale):

```latex
\theta(v) = 225^\circ - 270^\circ\cdot\text{clamp}\!\left(\frac{v}{v_{max}},0,1\right),\qquad
(x,y) = (c_x + r\cos\theta,\; c_y - r\sin\theta)
```

Speedometer `v_max = 300 km/h` (150 km/h points straight up); tachometer `v_max = 9000 rpm`, redline arc at 7500 rpm drawn in the static dial decal. Needles use first-order smoothing per tick with `k = 0.25` (speed) and `k = 0.35` (rpm); above 7000 rpm add `0.006 * sin(0.9 * tick)` rad of jitter.

**Steering wheel.** Rim points are an oblique projection of a circle rotated by the wheel angle `a = steer * 100 deg` (smoothed with `k = 0.2`):

```latex
p_k = \big(c_x + r_x\cos(\theta_k + a),\; c_y + r_y\sin(\theta_k + a)\big),\quad \theta_k = \tfrac{2\pi k}{48}
```

The rim is 48 short thick segments drawn with `thickLine`; three spokes at angles `a + {180, 0, 270} deg` run from the hub to the rim. `thickLine` uses Bresenham and writes a horizontal span of the pen width at each step, so every write is a sequential span.

**Camera shake (applied to the world, not by copying pixels).** The world renderer receives `Camera::shiftX = shakeX` (added to `W/2` in projection) and `Camera::pitchPx += shakeY`:

```latex
shake_x = A\,e^{-t/0.4}\sin(47t + \varphi_1) + 0.3\,rpmFrac\,\sin(2\pi\cdot 24\,t)\\
shake_y = A\,e^{-t/0.4}\sin(61t + \varphi_2) + 0.4\,speedFrac\,\sin(2\pi\cdot 31\,t) + pitch_{accel}\\
pitch_{accel} \to 3\cdot accelFrac\;\text{(first order, } \tau = 0.1\,s\text{)}
```

where `A` is the impact amplitude in pixels (6 to 12 on a crash, else 0) and `accelFrac` in \[-1, 1\] is longitudinal acceleration over its maximum (positive pitches the nose up, which lowers the horizon). The interior shifts by `(round(0.3*shakeX), round(0.3*shakeY))`, clamped to +-4.

### 5.7 Glass cracks

`triggerCrack(ix, iy, seed)` fills `CrackSet` once with an xorshift32 generator (no allocation, max 160 segments):

```text
N = 10 + rand(5) radial rays, theta_k = 2*pi*k/N + jitter(+-0.25)
for each ray: len = 40 + rand(120);  walk in steps of 10..24 px, direction += jitter(+-0.35)
    with probability 0.3 per step spawn one child ray (depth <= 2, direction +-0.9, length 40%)
rings: at radii 22, 48, 80 connect the ray points nearest to that radius with jittered chords
stop when count == 160
```

Rendering: draw the first `count * crackProgress` segments as a 1 px light line (230,235,240), then the same segments offset by (+1,+1) in dark (60,60,70) for a glass edge. `crackProgress` rises linearly over 12 ticks after impact. Because `drawCracks` runs before `drawFrame`, pillars and dash automatically mask any crack that leaves the windshield. `flash` is a white fade of the windshield rows: per tick `flash *= 0.85`, applied as `c = ((c >> 1) & 0x7F7F7F) + 0x808080` while `flash > 0.5` and `c + (((0xFFFFFF - c) >> 2) & 0x3F3F3F)` while `flash > 0.15`.

### 5.8 Unit verification checks

| # | Check | Pass criterion |
| --- | --- | --- |
| 5.a | Windshield geometry | Row 100: opening is `[28, 292)` (+/- 1 px); row 14: opening begins; rows 0..13 fully opaque |
| 5.b | Masking | Render the world as solid magenta: magenta appears **only** inside the windshield polygon, and covers at least 99.9% of it (cracks and wheel excluded) |
| 5.c | Needle angles | Speed 0, 150, 300 km/h: needle tips at 225, 90 and -45 degrees (+/- 1 px at r = 22) |
| 5.d | Wheel symmetry | `steer = 0`: left half equals mirrored right half; `steer = +0.5` is the mirror of `steer = -0.5` |
| 5.e | Shake decay | `impact(8)`: amplitude below 0.4 px after 1.2 s; interior offset never exceeds +-4 |
| 5.f | Edge leak | At offset (+4, +4) and (-4, -4) no world pixel is visible in the pillars or at the screen edges |
| 5.g | Crack determinism | Same seed gives byte-identical frames; segment count at most 160; growth is monotonic over 12 ticks |
| 5.h | Draw order | Cracks never appear on pillars or dash (frame drawn after them) |
| 5.i | Budget | `tint + drawCracks + drawFrame + drawGauges` under 0.1 ms; zero allocations |

## Phase 6: Vehicle dynamics, collision and crash state machine

**Goal:** arcade driving model (throttle, brake, speed-dependent steering, centrifugal drift, hill drag, off-road friction), swept collision against scenery and traffic with no tunnelling at 12,000 wu/s, and a five-state crash machine that drives the screen shake, glass cracks, fade and respawn.

### 6.1 Structs

```cpp
// vehicle.h
enum class CrashState : uint8_t { Driving, Impact, Spin, FadeOut, Respawn };

struct AudioParams {                      // consumed by Phase 7; counters instead of queues
    float    rpm = 900, throttle = 0, skid = 0, offroad = 0, speedFrac = 0;
    uint32_t crashCount = 0, scrapeCount = 0, shiftCount = 0;   // monotonic; audio compares to its last value
};

struct Vehicle {
    // kinematics
    float z = 0, x = 0;                   // z wrapped to [0,length); x normalised, 1 = road edge
    float speed = 0, latV = 0;            // wu/s ; normalised lateral velocity (x units / s)
    float steer = 0;                      // smoothed steering, [-1, 1]
    float accelLong = 0;                  // wu/s^2, for camera pitch / audio
    // drivetrain
    int   gear = 1;  float rpm = 900;  float shiftCooldown = 0;
    // crash machine
    CrashState state = CrashState::Driving;
    float stateT = 0;                     // seconds in current state
    float invulnT = 0;                    // seconds of collision immunity left
    float spinDir = 1;                    // +-1 sway direction
    float yawPx = 0, tumblePx = 0;        // view sway outputs (px)
    float fade = 0;                       // 0 = clear, 1 = black
    float damage = 0;
    uint32_t crashSeq = 0;                // seeds the glass-crack generator
    AudioParams audio;
};

struct Traffic { Car cars[cfg::MAX_TRAFFIC]; float prevZ[cfg::MAX_TRAFFIC]; uint32_t rng = 1; };

struct CollisionEvent {
    enum Kind : uint8_t { None, Scenery, Traffic, Wall } kind = None;
    float severity = 0;                   // 0..1
    float dir = 0;                        // +-1: which way to push the player laterally
};

class VehiclePhysics {
public:
    static void reset(Vehicle&, const Track&, Traffic&);
    // One fixed step. Order is mandatory, see 6.7.
    static void tick(Vehicle&, const InputState&, const Track&, const SpriteAtlas&, Traffic&);
    static void fillCockpit(const Vehicle&, CockpitState&);
    static void fillCamera (const Vehicle&, const Track&, const CameraFx&, Camera&);
};
CollisionEvent detectCollision(const Vehicle& v, float prevZ, float prevX,
                               const Track&, const SpriteAtlas&, const Traffic&);
void updateTraffic(Traffic&, const Vehicle&, const Track&);   // movement + recycling
```

### 6.2 Constants

| Name | Value | Meaning |
| --- | --- | --- |
| `ACCEL_A` | 4800 wu/s^2 | engine term; time constant `tau = MAX_SPEED / ACCEL_A` = 2.5 s |
| `BRAKE_A` | 12000 wu/s^2 | stops from top speed in 1.0 s |
| `COAST_A` | 1800 wu/s^2 | engine braking with no input |
| `OFFROAD_A` | 6000 wu/s^2 | extra deceleration while `abs(x) > 1` |
| `OFFROAD_LIMIT` | 3000 wu/s | speed floor while off-road with throttle held |
| `STEER_ACCEL` | 4.0 x-units/s^2 | lateral acceleration at full lock |
| `GRIP_DAMP` | 3.0 1/s | lateral velocity damping |
| `CENT_K` | 0.9 | centrifugal gain |
| `HILL_G` | 6000 wu/s^2 | slope drag factor |
| `PLAYER_HALF_W` | 0.22 | normalised (260 wu) |
| `PLAYER_LEN` | 400 wu | hood length ahead of the camera |
| `WALL_X` | 2.6 | invisible barrier |

### 6.3 Dynamics mathematics

With `f = speed / MAX_SPEED`, `slope = (y1 - y0) / SEG_LEN` and `curve` of the segment under the player:

```latex
\dot v = A\,(1 - f)\,[\text{throttle}] - B\,[\text{brake}] - C\,[\text{no input}] - G_h\cdot slope - D_{off}\,[|x|>1]\\
v \leftarrow \text{clamp}(v,\,0,\,1.05\,V_{max}),\qquad v \leftarrow \max(v,\,V_{off})\ \text{if }|x|>1\text{ and throttle}
```

Lateral dynamics (x in road half-widths):

```latex
a_c = -K_c\cdot curve\cdot f^2\\
a_s = S\cdot steer\cdot \text{clamp}(5f,0,1)\cdot(1 - 0.25 f)\\
\dot{lv} = a_s + a_c - \gamma\,lv,\qquad \dot x = lv
```

Positive curve (right bend) pushes the car left (negative x). Closed form for `steer = 0` and constant `a_c` from rest laterally, with `v_inf = a_c / gamma`:

```latex
x(t) = v_\infty\Big(t - \frac{1 - e^{-\gamma t}}{\gamma}\Big)
```

Steering input smoothing: `steer += clamp(target - steer, -r*dt, +r*dt)` with `r = 4/s` when applying lock and `6/s` when returning toward zero.

**Skid intensity** (drives tyre screech): `driftSkid = smoothstep(2.2, 3.4, abs(a_s) + abs(a_c))`, `brakeSkid = brake ? 0.8 * f * [f > 0.35] : 0`, `skid = max(driftSkid, brakeSkid)`.

**Gearbox and RPM.** Five gears with upshift speed fractions 0.12, 0.26, 0.44, 0.68 and downshift at 90% of those values (hysteresis), minimum 0.3 s between shifts:

```latex
g_f = \frac{f - f_{lo}}{f_{hi} - f_{lo}},\qquad rpmFrac = \text{lerp}(r_{start}, 1, g_f),\quad r_{start} = 0\ (\text{gear }1),\ 0.5\ (\text{else})\\
rpm = 900 + 7600\cdot rpmFrac \cdot (0.9 + 0.1\,[\text{throttle}])
```

Each shift increments `audio.shiftCount`.

## Phase 7: Audio synthesis and playback

**Goal:** a real-time synthesized soundtrack: continuously pitch-bent engine, tyre screech, gravel rumble, metal crunch and a looping chiptune, mixed into one 44.1 kHz stream with no assets and no allocation in the audio callback.

### 7.1 Key decision: one `QAudioSink` pull stream

`QSoundEffect` plays fixed samples and **cannot change pitch while playing**, so it cannot do engine RPM bending. The whole mixer therefore lives in a `QIODevice` that `QAudioSink` pulls from. `QSoundEffect` remains a valid fallback only for pre-recorded one-shots (crash WAV) or a looping music WAV, never for the engine.

The audio callback may run on a different thread than the game loop (backend dependent), so parameters cross over as independent `std::atomic<float>` and `std::atomic<uint32_t>` values (relaxed ordering): no mutex, no allocation, no torn floats.

### 7.2 Interfaces

```cpp
// audio.h
class SynthDevice : public QIODevice {
public:
    static constexpr int SR = 44100;
    SynthDevice();
    void setParams(const AudioParams&);                 // game thread, once per tick, lock-free
    void render(int16_t* out, int frames);              // audio thread; also called by offline tests (mono)
    void setMasterGain(float g);
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return (1 << 20) + QIODevice::bytesAvailable(); }
protected:
    qint64 readData(char* d, qint64 maxlen) override;   // frames = maxlen / 2; calls render()
    qint64 writeData(const char*, qint64) override { return -1; }
private:
    struct Shared {                                      // written by game thread, read by audio thread
        std::atomic<float> rpm{900}, throttle{0}, skid{0}, offroad{0}, speedFrac{0}, master{0.8f};
        std::atomic<uint32_t> crashCount{0}, scrapeCount{0}, shiftCount{0};
    } sh_;
    EngineVoice engine_;  NoiseVoice skidV_, gravelV_;  CrashVoice crash_;  ScrapeVoice scrape_;  MusicSeq music_;
    uint32_t seenCrash_ = 0, seenScrape_ = 0, seenShift_ = 0;
    float    duck_ = 0;                                  // crash ducking, 0..1
    alignas(64) float engineTable_[2048 + 1];            // band-limited single cycle + guard sample
};

class AudioEngine : public QObject {
    Q_OBJECT
public:
    bool init();                    // QAudioFormat: 44100 Hz, 1 ch, Int16; sink->setBufferSize(4096) ~ 46 ms
    void start();  void stop();  void setMuted(bool);
    void push(const AudioParams& p) { dev_.setParams(p); }
private:
    SynthDevice dev_;  QAudioSink* sink_ = nullptr;      // sink_->start(&dev_) pulls from readData
};
```

Call `init()` once from `GameWidget`'s constructor and `push(vehicle.audio)` at the end of every tick. The `M` key toggles mute (in the Phase 1 key map `P` is already pause).

### 7.3 Parameter mapping

| Input (`AudioParams`) | Drives | Mapping |
| --- | --- | --- |
| `rpm` | engine pitch | `f0 = rpm / 30` Hz (4-cylinder, 4-stroke: 2 firings per revolution) |
| `throttle` | engine gain and intake noise | `gain = 0.16 + 0.20 * rpmFrac + 0.12 * throttle`; noise `= 0.05 * throttle` |
| `skid` | screech level | gain `0.30 * skid`; band-pass centre `1500 + 700 * skid` Hz |
| `offroad`, `speedFrac` | gravel rumble | gain `0.35 * offroad * speedFrac`, low-passed noise at 220 Hz |
| `shiftCount` change | clutch tick | 15 ms noise click at gain 0.12, and rpm dip comes from physics |
| `crashCount` change | crunch | see 7.5; engine ducked by 70%, music by 40% |
| `scrapeCount` change | scrape | 150 ms band-passed noise burst at 2.5 kHz, gain 0.3 |

### 7.4 Engine voice mathematics

One wavetable oscillator with linear interpolation. The table holds 12 harmonics (highest partial at the top RPM is `12 * 283 = 3.4 kHz`, far below Nyquist, so no aliasing):

```latex
T[i] = \sum_{k=1}^{12} \frac{1}{k^{0.9}}\sin\!\Big(2\pi k \tfrac{i}{2048} + \phi_k\Big)\Big/ N,\qquad \phi_k = 0.7\,k^2\ (\text{fixed phases for a buzzy timbre})
```

Per sample:

```latex
\varphi \leftarrow (\varphi + f_0/SR)\bmod 1,\qquad s = \text{lerp}\big(T[\lfloor 2048\varphi\rfloor],\,T[\lfloor 2048\varphi\rfloor + 1],\,\text{frac}\big)\\
y \leftarrow y + a\,(x - y),\qquad a = 1 - e^{-1/(\tau\,SR)}
```

The one-pole `y` smooths `f0` (`tau = 0.04 s`, gives a natural engine lag with no zipper noise) and the gain (`tau = 0.02 s`). Intake noise is white noise through a one-pole low-pass at about 2 kHz.

### 7.5 Noise voices, screech and crash

Skid and scrape use a Chamberlin state-variable band-pass; per sample:

```latex
low \mathrel{+}= F\cdot band,\quad high = in - low - q\cdot band,\quad band \mathrel{+}= F\cdot high,\qquad F = 2\sin\!\big(\pi f_c / SR\big),\ q = 1/Q\ (Q = 3)
```

The screech adds a 7 Hz amplitude wobble `1 + 0.15 * sin(2*pi*7*t)`. The crash is the sum of three parts, started when `crashCount` changes:

```latex
noise(t) = e^{-t/0.22}\cdot BP\big(white;\,f_c(t) = 3000\,e^{-t/0.35} + 400\big)\cdot 0.9\\
thump(t) = e^{-t/0.15}\sin\!\big(2\pi\,(35 + 40\,e^{-t/0.08})\,t\big)\cdot 0.8\\
clang(t) = \sum_{j} 0.12\,e^{-t/0.25}\sin(2\pi f_j t),\quad f_j \in \{523, 1130, 1790\}\ \text{Hz}
```

Crash ducking: on trigger `duck_ = 1`, then `duck_ *= exp(-1/(1.5*SR))` per sample; `engineGain *= 1 - 0.7*duck_`, `musicGain *= 1 - 0.4*duck_`. While `Vehicle::state` is FadeOut the incoming `rpm` already falls toward idle, so the pitch drop needs no special case.

### 7.6 Music sequencer

128 BPM, one sample-accurate step every `60/128/4 * 44100 = 5167.97` samples; use a fractional accumulator (`acc += 1; if acc >= stepLen: acc -= stepLen; advanceStep()`), never an integer step length, so tempo does not drift. Four bars of 16 steps in A minor (chords Am, F, C, G; roots MIDI 45, 41, 48, 43), looped. Note frequency `f = 440 * 2^((m - 69)/12)` from a 128-entry LUT.

| Voice | Waveform | Pattern |
| --- | --- | --- |
| Bass | triangle | root on steps 0, 2, 4, 6, 8, 10, 12, 14; steps 4 and 12 jump up an octave; step 15 plays root + 7 |
| Arpeggio | pulse, duty 25% | chord tones (root, third, fifth, third) stepping on every 16th, one octave above the bass, gain 0.10 |
| Lead | pulse, duty 50% | table below, 0 = rest, gain 0.12, 90 ms decay per note |
| Kick | sine sweep 120 to 45 Hz over 90 ms | steps 0, 4, 8, 12 of each bar |
| Clap | band-passed noise at 1.8 kHz, 120 ms decay | steps 4 and 12 |
| Hat | high-passed noise, 25 ms decay | steps 2, 6, 10, 14 |

```text
Lead melody (MIDI note per 16th step, 0 = rest):
bar 1 (Am): 76 0 0 72  0 74 0 76  0 0 79 0  76 0 0 0
bar 2 (F) : 77 0 0 74  0 72 0 74  0 0 77 0  81 0 0 0
bar 3 (C) : 79 0 0 76  0 72 0 76  0 0 79 0  84 0 0 0
bar 4 (G) : 83 0 0 79  0 74 0 79  0 0 83 0  79 74 71 0
```

The music bus runs at 0.30 of full scale before ducking.

### 7.7 Master mix and limiter

```latex
mix = g_{eng}\,E + g_{skid}\,S + g_{grav}\,G + C + P + g_{mus}\,M,\qquad out = \text{round}\big(32767\cdot \text{softclip}(master\cdot mix)\big)\\
\text{softclip}(x) = \frac{u(27 + u^2)}{27 + 9u^2},\quad u = \text{clamp}(x, -3, 3)\quad(\text{Pade approximation of }\tanh)
```

`readData` always fills the whole request (silence if muted), so the sink never underruns. Buffer size 4096 bytes gives about 46 ms of latency; with the 16.7 ms parameter update the worst-case input-to-pitch delay is about 70 ms.

### 7.8 Unit verification checks (offline, no sound card needed)

All of these call `SynthDevice::render()` into a stack or static buffer and analyse it with a small FFT in the test harness.

| # | Check | Pass criterion |
| --- | --- | --- |
| 7.a | Pitch law | `rpm = 3000` for 2 s: spectral peak at 100 Hz (+/- 2 Hz) with partials at 200 and 300 Hz; `rpm = 6000`: peak at 200 Hz; the ratio of the two peaks is 2.00 (+/- 1%) |
| 7.b | Glide | Step `rpm` 3000 to 6000: instantaneous frequency covers 63% of the step in 40 ms (+/- 10 ms); largest sample-to-sample jump below 25% of full scale (no clicks) |
| 7.c | Headroom | Engine, skid, gravel, crash and music all at maximum: peak stays at or below 0.95 of full scale and no sample wraps |
| 7.d | Engine level | Engine alone at mid RPM: RMS between -24 and -14 dBFS |
| 7.e | Crash event | Incrementing `crashCount` once: first 100 ms is at least 6 dB louder than engine alone; envelope is 40 dB down within 1.2 s; the same count value seen again retriggers nothing |
| 7.f | Ducking | Music gain falls 40% within 50 ms of a crash and recovers to within 5% after 3 s |
| 7.g | Tempo | After 10 minutes of rendering, the step counter equals `floor(600 * 128 * 4 / 60)` = 5120 +/- 1 |
| 7.h | Real-time safety | `std::atomic<float>::is_always_lock_free` is true; the debug `operator new` counter does not change during 60 s of `render()` |
| 7.i | Live run | 5 minutes with the real `QAudioSink`: `error()` stays `NoError`, no underruns, synthesis uses under 1.5% of one core |

## Phase 8: Multi-threading and SIMD span-fill optimisation

**Goal:** split the raster work into row slices drawn in parallel by persistent worker threads, make every span write a vectorised, cache-aligned store, and prove the result is byte-identical to the single-threaded scalar renderer. Amend `config.h`: `NUM_SLICES = 8` (25 rows per slice over `WORLD_ROWS = 200`).

### 8.1 What runs where

| Stage | Thread | Why |
| --- | --- | --- |
| Tick, `projectRoad` (Pass A), `buildDrawList` | main | about 50 us total, serial dependencies |
| Per slice: `fillRows` then `drawSprites` then `tint` rows | any worker or main | rows are independent; writes are disjoint |
| `drawCracks`, `drawFrame`, `drawGauges`, fade pass | main, after join | about 0.1 ms, shares line drawing |
| `update()` and `drawImage` | main (Qt) | Qt requires the GUI thread |

**Why there is no false sharing.** A row is 1280 bytes = 20 cache lines and the buffer is 64-byte aligned, so every slice boundary falls exactly on a cache-line boundary. Two threads never write the same line. Add `static_assert((cfg::WORLD_ROWS / cfg::NUM_SLICES) * Framebuffer::STRIDE * 4 % 64 == 0)`. Read-shared data (`RenderContext`, `DrawList`, atlas, `Track`) is immutable during the parallel phase. Per-slice timing counters are `alignas(64)` so they never share a line either.

### 8.2 Persistent worker pool (no allocation, no mutex)

Threads are `std::jthread`s created once in `init()`. Jobs are claimed with a single packed atomic **ticket** so a straggler from frame N can never claim a slice of frame N+1 with stale data.

```cpp
// jobs.h
struct FrameJob {                                  // everything a slice needs, all read-only
    const RenderContext* ctx;  const DrawList* dl;  const SpriteAtlas* atlas;
    const Track* track;  Framebuffer* fb;
};
struct alignas(64) SliceStat { uint64_t ns = 0; };

class SliceScheduler {
public:
    void init(int workerCount);                    // clamp(hardware_concurrency - 1, 1, NUM_SLICES - 1)
    void shutdown();                               // request_stop + notify, joins in under 100 ms
    void run(const FrameJob& job);                 // fork-join; main thread participates
    SliceStat stats[cfg::NUM_SLICES];
private:
    static constexpr int ROWS_PER_SLICE = cfg::WORLD_ROWS / cfg::NUM_SLICES;
    FrameJob jobs_[2];                             // double buffered by generation parity
    alignas(64) std::atomic<uint64_t> ticket_{0};  // high 32 bits: generation, low 32: next slice
    alignas(64) std::atomic<int>      done_{0};
    alignas(64) std::atomic<uint32_t> wake_{0};    // workers sleep on this
    uint32_t gen_ = 0;
    std::jthread workers_[cfg::NUM_SLICES - 1];
    void drain();                                  // claim and render slices until none left
    void renderSlice(const FrameJob&, int s);
};
```

```text
run(job):
    gen_ += 1
    jobs_[gen_ & 1] = job                                   // plain write, published by the release below
    done_.store(0, relaxed)
    ticket_.store((uint64(gen_) << 32) | 0, release)
    wake_.store(gen_, release);  wake_.notify_all()
    drain()                                                 // main renders slices too
    while done_.load(acquire) < NUM_SLICES:  spin 2000 x _mm_pause (or yield), then done_.wait(current)

drain():
    loop:
        t = ticket_.fetch_add(1, acq_rel)
        g = t >> 32;  s = t & 0xFFFFFFFF
        if s >= NUM_SLICES:  return                         // nothing left (or a stale ticket: ignore)
        renderSlice(jobs_[g & 1], s)
        if done_.fetch_add(1, acq_rel) + 1 == NUM_SLICES:  done_.notify_one()

workerLoop(stop):
    seen = 0
    while not stop.requested:
        spin up to 2000 x _mm_pause while wake_ == seen
        wake_.wait(seen)                                    // futex sleep
        seen = wake_.load(acquire)
        drain()

renderSlice(job, s):
    y0 = s * ROWS_PER_SLICE;  y1 = y0 + ROWS_PER_SLICE
    t0 = now()
    fillRows(*job.ctx, *job.fb, y0, y1)
    drawSprites(*job.dl, *job.atlas, *job.fb, y0, y1)
    tintRows(*job.fb, y0, y1)
    stats[s].ns = now() - t0
```

Eight slices over at most four threads gives dynamic load balancing without any queue: whichever thread finishes early claims the next slice. If `workerCount` resolves to 0 (single core), `run` simply calls `drain()` on the main thread, and the output is identical.

`QThreadPool` alternative: preallocate `NUM_SLICES` `QRunnable` objects with `setAutoDelete(false)` and call `QThreadPool::globalInstance()->start()` per slice, then `waitForDone()`. It works with zero steady-state allocation but has higher wake-up latency, so the scheduler above is the default.

### 8.3 SIMD `fillSpan`

Signature never changes: `void fillSpan(uint32_t* row, int x0, int x1, uint32_t c)` clamps then calls a function pointer `g_fillPixels(uint32_t* d, int n, uint32_t c)` chosen once at startup. Spans under 16 pixels use an inline scalar loop and skip the call.

```cpp
// spanfill_avx2.cpp  (compile this TU alone with -mavx2 / /arch:AVX2)
void fillPixelsAVX2(uint32_t* d, int n, uint32_t c) {
    const __m256i v = _mm256_set1_epi32(int(c));
    uint32_t mis = (reinterpret_cast<uintptr_t>(d) & 31) >> 2;   // pixels past the 32-byte boundary
    if (mis) { _mm256_storeu_si256(reinterpret_cast<__m256i*>(d), v); d += 8 - mis; n -= 8 - mis; }
    while (n >= 32) {                                            // aligned, 128 bytes per iteration
        _mm256_store_si256(reinterpret_cast<__m256i*>(d),      v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(d + 8),  v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(d + 16), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(d + 24), v);
        d += 32; n -= 32;
    }
    while (n >= 8) { _mm256_store_si256(reinterpret_cast<__m256i*>(d), v); d += 8; n -= 8; }
    if (n) _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + n - 8), v);   // overlapping tail, same colour
}
```

The head store and the tail store both overlap pixels that already hold `c`, so they never touch memory outside the span, and this requires `n >= 16` on entry (guaranteed by the caller's small-span path). NEON (`spanfill_neon.cpp`, AArch64) uses `vdupq_n_u32` and `vst1q_u32` four times per iteration (16 pixels), with the same head/tail overlap trick on 16-byte alignment. Do **not** use non-temporal stores: the buffer is L2-resident and Qt reads it again immediately.

Dispatch (once, in `initSpanFill()`):

```text
x86-64:  g_fillPixels = __builtin_cpu_supports(avx2) ? fillPixelsAVX2 : fillPixelsScalar     (MSVC: __cpuid, leaf 7 bit 5)
AArch64: g_fillPixels = fillPixelsNEON
environment variable FORCE_SCALAR=1 overrides to scalar (used by the tests)
```

Other vectorised kernels, each with a scalar twin used as the oracle:

| Kernel | AVX2 intrinsic recipe |
| --- | --- |
| `fadeRows` (levels 1..3) | `_mm256_sub_epi32(c, _mm256_and_si256(_mm256_srli_epi32(c, 2), set1(0x003F3F3F)))` and the 50% / 25% variants; level 4 is `memset` |
| `tintRows` | same shift-mask-subtract pattern, 8 pixels per instruction |
| Keyed span copy (mountain strip) | `t = _mm256_cmpeq_epi32(_mm256_and_si256(src, set1(0xFF000000)), zero)`; `out = _mm256_blendv_epi8(src, dst, t)` |
| Sprite blit (scaled) | stays scalar: nearest-neighbour sampling needs a gather, which is slower than the scalar loop at these sizes |

### 8.4 Frame budget (3 GHz core, 320x240) and profiling

| Stage | Target |
| --- | --- |
| Tick (physics, traffic, collision, audio push) | under 25 us |
| Pass A + draw list | under 60 us |
| Pass B + sprites + tint, 4 threads, AVX2 | under 120 us wall (about 450 us single-thread scalar) |
| Cracks, frame, gauges, fade (serial) | under 100 us |
| `drawImage` scale to window | 0.3 to 1.5 ms, usually the largest item at 1080p and above |
| **Total frame** | **under 2 ms**, leaving over 14 ms of slack at 60 FPS |

The host-side `drawImage` scale is the real ceiling at high resolution. Keep `QImage::Format_RGB32` (matches the backing store, no conversion) and `WA_OpaquePaintEvent`; if profiling at 4K shows it dominating, render at 480x270 instead (same code, change `cfg::SCREEN_W/H`) rather than adding Qt calls.

`ScopedTimer` writes `std::chrono::steady_clock` deltas into a static ring buffer per stage; **F3** toggles a HUD line with the average and p99 of each stage and the per-slice `stats[s].ns`.

### 8.5 Unit verification checks

| # | Check | Pass criterion |
| --- | --- | --- |
| 8.a | Golden hash | A scripted 3000-frame drive hashed (FNV-1a over the framebuffer each frame) is identical for: 1 thread scalar, 4 threads scalar, 4 threads AVX2, and `FORCE_SCALAR=1` |
| 8.b | Fill exhaustive | For every pixel misalignment 0..31 and every `n` in 0..70: `fillPixelsAVX2` equals `std::fill`, and 16-pixel canary guards before and after the span are untouched |
| 8.c | Fade exhaustive | All 2^24 RGB values, levels 1 to 3: AVX2 result equals the scalar SWAR result |
| 8.d | Slice alignment | `static_assert` on slice byte size passes; `perf c2c` (Linux) shows no contended cache lines inside the framebuffer |
| 8.e | Scaling | Pass B + sprites speed-up from 1 to 4 threads is at least 2.5x on a 4-core machine |
| 8.f | Fork-join cost | Median wake-to-first-slice under 100 us; join overhead under 50 us; with workers forced to 0 the output is identical |
| 8.g | Soak | 10 minutes: frame time p99.9 under 18.5 ms, tick count equals 36,000 (+/- 2), zero allocations (debug `operator new` counter flat) |
| 8.h | Race freedom | 60 s of rendering under ThreadSanitizer reports nothing; shutdown joins all workers in under 100 ms with no deadlock |
| 8.i | Straggler safety | Inject a 5 ms sleep into one worker after it claims its last slice: the next frame still renders correctly and the golden hash is unchanged |

## Appendix: integration order, cross-phase amendments and risks

### A.1 Build order and gates

Generate and verify one phase at a time. Do not start a phase until every unit check of the previous one passes, because each phase assumes the earlier structs compile unchanged.

| Order | Phase | Needs | New files | Gate to pass before continuing |
| --- | --- | --- | --- | --- |
| 1 | 1 Harness | `config.h` | `framebuffer.h`, `gamewidget.*`, `main.cpp` | checks 1.a to 1.h, especially tick rate and no allocation |
| 2 | 2 Road | 1 | `track.*`, `roadrender.*`, `spanfill.h` | 2.a (flat-ground oracle) and 2.e (no seams) |
| 3 | 3 Geometry | 2 | `TrackBuilder`, mountain strip | 3.a, 3.d, 3.e (curve, slope, crest oracles) |
| 4 | 4 Sprites | 3 | `sprites.*` | 4.g (slice-identical output) |
| 5 | 5 Cockpit | 4 (blit helper) | `cockpit.*` | 5.b (masking) |
| 6 | 6 Vehicle | 3, 4, 5 | `vehicle.*`, `collision.*` | 6.d, 6.f, 6.g (drift oracle, no tunnelling, crash cycle) |
| 7 | 7 Audio | 6 (`AudioParams`) | `audio.*` | 7.a, 7.c (pitch law, headroom) |
| 8 | 8 Threads and SIMD | all | `jobs.*`, `spanfill_*.cpp` | 8.a (golden hash across all configurations) |

Before Phase 8, record the **golden hash** from the single-threaded scalar build. Phase 8's main acceptance test is that every optimised configuration reproduces it exactly.

### A.2 Amendments that later phases make to earlier ones

Paste these into the prompt for the earlier phase if it is regenerated after the later one exists.

| Where | Change | Introduced in |
| --- | --- | --- |
| `config.h` | `NUM_SLICES = 8` (was 4) | Phase 8 |
| `Camera` | adds `float shiftX` (pixels, added to `W/2` in projection) | Phases 5 and 6 |
| `RoadPoint` | adds `wx`, `wy` (camera-space lateral offset and height) | Phase 4 |
| `RenderContext` | adds `clipY[DRAW_DIST]`, `topRoadY` | Phase 3 |
| `fillRows` | tests `ROW_INVALID` first, then sky or fog | Phase 3 |
| `CameraFx::update` | gains an `offroad` argument | Phase 6 |
| `Track` | adds `Scenery scenery[MAX_SCENERY]` | Phase 4 |
| `GameWidget` | calls `AudioEngine::push()` each tick; `M` toggles mute | Phase 7 |

### A.3 Risk register

| Risk | Effect | Mitigation |
| --- | --- | --- |
| `drawImage` scaling dominates at 4K | frame time above 2 ms | integer scale factors, `Format_RGB32`, `WA_OpaquePaintEvent`; fall back to 480x270 internal resolution |
| QWidget raster paths are not vsynced | visible tearing or 16/17 ms judder | deadline-based timer with accumulator (1.4); optional `QWindow` with swap interval is out of scope for this blueprint |
| Audio callback thread differs by backend | torn parameters | relaxed atomics only, no shared structs (7.1) |
| Float precision at large Z | road shimmer after long runs | camera Z always wrapped into `[0, length)`; boundary depth computed relative to the camera (2.3) |
| Hill steepness above about 25% grade | inverted trapezoids, flicker | `hill::HIGH` capped at 2400 wu over 60 segments; check 3.f asserts `hw > 0` |
| Hybrid CPUs (performance/efficiency cores) | one slow worker stalls the join | 8 slices over 3 to 4 threads with dynamic claiming (8.2) |
| AVX2 absent or compiler flags leak | crash on older CPUs | AVX2 code in its own translation unit, runtime dispatch, `FORCE_SCALAR` test path |
| Qt Multimedia API differences | `QAudioSink` needs Qt 6.2 or later | target Qt 6.5 LTS or newer; the `QIODevice` design is unchanged across 6.x |

### A.4 Feeding a phase to a code-generation model

Send, in this order: section 0.2 (`config.h`), the struct listings of every earlier phase **as they were finally accepted** (not as drafted here), the single phase section including its unit-check table, and the template from 0.6. Ask for the files named in 0.4 for that phase plus a `tests_phaseN.cpp` that implements every row of the check table as an assertion, so each phase ships its own pass/fail evidence.

### 6.4 Swept collision

The fastest the player moves is `MAX_SPEED * STEP` = 200 wu per tick, so tests must be swept along Z. Work in the player's frame: let `r` be the signed, wrap-safe distance from the player's camera to an object's rear edge (positive = ahead).

```text
wrapDist(a, b) = ((a - b + L/2) mod L) - L/2           // L = track.length

detectCollision(v, prevZ, prevX, track, atlas, traffic):
    // 1. traffic: relative sweep
    for each active car i:
        r0 = wrapDist(traffic.prevZ[i], prevZ);   r1 = wrapDist(car.z, v.z)
        lo = min(r0, r1);  hi = max(r0, r1)
        if hi <= -OTHER_LEN or lo >= PLAYER_LEN:  continue          // no z interval overlap, OTHER_LEN = img.worldH*0.6
        dx = abs(v.x - car.offset);  lim = PLAYER_HALF_W + img.worldW / (2*ROAD_HALF_W)
        if dx >= lim:  continue
        vrel = v.speed - car.speed
        f    = (lim - dx) / lim
        return Traffic, sev = clamp(vrel / (0.6*MAX_SPEED) * (0.4 + 0.6*f), 0, 1), dir = sign(v.x - car.offset)
    // 2. scenery: only segments crossed this tick (at most 3)
    sA = floor(prevZ / SEG_LEN);  sB = floor(v.z / SEG_LEN) + 1       // add L to v.z first if the lap wrapped this tick
    for s in sA..sB:  for each Scenery item in segs[s mod count] where SOLID[item.spriteId]:
        halfN = img.worldW * TRUNK_FRAC[spriteId] / (2*ROAD_HALF_W)     // trunk only, e.g. 0.3 for trees
        r = wrapDist(s * SEG_LEN, v.z);   rPrev = wrapDist(s * SEG_LEN, prevZ)
        if not (min(r, rPrev) < PLAYER_LEN and max(r, rPrev) > -120):  continue
        dx = abs(v.x - item.offset);  if dx >= PLAYER_HALF_W + halfN:  continue
        f = (PLAYER_HALF_W + halfN - dx) / (PLAYER_HALF_W + halfN)
        return Scenery, sev = clamp(speedFrac * (0.4 + 0.6*f), 0, 1), dir = sign(v.x - item.offset)
    // 3. wall
    if abs(v.x) >= WALL_X:  return Wall, sev = speedFrac * 0.5, dir = -sign(v.x)
    return None
```

`TRUNK_FRAC`: tree 0.30, pine 0.25, lamp 0.50, sign 0.15 (posts), rock 0.8, bush 0 (not solid). The first matching traffic hit wins; hits are skipped while `invulnT > 0`.

**Response** (`sev >= 0.12` is a crash, below that a scrape):

```text
scrape: speed *= 0.85;  latV += dir*1.0;  audio.scrapeCount++;  fx.impact(3)
crash : speed *= (0.45 - 0.35*sev);  latV += dir*(1.5 + 2.0*sev);  damage = min(1, damage + 0.25 + 0.5*sev)
        spinDir = dir (or +1 if 0);  audio.crashCount++;  state = Impact;  stateT = 0
        fx.impact(6 + 6*sev);  cockpit.flash = 1;  cockpit.triggerCrack(160 + 60*dir*rand01, 70 + 40*rand01, ++crashSeq)
```

### 6.5 Crash state machine

| State | Duration | Behaviour | Exit |
| --- | --- | --- | --- |
| Driving | open | full controls; collisions active when `invulnT == 0` | crash response, then Impact |
| Impact | 0.25 s | steering and throttle ignored; speed bleeds at 6000 wu/s^2; flash decays; cracks grow over 12 ticks | after 0.25 s, Spin |
| Spin | up to 1.6 s | view sway (formulas below); speed decays at 6000 wu/s^2; lateral slide with `GRIP_DAMP` | `speed < 400` or `stateT > 1.6`, then FadeOut |
| FadeOut | 0.4 s | `fade` 0 to 1 linearly; engine pitch drops; sway keeps decaying | `fade == 1`, then respawn placement and Respawn |
| Respawn | 0.6 s | `fade` 1 to 0; `invulnT = 2.0 s`; steering enabled after 0.3 s | after 0.6 s, Driving |

View sway during Spin (`t` = `stateT`):

```latex
yaw_{px} = spinDir\cdot 80\,e^{-t/0.7}\sin(2\pi\cdot 1.1\,t),\qquad tumble_{px} = 14\,e^{-t/0.6}\sin(2\pi\cdot 1.7\,t)
```

**Respawn placement** (done on the black frame): `speed = 0`, `latV = 0`, `yawPx = tumblePx = 0`, cracks cleared, `z -= 400` (so a solid object is not still under the bumper), then pick the lane centre `-1 + (2*i + 1)/LANES` that maximises the distance to the nearest car inside the window from -1500 to +4000 wu, and set `x` to it. The whole crash cycle takes about 2.65 to 3.05 s.

`fade` is rendered by the Phase 8 fade pass with levels 0 to 4: `c - ((c >> 2) & 0x3F3F3F)` (75%), `(c >> 1) & 0x7F7F7F` (50%), `(c >> 2) & 0x3F3F3F` (25%), and black.

### 6.6 Traffic update

```text
updateTraffic(t, player, track):
    for each active car:  prevZ = z;  z = wrap(z + speed * STEP)
        d = wrapDist(z, player.z)
        if d < -3000 or d > (DRAW_DIST + 40) * SEG_LEN:       // behind the player or too far ahead: recycle
            lane = rand(LANES);  offset = -1 + (2*lane + 1)/LANES
            z = player.z + (40 + rand(140)) * SEG_LEN;  retry up to 8 times until no car in the same lane within 12*SEG_LEN
            speed = laneSpeed[lane]                           // lanes 0..2 = 0.28, 0.36, 0.44 times MAX_SPEED
            spriteId = pick(SPR_CAR_RED, SPR_CAR_BLUE, SPR_CAR_YELLOW, SPR_TRUCK)
```

Because speed is constant **per lane** and below any player cruise speed, traffic never needs car-to-car collision.

### 6.7 Tick order (mandatory)

```text
1. prevZ = z, prevX = x, traffic.prevZ[] snapshot
2. read input; smooth steer           (ignored in Impact/Spin/FadeOut)
3. state-specific longitudinal + lateral integration (6.3)
4. z += speed*STEP (wrap);  x += latV*STEP;  x = clamp(x, -WALL_X, WALL_X)
5. updateTraffic
6. collision (Driving only, invulnT == 0): detectCollision, apply response
7. state machine transition (6.5);  invulnT -= STEP
8. gearbox and rpm;  fill audio, cockpit, camera effects
```

`fillCamera` writes `cam.x = v.x * ROAD_HALF_W`, `cam.y = CAM_HEIGHT + segment height`, `cam.pitchPx = slopePitch + fx.shakeY + v.tumblePx` and `cam.shiftX = fx.shakeX + v.yawPx`. A pure yaw is a uniform horizontal image shift of `F * tan(yaw)` px for small angles, which is why the sway needs no extra rendering code. `CameraFx::update` gains a fourth argument `offroad` (0 or 1) that adds `1.2 * f * sin(2*pi*37*t)` px of vertical rumble.

### 6.8 Unit verification checks

| # | Check | Pass criterion |
| --- | --- | --- |
| 6.a | Acceleration | Full throttle from rest on flat road: speed at 2.5 s is 7587 wu/s (+/- 3%) |
| 6.b | Top speed and braking | Never above 12,600 wu/s; braking from 12,000 reaches 0 in 1.0 s (+/- 0.05) |
| 6.c | Off-road | Throttle held and off the road edge: speed settles at 3000 wu/s within 3 s and never drops below it |
| 6.d | Drift oracle | Curve 3.0, `f = 1`, `steer = 0`, start `x = 0`: `x(1 s)` is -0.615 (+/- 0.03); positive curve drifts left |
| 6.e | Counter-steer | Curve 1.5 at top speed: constant `steer` of about 0.5 holds `x` within +/-0.05 |
| 6.f | No tunnelling | 10,000 random trials (closing speed up to 12,000 wu/s, random lateral overlap): `detectCollision` agrees with a brute-force 1/16-tick sub-stepped test in every trial |
| 6.g | Crash cycle | Scripted crash: states occur in order Driving, Impact, Spin, FadeOut, Respawn, Driving; total 2.65 to 3.05 s |
| 6.h | Respawn safety | After respawn: `speed == 0`, no car in the chosen lane within the -1500 to +4000 wu window, collisions ignored for 2.0 s |
| 6.i | Gearbox | At most one shift per 0.3 s; `rpm` always within 900 to 8500; `shiftCount` equals the number of gear changes |
| 6.j | Determinism | Replaying the same 60 s input script twice gives identical hashes of all `Vehicle` and `Traffic` state |
| 6.k | Budget | Whole `tick` under 20 us; zero allocations |
