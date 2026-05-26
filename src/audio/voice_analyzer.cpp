#include "voice_analyzer.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {
// Largest power-of-two <= n.
int floor_pow2(int n) {
    if (n < 2) return 1;
    int p = 1;
    while ((p << 1) <= n) p <<= 1;
    return p;
}
constexpr float PI = 3.14159265358979323846f;
} // namespace

VoiceAnalyzer::VoiceAnalyzer(Config cfg)
    : cfg_(cfg),
      enabled_    (cfg.enabled),
      sensitivity_(cfg.sensitivity),
      noise_gate_ (cfg.noise_gate),
      attack_ms_  (cfg.attack_ms),
      release_ms_ (cfg.release_ms),
      band_lo_hz_ (cfg.band_lo_hz),
      band_hi_hz_ (cfg.band_hi_hz)
{
    // Clamp fft_size to a power of 2, hop into [1, fft_size].
    cfg_.fft_size = floor_pow2(std::max(64, cfg_.fft_size));
    cfg_.hop_size = std::clamp(cfg_.hop_size, 1, cfg_.fft_size);

    ring_.assign(cfg_.fft_size, 0.f);
    hann_.resize(cfg_.fft_size);
    for (int i = 0; i < cfg_.fft_size; ++i)
        hann_[i] = 0.5f * (1.f - std::cos(2.f * PI * i / (cfg_.fft_size - 1)));

    work_re_.resize(cfg_.fft_size);
    work_im_.resize(cfg_.fft_size);
}

void VoiceAnalyzer::set_band(float lo_hz, float hi_hz) {
    if (lo_hz < 20.f)   lo_hz = 20.f;
    if (hi_hz < lo_hz)  hi_hz = lo_hz + 100.f;
    band_lo_hz_.store(lo_hz);
    band_hi_hz_.store(hi_hz);
}

// ── Iterative radix-2 Cooley-Tukey, in-place ─────────────────────────────────

void VoiceAnalyzer::fft_radix2_in_place(std::vector<float>& re,
                                        std::vector<float>& im) const {
    const int N = static_cast<int>(re.size());

    // Bit-reversal permutation.
    int j = 0;
    for (int i = 1; i < N; ++i) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    // Butterflies.
    for (int len = 2; len <= N; len <<= 1) {
        const float ang = -2.f * PI / len;
        const float wlen_re = std::cos(ang);
        const float wlen_im = std::sin(ang);
        for (int i = 0; i < N; i += len) {
            float w_re = 1.f, w_im = 0.f;
            const int half = len / 2;
            for (int k = 0; k < half; ++k) {
                const float u_re = re[i + k];
                const float u_im = im[i + k];
                const float v_re = re[i + k + half] * w_re - im[i + k + half] * w_im;
                const float v_im = re[i + k + half] * w_im + im[i + k + half] * w_re;
                re[i + k]        = u_re + v_re;
                im[i + k]        = u_im + v_im;
                re[i + k + half] = u_re - v_re;
                im[i + k + half] = u_im - v_im;
                const float nw_re = w_re * wlen_re - w_im * wlen_im;
                const float nw_im = w_re * wlen_im + w_im * wlen_re;
                w_re = nw_re;
                w_im = nw_im;
            }
        }
    }
}

// ── Sample ingestion ─────────────────────────────────────────────────────────

void VoiceAnalyzer::push_stereo_s16(const int16_t* samples, int frames) {
    if (!enabled_.load() || frames <= 0) return;

    const int N = cfg_.fft_size;
    const int H = cfg_.hop_size;

    // Downmix stereo → mono into the ring buffer.
    for (int i = 0; i < frames; ++i) {
        const float mono =
            (static_cast<float>(samples[2 * i]) + static_cast<float>(samples[2 * i + 1]))
            / (2.f * 32768.f);
        ring_[ring_pos_] = mono;
        ring_pos_ = (ring_pos_ + 1) % N;
        ++since_hop_;
    }

    // No FFT yet — wait for a full hop.
    if (since_hop_ < static_cast<size_t>(H)) return;

    while (since_hop_ >= static_cast<size_t>(H)) {
        since_hop_ -= H;

        // Copy ring buffer into work buffers in chronological order, applying
        // the Hann window as we go.
        for (int i = 0; i < N; ++i) {
            const size_t idx = (ring_pos_ + i) % N;
            work_re_[i] = ring_[idx] * hann_[i];
            work_im_[i] = 0.f;
        }
        fft_radix2_in_place(work_re_, work_im_);

        // Convert magnitudes to band energy + spectral centroid in the user's
        // configured speech band. Only bins [1, N/2] are physically meaningful
        // for a real input.
        const float lo = band_lo_hz_.load();
        const float hi = band_hi_hz_.load();
        const float bin_hz = static_cast<float>(cfg_.sample_rate) / N;
        const int k_lo = std::clamp(static_cast<int>(std::floor(lo / bin_hz)),
                                    1, N / 2);
        const int k_hi = std::clamp(static_cast<int>(std::ceil (hi / bin_hz)),
                                    k_lo, N / 2);

        double broadband_mag2 = 0.0, band_mag2 = 0.0;
        double centroid_num = 0.0, centroid_den = 0.0;
        for (int k = 1; k <= N / 2; ++k) {
            const double m2 = static_cast<double>(work_re_[k]) * work_re_[k]
                            + static_cast<double>(work_im_[k]) * work_im_[k];
            broadband_mag2 += m2;
            if (k >= k_lo && k <= k_hi) {
                band_mag2 += m2;
                const double m = std::sqrt(m2);
                centroid_num += m * (k * bin_hz);
                centroid_den += m;
            }
        }

        // Hann gain ≈ N/2 for power; normalise so the result is roughly RMS
        // in [0, ~1].
        const double scale = (broadband_mag2 > 0.0)
            ? 2.0 / static_cast<double>(N) : 0.0;
        const double rms_full = std::sqrt(broadband_mag2) * scale;
        const double rms_band = std::sqrt(band_mag2)      * scale;

        const float gain = sensitivity_.load();
        const float gate = noise_gate_.load();

        const double gated = (rms_band > gate)
            ? std::min(1.0, static_cast<double>(rms_band - gate) * gain)
            : 0.0;

        // Targets for the envelope follower in update().
        env_vol_   = std::min(1.0, rms_full * 4.0);   // broadband loudness, generous range
        env_mouth_ = gated;
        if (centroid_den > 0.0)
            centroid_hz_.store(centroid_num / centroid_den);
        else
            centroid_hz_.store(0.0);
    }
}

void VoiceAnalyzer::update(double dt_s) {
    if (!enabled_.load() || dt_s <= 0.0) return;

    const double atk_ms = std::max(1.0, static_cast<double>(attack_ms_.load()));
    const double rel_ms = std::max(1.0, static_cast<double>(release_ms_.load()));

    const double cur_mouth = mouth_open_.load();
    const double cur_vol   = volume_.load();

    auto step = [dt_s](double tgt, double cur, double atk_s, double rel_s) {
        // Different time constants for opening vs. closing — feels much more
        // speech-like than a single LPF.
        const double tau   = (tgt > cur) ? atk_s : rel_s;
        const double alpha = 1.0 - std::exp(-dt_s / std::max(1e-4, tau));
        return cur + (tgt - cur) * alpha;
    };

    mouth_open_.store(step(env_mouth_, cur_mouth, atk_ms / 1000.0, rel_ms / 1000.0));
    volume_    .store(step(env_vol_,   cur_vol,   atk_ms / 1000.0, rel_ms / 1000.0));
}

} // namespace audio
