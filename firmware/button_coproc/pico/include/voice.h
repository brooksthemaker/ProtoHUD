#pragma once
// ── voice.h ──────────────────────────────────────────────────────────────────
// Optional real-time voice changer for the ProtoHUD coprocessor, running on
// core1 (see docs/voice-changer.md and config.h's "Voice changer" section).
//
// core0 (main.cpp) owns the buttons + serial protocol and forwards voice
// commands here via voice_handle_command(). core1 owns the audio loop:
//   voice_setup()   once from setup1()
//   voice_service() repeatedly from loop1()  — processes one I2S-paced frame
//
// The control struct is written by core0 (command parsing) and read by core1
// (the audio loop). Fields are simple scalars marked volatile; a stale read for
// one frame during a change is harmless (no torn multi-word state is relied on).

#include <Arduino.h>

enum VoiceFx {
    VFX_PASS = 0,   // clean passthrough (use this first to prove mic + DAC)
    VFX_PITCH,      // granular pitch shift (deep / chipmunk) — the headline effect
    VFX_ROBOT,      // ring modulation (metallic / robot)
    VFX_CRUSH,      // bitcrush + sample-rate reduction (glitchy)
    VFX_ECHO,       // delay line with feedback
    VFX_COUNT
};

struct VoiceState {
    volatile bool  enabled    = false;   // false = hard bypass (mic → DAC dry)
    volatile int   fx         = VFX_PITCH;
    volatile float pitch_semi = -5.0f;   // semitones; negative = deeper voice
    volatile int   mix        = 100;     // wet %, 0..100 (dry = 100-mix)
    volatile float robot_hz   = 80.0f;   // ring-mod carrier frequency
    volatile int   crush_bits = 6;       // 1..16 effective bit depth
    volatile int   crush_down = 3;       // 1..16 sample-hold (SR reduction)
    volatile int   echo_ms    = 180;     // echo delay
    volatile int   echo_fb    = 45;      // echo feedback %, 0..95
};
extern VoiceState g_voice;

void voice_setup();                              // core1: init DAC, I2S, ADC
void voice_service();                            // core1: one I2S-paced frame
bool voice_handle_command(const String& line);   // core0: returns true if consumed
void voice_local_toggle();                       // core0: standalone on/off
void voice_local_cycle();                        // core0: standalone next effect
const char* voice_fx_name(int fx);
