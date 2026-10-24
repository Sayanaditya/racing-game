#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace mathutil {

inline constexpr float PI = 3.14159265358979323846f;
inline constexpr float TWO_PI = 6.28318530717958647692f;

template <typename T>
constexpr T clamp(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline constexpr float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

inline constexpr float easeIn(float a, float b, float p) {
    return a + (b - a) * (p * p);
}

inline constexpr float easeOut(float a, float b, float p) {
    float inv = 1.0f - p;
    return a + (b - a) * (1.0f - inv * inv);
}

inline float easeInOut(float a, float b, float p) {
    return a + (b - a) * (0.5f - 0.5f * std::cos(PI * p));
}

inline constexpr float smoothstep(float edge0, float edge1, float x) {
    float t = clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline float frac(float x) {
    return x - std::floor(x);
}

inline float wrap(float z, float length) {
    if (length <= 0.0f) return 0.0f;
    float r = std::fmod(z, length);
    if (r < 0.0f) r += length;
    return r;
}

inline float wrapDist(float a, float b, float length) {
    float half = 0.5f * length;
    float d = std::fmod(a - b + half, length);
    if (d < 0.0f) d += length;
    return d - half;
}

inline uint32_t xorshift32(uint32_t& state) {
    if (state == 0) state = 0x12345678u;
    uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}

inline float rand01(uint32_t& state) {
    return static_cast<float>(xorshift32(state) & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

inline int randRange(uint32_t& state, int count) {
    if (count <= 0) return 0;
    return static_cast<int>(xorshift32(state) % static_cast<uint32_t>(count));
}

inline uint32_t lerpColor(uint32_t c0, uint32_t c1, float t) {
    t = clamp(t, 0.0f, 1.0f);
    uint32_t r0 = (c0 >> 16) & 0xFFu, g0 = (c0 >> 8) & 0xFFu, b0 = c0 & 0xFFu;
    uint32_t r1 = (c1 >> 16) & 0xFFu, g1 = (c1 >> 8) & 0xFFu, b1 = c1 & 0xFFu;
    uint32_t r = static_cast<uint32_t>(r0 + (static_cast<float>(r1) - static_cast<float>(r0)) * t + 0.5f);
    uint32_t g = static_cast<uint32_t>(g0 + (static_cast<float>(g1) - static_cast<float>(g0)) * t + 0.5f);
    uint32_t b = static_cast<uint32_t>(b0 + (static_cast<float>(b1) - static_cast<float>(b0)) * t + 0.5f);
    return 0xFF000000u | (clamp(r, 0u, 255u) << 16) | (clamp(g, 0u, 255u) << 8) | clamp(b, 0u, 255u);
}

// FNV-1a 64-bit hash for deterministic framebuffer and state verification
inline uint64_t fnv1a64(const void* data, size_t bytes, uint64_t seed = 14695981039346656037ULL) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

} // namespace mathutil
