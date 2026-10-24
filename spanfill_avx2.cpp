#include "spanfill.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2")))
#endif
void fillPixelsAVX2(uint32_t* d, int n, uint32_t c) {
    if (n < 16) {
        for (int i = 0; i < n; ++i) d[i] = c;
        return;
    }
    const __m256i v = _mm256_set1_epi32(static_cast<int>(c));
    uint32_t mis = (reinterpret_cast<uintptr_t>(d) & 31u) >> 2;
    if (mis) {
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d), v);
        int adv = static_cast<int>(8u - mis);
        d += adv;
        n -= adv;
    }
    while (n >= 32) {
        _mm256_store_si256(reinterpret_cast<__m256i*>(d),      v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(d + 8),  v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(d + 16), v);
        _mm256_store_si256(reinterpret_cast<__m256i*>(d + 24), v);
        d += 32;
        n -= 32;
    }
    while (n >= 8) {
        _mm256_store_si256(reinterpret_cast<__m256i*>(d), v);
        d += 8;
        n -= 8;
    }
    if (n > 0) {
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + n - 8), v);
    }
}
#else
void fillPixelsAVX2(uint32_t* d, int n, uint32_t c) {
    fillPixelsNEON(d, n, c);
}
#endif
