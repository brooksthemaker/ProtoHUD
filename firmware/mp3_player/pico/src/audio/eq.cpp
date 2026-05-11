#include "eq.h"

static constexpr float kPi = 3.14159265358979323846f;

// Centre frequencies (Hz) for the five bands.
const float Eq::BAND_FREQS[Eq::NUM_BANDS] = { 60.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f };

// Q factor per band (1.0 gives a moderate shelf width).
const float Eq::BAND_Q[Eq::NUM_BANDS] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

// Preset gain tables [dB], rows indexed by EqPreset.
// Columns: 60 Hz, 250 Hz, 1 kHz, 4 kHz, 12 kHz
const float Eq::PRESET_GAINS[static_cast<int>(EqPreset::COUNT)][Eq::NUM_BANDS] = {
    {  0.0f,  0.0f,  0.0f,  0.0f,  0.0f },  // FLAT
    { +8.0f, +5.0f, -2.0f, -1.0f,  0.0f },  // BASS_BOOST
    { -2.0f, -1.0f, +5.0f, +4.0f, +1.0f },  // VOCAL
    {  0.0f, -1.0f, -1.0f, +4.0f, +8.0f },  // TREBLE
    {  0.0f,  0.0f,  0.0f,  0.0f,  0.0f },  // CUSTOM (placeholder)
};

void Eq::set_sample_rate(uint32_t hz) {
    if (hz == 0) return;
    sample_rate_ = hz;
    for (int b = 0; b < NUM_BANDS; ++b) recompute(b);
}

void Eq::apply_preset(EqPreset preset) {
    const int idx = static_cast<int>(preset);
    for (int b = 0; b < NUM_BANDS; ++b) {
        gains_[b] = PRESET_GAINS[idx][b];
        recompute(b);
    }
}

void Eq::recompute(int b) {
    const float dB = gains_[b];

    // Near-zero gain → identity filter (no history corruption).
    if (dB > -0.05f && dB < 0.05f) {
        for (int ch = 0; ch < MAX_CH; ++ch) {
            auto& f = filters_[ch][b];
            f.b0 = 1.0f; f.b1 = 0.0f; f.b2 = 0.0f;
            f.a1 = 0.0f; f.a2 = 0.0f;
        }
        return;
    }

    // Peaking EQ coefficients from Audio EQ Cookbook (Bristow-Johnson).
    const float A     = std::pow(10.0f, dB / 40.0f);
    const float w0    = 2.0f * kPi * BAND_FREQS[b] / static_cast<float>(sample_rate_);
    const float cosw  = std::cos(w0);
    const float alpha = std::sin(w0) / (2.0f * BAND_Q[b]);
    const float a0inv = 1.0f / (1.0f + alpha / A);

    const float b0 = (1.0f + alpha * A) * a0inv;
    const float b1 = (-2.0f * cosw)     * a0inv;
    const float b2 = (1.0f - alpha * A) * a0inv;
    const float a1 = (-2.0f * cosw)     * a0inv;  // same as b1
    const float a2 = (1.0f - alpha / A) * a0inv;

    for (int ch = 0; ch < MAX_CH; ++ch) {
        auto& f    = filters_[ch][b];
        f.b0 = b0; f.b1 = b1; f.b2 = b2;
        f.a1 = a1; f.a2 = a2;
        // Intentionally preserve state (x1,x2,y1,y2) to avoid clicks on change.
    }
}

void Eq::process(int16_t* pcm, size_t sample_pairs, int channels) {
    if (!enabled || channels < 1) return;
    const int ch_count = channels < MAX_CH ? channels : MAX_CH;

    for (size_t i = 0; i < sample_pairs; ++i) {
        for (int ch = 0; ch < ch_count; ++ch) {
            float s = static_cast<float>(pcm[i * channels + ch]);
            for (int b = 0; b < NUM_BANDS; ++b)
                s = filters_[ch][b].process(s);
            // Hard-clip to int16 range.
            if      (s >  32767.0f) s =  32767.0f;
            else if (s < -32768.0f) s = -32768.0f;
            pcm[i * channels + ch] = static_cast<int16_t>(s);
        }
    }
}
