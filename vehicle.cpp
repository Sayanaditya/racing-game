#include "vehicle.h"
#include "collision.h"
#include "mathutil.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace {

const CarSpec CAR_SPECS[NUM_CAR_PRESETS] = {
    { "APEX GT-R",      "BALANCED TWIN-TURBO COUPE", 300, 1.00f, 1.00f, 1.00f, 1.00f, RGB(0, 235, 255),   RGB(30, 110, 220) },
    { "VORTEX V12",     "HYPERCAR INTERCEPTOR",      345, 1.15f, 1.24f, 0.94f, 0.85f, RGB(255, 55, 95),   RGB(215, 30, 45)  },
    { "DRIFT SPEC-R",   "TOUGE TRACK SPECIAL",       288, 0.96f, 1.14f, 1.28f, 0.95f, RGB(255, 195, 35),  RGB(235, 165, 20) },
    { "TITAN ENFORCER", "ARMORED ENDURANCE GT",      294, 0.98f, 0.95f, 1.02f, 1.55f, RGB(50, 245, 130),  RGB(40, 155, 90)  }
};

inline constexpr float LANE_SPEED_FRAC[3] = { 0.28f, 0.36f, 0.44f };
inline constexpr float GEAR_HI[5] = { 0.12f, 0.26f, 0.44f, 0.68f, 1.05f };
inline constexpr float GEAR_LO[5] = { 0.00f, 0.12f * 0.90f, 0.26f * 0.90f, 0.44f * 0.90f, 0.68f * 0.90f };

inline float laneCenterOffset(int lane) {
    return -1.0f + (2.0f * static_cast<float>(lane) + 1.0f) / static_cast<float>(cfg::LANES);
}

uint16_t pickTrafficSprite(uint32_t& rng) {
    static const uint16_t IDS[4] = { SPR_CAR_RED, SPR_CAR_BLUE, SPR_CAR_YELLOW, SPR_TRUCK };
    return IDS[mathutil::randRange(rng, 4)];
}

void placeRespawn(Vehicle& v, const Track& track, const Traffic& traffic) {
    v.speed   = 0.0f;
    v.latV    = 0.0f;
    v.yawPx   = 0.0f;
    v.tumblePx = 0.0f;
    v.crackProgress = 0.0f;
    v.z = mathutil::wrap(v.z - 400.0f, track.length);

    int   bestLane = 1;
    float bestMinDist = -1.0f;

    for (int lane = 0; lane < cfg::LANES; ++lane) {
        float laneX = laneCenterOffset(lane);
        float nearest = 1e9f;
        for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) {
            const Car& c = traffic.cars[i];
            if (!c.active) continue;
            if (std::fabs(c.offset - laneX) > 0.35f) continue;
            float d = mathutil::wrapDist(c.z, v.z, track.length);
            if (d >= -1500.0f && d <= 4000.0f) {
                nearest = std::min(nearest, std::fabs(d));
            }
        }
        if (nearest > bestMinDist) {
            bestMinDist = nearest;
            bestLane = lane;
        }
    }
    v.x = laneCenterOffset(bestLane);
}

} // namespace

const CarSpec& getCarSpec(int carIdx) {
    int idx = ((carIdx % NUM_CAR_PRESETS) + NUM_CAR_PRESETS) % NUM_CAR_PRESETS;
    return CAR_SPECS[idx];
}

void initTraffic(Traffic& t, const Track& track, uint32_t seed, TrafficDensity density) {
    t.rng = seed ? seed : 1u;
    t.density = density;
    t.activeCount = (density == TrafficDensity::Low) ? 8
                  : (density == TrafficDensity::Medium) ? 16
                  : cfg::MAX_TRAFFIC;

    int segSpacing = (density == TrafficDensity::Low) ? 24
                   : (density == TrafficDensity::Medium) ? 14
                   : 10;

    for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) {
        int lane = i % cfg::LANES;
        t.cars[i].active   = (i < t.activeCount) ? 1 : 0;
        t.cars[i].offset   = laneCenterOffset(lane);
        t.cars[i].speed    = LANE_SPEED_FRAC[lane] * cfg::MAX_SPEED;
        t.cars[i].spriteId = pickTrafficSprite(t.rng);
        float zSeg = static_cast<float>(24 + i * segSpacing);
        t.cars[i].z = mathutil::wrap(zSeg * cfg::SEG_LEN, track.length > 0.0f ? track.length : 100000.0f);
        t.prevZ[i]  = t.cars[i].z;
    }
}

void updateTraffic(Traffic& t, const Vehicle& player, const Track& track) {
    if (track.length <= 0.0f) return;
    const float maxAhead = static_cast<float>(cfg::DRAW_DIST + 40) * cfg::SEG_LEN;
    float minSepSegs = (t.density == TrafficDensity::Low) ? 22.0f
                     : (t.density == TrafficDensity::Medium) ? 15.0f
                     : 12.0f;

    for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) {
        Car& car = t.cars[i];
        if (!car.active) continue;
        t.prevZ[i] = car.z;
        car.z = mathutil::wrap(car.z + car.speed * cfg::STEP, track.length);

        float d = mathutil::wrapDist(car.z, player.z, track.length);
        if (d < -3000.0f || d > maxAhead) {
            int lane = mathutil::randRange(t.rng, cfg::LANES);
            float candZ = 0.0f;
            for (int attempt = 0; attempt < 10; ++attempt) {
                lane = mathutil::randRange(t.rng, cfg::LANES);
                float off = laneCenterOffset(lane);
                candZ = mathutil::wrap(player.z + static_cast<float>(45 + mathutil::randRange(t.rng, 140)) * cfg::SEG_LEN,
                                       track.length);
                bool conflict = false;
                for (int j = 0; j < cfg::MAX_TRAFFIC; ++j) {
                    if (j == i || !t.cars[j].active) continue;
                    if (std::fabs(t.cars[j].offset - off) < 0.25f) {
                        float sep = std::fabs(mathutil::wrapDist(candZ, t.cars[j].z, track.length));
                        if (sep < minSepSegs * cfg::SEG_LEN) {
                            conflict = true;
                            break;
                        }
                    }
                }
                if (!conflict) break;
            }
            car.offset   = laneCenterOffset(lane);
            car.z        = candZ;
            t.prevZ[i]   = candZ;
            car.speed    = LANE_SPEED_FRAC[lane] * cfg::MAX_SPEED;
            car.spriteId = pickTrafficSprite(t.rng);
        }
    }
}

void VehiclePhysics::reset(Vehicle& v, const Track& track, Traffic& traffic) {
    int  savedCar    = v.carPreset;
    bool savedAssist = v.steerAssist;
    bool savedReal   = v.realisticMode;
    TrafficDensity savedDensity = traffic.density;

    v = Vehicle{};
    v.carPreset     = savedCar;
    v.steerAssist   = savedAssist;
    v.realisticMode = savedReal;
    v.hitboxScale   = savedReal ? 0.64f : 1.0f;
    v.gear          = 1;
    v.rpm           = 900.0f;
    v.nitro         = 1.0f;
    v.health        = 100.0f;
    v.wrecked       = false;
    initTraffic(traffic, track, 1u, savedDensity);
}

void VehiclePhysics::repairInPlace(Vehicle& v, Cockpit* cockpit) {
    v.health        = 100.0f;
    v.damage        = 0.0f;
    v.wrecked       = false;
    v.state         = CrashState::Driving;
    v.stateT        = 0.0f;
    v.fade          = 0.0f;
    v.yawPx         = 0.0f;
    v.tumblePx      = 0.0f;
    v.crackProgress = 0.0f;
    v.invulnT       = 2.5f;
    if (cockpit) cockpit->clearCracks();
}

void VehiclePhysics::tick(Vehicle& v, const InputState& in, const Track& track,
                          const SpriteAtlas& atlas, Traffic& traffic,
                          CameraFx* fx, Cockpit* cockpit) {
    if (track.count <= 0 || track.length <= 0.0f) return;

    const CarSpec& spec = getCarSpec(v.carPreset);
    const float maxCarSpeed = cfg::MAX_SPEED * (v.realisticMode ? spec.topSpeedMult : 1.0f);
    const float accelGain   = vphys::ACCEL_A * (v.realisticMode ? spec.accelMult    : 1.0f);
    const float steerGain   = vphys::STEER_ACCEL * (v.realisticMode ? spec.steerMult : 1.0f);

    // 1. Snapshot prevZ, prevX, traffic.prevZ[]
    const float prevZ = v.z;
    const float prevX = v.x;
    for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) {
        traffic.prevZ[i] = traffic.cars[i].z;
    }

    // If vehicle is wrecked (Health == 0), brake to a stop and wait for Repair [R] or Restart [Enter]
    if (v.wrecked) {
        v.speed = std::max(0.0f, v.speed - 9000.0f * cfg::STEP);
        v.latV  += (-vphys::GRIP_DAMP * v.latV) * cfg::STEP;
        v.z = mathutil::wrap(v.z + v.speed * cfg::STEP, track.length);
        v.x = mathutil::clamp(v.x + v.latV * cfg::STEP, -vphys::WALL_X, vphys::WALL_X);
        v.rpm = mathutil::lerp(v.rpm, 0.0f, 0.1f);
        v.audio.rpm = 0.0f;
        v.audio.throttle = 0.0f;
        v.audio.skid = 0.0f;
        updateTraffic(traffic, v, track);
        if (fx) fx->update(0.0f, 0.0f, 0.0f, 0.0f);
        if (cockpit && fx) {
            CockpitState cs;
            fillCockpit(v, cs);
            cockpit->update(cs, *fx);
        }
        return;
    }

    // 2. Read input; smooth steer
    bool controlsAllowed = (v.state == CrashState::Driving) ||
                           (v.state == CrashState::Respawn && v.stateT >= 0.3f);

    int segIdx = static_cast<int>(std::floor(mathutil::wrap(v.z, track.length) / cfg::SEG_LEN)) % track.count;
    const Segment& seg = track.segs[segIdx];
    float slope = (seg.y1 - seg.y0) / cfg::SEG_LEN;

    float targetSteer = 0.0f;
    if (controlsAllowed) {
        if (in.has(InputState::LEFT))  targetSteer -= 1.0f;
        if (in.has(InputState::RIGHT)) targetSteer += 1.0f;
    }

    // Optional ADAS Steer Assist (helps turn into curves & gently stabilizes lane centre when no manual steer is held)
    if (v.realisticMode && v.steerAssist && controlsAllowed && std::fabs(targetSteer) < 0.01f) {
        float fSpd = v.speed / cfg::MAX_SPEED;
        float curveAssist = mathutil::clamp(seg.curve * 0.24f * fSpd, -0.65f, +0.65f);
        float centerAssist = (std::fabs(v.x) > 0.75f) ? (-0.25f * v.x) : 0.0f;
        targetSteer = mathutil::clamp(curveAssist + centerAssist, -0.75f, +0.75f);
    }

    float steerRate = (std::fabs(targetSteer) > std::fabs(v.steer)) ? 4.0f : 6.0f;
    v.steer += mathutil::clamp(targetSteer - v.steer, -steerRate * cfg::STEP, +steerRate * cfg::STEP);
    v.steer = mathutil::clamp(v.steer, -1.0f, 1.0f);

    // 3. Slipstream drafting check behind nearby rival cars
    v.drafting = 0.0f;
    if (v.realisticMode && v.speed > 0.35f * cfg::MAX_SPEED) {
        for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) {
            if (!traffic.cars[i].active) continue;
            float dz = mathutil::wrapDist(traffic.cars[i].z, v.z, track.length);
            float dx = std::fabs(v.x - traffic.cars[i].offset);
            if (dz > 280.0f && dz < 2200.0f && dx < 0.28f) {
                float draftProx = 1.0f - ((dz - 280.0f) / 1920.0f);
                v.drafting = std::max(v.drafting, mathutil::clamp(draftProx, 0.0f, 1.0f));
            }
        }
    }

    // 4. State-specific longitudinal + lateral integration
    float f = v.speed / maxCarSpeed;
    bool throttle = controlsAllowed && in.has(InputState::ACCEL);
    bool brake    = controlsAllowed && in.has(InputState::BRAKE);
    bool offroad  = std::fabs(v.x) > 1.0f;

    v.nitroActive = false;
    float nitroMult = 1.0f + 0.12f * v.drafting;
    if (v.drafting > 0.2f) {
        v.nitro = std::min(1.0f, v.nitro + 0.14f * v.drafting * cfg::STEP);
    }
    if (controlsAllowed && throttle && in.has(InputState::NITRO) && v.nitro > 0.02f) {
        v.nitroActive = true;
        v.nitro = std::max(0.0f, v.nitro - 0.35f * cfg::STEP);
        nitroMult += 0.28f;
    } else {
        v.nitro = std::min(1.0f, v.nitro + 0.08f * cfg::STEP);
    }

    float a_s = 0.0f;
    float a_c = 0.0f;

    if (v.state == CrashState::Driving || v.state == CrashState::Respawn) {
        float dv = 0.0f;
        if (throttle) {
            dv += accelGain * nitroMult * (1.0f - f);
        } else if (brake) {
            dv -= vphys::BRAKE_A;
        } else {
            dv -= vphys::COAST_A;
        }
        dv -= vphys::HILL_G * slope;
        if (offroad) {
            dv -= vphys::OFFROAD_A;
        }

        float prevSpeed = v.speed;
        v.speed += dv * cfg::STEP;
        float topCap = (v.nitroActive || v.drafting > 0.4f) ? (1.08f * maxCarSpeed) : maxCarSpeed;
        v.speed = mathutil::clamp(v.speed, 0.0f, 1.08f * maxCarSpeed);
        if (!v.nitroActive && v.drafting <= 0.4f && v.speed > topCap) v.speed = topCap;
        if (offroad && throttle && prevSpeed >= vphys::OFFROAD_LIMIT) {
            v.speed = std::max(v.speed, vphys::OFFROAD_LIMIT);
        }
        v.accelLong = (v.speed - prevSpeed) / cfg::STEP;

        float fBase = v.speed / cfg::MAX_SPEED;
        // When Steer Assist is OFF in realistic mode, centrifugal push is stronger so corners require active steering
        float centGain = vphys::CENT_K;
        if (v.realisticMode) {
            centGain = v.steerAssist ? (0.45f * vphys::CENT_K) : (1.22f * vphys::CENT_K);
        }
        a_c = -centGain * seg.curve * (fBase * fBase);
        a_s = steerGain * v.steer * mathutil::clamp(5.0f * fBase, 0.0f, 1.0f) * (1.0f - 0.25f * fBase);
        float dlv = a_s + a_c - vphys::GRIP_DAMP * v.latV;
        v.latV += dlv * cfg::STEP;
    } else {
        float prevSpeed = v.speed;
        v.speed = std::max(0.0f, v.speed - 6000.0f * cfg::STEP);
        v.accelLong = (v.speed - prevSpeed) / cfg::STEP;
        v.latV += (-vphys::GRIP_DAMP * v.latV) * cfg::STEP;
    }

    // 5. Advance z and x
    float unclampedZ = v.z + v.speed * cfg::STEP;
    if (unclampedZ >= track.length) {
        v.lap++;
        if (v.lapTime > 5.0f && (v.bestLapTime <= 0.0f || v.lapTime < v.bestLapTime)) {
            v.bestLapTime = v.lapTime;
        }
        v.lapTime = 0.0f;
    } else {
        v.lapTime += cfg::STEP;
    }
    v.z = mathutil::wrap(unclampedZ, track.length);
    v.x = mathutil::clamp(v.x + v.latV * cfg::STEP, -vphys::WALL_X, vphys::WALL_X);

    // Count overtakes & high-speed near misses against active traffic cars
    for (int i = 0; i < cfg::MAX_TRAFFIC; ++i) {
        if (!traffic.cars[i].active) continue;
        float dBefore = mathutil::wrapDist(traffic.prevZ[i], prevZ, track.length);
        float dAfter  = mathutil::wrapDist(traffic.cars[i].z, v.z, track.length);
        if (dBefore > 0.0f && dBefore < 600.0f && dAfter <= 0.0f && dAfter > -600.0f) {
            v.overtakes++;
            float latDist = std::fabs(v.x - traffic.cars[i].offset);
            if (latDist < 0.46f && v.speed > 0.55f * cfg::MAX_SPEED) {
                v.nearMisses++;
            }
        }
    }

    // 6. Update traffic
    updateTraffic(traffic, v, track);

    // 7. Collision detection & response
    if (v.state == CrashState::Driving && v.invulnT <= 0.0f) {
        CollisionEvent ce = detectCollision(v, prevZ, prevX, track, atlas, traffic);
        if (ce.kind != CollisionEvent::None) {
            if (v.realisticMode) {
                // Modern realistic physical bump/rebound + Car Health damage
                float sev = ce.severity;
                float armor = std::max(0.5f, spec.armorMult);
                float hpLoss = ((ce.kind == CollisionEvent::Wall) ? 6.0f : (8.0f + 26.0f * sev)) / armor;
                v.health = std::max(0.0f, v.health - hpLoss);
                v.damage = mathutil::clamp(1.0f - (v.health / 100.0f), 0.0f, 1.0f);

                // Push player laterally out of overlap and transfer momentum
                float pushDir = (ce.dir != 0.0f) ? ce.dir : +1.0f;
                v.latV  = pushDir * (1.4f + 1.6f * sev);
                v.x     = mathutil::clamp(v.x + pushDir * 0.08f, -vphys::WALL_X + 0.05f, vphys::WALL_X - 0.05f);
                v.speed *= (0.82f - 0.22f * sev);

                // Also nudge the traffic car laterally if it was a car-to-car bump
                if (ce.kind == CollisionEvent::Traffic && ce.carIndex >= 0 && ce.carIndex < cfg::MAX_TRAFFIC) {
                    Car& hitCar = traffic.cars[ce.carIndex];
                    hitCar.offset = mathutil::clamp(hitCar.offset - pushDir * 0.12f, -0.92f, +0.92f);
                    hitCar.z = mathutil::wrap(hitCar.z + 140.0f, track.length);
                }

                v.invulnT = 0.45f; // brief bounce separation immunity
                v.flash   = std::min(1.0f, 0.35f + 0.65f * sev);
                if (fx) fx->impact(4.0f + 6.0f * sev);

                if (sev >= 0.25f || v.health < 60.0f) {
                    v.audio.crashCount++;
                    v.crackProgress = mathutil::clamp(v.damage, 0.2f, 1.0f);
                    uint32_t seed = ++v.crashSeq;
                    if (cockpit) {
                        uint32_t rState = seed;
                        int ix = 160 + static_cast<int>(std::lround(60.0f * pushDir * mathutil::rand01(rState)));
                        int iy = 70  + static_cast<int>(std::lround(40.0f * mathutil::rand01(rState)));
                        cockpit->triggerCrack(ix, iy, seed);
                    }
                } else {
                    v.audio.scrapeCount++;
                }

                // Check if Car Health reached 0 -> Totaled / Wrecked!
                if (v.health <= 0.0f) {
                    v.health  = 0.0f;
                    v.wrecked = true;
                    v.audio.crashCount++;
                    if (fx) fx->impact(12.0f);
                }
            } else {
                // Original Phase 6 state machine for blueprint unit tests
                if (ce.severity < 0.12f) {
                    v.speed *= 0.85f;
                    v.latV  += ce.dir * 1.0f;
                    v.audio.scrapeCount++;
                    if (fx) fx->impact(3.0f);
                } else {
                    float sev = ce.severity;
                    v.speed *= (0.45f - 0.35f * sev);
                    v.latV  += ce.dir * (1.5f + 2.0f * sev);
                    v.damage = std::min(1.0f, v.damage + 0.25f + 0.5f * sev);
                    v.health = std::max(0.0f, (1.0f - v.damage) * 100.0f);
                    v.spinDir = (ce.dir != 0.0f) ? ce.dir : +1.0f;
                    v.audio.crashCount++;
                    v.state  = CrashState::Impact;
                    v.stateT = 0.0f;
                    v.flash  = 1.0f;
                    v.crackProgress = 1.0f / 12.0f;
                    uint32_t seed = ++v.crashSeq;
                    if (fx) fx->impact(6.0f + 6.0f * sev);
                    if (cockpit) {
                        uint32_t rState = seed;
                        int ix = 160 + static_cast<int>(std::lround(60.0f * v.spinDir * mathutil::rand01(rState)));
                        int iy = 70  + static_cast<int>(std::lround(40.0f * mathutil::rand01(rState)));
                        cockpit->triggerCrack(ix, iy, seed);
                    }
                }
            }
        }
    }

    // 8. Crash state machine transition & timers
    v.invulnT = std::max(0.0f, v.invulnT - cfg::STEP);
    v.flash   *= 0.85f;
    if (v.flash < 0.01f) v.flash = 0.0f;

    if (v.state != CrashState::Driving) {
        v.stateT += cfg::STEP;
        if (v.crackProgress > 0.0f && v.crackProgress < 1.0f) {
            v.crackProgress = std::min(1.0f, v.crackProgress + (1.0f / 12.0f));
        }
    }

    switch (v.state) {
        case CrashState::Driving:
            v.fade = 0.0f;
            v.yawPx = 0.0f;
            v.tumblePx = 0.0f;
            break;

        case CrashState::Impact:
            if (v.stateT >= 0.25f) {
                v.state  = CrashState::Spin;
                v.stateT = 0.0f;
            }
            break;

        case CrashState::Spin: {
            float t = v.stateT;
            v.yawPx    = v.spinDir * 80.0f * std::exp(-t / 0.7f) * std::sin(mathutil::TWO_PI * 1.1f * t);
            v.tumblePx = 14.0f * std::exp(-t / 0.6f) * std::sin(mathutil::TWO_PI * 1.7f * t);
            if ((v.stateT >= 1.4f && v.speed < 400.0f) || v.stateT >= 1.6f) {
                v.state  = CrashState::FadeOut;
                v.stateT = 0.0f;
            }
            break;
        }

        case CrashState::FadeOut: {
            float t = v.stateT;
            v.fade = mathutil::clamp(t / 0.4f, 0.0f, 1.0f);
            v.yawPx    *= 0.90f;
            v.tumblePx *= 0.90f;
            if (v.fade >= 1.0f || v.stateT >= 0.4f) {
                v.fade = 1.0f;
                placeRespawn(v, track, traffic);
                if (cockpit) cockpit->clearCracks();
                v.state   = CrashState::Respawn;
                v.stateT  = 0.0f;
                v.invulnT = 2.0f;
            }
            break;
        }

        case CrashState::Respawn: {
            v.fade = mathutil::clamp(1.0f - (v.stateT / 0.6f), 0.0f, 1.0f);
            if (v.stateT >= 0.6f) {
                v.fade   = 0.0f;
                v.state  = CrashState::Driving;
                v.stateT = 0.0f;
            }
            break;
        }
    }

    // 9. Gearbox, RPM, camera slope pitch, and audio/cockpit outputs
    float fGear = mathutil::clamp(v.speed / maxCarSpeed, 0.0f, 1.05f);
    v.shiftCooldown = std::max(0.0f, v.shiftCooldown - cfg::STEP);
    if (v.shiftCooldown <= 0.0f) {
        if (v.gear < 5 && fGear > GEAR_HI[v.gear - 1]) {
            v.gear++;
            v.shiftCooldown = 0.30f;
            v.audio.shiftCount++;
        } else if (v.gear > 1 && fGear < GEAR_LO[v.gear - 1]) {
            v.gear--;
            v.shiftCooldown = 0.30f;
            v.audio.shiftCount++;
        }
    }

    float fLo = GEAR_LO[v.gear - 1];
    float fHi = GEAR_HI[v.gear - 1];
    float gf  = mathutil::clamp((fGear - fLo) / std::max(0.01f, fHi - fLo), 0.0f, 1.0f);
    float rStart  = (v.gear == 1) ? 0.0f : 0.5f;
    float rpmFrac = mathutil::lerp(rStart, 1.0f, gf);

    if (v.state == CrashState::FadeOut) {
        v.rpm = mathutil::lerp(v.rpm, 900.0f, 0.15f);
    } else {
        v.rpm = 900.0f + 7600.0f * rpmFrac * (0.9f + 0.1f * (throttle ? 1.0f : 0.0f));
    }
    v.rpm = mathutil::clamp(v.rpm, 900.0f, 8500.0f);

    int curSegIdx = static_cast<int>(std::floor(mathutil::wrap(v.z, track.length) / cfg::SEG_LEN)) % track.count;
    const Segment& curSeg = track.segs[curSegIdx];
    float curSlope = (curSeg.y1 - curSeg.y0) / cfg::SEG_LEN;
    float pitchTarget = mathutil::clamp(cfg::FOCAL_PX * curSlope * 0.35f, -24.0f, +24.0f);
    const float kSlopePitch = 1.0f - std::exp(-cfg::STEP / 0.15f);
    v.slopePitch += (pitchTarget - v.slopePitch) * kSlopePitch;

    float driftSkid = mathutil::smoothstep(2.2f, 3.4f, std::fabs(a_s) + std::fabs(a_c));
    float brakeSkid = (brake && fGear > 0.35f) ? (0.8f * fGear) : 0.0f;
    float skid = (v.state == CrashState::Spin) ? 0.85f : std::max(driftSkid, brakeSkid);

    v.audio.rpm       = v.rpm;
    v.audio.throttle  = throttle ? 1.0f : 0.0f;
    v.audio.skid      = mathutil::clamp(skid, 0.0f, 1.0f);
    v.audio.offroad   = (std::fabs(v.x) > 1.0f) ? 1.0f : 0.0f;
    v.audio.speedFrac = mathutil::clamp(fGear, 0.0f, 1.0f);

    if (fx) {
        float rpmNorm   = mathutil::clamp((v.rpm - 900.0f) / 7600.0f, 0.0f, 1.0f);
        float accelNorm = mathutil::clamp(v.accelLong / vphys::ACCEL_A, -1.0f, 1.0f);
        fx->update(v.audio.speedFrac, rpmNorm, accelNorm, v.audio.offroad);
    }
    if (cockpit && fx) {
        CockpitState cs;
        fillCockpit(v, cs);
        cockpit->update(cs, *fx);
    }
}

void VehiclePhysics::fillCockpit(const Vehicle& v, CockpitState& cs) {
    cs.speedKmh = v.speed * cfg::KMH_PER_WU;
    cs.rpm      = v.rpm;
    cs.steer    = v.steer;
    cs.crack    = v.crackProgress;
    cs.flash    = v.flash;
    cs.damage   = v.damage;
    cs.gear     = v.gear;
}

void VehiclePhysics::fillCamera(const Vehicle& v, const Track& track, const CameraFx& fx, Camera& cam) {
    cam.x = v.x * cfg::ROAD_HALF_W;
    cam.z = mathutil::wrap(v.z, track.length);

    float zNorm = cam.z / cfg::SEG_LEN;
    int segIdx  = (track.count > 0) ? (static_cast<int>(std::floor(zNorm)) % track.count) : 0;
    float pct   = zNorm - std::floor(zNorm);
    const Segment& s = track.segs[segIdx];
    float segY  = s.y0 + pct * (s.y1 - s.y0);

    cam.y       = cfg::CAM_HEIGHT + segY;
    cam.pitchPx = v.slopePitch + fx.shakeY + v.tumblePx;
    cam.shiftX  = fx.shakeX + v.yawPx;
}
