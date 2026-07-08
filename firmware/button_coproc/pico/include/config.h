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
static constexpr const char* kFwVersion = "1.3.0";

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

// ── Peripheral hub (optional; build with -DPERIPHERAL_HUB) ───────────────────
// The coprocessor as the helmet's peripheral board: boop pads, temperature
// probes and fan PWM move here so the CM5's 40-pin header keeps only the HUB75
// bonnet + IMU. Events go up the same USB link ("BOOP <e> <0|1>", "TEMP <rom>
// <milli°C>"); the Pi drives fans with "FAN <zone> <duty%>". Pins below stay
// clear of the buttons (GP2-9), MAX bridge (GP10/11/13) and voice (GP16-18,
// GP20/21, GP22 reset, GP26).
static constexpr uint8_t  kMpr121Addr    = 0x5A;      // boop pads, on the shared
                                                      // I2C0 bus (GP20/21, w/ DAC)
static constexpr uint8_t  kOneWirePin    = 19;        // DS18B20 bus (4.7k → 3V3)
static constexpr uint8_t  kFanPins[]     = { 14, 15 };// PWM fan zones (25 kHz)
static constexpr uint32_t kBoopPollMs    = 33;        // MPR121 touch poll (~30 Hz)
static constexpr uint32_t kTempPeriodMs  = 2000;      // convert+read cycle
static constexpr int      kMaxOwDevices  = 8;         // probes on the 1-Wire bus

// ── Pre-assigned TEST pins (planned-feature bring-up) ────────────────────────
// Touch pads, servos, an addressable-LED zone and the ADC inputs get fixed
// test pins so the planned peripherals can be wired and exercised NOW (menu:
// GPIO > RP2350 GPIO Expander > Peripheral Test). All of these coexist with
// the defaults above; the sharing rules are spelled out per block.

// TTP223 capacitive touch pads (boop sensors), up to 6. Each module's OUT pin
// goes to the listed GP; VCC=3V3, GND=GND. Stock TTP223 boards are ACTIVE-HIGH
// momentary (solder-jumper variants can invert/latch — set the polarity flag
// to match). Touch edges stream up as "BOOP <idx> <1|0>", so the Pi maps them
// exactly like MPR121 electrodes: give the boop zones these INDICES (0-5) in
// the boop config, and/or map extra pads to any GpioFunc via
// inputs.coprocessor.touch. -1 disables a slot.
// NOTE: pads 3-5 (GP16/17/18) share the OPTIONAL voice-changer I2S pins —
// with kVoiceEnabled/VOICE_CHANGER on, wire at most pads 0-2.
static constexpr int8_t  kTouchPins[6]    = { 0, 1, 12, 16, 17, 18 };
static constexpr bool    kTouchActiveHigh = true;   // stock TTP223 = high on touch
static constexpr uint32_t kTouchDebounceMs = 30;

// Servo TEST channels (planned RP2354B carrier feature: 8 channels; 4 here).
// SHARED with buttons 4-7 (GP6-9): a slot converts from button to servo the
// FIRST time the Pi commands it ("SERVO <ch> <deg>"), and stays a servo until
// reboot / PINCFG APPLY. Wire signal to GP, servo V+ to an EXTERNAL 5-6 V
// supply (never the Pico's 3V3), grounds common.
static constexpr int8_t  kServoPins[4]    = { 6, 7, 8, 9 };

// Digital addressable LED TEST zone (planned: 4 zones on the carrier) —
// a strip OR a custom panel (a panel is just a strip in serpentine order;
// the Pi does the 2-D mapping). Two supported LED families:
//   0 = WS2812/NeoPixel — 1 wire (data), strict 800 kHz timing (PIO handles it)
//   1 = APA102/DotStar  — 2 wires (data+clock), timing-free: immune to
//       interrupt jitter and refreshes fast enough for POV/spinning use
// Level-shift the data (and clock) line to 5 V for long or strict strips;
// short runs usually accept 3.3 V. Verbs: LEDZ (solid), LEDP (pattern),
// LEDB (brightness), LEDF/LEDSHOW (per-pixel frames from the Pi).
// GP22 doubles as the voice DAC reset; the APA102 clock (GP28) doubles as
// ADC ch2 — with voice enabled move/skip, with APA102 you lose that ADC ch.
static constexpr uint8_t  kLedZoneType    = 0;     // 0 = WS2812, 1 = APA102
static constexpr int8_t   kLedZonePin     = 22;    // data
static constexpr int8_t   kLedZoneClkPin  = 28;    // APA102 clock (WS2812: unused)
static constexpr uint16_t kLedZoneCount   = 16;    // strip/panel pixel count
static constexpr uint16_t kLedZoneMax     = 300;   // hard cap (frame buffer size)

// ADC TEST inputs (planned: flex sensors / pots / battery sense). GP26-28 =
// ADC0-2; read on demand with "ADCREAD" -> "ADC <ch> <raw> <mv>" x3.
// GP26 doubles as the voice mic input - with voice enabled ch0 reads the mic.
static constexpr uint8_t kAdcPins[3]      = { 26, 27, 28 };

// Optional LOCAL control so the changer works standalone (no Pi): a button id
// (index into kButtonPins) that toggles voice on a SHORT press / cycles the
// effect. -1 = disabled (control only over the serial protocol). These are
// handled on core0 IN ADDITION to being reported to the Pi.
static constexpr int8_t   kVoiceToggleBtn = -1;
static constexpr int8_t   kVoiceCycleBtn  = -1;
