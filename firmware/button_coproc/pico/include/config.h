#pragma once
// ── config.h ─────────────────────────────────────────────────────────────────
// Pin map + timing for the ProtoHUD button coprocessor firmware. Edit this file
// to match your wiring; nothing else needs changing for a basic build.
//
// The button id reported to the Pi is the INDEX in kButtonPins (0..N-1). The Pi
// maps id → GpioFunc in inputs.coprocessor.buttons, so the ORDER here is your id
// assignment — keep it stable once you've configured the HUD.

#include <Arduino.h>

// Firmware version, reported in the HELLO line so the Pi (and the flash script)
// can confirm an update actually took. Bump it whenever you change the firmware.
static constexpr const char* kFwVersion = "1.1.0";

// Switches wired between the listed GP pin and GND. We use INPUT_PULLUP, so a
// pressed switch reads LOW (active-low). Add/remove entries freely — kLedPins
// must stay the same length.
static constexpr uint8_t kButtonPins[] = { 2, 3, 4, 5, 6, 7, 8, 9 };

// Optional per-button LED / switch-backlight pins, parallel to kButtonPins.
// -1 = none. Driven HIGH/LOW by the Pi via "LED <id> <0|1>".
static constexpr int8_t  kLedPins[]    = { -1, -1, -1, -1, -1, -1, -1, -1 };

// ── Timing ────────────────────────────────────────────────────────────────────
static constexpr uint32_t kDebounceMs   = 15;    // ignore contact bounce shorter than this
static constexpr uint32_t kLongMsInit   = 600;   // initial short/long threshold (CFG-tunable)
static constexpr uint32_t kPingMs       = 1000;  // heartbeat period (~1 Hz)

// Emit advisory "BTN <id> DOWN/UP" raw edges too. The Pi ignores them in v1, so
// leave false unless you're debugging with a serial terminal.
static constexpr bool     kEmitDownUp   = false;

static_assert(sizeof(kButtonPins) == sizeof(kLedPins),
              "kButtonPins and kLedPins must have the same number of entries");

// ── Voice changer (optional, runs on core1) ──────────────────────────────────
// Real-time mic → effect → speaker, on the SECOND core so button debounce on
// core0 is never disturbed. Signal path (see docs/voice-changer.md):
//   electret mic → MAX9814 preamp/AGC → RP2350 ADC → DSP (core1) → I2S → TLV320
//   DAC3100 → speaker.  The loop is paced by the I2S output clock, so the ADC
//   input and DAC output stay sample-locked with no drift.
// Set kVoiceEnabled=false to build a plain button coprocessor (nothing below is
// used, no extra libraries needed).
static constexpr bool     kVoiceEnabled = false;   // flip to true once wired

// Analog mic input: MAX9814 OUT → an ADC-capable pin (GP26/27/28 = ADC0/1/2).
static constexpr uint8_t  kMicAdcPin    = 26;      // ADC0

// I2S out to the TLV320DAC3100. BCLK and LRCLK/WS must be CONSECUTIVE GPIOs
// (WS = BCLK+1 automatically); DIN is independent. Keep these clear of the
// button pins above.
static constexpr uint8_t  kI2sBclkPin   = 16;      // → TLV320 BCLK (WS = GP17)
static constexpr uint8_t  kI2sDoutPin   = 18;      // → TLV320 DIN

// I2C control for the TLV320 (register init). Remapped OFF the earlephilhower
// default GP4/GP5 — those are two of the button pins.
static constexpr uint8_t  kDacSdaPin    = 20;
static constexpr uint8_t  kDacSclPin    = 21;
static constexpr int8_t   kDacResetPin  = 22;      // -1 if the board's RST is tied high
static constexpr uint8_t  kDacI2cAddr   = 0x18;    // TLV320DAC3100 default

static constexpr uint32_t kSampleRate   = 16000;   // voice band; low latency

// ── MAX7219 SPI bridge (optional; build with -DMAX_BRIDGE) ───────────────────
// Lets the CM5 drive MAX7219 LED-matrix panels THROUGH this coprocessor: the Pi
// ships already-formatted SPI bytes as "SPI <cs> <hex>", and we shift them out
// of hardware SPI1 and pulse the addressed CS/LOAD line. This runs the MAX
// panels alongside HUB75 with zero CM5 GPIO (piomatter's PIO ties those up).
// Host side: src/face/max7219_chain.* Transport::Coproc. Pins below are clear of
// the buttons (GP2-9) and the voice audio (GP16-22, GP26).
static constexpr uint8_t  kMaxSpiSck   = 10;          // SPI1 SCK → MAX7219 CLK
static constexpr uint8_t  kMaxSpiTx    = 11;          // SPI1 TX  → MAX7219 DIN
// CS/LOAD pins, indexed by the <cs> field of the SPI command (one per chain).
static constexpr uint8_t  kMaxCsPins[] = { 13 };
static constexpr uint32_t kMaxSpiHz    = 8000000;     // datasheet max ~10 MHz

// Optional LOCAL control so the changer works standalone (no Pi): a button id
// (index into kButtonPins) that toggles voice on a SHORT press / cycles the
// effect. -1 = disabled (control only over the serial protocol). These are
// handled on core0 IN ADDITION to being reported to the Pi.
static constexpr int8_t   kVoiceToggleBtn = -1;
static constexpr int8_t   kVoiceCycleBtn  = -1;
