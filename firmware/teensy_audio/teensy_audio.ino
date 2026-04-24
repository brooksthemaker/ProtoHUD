// ─────────────────────────────────────────────────────────────────────────────
// teensy_audio.ino  —  ProtoHUD 6-channel ICS-43434 USB Audio bridge (fallback)
//
// PREFERRED: wire ICS-43434 mics directly to CM5 I2S GPIO using the
//            overlays/cm5-6mic.dts device tree overlay — lower latency,
//            no USB overhead, no extra MCU required.
//
// Use this sketch only if BCM2712 I2S does not support 3 simultaneous DIN lines.
// Set config.json audio.capture_device = "hw:CARD=TeensyAudio,DEV=0".
//
// Hardware: Teensy 4.1
//
// Mic wiring (3 × stereo I2S pairs)
//   Pair A  →  I2S1 (built-in)
//     SCK  = pin 21   WS = pin 20   DATA = pin 8
//     Left  slot : FRONT_L
//     Right slot : FRONT_R
//
//   Pair B  →  I2S1 quad (second data line on I2S1)
//     SCK/WS shared with Pair A    DATA = pin 6
//     Left  slot : SIDE_L
//     Right slot : SIDE_R
//
//   Pair C  →  I2S2
//     SCK  = pin 33   WS = pin 34   DATA = pin 5
//     Left  slot : REAR_L
//     Right slot : REAR_R
//
// The ICS-43434 L/R pin:
//   L/R = GND  → left  time-slot
//   L/R = VDD  → right time-slot
//
// USB Audio:  6-channel UAC1 at 48 kHz, 16-bit (requires usb_desc.h patch, see
//             README). CM5 sees it as "hw:CARD=TeensyAudio,DEV=0" with 6
//             capture channels.
//
// Build settings (Arduino IDE / Teensyduino):
//   Board   : Teensy 4.1
//   USB Type: Audio (or "Serial + Audio" for debug)
//   CPU     : 600 MHz
//   Optimize: Faster
// ─────────────────────────────────────────────────────────────────────────────

#include <Audio.h>
#include <Wire.h>
#include <SPI.h>

// ── Audio object graph ────────────────────────────────────────────────────────
//
//  I2S quad input (I2S1, 4 channels: FRONT_L/R + SIDE_L/R)
//  I2S2 input     (I2S2, 2 channels: REAR_L/R)
//  → USB audio output (6 channels)
//
// AudioInputI2SQuad  :  ch0=FRONT_L  ch1=FRONT_R  ch2=SIDE_L  ch3=SIDE_R
// AudioInputI2S2     :  ch0=REAR_L   ch1=REAR_R
// AudioOutputUSB     :  ch0..ch5  (6-channel UAC1)

AudioInputI2SQuad   mic_quad;    // Pair A + Pair B
AudioInputI2S2      mic_rear;    // Pair C

AudioOutputUSB      usb_out;

// Six direct patch cords: mic → USB channel
AudioConnection     pch0(mic_quad, 0, usb_out, 0);   // FRONT_L
AudioConnection     pch1(mic_quad, 1, usb_out, 1);   // FRONT_R
AudioConnection     pch2(mic_quad, 2, usb_out, 2);   // SIDE_L
AudioConnection     pch3(mic_quad, 3, usb_out, 3);   // SIDE_R
AudioConnection     pch4(mic_rear, 0, usb_out, 4);   // REAR_L
AudioConnection     pch5(mic_rear, 1, usb_out, 5);   // REAR_R

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
    // Reserve audio memory blocks.  128 blocks × 128 samples × 2 bytes = 32 KB.
    // 6 input channels + 6 USB output + routing margin = 20 blocks minimum.
    AudioMemory(64);

    // Optional: print memory stats over Serial when USB type is "Serial + Audio"
#ifdef ARDUINO_TEENSY41
    Serial.begin(115200);
    // Brief delay so host can open serial monitor before first prints
    delay(500);
    Serial.println("[teensy_audio] ProtoHUD 6-ch mic array started");
    Serial.print("[teensy_audio] Audio block size: ");
    Serial.print(AUDIO_BLOCK_SAMPLES);
    Serial.println(" samples");
    Serial.print("[teensy_audio] Sample rate: ");
    Serial.print(AUDIO_SAMPLE_RATE);
    Serial.println(" Hz");
#endif
}

// ── Loop ──────────────────────────────────────────────────────────────────────
// Nothing to do in the main loop — the Teensy Audio library handles all DMA
// transfers and USB isochronous packets in interrupt context.
// We periodically report memory usage over Serial for debugging.

static uint32_t last_report_ms = 0;

void loop() {
#ifdef ARDUINO_TEENSY41
    uint32_t now = millis();
    if (now - last_report_ms >= 5000) {
        last_report_ms = now;
        Serial.print("[teensy_audio] mem max=");
        Serial.print(AudioMemoryUsageMax());
        Serial.print(" cpu max=");
        Serial.print(AudioProcessorUsageMax(), 1);
        Serial.println("%");
        AudioMemoryUsageMaxReset();
        AudioProcessorUsageMaxReset();
    }
#endif
}
