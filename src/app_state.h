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
    uint8_t  material_color  = 0;   // SET_MENU_ITEM(8) index 0-11
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
enum class InfoWidget : uint8_t { Clock = 0, Notifications, Schedule, Weather, Count };

struct InfoPanelConfig {
    bool  enabled   = false;    // off by default; user opts in per side
    float anchor_x  = 0.85f;    // screen fraction — right side, mirroring a left minimap
    float anchor_y  = 0.5f;
    float pan_x     = 0.f;
    float pan_y     = 0.f;
    float size_px   = 150.f;    // half-extent (radius), matching the minimap footprint
    float cycle_sec = 6.f;      // dwell per widget before advancing
    // Which widgets take part in the cycle (indexed by InfoWidget).
    bool  show[static_cast<int>(InfoWidget::Count)] = { true, true, true, false };
};

// ── HUD dock layout ─────────────────────────────────────────────────────────────
// The minimap and info panel are opposite-side twins docked to the top or bottom
// edge. `bottom` picks the edge; `swap` exchanges which side each occupies. Anchors
// for both elements are derived from this (see apply_hud_dock in main).
struct HudDock {
    bool bottom = false;   // false = top edge, true = bottom edge
    bool swap   = false;   // false = minimap left / panel right, true = swapped
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
};

struct SshState {
    bool active = false;
    int  port   = 22;
};

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
        while (static_cast<int>(items.size()) > kMax) items.pop_back();
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

    // Heading used for the HUD compass. Updated by LoRa or IMU.
    float compass_heading    = 0.0f;
    bool  compass_bg_enabled = true;
    bool  compass_tape       = true;   // the top-of-screen compass tape

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
    ClockConfig          clock_cfg;
    CameraResolutionState camera_resolution;
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

    // Notification queue — render-thread owned; push from main thread while holding mtx
    NotificationQueue    notifs;

    // Particle effects config (render-thread only)
    EffectsConfig        effects_cfg;

    // Map overlay config (render-thread owned; menu writes while holding mtx)
    MapOverlayConfig     map_overlay;

    // Cycling info panel config (render-thread owned; menu writes while holding mtx)
    InfoPanelConfig      info_panel;

    // Top/bottom + swap docking that positions the minimap & info panel as twins.
    HudDock              hud_dock;

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
