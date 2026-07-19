// ── voice.cpp — ProtoHUD coprocessor voice changer (core1) ───────────────────
// Real-time mic → effect → speaker on the RP2350's second core. Signal path:
//   electret mic → MAX9814 → RP2350 ADC → DSP here → I2S → TLV320DAC3100 → spkr
//
// Timing model: the I2S output is the clock. i2s.write() blocks when its DMA
// buffer is full, so the loop runs at exactly the sample rate; we read ONE ADC
// sample per output frame, which keeps input and output sample-locked with no
// drift (and no input DMA to babysit).
//
// Compiled only when -DVOICE_CHANGER is set (see platformio.ini). Without it
// this file is empty and the plain button coprocessor needs no audio libraries.
//
// The DSP (pitch/robot/crush/echo) is verified numerically, and the file now
// compiles against the pinned library and core, which settles the TLV320 and
// earlephilhower I2S/ADC call names. What no build can settle is whether
// dac_begin() actually produces sound: its divider chain is solved against the
// datasheet bounds but has not been run on the board. Bring up with VFX_PASS.

#ifdef VOICE_CHANGER

#include <Arduino.h>
#include <I2S.h>
#include <Wire.h>
#include <Adafruit_TLV320DAC3100.h>

#include "config.h"
#include "voice.h"

VoiceState g_voice;

namespace {

I2S                      g_i2s(OUTPUT);
Adafruit_TLV320DAC3100   g_dac;

// DAC bring-up status: set to the step name before each fallible call in
// dac_begin(), left there if the call fails, or "ok" once the whole chain
// passes. Queried by the DACSTAT verb so "tone but no sound" can be split into
// a failed init (software / PLL) vs a good init but dead amp (SPKVDD / speaker).
const char* g_dacStatus = "not run";

constexpr int   kFs   = static_cast<int>(kSampleRate);

// ── Effect state (touched only by core1) ─────────────────────────────────────
// Pitch: granular 2-tap delay-line shifter (bounded, no FFT).
constexpr int   kPBuf = 4096;          // ring buffer (power of two)
constexpr int   kGrain = 1024;         // grain length
float           g_pbuf[kPBuf];
int             g_wp    = 0;
float           g_phase = 0.0f;

// Ring modulator carrier phase.
float           g_rmPhase = 0.0f;

// Bitcrush sample-and-hold.
float           g_held = 0.0f;
int             g_holdCnt = 0;

// Echo delay line (max 0.5 s at the configured rate).
constexpr int   kEchoMax = kFs / 2;
float           g_echo[kEchoMax];
int             g_ei = 0;

// DC blocker (one-pole high-pass) — strips the ADC's mid-rail bias/drift.
float           g_dcx1 = 0.0f, g_dcy1 = 0.0f;

// Speaker self-test tone (TONE verb): sine phase accumulator + amplitude.
// 0.6 of full scale — plainly audible without slamming the class-D into clip.
float           g_tonePhase = 0.0f;
constexpr float kToneAmp   = 0.6f;
constexpr float kTwoPi     = 6.2831853071795864f;

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
inline float samp_at(float f) {                 // linear-interp read of g_pbuf
    while (f < 0) f += kPBuf;
    int i = static_cast<int>(f) & (kPBuf - 1);
    int j = (i + 1) & (kPBuf - 1);
    float fr = f - static_cast<int>(f);
    return g_pbuf[i] + (g_pbuf[j] - g_pbuf[i]) * fr;
}
inline float tri(float p) { return 1.0f - fabsf(2.0f * p / kGrain - 1.0f); }

// ── Individual effects: take the current dry sample, return the wet sample ────
float fx_pitch(float x) {
    g_pbuf[g_wp] = x;
    // powf costs 100+ cycles; at 16 kHz per-sample that's real core1 time, so
    // the semitone→speed curve is cached and recomputed only when PITCH changes.
    static float cached_semi  = 1e9f;
    static float cached_speed = 1.0f;
    const float semi = g_voice.pitch_semi;
    if (semi != cached_semi) {
        cached_semi  = semi;
        cached_speed = powf(2.0f, semi / 12.0f);
    }
    // step = speed-1: read pointer catches up to the writer for higher pitch.
    // (If pitch direction ends up reversed on your build, negate this.)
    const float step = cached_speed - 1.0f;
    const float p2   = fmodf(g_phase + kGrain / 2.0f, kGrain);
    const float out  = tri(g_phase) * samp_at(g_wp - kGrain + g_phase)
                     + tri(p2)      * samp_at(g_wp - kGrain + p2);
    g_phase = fmodf(g_phase + step + kGrain, kGrain);
    g_wp    = (g_wp + 1) & (kPBuf - 1);
    return out;
}
float fx_robot(float x) {
    const float y = x * sinf(g_rmPhase);
    g_rmPhase += 2.0f * static_cast<float>(M_PI) * g_voice.robot_hz / kFs;
    if (g_rmPhase > 2.0f * static_cast<float>(M_PI)) g_rmPhase -= 2.0f * static_cast<float>(M_PI);
    return y;
}
float fx_crush(float x) {
    const int down = g_voice.crush_down < 1 ? 1 : g_voice.crush_down;
    if (g_holdCnt <= 0) { g_held = x; g_holdCnt = down; }
    g_holdCnt--;
    const int bits = g_voice.crush_bits < 1 ? 1 : (g_voice.crush_bits > 16 ? 16 : g_voice.crush_bits);
    const float q = static_cast<float>(1 << (bits - 1));
    return roundf(g_held * q) / q;
}
float fx_echo(float x) {
    int d = g_voice.echo_ms * kFs / 1000;
    if (d < 1) d = 1; if (d >= kEchoMax) d = kEchoMax - 1;
    const float fb = clampf(g_voice.echo_fb * 0.01f, 0.0f, 0.95f);
    const float y = x + fb * g_echo[(g_ei - d + kEchoMax) % kEchoMax];
    g_echo[g_ei] = y;
    g_ei = (g_ei + 1) % kEchoMax;
    return clampf(y, -1.5f, 1.5f);
}

// ── TLV320DAC3100 register init ───────────────────────────────────────────────
// Written against the "Adafruit TLV320 I2S" library, mirroring the order of its
// examples/basicI2Sconfig sketch. Goal: I2S slave, 16-bit, PLL clocked from the
// incoming BCLK (no MCLK wire), DAC → mixer → class-D speaker.
//
// The divider chain below is solved for kSampleRate and nothing else — redo all
// of it together if you change the rate, because the datasheet bounds (enforced
// by the library's validatePLLConfig) are tight at 16 kHz:
//   BCLK    = 16 bit x 2 ch x 16 kHz              = 512 kHz  (RP2350 is master)
//   PLL_CLK = BCLK x R x J / P = 512k x 4 x 48 / 1 = 98.304 MHz  (must be
//                                                    80..110 MHz)
//   Fs      = PLL_CLK / (NDAC x MDAC x DOSR) = 98.304M / (6 x 8 x 128) = 16 kHz
// PLL_CLKIN/P must be >= 512 kHz, which pins P at 1 for this BCLK.
//
// Don't "simplify" this to configurePLL(): it solves J = ratio x P x R, the
// inverse of the datasheet's J = ratio x P / R that the rest of the library
// (and this chain) follows, so it finds no solution here and returns false.
bool dac_begin() {
    if (kDacResetPin >= 0) {
        pinMode(kDacResetPin, OUTPUT);
        digitalWrite(kDacResetPin, LOW);  delay(5);
        digitalWrite(kDacResetPin, HIGH); delay(10);
    }
    Wire.setSDA(kDacSdaPin);
    Wire.setSCL(kDacSclPin);
    Wire.begin();
    g_dacStatus = "begin";
    if (!g_dac.begin(kDacI2cAddr, &Wire)) return false;

    // Interface. bclk_out/wclk_out stay at their false defaults: the codec is
    // the I2S slave, the RP2350 drives BCLK and WS.
    g_dacStatus = "setCodecInterface";
    if (!g_dac.setCodecInterface(TLV320DAC3100_FORMAT_I2S,
                                 TLV320DAC3100_DATA_LEN_16)) return false;

    // Clocks: codec runs off the PLL, PLL runs off BCLK.
    g_dacStatus = "setCodecClockInput";
    if (!g_dac.setCodecClockInput(TLV320DAC3100_CODEC_CLKIN_PLL)) return false;
    g_dacStatus = "setPLLClockInput";
    if (!g_dac.setPLLClockInput(TLV320DAC3100_PLL_CLKIN_BCLK)) return false;
    g_dacStatus = "setPLLValues";
    if (!g_dac.setPLLValues(1, 4, 48, 0)) return false;   // P, R, J, D (see above)
    g_dacStatus = "setNDAC";
    if (!g_dac.setNDAC(true, 6)) return false;
    g_dacStatus = "setMDAC";
    if (!g_dac.setMDAC(true, 8)) return false;
    g_dacStatus = "setDOSR";
    if (!g_dac.setDOSR(128)) return false;
    g_dacStatus = "powerPLL";
    if (!g_dac.powerPLL(true)) return false;

    // DAC → output mixer → class-D speaker.
    g_dacStatus = "setDACDataPath";
    if (!g_dac.setDACDataPath(true, true,                 // L+R DAC on
                              TLV320_DAC_PATH_NORMAL,
                              TLV320_DAC_PATH_NORMAL,
                              TLV320_VOLUME_STEP_1SAMPLE)) return false;
    g_dacStatus = "configureAnalogInputs";
    if (!g_dac.configureAnalogInputs(TLV320_DAC_ROUTE_MIXER,
                                     TLV320_DAC_ROUTE_MIXER,
                                     false, false, false,  // no AIN routing
                                     false)) return false; // no HPL→HPR
    g_dacStatus = "setDACVolumeControl";
    if (!g_dac.setDACVolumeControl(false, false,          // unmute L+R
                                   TLV320_VOL_INDEPENDENT)) return false;
    g_dacStatus = "setChannelVolume";
    if (!g_dac.setChannelVolume(false, 0.0f) ||           // 0 dB
        !g_dac.setChannelVolume(true,  0.0f)) return false;

    // Headphone drivers. HPL/HPR are this build's actual output: they feed the
    // external MAX98306 power amp (HPL→L+, HPR→R+, its L−/R− grounded), which
    // drives the speaker. Powering + routing + unmuting them is what the old
    // speaker-only init left out, so the HP pins sat dead. Note HPL/HPR carry a
    // ~1.35 V common-mode bias; the amp input must tolerate or AC-couple it.
    g_dacStatus = "configureHeadphoneDriver";
    if (!g_dac.configureHeadphoneDriver(true, true,       // power up L+R
                                        TLV320_HP_COMMON_1_35V,
                                        false)) return false;
    g_dacStatus = "configureHPL_PGA";
    if (!g_dac.configureHPL_PGA(0, true)) return false;   // 0 dB, unmute
    g_dacStatus = "configureHPR_PGA";
    if (!g_dac.configureHPR_PGA(0, true)) return false;
    g_dacStatus = "setHPLVolume";
    if (!g_dac.setHPLVolume(true, 6)) return false;       // route on, +vol
    g_dacStatus = "setHPRVolume";
    if (!g_dac.setHPRVolume(true, 6)) return false;

    g_dacStatus = "enableSpeaker";
    if (!g_dac.enableSpeaker(true)) return false;
    g_dacStatus = "configureSPK_PGA";
    if (!g_dac.configureSPK_PGA(TLV320_SPK_GAIN_6DB, true)) return false;
    g_dacStatus = "setSPKVolume";
    if (!g_dac.setSPKVolume(true, 0)) return false;       // 0 dB; raise to taste
    g_dacStatus = "ok";
    return true;
}

}  // namespace

const char* voice_fx_name(int fx) {
    switch (fx) {
        case VFX_PASS:  return "pass";
        case VFX_PITCH: return "pitch";
        case VFX_ROBOT: return "robot";
        case VFX_CRUSH: return "crush";
        case VFX_ECHO:  return "echo";
        default:        return "?";
    }
}

void voice_setup() {
    g_voice.enabled = kVoiceEnabled;           // config.h default (start active?)
    for (int i = 0; i < kPBuf; ++i)   g_pbuf[i] = 0.0f;
    for (int i = 0; i < kEchoMax; ++i) g_echo[i] = 0.0f;

    analogReadResolution(12);                  // 0..4095 from the mic ADC

    // I2S master out: generates BCLK (+WS on BCLK+1) and DATA for the TLV320.
    g_i2s.setBCLK(kI2sBclkPin);                // WS = kI2sBclkPin + 1 automatically
    g_i2s.setDATA(kI2sDoutPin);
    g_i2s.setBitsPerSample(16);
    g_i2s.begin(kSampleRate);

    dac_begin();                               // (returns false if I2C init failed)
}

void voice_service() {
    // One frame, paced by the blocking I2S write below.
    const int raw = analogRead(kMicAdcPin);           // 0..4095
    float in = (raw - 2048) * (1.0f / 2048.0f);       // ~ -1..1
    // DC block: hp = in - x1 + 0.995*y1
    const float hp = in - g_dcx1 + 0.995f * g_dcy1;
    g_dcx1 = in; g_dcy1 = hp; in = hp;

    // Mic level meter: hold the peak |sample| until the host reads it (MICLVL).
    // Post-DC-block, so it's the AC swing the mic actually picks up, not bias.
    { const float a = fabsf(in); if (a > g_voice.mic_peak) g_voice.mic_peak = a; }

    float wet = in;
    if (g_voice.enabled) {
        switch (g_voice.fx) {
            case VFX_PITCH: wet = fx_pitch(in); break;
            case VFX_ROBOT: wet = fx_robot(in); break;
            case VFX_CRUSH: wet = fx_crush(in); break;
            case VFX_ECHO:  wet = fx_echo(in);  break;
            case VFX_PASS:
            default:        wet = in;           break;
        }
    }

    float out = in;
    if (g_voice.enabled) {
        const float w = g_voice.mix * 0.01f;
        out = w * wet + (1.0f - w) * in;
    }

    // Speaker self-test: while a TONE is queued, replace the mic path with a
    // sine straight to the DAC and count it down. Runs regardless of enabled,
    // so the beep works even in hard-bypass; the I2S clock paces it as usual.
    if (g_voice.test_tone_left > 0) {
        out = kToneAmp * sinf(g_tonePhase);
        g_tonePhase += kTwoPi * g_voice.test_tone_hz / kFs;
        if (g_tonePhase >= kTwoPi) g_tonePhase -= kTwoPi;
        g_voice.test_tone_left--;
    }

    const int16_t s = static_cast<int16_t>(clampf(out, -1.0f, 1.0f) * 32767.0f);
    g_i2s.write(s);   // left  — blocks when the DMA buffer is full (= our clock)
    g_i2s.write(s);   // right — mono voice duplicated to both channels
}

// ── Control (called from core0's protocol / buttons) ─────────────────────────
static int fx_from_name(const String& n) {
    for (int i = 0; i < VFX_COUNT; ++i)
        if (n.equalsIgnoreCase(voice_fx_name(i))) return i;
    return -1;
}

bool voice_handle_command(const String& line) {
    // Hardware self-test verbs. These reply on their own line and return early,
    // skipping the VOICE-state echo the effect commands share below.
    if (line.startsWith("TONE")) {
        // TONE [hz] [ms] — speaker test; defaults 1 kHz for 500 ms.
        int hz = 1000, ms = 500;
        const int sp1 = line.indexOf(' ');
        if (sp1 > 0) {
            const int sp2 = line.indexOf(' ', sp1 + 1);
            hz = line.substring(sp1 + 1, sp2 > 0 ? sp2 : line.length()).toInt();
            if (sp2 > 0) ms = line.substring(sp2 + 1).toInt();
        }
        hz = constrain(hz, 50, 8000);
        ms = constrain(ms, 20, 3000);
        g_tonePhase = 0.0f;
        g_voice.test_tone_hz   = hz;
        g_voice.test_tone_left = static_cast<long>(ms) * kFs / 1000;
        Serial.print("TONE "); Serial.print(hz);
        Serial.print(' ');     Serial.println(ms);
        return true;
    }
    if (line == "MICLVL") {
        // Peak mic level since the last MICLVL, 0..1000 per-mille of full scale;
        // reset after reporting so each poll measures a fresh window.
        int pm = static_cast<int>(g_voice.mic_peak * 1000.0f + 0.5f);
        g_voice.mic_peak = 0.0f;
        Serial.print("MIC "); Serial.println(pm > 1000 ? 1000 : pm);
        return true;
    }
    if (line == "DACSTAT") {
        // Where dac_begin() got to, plus live codec flags when it finished:
        //   dac_run — DAC actually clocked+running (needs BCLK → PLL lock, so
        //             0 here = the I2S clock/BCLK wire isn't reaching the codec)
        //   classd  — class-D speaker driver powered (0 = SPKVDD/5V rail dead)
        //   spk_en  — speaker path enabled per our config
        //   spk_sc  — speaker output short-circuit detected
        // "ok" + classd=1 + spk_sc=0 but silent → it's the speaker / its wiring.
        Serial.print("DAC "); Serial.print(g_dacStatus);
        if (strcmp(g_dacStatus, "ok") == 0) {
            bool ldac=false, hpl=false, lcd=false, rdac=false,
                 hpr=false, rcd=false, lpga=false, rpga=false;
            g_dac.getDACFlags(&ldac, &hpl, &lcd, &rdac, &hpr, &rcd, &lpga, &rpga);
            Serial.print(" pll=");    Serial.print(g_dac.isPLLpowered() ? 1 : 0);
            Serial.print(" dac_run="); Serial.print(ldac ? 1 : 0);
            Serial.print(" hpl=");     Serial.print(hpl ? 1 : 0);   // HP left powered
            Serial.print(" hpr=");     Serial.print(hpr ? 1 : 0);   // HP right powered
            Serial.print(" classd=");  Serial.print(lcd ? 1 : 0);
            Serial.print(" spk_en=");  Serial.print(g_dac.speakerEnabled() ? 1 : 0);
            Serial.print(" spk_sc=");  Serial.print(g_dac.isSpeakerShorted() ? 1 : 0);
        }
        Serial.println();
        return true;
    }
    if (line.startsWith("BEEP")) {
        // BEEP [hz] [ms] — codec's INTERNAL tone generator, injected after the
        // I2S input, so it needs NO DIN data. If BEEP sounds but TONE doesn't,
        // the GP18 DIN wire is the problem; if neither sounds (and classd=1),
        // it's the speaker/its wiring. Defaults 1 kHz for 500 ms.
        int hz = 1000, ms = 500;
        const int sp1 = line.indexOf(' ');
        if (sp1 > 0) {
            const int sp2 = line.indexOf(' ', sp1 + 1);
            hz = line.substring(sp1 + 1, sp2 > 0 ? sp2 : line.length()).toInt();
            if (sp2 > 0) ms = line.substring(sp2 + 1).toInt();
        }
        hz = constrain(hz, 50, 8000);
        ms = constrain(ms, 20, 3000);
        g_dac.setBeepVolume(0, 0);                                  // 0 dB L+R
        g_dac.configureBeepTone((float)hz, (uint32_t)ms, kSampleRate);
        g_dac.enableBeep(true);
        Serial.print("BEEP "); Serial.print(hz);
        Serial.print(' ');     Serial.println(ms);
        return true;
    }

    if (line.startsWith("VOICE ")) {
        g_voice.enabled = line.substring(6).toInt() != 0;
    } else if (line.startsWith("FX ")) {
        int f = fx_from_name(line.substring(3));
        if (f >= 0) g_voice.fx = f;
    } else if (line.startsWith("PITCH ")) {
        g_voice.pitch_semi = clampf(line.substring(6).toFloat(), -12.0f, 12.0f);
    } else if (line.startsWith("MIX ")) {
        g_voice.mix = constrain(line.substring(4).toInt(), 0, 100);
    } else if (line.startsWith("PARAM ")) {
        // PARAM <name> <value>  — effect-specific knobs.
        const int sp = line.indexOf(' ', 6);
        if (sp > 0) {
            const String k = line.substring(6, sp);
            const float  v = line.substring(sp + 1).toFloat();
            if      (k == "robot_hz")   g_voice.robot_hz   = clampf(v, 10.0f, 4000.0f);
            else if (k == "crush_bits") g_voice.crush_bits = constrain((int)v, 1, 16);
            else if (k == "crush_down") g_voice.crush_down = constrain((int)v, 1, 16);
            else if (k == "echo_ms")    g_voice.echo_ms    = constrain((int)v, 1, 500);
            else if (k == "echo_fb")    g_voice.echo_fb    = constrain((int)v, 0, 95);
            else return false;
        }
    } else {
        return false;   // not a voice command
    }
    // Echo back the live state so the host can confirm.
    Serial.print("VOICE "); Serial.print(g_voice.enabled ? 1 : 0);
    Serial.print(" FX ");   Serial.print(voice_fx_name(g_voice.fx));
    Serial.print(" PITCH "); Serial.print(g_voice.pitch_semi, 1);
    Serial.print(" MIX ");  Serial.println(g_voice.mix);
    return true;
}

void voice_local_toggle() { g_voice.enabled = !g_voice.enabled; }
void voice_local_cycle()  { g_voice.fx = (g_voice.fx + 1) % VFX_COUNT; }

#endif  // VOICE_CHANGER
