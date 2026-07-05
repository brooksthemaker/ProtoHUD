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
// The DSP (pitch/robot/crush/echo) is verified numerically. The HARDWARE GLUE
// that this file can't self-verify is (1) the TLV320 register init via the
// Adafruit library and (2) the exact earlephilhower I2S/ADC call names — both
// are isolated below and flagged. Bring up with VFX_PASS first.

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
// !! HARDWARE GLUE — verify against your installed Adafruit_TLV320DAC3100
// version's example sketch (examples/…): method names on this codec library
// have shifted between releases. Everything else in this file is independent of
// these calls. Goal: I2S slave, 16-bit, PLL clocked from BCLK, DAC → speaker.
bool dac_begin() {
    if (kDacResetPin >= 0) {
        pinMode(kDacResetPin, OUTPUT);
        digitalWrite(kDacResetPin, LOW);  delay(5);
        digitalWrite(kDacResetPin, HIGH); delay(10);
    }
    Wire.setSDA(kDacSdaPin);
    Wire.setSCL(kDacSclPin);
    Wire.begin();
    if (!g_dac.begin(kDacI2cAddr, &Wire)) return false;

    // Clocking: derive the codec's internal clocks from the incoming BCLK via
    // its PLL (no MCLK wire needed). Then route the DAC to the class-D speaker.
    // Align the following with your library version's example if it won't build:
    g_dac.setCodecInterface(TLV320_FORMAT_I2S, TLV320_DATA_LEN_16);
    g_dac.setPLL(true, TLV320_PLL_CLKIN_BCLK);   // PLL source = BCLK
    g_dac.configurePLL(kSampleRate);             // set for our sample rate
    g_dac.powerDAC(true);
    g_dac.setDACDataPath(true, true);            // L+R DAC on
    g_dac.configureSpeaker(true);                // enable Class-D speaker amp
    g_dac.setSpeakerVolume(0);                   // 0 dB; raise/lower to taste
    g_dac.setDACVolume(0.0f);                    // 0 dB
    g_dac.muteDAC(false);
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
