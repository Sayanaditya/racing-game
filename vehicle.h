#pragma once
#include <cstdint>
#include "config.h"
#include "track.h"
#include "sprites.h"
#include "cockpit.h"
#include "camera.h"
#include "input.h"

namespace vphys {
inline constexpr float ACCEL_A       = 4800.0f;  // wu/s^2; tau = MAX_SPEED / ACCEL_A = 2.5 s
inline constexpr float BRAKE_A       = 12000.0f; // wu/s^2; stops from top speed in 1.0 s
inline constexpr float COAST_A       = 1800.0f;  // wu/s^2; engine braking with no input
inline constexpr float OFFROAD_A     = 6000.0f;  // wu/s^2; extra decel while abs(x) > 1
inline constexpr float OFFROAD_LIMIT = 3000.0f;  // wu/s; speed floor while off-road with throttle held
inline constexpr float STEER_ACCEL   = 4.0f;     // x-units/s^2 at full lock
inline constexpr float GRIP_DAMP     = 3.0f;     // 1/s lateral velocity damping
inline constexpr float CENT_K        = 0.9f;     // centrifugal gain
inline constexpr float HILL_G        = 6000.0f;  // wu/s^2 slope drag factor
inline constexpr float PLAYER_HALF_W = 0.22f;    // normalised (260 wu)
inline constexpr float PLAYER_LEN    = 400.0f;   // wu hood length ahead of camera
inline constexpr float WALL_X        = 2.6f;     // invisible barrier
} // namespace vphys

enum class CrashState : uint8_t {
    Driving = 0,
    Impact,
    Spin,
    FadeOut,
    Respawn
};

struct AudioParams {
    float    rpm        = 900.0f;
    float    throttle   = 0.0f;
    float    skid       = 0.0f;
    float    offroad    = 0.0f;
    float    speedFrac  = 0.0f;
    uint32_t crashCount  = 0;
    uint32_t scrapeCount = 0;
    uint32_t shiftCount  = 0;
};

struct Vehicle {
    // kinematics
    float z = 0.0f, x = 0.0f;             // z wrapped to [0,length); x normalised, 1 = road edge
    float speed = 0.0f, latV = 0.0f;      // wu/s ; normalised lateral velocity (x units / s)
    float steer = 0.0f;                   // smoothed steering, [-1, 1]
    float accelLong = 0.0f;               // wu/s^2, for camera pitch / audio
    float slopePitch = 0.0f;              // smoothed camera slope pitch (px)
    // drivetrain
    int   gear = 1;
    float rpm = 900.0f;
    float shiftCooldown = 0.0f;
    // crash machine
    CrashState state = CrashState::Driving;
    float stateT  = 0.0f;                 // seconds in current state
    float invulnT = 0.0f;                 // seconds of collision immunity left
    float spinDir = 1.0f;                 // +-1 sway direction
    float yawPx   = 0.0f;
    float tumblePx = 0.0f;                // view sway outputs (px)
    float fade    = 0.0f;                 // 0 = clear, 1 = black
    float damage  = 0.0f;
    float flash   = 0.0f;
    float crackProgress = 0.0f;
    uint32_t crashSeq = 0;                // seeds the glass-crack generator
    // gameplay / lap tracking
    int   lap = 1;
    float lapTime = 0.0f;
    float bestLapTime = 0.0f;
    int   overtakes = 0;
    float nitro = 1.0f;                   // 0..1 turbo boost reserve
    bool  nitroActive = false;
    AudioParams audio;
};

struct Traffic {
    Car      cars[cfg::MAX_TRAFFIC];
    float    prevZ[cfg::MAX_TRAFFIC] = {};
    uint32_t rng = 1;
};

class VehiclePhysics {
public:
    static void reset(Vehicle& v, const Track& track, Traffic& traffic);
    static void tick(Vehicle& v, const InputState& in, const Track& track,
                     const SpriteAtlas& atlas, Traffic& traffic,
                     CameraFx* fx = nullptr, Cockpit* cockpit = nullptr);
    static void fillCockpit(const Vehicle& v, CockpitState& cs);
    static void fillCamera(const Vehicle& v, const Track& track, const CameraFx& fx, Camera& cam);
};

void initTraffic(Traffic& t, const Track& track, uint32_t seed = 1);
void updateTraffic(Traffic& t, const Vehicle& player, const Track& track);
