#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>
#include <ctime>
#include <imgui.h>

// ── Post-processing config ────────────────────────────────────────────────────
// Modified from menu (any thread); read by the render thread via snap.
// Simple struct — individual fields are written atomically enough for a bool/float.

struct PostProcessConfig {
    bool  edge_enabled       = false;
    float edge_strength      = 0.7f;
    ImU32 edge_color         = IM_COL32(255, 160, 32, 255);
    bool  desat_enabled      = false;
    float desat_strength     = 0.8f;
    float contrast_threshold = 0.15f;  // 0.07 = aggressive, 0.25 = subtle
};

// ── Overlay layout config (PiP and Android mirror) ───────────────────────────
// Modified at runtime by menu actions; read exclusively on the render thread,
// so no mutex is needed.

struct OverlayConfig {
    enum class Anchor {
        TOP_LEFT, TOP_CENTER, TOP_RIGHT,
        BOTTOM_LEFT, BOTTOM_CENTER, BOTTOM_RIGHT
    };
    Anchor anchor = Anchor::TOP_CENTER;
    float  size   = 0.25f;   // fraction of screen height
};

// ── Sub-states ────────────────────────────────────────────────────────────────

struct FaceState {
    uint8_t  effect_id    = 0;
    uint8_t  gif_id       = 0;
    uint8_t  r = 0, g = 220, b = 180;
    uint8_t  brightness   = 200;
    uint8_t  palette_id   = 0;
    bool     playing_gif  = false;
    bool     connected    = false;
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
};

struct SystemHealth {
    bool teensy_ok       = false;
    bool lora_ok         = false;
    bool knob_ok         = false;
    bool knob_ready      = false;  // Motor calibration complete
    bool cam_owl_left    = false;
    bool cam_owl_right   = false;
    bool cam_usb1        = false;
    bool cam_usb2        = false;
    bool audio_ok        = false;  // Spatial audio engine running
    bool android_mirror  = false;  // scrcpy connected and streaming
    bool mpu9250_ok      = false;  // MPU-9250 backup compass running
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
    float exposure_ev = 0.0f;  // -3.0 to +3.0
    int   shutter_us  = 33333; // microseconds (40 to 1000000)
    bool  nv_enabled  = false; // night vision preset active (HUD indicator + apply flag)
};

struct CameraResolutionState {
    int width  = 1280;
    int height = 800;
    int fps    = 60;
};

struct ImuPose {
    float roll  = 0.0f;
    float pitch = 0.0f;
    float yaw   = 0.0f;
};

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
    bool  compass_bg_enabled = false;

    // Latest IMU pose (NWU coordinates). Updated by XRDisplay IMU callback.
    ImuPose imu_pose;

    // Camera focus, night vision, and resolution control
    CameraFocusState     focus_left, focus_right;
    NightVisionState     night_vision;
    CameraResolutionState camera_resolution;

    // Post-processing (edge highlight + background desaturation)
    PostProcessConfig    pp_cfg;

    // Cached XR display control values (no SDK getter; updated when menu writes).
    int xr_brightness     = 5;   // 1–7; mirrors last xr->set_brightness() call
    int xr_dimming        = 5;   // 0–9; mirrors last xr->set_dimming() call
    int xr_hud_brightness = 5;   // 1–9; mirrors last xr->set_hud_brightness() call

    // Signals render thread to quit.
    std::atomic<bool> quit { false };

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
