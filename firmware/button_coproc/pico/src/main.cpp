// ── ProtoHUD proto-coproc — RP2350 / Raspberry Pi Pico 2 W ───────────────────
// One companion MCU that offloads three jobs from the CM5 (see config.h):
//   1. buttons — debounce + short/long classification.
//   2. sensors — I²C master for the boop/IMU/light chips; streams DECODED values
//                to the CM5 (aggregator). The CM5 applies declination / axis
//                remap / boop coalescing / squint logic, so we send RAW fused /
//                native values + a millis() timestamp only.
//   3. panels  — (phase 3) drive MAX7219 chains from 1bpp frames. Not yet here.
//
// Wire format: shared header firmware/proto_link/coproc_proto.h. HELLO + PING are
// ASCII (board detection + heartbeat); telemetry + control are binary frames.
// The CM5 is "smart about meaning"; this firmware is "smart about timing".
//
// UNTESTED ON HARDWARE in this commit — register sequences are ported 1:1 from
// the CM5 drivers (src/sensor/*.cpp); verify on a bench before relying on it.

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

#include "config.h"
#include "../../../proto_link/coproc_proto.h"

namespace cp = coproc_proto;

// ── TX ───────────────────────────────────────────────────────────────────────
static void send_frame(uint8_t cmd, const void* payload, uint16_t len) {
    static uint8_t out[cp::WIRE_HEADER + cp::MAX_PAYLOAD + cp::CRC_LEN];
    if (len > cp::MAX_PAYLOAD) return;
    const size_t n = cp::encode(out, cmd, payload, len);
    Serial.write(out, n);
}

// ── I²C primitives (sensor bus = Wire1) ──────────────────────────────────────
static bool i2c_wr(uint8_t addr, uint8_t reg, uint8_t val) {
    Wire1.beginTransmission(addr);
    Wire1.write(reg);
    Wire1.write(val);
    return Wire1.endTransmission() == 0;
}
static bool i2c_rd(uint8_t addr, uint8_t reg, uint8_t* buf, size_t len) {
    Wire1.beginTransmission(addr);
    Wire1.write(reg);
    if (Wire1.endTransmission(false) != 0) return false;     // repeated start
    const size_t got = Wire1.requestFrom(addr, static_cast<uint8_t>(len));
    for (size_t i = 0; i < len && Wire1.available(); ++i) buf[i] = Wire1.read();
    return got == len;
}
static bool i2c_cmd(uint8_t addr, uint8_t c) {               // BH1750 has no regs
    Wire1.beginTransmission(addr);
    Wire1.write(c);
    return Wire1.endTransmission() == 0;
}
static bool i2c_rdraw(uint8_t addr, uint8_t* buf, size_t len) {
    const size_t got = Wire1.requestFrom(addr, static_cast<uint8_t>(len));
    for (size_t i = 0; i < len && Wire1.available(); ++i) buf[i] = Wire1.read();
    return got == len;
}

// ════════════════════════════════════════════════════════════════════════════
// Buttons
// ════════════════════════════════════════════════════════════════════════════
namespace btn {
constexpr size_t N = sizeof(kButtonPins) / sizeof(kButtonPins[0]);
uint32_t g_long_ms = kLongMsInit;
struct B { bool raw = true, stable = true; uint32_t edge = 0, down = 0; bool long_fired = false; };
B g[N];

void setup() {
    for (size_t i = 0; i < N; ++i) {
        pinMode(kButtonPins[i], INPUT_PULLUP);
        if (kLedPins[i] >= 0) { pinMode(kLedPins[i], OUTPUT); digitalWrite(kLedPins[i], LOW); }
    }
}
void emit(uint8_t id, uint8_t kind) {
    cp::BtnPayload p{ millis(), id, kind };
    send_frame(cp::RSP_BTN, &p, sizeof p);
}
void poll(uint32_t now) {
    for (size_t i = 0; i < N; ++i) {
        B& b = g[i];
        const bool raw = digitalRead(kButtonPins[i]) == HIGH;   // HIGH = released
        if (raw != b.raw) { b.raw = raw; b.edge = now; }
        if ((now - b.edge) < kDebounceMs) continue;
        if (raw != b.stable) {
            b.stable = raw;
            if (!raw) { b.down = now; b.long_fired = false; }
            else if (!b.long_fired) emit(static_cast<uint8_t>(i), cp::BTN_SHORT);
            continue;
        }
        if (!b.stable && !b.long_fired && (now - b.down) >= g_long_ms) {
            b.long_fired = true;
            emit(static_cast<uint8_t>(i), cp::BTN_LONG);
        }
    }
}
} // namespace btn

// ════════════════════════════════════════════════════════════════════════════
// BNO055 — on-chip 9-DOF fusion (ported from src/sensor/bno055.cpp)
// ════════════════════════════════════════════════════════════════════════════
namespace bno {
constexpr uint8_t R_CHIP_ID = 0x00, R_ACC = 0x08, R_MAG = 0x0E, R_GYR = 0x14,
                  R_EUL = 0x1A, R_CALIB = 0x35, R_UNIT = 0x3B, R_OPR = 0x3D,
                  R_PWR = 0x3E, R_SYS_TRIG = 0x3F, R_PAGE = 0x07;
constexpr uint8_t CHIP_ID = 0xA0, OPR_CONFIG = 0x00, OPR_NDOF = 0x0C;
bool ok = false;

bool init() {
    uint8_t id = 0;
    if (!i2c_rd(kBnoAddr, R_CHIP_ID, &id, 1) || id != CHIP_ID) return false;
    i2c_wr(kBnoAddr, R_OPR, OPR_CONFIG);   delay(25);
    i2c_wr(kBnoAddr, R_SYS_TRIG, 0x20);    delay(700);   // soft reset (≥650 ms)
    if (!i2c_rd(kBnoAddr, R_CHIP_ID, &id, 1) || id != CHIP_ID) return false;
    i2c_wr(kBnoAddr, R_PWR, 0x00);         // normal power
    i2c_wr(kBnoAddr, R_PAGE, 0x00);
    i2c_wr(kBnoAddr, R_SYS_TRIG, 0x00);
    i2c_wr(kBnoAddr, R_UNIT, 0x00);        // m/s², deg, deg/s, °C
    i2c_wr(kBnoAddr, R_OPR, OPR_NDOF);     delay(25);
    return true;
}
void poll() {
    if (!ok) return;
    uint8_t e[6], a[6], g[6], m[6], c = 0;
    if (!i2c_rd(kBnoAddr, R_EUL, e, 6)) return;
    i2c_rd(kBnoAddr, R_ACC, a, 6);
    i2c_rd(kBnoAddr, R_GYR, g, 6);
    i2c_rd(kBnoAddr, R_MAG, m, 6);
    i2c_rd(kBnoAddr, R_CALIB, &c, 1);
    auto s16 = [](const uint8_t* p){ return (int16_t)((p[1] << 8) | p[0]); };
    cp::BnoPayload p{};
    p.t_ms = millis();
    p.euler[0] = s16(e + 0) / 16.0f;   // heading
    p.euler[1] = s16(e + 2) / 16.0f;   // roll
    p.euler[2] = s16(e + 4) / 16.0f;   // pitch
    for (int i = 0; i < 3; ++i) {
        p.accel_g[i] = (s16(a + 2 * i) / 100.0f) / 9.80665f;  // m/s² → g
        p.gyro_dps[i] = s16(g + 2 * i) / 16.0f;
        p.mag_ut[i]   = s16(m + 2 * i) / 16.0f;
    }
    p.calib_sys = (c >> 6) & 3; p.calib_gyro = (c >> 4) & 3;
    p.calib_accel = (c >> 2) & 3; p.calib_mag = c & 3;
    send_frame(cp::RSP_IMU_BNO, &p, sizeof p);
}
} // namespace bno

// ════════════════════════════════════════════════════════════════════════════
// MPU9250 + AK8963 — tilt-compensated heading (ported from src/sensor/mpu9250.cpp)
// ════════════════════════════════════════════════════════════════════════════
namespace mpu {
constexpr uint8_t SMPLRT = 0x19, CONFIG = 0x1A, GYRO_CFG = 0x1B, ACC_CFG = 0x1C,
                  INT_PIN = 0x37, ACC_XOUT = 0x3B, TEMP = 0x41, GYR_XOUT = 0x43,
                  PWR1 = 0x6B, WHO = 0x75;
constexpr uint8_t AK_ST1 = 0x02, AK_HXL = 0x03, AK_CNTL1 = 0x0A, AK_ASAX = 0x10;
bool  ok = false;
float adj[3] = { 1, 1, 1 };

bool init() {
    uint8_t who = 0;
    if (!i2c_rd(kMpuAddr, WHO, &who, 1)) return false;      // accept clones
    i2c_wr(kMpuAddr, PWR1, 0x80); delay(150);               // reset
    i2c_wr(kMpuAddr, PWR1, 0x01); delay(10);                // wake, PLL
    i2c_wr(kMpuAddr, CONFIG, 0x01);                         // DLPF ~184 Hz
    i2c_wr(kMpuAddr, SMPLRT, 0x09);                         // 100 Hz
    i2c_wr(kMpuAddr, GYRO_CFG, 0x00);                       // ±250 °/s
    i2c_wr(kMpuAddr, ACC_CFG, 0x00);                        // ±2 g
    i2c_wr(kMpuAddr, INT_PIN, 0x02); delay(10);             // I²C bypass → AK8963
    uint8_t akwho = 0;
    if (!i2c_rd(kAkAddr, 0x00, &akwho, 1) || akwho != 0x48) return false;
    i2c_wr(kAkAddr, AK_CNTL1, 0x00); delay(15);             // power down
    i2c_wr(kAkAddr, AK_CNTL1, 0x0F); delay(15);             // fuse ROM
    uint8_t asa[3] = {};
    i2c_rd(kAkAddr, AK_ASAX, asa, 3);
    for (int i = 0; i < 3; ++i) adj[i] = (asa[i] - 128.0f) / 256.0f + 1.0f;
    i2c_wr(kAkAddr, AK_CNTL1, 0x00); delay(15);
    i2c_wr(kAkAddr, AK_CNTL1, 0x16); delay(10);             // 16-bit, 100 Hz cont
    return true;
}
// Tilt-compensated heading for the standard face-up mount (axes preset 0). The
// CM5 adds declination + offset; hard-iron calibration is a follow-up (sent 0).
float heading(float mx, float my, float mz, int16_t ax, int16_t ay, int16_t az) {
    float axf = ax / 16384.0f, ayf = ay / 16384.0f, azf = az / 16384.0f;
    const float norm = sqrtf(axf * axf + ayf * ayf + azf * azf);
    float h;
    if (norm >= 0.1f) {
        axf /= norm; ayf /= norm; azf /= norm;
        const float pitch = asinf(-axf), roll = atan2f(ayf, azf);
        const float cp_ = cosf(pitch), sp = sinf(pitch), cr = cosf(roll), sr = sinf(roll);
        const float mxh = mx * cp_ + mz * sp;
        const float myh = mx * sr * sp + my * cr - mz * sr * cp_;
        h = atan2f(-myh, mxh) * (180.0f / (float)M_PI);
    } else {
        h = atan2f(-my, mx) * (180.0f / (float)M_PI);
    }
    if (h < 0.f) h += 360.f;
    if (h >= 360.f) h -= 360.f;
    return h;
}
void poll() {
    if (!ok) return;
    uint8_t st1 = 0;
    if (!i2c_rd(kAkAddr, AK_ST1, &st1, 1) || !(st1 & 0x01)) return;   // mag ready?
    uint8_t r[7] = {};
    if (!i2c_rd(kAkAddr, AK_HXL, r, 7)) return;
    if (r[6] & 0x08) return;                                          // overflow
    const int16_t rx = (int16_t)((r[1] << 8) | r[0]);
    const int16_t ry = (int16_t)((r[3] << 8) | r[2]);
    const int16_t rz = (int16_t)((r[5] << 8) | r[4]);
    const float mx = rx * adj[0], my = ry * adj[1], mz = rz * adj[2];
    uint8_t a[6] = {};
    i2c_rd(kMpuAddr, ACC_XOUT, a, 6);
    const int16_t ax = (int16_t)((a[0] << 8) | a[1]);
    const int16_t ay = (int16_t)((a[2] << 8) | a[3]);
    const int16_t az = (int16_t)((a[4] << 8) | a[5]);
    cp::MpuPayload p{};
    p.t_ms = millis();
    p.heading_deg = heading(mx, my, mz, ax, ay, az);
    p.accel_g[0] = ax / 16384.0f; p.accel_g[1] = ay / 16384.0f; p.accel_g[2] = az / 16384.0f;
    uint8_t g[6] = {};
    if (i2c_rd(kMpuAddr, GYR_XOUT, g, 6)) {
        p.gyro_dps[0] = (int16_t)((g[0] << 8) | g[1]) / 131.0f;
        p.gyro_dps[1] = (int16_t)((g[2] << 8) | g[3]) / 131.0f;
        p.gyro_dps[2] = (int16_t)((g[4] << 8) | g[5]) / 131.0f;
    }
    p.mag_ut[0] = mx * 0.15f; p.mag_ut[1] = my * 0.15f; p.mag_ut[2] = mz * 0.15f;
    uint8_t t[2] = {};
    if (i2c_rd(kMpuAddr, TEMP, t, 2))
        p.temp_c = (int16_t)((t[0] << 8) | t[1]) / 333.87f + 21.0f;
    send_frame(cp::RSP_IMU_MPU, &p, sizeof p);
}
} // namespace mpu

// ════════════════════════════════════════════════════════════════════════════
// MPR121 capacitive boop — RAW per-electrode edges (ported from mpr121_*.cpp)
// ════════════════════════════════════════════════════════════════════════════
namespace boop {
constexpr uint8_t R_TOUCH = 0x00, R_MHD_R = 0x2B, R_TH_E0 = 0x41, R_REL_E0 = 0x42,
                  R_DEBOUNCE = 0x5B, R_CFG1 = 0x5C, R_CFG2 = 0x5D, R_ECR = 0x5E,
                  R_RESET = 0x80;
bool ok = false;
bool last_touch[3] = { false, false, false };

bool init() {
    if (!i2c_wr(kBoopAddr, R_RESET, 0x63)) return false; delay(5);
    i2c_wr(kBoopAddr, R_ECR, 0x00);                       // stop to configure
    // Baseline filters (MPR121 cookbook).
    i2c_wr(kBoopAddr, R_MHD_R + 0, 0x01); i2c_wr(kBoopAddr, R_MHD_R + 1, 0x01);
    i2c_wr(kBoopAddr, R_MHD_R + 2, 0x0E); i2c_wr(kBoopAddr, R_MHD_R + 3, 0x00);
    i2c_wr(kBoopAddr, R_MHD_R + 4, 0x01); i2c_wr(kBoopAddr, R_MHD_R + 5, 0x05);
    i2c_wr(kBoopAddr, R_MHD_R + 6, 0x01); i2c_wr(kBoopAddr, R_MHD_R + 7, 0x00);
    for (int z = 0; z < 3; ++z) {
        const int8_t e = kBoopElectrode[z];
        if (e < 0 || e > 11) continue;
        i2c_wr(kBoopAddr, R_TH_E0 + 2 * e, kBoopTouchTh[z]);
        i2c_wr(kBoopAddr, R_REL_E0 + 2 * e, kBoopReleaseTh[z]);
    }
    i2c_wr(kBoopAddr, R_DEBOUNCE, (2 << 4) | 2);
    i2c_wr(kBoopAddr, R_CFG1, 0x10);
    i2c_wr(kBoopAddr, R_CFG2, 0x20);
    return i2c_wr(kBoopAddr, R_ECR, 0x8F);               // run, 12 electrodes
}
void poll() {
    if (!ok) return;
    uint8_t st[2] = {};
    if (!i2c_rd(kBoopAddr, R_TOUCH, st, 2)) return;
    const uint16_t touched = (uint16_t)st[0] | ((uint16_t)(st[1] & 0x1F) << 8);
    for (int z = 0; z < 3; ++z) {
        const int8_t e = kBoopElectrode[z];
        if (e < 0 || e > 11) continue;
        const bool now = (touched >> e) & 1u;
        if (now != last_touch[z]) {
            last_touch[z] = now;
            cp::BoopPayload p{ millis(), (uint8_t)z, (uint8_t)(now ? cp::BOOP_PRESS : cp::BOOP_RELEASE) };
            send_frame(cp::RSP_BOOP, &p, sizeof p);
        }
    }
}
} // namespace boop

// ════════════════════════════════════════════════════════════════════════════
// BH1750 ambient light (ported from src/sensor/light_sensor.cpp)
// ════════════════════════════════════════════════════════════════════════════
namespace light {
bool ok = false;
bool init() {
    if (!i2c_cmd(kLightAddr, 0x01)) return false;   // power on
    i2c_cmd(kLightAddr, 0x07);                       // reset
    i2c_cmd(kLightAddr, 0x10);                       // continuous hi-res
    delay(150);
    return true;
}
void poll() {
    if (!ok) return;
    uint8_t b[2] = {};
    if (!i2c_rdraw(kLightAddr, b, 2)) return;
    const uint16_t raw = ((uint16_t)b[0] << 8) | b[1];
    cp::LightPayload p{ millis(), raw / 1.2f };
    send_frame(cp::RSP_LIGHT, &p, sizeof p);
}
} // namespace light

// ── Inbound (CM5 → Pico): binary CMD_* frames + ASCII (PONG ignored) ──────────
namespace rx {
uint8_t frame[cp::WIRE_HEADER + cp::MAX_PAYLOAD + cp::CRC_LEN];
size_t  flen = 0;
bool    in_frame = false;
String  line;

void on_frame(uint8_t cmd, const uint8_t* p, uint16_t len) {
    if (cmd == cp::CMD_CFG && len >= sizeof(cp::CfgPayload)) {
        cp::CfgPayload c; memcpy(&c, p, sizeof c);
        if (c.long_ms >= 100 && c.long_ms <= 5000) btn::g_long_ms = c.long_ms;
        // Boop threshold push could re-write MPR121 regs here (future).
    } else if (cmd == cp::CMD_LED && len >= sizeof(cp::LedPayload)) {
        cp::LedPayload l; memcpy(&l, p, sizeof l);
        if (l.id < (sizeof(kButtonPins)/sizeof(kButtonPins[0])) && kLedPins[l.id] >= 0)
            digitalWrite(kLedPins[l.id], l.on ? HIGH : LOW);
    }
    // CMD_PANEL_* handled in phase 3.
}
void byte_in(uint8_t c) {
    if (in_frame) {
        frame[flen++] = c;
        if (flen == 2 && frame[1] != cp::MAGIC1) { in_frame = false; flen = 0; return; }
        if (flen >= cp::WIRE_HEADER) {
            const uint16_t len = frame[3] | (frame[4] << 8);
            if (len > cp::MAX_PAYLOAD) { in_frame = false; flen = 0; return; }
            if (flen == (size_t)(cp::WIRE_HEADER + len + cp::CRC_LEN)) {
                const uint16_t want = cp::crc16(frame + 2, 3 + len);
                const uint16_t got  = frame[cp::WIRE_HEADER + len] |
                                      (frame[cp::WIRE_HEADER + len + 1] << 8);
                if (want == got) on_frame(frame[2], frame + cp::WIRE_HEADER, len);
                in_frame = false; flen = 0;
            }
        }
        return;
    }
    if (c == cp::MAGIC0) { in_frame = true; flen = 0; frame[flen++] = c; return; }
    // ASCII (e.g. "PONG") — accumulate but we act on nothing today.
    if (c == '\n' || c == '\r') line = "";
    else if (line.length() < 64) line += (char)c;
    else line = "";
}
void drain() { while (Serial.available()) byte_in((uint8_t)Serial.read()); }
} // namespace rx

// ── HELLO with capability list ────────────────────────────────────────────────
static uint16_t g_caps = 0;
static void send_hello() {
    Serial.print("HELLO proto-coproc v2 caps=buttons");
    if (bno::ok)   Serial.print(",imu_bno");
    if (mpu::ok)   Serial.print(",imu_mpu");
    if (boop::ok)  Serial.print(",boop");
    if (light::ok) Serial.print(",light");
    if (kPanelsEnabled) Serial.print(",panels");
    Serial.print(" n_btn="); Serial.print((int)(sizeof(kButtonPins)/sizeof(kButtonPins[0])));
    Serial.print(" n_chain="); Serial.println(kPanelsEnabled ? kPanelChains : 0);
}

void setup() {
    Serial.begin(115200);
    btn::setup();
    if (kSensorsEnabled) {
        Wire1.setSDA(kI2cSdaPin);
        Wire1.setSCL(kI2cSclPin);
        Wire1.begin();
        Wire1.setClock(kI2cHz);
        if (kBnoEnabled)   bno::ok   = bno::init();
        if (kMpuEnabled)   mpu::ok   = mpu::init();
        if (kBoopEnabled)  boop::ok  = boop::init();
        if (kLightEnabled) light::ok = light::init();
    }
}

void loop() {
    const uint32_t now = millis();

    // (Re)greet on the USB CDC connect edge so a HUD restart re-reads our caps.
    static bool was_connected = false;
    static uint32_t last_ping = 0;
    const bool connected = (bool)Serial;
    if (connected && !was_connected) { send_hello(); last_ping = now; }
    was_connected = connected;

    btn::poll(now);

    static uint32_t t_bno = 0, t_mpu = 0, t_boop = 0, t_light = 0;
    if (bno::ok   && now - t_bno   >= kBnoPollMs)   { t_bno   = now; bno::poll(); }
    if (mpu::ok   && now - t_mpu   >= kMpuPollMs)   { t_mpu   = now; mpu::poll(); }
    if (boop::ok  && now - t_boop  >= kBoopPollMs)  { t_boop  = now; boop::poll(); }
    if (light::ok && now - t_light >= kLightPollMs) { t_light = now; light::poll(); }

    if (connected && now - last_ping >= kPingMs) { last_ping = now; Serial.println("PING"); }

    rx::drain();
}
