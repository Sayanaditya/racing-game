#pragma once
#include <cstdint>

struct InputState {
    enum Bit : uint8_t {
        ACCEL = 1 << 0,
        BRAKE = 1 << 1,
        LEFT  = 1 << 2,
        RIGHT = 1 << 3,
        RESET = 1 << 4,
        PAUSE = 1 << 5,
        NITRO = 1 << 6,
        WIPER = 1 << 7
    };
    uint8_t down = 0;        // currently held
    uint8_t pressed = 0;     // went down since last tick (cleared by widget after each tick)
    bool has(Bit b) const { return (down & b) != 0; }
    bool justPressed(Bit b) const { return (pressed & b) != 0; }
};
