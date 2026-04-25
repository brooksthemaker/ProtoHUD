#include "audio_engine.h"

#include <alsa/asoundlib.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <vector>

// ── Construction ──────────────────────────────────────────────────────────────

AudioEngine::AudioEngine(const AudioConfig& cfg, AppState& state)
    : cfg_(cfg), state_(state)
{
    enabled_.store(cfg.enabled);
    master_gain_.store(cfg.master_gain);
    pending_output_.store(static_cast<int>(cfg.active_output));
}

AudioEngine::~AudioEngine() { stop(); }

// ── ALSA helpers ──────────────────────────────────────────────────────────────

static bool alsa_set_hw_params(snd_pcm_t* pcm, int rate, int channels,
                                int period, int n_periods) {
    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);

    auto chk = [](int r, const char* msg) -> bool {
        if (r < 0) { std::cerr << "[audio] " << msg << ": " << snd_strerror(r) << "\n"; return false; }
        return true;
    };

    if (!chk(snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED), "set_access")) return false;
    if (!chk(snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE),          "set_format")) return false;
    if (!chk(snd_pcm_hw_params_set_rate(pcm, hw, rate, 0),                           "set_rate"))   return false;
    if (!chk(snd_pcm_hw_params_set_channels(pcm, hw, channels),                      "set_ch"))     return false;

    snd_pcm_uframes_t pf = period;
    if (!chk(snd_pcm_hw_params_set_period_size_near(pcm, hw, &pf, nullptr),          "set_period")) return false;

    unsigned int np = n_periods;
    if (!chk(snd_pcm_hw_params_set_periods_near(pcm, hw, &np, nullptr),              "set_periods")) return false;
    if (!chk(snd_pcm_hw_params(pcm, hw),                                             "hw_params"))   return false;
    return true;
}

static bool alsa_set_sw_params(snd_pcm_t* pcm, int period) {
    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, period);
    snd_pcm_sw_params_set_avail_min(pcm, sw, period);
    int r = snd_pcm_sw_params(pcm, sw);
    if (r < 0) { std::cerr << "[audio] sw_params: " << snd_strerror(r) << "\n"; return false; }
    return true;
}

bool AudioEngine::open_alsa_capture(void** pcm_out) {
    snd_pcm_t* pcm = nullptr;
    int r = snd_pcm_open(&pcm, cfg_.capture_device.c_str(),
                         SND_PCM_STREAM_CAPTURE, 0);
    if (r < 0) {
        std::cerr << "[audio] Cannot open capture '" << cfg_.capture_device
                  << "': " << snd_strerror(r) << "\n"
                  << "[audio] Verify RP2350 is connected and check: arecord -l\n";
        return false;
    }
    if (!alsa_set_hw_params(pcm, cfg_.sample_rate, 2,
                             cfg_.period_size, cfg_.n_periods)) {
        snd_pcm_close(pcm); return false;
    }
    if (!alsa_set_sw_params(pcm, cfg_.period_size)) {
        snd_pcm_close(pcm); return false;
    }
    *pcm_out = pcm;
    std::cout << "[audio] Capture  : " << cfg_.capture_device
              << " | stereo @ " << cfg_.sample_rate << " Hz  (RP2350)\n";
    return true;
}

bool AudioEngine::open_alsa_playback(void** pcm_out, const std::string& device) {
    snd_pcm_t* pcm = nullptr;
    int r = snd_pcm_open(&pcm, device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (r < 0) {
        std::cerr << "[audio] Cannot open playback '" << device
                  << "': " << snd_strerror(r) << "\n";
        return false;
    }
    if (!alsa_set_hw_params(pcm, cfg_.sample_rate, 2,
                             cfg_.period_size, cfg_.n_periods)) {
        snd_pcm_close(pcm); return false;
    }
    if (!alsa_set_sw_params(pcm, cfg_.period_size)) {
        snd_pcm_close(pcm); return false;
    }
    *pcm_out = pcm;
    std::cout << "[audio] Playback : " << device
              << " | stereo @ " << cfg_.sample_rate << " Hz\n";
    return true;
}

std::string AudioEngine::output_device_string(AudioOutput out) const {
    switch (out) {
        case AudioOutput::HEADPHONES: return cfg_.output_headphones;
        case AudioOutput::HDMI:       return cfg_.output_hdmi;
        case AudioOutput::VITURE:
        default:                      return cfg_.output_viture;
    }
}

// ── start / stop ──────────────────────────────────────────────────────────────

bool AudioEngine::start() {
    if (!cfg_.enabled) return true;

    if (!open_alsa_capture(&pcm_capture_)) return false;

    std::string initial_out = output_device_string(cfg_.active_output);
    if (!open_alsa_playback(&pcm_playback_, initial_out)) {
        snd_pcm_close(static_cast<snd_pcm_t*>(pcm_capture_));
        pcm_capture_ = nullptr;
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(state_.mtx);
        state_.health.audio_ok = true;
        state_.audio.device_ok = true;
        state_.audio.enabled   = true;
        state_.audio.output    = static_cast<int>(cfg_.active_output);
    }

    // Clear any pending switch queued before start
    pending_output_.store(-1);

    running_.store(true);
    thread_ = std::thread(&AudioEngine::audio_thread_fn, this);

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(3, &cpuset);
    pthread_setaffinity_np(thread_.native_handle(), sizeof(cpuset), &cpuset);

    return true;
}

void AudioEngine::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();

    if (pcm_capture_) {
        snd_pcm_close(static_cast<snd_pcm_t*>(pcm_capture_));
        pcm_capture_ = nullptr;
    }
    if (pcm_playback_) {
        snd_pcm_close(static_cast<snd_pcm_t*>(pcm_playback_));
        pcm_playback_ = nullptr;
    }

    std::lock_guard<std::mutex> lk(state_.mtx);
    state_.health.audio_ok = false;
    state_.audio.enabled   = false;
}

// ── Output switching ──────────────────────────────────────────────────────────

void AudioEngine::set_output(AudioOutput out) {
    pending_output_.store(static_cast<int>(out));
}

AudioOutput AudioEngine::get_output() const {
    return cfg_.active_output;
}

// ── Audio thread ──────────────────────────────────────────────────────────────

void AudioEngine::audio_thread_fn() {
    snd_pcm_t* cap = static_cast<snd_pcm_t*>(pcm_capture_);
    snd_pcm_t* pb  = static_cast<snd_pcm_t*>(pcm_playback_);

    const int P = cfg_.period_size;

    // Stereo interleaved buffer (RP2350 outputs S16_LE stereo)
    std::vector<int16_t> buf(P * 2, 0);

    snd_pcm_start(cap);

    while (running_.load()) {
        // ── Output device switch ──────────────────────────────────────
        int pending = pending_output_.exchange(-1);
        if (pending >= 0) {
            std::string new_dev = output_device_string(static_cast<AudioOutput>(pending));
            if (pb) {
                snd_pcm_close(pb);
                pb = nullptr;
                pcm_playback_ = nullptr;
            }
            if (open_alsa_playback(reinterpret_cast<void**>(&pcm_playback_), new_dev)) {
                pb = static_cast<snd_pcm_t*>(pcm_playback_);
                cfg_.active_output = static_cast<AudioOutput>(pending);
                std::lock_guard<std::mutex> lk(state_.mtx);
                state_.audio.output    = pending;
                state_.audio.device_ok = true;
            } else {
                std::cerr << "[audio] Output switch failed — audio paused\n";
                std::lock_guard<std::mutex> lk(state_.mtx);
                state_.audio.device_ok = false;
            }
        }

        // ── Capture from RP2350 ───────────────────────────────────────
        snd_pcm_sframes_t frames = snd_pcm_readi(cap, buf.data(), P);
        if (frames == -EPIPE || frames == -ESTRPIPE) {
            snd_pcm_recover(cap, frames, 0);
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.audio.xrun_count++;
            continue;
        }
        if (frames < 0) {
            std::cerr << "[audio] Capture error: " << snd_strerror(frames) << "\n";
            break;
        }

        auto t0 = std::chrono::steady_clock::now();

        // ── Apply master gain (or mute) ───────────────────────────────
        if (enabled_.load()) {
            float mg = master_gain_.load();
            if (mg != 1.0f) {
                for (auto& s : buf)
                    s = static_cast<int16_t>(
                        std::clamp(static_cast<float>(s) * mg, -32768.0f, 32767.0f));
            }
        } else {
            std::fill(buf.begin(), buf.end(), int16_t{0});
        }

        // ── Write to selected output ──────────────────────────────────
        if (pb) {
            snd_pcm_sframes_t written = snd_pcm_writei(
                pb, buf.data(), static_cast<snd_pcm_uframes_t>(frames));
            if (written == -EPIPE || written == -ESTRPIPE) {
                snd_pcm_recover(pb, written, 0);
                std::lock_guard<std::mutex> lk(state_.mtx);
                state_.audio.xrun_count++;
            }
        }

        // ── CPU load & telemetry (sampled every ~250 ms) ──────────────
        auto t1 = std::chrono::steady_clock::now();
        float elapsed_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
        float period_ms  = (static_cast<float>(P) / cfg_.sample_rate) * 1000.0f;
        cpu_load_.store(elapsed_ms / period_ms);

        static int tick = 0;
        if (++tick >= 50) {
            tick = 0;
            std::lock_guard<std::mutex> lk(state_.mtx);
            state_.audio.cpu_load = cpu_load_.load();
            state_.audio.enabled  = enabled_.load();
        }
    }
}
