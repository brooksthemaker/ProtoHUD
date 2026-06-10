#pragma once
// ── config.h ─────────────────────────────────────────────────────────────────
// Pin map + timing for the ProtoHUD button coprocessor firmware. Edit this file
// to match your wiring; nothing else needs changing for a basic build.
//
// The button id reported to the Pi is the INDEX in kButtonPins (0..N-1). The Pi
// maps id → GpioFunc in inputs.coprocessor.buttons, so the ORDER here is your id
// assignment — keep it stable once you've configured the HUD.

#include <Arduino.h>

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
