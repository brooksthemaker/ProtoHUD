# Accessory-LED diagnostics

`wire_order_check.cpp` — headless test of the chain maths behind
**Accessory LEDs > Layout & Sides > Wiring Order**: permutation normalisation
(`normalize_wire_order`) and the start-index chaining (`chain_zones`).

Worth re-running after any change to zone chaining, because the properties it
checks are the ones that decide which physical LEDs each area owns:

- every zone appears exactly once, even from a malformed/hand-edited order
- no two areas claim the same LED
- every LED on the strip belongs to an area
- an empty area consumes no chain space but keeps a valid start

```sh
g++ -std=c++17 -Wall -Wextra -Isrc \
    scripts/led_diag/wire_order_check.cpp \
    src/accessory/accessory_leds.cpp src/accessory/led_strip.cpp \
    -o /tmp/wire_order_check && /tmp/wire_order_check
```
