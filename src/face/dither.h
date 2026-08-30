#pragma once
// Ordered (Bayer) dithering, shared by the outer-rim fade and the face-layer
// depth reduction.
//
// ORDERED, not error-diffused, and that choice is deliberate: these masks are
// applied to moving content, and a diffused pattern re-solves every frame, so
// the noise swims and shimmers. An ordered matrix is pinned to the pixel grid,
// so the stipple sits still and reads as texture rather than as sparkle.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace face {

// 8x8 Bayer threshold matrix, values 0..63.
inline int bayer8_raw(int x, int y) {
    static const int kB[8][8] = {
        {  0, 32,  8, 40,  2, 34, 10, 42 },
        { 48, 16, 56, 24, 50, 18, 58, 26 },
        { 12, 44,  4, 36, 14, 46,  6, 38 },
        { 60, 28, 52, 20, 62, 30, 54, 22 },
        {  3, 35, 11, 43,  1, 33,  9, 41 },
        { 51, 19, 59, 27, 49, 17, 57, 25 },
        { 15, 47,  7, 39, 13, 45,  5, 37 },
        { 63, 31, 55, 23, 61, 29, 53, 21 },
    };
    return kB[y & 7][x & 7];
}

// Signed offset in (-0.5, +0.5], for spreading a rounding error.
inline float bayer8_offset(int x, int y) {
    return bayer8_raw(x, y) / 64.f - 0.5f;
}

// Quantise one 0..255 channel to `levels` evenly spaced steps, spreading the
// rounding error with the ordered offset at (x, y). levels <= 1 or >= 256 is a
// no-op — there is nothing to gain by "reducing" to the depth you already have.
inline uint8_t dither_channel(uint8_t v, int levels, int x, int y) {
    if (levels <= 1 || levels >= 256) return v;
    const float lm1 = static_cast<float>(levels - 1);
    const float t   = v / 255.f * lm1;          // position in level space
    int q = static_cast<int>(std::floor(t + bayer8_offset(x, y) + 0.5f));
    q = std::clamp(q, 0, levels - 1);
    return static_cast<uint8_t>(q / lm1 * 255.f + 0.5f);
}

}  // namespace face
