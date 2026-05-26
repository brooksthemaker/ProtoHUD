#pragma once
// ── voice_analyzer.h ──────────────────────────────────────────────────────────
// Light-weight mic→mouth_open driver. Stereo S16 samples (as they come off
// the RP2350 UAC2 stream in AudioEngine) are downmixed to mono, windowed,
// FFT'd, and reduced to two smoothed scalars:
//
//   volume()      — broadband RMS energy in [0, 1]
//   mouth_open()  — speech-band energy after gain + noise gate + envelope
//                   follower in [0, 1]; this is what face::FaceState::set_audio
//                   gets called with each period.
//
// All tuning live-mutable from the menu thread; outputs read-safe from any
// thread (atomics). Hand-rolled radix-2 FFT — no external dependency. Sized
// for ~21 ms hop at 48 kHz (fft_size=1024, hop=512).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace audio {

class VoiceAnalyzer {
public:
    struct Config {
        bool   enabled     = false;     // off by default; flip via config.json or menu
        int    sample_rate = 48000;
        int    fft_size    = 1024;      // power of 2
        int    hop_size    = 512;       // samples between analyses (FFT runs each hop)
        float  band_lo_hz  = 100.f;     // speech-band lower edge
        float  band_hi_hz  = 3500.f;    // speech-band upper edge
        float  sensitivity = 1.0f;      // band energy multiplied by this before gate
        float  noise_gate  = 0.02f;     // band RMS below this → mouth_open = 0
        float  attack_ms   = 30.f;      // env follower time constant when opening
        float  release_ms  = 120.f;     // env follower time constant when closing
    };

    explicit VoiceAnalyzer(Config cfg);
    ~VoiceAnalyzer() = default;

    // Push one period of interleaved stereo S16 (the format AudioEngine uses).
    // Called from the audio thread; the only writer of internal buffers.
    void push_stereo_s16(const int16_t* samples, int frames);

    // Advance the envelope follower. dt is the period this push covered;
    // call after push() on the audio thread.
    void update(double dt_s);

    // Live tuning — safe to call from the menu thread. Picked up by the next
    // analysis cycle (no allocation, no lock).
    void set_enabled    (bool  v) { enabled_.store(v); }
    void set_sensitivity(float v) { sensitivity_.store(v > 0.f ? v : 0.f); }
    void set_noise_gate (float v) { noise_gate_.store(v >= 0.f ? v : 0.f); }
    void set_attack_ms  (float v) { attack_ms_.store(v > 0.f ? v : 1.f); }
    void set_release_ms (float v) { release_ms_.store(v > 0.f ? v : 1.f); }
    void set_band       (float lo_hz, float hi_hz);

    // Smoothed outputs in [0, 1]. centroid_hz() is the energy-weighted mean
    // bin frequency inside the active band (or 0 when below the gate); useful
    // later for picking between multiple mouth shapes once the asset side
    // supports visemes.
    double mouth_open()  const { return mouth_open_.load(); }
    double volume()      const { return volume_.load();     }
    double centroid_hz() const { return centroid_hz_.load(); }

    bool   enabled() const { return enabled_.load(); }

private:
    void fft_radix2_in_place(std::vector<float>& re, std::vector<float>& im) const;

    Config cfg_;

    // Atomic live-tunable params (mirror Config; menu writes here directly).
    std::atomic<bool>  enabled_;
    std::atomic<float> sensitivity_;
    std::atomic<float> noise_gate_;
    std::atomic<float> attack_ms_;
    std::atomic<float> release_ms_;
    std::atomic<float> band_lo_hz_;
    std::atomic<float> band_hi_hz_;

    // Atomic outputs.
    std::atomic<double> mouth_open_  { 0.0 };
    std::atomic<double> volume_      { 0.0 };
    std::atomic<double> centroid_hz_ { 0.0 };

    // Internal state (audio-thread only).
    std::vector<float> ring_;        // mono ring buffer, length fft_size
    size_t             ring_pos_  = 0;
    size_t             since_hop_ = 0;
    std::vector<float> hann_;        // precomputed Hann window
    std::vector<float> work_re_, work_im_;
    double             env_mouth_ = 0.0;   // smoothed mouth target
    double             env_vol_   = 0.0;   // smoothed broadband volume
};

} // namespace audio
