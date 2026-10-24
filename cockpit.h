#pragma once
#include <cstdint>
#include "config.h"
#include "framebuffer.h"

struct Run {
    int16_t  x0;
    int16_t  x1;
    uint32_t color;
};

struct RowRuns {
    uint8_t n = 0;
    Run     r[6];
};

struct CockpitState {
    float speedKmh = 0.0f;
    float rpm      = 900.0f;
    float steer    = 0.0f;    // in [-1, 1]
    float crack    = 0.0f;    // 0..1 growth of glass cracks
    float flash    = 0.0f;    // 0..1 white impact flash
    float damage   = 0.0f;    // 0..1, tints the dash lamps
    int   gear     = 1;       // 1..5
};

struct CrackSeg {
    int16_t x0, y0, x1, y1;
};

struct CrackSet {
    CrackSeg s[160];
    int      count = 0;
};

struct CameraFx {
    float shakeX = 0.0f;
    float shakeY = 0.0f;
    float impactAmp = 0.0f;
    float impactT   = 0.0f;
    float accelPitch = 0.0f;
    float phase      = 0.0f;

    void impact(float ampPx);
    void update(float speedFrac, float rpmFrac, float accelFrac, float offroad = 0.0f);
};

class Cockpit {
public:
    void init();                                            // builds run table, dial decals, LUTs
    void update(const CockpitState& s, const CameraFx& fx); // smoothing only; no pixels
    void tint(Framebuffer& fb);                             // full windshield visor gradient
    void tintRows(Framebuffer& fb, int yBegin, int yEnd);   // slice-aware visor gradient
    void drawCracks(Framebuffer& fb);                       // BEFORE the frame so the frame masks them
    void drawFrame(Framebuffer& fb);                        // runs + decals
    void drawGauges(Framebuffer& fb);                       // needles, digits, wheel, flash
    void triggerCrack(int impactX, int impactY, uint32_t seed);
    void clearCracks();

    // Bitmap text & HUD primitives (pure software rasterizer, zero Qt calls)
    static void drawText(Framebuffer& fb, int x, int y, const char* text, uint32_t color, int scale = 1);
    static void drawRect(Framebuffer& fb, int x, int y, int w, int h, uint32_t color);
    static void thickLine(Framebuffer& fb, int x0, int y0, int x1, int y1, int halfW, uint32_t color,
                          int minY = 0, int maxY = cfg::SCREEN_H);

    int ox() const { return ox_; }
    int oy() const { return oy_; }
    const CrackSet& cracks() const { return cracks_; }
    float dispSpeed() const { return dispSpeed_; }
    float dispRpm() const { return dispRpm_; }
    float dispSteer() const { return dispSteer_; }

private:
    RowRuns      rows_[cfg::SCREEN_H];
    float        dispSpeed_ = 0.0f;
    float        dispRpm_   = 900.0f;
    float        dispSteer_ = 0.0f;
    int          ox_ = 0;
    int          oy_ = 0;            // interior shake offset, clamped to +-4
    CrackSet     cracks_;
    float        crackProgress_ = 0.0f;
    CockpitState st_;

    alignas(64) uint32_t tachDecal_[56 * 56];
    alignas(64) uint32_t speedDecal_[56 * 56];
    alignas(64) uint32_t consoleDecal_[80 * 40];
};
