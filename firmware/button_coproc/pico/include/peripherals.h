#pragma once
// ── peripherals.h ─────────────────────────────────────────────────────────────
// Optional peripheral hub for the ProtoHUD coprocessor (build with
// -DPERIPHERAL_HUB): boop pads (MPR121 on the shared I2C0 bus), DS18B20
// temperature probes (bit-banged 1-Wire, no library), and PWM fan zones — so
// the CM5's GPIO header keeps only the HUB75 bonnet + IMU.
//
// core0 only, cooperative: periph_service() does at most one short piece of
// work per call (an MPR121 poll, or ONE probe read ≈7 ms) so button debounce
// never stalls longer than a bit-bang transaction.
//
//   periph_setup()            once from setup()
//   periph_service()          every loop() pass
//   periph_handle_command()   from handle_line(); consumes "FAN <zone> <duty%>"
//
// Emits over USB serial:  BOOP <electrode> <1|0>   (touch edges)
//                         TEMP <rom16hex> <milli°C> (per probe, each cycle)

#include <Arduino.h>

void periph_setup();
void periph_service();
bool periph_handle_command(const String& line);
