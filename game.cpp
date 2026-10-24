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
const char* DENSITY_NAMES[3] = {
    "LOW (8 CARS - OPEN)",
    "MEDIUM (16 CARS)",
    "HIGH (24 CARS - RUSH)"
};
const char* SCALE_NAMES[3] = {
    "FULL WINDOW STRETCH",
    "4:3 ASPECT FIT",
    "INTEGER PIXEL-PERFECT"
};
inline constexpr int LAUNCH_MENU_ITEMS = 10;
inline constexpr int PAUSE_MENU_ITEMS  = 7;
} // namespace

void Game::init(int workerCount, bool forceScalar) {
    assert(reinterpret_cast<uintptr_t>(fb.px) % 64 == 0);
    assert(reinterpret_cast<uintptr_t>(displayFb.px) % 64 == 0);

    initSpanFill(forceScalar);
    highScores.init("highscores.dat");
    buildSpriteAtlas(atlas);

    selectedTrack   = 0;
    selectedCar     = 0;
    selectedDensity = 1;
    steerAssistOn   = false;
    use3DPipeline   = true;
    showGapGuide    = true;

    buildDemoTrack(track);
    initRenderContext(ctx, 0);
    cockpit.init();
    camFx = CameraFx{};
    vehicle = Vehicle{};
    traffic.density = TrafficDensity::High;
    VehiclePhysics::reset(vehicle, track, traffic);
    VehiclePhysics::fillCamera(vehicle, track, camFx, cam);
    scheduler.init(workerCount);

    fb.clear(0xFF000000u);
    displayFb.clear(0xFF000000u);
    cockpitFb.clear(0xFF000000u);
    pipe3d.bind(displayFb.px, DisplayBuffer::W, DisplayBuffer::H, cfg::HORIZON_BASE, 0.0f);
    pipe3d.clearDepth();

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
    lastNearMisses  = 0;
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
    vehicle.realisticMode = true;
    vehicle.carPreset     = selectedCar;
    vehicle.steerAssist   = steerAssistOn;
    traffic.density       = static_cast<TrafficDensity>(selectedDensity);
    selectTrack(selectedTrack);
}

void Game::selectCar(int carIdx) {
    selectedCar = ((carIdx % NUM_CAR_PRESETS) + NUM_CAR_PRESETS) % NUM_CAR_PRESETS;
    vehicle.carPreset = selectedCar;
    const CarSpec& spec = getCarSpec(selectedCar);
    char msg[48];
    std::snprintf(msg, sizeof(msg), "CAR: %s (%d KM/H)", spec.name, spec.topSpeedKmh);
    triggerBanner(msg, spec.accentColor, 1.8f);
}

void Game::selectTrafficDensity(int densityIdx) {
    selectedDensity = ((densityIdx % 3) + 3) % 3;
    traffic.density = static_cast<TrafficDensity>(selectedDensity);
    const TrackMetadata& meta = getTrackMetadata(selectedTrack);
    initTraffic(traffic, track, 1u, traffic.density);
    for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) {
        traffic.cars[i].speed *= meta.trafficSpeedMult;
    }
    char msg[48];
    std::snprintf(msg, sizeof(msg), "TRAFFIC: %s", DENSITY_NAMES[selectedDensity]);
    triggerBanner(msg, RGB(255, 215, 70), 1.8f);
}

void Game::toggleSteerAssist() {
    steerAssistOn = !steerAssistOn;
    vehicle.steerAssist = steerAssistOn;
    triggerBanner(steerAssistOn ? "STEER ASSIST (ADAS): ON" : "STEER ASSIST: OFF (MANUAL)",
                  steerAssistOn ? RGB(80, 255, 150) : RGB(255, 190, 60), 1.8f);
}

void Game::toggle3DPipeline() {
    use3DPipeline = !use3DPipeline;
    triggerBanner(use3DPipeline ? "3D Z-BUFFER MESH PIPELINE: ON" : "CLASSIC 2.5D BILLBOARD MODE",
                  use3DPipeline ? RGB(0, 245, 255) : RGB(255, 210, 70), 1.8f);
}

void Game::selectTrack(int presetIdx) {
    selectedTrack = ((presetIdx % NUM_TRACK_PRESETS) + NUM_TRACK_PRESETS) % NUM_TRACK_PRESETS;
    const TrackMetadata& meta = getTrackMetadata(selectedTrack);
    buildTrackPreset(track, selectedTrack);
    initRenderContext(ctx, meta.defaultTheme);
    weatherMode = meta.defaultWeather;
    wipersOn    = (weatherMode != 0);

    vehicle.realisticMode = !testMode;
    vehicle.carPreset     = selectedCar;
    vehicle.steerAssist   = steerAssistOn;
    traffic.density       = static_cast<TrafficDensity>(selectedDensity);

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
    timeLeftSec     = (gameMode == GameMode::ArcadeCheckpoint) ? 50.0f : 999.0f;
    countdownTimer  = 3.2f;
    raceMaxSpeedKmh = 0.0f;
    racePosition    = traffic.activeCount + 1;
    lastCheckpointSeg = 0;
    lastNearMisses  = 0;
    newHighScoreAchieved = false;
    newBestLapAchieved   = false;
    paused          = false;
    screenState     = ScreenState::Countdown;
    triggerBanner("GET READY!", RGB(255, 220, 60), 2.0f);
}

void Game::repairAndContinue() {
    VehiclePhysics::repairInPlace(vehicle, &cockpit);
    if (screenState == ScreenState::Wrecked) {
        screenState = ScreenState::Racing;
    }
    paused = false;
    triggerBanner("VEHICLE REPAIRED (100% HEALTH)!", RGB(60, 255, 140), 2.2f);
    vehicle.audio.shiftCount++;
}

void Game::fieldRepair() {
    if (vehicle.wrecked || screenState == ScreenState::Wrecked) {
        repairAndContinue();
        return;
    }
    if (vehicle.health < 99.0f) {
        vehicle.health        = std::min(100.0f, vehicle.health + 50.0f);
        vehicle.damage        = 1.0f - (vehicle.health / 100.0f);
        vehicle.crackProgress = vehicle.damage;
        if (vehicle.health >= 99.0f) cockpit.clearCracks();
        triggerBanner("FIELD REPAIR +50% HEALTH!", RGB(80, 255, 160), 1.8f);
        vehicle.audio.shiftCount++;
    }
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
            case 0: if (dir > 0) startNewRace(); break;
            case 1: selectCar(selectedCar + dir); break;
            case 2: selectTrack(selectedTrack + dir); break;
            case 3: selectTrafficDensity(selectedDensity + dir); break;
            case 4: toggleSteerAssist(); break;
            case 5: gameMode = static_cast<GameMode>(((static_cast<int>(gameMode) + dir) % 3 + 3) % 3); break;
            case 6: setPixelSizePreset(pixelSizeIdx + dir); break;
            case 7: scaleMode = static_cast<ScaleMode>(((static_cast<int>(scaleMode) + dir) % 3 + 3) % 3); break;
            case 8: outToggleFullscreen = true; break;
            case 9: break;
        }
        vehicle.audio.shiftCount++;
    } else if (screenState == ScreenState::Paused) {
        switch (pauseCursor) {
            case 2: toggleSteerAssist(); break;
            case 3: selectTrafficDensity(selectedDensity + dir); break;
            case 4: setPixelSizePreset(pixelSizeIdx + dir); break;
            default: break;
        }
        vehicle.audio.shiftCount++;
    }
}

void Game::menuActivate(bool& outToggleFullscreen, bool& outQuit) {
    outToggleFullscreen = false;
    outQuit = false;
    if (screenState == ScreenState::LaunchMenu) {
        switch (menuCursor) {
            case 0: startNewRace(); break;
            case 1: selectCar(selectedCar + 1); break;
            case 2: selectTrack(selectedTrack + 1); break;
            case 3: selectTrafficDensity(selectedDensity + 1); break;
            case 4: toggleSteerAssist(); break;
            case 5: gameMode = static_cast<GameMode>((static_cast<int>(gameMode) + 1) % 3); break;
            case 6: setPixelSizePreset(pixelSizeIdx + 1); break;
            case 7: cycleScaleMode(); break;
            case 8: outToggleFullscreen = true; break;
            case 9: outQuit = true; break;
        }
        vehicle.audio.shiftCount++;
    } else if (screenState == ScreenState::Paused) {
        switch (pauseCursor) {
            case 0: togglePause(); break;
            case 1: repairAndContinue(); break;
            case 2: toggleSteerAssist(); break;
            case 3: selectTrafficDensity(selectedDensity + 1); break;
            case 4: setPixelSizePreset(pixelSizeIdx + 1); break;
            case 5: outToggleFullscreen = true; break;
            case 6: enterLaunchMenu(); break;
        }
        vehicle.audio.shiftCount++;
    } else if (screenState == ScreenState::Wrecked) {
        repairAndContinue();
    } else if (screenState == ScreenState::RaceOver) {
        enterLaunchMenu();
    }
}

void Game::handleMouseMove(int normX960, int normY720) {
    if (screenState == ScreenState::LaunchMenu) {
        if (normX960 >= 45 && normX960 <= 510 && normY720 >= 216 && normY720 < 216 + LAUNCH_MENU_ITEMS * 41) {
            int idx = (normY720 - 216) / 41;
            if (idx >= 0 && idx < LAUNCH_MENU_ITEMS) menuCursor = idx;
        }
    } else if (screenState == ScreenState::Paused) {
        if (normX960 >= 260 && normX960 <= 700 && normY720 >= 230 && normY720 < 230 + PAUSE_MENU_ITEMS * 42) {
            int idx = (normY720 - 230) / 42;
            if (idx >= 0 && idx < PAUSE_MENU_ITEMS) pauseCursor = idx;
        }
    }
}

void Game::handleMouseClick(int normX960, int normY720, bool& outToggleFullscreen, bool& outQuit) {
    handleMouseMove(normX960, normY720);
    if (screenState == ScreenState::LaunchMenu) {
        if (normX960 >= 45 && normX960 <= 510 && normY720 >= 216 && normY720 < 216 + LAUNCH_MENU_ITEMS * 41) {
            menuActivate(outToggleFullscreen, outQuit);
        }
    } else if (screenState == ScreenState::Paused) {
        if (normX960 >= 260 && normX960 <= 700 && normY720 >= 230 && normY720 < 230 + PAUSE_MENU_ITEMS * 42) {
            menuActivate(outToggleFullscreen, outQuit);
        }
    } else if (screenState == ScreenState::Wrecked) {
        repairAndContinue();
    } else if (screenState == ScreenState::RaceOver) {
        enterLaunchMenu();
    }
}

void Game::tick(const InputState& in) {
    auto t0 = std::chrono::steady_clock::now();

    // Unit test mode runs pure Phase 1-8 physics tick without menu state interception
    if (testMode) {
        vehicle.realisticMode = false;
        vehicle.hitboxScale   = 1.0f;
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

    vehicle.realisticMode = true;
    vehicle.hitboxScale   = 0.64f;

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
        if (c > 0.15f || vehicle.x < -0.20f) autoIn.down |= InputState::RIGHT;
        if (c < -0.15f || vehicle.x > +0.20f) autoIn.down |= InputState::LEFT;

        vehicle.invulnT = 2.0f;
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
        vehicle.rpm = mathutil::lerp(vehicle.rpm, revThrottle ? 6200.0f : 1200.0f, 0.15f);
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

    // 3. Wrecked (Health == 0) or Race Over states
    if (screenState == ScreenState::Wrecked || screenState == ScreenState::RaceOver) {
        InputState coastIn;
        coastIn.down = InputState::BRAKE;
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
        if (vehicle.health < 100.0f) {
            repairAndContinue();
        } else {
            startNewRace();
        }
    }
    if (in.justPressed(InputState::WIPER)) {
        wipersOn = !wipersOn;
    }

    if (!paused && screenState == ScreenState::Racing) {
        int prevOvertakes = vehicle.overtakes;
        int prevLap       = vehicle.lap;
        float prevHealth  = vehicle.health;

        VehiclePhysics::tick(vehicle, in, track, atlas, traffic, &camFx, &cockpit);
        VehiclePhysics::fillCamera(vehicle, track, camFx, cam);

        float spdKmh = vehicle.speed * cfg::KMH_PER_WU;
        raceMaxSpeedKmh = std::max(raceMaxSpeedKmh, spdKmh);

        if (vehicle.wrecked || vehicle.health <= 0.0f) {
            vehicle.health  = 0.0f;
            vehicle.wrecked = true;
            screenState     = ScreenState::Wrecked;
            triggerBanner("CAR WRECKED! [R] REPAIR HERE OR [ENTER] RESTART", RGB(255, 60, 60), 5.0f);
            return;
        }

        if (vehicle.health < prevHealth - 1.0f) {
            comboMult  = 1;
            comboTimer = 0.0f;
            char msg[48];
            std::snprintf(msg, sizeof(msg), "IMPACT! -%.0F%% HEALTH (%0.0F%% LEFT)",
                          prevHealth - vehicle.health, vehicle.health);
            triggerBanner(msg, (vehicle.health < 30.0f) ? RGB(255, 55, 55) : RGB(255, 165, 50), 1.6f);
        }

        int newPasses = vehicle.overtakes - prevOvertakes;
        int newNear   = vehicle.nearMisses - lastNearMisses;
        lastNearMisses = vehicle.nearMisses;

        if (newPasses > 0) {
            comboTimer = 5.0f;
            comboMult  = std::min(8, comboMult + newPasses);
            uint32_t pts = static_cast<uint32_t>((newNear > 0 ? 850 : 500) * newPasses * comboMult);
            score += pts;
            racePosition = std::max(1, (traffic.activeCount + 1) - vehicle.overtakes);
            char msg[48];
            if (newNear > 0) {
                std::snprintf(msg, sizeof(msg), "CLOSE PASS! +%u (%dX COMBO)", pts, comboMult);
                triggerBanner(msg, RGB(0, 245, 255), 1.6f);
            } else {
                std::snprintf(msg, sizeof(msg), "OVERTAKE +%u (%dX COMBO!)", pts, comboMult);
                triggerBanner(msg, RGB(80, 255, 160), 1.5f);
            }
        }

        if (comboTimer > 0.0f) {
            comboTimer -= cfg::STEP;
            if (comboTimer <= 0.0f) comboMult = 1;
        }

        if (vehicle.state == CrashState::Driving && vehicle.speed > 1000.0f) {
            float spdFrac = vehicle.speed / cfg::MAX_SPEED;
            float draftBonus = 1.0f + 0.5f * vehicle.drafting;
            scoreAccum += (spdFrac * spdFrac * 45.0f * draftBonus * static_cast<float>(comboMult)) * cfg::STEP;
            uint32_t whole = static_cast<uint32_t>(scoreAccum);
            if (whole > 0) {
                score += whole;
                scoreAccum -= static_cast<float>(whole);
            }
        }

        int curSeg = (track.count > 0)
            ? (static_cast<int>(std::floor(mathutil::wrap(vehicle.z, track.length) / cfg::SEG_LEN)) % track.count)
            : 0;
        if (curSeg != lastCheckpointSeg) {
            int stepCount = (curSeg - lastCheckpointSeg + track.count) % track.count;
            if (stepCount > 0 && stepCount < 20) {
                for (int s = 1; s <= stepCount; ++s) {
                    int checkSeg = (lastCheckpointSeg + s) % track.count;
                    if (track.segs[checkSeg].flags & 0x01) {
                        score += 2000u * static_cast<uint32_t>(comboMult);
                        vehicle.health = std::min(100.0f, vehicle.health + 25.0f);
                        vehicle.damage = 1.0f - (vehicle.health / 100.0f);
                        if (vehicle.health >= 95.0f) cockpit.clearCracks();

                        if (gameMode == GameMode::ArcadeCheckpoint) {
                            timeLeftSec = std::min(99.0f, timeLeftSec + 15.0f);
                            triggerBanner("CHECKPOINT! +15S & +25% REPAIR", RGB(0, 245, 255), 2.2f);
                        } else {
                            triggerBanner("CHECKPOINT! +25% HEALTH REPAIRED", RGB(0, 245, 255), 2.0f);
                        }
                        vehicle.audio.shiftCount++;
                    }
                }
            }
            lastCheckpointSeg = curSeg;
        }

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
                std::snprintf(msg, sizeof(msg), "LAP %d! +5000 PTS & REPAIRED", vehicle.lap);
                triggerBanner(msg, RGB(255, 230, 80), 2.5f);
            }
        }

        if (score > highScores.record(selectedTrack).highScore && !newHighScoreAchieved) {
            newHighScoreAchieved = true;
            triggerBanner("NEW TRACK HIGH SCORE!", RGB(255, 215, 0), 2.5f);
        }

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

void Game::compositeCockpitForwardPass() {
    // Render cockpit interior, cracks, wipers, gauges, and rear-view mirror in a distinct forward pass
    // with depth testing disabled (maintaining 1:1 screen-space fidelity over the 3D Z-buffered world).
    constexpr uint32_t KEY_TRANSPARENT = 0xFF01FE02u;
    fb.clear(KEY_TRANSPARENT);

    if (weatherMode != 0 || wipersOn || vehicle.nitroActive) {
        drawWeatherAndWipers();
    }
    cockpit.drawCracks(fb);
    cockpit.drawFrame(fb);
    cockpit.drawGauges(fb);

    if (screenState != ScreenState::LaunchMenu) {
        if (showMirror) {
            drawRearViewMirror();
        }
        drawHudOverlay();
    }

    // Upscale cockpit layer and composite non-transparent pixels onto displayFb
    for (int y = 0; y < Framebuffer::H; ++y) {
        const uint32_t* sRow = fb.row(y);
        uint32_t* d0 = displayFb.row(y * 3);
        uint32_t* d1 = displayFb.row(y * 3 + 1);
        uint32_t* d2 = displayFb.row(y * 3 + 2);
        for (int x = 0; x < Framebuffer::W; ++x) {
            uint32_t c = sRow[x];
            if (c != KEY_TRANSPARENT) {
                int dx = x * 3;
                d0[dx] = d0[dx + 1] = d0[dx + 2] = c;
                d1[dx] = d1[dx + 1] = d1[dx + 2] = c;
                d2[dx] = d2[dx + 1] = d2[dx + 2] = c;
            } else if (y >= 14 && y <= 37) {
                // Apply windshield top sun-visor tint directly onto the 960x720 3D world rows
                int dx = x * 3;
                for (int ry = 0; ry < 3; ++ry) {
                    uint32_t* dRow = displayFb.row(y * 3 + ry);
                    for (int rx = 0; rx < 3; ++rx) {
                        uint32_t wc = dRow[dx + rx];
                        dRow[dx + rx] = (y <= 25)
                            ? (0xFF000000u | (wc - ((wc >> 2) & 0x003F3F3Fu)))
                            : (0xFF000000u | (wc - ((wc >> 3) & 0x001F1F1Fu)));
                    }
                }
            }
        }
    }

    // Render high-resolution 3D-shaded instrument binnacle, beveled gauges & flat-bottom GT wheel
    RenderCockpit3D(pipe3d, vehicle, cockpit, getCarSpec(selectedCar), wipersOn, wiperPhase, weatherMode);
}

void Game::render() {
    auto frameStart = std::chrono::steady_clock::now();

    // 1. Prepare (Pass A road projection + depth-sorted sprite draw list)
    auto tA0 = std::chrono::steady_clock::now();
    projectRoad(track, cam, ctx);
    buildDrawList(track, traffic.cars, cfg::MAX_TRAFFIC, ctx, atlas, drawList);
    auto tA1 = std::chrono::steady_clock::now();
    timers.passAUs.push(std::chrono::duration<float, std::micro>(tA1 - tA0).count());

    // Unit test mode or Classic 2.5D mode uses the Phase 1-8 slice scheduler on fb
    if (testMode || !use3DPipeline) {
        auto tB0 = std::chrono::steady_clock::now();
        FrameJob job{ &ctx, &drawList, &atlas, &track, &cockpit, &fb };
        scheduler.run(job);
        auto tB1 = std::chrono::steady_clock::now();
        timers.passBUs.push(std::chrono::duration<float, std::micro>(tB1 - tB0).count());

        auto tO0 = std::chrono::steady_clock::now();
        if (!testMode) {
            drawRoadHeadlightsAndSheen();
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

        if (vehicle.fade > 0.01f) {
            int level = mathutil::clamp(static_cast<int>(std::lround(vehicle.fade * 4.0f)), 1, 4);
            fadeRows(fb, 0, cfg::SCREEN_H, level);
        }

        if (!testMode) {
            displayFb.upscaleFromBase(fb, pixelSize);
            RenderCockpit3D(pipe3d, vehicle, cockpit, getCarSpec(selectedCar), wipersOn, wiperPhase, weatherMode);
        }
        auto tO1 = std::chrono::steady_clock::now();
        timers.overlayUs.push(std::chrono::duration<float, std::micro>(tO1 - tO0).count());
        if (testMode) {
            timers.totalMs.push(std::chrono::duration<float, std::milli>(tO1 - frameStart).count());
            return;
        }
    } else {
        // Full-Depth 3D Software Raster Pipeline (Z-Buffer, DrawTrapezoid, DrawTriangle3D Meshes, Lambert Shading & Fog)
        auto tB0 = std::chrono::steady_clock::now();
        pipe3d.frameTick = tickCount;
        RenderFrame(pipe3d, track, vehicle, traffic, cam, ctx, cockpit, displayFb);
        auto tB1 = std::chrono::steady_clock::now();
        timers.passBUs.push(std::chrono::duration<float, std::micro>(tB1 - tB0).count());

        // Forward-Pass Cockpit / Dash Layering (depth test disabled)
        auto tO0 = std::chrono::steady_clock::now();
        compositeCockpitForwardPass();
        auto tO1 = std::chrono::steady_clock::now();
        timers.overlayUs.push(std::chrono::duration<float, std::micro>(tO1 - tO0).count());
    }

    // High-Resolution HUD, AR Passing-Gap Rails, 2D GPS & Menus on DisplayBuffer
    if (screenState == ScreenState::LaunchMenu) {
        renderLaunchScreen();
    } else {
        if (showGapGuide && (screenState == ScreenState::Racing || screenState == ScreenState::Countdown)) {
            renderArPassingGapAndTargets();
        }
        renderModernCockpitDetails();
        renderHighResHudAndGps();

        if (screenState == ScreenState::Countdown) {
            renderCountdownOverlay();
        } else if (screenState == ScreenState::Paused) {
            renderPauseOverlay();
        } else if (screenState == ScreenState::Wrecked) {
            renderWreckedOverlay();
        } else if (screenState == ScreenState::RaceOver) {
            renderRaceOverOverlay();
        }
    }

    displayFb.applyPostProcess(pixelSize, crtScanlines);

    auto frameEnd = std::chrono::steady_clock::now();
    timers.totalMs.push(std::chrono::duration<float, std::milli>(frameEnd - frameStart).count());
}

void Game::drawRoadHeadlightsAndSheen() {
    if (ctx.theme != 1 && ctx.theme != 2 && weatherMode != 1) return;
    for (int y = 135; y < 185; ++y) {
        const RowInfo& r = ctx.rows[y];
        if (r.seg == ROW_INVALID) continue;
        float t = static_cast<float>(y - 135) / 50.0f;
        int beamHalfW = static_cast<int>(r.hw * (0.28f + 0.35f * t));
        int x0 = std::max(22, static_cast<int>(r.cx) - beamHalfW);
        int x1 = std::min(298, static_cast<int>(r.cx) + beamHalfW);
        uint32_t* row = fb.row(y);
        for (int x = x0; x < x1; ++x) {
            uint32_t c = row[x];
            uint32_t add = ((c >> 3) & 0x001F1F1Fu);
            row[x] = 0xFF000000u | std::min(0x00FFFFFFu, (c & 0x00FFFFFFu) + add);
        }
    }
}

void Game::renderArPassingGapAndTargets() {
    if (track.count <= 0) return;

    const float playerHW = vphys::PLAYER_HALF_W * vehicle.hitboxScale;
    int   threatCarIdx = -1;
    float minThreatDist = 1e9f;
    bool  tightRisk = false;

    for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) {
        const Car& c = traffic.cars[i];
        if (!c.active) continue;
        float dz = mathutil::wrapDist(c.z, vehicle.z, track.length);
        if (dz > 100.0f && dz < 4200.0f) {
            float carHW = (atlas.img[c.spriteId].worldW / (2.0f * cfg::ROAD_HALF_W)) * vehicle.hitboxScale;
            float safeLim = playerHW + carHW;
            float dx = std::fabs(vehicle.x - c.offset);
            if (dx < safeLim * 1.35f && dz < minThreatDist) {
                minThreatDist = dz;
                threatCarIdx  = i;
                tightRisk     = (dx < safeLim * 1.05f);
            }
        }
    }

    uint32_t railCol = RGB(40, 255, 130);
    if (vehicle.drafting > 0.25f && !tightRisk) {
        railCol = RGB(0, 235, 255);
    } else if (threatCarIdx != -1) {
        railCol = tightRisk ? RGB(255, 55, 65) : RGB(255, 210, 45);
    }

    const float centerX = (cfg::SCREEN_W * 0.5f) + ctx.shiftX;
    for (int n = 4; n < 24; ++n) {
        const RoadPoint& p0 = ctx.pts[n];
        const RoadPoint& p1 = ctx.pts[n + 1];
        if (p0.sy <= 16 || p1.sy <= 16 || p0.sy >= ctx.clipY[n] || p0.sy >= 176) continue;

        float s0 = cfg::FOCAL_PX / p0.cz;
        float s1 = cfg::FOCAL_PX / p1.cz;

        for (int side = -1; side <= 1; side += 2) {
            float worldOff = (vehicle.x + side * playerHW) * cfg::ROAD_HALF_W;
            int x0 = static_cast<int>(std::lround((centerX + s0 * (p0.wx + worldOff)) * 3.0f));
            int y0 = static_cast<int>(std::lround(p0.sy * 3.0f));
            int x1 = static_cast<int>(std::lround((centerX + s1 * (p1.wx + worldOff)) * 3.0f));
            int y1 = static_cast<int>(std::lround(p1.sy * 3.0f));

            if (y0 >= 48 && y0 < 528 && y1 >= 48 && y1 < 528) {
                displayFb.drawLine(x0, y0, x1, y1, 1, railCol);
            }
        }
    }

    for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) {
        const Car& c = traffic.cars[i];
        if (!c.active) continue;
        float dz = mathutil::wrapDist(c.z, vehicle.z, track.length);
        if (dz < 250.0f || dz > 4800.0f) continue;

        float zNorm = mathutil::wrap(c.z, track.length) / cfg::SEG_LEN;
        int segIdx  = static_cast<int>(std::floor(zNorm)) % track.count;
        int n       = (segIdx - ctx.baseSeg + track.count) % track.count;
        if (n <= 0 || n >= cfg::DRAW_DIST) continue;

        float t = zNorm - std::floor(zNorm);
        const RoadPoint& p0 = ctx.pts[n];
        const RoadPoint& p1 = ctx.pts[n + 1];
        float cz = (1.0f - t) * p0.cz + t * p1.cz;
        float wx = (1.0f - t) * p0.wx + t * p1.wx + c.offset * cfg::ROAD_HALF_W;
        float wy = (1.0f - t) * p0.wy + t * p1.wy;
        float s  = cfg::FOCAL_PX / cz;

        int sx960 = static_cast<int>(std::lround((centerX + s * wx) * 3.0f));
        int sy720 = static_cast<int>(std::lround((static_cast<float>(ctx.horizonY) - s * wy) * 3.0f));
        int hPx720 = static_cast<int>(std::lround(s * atlas.img[c.spriteId].worldH * 3.0f));
        int wPx960 = static_cast<int>(std::lround(s * atlas.img[c.spriteId].worldW * 3.0f));

        int topY = sy720 - hPx720 - 18;
        if (topY < 50 || sy720 >= ctx.clipY[n] * 3 || sx960 < 90 || sx960 > 870) continue;

        float carHW   = (atlas.img[c.spriteId].worldW / (2.0f * cfg::ROAD_HALF_W)) * vehicle.hitboxScale;
        float safeLim = playerHW + carHW;
        float dx      = std::fabs(vehicle.x - c.offset);

        uint32_t bCol = (dx < safeLim) ? RGB(255, 60, 65)
                      : (dx < safeLim * 1.45f) ? RGB(255, 210, 50)
                                               : RGB(70, 245, 140);

        int bw = std::clamp(wPx960, 44, 150);
        int bx = sx960 - bw / 2;
        displayFb.drawRect(bx, topY, bw, 3, bCol);
        displayFb.drawRect(bx, topY, 3, 10, bCol);
        displayFb.drawRect(bx + bw - 3, topY, 3, 10, bCol);

        if (i == threatCarIdx && tightRisk) {
            bool passLeft = (c.offset > 0.0f);
            const char* passHint = passLeft ? "<< PASS LEFT" : "PASS RIGHT >>";
            int tw = DisplayBuffer::textWidth(passHint, 2);
            displayFb.drawRectAlpha(sx960 - tw / 2 - 6, topY - 24, tw + 12, 20, RGB(12, 16, 26), 215);
            displayFb.drawText(sx960 - tw / 2, topY - 21, passHint, RGB(80, 255, 140), 2, true);
        }
    }
}

void Game::renderModernCockpitDetails() {
    const CarSpec& spec = getCarSpec(selectedCar);
    const int ox960 = cockpit.ox() * 3;
    const int oy720 = cockpit.oy() * 3;

    displayFb.drawRect(60 + ox960, 576 + oy720, 840, 3, spec.accentColor);

    // Left-Side Wing Mirror
    {
        int mx = 16 + ox960, my = 456 + oy720, mw = 86, mh = 48;
        displayFb.drawRect(mx - 3, my - 3, mw + 6, mh + 6, RGB(45, 52, 64));
        displayFb.drawBorder(mx - 3, my - 3, mw + 6, mh + 6, 2, spec.accentColor);
        displayFb.drawRect(mx, my, mw, mh / 2, ctx.skyLut[190]);
        displayFb.drawRect(mx, my + mh / 2, mw, mh / 2, ctx.pal[2][0].road);
        displayFb.drawText(mx + 6, my + 5, "BLIND SPOT", RGB(180, 215, 245), 1, true);

        bool blindWarn = false;
        for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) {
            if (!traffic.cars[i].active) continue;
            float d = mathutil::wrapDist(traffic.cars[i].z, vehicle.z, track.length);
            if (d < 100.0f && d > -1200.0f && std::fabs(traffic.cars[i].offset - vehicle.x) < 0.65f) {
                blindWarn = true;
                break;
            }
        }
        if (blindWarn) {
            displayFb.drawRect(mx + 8, my + 24, mw - 16, 18, RGB(255, 65, 55));
            displayFb.drawText(mx + 14, my + 29, "! CAR NEAR !", RGB(255, 255, 255), 1, false);
        } else {
            displayFb.drawText(mx + 22, my + 29, "CLEAR", RGB(80, 245, 140), 1, true);
        }
    }

    // High-Res Car Health & Integrity Display on Left Dashboard Panel
    {
        int hx = 24 + ox960, hy = 668 + oy720, hw = 136, hh = 38;
        displayFb.drawRectAlpha(hx, hy, hw, hh, RGB(10, 14, 22), 230);
        uint32_t hCol = (vehicle.health > 60.0f) ? RGB(50, 245, 130)
                      : (vehicle.health > 28.0f) ? RGB(255, 195, 40)
                                                 : RGB(255, 50, 55);
        displayFb.drawBorder(hx, hy, hw, hh, 2, hCol);

        char buf[32];
        std::snprintf(buf, sizeof(buf), "HEALTH %03.0F%%", vehicle.health);
        displayFb.drawText(hx + 10, hy + 6, buf, hCol, 2, true);

        int barW = static_cast<int>(std::lround((vehicle.health / 100.0f) * (hw - 20)));
        displayFb.drawRect(hx + 10, hy + 25, hw - 20, 7, RGB(30, 35, 45));
        displayFb.drawRect(hx + 10, hy + 25, std::max(0, barW), 7, hCol);
    }

    // Right Dashboard ADAS / Car Spec / 3D Pipeline Telemetry Panel
    {
        int rx = 800 + ox960, ry = 668 + oy720, rw = 136, rh = 38;
        displayFb.drawRectAlpha(rx, ry, rw, rh, RGB(10, 14, 22), 230);
        displayFb.drawBorder(rx, ry, rw, rh, 2, spec.accentColor);

        displayFb.drawText(rx + 8, ry + 6, spec.name, spec.accentColor, 1, true);
        const char* adasTxt = steerAssistOn ? "ADAS:ON [G] 3D[E]" : "ADAS:OFF[G] 3D[E]";
        displayFb.drawText(rx + 8, ry + 20, adasTxt,
                           steerAssistOn ? RGB(80, 255, 150) : RGB(195, 205, 225), 1, true);
    }

    if (vehicle.drafting > 0.15f) {
        int dx = 390 + ox960, dy = 528 + oy720, dw = 180, dh = 26;
        displayFb.drawRectAlpha(dx, dy, dw, dh, RGB(8, 22, 38), 220);
        displayFb.drawBorder(dx, dy, dw, dh, 2, RGB(0, 240, 255));
        displayFb.drawText(dx + 14, dy + 6, "SLIPSTREAM DRAFTING!", RGB(0, 245, 255), 2, true);
    }
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

        Cockpit::drawRect(fb, cx - hw - postW, topY, postW, sy - topY, postCol);
        Cockpit::drawRect(fb, cx + hw,         topY, postW, sy - topY, postCol);

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
            if (absC >= 2.2f)      segCol = RGB(255, 65, 65);
            else if (absC >= 1.0f) segCol = RGB(255, 215, 50);
            else                   segCol = RGB(0, 245, 180);
        }
        displayFb.drawLine(x0, y0, x1, y1, thickness, segCol);

        if (track.segs[i].flags & 0x02) {
            displayFb.drawCircle(x0, y0, 4, RGB(255, 255, 255), true);
        } else if (track.segs[i].flags & 0x01) {
            displayFb.drawCircle(x0, y0, 3, RGB(0, 235, 255), true);
        }
    }

    for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) {
        if (!traffic.cars[i].active) continue;
        int cSeg = static_cast<int>(std::floor(mathutil::wrap(traffic.cars[i].z, track.length) / cfg::SEG_LEN)) % track.count;
        int tx, ty;
        projectPt(cSeg, tx, ty);
        if (tx >= boxX + 5 && tx < boxX + boxW - 5 && ty >= boxY + 5 && ty < boxY + boxH - 5) {
            displayFb.drawCircle(tx, ty, 3, RGB(255, 75, 75), true);
        }
    }

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

    displayFb.drawRectAlpha(0, 0, DisplayBuffer::W, 42, RGB(10, 14, 22), 215);
    displayFb.drawRect(0, 40, DisplayBuffer::W, 2, RGB(50, 140, 220));

    std::snprintf(buf, sizeof(buf), "SCORE %07u", score);
    displayFb.drawText(14, 12, buf, RGB(255, 255, 255), 2, true);

    std::snprintf(buf, sizeof(buf), "HI %07u", bestScore);
    uint32_t hiCol = newHighScoreAchieved ? RGB(255, 215, 40) : RGB(120, 215, 255);
    displayFb.drawText(195, 12, buf, hiCol, 2, true);

    if (gameMode == GameMode::ArcadeCheckpoint) {
        uint32_t tCol = (timeLeftSec < 10.0f && ((tickCount / 10) & 1)) ? RGB(255, 60, 60) : RGB(255, 235, 80);
        std::snprintf(buf, sizeof(buf), "TIME %02.1FS", timeLeftSec);
        displayFb.drawText(595, 12, buf, tCol, 2, true);
    } else if (gameMode == GameMode::GrandPrix3Lap) {
        std::snprintf(buf, sizeof(buf), "LAP %d/3  POS %d/%d",
                      std::min(3, vehicle.lap), racePosition, traffic.activeCount + 1);
        displayFb.drawText(575, 12, buf, RGB(255, 230, 80), 2, true);
    } else {
        int lMin = static_cast<int>(vehicle.lapTime) / 60;
        int lSec = static_cast<int>(vehicle.lapTime) % 60;
        std::snprintf(buf, sizeof(buf), "LAP %d  %02d:%02d", vehicle.lap, lMin, lSec);
        displayFb.drawText(590, 12, buf, RGB(140, 240, 180), 2, true);
    }

    std::snprintf(buf, sizeof(buf), "G:%d  %dX  PASS:%d", vehicle.gear, comboMult, vehicle.overtakes);
    displayFb.drawText(760, 12, buf, (comboMult > 1) ? RGB(80, 255, 150) : RGB(230, 235, 245), 2, true);

    if (gpsMode == 0) {
        renderGpsMap(772, 52, 168, 156, true);
    } else if (gpsMode == 1) {
        renderGpsMap(732, 52, 208, 196, false);
    }

    if (screenState == ScreenState::Racing) {
        renderRallyCallout();
    }

    const TrackMetadata& meta = getTrackMetadata(selectedTrack);
    std::snprintf(buf, sizeof(buf), "%s | [E]3D-MESH:%s | [H]REPAIR | [G]ASSIST:%s | [Q]GAP",
                  meta.name, use3DPipeline ? "ON" : "2.5D", steerAssistOn ? "ON" : "OFF");
    displayFb.drawText(16, 50, buf, RGB(170, 205, 235), 1, true);

    if (bannerTimer > 0.0f && bannerText[0] != '\0') {
        int tw = DisplayBuffer::textWidth(bannerText, 3);
        int bx = (DisplayBuffer::W - tw) / 2 - 16;
        int by = 104;
        displayFb.drawRectAlpha(bx, by, tw + 32, 38, RGB(10, 14, 24), 215);
        displayFb.drawBorder(bx, by, tw + 32, 38, 2, bannerColor);
        displayFb.drawText(bx + 16, by + 9, bannerText, bannerColor, 3, true);
    }

    if (showDebugHud) {
        displayFb.drawRectAlpha(16, 68, 360, 130, RGB(12, 16, 26), 220);
        displayFb.drawBorder(16, 68, 360, 130, 2, RGB(80, 190, 255));
        std::snprintf(buf, sizeof(buf), "FRAME %.2FMS (P99 %.2FMS)", timers.totalMs.avg(), timers.totalMs.p99());
        displayFb.drawText(28, 78, buf, RGB(100, 255, 150), 2, true);
        std::snprintf(buf, sizeof(buf), "TICK:%.0FUS PA:%.0FUS 3D-RAST:%.0FUS",
                      timers.tickUs.avg(), timers.passAUs.avg(), timers.passBUs.avg());
        displayFb.drawText(28, 104, buf, RGB(220, 235, 255), 1, true);
        std::snprintf(buf, sizeof(buf), "PIPELINE:%s PIXEL:%dPX",
                      use3DPipeline ? "3D Z-BUFFER" : "2.5D SLICE", pixelSize);
        displayFb.drawText(28, 120, buf, RGB(220, 235, 255), 1, true);
        for (int s = 0; s < cfg::NUM_SLICES; ++s) {
            int us = static_cast<int>(scheduler.stats[s].ns / 1000u);
            int bw = std::clamp(us, 4, 38);
            displayFb.drawRect(28 + s * 42, 142, bw, 10, RGB(60, 195, 255));
        }
        displayFb.drawText(28, 164, "E:3D-PIPE F:FULLSCREEN G:ASSIST T:TRAFFIC H:REPAIR", RGB(165, 190, 220), 1, true);
    }
}

void Game::renderLaunchScreen() {
    displayFb.drawRectAlpha(0, 0, DisplayBuffer::W, DisplayBuffer::H, RGB(8, 12, 24), 155);

    displayFb.drawRectAlpha(40, 18, 880, 138, RGB(12, 18, 34), 225);
    displayFb.drawBorder(40, 18, 880, 138, 3, RGB(0, 225, 255));
    displayFb.drawRect(46, 146, 868, 4, RGB(255, 50, 140));

    const char* title = "ARGON : OVERDRIVE";
    int titleW = DisplayBuffer::textWidth(title, 6);
    displayFb.drawText((DisplayBuffer::W - titleW) / 2, 36, title, RGB(0, 245, 255), 6, true);

    const char* sub = "FULL-DEPTH 3D SOFTWARE-RASTERIZED FIRST-PERSON RACER (QT 6)";
    int subW = DisplayBuffer::textWidth(sub, 2);
    displayFb.drawText((DisplayBuffer::W - subW) / 2, 98, sub, RGB(255, 210, 80), 2, true);

    const char* hint = "ARROWS / WASD / MOUSE TO SELECT   |   LEFT/RIGHT OR CLICK TO ADJUST   |   ENTER TO START";
    int hintW = DisplayBuffer::textWidth(hint, 1);
    displayFb.drawText((DisplayBuffer::W - hintW) / 2, 126, hint, RGB(180, 205, 235), 1, true);

    displayFb.drawRectAlpha(40, 170, 480, 474, RGB(12, 18, 32), 230);
    displayFb.drawBorder(40, 170, 480, 474, 2, RGB(70, 130, 210));
    displayFb.drawText(60, 182, "GARAGE, TRACK & RACE SETTINGS", RGB(255, 190, 60), 2, true);

    const TrackMetadata& meta = getTrackMetadata(selectedTrack);
    const CarSpec&       car  = getCarSpec(selectedCar);

    char items[LAUNCH_MENU_ITEMS][64];
    std::snprintf(items[0], 64, ">> START RACE NOW <<");
    std::snprintf(items[1], 64, "CAR     : < %s (%dKM/H) >", car.name, car.topSpeedKmh);
    std::snprintf(items[2], 64, "TRACK   : < %d/5 %s >", selectedTrack + 1, meta.name);
    std::snprintf(items[3], 64, "TRAFFIC : < %s >", DENSITY_NAMES[selectedDensity]);
    std::snprintf(items[4], 64, "ASSIST  : < STEER ASSIST %s >", steerAssistOn ? "ON (ADAS)" : "OFF (MANUAL)");
    std::snprintf(items[5], 64, "MODE    : < %s >", MODE_NAMES[static_cast<int>(gameMode)]);
    std::snprintf(items[6], 64, "PIXEL   : < %s >", PIXEL_PRESET_NAMES[pixelSizeIdx]);
    std::snprintf(items[7], 64, "SCALE   : < %s >", SCALE_NAMES[static_cast<int>(scaleMode)]);
    std::snprintf(items[8], 64, "SCREEN  : < FULLSCREEN %s >", isFullscreen ? "ON" : "OFF (PRESS F)");
    std::snprintf(items[9], 64, "EXIT TO DESKTOP");

    for (int i = 0; i < LAUNCH_MENU_ITEMS; ++i) {
        int iy = 216 + i * 41;
        bool sel = (menuCursor == i);
        if (sel) {
            displayFb.drawRectAlpha(52, iy - 5, 456, 33, RGB(0, 160, 230), 185);
            displayFb.drawBorder(52, iy - 5, 456, 33, 2, RGB(120, 250, 255));
        }
        uint32_t col = (i == 0) ? (sel ? RGB(100, 255, 140) : RGB(60, 230, 110))
                     : (i == 1) ? car.accentColor
                                : (sel ? RGB(255, 255, 255) : RGB(185, 200, 225));
        displayFb.drawText(64, iy + 4, items[i], col, 2, true);
    }

    displayFb.drawRectAlpha(535, 170, 385, 474, RGB(12, 18, 32), 230);
    displayFb.drawBorder(535, 170, 385, 474, 2, RGB(70, 130, 210));

    char buf[96];
    std::snprintf(buf, sizeof(buf), "TRACK %d: %s", selectedTrack + 1, meta.name);
    displayFb.drawText(555, 184, buf, RGB(0, 240, 255), 2, true);
    displayFb.drawText(555, 206, meta.subtitle, RGB(190, 210, 235), 1, true);

    renderGpsMap(555, 222, 345, 195, false);

    displayFb.drawRect(555, 426, 345, 2, car.accentColor);
    std::snprintf(buf, sizeof(buf), "CAR: %s (%s)", car.name, car.subtitle);
    displayFb.drawText(555, 434, buf, car.accentColor, 1, true);
    std::snprintf(buf, sizeof(buf), "TOP SPD: %d KM/H  |  ARMOR: %.0F%%  |  GRIP: %.0F%%",
                  car.topSpeedKmh, car.armorMult * 100.0f, car.steerMult * 100.0f);
    displayFb.drawText(555, 450, buf, RGB(220, 235, 255), 1, true);

    const TrackRecord& rec = highScores.record(selectedTrack);
    displayFb.drawRect(555, 470, 345, 2, RGB(60, 110, 180));
    displayFb.drawText(555, 478, "TRACK HALL OF FAME (SAVED RECORDS)", RGB(255, 120, 190), 2, true);

    std::snprintf(buf, sizeof(buf), "HIGHEST SCORE  : %07u PTS", rec.highScore);
    displayFb.drawText(555, 506, buf, RGB(255, 240, 90), 2, true);

    if (rec.bestLapSec > 0.0f) {
        int bMin = static_cast<int>(rec.bestLapSec) / 60;
        int bSec = static_cast<int>(rec.bestLapSec) % 60;
        int bSub = static_cast<int>((rec.bestLapSec - std::floor(rec.bestLapSec)) * 100.0f);
        std::snprintf(buf, sizeof(buf), "BEST LAP TIME  : %02d:%02d.%02d", bMin, bSec, bSub);
    } else {
        std::snprintf(buf, sizeof(buf), "BEST LAP TIME  : --:--.--");
    }
    displayFb.drawText(555, 534, buf, RGB(100, 255, 170), 2, true);

    std::snprintf(buf, sizeof(buf), "TOP SPEED REC  : %.0F KM/H", rec.maxSpeedKmh);
    displayFb.drawText(555, 562, buf, RGB(130, 220, 255), 2, true);

    std::snprintf(buf, sizeof(buf), "MOST OVERTAKES : %d CARS", rec.mostOvertakes);
    displayFb.drawText(555, 590, buf, RGB(220, 195, 255), 2, true);

    displayFb.drawRectAlpha(40, 656, 880, 44, RGB(12, 18, 32), 230);
    displayFb.drawBorder(40, 656, 880, 44, 2, RGB(60, 110, 180));
    const char* ctl = "WASD/ARROWS: DRIVE | SPACE: NITRO | E: 3D PIPELINE | H: REPAIR | G: ASSIST | T: TRAFFIC | F: FULLSCREEN";
    int ctlW = DisplayBuffer::textWidth(ctl, 1);
    displayFb.drawText((DisplayBuffer::W - ctlW) / 2, 674, ctl, RGB(210, 230, 255), 1, true);
}

void Game::renderCountdownOverlay() {
    const int gx = 340, gy = 135, gw = 280, gh = 110;
    displayFb.drawRectAlpha(gx, gy, gw, gh, RGB(12, 16, 26), 230);
    displayFb.drawBorder(gx, gy, gw, gh, 3, RGB(200, 210, 230));

    int stage = static_cast<int>(std::ceil(countdownTimer));
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
    displayFb.drawRectAlpha(0, 0, DisplayBuffer::W, DisplayBuffer::H, RGB(6, 10, 20), 175);
    const int bx = 240, by = 140, bw = 480, bh = 420;
    displayFb.drawRectAlpha(bx, by, bw, bh, RGB(14, 20, 36), 238);
    displayFb.drawBorder(bx, by, bw, bh, 3, RGB(0, 225, 255));

    const char* hdr = "RACE PAUSED";
    int hw = DisplayBuffer::textWidth(hdr, 4);
    displayFb.drawText(bx + (bw - hw) / 2, by + 24, hdr, RGB(255, 225, 70), 4, true);

    char opts[PAUSE_MENU_ITEMS][64];
    std::snprintf(opts[0], 64, "RESUME RACE");
    std::snprintf(opts[1], 64, "REPAIR CAR TO 100%% & CONTINUE");
    std::snprintf(opts[2], 64, "STEER ASSIST : < %s >", steerAssistOn ? "ON (ADAS)" : "OFF (MANUAL)");
    std::snprintf(opts[3], 64, "TRAFFIC      : < %s >", DENSITY_NAMES[selectedDensity]);
    std::snprintf(opts[4], 64, "PIXEL QUALITY: < %s >", PIXEL_PRESET_NAMES[pixelSizeIdx]);
    std::snprintf(opts[5], 64, "TOGGLE FULLSCREEN (F)");
    std::snprintf(opts[6], 64, "RETURN TO LAUNCH SCREEN");

    for (int i = 0; i < PAUSE_MENU_ITEMS; ++i) {
        int iy = 232 + i * 42;
        bool sel = (pauseCursor == i);
        if (sel) {
            displayFb.drawRectAlpha(bx + 20, iy - 5, bw - 40, 34, RGB(0, 155, 225), 185);
            displayFb.drawBorder(bx + 20, iy - 5, bw - 40, 34, 2, RGB(120, 250, 255));
        }
        displayFb.drawText(bx + 32, iy + 5, opts[i], sel ? RGB(255, 255, 255) : RGB(185, 205, 230), 2, true);
    }
}

void Game::renderWreckedOverlay() {
    displayFb.drawRectAlpha(0, 0, DisplayBuffer::W, DisplayBuffer::H, RGB(35, 6, 10), 165);
    const int bx = 190, by = 160, bw = 580, bh = 340;
    displayFb.drawRectAlpha(bx, by, bw, bh, RGB(18, 14, 24), 242);
    displayFb.drawBorder(bx, by, bw, bh, 3, RGB(255, 55, 60));

    const char* hdr = "CRITICAL VEHICLE WRECKED!";
    int hw = DisplayBuffer::textWidth(hdr, 4);
    displayFb.drawText(bx + (bw - hw) / 2, by + 30, hdr, RGB(255, 65, 65), 4, true);

    const char* sub = "CAR HEALTH REACHED 0% — ENGINE & CHASSIS DISABLED";
    int sw = DisplayBuffer::textWidth(sub, 2);
    displayFb.drawText(bx + (bw - sw) / 2, by + 84, sub, RGB(255, 215, 80), 2, true);

    displayFb.drawRectAlpha(bx + 50, by + 130, bw - 100, 48, RGB(0, 140, 90), 210);
    displayFb.drawBorder(bx + 50, by + 130, bw - 100, 48, 2, RGB(80, 255, 160));
    displayFb.drawText(bx + 78, by + 146, "PRESS [R] OR [H] : REPAIR HERE & CONTINUE!", RGB(255, 255, 255), 2, true);

    displayFb.drawRectAlpha(bx + 50, by + 195, bw - 100, 48, RGB(25, 80, 170), 210);
    displayFb.drawBorder(bx + 50, by + 195, bw - 100, 48, 2, RGB(90, 200, 255));
    displayFb.drawText(bx + 78, by + 211, "PRESS [ENTER]    : RESTART RACE FROM GRID", RGB(255, 255, 255), 2, true);

    displayFb.drawText(bx + 125, by + 275, "PRESS [ESC] TO RETURN TO LAUNCH MENU", RGB(180, 200, 230), 2, true);
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

    std::snprintf(buf, sizeof(buf), "TOTAL OVERTAKES  : %d CARS (POS %d/%d)",
                  vehicle.overtakes, racePosition, traffic.activeCount + 1);
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
