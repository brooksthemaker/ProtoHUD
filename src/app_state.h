#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <cstdint>
#include <ctime>
#include <imgui.h>
#include "capture.h"

// ── Post-processing config ────────────────────────────────────────────────────
// Modified from menu (any thread); read by the render thread via snap.
// Simple struct — individual fields are written atomically enough for a bool/float.

struct PostProcessConfig {
    bool  edge_enabled       = false;
    float edge_strength      = 1.0f;
    ImU32 edge_color         = IM_COL32(30, 220, 60, 255);
    bool  desat_enabled      = false;
    float desat_strength     = 0.8f;
    float contrast_threshold = 0.15f;  // 0.07 = aggressive, 0.25 = subtle
    float edge_scale         = 1.0f;   // 1.0–5.0; larger step = coarser outline, fewer interior edges
    float edge_threshold     = 0.30f;  // 0.0–0.6; suppress edges weaker than this magnitude
    float focus_str          = 0.0f;   // 0.0–1.0; blend Laplacian sharpness into bg_weight
    int   focus_lens_pos     = 500;    // 0–1000 from AF; set each frame — not persisted
    float edge_gate_scale    = 2.0f;   // 0.0=off, else=step multiplier for coarse confirmatory Sobel
    float color_protect      = 0.5f;   // 0.0–1.0; saturated pixels resist desaturation
    float edge_dilate        = 1.0f;   // 0.0–3.0; widens colour-kept zone around object edges

    // Motion highlight (temporal frame-difference)
    bool  motion_enabled  = false;
    float motion_strength = 0.9f;
    float motion_thresh   = 0.04f;   // min luma delta to count as motion (~4%); noise ~1-2%
    float motion_radius   = 3.0f;    // sampling step in texels (smaller = tighter line)
    ImU32 motion_color    = IM_COL32(0, 255, 100, 255);
    float motion_line        = 1.0f;    // 0.0=filled blob, 1.0=fine boundary line
    float motion_update_rate = 0.5f;   // EMA blend rate: 1.0=instant (1 frame), lower=longer trail
};

// ── Overlay layout config (PiP and Android mirror) ───────────────────────────
// Modified at runtime by menu actions; read exclusively on the render thread,
// so no mutex is needed.

struct OverlayConfig {
    float    anchor_x = 0.0f;   // screen fraction (0=left, 1=right)
    float    anchor_y = 0.0f;   // screen fraction (0=top,  1=bottom)
    float    pan_x    = 0.f;    // pixel nudge from anchor point
    float    pan_y    = 0.f;
    float    size     = 0.25f;  // fraction of screen height
    enum class Rotation { Landscape = 0, Portrait, LandscapeFlipped, PortraitFlipped };
    Rotation rotation = Rotation::Landscape;
};

// ── Sub-states ────────────────────────────────────────────────────────────────

struct FaceState {
    uint8_t  effect_id    = 0;
    uint8_t  gif_id       = 0;
    uint8_t  r = 0, g = 220, b = 180;
    uint8_t  brightness   = 200;
    uint8_t  palette_id      = 0;
    uint8_t  face_index      = 0;
    uint8_t  accent_bright   = 5;
    uint8_t  microphone      = 0;
    uint8_t  mic_level       = 5;
    uint8_t  boop_sensor     = 0;
    uint8_t  spectrum_mirror = 0;
    uint8_t  face_size       = 5;
    uint8_t  fan_speed       = 5;
    uint8_t  material_color  = 0;   // SET_MENU_ITEM(8) index (0-21; see preset_material)
    bool     playing_gif  = false;
    bool     connected    = false;
    bool     hud_control  = false;  // true = HUD has taken manual control; false = Teensy autonomous
};

struct LoRaNode {
    uint8_t     local_id     = 0;        // compact 1-based index from the radio firmware
    uint32_t    node_id      = 0;        // full 32-bit hardware ID of the remote radio
    std::string name;                    // display name (UTF-8, up to 12 chars)
    float       heading_deg  = 0.0f;    // bearing from us to them (degrees, 0–360)
    float       distance_m   = 0.0f;    // great-circle distance in metres
    int8_t      rssi         = -120;    // last received RSSI (dBm)
    int8_t      snr          = 0;       // last received SNR (dB)
    time_t      last_seen    = 0;
};

struct LoRaMessage {
    uint8_t     local_id  = 0;   // matches LoRaNode::local_id for name lookup
    time_t      timestamp = 0;
    std::string text;
    bool        read      = false;
};

struct KnobState {
    int32_t  angle_milli    = 0;
    int      detent_index   = 0;
    int8_t   direction      = 0;
    float    velocity_rpm   = 0.0f;
    bool     connected      = false;
    bool     awake          = false;
    uint8_t  last_button_id    = 0;
    uint8_t  last_button_event = 0;
};

struct SystemHealth {
    bool teensy_ok       = false;
    bool lora_ok         = false;
    bool knob_ok         = false;
    bool knob_ready      = false;  // Motor calibration complete
    bool gamepad_ok      = false;
    bool wireless_ok     = false;
    int  wireless_battery_pct = -1;  // -1 = unknown
    // Paired phone battery (via KDE Connect bridge). -1 when no phone is
    // bound or the daemon isn't running; charging flag is meaningless in
    // that case.
    int  phone_battery_pct    = -1;
    bool phone_charging       = false;
    bool cam_owl_left    = false;
    bool cam_owl_right   = false;
    bool cam_usb1        = false;
    bool cam_usb2        = false;
    bool cam_usb3        = false;
    bool audio_ok        = false;  // Spatial audio engine running
    bool android_mirror  = false;  // scrcpy connected and streaming
    bool mpu9250_ok      = false;  // MPU-9250 backup compass running
    bool wifi_ok         = false;  // Wi-Fi associated
    bool ssh_active      = false;  // SSH daemon is running
    // Render-thread-only: connected AND overlay enabled
    bool cam_usb1_overlay = false;
    bool cam_usb2_overlay = false;
    bool cam_usb3_overlay = false;
};

struct AudioState {
    bool    enabled     = false;
    bool    device_ok   = false;  // ALSA devices opened successfully
    float   master_gain = 1.0f;
    int     xrun_count  = 0;      // cumulative ALSA xruns
    float   cpu_load    = 0.0f;   // 0.0–1.0 fraction of period budget used
    int     output      = 0;      // AudioOutput enum: 0=VITURE, 1=HEADPHONES, 2=HDMI
};

struct CameraFocusState {
    enum class Mode { MANUAL = 0, AUTO = 1, SLAVE = 2 } mode = Mode::AUTO;
    int focus_position = 500;  // 0-1000
    bool af_active = false;
    bool af_locked = false;
};

// Voice → mouth_open driving for the native face. Tunables read by main.cpp
// from config.json at startup, written back via mutate_cfg on shutdown, and
// menu-mutated through the audio_engine.voice() analyzer at runtime so the
// next FFT cycle picks them up.
struct VoiceMouthConfig {
    bool   enabled    = false;
    float  sensitivity = 1.0f;     // band RMS gain
    float  noise_gate  = 0.02f;    // below this band RMS → mouth stays closed
    float  attack_ms   = 30.f;     // envelope follower time when opening
    float  release_ms  = 120.f;    // envelope follower time when closing
    float  band_lo_hz  = 100.f;
    float  band_hi_hz  = 3500.f;

    // Viseme selection (spectral-centroid classifier picks one of
    // mouth_open / mouth_small / mouth_smile / mouth_round each audio period).
    bool   visemes_enabled     = false;
    float  viseme_round_max_hz = 600.f;
    float  viseme_open_max_hz  = 1200.f;
    float  viseme_small_max_hz = 2000.f;
};

// Manual driver for the optical mouth blendshape stack — exercises the render
// path (FaceState::set_mouth_blendshapes → FaceLoader stack) with no Pi Zero
// tracker attached. The future MouthTracker UART module replaces this as the
// live source. weights are indexed by face::mouth_blendshapes(); sized lazily
// when the menu is built.
struct MouthTrackerTestConfig {
    bool               enabled    = false;   // push weights at confidence below
    float              confidence = 1.0f;    // [0,1]; 0 = renderer uses audio path
    std::vector<float> weights;              // [0,1] per blendshape, contract order
};

// One boop-sensor zone's user-visible behaviour. The sensor reports zone
// touches (no expression knowledge); main.cpp's on_boop callback reads this
// to call IFaceController::trigger_boop with the per-zone expression. Indexed
// in lockstep with sensor::BoopSensor::Zone (Snout=0, LeftCheek=1, RightCheek=2).
// Rapid-trigger "animated eyes" easter egg for a boop zone: boop the same zone
// `count` times within `window_s` and a procedural eye animation takes over the
// panels for `duration_s` (played instead of the normal reaction, then the
// counter resets). anim is a face::EyeAnim value; the other fields re-skin the
// built-in animations. Stored as plain types to keep app_state.h dependency-free.
struct EyeTriggerConfig {
    bool    enabled    = false;
    int     count      = 3;       // boops within the window to fire
    double  window_s   = 4.0;     // consecutive boops must land this close together
    int     anim       = 0;       // face::EyeAnim value
    double  speed      = 1.0;
    double  size       = 1.0;
    double  duration_s = 2.5;     // how long the animation plays
    uint8_t r = 0, g = 220, b = 180;   // primary colour
};

struct BoopZoneConfig {
    bool        enabled    = true;
    std::string expression = "surprised";   // canonical face PNG name
    double      duration_s = 0.8;           // how long the expression holds
    uint8_t     threshold  = 12;            // MPR121 touch threshold (lower = more sensitive)
    int         electrode  = -1;            // MPR121 electrode 0..11 driving this zone (-1 = none/derived)
    EyeTriggerConfig eye_trigger;           // optional rapid-boop animated-eyes reaction
};

// ── Light-sensor squint trigger ──────────────────────────────────────────────
// Edge-detects the wearer stepping from a dim area into a bright one and
// fires a transient expression (default "squint") for a few seconds before
// reverting. The driver lives in sensor::LightSensor; main hosts the edge
// detector and the menu mutates this struct.
struct LightSquintConfig {
    bool        enabled              = false;
    float       dark_threshold_lux   = 100.f;   // below = "dark"
    float       bright_threshold_lux = 800.f;   // above = "bright"
    float       transition_window_s  = 2.0f;    // dark→bright must happen within this many seconds
    std::string expression           = "squint";
    double      duration_s           = 1.5;     // hold time before reverting
    float       cooldown_s           = 3.0f;    // min time between consecutive squints
};

struct NightVisionState {
    float exposure_ev        = 0.0f;  // -3.0 to +3.0
    int   shutter_us         = 33333; // microseconds (40 to 1000000)
    bool  nv_enabled         = false; // night vision preset active (HUD indicator + apply flag)
    bool  csi_awb_left       = true;  // left OWLsight auto white balance
    bool  csi_awb_right      = true;  // right OWLsight auto white balance
    bool  auto_nv            = false; // auto-enable NV when scene is dark
    float auto_nv_gain_threshold = 4.0f; // AnalogueGain above which auto-NV activates
};

struct ClockConfig {
    bool  use_24h         = true;
    bool  show_seconds    = true;
    bool  show_date       = false;
    float font_scale      = 1.5f;  // multiplied against font_mono_->FontSize at draw time
    int   manual_offset_s = 0;     // seconds added to system time; 0 = pure system clock
};

struct CameraResolutionState {
    int width  = 1280;
    int height = 800;
    int fps    = 60;
};

// Per-CSI-camera libcamera control state, persisted to config and re-applied at
// startup. Defaults are the libcamera/ISP neutral values. Mirrors the cur_*
// values DmaCamera tracks; the menu writes to the camera, save reads them back.
struct CameraControlsState {
    int   af_range        = 0;     // 0 Normal, 1 Macro, 2 Full
    int   af_speed        = 0;     // 0 Normal, 1 Fast
    float gain            = 0.0f;  // manual AnalogueGain; 0 = auto
    int   ae_metering     = 0;     // 0 Centre, 1 Spot, 2 Matrix
    int   ae_constraint   = 0;     // 0 Normal, 1 Highlight, 2 Shadows
    int   ae_exp_mode     = 0;     // 0 Normal, 1 Short, 2 Long
    int   flicker         = 0;     // 0 Off, 1 Auto, 2 50 Hz, 3 60 Hz
    int   awb_mode        = 0;     // 0 Auto … 6 Cloudy (libcamera AwbMode)
    float brightness      = 0.0f;  // -1 .. 1
    float contrast        = 1.0f;  //  0 .. 2
    float saturation      = 1.0f;  //  0 .. 2
    float sharpness       = 1.0f;  //  0 .. 2
    int   noise_reduction = 0;     // 0 Off,1 Fast,2 HQ,3 Minimal
    int   hdr             = 0;     // libcamera HdrMode (0 Off,2 Multi,3 Single,4 Night)
};

// Digital zoom / crop for OWLsight cameras.
// zoom=1.0 → full frame (identity). zoom>1.0 → crops to 1/zoom of the frame.
// center_x / center_y are normalized (0.0–1.0) screen-space coordinates.
// Option B hook: the same values are forwarded to libcamera ScalerCrop when
// apply_pending_controls() implements it.
struct ZoomCropState {
    float zoom     = 1.0f;
    float center_x = 0.5f;
    float center_y = 0.5f;
};

// Synchronized mirror crop: both eyes zoom to the same level and pan to their
// inner (nose-side) edges. The left eye pans right (center_x > 0.5) and the
// right eye pans left (center_x < 0.5) by the same inner_bias offset.
enum class CropVertical { Top, Middle, Bottom };
struct MirrorCropState {
    bool         enabled    = false;
    float        zoom       = 2.0f;
    CropVertical vertical   = CropVertical::Middle;
    float        inner_bias = 0.15f;   // UV offset from center toward nose
};

// Single-camera full/partial-screen mode: one eye's feed fills an anchor region.
enum class CamSingleAnchor { Full, Top, Bottom, Left, Right };
struct CamSingleState {
    bool           enabled   = false;
    bool           use_right = false;  // false = left camera, true = right camera
    CamSingleAnchor anchor   = CamSingleAnchor::Full;
};

struct ImuPose {
    float roll  = 0.0f;
    float pitch = 0.0f;
    float yaw   = 0.0f;
};

// ── Full IMU debug readout ─────────────────────────────────────────────────────
// Aggregates every value delivered by the two IMUs so the debug window can show
// live readings for fault-finding:
//   • VITURE XR glasses — fused Euler pose (roll/pitch/yaw), ~1 kHz.
//   • MPU-9250          — raw accel (g), gyro (deg/s), mag (µT), die temp, heading.
// Written by the IMU callbacks under AppState::mtx; read on the render thread.
struct ImuData {
    // VITURE XR glasses fused pose (degrees)
    bool  xr_active  = false;   // IMU frames arrived within the freshness window
    float xr_roll    = 0.f;
    float xr_pitch   = 0.f;
    float xr_yaw     = 0.f;
    float xr_rate_hz = 0.f;     // measured callback rate (EMA)

    // MPU-9250 raw 9-axis readout
    bool  mpu_ok      = false;
    float accel_g[3]  = {0.f, 0.f, 0.f};   // x, y, z (g)
    float gyro_dps[3] = {0.f, 0.f, 0.f};   // x, y, z (deg/s)
    float mag_ut[3]   = {0.f, 0.f, 0.f};   // x, y, z (µT, bias-corrected)
    float temp_c      = 0.f;               // MPU die temperature
    float mpu_heading = 0.f;               // fused compass heading (deg)
    float mpu_rate_hz = 0.f;               // measured sample rate (EMA)

    // BNO055 9-DOF (on-chip absolute orientation) — populated when the
    // sensor is wired and enabled. calib_* are 0..3 per axis (3 = fully
    // calibrated); the heading should be treated as drifting until calib_sys
    // reaches at least 2.
    bool  bno_ok          = false;
    float bno_accel_g[3]  = {0.f, 0.f, 0.f};
    float bno_gyro_dps[3] = {0.f, 0.f, 0.f};
    float bno_mag_ut[3]   = {0.f, 0.f, 0.f};
    float bno_euler[3]    = {0.f, 0.f, 0.f};  // [0]=heading, [1]=roll, [2]=pitch
    uint8_t bno_calib_sys   = 0;
    uint8_t bno_calib_gyro  = 0;
    uint8_t bno_calib_accel = 0;
    uint8_t bno_calib_mag   = 0;
};

// ── Map overlay ───────────────────────────────────────────────────────────────

struct MapOverlayConfig {
    bool        enabled             = true;   // minimap is a permanent HUD element by default
    std::string map_path;            // full filesystem path to loaded image
    float       anchor_x            = 0.5f;   // screen fraction (centre by default)
    float       anchor_y            = 0.5f;
    float       size_px             = 300.f;  // half-width of the displayed map in pixels
    float       opacity             = 0.80f;
    float       pan_x               = 0.f;    // pixel offset from anchor
    float       pan_y               = 0.f;
    float       map_north_deg       = 0.f;    // compass_heading saved at calibration
    bool        calibrated          = false;
    bool        rotate_with_heading = true;
    float       image_rotate_deg    = 0.f;    // manual image rotation offset (degrees)
    bool        circle_window       = true;   // round minimap by default
    float       zoom                = 1.0f;   // >1 = zoom into map content (shows less of the image)

    // Compass ring around the minimap (cardinals + ticks + LoRa markers).
    bool        compass_ring        = true;
    // Battery arc — a partial ring (~quarter) hugging the minimap's left side,
    // sitting OUTSIDE the compass ring. When system_debug is on, the gauge instead
    // shows CPU (bar 1) + GPU/render load (bar 2, concentric, angularly offset).
    bool        battery_arc         = true;
    bool        system_debug        = false;
    // Clock above the minimap, with an active timer/alarm shown beside it.
    bool        clock               = true;
    bool        clock_date          = true;   // show the date under the clock

    // Protoface "portrait" — a scaled, one-side preview of the LED face shown
    // beside the minimap (American Fugitive-style character portrait).
    bool        portrait            = false;
    bool        portrait_right_half = false;  // false = left face half, true = right
    float       portrait_scale      = 1.0f;   // size multiplier for the preview window

    // Helldivers-style temporary expanded view (pan/zoom) — runtime only.
    bool        expanded            = false;
    float       view_zoom           = 1.0f;   // expanded-view zoom (independent of minimap)
    float       view_pan_x          = 0.f;    // expanded-view pan, image-space fraction
    float       view_pan_y          = 0.f;
};

// ── Info panel (cycling side widgets) ───────────────────────────────────────────
// A configurable HUD region — typically mirroring the minimap on the opposite
// side — that auto-cycles through glanceable widgets so a user can take in
// weather / notifications / schedule / time at a glance. Render-thread owned; the
// menu writes fields while holding the mutex (like MapOverlayConfig).
enum class InfoWidget : uint8_t { Clock = 0, Notifications, Schedule, Weather, WeatherPrecip, Count };

struct InfoPanelConfig {
    bool  enabled   = false;    // off by default; user opts in per side
    float anchor_x  = 0.85f;    // screen fraction — right side, mirroring a left minimap
    float anchor_y  = 0.5f;
    float pan_x     = 0.f;
    float pan_y     = 0.f;
    float size_px   = 150.f;    // half-extent (radius), matching the minimap footprint
    float cycle_sec = 6.f;      // dwell per widget before advancing
    int   clock_face = 0;       // clock style: 0=ticks 1=numbers 2=minimal
                               // 3=Halo 4=Solar 5=Fallout 6=Space 7=Auto(theme)
    // Which widgets take part in the cycle (indexed by InfoWidget):
    // clock, notifications, schedule, weather (now), weather (precip).
    bool  show[static_cast<int>(InfoWidget::Count)] = { true, true, true, false, false };
};

// ── HUD dock layout ─────────────────────────────────────────────────────────────
// The minimap and info panel are opposite-side twins: the minimap is always on the
// RIGHT, the info panel always on the LEFT. `bottom` picks the top vs bottom edge;
// `v_offset` is a fine vertical nudge in pixels applied to both. Anchors are derived
// from this (see apply_hud_dock in main).
struct HudDock {
    bool  bottom   = true;    // false = top edge, true = bottom edge (default bottom)
    float v_offset = 0.f;     // vertical nudge in pixels (Up/Down menu), applied to both
};

// ── Particle effects ──────────────────────────────────────────────────────────

enum class EffectType : uint8_t {
    None = 0, ArmGlints, CornerDrift, PopupBurst, CompassTurbulence, NebulaEdge, DarkVignette
};
enum class EffectPalette : uint8_t {
    Theme = 0, Halo, Solar, Fallout, Space
};
struct EffectsConfig {
    EffectType    effect  = EffectType::None;
    EffectPalette palette = EffectPalette::Theme;
};

struct TimerAlarmState {
    bool   timer_active    = false;
    time_t timer_end       = 0;       // epoch when countdown expires
    bool   alarm_active    = false;
    time_t alarm_fire_at   = 0;       // epoch when alarm fires
    bool   alarm_triggered = false;   // true → alarm popup is shown
    bool   timer_triggered = false;   // true → timer-expired popup is shown
    int    alarm_hour      = 0;       // picker working value (0–23)
    int    alarm_minute    = 0;       // picker working value (0–59)
    int    custom_timer_min = 0;      // custom timer minutes (0–99)
    int    custom_timer_sec = 0;      // custom timer seconds (0–59)
};

// ── Scheduler / reminders ───────────────────────────────────────────────────────
// Full calendar events fed from a Python companion daemon (scheduler_daemon/) that
// owns all networking: it serves a phone web form and (later) syncs Google Calendar,
// merges both, and writes events.json + scheduler_status.json atomically. ProtoHUD
// reads those files via SchedulerMonitor (file poll) and fires reminders through the
// existing NotificationQueue. The C++ side never does HTTP/OAuth.
enum class EventSource : uint8_t { Manual = 0, Google = 1 };

struct ScheduledEvent {
    std::string uid;                  // stable dedupe key (gcal id, or daemon UUID)
    std::string title;
    std::string location;
    time_t      start_utc = 0;        // epoch UTC; 0 = invalid. Daemon does all tz math.
    time_t      end_utc   = 0;
    bool        all_day   = false;
    EventSource source    = EventSource::Manual;
    // Render-thread-only fire bookkeeping — NOT serialized; carried across reloads
    // by SchedulerMonitor's uid merge (reset only when start_utc changes).
    bool        fired_lead  = false;  // lead-time reminder already raised
    bool        fired_start = false;  // at-start reminder already raised
    time_t      snooze_until = 0;     // if >0, re-raise the lead reminder at this time
};

struct SchedulerStatus {
    bool        daemon_ok      = false;  // files parsed and fresh (mtime recent)
    std::string web_url;                 // e.g. "http://192.168.1.42:8770"
    std::string gcal_state     = "disconnected"; // disconnected|pending|connected|error
    std::string gcal_user_code;          // device-flow code to type on phone
    std::string gcal_verify_url;         // device-flow verification URL
    time_t      last_sync_utc  = 0;
    int         event_count    = 0;
};

// ── System metrics ────────────────────────────────────────────────────────────
static constexpr int kSysHistLen  = 60;
static constexpr int kPingHistLen = 30;
static constexpr int kMaxCpuCores = 16;

struct SysMetrics {
    uint64_t uptime_s     = 0;
    float    cpu_pct      = 0.f;      // 0–100
    float    ram_used_mb  = 0.f;
    float    ram_total_mb = 0.f;
    float    cpu_history [kSysHistLen]  = {};
    float    ram_history [kSysHistLen]  = {};
    int      history_head = 0;        // shared head for cpu/ram (updated by SystemMonitor)
    // Per-core CPU breakdown (filled by SystemMonitor)
    int      cpu_core_count = 0;
    float    cpu_core_pct[kMaxCpuCores] = {};   // 0–100 per logical core
    float    cpu_core_mhz[kMaxCpuCores] = {};   // current freq; 0 if unavailable
    float    cpu_temp_c     = 0.f;              // package temperature (0 if unknown)
    // Frame-time metrics — updated by render thread each frame
    float    frame_time_ms  = 0.f;    // last frame duration in ms
    float    fps_avg        = 0.f;    // 1000 / frame_time_ms (instantaneous)
    float    fps_avg_smooth = 0.f;    // EMA-smoothed FPS over fps_avg_interval_s
    float    ft_history[kSysHistLen]  = {};
    int      ft_history_head = 0;
};

// ── GPU metrics ─────────────────────────────────────────────────────────────────
// VideoCore (Raspberry Pi) per-domain clock breakdown + temperature, polled via
// vcgencmd by SystemMonitor. The clock domains are the GPU's functional cores.
static constexpr int kMaxGpuClocks = 6;

struct GpuClock {
    char  name[6] = {};   // "core","v3d","isp","h264","hevc"
    float mhz     = 0.f;
};

struct GpuMetrics {
    bool     available   = false;   // vcgencmd produced usable data
    float    temp_c      = 0.f;
    int      clock_count = 0;
    GpuClock clocks[kMaxGpuClocks] = {};
};

struct SerialMetrics {
    float  teensy_rtt_ms     = -1.f;  // round-trip time for REQ_STATUS→STATUS; -1 = no sample
    float  knob_event_age_ms = -1.f;  // ms since last SmartKnob event; -1 = no events yet
    int8_t lora_rssi         = 0;     // last RADIO_STATUS RSSI (dBm)
    float  lora_snr          = 0.f;   // last RADIO_STATUS SNR (dB)
};

struct WifiState {
    bool        connected  = false;
    std::string ssid;
    std::string ip;
    int         signal_dbm = -100;
};

struct PingState {
    bool        reachable    = false;
    float       latency_ms   = 0.f;
    float       history[kPingHistLen] = {};
    int         history_head = 0;
    std::string host;
};

struct BtDevice {
    std::string name;
    std::string mac;
    bool        connected = false;
    bool        paired    = false;   // false + !connected ⇒ discovered-only (pairable)
};

struct SshState {
    bool active = false;
    int  port   = 22;
};

// ── Weather ─────────────────────────────────────────────────────────────────────
// Live weather for the info-panel widget. Fetched off the render thread by
// WeatherMonitor (Open-Meteo via curl) into `weather`; settings live in
// `weather_cfg`. WMO weather codes map to short text + an icon asset name.
// One day of the daily forecast (index 0 = today). Day-of-week is derived at draw
// time from the local clock (today + index), so no date field is needed here.
struct WeatherDay {
    int   code      = -1;   // WMO daily weather code
    float tmax      = 0.f;
    float tmin      = 0.f;
    int   rain_prob = -1;   // max precip probability (%)
};

struct WeatherState {
    bool        ok          = false;   // last fetch succeeded
    float       temp        = 0.f;     // in the configured unit
    float       feels       = 0.f;
    float       wind        = 0.f;
    int         humidity    = -1;      // %
    int         code        = -1;      // WMO weather code
    bool        is_day      = true;
    float       temp_high   = 0.f;     // today's high (daily)
    float       temp_low    = 0.f;     // today's low (daily)
    float       precip_now  = 0.f;     // current precipitation amount
    int         rain_prob   = -1;      // today's max precip probability (%)
    std::string condition;             // "Clear", "Rain", …
    std::string location;              // city name
    time_t      updated_utc = 0;

    // Multi-day forecast (for the expanded-map sidebar). forecast[0] == today.
    static constexpr int kForecastDays = 3;
    WeatherDay  forecast[kForecastDays];
    int         forecast_count = 0;
};

struct WeatherConfig {
    bool        enabled      = false;  // run the fetcher
    bool        auto_locate  = true;   // IP geolocation vs manual lat/lon
    bool        metric       = true;   // C/kph vs F/mph
    double      lat          = 0.0;
    double      lon          = 0.0;
    std::string place;                 // optional manual label
    int         interval_min = 15;
};

// WMO weather code → short label.
inline const char* wmo_text(int c) {
    if (c <= 0)               return "Clear";
    if (c <= 2)               return "Partly Cloudy";
    if (c == 3)               return "Overcast";
    if (c == 45 || c == 48)   return "Fog";
    if (c >= 51 && c <= 57)   return "Drizzle";
    if ((c >= 61 && c <= 67) || (c >= 80 && c <= 82)) return "Rain";
    if ((c >= 71 && c <= 77) || c == 85 || c == 86)   return "Snow";
    if (c >= 95)              return "Thunderstorm";
    return "--";
}

// WMO weather code → icon asset name (assets/icons/<name>.png; no-ops if absent).
inline const char* wmo_icon(int c, bool day) {
    if (c <= 0)               return day ? "wx-clear" : "wx-clear-night";
    if (c <= 2)               return day ? "wx-partly" : "wx-partly-night";
    if (c == 3)               return "wx-cloudy";
    if (c == 45 || c == 48)   return "wx-fog";
    if (c >= 51 && c <= 57)   return "wx-drizzle";
    if ((c >= 61 && c <= 67) || (c >= 80 && c <= 82)) return "wx-rain";
    if ((c >= 71 && c <= 77) || c == 85 || c == 86)   return "wx-snow";
    if (c >= 95)              return "wx-storm";
    return "wx-cloudy";
}

// ── Notification system ───────────────────────────────────────────────────────

enum class NotifType : uint8_t { Alarm, Timer, LoRa, App };

// Default icon asset name per notification type (resolves to <icons>/<name>.png).
// A Notification may override this via its `icon` field.
inline const char* notif_type_icon(NotifType t) {
    switch (t) {
        case NotifType::Alarm: return "alarm";
        case NotifType::Timer: return "timer";
        case NotifType::LoRa:  return "message";
        case NotifType::App:   return "bell";
    }
    return "bell";
}

struct NotifAction {
    std::string label;                              // "DISMISS", "+2 MIN", etc.
    std::function<void(struct AppState&)> fn;
};

struct Notification {
    uint32_t    id           = 0;
    NotifType   type         = NotifType::App;
    std::string title;
    std::string body;
    int64_t     timestamp    = 0;   // epoch seconds
    float       auto_dismiss_s = 8.f;  // 0 = manual only
    bool        read         = false;
    bool        dismissed    = false;
    bool        big          = false;  // render a larger toast (wrapped body) — chat/DM messages
    bool        saved        = false;  // pinned by the user: survives Clear and the rolling buffer
    std::string icon;                  // optional icon asset name; empty → by type
    std::vector<NotifAction> actions;
};

struct NotificationQueue {
    std::deque<Notification> items;   // newest first
    static constexpr int kMax = 50;
    uint32_t next_id = 1;

    void push(Notification n) {
        n.id = next_id++;
        if (n.timestamp == 0) n.timestamp = static_cast<int64_t>(time(nullptr));
        items.push_front(std::move(n));
        // Evict the oldest *unsaved* entry when over capacity so pinned
        // messages aren't rolled off the end of the buffer.
        while (static_cast<int>(items.size()) > kMax) {
            auto victim = items.end();
            for (auto it = items.begin(); it != items.end(); ++it)
                if (!it->saved) victim = it;            // last (oldest) unsaved
            if (victim == items.end()) break;           // all saved — keep them
            items.erase(victim);
        }
    }

    // Remove notifications matching pred, but never ones the user saved
    // unless include_saved is set. Returns how many were removed.
    template <typename Pred>
    int clear_if(Pred pred, bool include_saved) {
        int removed = 0;
        for (auto it = items.begin(); it != items.end(); ) {
            if ((include_saved || !it->saved) && pred(*it)) {
                it = items.erase(it); ++removed;
            } else ++it;
        }
        return removed;
    }

    int unread_count() const {
        int c = 0;
        for (const auto& n : items) c += (!n.read && !n.dismissed);
        return c;
    }

    void mark_read(uint32_t id) {
        for (auto& n : items) if (n.id == id) { n.read = true; return; }
    }

    void dismiss(uint32_t id) {
        for (auto& n : items) if (n.id == id) { n.dismissed = true; n.read = true; return; }
    }

    void dismiss_all() {
        for (auto& n : items) { n.dismissed = true; n.read = true; }
    }
};

// ── Scanned QR / barcode capture log ──────────────────────────────────────────
// Each newly-seen code is saved to its own folder (link.txt + the raw frame it
// was captured from + meta.json). The log keeps a running, de-duplicated list
// (newest first) so the same code isn't captured twice across sessions.
struct QrCapture {
    std::string text;            // decoded payload (URL or raw text)
    std::string type;            // symbol type, e.g. "QR-Code"
    int64_t     timestamp = 0;   // epoch seconds first captured
    std::string folder;          // absolute path to this capture's folder
    std::string image;           // colour camera frame filename (preview); may be empty
    std::string decode;          // grayscale decode frame filename (what ZBar saw)
};

struct QrCaptureLog {
    std::deque<QrCapture> items;       // newest first
    std::set<std::string> seen;         // exact-payload set for de-duplication
    static constexpr int  kMax = 200;   // safety cap on retained entries

    bool contains(const std::string& t) const { return seen.count(t) > 0; }

    void add(QrCapture c) {
        seen.insert(c.text);
        items.push_front(std::move(c));
        while (static_cast<int>(items.size()) > kMax) {
            seen.erase(items.back().text);
            items.pop_back();
        }
    }
    void rebuild_seen() {
        seen.clear();
        for (const auto& c : items) seen.insert(c.text);
    }
};

// ── Video recording control ───────────────────────────────────────────────────
// Requests are posted by input handlers / toast actions (any thread) and consumed
// by the render thread, which owns the VideoRecorder. Start acts as a toggle.
enum class VideoRequest : uint8_t { None, Start, Stop, Pause, Resume };
enum class VideoCamera  : uint8_t { Left, Right, Both };

// ── Master state ──────────────────────────────────────────────────────────────
// Mutable fields are updated from serial/camera threads and read by the
// render thread.  Callers must hold the mutex for any multi-field access.

struct AppState {
    std::mutex mtx;

    FaceState    face;
    SystemHealth health;
    KnobState    knob;
    AudioState   audio;

    std::vector<LoRaNode>    lora_nodes;
    std::deque<LoRaMessage>  lora_messages;
    size_t                   max_messages = 50;

    // Heading used for the HUD compass. Updated by LoRa or IMU via the
    // pick_imu_heading() helper in main.cpp.
    float compass_heading    = 0.0f;
    bool  compass_bg_enabled = true;
    bool  compass_tape       = true;   // the top-of-screen compass tape

    // ── IMU source selection ────────────────────────────────────────────────
    // The HUD has three possible heading sources at runtime: the BNO055
    // (best — on-chip 9-DOF fusion), the MPU9250 (compass with software
    // fusion), and the VITURE XR glasses' built-in IMU. Each writes into
    // its own ImuSlot below; pick_imu_heading(state, now_us) in main.cpp
    // chooses the active one per frame based on this enum + slot freshness.
    enum class ImuSource : uint8_t {
        Auto    = 0,   // best fresh source in priority order: BNO055 > MPU9250 > Viture
        Bno055  = 1,
        Mpu9250 = 2,
        Viture  = 3,
        None    = 4,   // hold last value, ignore live updates
    };
    ImuSource imu_source = ImuSource::Auto;

    struct ImuSlot {
        int64_t last_us     = 0;     // steady_clock-microsecond timestamp of last update
        float   heading_deg = 0.f;   // 0..360, normalised + offset/declination applied
        bool    calibrated  = true;  // BNO055 sets false until cal_sys >= 2
    };
    ImuSlot imu_bno;
    ImuSlot imu_mpu;
    ImuSlot imu_viture;

    // Legacy HUD chrome (edge/corner indicators: compass tape, health sides, face
    // indicator, corner clock/timer, LoRa message list). Off = show only the new
    // modular HUD (minimap + info panel). Render-thread-owned; menu writes under mtx.
    bool  legacy_hud         = true;

    // Skip the startup landing/profile screen and run the current config directly.
    // Startup-only behavior; toggled in the System menu, persisted to config.
    bool  skip_landing       = false;

    // Which XR-IMU axis drives the compass, and whether to invert the rotation
    // direction. Configurable from the menu so different IMU orientations work
    // without recompiling.
    enum class CompassAxis { Roll = 0, Pitch, Yaw };
    CompassAxis compass_axis   = CompassAxis::Roll;
    bool        compass_invert = false;

    // Theater mode: render OWLsight cameras at their native aspect ratio with
    // black bars filling the remaining FBO area. Letterbox or pillarbox depending
    // on camera AR vs. display AR. Future: USB cameras fill the black bar regions.
    bool  theater_mode    = false;
    bool  cameras_swapped = false; // swap left ↔ right OWLsight draw assignment

    // Theater mode anchor: controls which edge the letterbox/pillarbox bars sit on.
    // Center: cameras pushed to inner edges (meet at seam, black on outer sides).
    // Outside: cameras pushed to outer edges (black gap in centre of display).
    // Left: both feeds on the right side, black on the left.
    // Right: both feeds on the left side, black on the right.
    // Top/Bottom: letterbox-mode vertical anchor only.
    enum class TheaterAnchor { Center = 0, Outside, Left, Right, Top, Bottom };
    TheaterAnchor theater_anchor = TheaterAnchor::Center;

    // Photo capture: set by menu or GPIO long-press; consumed by the render thread.
    CaptureRequest capture_request = CaptureRequest::None;
    int            capture_burst   = 0;  // extra stereo shots to take after the current one
    // Full-resolution still capture request: 0 = none, 1 = left, 2 = right.
    // Serviced on the render thread (reinit camera to max res, grab, reinit back).
    int            fullres_capture_req = 0;

    // Video recording: video_request is posted by input/toast handlers and consumed
    // by the render thread's VideoRecorder; video_recording/paused are status mirrors
    // written by the recorder for the menu/HUD to read.
    VideoRequest video_request   = VideoRequest::None;
    VideoCamera  video_camera    = VideoCamera::Left;
    bool         video_recording = false;
    bool         video_paused    = false;

    // QR / barcode scanning (requires libzbar-dev).
    // qr_scan_main: periodic glReadPixels from OWLsight FBO → ZBar.
    // qr_scan_usb:  scanned in the USB capture thread from the raw BGR frame.
    bool qr_scan_main = false;
    bool qr_scan_usb  = false;
    // Captured QR/barcode log + the directory each capture folder lives under
    // (set once at boot). Guarded by mtx.
    QrCaptureLog qr_captures;
    std::string  qr_dir;

    // Profile requests — posted by deep-menu callbacks (any thread; hold mtx) and
    // consumed by the main loop. Empty string = no request.
    //   profile_save_name   → write the current config snapshot to that profile.
    //   profile_load_name   → relaunch ProtoHUD using that profile (restart).
    //   profile_delete_name → delete that profile file.
    std::string profile_save_name;
    std::string profile_load_name;
    std::string profile_delete_name;

    // HUD/menu preset requests — visual-only (colors + menu style), applied LIVE
    // (no restart, unlike full profiles). Consumed by the main loop. Empty = none.
    std::string hud_preset_save_name;
    std::string hud_preset_load_name;
    std::string hud_preset_delete_name;

    // Quick (corner) menu user-pinned favorites: keys of optional catalog actions
    // the user has chosen to show. Read by the menu's visible_fn (render thread),
    // written by the "Customize Quick Menu" toggles (any thread) — guard with mtx.
    std::set<std::string> quick_favorites;

    // Latest IMU pose (NWU coordinates). Updated by XRDisplay IMU callback.
    ImuPose imu_pose;

    // Camera focus, night vision, resolution, and digital zoom
    CameraFocusState     focus_left, focus_right;
    NightVisionState     night_vision;
    // Boop zones: [0]=Snout, [1]=LeftCheek, [2]=RightCheek. Sane defaults so
    // a user with the sensor wired sees something sensible before they ever
    // open the menu.
    // [0]=Snout, [1]=LeftCheek, [2]=RightCheek, [3]=BothCheeks (derived).
    // Threshold on the BothCheeks slot is unused (it doesn't probe an
    // electrode directly) but kept in the schema for index parity.
    BoopZoneConfig       boop_zones[4] = {
        { true, "surprised", 0.8, 12,  0 },   // Snout      → electrode 0
        { true, "happy",     0.6, 12,  1 },   // LeftCheek  → electrode 1
        { true, "happy",     0.6, 12,  2 },   // RightCheek → electrode 2
        { true, "surprised", 1.0, 12, -1 },   // BothCheeks → derived (no electrode)
    };
    // Coalesce window (seconds) for combining near-simultaneous left + right
    // cheek touches into a single BothCheeks event. Mirror of the sensor's
    // cfg field; the menu writes both this and the sensor's live value.
    float                boop_coalesce_window_s = 0.10f;
    LightSquintConfig    light_squint;
    VoiceMouthConfig     voice_mouth;
    MouthTrackerTestConfig mouth_test;
    ClockConfig          clock_cfg;
    CameraResolutionState camera_resolution;        // left / primary eye
    CameraResolutionState camera_resolution_right;   // right eye (set independently)
    CameraControlsState   camera_controls_left;      // per-eye AF/AE/WB/ISP/HDR
    CameraControlsState   camera_controls_right;
    ZoomCropState        zoom_left, zoom_right;
    MirrorCropState      mirror_crop;
    CamSingleState       cam_single;

    // Post-processing (edge highlight + background desaturation)
    PostProcessConfig    pp_cfg;

    // Timer / alarm state (managed on render thread; no mutex needed for reads)
    TimerAlarmState      timer_alarm;

    // Scheduler / reminders. scheduler_events + scheduler_status are written by the
    // SchedulerMonitor thread under mtx and read on the render thread; lead_min is
    // render-thread-only (menu slider + fire loop).
    std::vector<ScheduledEvent> scheduler_events;   // sorted by start_utc
    SchedulerStatus             scheduler_status;
    int                         scheduler_lead_min = 10;  // reminder lead time (minutes)
    bool                        sched_send_link_startup = false;  // push web link to phone on boot

    // Notification queue — render-thread owned; push from main thread while holding mtx
    NotificationQueue    notifs;
    // Notification-browser filter (menu-driven): type_filter < 0 = all types,
    // else a NotifType value; sender_filter is a case-insensitive substring on
    // the title (empty = any sender).
    int                  notif_type_filter = -1;
    std::string          notif_sender_filter;          // legacy substring (unused by the checklist UI)
    std::set<std::string> notif_sender_sel;            // checklist of senders to show (empty = all)
    bool                 notif_persist = true;   // persist the log to disk across reboots
    std::atomic<bool>    notif_dirty{false};     // a metadata-only edit (e.g. save/pin) wants a flush

    // Particle effects config (render-thread only)
    EffectsConfig        effects_cfg;

    // Map overlay config (render-thread owned; menu writes while holding mtx)
    MapOverlayConfig     map_overlay;

    // Cycling info panel config (render-thread owned; menu writes while holding mtx)
    InfoPanelConfig      info_panel;

    // Top/bottom + swap docking that positions the minimap & info panel as twins.
    HudDock              hud_dock;

    // Expanded-map view options: optionally show the debug panel (opened to the right
    // of the info sidebar) and/or hide the info panel while the map is expanded.
    bool expanded_show_debug = false;
    bool expanded_hide_info  = false;

    // Per-LoRa-node compass marker colors (indexed by node.local_id % 8)
    ImU32 lora_node_colors[8] = {
        IM_COL32(255, 160,  32, 255),  // 0 orange
        IM_COL32(  0, 220, 180, 255),  // 1 teal
        IM_COL32(  0, 180, 255, 255),  // 2 cyan
        IM_COL32( 30, 220,  60, 255),  // 3 green
        IM_COL32(255, 220,   0, 255),  // 4 yellow
        IM_COL32(220,  30, 220, 255),  // 5 purple
        IM_COL32(255,  60,  60, 255),  // 6 red
        IM_COL32(255, 255, 255, 255),  // 7 white
    };

    // System monitor / network / Bluetooth state
    SysMetrics             sys_metrics;
    GpuMetrics             gpu;
    ImuData                imu_data;
    WifiState              wifi;
    PingState              ping;
    SshState               ssh;
    std::vector<BtDevice>  bt_devices;
    SerialMetrics          serial_metrics;

    // Weather (live data written by WeatherMonitor; settings written by menu/config)
    WeatherState           weather;
    WeatherConfig          weather_cfg;
    std::atomic<bool>      weather_refresh { false };  // menu → force an immediate fetch

    // Cached XR display control values (no SDK getter; updated when menu writes).
    int xr_brightness     = 5;   // 1–7; mirrors last xr->set_brightness() call
    int xr_dimming        = 5;   // 0–9; mirrors last xr->set_dimming() call
    int xr_hud_brightness = 5;   // 1–9; mirrors last xr->set_hud_brightness() call

    // FPS display smoothing: EMA time constant in seconds (1 / 5 / 10).
    // Written by menu (render thread); read in render loop EMA computation.
    int fps_avg_interval_s = 1;

    // I2C bus scanner — triggered from menu, results written by background thread.
    std::vector<uint8_t> i2c_scan_results;
    bool                 i2c_scan_busy = false;
    std::string          i2c_scan_bus  = "/dev/i2c-1";

    // GPIO pin monitor — populated at startup from config; values updated ~1 Hz.
    struct GpioPinState { int pin; int value; };  // value: 0/1, -1=unavailable
    std::vector<GpioPinState> gpio_states;

    // Signals render thread to quit.
    std::atomic<bool> quit { false };

    // Window mode (desktop/dev): menu toggles set these + win_mode_dirty; the render
    // loop applies them via XRDisplay::apply_window_mode on the main thread. Ignored
    // while the glasses are connected (the window lives on the glasses monitor).
    std::atomic<bool> win_fullscreen { false };
    std::atomic<bool> win_frameless  { false };
    std::atomic<bool> win_mode_dirty { false };
    // Windowed resolution (desktop/dev): menu sets w/h + dirty; the render loop
    // applies glfwSetWindowSize on the main thread. Ignored on the glasses.
    std::atomic<int>  win_resize_w     { 0 };
    std::atomic<int>  win_resize_h     { 0 };
    std::atomic<bool> win_resize_dirty { false };

    // QR scan mute: set to future epoch-seconds to suppress notifications temporarily.
    std::atomic<int64_t> qr_mute_until_s { 0 };

    // ── Helpers (call with mutex held) ────────────────────────────────────────

    void upsert_lora_node(const LoRaNode& n) {
        for (auto& existing : lora_nodes) {
            if (existing.local_id == n.local_id) { existing = n; return; }
        }
        lora_nodes.push_back(n);
    }

    // Merge name / full node_id into an existing entry without clobbering
    // heading/distance (NODE_INFO arrives independently of LOCATION_UPDATE).
    void upsert_lora_node_info(uint8_t local_id, uint32_t full_node_id,
                                const std::string& name) {
        for (auto& existing : lora_nodes) {
            if (existing.local_id == local_id) {
                existing.node_id = full_node_id;
                existing.name    = name;
                return;
            }
        }
        // Node not yet seen via LOCATION_UPDATE — pre-populate so name is
        // available as soon as the first fix arrives.
        LoRaNode n;
        n.local_id = local_id;
        n.node_id  = full_node_id;
        n.name     = name;
        lora_nodes.push_back(n);
    }

    void push_lora_message(LoRaMessage m) {
        lora_messages.push_front(std::move(m));
        while (lora_messages.size() > max_messages)
            lora_messages.pop_back();
    }

    int unread_message_count() const {
        int c = 0;
        for (const auto& m : lora_messages) c += !m.read;
        return c;
    }
};
