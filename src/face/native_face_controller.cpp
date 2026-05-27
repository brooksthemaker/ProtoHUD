#include "native_face_controller.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include <opencv2/imgproc.hpp>

#include "face_state.h"
#include "face_loader.h"
#include "materials.h"
#include "renderer.h"
#include "particles.h"
#include "gif_player.h"
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
            pn.gif = std::make_unique<GifPlayer>(pc.w, pc.h);
            pn.material_spec  = pc.material.active;
            pn.particles_spec = pc.particles;
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

    gif_files_   = GifPlayer::scan_folder(cfg_.gifs_dir);
    gif_slots_.assign(8, "");   // unbound by default; load_state() may fill these
    gif_release_ = cfg_.gif_auto_release;

    load_state();   // overlay any auto-saved look on the config defaults

    // Back-compat: if the user already had GIFs in gifs_dir before the manifest
    // existed, auto-bind the sorted scan so the menu shows real entries on
    // first launch instead of an all-"(empty)" list. Skip when any slot is
    // already bound (state.json was authoritative).
    const bool any_bound =
        std::any_of(gif_slots_.begin(), gif_slots_.end(),
                    [](const std::string& s){ return !s.empty(); });
    if (!any_bound && !gif_files_.empty()) {
        for (size_t i = 0; i < gif_slots_.size() && i < gif_files_.size(); ++i)
            gif_slots_[i] = std::filesystem::path(gif_files_[i]).filename().string();
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
                if (pn.gif) pn.gif->update(dt);

                // GIF auto-revert: stop the clip N seconds after play_gif.
                if (pn.gif && pn.gif_release_timer > 0.0) {
                    pn.gif_release_timer -= dt;
                    if (pn.gif_release_timer <= 0.0) pn.gif->stop();
                }

                cv::Mat bg  = solid_layer(cfg_.background[0], cfg_.background[1],
                                          cfg_.background[2], pc.w, pc.h);

                // A playing GIF replaces the face (full colour, no material tint);
                // otherwise composite the material-tinted face.
                cv::Mat face_layer;
                cv::Mat gframe = pn.gif ? pn.gif->get_frame() : cv::Mat();
                if (!gframe.empty()) {
                    face_layer = gframe;
                } else {
                    cv::Mat mat = pn.material->get_frame();
                    cv::Mat face_rgba = pn.loader->get_frame(*pn.state);
                    face_layer = apply_material(face_rgba, mat);
                }

                std::vector<Layer> layers{ Layer{face_layer, Blend::Normal} };
                if (pn.particles) {
                    ParticleFrame pf = pn.particles->render();
                    if (pf.has) layers.push_back(Layer{pf.rgba, pf.blend});
                }

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
    char spec[32];
    std::snprintf(spec, sizeof(spec), "solid:%u,%u,%u", r, g, b);
    for (auto& pn : panels_)
        if (!pn.is_mirror) {
            pn.material = std::make_shared<SolidMaterial>(r, g, b, pn.cfg.w, pn.cfg.h);
            pn.material_spec = spec;
        }
    save_state_locked();
}

void NativeFaceController::set_effect(uint8_t effect_id, uint8_t, uint8_t) {
    nlohmann::json cfg = effect_cfg_for_id(effect_id);
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.particles) { pn.particles->set_effect(cfg); pn.particles_spec = cfg; }
    save_state_locked();
}

void NativeFaceController::set_face(uint8_t face_id) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.state) pn.state->set_expression_by_index(face_id);
    save_state_locked();
}

void NativeFaceController::play_gif(uint8_t gif_id) {
    std::lock_guard<std::mutex> lk(state_mtx_);

    // Prefer the slot manifest binding; fall back to the sorted scan when the
    // slot is unbound or its bound file has been removed from gifs_dir.
    std::string path;
    if (gif_id < gif_slots_.size() && !gif_slots_[gif_id].empty()) {
        const std::string p = cfg_.gifs_dir + "/" + gif_slots_[gif_id];
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) path = p;
    }
    if (path.empty()) {
        if (gif_id >= gif_files_.size()) return;
        path = gif_files_[gif_id];
    }

    for (auto& pn : panels_)
        if (pn.gif) {
            pn.gif->load(path);
            pn.gif_release_timer = gif_release_;   // 0 = loop forever
        }
}

void NativeFaceController::set_brightness(uint8_t value) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.state) pn.state->set_brightness(value);
    save_state_locked();
}

void NativeFaceController::set_palette(uint8_t palette_id) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    apply_material_all(preset_material(palette_id));
    save_state_locked();
}

void NativeFaceController::set_menu_item(uint8_t menu_index, uint8_t value) {
    if (menu_index != 8) return;   // 8 = material colour preset (matches Protoface)
    std::lock_guard<std::mutex> lk(state_mtx_);
    apply_material_all(preset_material(value));
    save_state_locked();
}

void NativeFaceController::release_control() {
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.gif) { pn.gif->stop(); pn.gif_release_timer = 0.0; }
}

void NativeFaceController::save_config() {
    std::lock_guard<std::mutex> lk(state_mtx_);
    save_state_locked();
}

void NativeFaceController::apply_material_all(const std::string& name) {
    for (auto& pn : panels_)
        if (!pn.is_mirror) {
            pn.material = load_material(name, pn.cfg.w, pn.cfg.h,
                                        pn.cfg.material.scroll_x,
                                        pn.cfg.material.scroll_y,
                                        cfg_.materials_dir);
            pn.material_spec = name;
        }
}

// ── Persistence (auto-saved live look) ───────────────────────────────────────

void NativeFaceController::save_state_locked() const {
    if (cfg_.state_path.empty()) return;
    nlohmann::json j;
    for (const auto& pn : panels_)
        if (pn.state) { j["brightness"] = pn.state->brightness(); break; }
    nlohmann::json jp = nlohmann::json::object();
    for (const auto& pn : panels_) {
        if (pn.is_mirror || !pn.state) continue;
        nlohmann::json e;
        e["material"]   = pn.material_spec;
        e["particles"]  = pn.particles_spec;
        e["expression"] = pn.state->expression();
        jp[pn.cfg.name] = e;
    }
    j["panels"] = jp;
    j["gif_slots"] = gif_slots_;

    const std::string tmp = cfg_.state_path + ".tmp";
    { std::ofstream f(tmp); if (!f) return; f << j.dump(2); }
    std::error_code ec;
    std::filesystem::rename(tmp, cfg_.state_path, ec);   // atomic replace
}

void NativeFaceController::load_state() {
    if (cfg_.state_path.empty()) return;
    std::ifstream f(cfg_.state_path);
    if (!f) return;
    nlohmann::json j;
    try { f >> j; } catch (...) { return; }

    if (j.contains("brightness") && j["brightness"].is_number()) {
        int b = j["brightness"].get<int>();
        for (auto& pn : panels_) if (pn.state) pn.state->set_brightness(b);
    }
    if (j.contains("gif_slots") && j["gif_slots"].is_array()) {
        for (size_t i = 0; i < gif_slots_.size() && i < j["gif_slots"].size(); ++i) {
            const auto& v = j["gif_slots"][i];
            if (v.is_string()) gif_slots_[i] = v.get<std::string>();
        }
    }
    if (!j.contains("panels") || !j["panels"].is_object()) return;
    const auto& jp = j["panels"];
    for (auto& pn : panels_) {
        if (pn.is_mirror || !pn.state) continue;
        auto it = jp.find(pn.cfg.name);
        if (it == jp.end()) continue;
        const auto& e = *it;
        if (e.contains("material") && e["material"].is_string()) {
            pn.material_spec = e["material"].get<std::string>();
            pn.material = load_material(pn.material_spec, pn.cfg.w, pn.cfg.h,
                                        pn.cfg.material.scroll_x,
                                        pn.cfg.material.scroll_y,
                                        cfg_.materials_dir);
        }
        if (e.contains("particles")) {
            pn.particles_spec = e["particles"];
            if (pn.particles) pn.particles->set_effect(pn.particles_spec);
        }
        if (e.contains("expression") && e["expression"].is_string())
            pn.state->set_expression(e["expression"].get<std::string>());
    }
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

// ── Slot manifest accessors ───────────────────────────────────────────────────

std::string NativeFaceController::gif_slot(uint8_t slot) const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    if (slot >= gif_slots_.size()) return {};
    return gif_slots_[slot];
}

void NativeFaceController::bind_gif_slot(uint8_t slot, const std::string& filename) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    if (slot >= gif_slots_.size()) return;
    // Always store a basename so the manifest survives folder relocation.
    gif_slots_[slot] = std::filesystem::path(filename).filename().string();
    save_state_locked();
}

void NativeFaceController::clear_gif_slot(uint8_t slot) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    if (slot >= gif_slots_.size()) return;
    gif_slots_[slot].clear();
    save_state_locked();
}

// ── Face image management ─────────────────────────────────────────────────────
// All paths are derived from the first non-mirror panel's active face folder
// (typically faces/main on the standard 2-panel mirrored setup). Imports
// rewrite the canonical filename and reload affected loaders.

namespace {
std::string canonical_face_filename(const std::string& expression) {
    // Lower-case + ".png". The face loader keys expressions by lowercase name
    // (see face_loader.cpp PNG-scan branch), so this lines up regardless of how
    // the expression appears in config.json.
    std::string s = expression;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s + ".png";
}
} // namespace

std::string NativeFaceController::face_image_path(const std::string& expression) const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (const auto& pn : panels_) {
        if (pn.is_mirror || !pn.loader) continue;
        return cfg_.faces_dir + "/" + pn.cfg.face.active + "/" +
               canonical_face_filename(expression);
    }
    return {};
}

bool NativeFaceController::face_image_exists(const std::string& expression) const {
    const std::string p = face_image_path(expression);
    if (p.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

bool NativeFaceController::import_face_image(const std::string& expression,
                                             const std::string& src_path) {
    std::lock_guard<std::mutex> lk(state_mtx_);

    // Find the first non-mirror panel's active face folder.
    const Panel* anchor = nullptr;
    for (const auto& pn : panels_) {
        if (!pn.is_mirror && pn.loader) { anchor = &pn; break; }
    }
    if (!anchor) return false;

    const std::string folder = cfg_.faces_dir + "/" + anchor->cfg.face.active;
    const std::string dst    = folder + "/" + canonical_face_filename(expression);

    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    std::filesystem::copy_file(
        src_path, dst,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        std::fprintf(stderr, "[face] import copy failed %s -> %s: %s\n",
                     src_path.c_str(), dst.c_str(), ec.message().c_str());
        return false;
    }

    // Rebuild every non-mirror panel's loader so the new PNG takes effect on
    // the next render tick. (Cheap — just re-decodes the folder's images.)
    for (auto& pn : panels_) {
        if (pn.is_mirror || !pn.state) continue;
        pn.loader = std::make_unique<FaceLoader>(
            cfg_.faces_dir + "/" + pn.cfg.face.active, pn.cfg.w, pn.cfg.h);
    }
    return true;
}

void NativeFaceController::clear_face_image(const std::string& expression) {
    std::lock_guard<std::mutex> lk(state_mtx_);

    const Panel* anchor = nullptr;
    for (const auto& pn : panels_) {
        if (!pn.is_mirror && pn.loader) { anchor = &pn; break; }
    }
    if (!anchor) return;

    const std::string p = cfg_.faces_dir + "/" + anchor->cfg.face.active + "/" +
                          canonical_face_filename(expression);
    std::error_code ec;
    std::filesystem::remove(p, ec);
    for (auto& pn : panels_) {
        if (pn.is_mirror || !pn.state) continue;
        pn.loader = std::make_unique<FaceLoader>(
            cfg_.faces_dir + "/" + pn.cfg.face.active, pn.cfg.w, pn.cfg.h);
    }
}

void NativeFaceController::set_face_by_name(const std::string& expression) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.state) pn.state->set_expression(expression);
    save_state_locked();
}

void NativeFaceController::trigger_boop(const std::string& expression, double duration_s) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.state) pn.state->trigger_boop(expression, duration_s);
    // Don't save_state_locked() — boop is transient by design; the auto-revert
    // in FaceState::update will bring expression back without our help.
}

void NativeFaceController::set_audio_drive(double volume, double mouth_open) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.state) pn.state->set_audio(volume, mouth_open);
    // Transient — no persistence on every audio frame.
}

void NativeFaceController::set_mouth_shape(const std::string& shape) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.state) pn.state->set_mouth_shape(shape);
}

std::vector<cv::Rect> NativeFaceController::led_covered_regions() const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    if (!output_) return {};
    return output_->covered_regions();
}

std::vector<NamedRegion> NativeFaceController::led_named_regions() const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    if (!output_) return {};
    return output_->covered_named_regions();
}

void NativeFaceController::reload_active_face() {
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_) {
        if (pn.is_mirror || !pn.state) continue;
        pn.loader = std::make_unique<FaceLoader>(
            cfg_.faces_dir + "/" + pn.cfg.face.active, pn.cfg.w, pn.cfg.h);
    }
}

bool NativeFaceController::has_led_face_editor() const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    // Capability flag on the active output, not chain count — so the
    // editor stays available even when MAX7219 / RGB-matrix is selected
    // but no chains are configured yet (the user can author the PNG and
    // the renderer will pick it up once chains land in config).
    return output_ && output_->supports_face_editor();
}

} // namespace face
