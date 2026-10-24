#pragma once
#include <cstdint>
#include "config.h"
#include "framebuffer.h"
#include "input.h"
#include "track.h"
#include "camera.h"
#include "roadrender.h"
#include "sprites.h"
#include "cockpit.h"
#include "vehicle.h"
#include "jobs.h"
#include "highscores.h"

struct StageTimers {
    FrameStats tickUs;
    FrameStats passAUs;
    FrameStats passBUs;
    FrameStats overlayUs;
    FrameStats totalMs;
};

enum class ScreenState : uint8_t {
    LaunchMenu = 0,
    Countdown,
    Racing,
    Paused,
    Wrecked,
    RaceOver
};

enum class GameMode : uint8_t {
    ArcadeCheckpoint = 0, // Race against checkpoint timer (+15s per checkpoint)
    GrandPrix3Lap,        // 3-Lap Championship race vs 24 rivals
    EndlessFreeRun        // Relaxed endless driving
};

enum class ScaleMode : uint8_t {
    FullStretch = 0,      // Fills 100% of window / fullscreen (default)
    AspectFit,            // Smooth 4:3 aspect-ratio fit
    IntegerScale          // Pixel-perfect integer multiple scaling
};

class Game {
public:
    void init(int workerCount = -1, bool forceScalar = false);
    void shutdown();
    void tick(const InputState& in);          // fixed STEP; NO allocation, NO pixel writes
    void render();                            // writes fb and displayFb; NO allocation

    // Menu & game flow controls
    void enterLaunchMenu();
    void startNewRace();
    void repairAndContinue();
    void fieldRepair();
    void selectTrack(int presetIdx);
    void selectCar(int carIdx);
    void selectTrafficDensity(int densityIdx);
    void toggleSteerAssist();
    void menuMove(int delta);
    void menuAdjust(int dir, bool& outToggleFullscreen, bool& outQuit);
    void menuActivate(bool& outToggleFullscreen, bool& outQuit);
    void handleMouseMove(int normX960, int normY720);
    void handleMouseClick(int normX960, int normY720, bool& outToggleFullscreen, bool& outQuit);

    // Mode & feature toggles
    void setTheme(int theme);
    int  theme() const { return ctx.theme; }
    void cycleTheme() { setTheme((ctx.theme + 1) % 5); }
    void togglePause();
    void toggleDebugHud() { showDebugHud = !showDebugHud; }
    void toggleMirror() { showMirror = !showMirror; }
    void toggleWipers() { wipersOn = !wipersOn; }
    void toggleWeather() { weatherMode = (weatherMode + 1) % 3; }
    void toggleMinimap() { gpsMode = (gpsMode + 1) % 3; }
    void toggleGapGuide() { showGapGuide = !showGapGuide; }
    void cyclePixelSize(int dir);
    void setPixelSizePreset(int presetIdx);
    void cycleScaleMode() { scaleMode = static_cast<ScaleMode>((static_cast<int>(scaleMode) + 1) % 3); }

    alignas(64) Framebuffer    fb;
    alignas(64) DisplayBuffer  displayFb;
    alignas(64) RenderContext  ctx;
    alignas(64) Track          track;
    alignas(64) SpriteAtlas    atlas;
    alignas(64) DrawList       drawList;
    Cockpit          cockpit;
    CameraFx         camFx;
    Camera           cam;
    Vehicle          vehicle;
    Traffic          traffic;
    SliceScheduler   scheduler;
    StageTimers      timers;
    HighScoreManager highScores;

    uint64_t    tickCount       = 0;
    ScreenState screenState     = ScreenState::Racing;
    GameMode    gameMode        = GameMode::ArcadeCheckpoint;
    ScaleMode   scaleMode       = ScaleMode::FullStretch;
    bool        isFullscreen    = false;

    int         selectedTrack   = 0;
    int         selectedCar     = 0;
    int         selectedDensity = 1;      // 0 = Low, 1 = Medium (default gameplay), 2 = High
    bool        steerAssistOn   = false;  // OFF by default so car doesn't auto-turn unless enabled
    bool        showGapGuide    = true;   // AR Passing-Gap & Trajectory Rails on road
    int         menuCursor      = 0;
    int         pauseCursor     = 0;
    int         pixelSizeIdx    = 0;      // 0=1px(960x720), 1=2px(480x360), 2=3px(320x240), 3=4px(240x180), 4=6px(160x120)
    int         pixelSize       = 1;      // effective pixel block size in 960x720 DisplayBuffer
    bool        crtScanlines    = false;

    // Gameplay telemetry, score, combo & checkpoint state
    uint32_t    score           = 0;
    float       scoreAccum      = 0.0f;
    int         comboMult       = 1;
    float       comboTimer      = 0.0f;
    float       timeLeftSec     = 45.0f;
    float       countdownTimer  = 3.0f;
    float       raceMaxSpeedKmh = 0.0f;
    int         racePosition    = 25;
    int         lastCheckpointSeg = -1;
    int         lastNearMisses  = 0;
    bool        newHighScoreAchieved = false;
    bool        newBestLapAchieved   = false;

    // Floating HUD event banner
    char        bannerText[48]  = {};
    uint32_t    bannerColor     = 0xFFFFFFFFu;
    float       bannerTimer     = 0.0f;

    bool        paused          = false;
    bool        testMode        = false;  // when true, runs exact blueprint configuration for unit tests
    bool        showDebugHud    = false;  // F3 toggle
    bool        showMirror      = true;   // V toggle
    int         gpsMode         = 0;      // 0 = Rotating Radar + Overview, 1 = Large Radar, 2 = Off (Tab toggle)
    bool        wipersOn        = false;  // X toggle
    int         weatherMode     = 0;      // 0 = Clear, 1 = Rain, 2 = Snow (C toggle)
    float       wiperPhase      = 0.0f;

private:
    void triggerBanner(const char* txt, uint32_t col, float duration = 1.8f);
    void drawRoadHeadlightsAndSheen();
    void draw3DCheckpointArches();
    void drawRearViewMirror();
    void drawWeatherAndWipers();
    void drawHudOverlay();
    void renderArPassingGapAndTargets();
    void renderModernCockpitDetails();
    void renderHighResHudAndGps();
    void renderGpsMap(int boxX, int boxY, int boxW, int boxH, bool rotatingRadar);
    void renderRallyCallout();
    void renderLaunchScreen();
    void renderCountdownOverlay();
    void renderPauseOverlay();
    void renderWreckedOverlay();
    void renderRaceOverOverlay();
};
