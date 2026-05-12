#pragma once
#include <cstdint>
#include <cmath>

enum class EqPreset : uint8_t {
    FLAT      = 0,
    BASS_BOOST,
    VOCAL,
    TREBLE,
    CUSTOM,      // user-tuned (not settable via UI in this release)
    COUNT
};

// Single biquad filter, Direct Form I.
struct EqBiquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;  // numerator (normalised)
    float a1 = 0.0f, a2 = 0.0f;              // denominator (normalised, sign-positive)
    float x1 = 0.0f, x2 = 0.0f;
    float y1 = 0.0f, y2 = 0.0f;

    inline float process(float x) {
        const float y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return y;
    }
};

// 5-band peaking-EQ (Audio EQ Cookbook, Bristow-Johnson).
// Lives on Core 1 inside SdPlayer; not thread-safe — call set_eq_preset()
// from Core 0 via SdPlayer::set_eq_preset(), which writes an atomic flag
// that Core 1 picks up between frames.
class Eq {
public:
    static constexpr int NUM_BANDS = 5;
    static constexpr int MAX_CH    = 2;  // stereo

    // Centre frequencies, Q factors, preset gains [dB].
    static const float BAND_FREQS[NUM_BANDS];  // Hz: 60,250,1k,4k,12k
    static const float BAND_Q[NUM_BANDS];
    static const float PRESET_GAINS[static_cast<int>(EqPreset::COUNT)][NUM_BANDS];

    bool enabled = true;

    void set_sample_rate(uint32_t hz);
    void apply_preset(EqPreset preset);

    // Process interleaved stereo (or mono) s16le in-place.
    // sample_pairs = number of audio frames; channels = 1 or 2.
    void process(int16_t* pcm, size_t sample_pairs, int channels);

private:
    void recompute(int band);

    EqBiquad filters_[MAX_CH][NUM_BANDS] = {};
    float    gains_[NUM_BANDS]           = {};
    uint32_t sample_rate_                = 44100;
};
