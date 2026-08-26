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
static constexpr const char* kFwVersion = "1.8.0";

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

// Analog mic input: MAX9814 OUT → an ADC-capable pin.
//
// WHICH PIN depends on the chip, and the two are not interchangeable: the ADC
// block moves between RP2350 packages. On RP2350A (QFN60, Pico 2 / Pico 2 W)
// it is GP26-28; on RP2350B (QFN80, Pimoroni Pico Plus 2 / Pico LiPo 2 XL W)
// it is GP40-47, and GP26-28 are plain digital pins there. Wiring the mic to
// GP26 on an RP2350B fails silently — analogRead() just never sees it — so
// this follows the chip rather than making you remember.
#if NUM_BANK0_GPIOS >= 48
static constexpr uint8_t  kMicAdcPin    = 40;      // ADC0 on RP2350B
#else
static constexpr uint8_t  kMicAdcPin    = 26;      // ADC0 on RP2350A
#endif

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
// the boop config (snout/cheeks = 0-2; top-of-head / mouth-top / mouth-bottom
// = 3-5), and/or map extra pads to any GpioFunc via inputs.coprocessor.touch.
// -1 disables a slot.
// On RP2350B, pads 3-5 live on GP31-33 — free pins the QFN60 package doesn't
// have — so all six pads coexist with the voice changer. (NOT GP30: that's
// the BOOT button on the Pimoroni Pico LiPo 2 XL W.) On RP2350A they fall
// back to GP16/17/18, which share the OPTIONAL voice-changer I2S pins: with
// kVoiceEnabled/VOICE_CHANGER on there, wire at most pads 0-2.
#if NUM_BANK0_GPIOS >= 48
static constexpr int8_t  kTouchPins[6]    = { 0, 1, 12, 31, 32, 33 };   // RP2350B
#else
static constexpr int8_t  kTouchPins[6]    = { 0, 1, 12, 16, 17, 18 };   // RP2350A
#endif
static constexpr bool    kTouchActiveHigh = true;   // stock TTP223 = high on touch
static constexpr uint32_t kTouchDebounceMs = 30;

// Servo channels (planned RP2354B carrier feature: 8 channels; 4 here).
// DEDICATED pins — these no longer overlap anything. They used to be GP6-9,
// shared with buttons 4-7, so a slot converted from button to servo the first
// time the Pi commanded it and stayed a servo until reboot / PINCFG APPLY;
// moving a servo therefore cost half the buttons. GP34-36/38 are unclaimed on
// the Pico LiPo 2 XL W (header pins 26/27/29/31), plain digital+PWM with no
// onboard function and no hidden ties, so all 8 buttons now stay buttons.
// (Deliberately NOT GP27: it's A1 with a 1k link to GP41, which is in kAdcPins,
// so driving it would corrupt ADCREAD channel 1. GP23-25/29 and GP40-42 are not
// options at all — the first four go to the RM2 wireless module and the rest sit
// behind internal 1k ADC links, so none of them reach a header.)
// Wire signal to GP, servo V+ to an EXTERNAL 5-6 V supply (never the Pico's
// 3V3), grounds common.
// NOTE these four are now the FALLBACK backend only — see the PCA9685 block
// below, which takes over automatically whenever the board is fitted.
static constexpr int8_t  kServoPins[4]    = { 34, 35, 36, 38 };

// ── PCA9685 servo driver (16-channel I2C PWM) — the primary backend ──────────
// Generates all 16 servo channels in hardware and, critically, takes servo power
// on its OWN V+ terminal, separate from the Pico's logic rail. Driving servos
// from shared logic power is what made four of them unusable on the direct-GPIO
// setup; a dedicated rail behind the board's own bulk capacitor is the fix.
//
// It sits on the SAME I2C bus already used by the voice DAC (0x18) and the
// MPR121 boop pads (0x5A). 0x40 is the PCA9685's default and clashes with
// neither. A0-A5 solder jumpers shift it if a second board is ever added.
//
// WIRING: SDA/SCL to the pins below; VCC to 3V3 (logic only, tiny draw); V+ to
// an EXTERNAL 5-6 V supply sized for every servo STALLING at once (~1 A each for
// micro servos, so 4-6 A for a full board); GND common to both the Pico and that
// supply. The V+ terminal and the 3V3 VCC pin are separate for a reason — never
// bridge them.
//
// The firmware probes for the board at boot and on demand. If nothing answers it
// falls back to the four direct-GPIO channels above, so this build works both
// before and after the board is fitted, with no reflash needed either way.
static constexpr uint8_t  kPcaAddr        = 0x40;
static constexpr uint8_t  kPcaSdaPin      = 20;    // shared I2C0 (with the DAC)
static constexpr uint8_t  kPcaSclPin      = 21;
static constexpr uint16_t kPcaFreqHz      = 50;    // standard servo frame (20 ms)
static constexpr int      kServoChannels  = 16;    // PCA9685 outputs

// Digital addressable LED TEST zone (planned: 4 zones on the carrier) —
// a strip OR a custom panel (a panel is just a strip in serpentine order;
// the Pi does the 2-D mapping). Two supported LED families:
//   0 = WS2812/NeoPixel — 1 wire (data), strict 800 kHz timing (PIO handles it)
//   1 = APA102/DotStar  — 2 wires (data+clock), timing-free: immune to
//       interrupt jitter and refreshes fast enough for POV/spinning use
// Level-shift the data (and clock) line to 5 V for long or strict strips;
// short runs usually accept 3.3 V. Verbs: LEDZ (solid), LEDP (pattern),
// LEDB (brightness), LEDF/LEDSHOW (per-pixel frames from the Pi).
// Data moved off GP22 (that's the voice DAC reset) to free GP37. The APA102
// clock (GP28) doubles as ADC ch2 — with APA102 you lose that ADC channel.
static constexpr uint8_t  kLedZoneType    = 0;     // 0 = WS2812, 1 = APA102
static constexpr int8_t   kLedZonePin     = 37;    // data (GP37: free on the XL W)
static constexpr int8_t   kLedZoneClkPin  = 28;    // APA102 clock (WS2812: unused)
static constexpr uint16_t kLedZoneCount   = 16;    // strip/panel pixel count
static constexpr uint16_t kLedZoneMax     = 300;   // hard cap (frame buffer size)

// ADC TEST inputs (planned: flex sensors / pots / battery sense). Read on demand
// with "ADCREAD" -> "ADC <ch> <raw> <mv>" x3. ch0 doubles as the voice mic input
// (kMicAdcPin) - with voice enabled ch0 reads the mic.
//
// ADC0-2, following the chip's ADC block like kMicAdcPin above. On RP2350B this
// deliberately stops at GP42: GP43 is VBAT sense on the Pico LiPo 2 XL W.
#if NUM_BANK0_GPIOS >= 48
static constexpr uint8_t kAdcPins[3]      = { 40, 41, 42 };   // RP2350B
#else
static constexpr uint8_t kAdcPins[3]      = { 26, 27, 28 };   // RP2350A
#endif

// Hand-editing the pins above onto a non-ADC GP is the one mistake this file
// can't survive: analogRead() returns garbage rather than failing, so the mic
// goes quiet and the DSP happily processes silence. Fail the build instead.
#if NUM_BANK0_GPIOS >= 48
static_assert(kMicAdcPin >= 40 && kMicAdcPin <= 47,
              "RP2350B: the mic must be on GP40-47 (GP26-28 are not ADC here)");
static_assert(kAdcPins[0] >= 40 && kAdcPins[1] >= 40 && kAdcPins[2] >= 40,
              "RP2350B: kAdcPins must all be GP40-47");
#else
static_assert(kMicAdcPin >= 26 && kMicAdcPin <= 28,
              "RP2350A: the mic must be on GP26-28");
static_assert(kAdcPins[0] >= 26 && kAdcPins[1] >= 26 && kAdcPins[2] >= 26,
              "RP2350A: kAdcPins must all be GP26-28");
#endif

// Optional LOCAL control so the changer works standalone (no Pi): a button id
// (index into kButtonPins) that toggles voice on a SHORT press / cycles the
// effect. -1 = disabled (control only over the serial protocol). These are
// handled on core0 IN ADDITION to being reported to the Pi.
static constexpr int8_t   kVoiceToggleBtn = -1;
static constexpr int8_t   kVoiceCycleBtn  = -1;
