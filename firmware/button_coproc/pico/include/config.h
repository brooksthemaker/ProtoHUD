#pragma once
// ── config.h ─────────────────────────────────────────────────────────────────
// Pin map + timing for the ProtoHUD "proto-coproc" firmware (RP2350 / Pico 2 W).
// Edit this file to match your wiring; nothing else needs changing for a build.
//
// The companion does three jobs (each independently toggleable here):
//   • buttons — debounce + short/long, reported as button INDEX in kButtonPins.
//   • sensors — it is the I²C master for the boop/IMU/light chips and streams
//               DECODED values to the CM5 (aggregator). The CM5 applies
//               declination / axis-remap / boop coalescing / squint logic.
//   • panels  — (phase 3) drive up to two MAX7219 chains from 1bpp frames the
//               CM5 pushes. Pins are reserved here now; the driver lands later.
//
// The button id reported to the Pi is the INDEX in kButtonPins (0..N-1). The Pi
// maps id → GpioFunc in inputs.coprocessor.buttons, so the ORDER here is your id
// assignment — keep it stable once you've configured the HUD.

#include <Arduino.h>

// ── Buttons ─────────────────────────────────────────────────────────────────
// Switches wired between the listed GP pin and GND. INPUT_PULLUP → pressed = LOW.
// (Trimmed to 4 so GP6/GP7 are free for the I²C sensor bus below; add more on
//  any spare GP pins that don't clash with I²C/SPI.)
static constexpr uint8_t kButtonPins[] = { 2, 3, 4, 5 };
// Optional per-button LED / switch-backlight pins, parallel to kButtonPins.
// -1 = none. Driven by the Pi via CMD_LED.
static constexpr int8_t  kLedPins[]    = { -1, -1, -1, -1 };
static_assert(sizeof(kButtonPins) == sizeof(kLedPins),
              "kButtonPins and kLedPins must have the same number of entries");

static constexpr uint32_t kDebounceMs = 15;    // contact-bounce reject window
static constexpr uint32_t kLongMsInit = 600;   // initial short/long threshold (CFG-tunable)
static constexpr uint32_t kPingMs     = 1000;  // ASCII heartbeat period (~1 Hz)
static constexpr bool     kEmitDownUp = false; // advisory raw edges (Pi ignores)

// ── Sensor I²C bus ───────────────────────────────────────────────────────────
// All sensors share one I²C bus on the RP2350's second controller (Wire1). Pick
// a valid I²C1 SDA/SCL pin pair for your board (defaults below are safe on the
// Pico 2 W and clear of the button pins above). The CM5 is NOT on this bus.
static constexpr bool     kSensorsEnabled = true;
static constexpr uint8_t  kI2cSdaPin      = 6;    // Wire1 SDA (GP6)
static constexpr uint8_t  kI2cSclPin      = 7;    // Wire1 SCL (GP7)
static constexpr uint32_t kI2cHz          = 400000;

// BNO055 (on-chip 9-DOF fusion) — preferred heading source.
static constexpr bool     kBnoEnabled = true;
static constexpr uint8_t  kBnoAddr    = 0x28;     // or 0x29 (ADR high)
static constexpr uint32_t kBnoPollMs  = 20;       // 50 Hz

// MPU9250 + AK8963 (backup compass).
static constexpr bool     kMpuEnabled = true;
static constexpr uint8_t  kMpuAddr    = 0x68;     // or 0x69 (AD0 high)
static constexpr uint8_t  kAkAddr     = 0x0C;
static constexpr uint32_t kMpuPollMs  = 20;       // 50 Hz

// MPR121 capacitive boop. The Pico reports RAW per-electrode edges; the CM5
// owns the BothCheeks coalesce window + per-zone expression/refractory.
static constexpr bool     kBoopEnabled = true;
static constexpr uint8_t  kBoopAddr    = 0x5A;    // or 0x5B/0x5C/0x5D
static constexpr uint32_t kBoopPollMs  = 33;      // ~30 Hz
// Electrodes for snout / left cheek / right cheek (zones 0/1/2). -1 = unused.
static constexpr int8_t   kBoopElectrode[3] = { 0, 1, 2 };
static constexpr uint8_t  kBoopTouchTh[3]   = { 12, 12, 12 };
static constexpr uint8_t  kBoopReleaseTh[3] = { 6, 6, 6 };

// BH1750 ambient light. The CM5 runs the dark→bright squint edge detector.
static constexpr bool     kLightEnabled = true;
static constexpr uint8_t  kLightAddr    = 0x23;   // or 0x5C (ADDR high)
static constexpr uint32_t kLightPollMs  = 125;    // 8 Hz

// ── MAX7219 panel chains (phase 3 — pins reserved, driver lands later) ────────
// Two independent chains preferred (e.g. left eye on SPI0, right eye on SPI1).
static constexpr bool     kPanelsEnabled = false;
static constexpr uint8_t  kPanelChains   = 2;
struct PanelChainPins { uint8_t spi_index; int8_t sck; int8_t mosi; int8_t cs; uint8_t modules; };
static constexpr PanelChainPins kPanelChain[kPanelChains] = {
    { 0, 18, 19, 17, 4 },   // chain 0 — SPI0 (GP18 SCK / GP19 MOSI / GP17 CS)
    { 1, 10, 11, 13, 4 },   // chain 1 — SPI1 (GP10 SCK / GP11 MOSI / GP13 CS)
};
