#pragma once
#include <cstdint>
#include <atomic>
#include <thread>
#include "config.h"
#include "roadrender.h"
#include "sprites.h"
#include "track.h"
#include "cockpit.h"
#include "framebuffer.h"

struct FrameJob {                                  // everything a slice needs, all read-only except fb
    const RenderContext* ctx     = nullptr;
    const DrawList*      dl      = nullptr;
    const SpriteAtlas*   atlas   = nullptr;
    const Track*         track   = nullptr;
    Cockpit*             cockpit = nullptr;
    Framebuffer*         fb      = nullptr;
};

struct alignas(64) SliceStat {
    uint64_t ns = 0;
};

class SliceScheduler {
public:
    SliceScheduler() = default;
    ~SliceScheduler();

    void init(int workerCount = -1);               // clamp(hardware_concurrency - 1, 1, NUM_SLICES - 1)
    void shutdown();                               // stops + joins workers in under 100 ms
    void run(const FrameJob& job);                 // fork-join; main thread participates
    int  workerCount() const { return workerCount_; }

    SliceStat stats[cfg::NUM_SLICES];

private:
    static constexpr int ROWS_PER_SLICE = cfg::WORLD_ROWS / cfg::NUM_SLICES;
    FrameJob jobs_[2];                             // double buffered by generation parity
    alignas(64) std::atomic<uint64_t> ticket_{0};  // high 32 bits: generation, low 32: next slice
    alignas(64) std::atomic<int>      done_{0};
    alignas(64) std::atomic<uint32_t> wake_{0};    // workers sleep on this
    alignas(64) std::atomic<bool>     stop_{false};
    uint32_t    gen_ = 0;
    int         workerCount_ = 0;
    bool        running_ = false;
    std::thread workers_[cfg::NUM_SLICES - 1];

    void workerLoop();
    void drain();
    void renderSlice(const FrameJob& job, int s);
};
