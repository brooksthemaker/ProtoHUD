#pragma once
// ── native_face_controller.h ───────────────────────────────────────────────────
// Native C++ Protoface backend. Implements ProtoHUD's IFaceController so it drops
// into the existing FaceProxy/menu seam, renders the LED face in-process on a
// worker thread (no Python daemon, no control socket), exposes the latest canvas
// for the in-HUD preview, and emits frames to a swappable PanelOutput.
//
// Scope note (Phase 1–2): face + materials + compositing are ported. Particles,
// GIF playback, and the C++ Piomatter output are stubbed/TODO for later phases.

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

#include "../serial/face_controller.h"
#include "expression_style.h"
#include "eye_anim.h"
#include "face_config.h"
#include "glitch.h"         // GlitchEffect + GlitchConfig
#include "panel_output.h"   // PanelOutput + NamedRegion
#include "scroll_text.h"    // ScrollText + ScrollTextConfig

namespace face {

class FaceState;
class FaceLoader;
class BaseMaterial;
class ParticleSystem;
class GifPlayer;

class NativeFaceController : public IFaceController {
public:
    NativeFaceController(RenderConfig cfg, std::unique_ptr<PanelOutput> output);
    ~NativeFaceController() override;

    // IFaceController
    bool start() override;
    void stop()  override;
    bool connected() const override { return running_.load(); }

    void set_color(uint8_t r, uint8_t g, uint8_t b, uint8_t layer = 0) override;
    void set_effect(uint8_t effect_id, uint8_t p1 = 0, uint8_t p2 = 0) override;
    // Push an arbitrary particle-system spec — either a {"layers": [...]} dict
    // or a single-effect spec — directly to every panel's ParticleSystem.
    // Used by the Layered Effects builder in the menu so the user isn't
    // limited to the canned effect_id mapping.
    void set_effect_json(const nlohmann::json& spec);
    // Read back the effect currently applied to the panels (the running
    // particle spec, restored from protoface_state.json at boot). Lets the
    // menu's Layered builder seed its editable fields from what's actually
    // rendering instead of showing empty defaults. Returns "none" if no panel
    // has a particle system yet.
    nlohmann::json get_effect_json() const;
    void set_face(uint8_t face_id) override;
    void play_gif(uint8_t gif_id) override;
    void set_brightness(uint8_t value) override;
    void set_palette(uint8_t palette_id) override;
    void set_menu_item(uint8_t menu_index, uint8_t value) override;
    void request_status() override {}
    void release_control() override;
    void save_config() override;          // persist current look now (also auto-saved)

    // Slot manifest — see IFaceController. Bindings store basenames inside
    // gifs_dir; play_gif() prefers the bound file, falling back to the sorted
    // scan when a slot is unbound or its file has gone missing.
    std::string gif_slot(uint8_t slot) const override;
    void        bind_gif_slot(uint8_t slot, const std::string& filename) override;
    void        clear_gif_slot(uint8_t slot) override;

    // Face expression image management (see IFaceController). Operates on the
    // active face folder of the first non-mirror panel (the "main" face in the
    // standard 2-panel mirrored layout). Imports copy the source PNG to the
    // canonical expression filename and rebuild the affected panels' loaders
    // so the change takes effect on the next frame.
    std::string face_image_path(const std::string& expression) const override;
    bool        face_image_exists(const std::string& expression) const override;
    bool        import_face_image(const std::string& expression,
                                  const std::string& src_path) override;
    void        clear_face_image(const std::string& expression) override;
    void        reload_faces() override { reload_active_face(); }
    void        set_face_by_name(const std::string& expression) override;
    std::string current_expression() const override;
    void        next_expression() override;
    void        prev_expression() override;
    void        trigger_boop(const std::string& expression, double duration_s) override;
    void        trigger_boop_ripple(int zone) override;   // expanding ring at the zone
    void        play_eye_animation(int type, double speed, double size,
                                   uint8_t r, uint8_t g, uint8_t b,
                                   double duration_s) override;
    void        set_audio_drive(double volume, double mouth_open) override;
    void        set_motion(double heading_deg, double yaw_rate, double pitch_deg,
                           double roll_deg, double accel_g) override;
    void        set_mouth_shape(const std::string& shape) override;

    // Expression-coupled effects: when enabled, the active particle effect is
    // swapped to a mood preset as the face expression changes (angry→fire,
    // happy→celebration, sad→rain, shocked→galaxy), restoring the user's chosen
    // base effect for neutral/unmapped expressions and when disabled.
    void        set_expression_effects(bool enabled);

    // Per-expression styles: any expression (built-in or custom) may carry its
    // own material / particle effect / glitch, overriding the default look
    // while it is active and restoring it on switch-away. Unset fields
    // inherit. set_expression_style re-applies immediately when `expr` is the
    // current expression. The override slot (set_style_override) is stronger
    // still — it wins over the current expression's own style until cleared;
    // custom-expression activation and the editor's live preview use it.
    void            set_expression_style(const std::string& expr,
                                         const ExpressionStyle& s) override;
    ExpressionStyle expression_style(const std::string& expr) const override;
    void            clear_expression_style(const std::string& expr) override;
    std::map<std::string, ExpressionStyle> all_expression_styles() const override;
    void            set_style_override(const ExpressionStyle& s) override;
    void            clear_style_override() override;

    // Motion-reactive particles: when on, directional effects (rain/snow/
    // embers/steam/…) default to real-gravity coupling — they lean with head
    // roll and sweep on quick turns. Forwarded to every panel's ParticleSystem.
    void        set_motion_particles(bool on);

    // Face Inertia: the whole face slides opposite quick head motion and
    // springs back with a small overshoot, like it has mass - eyes lag on a
    // fast turn and bob on a nod. Strength scales the maximum slide
    // (1.0 = up to ~10% of the panel).
    void        set_face_inertia(bool on);
    void        set_face_inertia_strength(double s);

    // Ambient override: an effect spec shown INSTEAD of the user's base
    // effect while set (expression moods still win on top of it). An empty/
    // null spec clears it. Driven by the ambient sync in main — Weather Sync
    // (WMO code → rain/snow/thunderstorm/…) and Temp Effects (freezing →
    // frost, scorching → heatwave) share this one slot.
    void        set_ambient_effect(const nlohmann::json& spec);

    // Face editor support: what the active PanelOutput addresses (so the
    // editor can pick the editable region), and a way to force a face
    // reload after the editor writes a PNG.
    std::vector<cv::Rect>    led_covered_regions() const;
    std::vector<NamedRegion> led_named_regions()   const;
    void                     reload_active_face();

    // Surface to IFaceController: true iff the active PanelOutput exposes
    // sub-region coverage info (MAX7219 / RGB matrix backends).
    bool has_led_face_editor() const override;

    // HUB75 named-layout tagging. main pushes the active layout name in here
    // and import_face_image stamps the face folder's config.json with it
    // (unless already tagged). face_image_layout reads back the stored tag —
    // empty string for untagged / legacy folders.
    void        set_active_layout_name(const std::string& name) override;
    std::string face_image_layout(const std::string& expression) const override;

    // Material spec string for a Material Color preset index (0-33; pride
    // flags return their smooth vertical variant). Used by the menu to build
    // the expression editor's material vocabulary.
    static std::string preset_material_spec(int idx) { return preset_material(idx); }

    // Copy the latest rendered RGB canvas (CV_8UC3) for the preview. Returns
    // false until the first frame exists. Safe to call from the main/GL thread.
    bool latest_frame(cv::Mat& out) const;

    int canvas_width()  const { return cfg_.canvas_w; }
    int canvas_height() const { return cfg_.canvas_h; }

    // Animation tuning — forwarded to every panel's FaceState so the
    // changes take effect on the next update tick. The menu pushes these
    // when the user adjusts the corresponding slider/toggle.
    void set_blink_enabled(bool enabled);
    // Hold the eyes shut (asleep) / release them. Uses the blink art at full
    // weight, so it works without a dedicated closed-eye expression.
    void set_eyes_closed(bool closed);
    void set_blink_timing(double min_s, double max_s, double duration_s);
    void set_expression_fade(double seconds);
    void set_wiggle(const WiggleCfg& w);
    // Live per-panel orientation flips (HUB75 layout Flip X / Flip Y). flips[i]
    // = {flip_x, flip_y} for panel i; extra/missing entries are ignored. Read by
    // the render thread on the next frame.
    void set_panel_flips(const std::vector<std::array<bool, 2>>& flips);
    // Apply an arbitrary material spec (e.g. "gradient:h:s:20:00DCB4-0064FF")
    // to every self-rendered panel and persist it. Used by the Material Color
    // gradient editor for live preview.
    void set_material_spec(const std::string& spec);
    // Face color pass-through: when true, draw the face's own RGBA art verbatim
    // (color faces); when false the selected material tints the face (default).
    // Also drives the editor's Color vs Mono canvas at open time (see main.cpp).
    void set_face_colors(bool on) { face_colors_.store(on); }
    bool face_colors() const      { return face_colors_.load(); }
    // Live "glitch" post-effect config — corrupts the composited face (chromatic
    // split, tearing, blocks, bitcrush, dropout, datamosh, region desync, and an
    // occasional wrong-expression flash). Read by the render thread each frame.
    void set_glitch(const GlitchConfig& cfg);

    // Live scrolling-text banner (see scroll_text.h): a marquee drawn onto the
    // composited canvas above every layer including glitch, so it spans all
    // panels and stays legible.
    void set_scroll_text(const ScrollTextConfig& cfg) { scroll_text_.set_config(cfg); }
    ScrollTextConfig scroll_text() const              { return scroll_text_.config(); }

    // Push a "transient" image for the named expression onto every panel
    // for duration_s seconds. The current image is stashed and restored
    // automatically when the timer expires (or on stop()). Used by the
    // face editor's "Preview to panels" key so the user can see their
    // unsaved canvas on the physical LEDs for a moment.
    void push_transient_face(const std::string& expression,
                             const cv::Mat& rgba_canvas,
                             double duration_s);

private:
    struct Panel {
        PanelCfg                      cfg;
        std::unique_ptr<FaceState>     state;     // null for mirror panels
        std::unique_ptr<FaceLoader>    loader;    // null for mirror panels
        std::shared_ptr<BaseMaterial>  material;  // null for mirror panels
        std::unique_ptr<ParticleSystem> particles; // null for mirror panels
        std::unique_ptr<GifPlayer>     gif;        // null for mirror panels
        double gif_release_timer = 0.0;            // s left before auto-revert (0 = off)
        // Per-expression style material — rendered INSTEAD of `material` while
        // set. The persisted base (`material`/`material_spec`) stays intact, so
        // switching to an unstyled expression restores it with no bookkeeping.
        std::shared_ptr<BaseMaterial>  style_material;
        std::string    material_spec;              // current material spec (for persistence)
        nlohmann::json particles_spec = "none";    // current particle config (for persistence)
        bool is_mirror = false;
        bool face_mirror = false;   // flip the face layer (continuous_effects un-mirror)
        int  src_index = -1;
    };

    void build_panels();
    void render_thread();
    // Resolve and apply the full look for `expr`: default look → the
    // expression's own style → the override slot. Covers material (per-panel
    // style_material), particle effect (transient set_effect), and glitch
    // (glitch_active_). The legacy expr_effect_map_ mood coupling remains the
    // effect fallback when the resolved style has none. Caller holds state_mtx_.
    void apply_expression_style_locked(const std::string& expr);
    // The spec the panels return to when no mood override is active: the
    // weather override while one is set, else the panel's own base effect.
    // Caller holds state_mtx_.
    const nlohmann::json& base_particles_locked(const Panel& pn) const;
    void apply_material_all(const std::string& name);   // caller holds state_mtx_
    void cycle_expression(int dir);                     // +1 next / -1 prev; mirrors to all panels
    // Auto-save is split so the file write happens outside state_mtx_ (the
    // render thread holds it for whole ticks; blocking it on disk I/O froze
    // the face). serialize under the lock, write after releasing it.
    std::string serialize_state_locked() const;          // build the state JSON; caller holds state_mtx_
    void        write_state_file(const std::string& json_text) const; // tmp+rename; call WITHOUT the lock
    void load_state();                                  // overlay saved look at startup
    static std::string preset_material(int idx);
    // preset_material(idx), then apply the pride-flag preferences: idx 22-33
    // render as hard-edged stripes when pride_sharp_ is set (smooth otherwise)
    // and rotated to pride_angle_ degrees. Non-pride presets are returned as-is.
    std::string material_for_index(int idx) const;

    RenderConfig                 cfg_;
    std::unique_ptr<PanelOutput> output_;
    std::vector<Panel>           panels_;
    std::vector<std::string>     gif_files_;   // sorted *.gif paths in gifs_dir
    std::vector<std::string>     gif_slots_;   // size 8, basename per slot ("" = unbound)
    double                       gif_release_ = 5.0;   // auto-revert seconds

    // Cached results for face_image_exists / face_image_layout. The menu's
    // label_fn/visible_fn hooks call those dozens of times per frame, and a
    // raw probe is a stat plus a config.json open+parse — formerly taken
    // under state_mtx_, contending with the face render thread. Entries
    // refresh on a short TTL and the cache clears on import/clear/reload so
    // edits show immediately.
    struct FaceProbe {
        bool        exists = false;
        std::string layout;
        std::chrono::steady_clock::time_point t{};
    };
    FaceProbe probe_face_image(const std::string& expression) const;
    void      invalidate_face_probes();
    mutable std::mutex                       probe_mtx_;
    mutable std::map<std::string, FaceProbe> probe_cache_;

    mutable std::mutex state_mtx_;   // panels_ mutation + render reads
    mutable std::mutex frame_mtx_;   // latest_
    cv::Mat            latest_;
    bool               have_frame_ = false;
    // Transient face overlays — one record per panel a push_transient_face
    // call has touched. tick_render_locked() restores them as their deadlines
    // pass. stop() restores any still-active records before tearing down.
    struct TransientFace {
        int         panel_idx = -1;
        std::string expression;
        cv::Mat     original_image;     // RGBA, sized to (panel.w, panel.h)
        std::chrono::steady_clock::time_point deadline{};
    };
    std::vector<TransientFace> transient_faces_;

    // Procedural "animated eye" reaction (boop rapid-trigger easter egg). While
    // eye_anim_timer_ > 0 the panels render the animation instead of the face,
    // and effects are suppressed. Lives under state_mtx_.
    EyeAnimParams      eye_anim_;
    double             eye_anim_timer_ = 0.0;   // seconds remaining
    double             eye_anim_t_     = 0.0;   // elapsed seconds (animation phase)

    // Glitch post-effect. glitch_ (sim state) is touched only by the render
    // thread; glitch_cfg_ (the default look's config) is written by
    // set_glitch() under state_mtx_. glitch_active_ is the RESOLVED config —
    // the active expression style's glitch when it has one, else glitch_cfg_ —
    // recomputed by apply_expression_style_locked and read by the render loop.
    GlitchEffect       glitch_;
    GlitchConfig       glitch_cfg_;
    GlitchConfig       glitch_active_;

    // Per-expression styles + the stronger override slot (custom expressions,
    // editor live preview). Live under state_mtx_.
    std::map<std::string, ExpressionStyle> expr_styles_;
    ExpressionStyle    style_override_;
    bool               override_active_ = false;

    // Scrolling-text banner. Thread-safe internally (own mutex): tick/render
    // run on the render thread, set_scroll_text() from menu/config threads.
    ScrollText         scroll_text_;

    // Expression → mood-preset coupling (off by default). expr_effects_ gates
    // it; current_expression_ tracks the latest set face so toggling re-applies
    // correctly. Lives under state_mtx_.
    bool                                          expr_effects_ = false;
    std::string                                   current_expression_;
    std::map<std::string, std::string>            expr_effect_map_;

    std::thread        thread_;
    std::atomic<bool>  running_{false};
    std::atomic<bool>  face_colors_{false};   // draw art's own RGB vs material override
    std::atomic<bool>  pride_sharp_{true};    // pride flags: hard bands vs smooth blend
    std::atomic<bool>  motion_particles_{true};  // gravity/slosh coupling for effects
    std::atomic<bool>   face_inertia_{true};          // whole-face motion slide
    std::atomic<double> face_inertia_strength_{1.0};  // 0..2 - scales the max slide
    // Face Inertia spring state - only ever touched by the render thread.
    double inertia_x_  = 0.0, inertia_y_  = 0.0;  // normalised offset, +-1.5 max
    double inertia_vx_ = 0.0, inertia_vy_ = 0.0;
    double inertia_prev_pitch_ = 0.0;
    // Slow-tracking roll baseline (tau ~4 s): the spring uses roll RELATIVE to
    // this so mounting tilt / head posture doesn't park the face off-centre —
    // a deliberate lean still throws it for a couple of seconds, then the
    // baseline catches up and the face re-centres.
    double inertia_roll_base_  = 0.0;
    bool   inertia_prev_valid_ = false;
    nlohmann::json     ambient_spec_;         // ambient override (guarded by state_mtx_)
    std::atomic<int>   pride_angle_{90};      // pride flag stripe rotation, degrees

    // Per-frame drive inputs written by other threads (audio thread, IMU
    // pump) at high rate. Atomics instead of state_mtx_: the render thread
    // holds that mutex for its entire compositing pass (multiple ms), so
    // taking it per audio/motion sample stalled the producer threads. The
    // render thread forwards the latest values into each panel every tick.
    std::atomic<double> audio_volume_{0.0};
    std::atomic<double> audio_mouth_{0.0};
    std::atomic<double> motion_heading_{0.0};
    std::atomic<double> motion_yaw_rate_{0.0};
    std::atomic<double> motion_pitch_{0.0};
    std::atomic<double> motion_roll_{0.0};
    std::atomic<double> motion_accel_{1.0};   // ≈1 g at rest (MotionInput default)

    // Name of the currently-active HUB75 layout (or "" when unset). Used to
    // stamp face folders on import_face_image and surfaced via
    // face_image_layout. Lives under state_mtx_.
    std::string        active_layout_name_;
};

} // namespace face
