#include "native_face_controller.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <opencv2/imgproc.hpp>

#include "face_state.h"
#include "face_loader.h"
#include "materials.h"
#include "renderer.h"
#include "particles.h"
#include "panel_output.h"

namespace face {

namespace {
// Numeric effect id → particle config, mirroring Protoface ipc.py _EFFECT_MAP.
nlohmann::json effect_cfg_for_id(int id) {
    switch (id) {
        case 0:  return std::string("none");
        case 1:  return std::string("sparkle");
        case 2:  return std::string("embers");
        case 3:  return std::string("rain");
        case 4:  return std::string("snow");
        case 5:  return std::string("confetti");
        case 6:  return std::string("rings");
        case 7:  return std::string("fireflies");
        case 8:  return nlohmann::json{{"preset", "fire"}};
        case 9:  return nlohmann::json{{"preset", "aurora"}};
        case 10: return nlohmann::json{{"preset", "blizzard"}};
        case 11: return nlohmann::json{{"preset", "sonar"}};
        case 12: return nlohmann::json{{"preset", "plasma"}};
        case 13: return nlohmann::json{{"preset", "celebration"}};
        case 14: return nlohmann::json{{"preset", "galaxy"}};
        case 15: return nlohmann::json{{"preset", "party"}};
        case 16: return std::string("clouds");
        case 17: return nlohmann::json{{"preset", "nebula"}};
        default: return std::string("none");
    }
}
} // namespace

NativeFaceController::NativeFaceController(RenderConfig cfg,
                                          std::unique_ptr<PanelOutput> output)
    : cfg_(std::move(cfg)), output_(std::move(output)) {
    build_panels();
}

NativeFaceController::~NativeFaceController() { stop(); }

void NativeFaceController::build_panels() {
    panels_.clear();
    for (const PanelCfg& pc : cfg_.panels) {
        Panel pn;
        pn.cfg = pc;
        if (pc.mirror_of.empty()) {
            pn.loader = std::make_unique<FaceLoader>(
                cfg_.faces_dir + "/" + pc.face.active, pc.w, pc.h);
            pn.state = std::make_unique<FaceState>(
                pc.face, pn.loader->expression_names());
            pn.material = load_material(pc.material.active, pc.w, pc.h,
                                        pc.material.scroll_x, pc.material.scroll_y,
                                        cfg_.materials_dir);
            pn.particles = std::make_unique<ParticleSystem>(pc.w, pc.h, pc.particles);
        } else {
            pn.is_mirror = true;
        }
        panels_.push_back(std::move(pn));
    }
    // Resolve mirror sources by panel name.
    for (auto& pn : panels_) {
        if (!pn.is_mirror) continue;
        for (size_t i = 0; i < panels_.size(); ++i)
            if (panels_[i].cfg.name == pn.cfg.mirror_of) { pn.src_index = static_cast<int>(i); break; }
    }
}

bool NativeFaceController::start() {
    if (running_.load()) return true;
    if (output_) output_->open();
    running_.store(true);
    thread_ = std::thread(&NativeFaceController::render_thread, this);
    return true;
}

void NativeFaceController::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (output_) output_->close();   // blank the panels on shutdown
}

void NativeFaceController::render_thread() {
    using clock = std::chrono::steady_clock;
    const double target_dt = 1.0 / std::max(1, cfg_.fps);
    auto prev = clock::now();

    while (running_.load()) {
        auto now = clock::now();
        double dt = std::chrono::duration<double>(now - prev).count();
        prev = now;
        dt = std::min(dt, 0.1);

        cv::Mat canvas(cfg_.canvas_h, cfg_.canvas_w, CV_8UC3,
                       cv::Scalar(cfg_.background[0], cfg_.background[1],
                                  cfg_.background[2]));
        {
            std::lock_guard<std::mutex> lk(state_mtx_);

            // Render each self-rendering panel into its region.
            for (auto& pn : panels_) {
                if (pn.is_mirror || !pn.state) continue;
                if (!pn.loader || !pn.loader->valid()) continue;
                const PanelCfg& pc = pn.cfg;

                pn.state->update(dt);
                pn.material->update(dt);
                if (pn.particles) pn.particles->update(dt);

                cv::Mat bg  = solid_layer(cfg_.background[0], cfg_.background[1],
                                          cfg_.background[2], pc.w, pc.h);
                cv::Mat mat = pn.material->get_frame();
                cv::Mat face_rgba  = pn.loader->get_frame(*pn.state);
                cv::Mat face_layer = apply_material(face_rgba, mat);

                std::vector<Layer> layers{ Layer{face_layer, Blend::Normal} };
                if (pn.particles) {
                    ParticleFrame pf = pn.particles->render();
                    if (pf.has) layers.push_back(Layer{pf.rgba, pf.blend});
                }
                // TODO(Phase 3): append GIF layer (overrides face) here.

                cv::Mat frame = composite(bg, layers);
                frame = scale_brightness(frame, pn.state->brightness());

                cv::Rect roi(pc.x, pc.y, pc.w, pc.h);
                if ((roi & cv::Rect(0, 0, canvas.cols, canvas.rows)) == roi)
                    frame.copyTo(canvas(roi));
            }

            // Mirror panels copy a horizontally-flipped source region.
            for (auto& pn : panels_) {
                if (!pn.is_mirror || pn.src_index < 0) continue;
                const PanelCfg& src = panels_[pn.src_index].cfg;
                const PanelCfg& dst = pn.cfg;
                cv::Rect sroi(src.x, src.y, src.w, src.h);
                cv::Rect droi(dst.x, dst.y, dst.w, dst.h);
                cv::Rect bounds(0, 0, canvas.cols, canvas.rows);
                if ((sroi & bounds) != sroi || (droi & bounds) != droi) continue;
                if (sroi.size() != droi.size()) continue;
                cv::Mat flipped;
                cv::flip(canvas(sroi), flipped, 1);
                flipped.copyTo(canvas(droi));
            }
        }

        {
            std::lock_guard<std::mutex> lk(frame_mtx_);
            canvas.copyTo(latest_);
            have_frame_ = true;
        }
        if (output_) output_->show(canvas);

        double elapsed = std::chrono::duration<double>(clock::now() - now).count();
        double sleep_s = target_dt - elapsed;
        if (sleep_s > 0)
            std::this_thread::sleep_for(std::chrono::duration<double>(sleep_s));
    }
}

bool NativeFaceController::latest_frame(cv::Mat& out) const {
    std::lock_guard<std::mutex> lk(frame_mtx_);
    if (!have_frame_) return false;
    latest_.copyTo(out);
    return true;
}

// ── Commands (mutate state under state_mtx_) ─────────────────────────────────

void NativeFaceController::set_color(uint8_t r, uint8_t g, uint8_t b, uint8_t) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (!pn.is_mirror)
            pn.material = std::make_shared<SolidMaterial>(r, g, b, pn.cfg.w, pn.cfg.h);
}

void NativeFaceController::set_effect(uint8_t effect_id, uint8_t, uint8_t) {
    nlohmann::json cfg = effect_cfg_for_id(effect_id);
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.particles) pn.particles->set_effect(cfg);
}

void NativeFaceController::set_face(uint8_t face_id) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.state) pn.state->set_expression_by_index(face_id);
}

void NativeFaceController::play_gif(uint8_t /*gif_id*/) {
    // TODO(Phase 3): GIF playback (decode + per-panel override).
}

void NativeFaceController::set_brightness(uint8_t value) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.state) pn.state->set_brightness(value);
}

void NativeFaceController::set_palette(uint8_t palette_id) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    apply_material_all(preset_material(palette_id));
}

void NativeFaceController::set_menu_item(uint8_t menu_index, uint8_t value) {
    if (menu_index != 8) return;   // 8 = material colour preset (matches Protoface)
    std::lock_guard<std::mutex> lk(state_mtx_);
    apply_material_all(preset_material(value));
}

void NativeFaceController::release_control() {
    // TODO(Phase 3): stop any active GIF and revert to the face.
}

void NativeFaceController::apply_material_all(const std::string& name) {
    for (auto& pn : panels_)
        if (!pn.is_mirror)
            pn.material = load_material(name, pn.cfg.w, pn.cfg.h,
                                        pn.cfg.material.scroll_x,
                                        pn.cfg.material.scroll_y,
                                        cfg_.materials_dir);
}

std::string NativeFaceController::preset_material(int idx) {
    // Mirrors Protoface ipc.py _MATERIAL_COLOR_MAP / ProtoHUD pf_palette order.
    switch (idx) {
        case 0:  return "teal";
        case 1:  return "solid:255,220,0";
        case 2:  return "solid:255,140,0";
        case 3:  return "solid:255,255,255";
        case 4:  return "solid:30,220,60";
        case 5:  return "solid:180,30,220";
        case 6:  return "solid:220,30,30";
        case 7:  return "solid:30,100,255";
        case 8:  return "rainbow";
        case 9:  return "cool";
        case 10: return "warm";
        case 11: return "solid:0,0,0";
        default: return "teal";
    }
}

} // namespace face
