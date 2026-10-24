#include "game.h"
#include "spanfill.h"
#include "mathutil.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <chrono>
#include <algorithm>

namespace {
inline constexpr int PIXEL_PRESETS[5] = { 1, 2, 3, 4, 6 };
const char* PIXEL_PRESET_NAMES[5] = {
    "1PX ULTRA HD (960X720)",
    "2PX HIGH (480X360)",
    "3PX CLASSIC (320X240)",
    "4PX RETRO (240X180)",
    "6PX CHUNKY (160X120)"
};
const char* MODE_NAMES[3] = {
    "ARCADE CHECKPOINT",
    "GRAND PRIX (3 LAPS)",
    "ENDLESS FREE RUN"
};
const char* SCALE_NAMES[3] = {
    "FULL WINDOW STRETCH",
    "4:3 ASPECT FIT",
    "INTEGER PIXEL-PERFECT"
};
inline constexpr int LAUNCH_MENU_ITEMS = 8;
inline constexpr int PAUSE_MENU_ITEMS  = 5;
} // namespace

void Game::init(int workerCount, bool forceScalar) {
    assert(reinterpret_cast<uintptr_t>(fb.px) % 64 == 0);
    assert(reinterpret_cast<uintptr_t>(displayFb.px) % 64 == 0);

    initSpanFill(forceScalar);
    highScores.init("highscores.dat");
    buildSpriteAtlas(atlas);

    selectedTrack = 0;
    buildDemoTrack(track);
    initRenderContext(ctx, 0);
    cockpit.init();
    camFx = CameraFx{};
    VehiclePhysics::reset(vehicle, track, traffic);
    VehiclePhysics::fillCamera(vehicle, track, camFx, cam);
    scheduler.init(workerCount);

    fb.clear(0xFF000000u);
    displayFb.clear(0xFF000000u);

    tickCount       = 0;
    paused          = false;
    screenState     = ScreenState::Racing; // default for unit tests; GameWidget calls enterLaunchMenu()
    gameMode        = GameMode::ArcadeCheckpoint;
    scaleMode       = ScaleMode::FullStretch;
    pixelSizeIdx    = 0;
    pixelSize       = 1;
    crtScanlines    = false;

    score           = 0;
    scoreAccum      = 0.0f;
    comboMult       = 1;
    comboTimer      = 0.0f;
    timeLeftSec     = 45.0f;
    countdownTimer  = 3.0f;
    raceMaxSpeedKmh = 0.0f;
    racePosition    = 25;
    lastCheckpointSeg = 0;
    newHighScoreAchieved = false;
    newBestLapAchieved   = false;
    bannerTimer     = 0.0f;
    wiperPhase      = 0.0f;
}

void Game::shutdown() {
    scheduler.shutdown();
}

void Game::enterLaunchMenu() {
    screenState = ScreenState::LaunchMenu;
    paused      = false;
    menuCursor  = 0;
    selectTrack(selectedTrack);
}

void Game::selectTrack(int presetIdx) {
    selectedTrack = ((presetIdx % NUM_TRACK_PRESETS) + NUM_TRACK_PRESETS) % NUM_TRACK_PRESETS;
    const TrackMetadata& meta = getTrackMetadata(selectedTrack);
    buildTrackPreset(track, selectedTrack);
    initRenderContext(ctx, meta.defaultTheme);
    weatherMode = meta.defaultWeather;
    wipersOn    = (weatherMode != 0);
    VehiclePhysics::reset(vehicle, track, traffic);
    for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) {
        traffic.cars[i].speed *= meta.trafficSpeedMult;
    }
    cockpit.clearCracks();
    camFx = CameraFx{};
    VehiclePhysics::fillCamera(vehicle, track, camFx, cam);
}

void Game::startNewRace() {
    selectTrack(selectedTrack);
    score           = 0;
    scoreAccum      = 0.0f;
    comboMult       = 1;
    comboTimer      = 0.0f;
    timeLeftSec     = (gameMode == GameMode::ArcadeCheckpoint) ? 45.0f : 999.0f;
    countdownTimer  = 3.2f;
    raceMaxSpeedKmh = 0.0f;
    racePosition    = 25;
    lastCheckpointSeg = 0;
    newHighScoreAchieved = false;
    newBestLapAchieved   = false;
    paused          = false;
    screenState     = ScreenState::Countdown;
    triggerBanner("GET READY!", RGB(255, 220, 60), 2.0f);
}

void Game::setPixelSizePreset(int presetIdx) {
    pixelSizeIdx = ((presetIdx % 5) + 5) % 5;
    pixelSize    = PIXEL_PRESETS[pixelSizeIdx];
}

void Game::cyclePixelSize(int dir) {
    setPixelSizePreset(pixelSizeIdx + dir);
    char msg[48];
    std::snprintf(msg, sizeof(msg), "QUALITY: %s", PIXEL_PRESET_NAMES[pixelSizeIdx]);
    triggerBanner(msg, RGB(80, 240, 255), 1.8f);
}

void Game::setTheme(int t) {
    float savedBgX = ctx.bgX;
    initRenderContext(ctx, ((t % 5) + 5) % 5);
    ctx.bgX = savedBgX;
}

void Game::togglePause() {
    if (screenState == ScreenState::Racing) {
        screenState = ScreenState::Paused;
        paused      = true;
        pauseCursor = 0;
    } else if (screenState == ScreenState::Paused) {
        screenState = ScreenState::Racing;
        paused      = false;
    }
}

void Game::triggerBanner(const char* txt, uint32_t col, float duration) {
    if (!txt) return;
    std::snprintf(bannerText, sizeof(bannerText), "%s", txt);
    bannerColor = col;
    bannerTimer = duration;
}

void Game::menuMove(int delta) {
    if (screenState == ScreenState::LaunchMenu) {
        menuCursor = ((menuCursor + delta) % LAUNCH_MENU_ITEMS + LAUNCH_MENU_ITEMS) % LAUNCH_MENU_ITEMS;
        vehicle.audio.shiftCount++;
    } else if (screenState == ScreenState::Paused) {
        pauseCursor = ((pauseCursor + delta) % PAUSE_MENU_ITEMS + PAUSE_MENU_ITEMS) % PAUSE_MENU_ITEMS;
        vehicle.audio.shiftCount++;
    }
}

void Game::menuAdjust(int dir, bool& outToggleFullscreen, bool& outQuit) {
    outToggleFullscreen = false;
    outQuit = false;
    if (screenState == ScreenState::LaunchMenu) {
        switch (menuCursor) {
            case 0: // START RACE
                if (dir > 0) startNewRace();
                break;
            case 1: // TRACK
                selectTrack(selectedTrack + dir);
                break;
            case 2: // GAME MODE
                gameMode = static_cast<GameMode>(((static_cast<int>(gameMode) + dir) % 3 + 3) % 3);
                break;
            case 3: // PIXEL SIZE
                setPixelSizePreset(pixelSizeIdx + dir);
                break;
            case 4: // DISPLAY SCALING
                scaleMode = static_cast<ScaleMode>(((static_cast<int>(scaleMode) + dir) % 3 + 3) % 3);
                break;
            case 5: // FULLSCREEN
                outToggleFullscreen = true;
                break;
            case 6: // CRT SCANLINES
                crtScanlines = !crtScanlines;
                break;
            case 7: // QUIT
                break;
        }
        vehicle.audio.shiftCount++;
    } else if (screenState == ScreenState::Paused) {
        if (pauseCursor == 2) {
            setPixelSizePreset(pixelSizeIdx + dir);
            vehicle.audio.shiftCount++;
        }
    }
}

void Game::menuActivate(bool& outToggleFullscreen, bool& outQuit) {
    outToggleFullscreen = false;
    outQuit = false;
    if (screenState == ScreenState::LaunchMenu) {
        switch (menuCursor) {
            case 0: startNewRace(); break;
            case 1: selectTrack(selectedTrack + 1); break;
            case 2: gameMode = static_cast<GameMode>((static_cast<int>(gameMode) + 1) % 3); break;
            case 3: setPixelSizePreset(pixelSizeIdx + 1); break;
            case 4: cycleScaleMode(); break;
            case 5: outToggleFullscreen = true; break;
            case 6: crtScanlines = !crtScanlines; break;
            case 7: outQuit = true; break;
        }
        vehicle.audio.shiftCount++;
    } else if (screenState == ScreenState::Paused) {
        switch (pauseCursor) {
            case 0: togglePause(); break;
            case 1: startNewRace(); break;
            case 2: setPixelSizePreset(pixelSizeIdx + 1); break;
            case 3: outToggleFullscreen = true; break;
            case 4: enterLaunchMenu(); break;
        }
        vehicle.audio.shiftCount++;
    } else if (screenState == ScreenState::RaceOver) {
        enterLaunchMenu();
    }
}

void Game::handleMouseMove(int normX960, int normY720) {
    if (screenState == ScreenState::LaunchMenu) {
        if (normX960 >= 50 && normX960 <= 500 && normY720 >= 230 && normY720 < 230 + LAUNCH_MENU_ITEMS * 48) {
            int idx = (normY720 - 230) / 48;
            if (idx >= 0 && idx < LAUNCH_MENU_ITEMS) menuCursor = idx;
        }
    } else if (screenState == ScreenState::Paused) {
        if (normX960 >= 280 && normX960 <= 680 && normY720 >= 250 && normY720 < 250 + PAUSE_MENU_ITEMS * 46) {
            int idx = (normY720 - 250) / 46;
            if (idx >= 0 && idx < PAUSE_MENU_ITEMS) pauseCursor = idx;
        }
    }
}

void Game::handleMouseClick(int normX960, int normY720, bool& outToggleFullscreen, bool& outQuit) {
    handleMouseMove(normX960, normY720);
    if (screenState == ScreenState::LaunchMenu) {
        if (normX960 >= 50 && normX960 <= 500 && normY720 >= 230 && normY720 < 230 + LAUNCH_MENU_ITEMS * 48) {
            menuActivate(outToggleFullscreen, outQuit);
        }
    } else if (screenState == ScreenState::Paused) {
        if (normX960 >= 280 && normX960 <= 680 && normY720 >= 250 && normY720 < 250 + PAUSE_MENU_ITEMS * 46) {
            menuActivate(outToggleFullscreen, outQuit);
        }
    } else if (screenState == ScreenState::RaceOver) {
        enterLaunchMenu();
    }
}

void Game::tick(const InputState& in) {
    auto t0 = std::chrono::steady_clock::now();

    // Unit test mode runs pure Phase 1-8 physics tick without menu state interception
    if (testMode) {
        if (in.justPressed(InputState::PAUSE)) paused = !paused;
        if (in.justPressed(InputState::RESET)) {
            VehiclePhysics::reset(vehicle, track, traffic);
            cockpit.clearCracks();
            camFx = CameraFx{};
        }
        if (!paused) {
            VehiclePhysics::tick(vehicle, in, track, atlas, traffic, &camFx, &cockpit);
            VehiclePhysics::fillCamera(vehicle, track, camFx, cam);
            int segIdx = (track.count > 0)
                ? (static_cast<int>(std::floor(mathutil::wrap(vehicle.z, track.length) / cfg::SEG_LEN)) % track.count)
                : 0;
            float speedFrac = vehicle.speed / cfg::MAX_SPEED;
            ctx.bgX = mathutil::wrap(ctx.bgX + track.segs[segIdx].curve * speedFrac * 1.2f, 512.0f);
            tickCount++;
        }
        auto t1 = std::chrono::steady_clock::now();
        timers.tickUs.push(std::chrono::duration<float, std::micro>(t1 - t0).count());
        return;
    }

    if (bannerTimer > 0.0f) {
        bannerTimer = std::max(0.0f, bannerTimer - cfg::STEP);
    }

    // 1. Launch Menu autopilot background flythrough
    if (screenState == ScreenState::LaunchMenu) {
        InputState autoIn;
        autoIn.down = InputState::ACCEL;
        int segIdx = (track.count > 0)
            ? (static_cast<int>(std::floor(mathutil::wrap(vehicle.z, track.length) / cfg::SEG_LEN)) % track.count)
            : 0;
        float c = track.segs[segIdx].curve;
        if (c > 0.2f || vehicle.x < -0.25f) autoIn.down |= InputState::RIGHT;
        if (c < -0.2f || vehicle.x > +0.25f) autoIn.down |= InputState::LEFT;

        vehicle.invulnT = 2.0f; // immune during attract-mode flythrough
        VehiclePhysics::tick(vehicle, autoIn, track, atlas, traffic, &camFx, &cockpit);
        vehicle.speed = std::min(vehicle.speed, 0.55f * cfg::MAX_SPEED);
        vehicle.audio.rpm = 1400.0f;
        vehicle.audio.throttle = 0.0f;
        vehicle.audio.skid = 0.0f;
        VehiclePhysics::fillCamera(vehicle, track, camFx, cam);
        ctx.bgX = mathutil::wrap(ctx.bgX + c * 0.6f, 512.0f);
        tickCount++;
        auto t1 = std::chrono::steady_clock::now();
        timers.tickUs.push(std::chrono::duration<float, std::micro>(t1 - t0).count());
        return;
    }

    // 2. Starting Grid Countdown (3.. 2.. 1.. GO!)
    if (screenState == ScreenState::Countdown) {
        float prevCd = countdownTimer;
        countdownTimer -= cfg::STEP;
        if (std::floor(prevCd) != std::floor(countdownTimer) && countdownTimer > 0.0f) {
            vehicle.audio.shiftCount++;
        }
        bool revThrottle = in.has(InputState::ACCEL);
        vehicle.rpm = mathutil::lerp(vehicle.rpm, revThrottle ? 5800.0f : 1200.0f, 0.15f);
        vehicle.audio.rpm = vehicle.rpm;
        vehicle.audio.throttle = revThrottle ? 0.6f : 0.0f;
        camFx.update(0.0f, (vehicle.rpm - 900.0f) / 7600.0f, 0.0f, 0.0f);
        CockpitState cs;
        VehiclePhysics::fillCockpit(vehicle, cs);
        cockpit.update(cs, camFx);
        VehiclePhysics::fillCamera(vehicle, track, camFx, cam);

        if (countdownTimer <= 0.0f) {
            screenState = ScreenState::Racing;
            triggerBanner("GO! GO! GO!", RGB(60, 255, 120), 1.5f);
            vehicle.audio.shiftCount++;
        }
        tickCount++;
        auto t1 = std::chrono::steady_clock::now();
        timers.tickUs.push(std::chrono::duration<float, std::micro>(t1 - t0).count());
        return;
    }

    // 3. Race Over state
    if (screenState == ScreenState::RaceOver) {
        InputState coastIn;
        coastIn.down = InputState::BRAKE;
        vehicle.invulnT = 2.0f;
        VehiclePhysics::tick(vehicle, coastIn, track, atlas, traffic, &camFx, &cockpit);
        VehiclePhysics::fillCamera(vehicle, track, camFx, cam);
        tickCount++;
        auto t1 = std::chrono::steady_clock::now();
        timers.tickUs.push(std::chrono::duration<float, std::micro>(t1 - t0).count());
        return;
    }

    // 4. Active Racing / Pause handling
    if (in.justPressed(InputState::PAUSE)) {
        togglePause();
    }
    if (in.justPressed(InputState::RESET)) {
        startNewRace();
    }
    if (in.justPressed(InputState::WIPER)) {
        wipersOn = !wipersOn;
    }

    if (!paused && screenState == ScreenState::Racing) {
        int prevOvertakes = vehicle.overtakes;
        int prevLap       = vehicle.lap;
        uint32_t prevCrashes = vehicle.audio.crashCount;

        VehiclePhysics::tick(vehicle, in, track, atlas, traffic, &camFx, &cockpit);
        VehiclePhysics::fillCamera(vehicle, track, camFx, cam);

        float spdKmh = vehicle.speed * cfg::KMH_PER_WU;
        raceMaxSpeedKmh = std::max(raceMaxSpeedKmh, spdKmh);

        // Reset combo on crash
        if (vehicle.audio.crashCount > prevCrashes) {
            comboMult  = 1;
            comboTimer = 0.0f;
            triggerBanner("CRASH! COMBO LOST", RGB(255, 70, 70), 1.8f);
        }

        // Overtake & Combo scoring
        int newPasses = vehicle.overtakes - prevOvertakes;
        if (newPasses > 0) {
            comboTimer = 4.5f;
            comboMult  = std::min(8, comboMult + newPasses);
            uint32_t pts = static_cast<uint32_t>(500 * newPasses * comboMult);
            score += pts;
            racePosition = std::max(1, 25 - vehicle.overtakes);
            char msg[48];
            std::snprintf(msg, sizeof(msg), "OVERTAKE +%u (%dX COMBO!)", pts, comboMult);
            triggerBanner(msg, RGB(80, 255, 160), 1.6f);
        }

        if (comboTimer > 0.0f) {
            comboTimer -= cfg::STEP;
            if (comboTimer <= 0.0f) comboMult = 1;
        }

        // Continuous speed score accumulation
        if (vehicle.state == CrashState::Driving && vehicle.speed > 1000.0f) {
            float spdFrac = vehicle.speed / cfg::MAX_SPEED;
            scoreAccum += (spdFrac * spdFrac * 45.0f * static_cast<float>(comboMult)) * cfg::STEP;
            uint32_t whole = static_cast<uint32_t>(scoreAccum);
            if (whole > 0) {
                score += whole;
                scoreAccum -= static_cast<float>(whole);
            }
        }

        // Checkpoint detection & time bonus
        int curSeg = (track.count > 0)
            ? (static_cast<int>(std::floor(mathutil::wrap(vehicle.z, track.length) / cfg::SEG_LEN)) % track.count)
            : 0;
        if (curSeg != lastCheckpointSeg) {
            // Check segments between lastCheckpointSeg and curSeg
            int stepCount = (curSeg - lastCheckpointSeg + track.count) % track.count;
            if (stepCount > 0 && stepCount < 20) {
                for (int s = 1; s <= stepCount; ++s) {
                    int checkSeg = (lastCheckpointSeg + s) % track.count;
                    if (track.segs[checkSeg].flags & 0x01) {
                        score += 2000u * static_cast<uint32_t>(comboMult);
                        if (gameMode == GameMode::ArcadeCheckpoint) {
                            timeLeftSec = std::min(99.0f, timeLeftSec + 14.0f);
                            triggerBanner("CHECKPOINT! +14.0S & +2000 PTS", RGB(0, 245, 255), 2.2f);
                        } else {
                            triggerBanner("SECTOR CHECKPOINT! +2000 PTS", RGB(0, 245, 255), 2.0f);
                        }
                        vehicle.audio.shiftCount++;
                    }
                }
            }
            lastCheckpointSeg = curSeg;
        }

        // Lap completion handling
        if (vehicle.lap > prevLap) {
            score += 5000u;
            bool newHS = false, newBL = false;
            highScores.submitResult(selectedTrack, score, vehicle.bestLapTime, raceMaxSpeedKmh,
                                    vehicle.overtakes, newHS, newBL);
            if (newHS) newHighScoreAchieved = true;
            if (newBL) newBestLapAchieved   = true;

            if (gameMode == GameMode::GrandPrix3Lap && vehicle.lap > 3) {
                screenState = ScreenState::RaceOver;
                triggerBanner("GRAND PRIX FINISHED!", RGB(255, 225, 60), 4.0f);
            } else {
                char msg[48];
                std::snprintf(msg, sizeof(msg), "LAP %d! +5000 PTS", vehicle.lap);
                triggerBanner(msg, RGB(255, 230, 80), 2.5f);
            }
        }

        // Live high-score check
        if (score > highScores.record(selectedTrack).highScore && !newHighScoreAchieved) {
            newHighScoreAchieved = true;
            triggerBanner("NEW TRACK HIGH SCORE!", RGB(255, 215, 0), 2.5f);
        }

        // Arcade Checkpoint timer countdown
        if (gameMode == GameMode::ArcadeCheckpoint) {
            timeLeftSec -= cfg::STEP;
            if (timeLeftSec <= 0.0f) {
                timeLeftSec = 0.0f;
                bool newHS = false, newBL = false;
                highScores.submitResult(selectedTrack, score, vehicle.bestLapTime, raceMaxSpeedKmh,
                                        vehicle.overtakes, newHS, newBL);
                if (newHS) newHighScoreAchieved = true;
                if (newBL) newBestLapAchieved   = true;
                screenState = ScreenState::RaceOver;
            }
        }

        // Mountain parallax scroll
        float speedFrac = vehicle.speed / cfg::MAX_SPEED;
        ctx.bgX = mathutil::wrap(ctx.bgX + track.segs[curSeg].curve * speedFrac * 1.2f, 512.0f);

        if (wipersOn || weatherMode != 0) {
            wiperPhase += cfg::STEP * 3.2f;
        }
        tickCount++;
    }

    auto t1 = std::chrono::steady_clock::now();
    timers.tickUs.push(std::chrono::duration<float, std::micro>(t1 - t0).count());
}

void Game::render() {
    auto frameStart = std::chrono::steady_clock::now();

    // 1. Prepare (Pass A road projection + depth-sorted sprite draw list)
    auto tA0 = std::chrono::steady_clock::now();
    projectRoad(track, cam, ctx);
    buildDrawList(track, traffic.cars, cfg::MAX_TRAFFIC, ctx, atlas, drawList);
    auto tA1 = std::chrono::steady_clock::now();
    timers.passAUs.push(std::chrono::duration<float, std::micro>(tA1 - tA0).count());

    // 2. Rasterize (Pass B + sprites + visor tint in parallel row slices)
    auto tB0 = std::chrono::steady_clock::now();
    FrameJob job{ &ctx, &drawList, &atlas, &track, &cockpit, &fb };
    scheduler.run(job);
    auto tB1 = std::chrono::steady_clock::now();
    timers.passBUs.push(std::chrono::duration<float, std::micro>(tB1 - tB0).count());

    // 3. Overlay on 320x240 base Framebuffer
    auto tO0 = std::chrono::steady_clock::now();
    if (!testMode) {
        draw3DCheckpointArches();
        if (weatherMode != 0 || wipersOn || vehicle.nitroActive) {
            drawWeatherAndWipers();
        }
    }
    cockpit.drawCracks(fb);
    cockpit.drawFrame(fb);
    cockpit.drawGauges(fb);

    if (!testMode && screenState != ScreenState::LaunchMenu) {
        if (showMirror) {
            drawRearViewMirror();
        }
        drawHudOverlay();
    }

    // Crash / Respawn fade pass (levels 0..4)
    if (vehicle.fade > 0.01f) {
        int level = mathutil::clamp(static_cast<int>(std::lround(vehicle.fade * 4.0f)), 1, 4);
        fadeRows(fb, 0, cfg::SCREEN_H, level);
    }

    // 4. High-Resolution 960x720 DisplayBuffer Upscale, Vector/Raster GPS, Menus & Variable Pixel Size
    if (!testMode) {
        displayFb.upscaleFromBase(fb, pixelSize);

        if (screenState == ScreenState::LaunchMenu) {
            renderLaunchScreen();
        } else {
            renderHighResHudAndGps();
            if (screenState == ScreenState::Countdown) {
                renderCountdownOverlay();
            } else if (screenState == ScreenState::Paused) {
                renderPauseOverlay();
            } else if (screenState == ScreenState::RaceOver) {
                renderRaceOverOverlay();
            }
        }

        displayFb.applyPostProcess(pixelSize, crtScanlines);
    }

    auto tO1 = std::chrono::steady_clock::now();
    timers.overlayUs.push(std::chrono::duration<float, std::micro>(tO1 - tO0).count());
    timers.totalMs.push(std::chrono::duration<float, std::milli>(tO1 - frameStart).count());
}

void Game::draw3DCheckpointArches() {
    if (track.count <= 0) return;
    for (int n = cfg::DRAW_DIST - 1; n >= 1; --n) {
        int segIdx = (ctx.baseSeg + n) % track.count;
        uint8_t flags = track.segs[segIdx].flags;
        if ((flags & 0x03) == 0) continue;

        const RoadPoint& pt = ctx.pts[n];
        int sy = static_cast<int>(std::lround(pt.sy));
        if (sy <= 14 || sy >= ctx.clipY[n] || sy >= cfg::WORLD_ROWS) continue;

        int hw    = static_cast<int>(std::lround(pt.sw));
        int cx    = static_cast<int>(std::lround(pt.sx));
        int archH = std::max(4, static_cast<int>(std::lround((cfg::FOCAL_PX / pt.cz) * 1450.0f)));
        int topY  = std::max(14, sy - archH);
        int beamH = std::max(2, archH / 5);
        int postW = std::max(2, hw / 10);

        bool isStartFinish = (flags & 0x02) != 0;
        uint32_t postCol = isStartFinish ? RGB(240, 240, 250) : RGB(0, 225, 255);
        uint32_t beamCol = isStartFinish ? RGB(255, 50, 60)   : RGB(20, 90, 220);

        // Left & right support towers
        Cockpit::drawRect(fb, cx - hw - postW, topY, postW, sy - topY, postCol);
        Cockpit::drawRect(fb, cx + hw,         topY, postW, sy - topY, postCol);

        // Overhead banner beam with alternating stripes
        int xL = cx - hw - postW;
        int xR = cx + hw + postW;
        for (int y = topY; y < std::min(sy, topY + beamH); ++y) {
            if (y < 14 || y >= cfg::WORLD_ROWS) continue;
            uint32_t* r = fb.row(y);
            int stripeW = std::max(2, (xR - xL) / 8);
            for (int x = std::max(0, xL); x < std::min(cfg::SCREEN_W, xR); ++x) {
                bool alt = (((x - xL) / stripeW) + ((y - topY) / 2)) & 1;
                r[x] = alt ? postCol : beamCol;
            }
        }
    }
}

void Game::drawRearViewMirror() {
    const int mx0 = 116 + cockpit.ox();
    const int my0 = 2   + std::max(-2, cockpit.oy());
    const int mw  = 88;
    const int mh  = 13;

    Cockpit::drawRect(fb, mx0 - 2, my0 - 1, mw + 4, mh + 2, RGB(70, 76, 88));
    Cockpit::drawRect(fb, mx0 - 1, my0,     mw + 2, mh,     RGB(18, 20, 26));

    int horizon = my0 + 5;
    for (int y = my0; y < my0 + mh; ++y) {
        if (y < 0 || y >= cfg::SCREEN_H) continue;
        uint32_t* row = fb.row(y);
        if (y < horizon) {
            fillSpan(row, mx0, mx0 + mw, ctx.skyLut[180]);
        } else {
            int dy = y - horizon + 1;
            int hw = 6 + dy * 4;
            int segIdx = (track.count > 0)
                ? ((ctx.baseSeg - dy + track.count) % track.count)
                : 0;
            int band = (segIdx / cfg::RUMBLE_LEN) & 1;
            int cx = mx0 + mw / 2 + static_cast<int>(track.segs[segIdx].curve * dy * 1.5f);
            const RoadPalette& P = ctx.pal[0][band];
            fillSpan(row, mx0, std::min(mx0 + mw, std::max(mx0, cx - hw)), P.grass);
            fillSpan(row, std::max(mx0, cx - hw), std::min(mx0 + mw, cx + hw), P.road);
            fillSpan(row, std::max(mx0, cx + hw), mx0 + mw, P.grass);
        }
    }

    for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) {
        const Car& c = traffic.cars[i];
        if (!c.active) continue;
        float d = mathutil::wrapDist(c.z, vehicle.z, track.length);
        if (d < 0.0f && d > -3000.0f) {
            float closeness = 1.0f - (-d / 3000.0f);
            int cw = std::max(2, static_cast<int>(closeness * 10.0f));
            int ch = std::max(2, static_cast<int>(closeness * 6.0f));
            int cx = mx0 + mw / 2 - static_cast<int>((c.offset - vehicle.x) * 18.0f * closeness);
            int cy = horizon + 1 + static_cast<int>(closeness * 6.0f);
            if (cx - cw / 2 >= mx0 && cx + cw / 2 < mx0 + mw) {
                uint32_t col = (c.spriteId == SPR_CAR_RED) ? RGB(230, 45, 45)
                             : (c.spriteId == SPR_CAR_BLUE) ? RGB(50, 120, 240)
                             : (c.spriteId == SPR_CAR_YELLOW) ? RGB(240, 200, 40)
                             : RGB(200, 205, 215);
                Cockpit::drawRect(fb, cx - cw / 2, cy - ch, cw, ch, col);
            }
        }
    }
}

void Game::drawWeatherAndWipers() {
    uint32_t rng = static_cast<uint32_t>(tickCount * 747796405u + 2891336453u);

    if (vehicle.nitroActive || vehicle.speed > 0.85f * cfg::MAX_SPEED) {
        uint32_t streakCol = vehicle.nitroActive ? RGB(100, 235, 255) : RGB(220, 235, 255);
        for (int i = 0; i < 14; ++i) {
            float angle = mathutil::rand01(rng) * mathutil::TWO_PI;
            float r0 = 95.0f + mathutil::rand01(rng) * 35.0f;
            float r1 = r0 + (vehicle.nitroActive ? 28.0f : 16.0f);
            int x0 = 160 + static_cast<int>(std::cos(angle) * r0);
            int y0 = 100 + static_cast<int>(std::sin(angle) * r0 * 0.65f);
            int x1 = 160 + static_cast<int>(std::cos(angle) * r1);
            int y1 = 100 + static_cast<int>(std::sin(angle) * r1 * 0.65f);
            Cockpit::thickLine(fb, x0, y0, x1, y1, 0, streakCol, 15, 175);
        }
    }

    if (weatherMode == 1) {
        for (int i = 0; i < 45; ++i) {
            int rx = 24 + mathutil::randRange(rng, 272);
            int ry = 16 + mathutil::randRange(rng, 155);
            int dx = static_cast<int>(-vehicle.steer * 4.0f);
            Cockpit::thickLine(fb, rx, ry, rx + dx, ry + 9, 0, RGB(175, 205, 245), 15, 175);
        }
    } else if (weatherMode == 2) {
        for (int i = 0; i < 50; ++i) {
            int sx = 24 + mathutil::randRange(rng, 272);
            int sy = 16 + mathutil::randRange(rng, 155);
            Cockpit::drawRect(fb, sx, sy, 2, 2, RGB(240, 246, 255));
        }
    }

    if (wipersOn) {
        float sweep = 0.5f + 0.5f * std::sin(wiperPhase);
        float ang   = mathutil::lerp(-2.55f, -0.55f, sweep);
        const int pivotsX[2] = { 96 + cockpit.ox(), 204 + cockpit.ox() };
        const int pivotY     = 182 + cockpit.oy();
        const float bladeLen = 68.0f;
        for (int w = 0; w < 2; ++w) {
            int tx = pivotsX[w] + static_cast<int>(std::lround(bladeLen * std::cos(ang)));
            int ty = pivotY     + static_cast<int>(std::lround(bladeLen * std::sin(ang)));
            Cockpit::thickLine(fb, pivotsX[w], pivotY, tx, ty, 1, RGB(28, 32, 38), 15, 185);
        }
    }
}

void Game::drawHudOverlay() {
    // Nitro bar on centre console (above digital speed)
    int nx = 136 + cockpit.ox();
    int ny = 198 + cockpit.oy();
    Cockpit::drawRect(fb, nx, ny, 48, 4, RGB(35, 40, 50));
    int nFill = static_cast<int>(std::lround(vehicle.nitro * 46.0f));
    uint32_t nCol = vehicle.nitroActive ? RGB(0, 245, 255) : RGB(40, 175, 255);
    Cockpit::drawRect(fb, nx + 1, ny + 1, nFill, 2, nCol);
}

void Game::renderGpsMap(int boxX, int boxY, int boxW, int boxH, bool rotatingRadar) {
    if (track.count <= 0) return;

    displayFb.drawRectAlpha(boxX, boxY, boxW, boxH, RGB(10, 16, 28), 210);
    displayFb.drawBorder(boxX, boxY, boxW, boxH, 2, RGB(60, 180, 255));

    const int cx = boxX + boxW / 2;
    const int cy = boxY + boxH / 2 + (rotatingRadar ? 16 : 0);
    const float radius = std::min(boxW, boxH) * 0.40f;

    int playerSeg = ctx.baseSeg % track.count;
    float pxWorld = track.mapX[playerSeg];
    float pyWorld = track.mapY[playerSeg];
    float heading = track.mapHeading[playerSeg];
    // Rotate so player's current heading points straight UP (-pi/2)
    float rotAngle = -0.5f * mathutil::PI - heading;
    float cosR = std::cos(rotAngle);
    float sinR = std::sin(rotAngle);

    auto projectPt = [&](int segIdx, int& outX, int& outY) {
        float wx = track.mapX[segIdx];
        float wy = track.mapY[segIdx];
        if (rotatingRadar) {
            float dx = (wx - pxWorld) * 1.35f;
            float dy = (wy - pyWorld) * 1.35f;
            float rx = dx * cosR - dy * sinR;
            float ry = dx * sinR + dy * cosR;
            outX = cx + static_cast<int>(std::lround(rx * radius));
            outY = cy + static_cast<int>(std::lround(ry * radius));
        } else {
            outX = cx + static_cast<int>(std::lround(wx * radius));
            outY = cy + static_cast<int>(std::lround(wy * radius));
        }
    };

    // Draw full track spline loop
    int step = std::max(1, track.count / 180);
    for (int i = 0; i < track.count; i += step) {
        int next = (i + step) % track.count;
        int x0, y0, x1, y1;
        projectPt(i, x0, y0);
        projectPt(next, x1, y1);

        if (x0 < boxX + 4 || x0 >= boxX + boxW - 4 || y0 < boxY + 4 || y0 >= boxY + boxH - 4) continue;
        if (x1 < boxX + 4 || x1 >= boxX + boxW - 4 || y1 < boxY + 4 || y1 >= boxY + boxH - 4) continue;

        int distAhead = (i - playerSeg + track.count) % track.count;
        float absC = std::fabs(track.segs[i].curve);
        uint32_t segCol = RGB(70, 95, 130);
        int thickness = 1;

        if (distAhead < 85) {
            thickness = 2;
            if (absC >= 2.2f)      segCol = RGB(255, 65, 65);   // Red sharp hairpin ahead
            else if (absC >= 1.0f) segCol = RGB(255, 215, 50);  // Yellow medium curve ahead
            else                   segCol = RGB(0, 245, 180);   // Cyan/Green straight ahead
        }
        displayFb.drawLine(x0, y0, x1, y1, thickness, segCol);

        // Checkpoint & Start/Finish markers
        if (track.segs[i].flags & 0x02) {
            displayFb.drawCircle(x0, y0, 4, RGB(255, 255, 255), true);
        } else if (track.segs[i].flags & 0x01) {
            displayFb.drawCircle(x0, y0, 3, RGB(0, 235, 255), true);
        }
    }

    // Draw Rival Traffic Cars on GPS
    for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) {
        if (!traffic.cars[i].active) continue;
        int cSeg = static_cast<int>(std::floor(mathutil::wrap(traffic.cars[i].z, track.length) / cfg::SEG_LEN)) % track.count;
        int tx, ty;
        projectPt(cSeg, tx, ty);
        if (tx >= boxX + 5 && tx < boxX + boxW - 5 && ty >= boxY + 5 && ty < boxY + boxH - 5) {
            displayFb.drawCircle(tx, ty, 3, RGB(255, 75, 75), true);
        }
    }

    // Draw Player Marker (bright green triangle/circle)
    int px, py;
    projectPt(playerSeg, px, py);
    px = std::clamp(px, boxX + 8, boxX + boxW - 8);
    py = std::clamp(py, boxY + 8, boxY + boxH - 8);
    displayFb.drawCircle(px, py, 5, RGB(60, 255, 110), true);
    displayFb.drawCircle(px, py, 5, RGB(255, 255, 255), false);

    displayFb.drawText(boxX + 8, boxY + 6, rotatingRadar ? "GPS RADAR [TAB]" : "GPS CIRCUIT [TAB]",
                       RGB(120, 225, 255), 1, true);
}

void Game::renderRallyCallout() {
    float peakCurve = 0.0f;
    int   distSegs  = 0;
    bool  crest     = false;
    scanUpcomingTurn(track, ctx.baseSeg, peakCurve, distSegs, crest);

    float absC = std::fabs(peakCurve);
    if (absC < 0.45f && !crest) return;

    const int bx = 330, by = 52, bw = 300, bh = 36;
    displayFb.drawRectAlpha(bx, by, bw, bh, RGB(14, 18, 30), 200);

    uint32_t col = RGB(80, 255, 140);
    const char* label = "STRAIGHT";
    if (absC >= 2.2f) {
        col   = RGB(255, 65, 65);
        label = (peakCurve > 0.0f) ? ">>> HARD RIGHT! >>>" : "<<< HARD LEFT! <<<";
    } else if (absC >= 1.1f) {
        col   = RGB(255, 215, 55);
        label = (peakCurve > 0.0f) ? ">> MEDIUM RIGHT >>" : "<< MEDIUM LEFT <<";
    } else if (absC >= 0.45f) {
        col   = RGB(90, 245, 190);
        label = (peakCurve > 0.0f) ? "> EASY RIGHT >" : "< EASY LEFT <";
    } else if (crest) {
        col   = RGB(255, 180, 60);
        label = "/\\ BLIND CREST AHEAD /\\";
    }

    displayFb.drawBorder(bx, by, bw, bh, 2, col);
    char buf[64];
    int meters = std::max(10, distSegs * 5);
    std::snprintf(buf, sizeof(buf), "%s  %dM", label, meters);
    int tw = DisplayBuffer::textWidth(buf, 2);
    displayFb.drawText(bx + (bw - tw) / 2, by + 11, buf, col, 2, true);
}

void Game::renderHighResHudAndGps() {
    char buf[96];
    const TrackRecord& rec = highScores.record(selectedTrack);
    uint32_t bestScore = std::max(score, rec.highScore);

    // Top HUD Bar (rows 0..42 in 960x720)
    displayFb.drawRectAlpha(0, 0, DisplayBuffer::W, 42, RGB(10, 14, 22), 215);
    displayFb.drawRect(0, 40, DisplayBuffer::W, 2, RGB(50, 140, 220));

    // 1. Score & High Score (Top Left)
    std::snprintf(buf, sizeof(buf), "SCORE %07u", score);
    displayFb.drawText(14, 12, buf, RGB(255, 255, 255), 2, true);

    std::snprintf(buf, sizeof(buf), "HI %07u", bestScore);
    uint32_t hiCol = newHighScoreAchieved ? RGB(255, 215, 40) : RGB(120, 215, 255);
    displayFb.drawText(195, 12, buf, hiCol, 2, true);

    // 2. Countdown Timer or Mode Status (Top Center-Left)
    if (gameMode == GameMode::ArcadeCheckpoint) {
        uint32_t tCol = (timeLeftSec < 10.0f && ((tickCount / 10) & 1)) ? RGB(255, 60, 60) : RGB(255, 235, 80);
        std::snprintf(buf, sizeof(buf), "TIME %02.1FS", timeLeftSec);
        displayFb.drawText(595, 12, buf, tCol, 2, true);
    } else if (gameMode == GameMode::GrandPrix3Lap) {
        std::snprintf(buf, sizeof(buf), "LAP %d/3  POS %d/25", std::min(3, vehicle.lap), racePosition);
        displayFb.drawText(575, 12, buf, RGB(255, 230, 80), 2, true);
    } else {
        int lMin = static_cast<int>(vehicle.lapTime) / 60;
        int lSec = static_cast<int>(vehicle.lapTime) % 60;
        std::snprintf(buf, sizeof(buf), "LAP %d  %02d:%02d", vehicle.lap, lMin, lSec);
        displayFb.drawText(590, 12, buf, RGB(140, 240, 180), 2, true);
    }

    // 3. Gear, Combo & Overtakes (Top Right)
    std::snprintf(buf, sizeof(buf), "G:%d  %dX  PASS:%d", vehicle.gear, comboMult, vehicle.overtakes);
    displayFb.drawText(760, 12, buf, (comboMult > 1) ? RGB(80, 255, 150) : RGB(230, 235, 245), 2, true);

    // 4. GPS Navigation Display (Top-Right inside windshield)
    if (gpsMode == 0) {
        renderGpsMap(772, 52, 168, 156, true);
    } else if (gpsMode == 1) {
        renderGpsMap(732, 52, 208, 196, false);
    }

    // 5. Turn-by-Turn GPS Rally Callout Banner
    if (screenState == ScreenState::Racing) {
        renderRallyCallout();
    }

    // 6. Track & Quality Info Badge (Top-Left below header)
    const TrackMetadata& meta = getTrackMetadata(selectedTrack);
    std::snprintf(buf, sizeof(buf), "%s | %dPX [1-5] | [P]AUSE", meta.name, pixelSize);
    displayFb.drawText(16, 50, buf, RGB(170, 200, 230), 1, true);

    // 7. Floating Event Banner (Checkpoint / Overtake / New Record)
    if (bannerTimer > 0.0f && bannerText[0] != '\0') {
        int tw = DisplayBuffer::textWidth(bannerText, 3);
        int bx = (DisplayBuffer::W - tw) / 2 - 16;
        int by = 104;
        displayFb.drawRectAlpha(bx, by, tw + 32, 38, RGB(10, 14, 24), 210);
        displayFb.drawBorder(bx, by, tw + 32, 38, 2, bannerColor);
        displayFb.drawText(bx + 16, by + 9, bannerText, bannerColor, 3, true);
    }

    // 8. F3 Profiler Overlay
    if (showDebugHud) {
        displayFb.drawRectAlpha(16, 68, 360, 130, RGB(12, 16, 26), 220);
        displayFb.drawBorder(16, 68, 360, 130, 2, RGB(80, 190, 255));
        std::snprintf(buf, sizeof(buf), "FRAME %.2FMS (P99 %.2FMS)", timers.totalMs.avg(), timers.totalMs.p99());
        displayFb.drawText(28, 78, buf, RGB(100, 255, 150), 2, true);
        std::snprintf(buf, sizeof(buf), "TICK:%.0FUS PA:%.0FUS PB:%.0FUS",
                      timers.tickUs.avg(), timers.passAUs.avg(), timers.passBUs.avg());
        displayFb.drawText(28, 104, buf, RGB(220, 235, 255), 1, true);
        std::snprintf(buf, sizeof(buf), "SPRITES:%d WORKERS:%d PIXEL:%dPX",
                      drawList.count, scheduler.workerCount() + 1, pixelSize);
        displayFb.drawText(28, 120, buf, RGB(220, 235, 255), 1, true);
        for (int s = 0; s < cfg::NUM_SLICES; ++s) {
            int us = static_cast<int>(scheduler.stats[s].ns / 1000u);
            int bw = std::clamp(us, 4, 38);
            displayFb.drawRect(28 + s * 42, 142, bw, 10, RGB(60, 195, 255));
        }
        displayFb.drawText(28, 164, "F:FULLSCREEN L:SCALE B:BIOME V:MIRROR C:WEATHER", RGB(165, 190, 220), 1, true);
    }
}

void Game::renderLaunchScreen() {
    // Translucent dark backdrop over the live 3D autopilot flythrough
    displayFb.drawRectAlpha(0, 0, DisplayBuffer::W, DisplayBuffer::H, RGB(8, 12, 24), 155);

    // Top Title Header Card
    displayFb.drawRectAlpha(40, 24, 880, 148, RGB(12, 18, 34), 225);
    displayFb.drawBorder(40, 24, 880, 148, 3, RGB(0, 225, 255));
    displayFb.drawRect(46, 162, 868, 4, RGB(255, 50, 140));

    const char* title = "ARGON : OVERDRIVE";
    int titleW = DisplayBuffer::textWidth(title, 6);
    displayFb.drawText((DisplayBuffer::W - titleW) / 2, 46, title, RGB(0, 245, 255), 6, true);

    const char* sub = "FIRST-PERSON SOFTWARE-RASTERIZED 3D ARCADE RACER (QT 6)";
    int subW = DisplayBuffer::textWidth(sub, 2);
    displayFb.drawText((DisplayBuffer::W - subW) / 2, 112, sub, RGB(255, 210, 80), 2, true);

    const char* hint = "ARROWS / WASD / MOUSE TO SELECT   |   ENTER / CLICK TO CONFIRM   |   F: FULLSCREEN";
    int hintW = DisplayBuffer::textWidth(hint, 1);
    displayFb.drawText((DisplayBuffer::W - hintW) / 2, 142, hint, RGB(180, 205, 235), 1, true);

    // Left Panel: Interactive Menu Options
    displayFb.drawRectAlpha(40, 192, 475, 448, RGB(12, 18, 32), 225);
    displayFb.drawBorder(40, 192, 475, 448, 2, RGB(70, 130, 210));
    displayFb.drawText(64, 204, "RACE CONFIGURATION & SETTINGS", RGB(255, 190, 60), 2, true);

    const TrackMetadata& meta = getTrackMetadata(selectedTrack);
    char items[LAUNCH_MENU_ITEMS][64];
    std::snprintf(items[0], 64, ">> START RACE NOW <<");
    std::snprintf(items[1], 64, "TRACK : < %d/5 %s >", selectedTrack + 1, meta.name);
    std::snprintf(items[2], 64, "MODE  : < %s >", MODE_NAMES[static_cast<int>(gameMode)]);
    std::snprintf(items[3], 64, "PIXEL : < %s >", PIXEL_PRESET_NAMES[pixelSizeIdx]);
    std::snprintf(items[4], 64, "SCALE : < %s >", SCALE_NAMES[static_cast<int>(scaleMode)]);
    std::snprintf(items[5], 64, "FULLSCREEN : < %s >", isFullscreen ? "ON (WINDOW: F)" : "OFF (PRESS F)");
    std::snprintf(items[6], 64, "CRT SCANLINES : < %s >", crtScanlines ? "ENABLED" : "DISABLED");
    std::snprintf(items[7], 64, "EXIT TO DESKTOP");

    for (int i = 0; i < LAUNCH_MENU_ITEMS; ++i) {
        int iy = 236 + i * 48;
        bool sel = (menuCursor == i);
        if (sel) {
            displayFb.drawRectAlpha(54, iy - 6, 447, 38, RGB(0, 160, 230), 180);
            displayFb.drawBorder(54, iy - 6, 447, 38, 2, RGB(120, 250, 255));
        }
        uint32_t col = (i == 0) ? (sel ? RGB(100, 255, 140) : RGB(60, 230, 110))
                                : (sel ? RGB(255, 255, 255) : RGB(185, 200, 225));
        displayFb.drawText(68, iy + 5, items[i], col, 2, true);
    }

    // Right Panel: Track 2D GPS Circuit Preview & Saved High Scores
    displayFb.drawRectAlpha(535, 192, 385, 448, RGB(12, 18, 32), 225);
    displayFb.drawBorder(535, 192, 385, 448, 2, RGB(70, 130, 210));

    char buf[96];
    std::snprintf(buf, sizeof(buf), "TRACK %d: %s", selectedTrack + 1, meta.name);
    displayFb.drawText(555, 206, buf, RGB(0, 240, 255), 2, true);
    displayFb.drawText(555, 228, meta.subtitle, RGB(190, 210, 235), 1, true);

    // Render 2D GPS Circuit Map Preview in box (555, 246, 345, 210)
    renderGpsMap(555, 246, 345, 210, false);

    // Track Telemetry & Records Table
    const TrackRecord& rec = highScores.record(selectedTrack);
    float lenKm = (track.length * cfg::KMH_PER_WU) / 100.0f;
    std::snprintf(buf, sizeof(buf), "DIFFICULTY: %-8s   LENGTH: %.1FKM", meta.difficulty, lenKm);
    displayFb.drawText(555, 470, buf, RGB(255, 215, 80), 2, true);

    displayFb.drawRect(555, 496, 345, 2, RGB(60, 110, 180));
    displayFb.drawText(555, 506, "TRACK HALL OF FAME (SAVED RECORDS)", RGB(255, 120, 190), 2, true);

    std::snprintf(buf, sizeof(buf), "HIGHEST SCORE  : %07u PTS", rec.highScore);
    displayFb.drawText(555, 534, buf, RGB(255, 240, 90), 2, true);

    if (rec.bestLapSec > 0.0f) {
        int bMin = static_cast<int>(rec.bestLapSec) / 60;
        int bSec = static_cast<int>(rec.bestLapSec) % 60;
        int bSub = static_cast<int>((rec.bestLapSec - std::floor(rec.bestLapSec)) * 100.0f);
        std::snprintf(buf, sizeof(buf), "BEST LAP TIME  : %02d:%02d.%02d", bMin, bSec, bSub);
    } else {
        std::snprintf(buf, sizeof(buf), "BEST LAP TIME  : --:--.--");
    }
    displayFb.drawText(555, 560, buf, RGB(100, 255, 170), 2, true);

    std::snprintf(buf, sizeof(buf), "TOP SPEED REC  : %.0F KM/H", rec.maxSpeedKmh);
    displayFb.drawText(555, 586, buf, RGB(130, 220, 255), 2, true);

    std::snprintf(buf, sizeof(buf), "MOST OVERTAKES : %d CARS", rec.mostOvertakes);
    displayFb.drawText(555, 612, buf, RGB(220, 195, 255), 2, true);

    // Bottom Controls Strip
    displayFb.drawRectAlpha(40, 654, 880, 44, RGB(12, 18, 32), 225);
    displayFb.drawBorder(40, 654, 880, 44, 2, RGB(60, 110, 180));
    const char* ctl = "DRIVE: WASD/ARROWS | NITRO: SPACE/SHIFT | QUALITY: 1-5 OR [/] | GPS: TAB | BIOME: B | FULLSCREEN: F";
    int ctlW = DisplayBuffer::textWidth(ctl, 1);
    displayFb.drawText((DisplayBuffer::W - ctlW) / 2, 672, ctl, RGB(210, 230, 255), 1, true);
}

void Game::renderCountdownOverlay() {
    const int gx = 340, gy = 135, gw = 280, gh = 110;
    displayFb.drawRectAlpha(gx, gy, gw, gh, RGB(12, 16, 26), 230);
    displayFb.drawBorder(gx, gy, gw, gh, 3, RGB(200, 210, 230));

    int stage = static_cast<int>(std::ceil(countdownTimer)); // 3, 2, 1
    for (int i = 0; i < 3; ++i) {
        int lx = gx + 55 + i * 85;
        int ly = gy + 38;
        bool lit = (3 - i) >= stage;
        uint32_t col = lit ? RGB(255, 45, 45) : RGB(45, 20, 24);
        displayFb.drawCircle(lx, ly, 22, col, true);
        displayFb.drawCircle(lx, ly, 22, RGB(180, 190, 205), false);
    }

    char buf[32];
    std::snprintf(buf, sizeof(buf), "GET READY... %d", std::clamp(stage, 1, 3));
    int tw = DisplayBuffer::textWidth(buf, 3);
    displayFb.drawText(gx + (gw - tw) / 2, gy + 74, buf, RGB(255, 230, 80), 3, true);
}

void Game::renderPauseOverlay() {
    displayFb.drawRectAlpha(0, 0, DisplayBuffer::W, DisplayBuffer::H, RGB(6, 10, 20), 170);
    const int bx = 260, by = 170, bw = 440, bh = 340;
    displayFb.drawRectAlpha(bx, by, bw, bh, RGB(14, 20, 36), 235);
    displayFb.drawBorder(bx, by, bw, bh, 3, RGB(0, 225, 255));

    const char* hdr = "RACE PAUSED";
    int hw = DisplayBuffer::textWidth(hdr, 4);
    displayFb.drawText(bx + (bw - hw) / 2, by + 24, hdr, RGB(255, 225, 70), 4, true);

    char opts[PAUSE_MENU_ITEMS][64];
    std::snprintf(opts[0], 64, "RESUME RACE");
    std::snprintf(opts[1], 64, "RESTART TRACK");
    std::snprintf(opts[2], 64, "PIXEL QUALITY: < %s >", PIXEL_PRESET_NAMES[pixelSizeIdx]);
    std::snprintf(opts[3], 64, "TOGGLE FULLSCREEN (F)");
    std::snprintf(opts[4], 64, "RETURN TO LAUNCH SCREEN");

    for (int i = 0; i < PAUSE_MENU_ITEMS; ++i) {
        int iy = 254 + i * 46;
        bool sel = (pauseCursor == i);
        if (sel) {
            displayFb.drawRectAlpha(bx + 20, iy - 6, bw - 40, 36, RGB(0, 155, 225), 185);
            displayFb.drawBorder(bx + 20, iy - 6, bw - 40, 36, 2, RGB(120, 250, 255));
        }
        displayFb.drawText(bx + 36, iy + 5, opts[i], sel ? RGB(255, 255, 255) : RGB(185, 205, 230), 2, true);
    }
}

void Game::renderRaceOverOverlay() {
    displayFb.drawRectAlpha(0, 0, DisplayBuffer::W, DisplayBuffer::H, RGB(6, 10, 20), 185);
    const int bx = 200, by = 130, bw = 560, bh = 430;
    displayFb.drawRectAlpha(bx, by, bw, bh, RGB(14, 20, 36), 240);
    displayFb.drawBorder(bx, by, bw, bh, 3, RGB(255, 200, 60));

    const char* hdr = (gameMode == GameMode::GrandPrix3Lap && vehicle.lap > 3)
        ? "GRAND PRIX COMPLETE!"
        : "CHECKPOINT TIME EXPIRED!";
    int hw = DisplayBuffer::textWidth(hdr, 4);
    displayFb.drawText(bx + (bw - hw) / 2, by + 28, hdr, RGB(0, 240, 255), 4, true);

    if (newHighScoreAchieved || newBestLapAchieved) {
        const char* recTxt = ">>> NEW TRACK RECORD SAVED! <<<";
        int rw = DisplayBuffer::textWidth(recTxt, 3);
        displayFb.drawText(bx + (bw - rw) / 2, by + 78, recTxt, RGB(255, 225, 40), 3, true);
    }

    char buf[96];
    const TrackRecord& rec = highScores.record(selectedTrack);

    std::snprintf(buf, sizeof(buf), "FINAL SCORE      : %07u PTS", score);
    displayFb.drawText(bx + 70, by + 135, buf, RGB(255, 255, 255), 3, true);

    std::snprintf(buf, sizeof(buf), "TRACK HIGH SCORE : %07u PTS", rec.highScore);
    displayFb.drawText(bx + 70, by + 180, buf, RGB(255, 215, 70), 3, true);

    std::snprintf(buf, sizeof(buf), "TOTAL OVERTAKES  : %d CARS (POS %d/25)", vehicle.overtakes, racePosition);
    displayFb.drawText(bx + 70, by + 225, buf, RGB(100, 255, 160), 2, true);

    std::snprintf(buf, sizeof(buf), "MAX SPEED REACHED: %.1F KM/H", raceMaxSpeedKmh);
    displayFb.drawText(bx + 70, by + 260, buf, RGB(120, 220, 255), 2, true);

    if (vehicle.bestLapTime > 0.0f) {
        int bMin = static_cast<int>(vehicle.bestLapTime) / 60;
        int bSec = static_cast<int>(vehicle.bestLapTime) % 60;
        int bSub = static_cast<int>((vehicle.bestLapTime - std::floor(vehicle.bestLapTime)) * 100.0f);
        std::snprintf(buf, sizeof(buf), "BEST LAP TIME    : %02d:%02d.%02d", bMin, bSec, bSub);
    } else {
        std::snprintf(buf, sizeof(buf), "LAPS COMPLETED   : %d", std::max(0, vehicle.lap - 1));
    }
    displayFb.drawText(bx + 70, by + 295, buf, RGB(230, 195, 255), 2, true);

    const char* prompt = "PRESS [ENTER] OR CLICK FOR LAUNCH MENU   |   PRESS [R] TO RETRY";
    int pw = DisplayBuffer::textWidth(prompt, 2);
    displayFb.drawText(bx + (bw - pw) / 2, by + 365, prompt, RGB(255, 235, 90), 2, true);
}
