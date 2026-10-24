#include "jobs.h"
#include <chrono>
#include <algorithm>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
inline void cpuPause() { _mm_pause(); }
#elif defined(__aarch64__) || defined(_M_ARM64)
inline void cpuPause() { asm volatile("yield" ::: "memory"); }
#else
inline void cpuPause() { std::this_thread::yield(); }
#endif

SliceScheduler::~SliceScheduler() {
    shutdown();
}

void SliceScheduler::init(int requestedWorkers) {
    shutdown();
    stop_.store(false, std::memory_order_relaxed);
    gen_ = 0;
    ticket_.store(0, std::memory_order_relaxed);
    done_.store(0, std::memory_order_relaxed);
    wake_.store(0, std::memory_order_relaxed);

    if (requestedWorkers < 0) {
        int hw = static_cast<int>(std::thread::hardware_concurrency());
        requestedWorkers = std::clamp(hw - 1, 1, cfg::NUM_SLICES - 1);
    } else {
        requestedWorkers = std::clamp(requestedWorkers, 0, cfg::NUM_SLICES - 1);
    }

    workerCount_ = requestedWorkers;
    running_ = true;

    for (int i = 0; i < workerCount_; ++i) {
        workers_[i] = std::thread(&SliceScheduler::workerLoop, this);
    }
}

void SliceScheduler::shutdown() {
    if (!running_) return;
    stop_.store(true, std::memory_order_release);
    gen_ += 1;
    wake_.store(gen_, std::memory_order_release);
    wake_.notify_all();

    for (int i = 0; i < workerCount_; ++i) {
        if (workers_[i].joinable()) {
            workers_[i].join();
        }
    }
    workerCount_ = 0;
    running_ = false;
}

void SliceScheduler::run(const FrameJob& job) {
    if (!running_ || workerCount_ == 0) {
        gen_ += 1;
        jobs_[gen_ & 1] = job;
        ticket_.store((static_cast<uint64_t>(gen_) << 32) | 0u, std::memory_order_release);
        done_.store(0, std::memory_order_relaxed);
        drain();
        return;
    }

    gen_ += 1;
    jobs_[gen_ & 1] = job;
    done_.store(0, std::memory_order_relaxed);
    ticket_.store((static_cast<uint64_t>(gen_) << 32) | 0u, std::memory_order_release);
    wake_.store(gen_, std::memory_order_release);
    wake_.notify_all();

    // Main thread participates in draining slices
    drain();

    while (done_.load(std::memory_order_acquire) < cfg::NUM_SLICES) {
        for (int spin = 0; spin < 2000; ++spin) {
            if (done_.load(std::memory_order_acquire) >= cfg::NUM_SLICES) return;
            cpuPause();
        }
        int cur = done_.load(std::memory_order_acquire);
        if (cur < cfg::NUM_SLICES) {
            done_.wait(cur, std::memory_order_acquire);
        }
    }
}

void SliceScheduler::drain() {
    while (true) {
        uint64_t t = ticket_.fetch_add(1, std::memory_order_acq_rel);
        uint32_t g = static_cast<uint32_t>(t >> 32);
        uint32_t s = static_cast<uint32_t>(t & 0xFFFFFFFFu);
        if (s >= static_cast<uint32_t>(cfg::NUM_SLICES)) {
            return;
        }
        renderSlice(jobs_[g & 1], static_cast<int>(s));
        if (done_.fetch_add(1, std::memory_order_acq_rel) + 1 == cfg::NUM_SLICES) {
            done_.notify_all();
        }
    }
}

void SliceScheduler::workerLoop() {
    uint32_t seen = 0;
    while (!stop_.load(std::memory_order_acquire)) {
        for (int spin = 0; spin < 2000; ++spin) {
            if (wake_.load(std::memory_order_acquire) != seen || stop_.load(std::memory_order_acquire)) {
                break;
            }
            cpuPause();
        }
        uint32_t cur = wake_.load(std::memory_order_acquire);
        if (cur == seen && !stop_.load(std::memory_order_acquire)) {
            wake_.wait(seen, std::memory_order_acquire);
        }
        if (stop_.load(std::memory_order_acquire)) break;
        seen = wake_.load(std::memory_order_acquire);
        drain();
    }
}

void SliceScheduler::renderSlice(const FrameJob& job, int s) {
    const int y0 = s * ROWS_PER_SLICE;
    const int y1 = y0 + ROWS_PER_SLICE;
    auto t0 = std::chrono::steady_clock::now();

    fillRows(*job.ctx, *job.fb, y0, y1);
    drawSprites(*job.dl, *job.atlas, *job.ctx, *job.fb, y0, y1);
    if (job.cockpit) {
        job.cockpit->tintRows(*job.fb, y0, y1);
    }

    auto t1 = std::chrono::steady_clock::now();
    stats[s].ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}
