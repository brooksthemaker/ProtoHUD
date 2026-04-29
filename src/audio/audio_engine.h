#pragma once
// ── audio_engine.h ────────────────────────────────────────────────────────────
// Receives processed stereo audio from the RP2350 helmet audio processor via
// USB Audio (UAC2) and routes it to the selected output device.
//
// The RP2350 handles all mic capture, beamforming, and noise suppression.
// This engine just does: RP2350 USB capture → master gain → ALSA playback.
//
// Supported outputs (switchable at runtime):
//   VITURE      — hw:CARD=VITUREXRGlasses,DEV=0  (VITURE Beast glasses)
//   HEADPHONES  — hw:CARD=Headphones,DEV=0        (3.5 mm jack)
//   HDMI        — hw:CARD=vc4hdmi0,DEV=0          (HDMI audio)

#include "../app_state.h"

#include <atomic>
#include <string>
#include <thread>

enum class AudioOutput {
    VITURE     = 0,
    HEADPHONES = 1,
    HDMI       = 2,
};

struct AudioConfig {
    bool        enabled           = true;
    // RP2350 presents itself as a UAC2 stereo device.
    // Verify name after connecting with: arecord -l
    std::string capture_device    = "hw:CARD=HelmetAudio6Mic,DEV=0";
    std::string output_viture     = "hw:CARD=VITUREXRGlasses,DEV=0";
    std::string output_headphones = "hw:CARD=Headphones,DEV=0";
    std::string output_hdmi       = "hw:CARD=vc4hdmi0,DEV=0";
    AudioOutput active_output     = AudioOutput::VITURE;
    int         sample_rate       = 48000;
    int         period_size       = 256;
    int         n_periods         = 4;
    float       master_gain       = 1.0f;
};

class AudioEngine {
public:
    AudioEngine(const AudioConfig& cfg, AppState& state);
    ~AudioEngine();

    bool start();
    void stop();

    void  set_enabled(bool en)      { enabled_.store(en);    }
    void  set_master_gain(float g)  { master_gain_.store(g); }
    float get_master_gain() const   { return master_gain_.load(); }
    bool  is_enabled()   const      { return enabled_.load(); }
    float get_cpu_load() const      { return cpu_load_.load(); }
    bool  is_running()   const      { return running_.load(); }

    void        set_output(AudioOutput out);
    AudioOutput get_output() const;

private:
    void  audio_thread_fn();
    bool  open_alsa_capture(void** pcm_out);
    bool  open_alsa_playback(void** pcm_out, const std::string& device);
    std::string output_device_string(AudioOutput out) const;

    AudioConfig cfg_;
    AppState&   state_;

    std::thread        thread_;
    std::atomic<bool>  running_       { false };
    std::atomic<bool>  enabled_       { true  };
    std::atomic<float> master_gain_   { 1.0f  };
    std::atomic<float> cpu_load_      { 0.0f  };
    std::atomic<int>   pending_output_{ -1    }; // -1 = no switch pending

    void* pcm_capture_  = nullptr;
    void* pcm_playback_ = nullptr;
};
