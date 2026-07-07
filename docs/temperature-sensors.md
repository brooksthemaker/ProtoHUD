# Temperature sensors

Read temperature probes into the HUD/menu and (optionally) drive the cooling
fans off them. v1 supports **DS18B20** 1-Wire probes — many share one GPIO, each
has a unique id, and reading one is a plain sysfs file read. Disabled unless
`temperature.enabled` is true.

## Wiring (DS18B20, 1-Wire)

- **Data** → a spare CM5 GPIO clear of the HUB75 bonnet — **BCM 25** works.
- **VDD** → 3.3V, **GND** → GND (parasitic power is possible but 3-wire is more
  reliable).
- One **4.7 kΩ pull-up** from data to 3.3V for the whole bus (not per probe).
- Multiple probes just tap the same three lines.

Enable the kernel 1-Wire driver in `/boot/firmware/config.txt`:

```
dtoverlay=w1-gpio,gpiopin=25
```

Reboot, then list the probes to get their ids:

```bash
ls /sys/bus/w1/devices/      # each DS18B20 shows as 28-XXXXXXXXXXXX
cat /sys/bus/w1/devices/28-XXXX/temperature   # milli-°C, e.g. 23456 = 23.456 °C
```

## Configure

`config.json` → `temperature` (see `config/config.example.json`):

```jsonc
"temperature": {
  "enabled": true,
  "poll_ms": 1000,
  "sensors": [
    { "type": "ds18b20", "id": "28-0000000000aa", "label": "Snout",     "warn_c": 40, "crit_c": 50 },
    { "type": "ds18b20", "id": "28-0000000000bb", "label": "Interior",  "warn_c": 42, "crit_c": 52 },
    { "type": "ds18b20", "id": "28-0000000000cc", "label": "Electronics","warn_c": 60, "crit_c": 75 }
  ]
}
```

- `id: ""` uses the first probe found (fine for a single sensor).
- Readings appear live in **System → Temperature**; rows turn amber/red at the
  `warn_c` / `crit_c` thresholds.

## Drive the fans off a probe

`FanController` ramps a zone from any milli-°C sysfs file (`fans.temp_path`,
default the SoC temp). To ramp off a helmet probe instead, point it at that
probe's file — same format, no code change:

```jsonc
"fans": { "temp_path": "/sys/bus/w1/devices/28-0000000000bb/temperature", ... }
```

## Other sensor types

The driver dispatches on `type`, so **MAX31865 (PT100/PT1000, via the kernel IIO
driver)** and **I2C chips (MCP9808/TMP117)** can be added as `type: "max31865"` /
`type: "i2c"` later and listed alongside the DS18B20 probes — same HUD, menu, and
fan hook.
