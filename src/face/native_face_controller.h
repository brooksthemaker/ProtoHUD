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
    void save_config() override {}        // TODO: persist look to ProtoHUD config

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
        bool is_mirror = false;
        int  src_index = -1;
    };

    void build_panels();
    void render_thread();
    void apply_material_all(const std::string& name);   // caller holds state_mtx_
    static std::string preset_material(int idx);

    RenderConfig                 cfg_;
    std::unique_ptr<PanelOutput> output_;
    std::vector<Panel>           panels_;

    mutable std::mutex state_mtx_;   // panels_ mutation + render reads
    mutable std::mutex frame_mtx_;   // latest_
    cv::Mat            latest_;
    bool               have_frame_ = false;

    std::thread        thread_;
    std::atomic<bool>  running_{false};
};

} // namespace face
