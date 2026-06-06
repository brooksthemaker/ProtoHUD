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
#include "eye_anim.h"
#include "face_config.h"
#include "panel_output.h"   // PanelOutput + NamedRegion

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
    void        trigger_boop(const std::string& expression, double duration_s) override;
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

    // Face editor support: what the active PanelOutput addresses (so the
    // editor can pick the editable region), and a way to force a face
    // reload after the editor writes a PNG.
    std::vector<cv::Rect>    led_covered_regions() const;
    std::vector<NamedRegion> led_named_regions()   const;
    void                     reload_active_face();

    // Panel override: while set, this RGBA frame fills the whole canvas instead
    // of the face composite (used by game mode to show a game on the panels).
    // Resized to the canvas with nearest-neighbour; thread-safe.
    void                     set_panel_override(const cv::Mat& rgba);
    void                     clear_panel_override();

    // Surface to IFaceController: true iff the active PanelOutput exposes
    // sub-region coverage info (MAX7219 / RGB matrix backends).
    bool has_led_face_editor() const override;

    // HUB75 named-layout tagging. main pushes the active layout name in here
    // and import_face_image stamps the face folder's config.json with it
    // (unless already tagged). face_image_layout reads back the stored tag —
    // empty string for untagged / legacy folders.
    void        set_active_layout_name(const std::string& name) override;
    std::string face_image_layout(const std::string& expression) const override;

    // Copy the latest rendered RGB canvas (CV_8UC3) for the preview. Returns
    // false until the first frame exists. Safe to call from the main/GL thread.
    bool latest_frame(cv::Mat& out) const;

    int canvas_width()  const { return cfg_.canvas_w; }
    int canvas_height() const { return cfg_.canvas_h; }

    // Animation tuning — forwarded to every panel's FaceState so the
    // changes take effect on the next update tick. The menu pushes these
    // when the user adjusts the corresponding slider/toggle.
    void set_blink_enabled(bool enabled);
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
        std::string    material_spec;              // current material spec (for persistence)
        nlohmann::json particles_spec = "none";    // current particle config (for persistence)
        bool is_mirror = false;
        bool face_mirror = false;   // flip the face layer (continuous_effects un-mirror)
        int  src_index = -1;
    };

    void build_panels();
    void render_thread();
    // Apply the mood preset mapped to `expr` (or restore each panel's base
    // particles_spec when neutral/unmapped). Caller holds state_mtx_.
    void apply_expression_effect_locked(const std::string& expr);
    void apply_material_all(const std::string& name);   // caller holds state_mtx_
    void save_state_locked() const;                     // auto-save look; caller holds state_mtx_
    void load_state();                                  // overlay saved look at startup
    static std::string preset_material(int idx);

    RenderConfig                 cfg_;
    std::unique_ptr<PanelOutput> output_;
    std::vector<Panel>           panels_;
    std::vector<std::string>     gif_files_;   // sorted *.gif paths in gifs_dir
    std::vector<std::string>     gif_slots_;   // size 8, basename per slot ("" = unbound)
    double                       gif_release_ = 5.0;   // auto-revert seconds

    mutable std::mutex state_mtx_;   // panels_ mutation + render reads
    mutable std::mutex frame_mtx_;   // latest_
    cv::Mat            latest_;
    bool               have_frame_ = false;
    mutable std::mutex override_mtx_;   // panel_override_
    cv::Mat            panel_override_;  // non-empty → fills the canvas (game mode)
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

    // Expression → mood-preset coupling (off by default). expr_effects_ gates
    // it; current_expression_ tracks the latest set face so toggling re-applies
    // correctly. Lives under state_mtx_.
    bool                                          expr_effects_ = false;
    std::string                                   current_expression_;
    std::map<std::string, std::string>            expr_effect_map_;

    std::thread        thread_;
    std::atomic<bool>  running_{false};

    // Name of the currently-active HUB75 layout (or "" when unset). Used to
    // stamp face folders on import_face_image and surfaced via
    // face_image_layout. Lives under state_mtx_.
    std::string        active_layout_name_;
};

} // namespace face
