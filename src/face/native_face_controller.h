#pragma once
// ── native_face_controller.h ───────────────────────────────────────────────────
// Native C++ Protoface backend. Implements ProtoHUD's IFaceController so it drops
// into the existing FaceProxy/menu seam, renders the LED face in-process on a
// worker thread (no Python daemon, no control socket), exposes the latest canvas
// for the in-HUD preview, and emits frames to a swappable PanelOutput.
//
// Scope note (Phase 1–2): face + materials + compositing are ported. Particles,
// GIF playback, and the C++ Piomatter output are stubbed/TODO for later phases.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

#include "../serial/face_controller.h"
#include "face_config.h"

namespace face {

class FaceState;
class FaceLoader;
class BaseMaterial;
class PanelOutput;
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
    void        set_face_by_name(const std::string& expression) override;
    void        trigger_boop(const std::string& expression, double duration_s) override;

    // Copy the latest rendered RGB canvas (CV_8UC3) for the preview. Returns
    // false until the first frame exists. Safe to call from the main/GL thread.
    bool latest_frame(cv::Mat& out) const;

    int canvas_width()  const { return cfg_.canvas_w; }
    int canvas_height() const { return cfg_.canvas_h; }

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
        int  src_index = -1;
    };

    void build_panels();
    void render_thread();
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

    std::thread        thread_;
    std::atomic<bool>  running_{false};
};

} // namespace face
