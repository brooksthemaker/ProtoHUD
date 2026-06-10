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
#include "eye_animations.h"
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
    // Default expression → mood-preset coupling (used when set_expression_effects
    // is enabled). Keys match the face expression stems; values are presets in
    // particles.cpp. An empty value (or missing key) means "show the base effect".
    expr_effect_map_ = {
        {"angry",     "fire"},
        {"happy",     "celebration"},
        {"sad",       "rain"},
        {"shocked",   "galaxy"},
        {"surprised", "galaxy"},
    };
    build_panels();
}

NativeFaceController::~NativeFaceController() { stop(); }

void NativeFaceController::build_panels() {
    panels_.clear();
    auto find_cfg = [&](const std::string& name) -> const PanelCfg* {
        for (const auto& p : cfg_.panels) if (p.name == name) return &p;
        return nullptr;
    };
    for (const PanelCfg& pc : cfg_.panels) {
        Panel pn;
        pn.cfg = pc;

        // Decide the face/material/particles config source. Normally a panel is
        // its own source; a mirror_of panel copies a flipped region. With
        // continuous_effects on, un-mirror it: build a full self-rendering panel
        // from the SOURCE's face config and flip just the face layer at render
        // time, so canvas-space effects (water) stay continuous across eyes.
        const PanelCfg* fc = &pc;
        bool face_mirror = false;
        if (!pc.mirror_of.empty()) {
            const PanelCfg* src = cfg_.continuous_effects ? find_cfg(pc.mirror_of) : nullptr;
            if (src) { fc = src; face_mirror = true; }
            else     { pn.is_mirror = true; panels_.push_back(std::move(pn)); continue; }
        }

        pn.loader = std::make_unique<FaceLoader>(
            cfg_.faces_dir + "/" + fc->face.active, pc.w, pc.h,
            cfg_.canvas_w, cfg_.canvas_h, pc.x, pc.y);
        pn.loader->set_whole_face_blink(!cfg_.output_panels.empty());
        pn.state = std::make_unique<FaceState>(
            fc->face, pn.loader->expression_names());
        pn.material = load_material(fc->material.active, pc.w, pc.h,
                                    fc->material.scroll_x, fc->material.scroll_y,
                                    cfg_.materials_dir);
        if (cfg_.effects_enabled) {
            pn.particles = std::make_unique<ParticleSystem>(pc.w, pc.h, fc->particles);
            // Tell canvas-space effects (water) where this panel sits so a
            // multi-panel face renders one continuous field.
            pn.particles->set_canvas_geometry(cfg_.canvas_w, cfg_.canvas_h, pc.x, pc.y);
            pn.particles_spec = fc->particles;
        }
        // GIFs play per physical panel (duplicated on each side) rather than
        // stretched across the whole multi-panel canvas, so size the player
        // to one physical panel when this is a one-logical-panel face.
        int gw = pc.w, gh = pc.h;
        if (!cfg_.output_panels.empty()) {
            gw = cfg_.output_panels.front().w;
            gh = cfg_.output_panels.front().h;
        }
        pn.gif = std::make_unique<GifPlayer>(gw, gh);
        pn.material_spec = fc->material.active;
        pn.face_mirror   = face_mirror;
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
    // Restore any still-active transient face overlays so the saved
    // PNG image survives the next start() (loaders cache in-memory).
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        for (const auto& t : transient_faces_) {
            if (t.panel_idx < 0 || t.panel_idx >= static_cast<int>(panels_.size()))
                continue;
            auto& pn = panels_[t.panel_idx];
            if (pn.loader)
                pn.loader->set_expression_image(t.expression, t.original_image);
        }
        transient_faces_.clear();
    }
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

            // Advance the procedural eye animation (if one is playing). When the
            // timer runs out the panels fall back to the normal face render.
            const bool eye_active = eye_anim_timer_ > 0.0;
            if (eye_active) { eye_anim_timer_ -= dt; eye_anim_t_ += dt; }

            // Expire any transient face overlays whose deadline has passed —
            // restore the original expression image to the loader and drop
            // the record. Done before render so this tick uses the restored
            // image.
            if (!transient_faces_.empty()) {
                auto tnow = clock::now();
                for (auto it = transient_faces_.begin(); it != transient_faces_.end();) {
                    if (tnow >= it->deadline) {
                        if (it->panel_idx >= 0 &&
                            it->panel_idx < static_cast<int>(panels_.size())) {
                            auto& pn = panels_[it->panel_idx];
                            if (pn.loader)
                                pn.loader->set_expression_image(it->expression,
                                                                it->original_image);
                        }
                        it = transient_faces_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

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
                // otherwise composite the material-tinted face. With effects
                // disabled (MAX7219 / RGB matrix), we also skip the material
                // and use the face PNG verbatim — the material's luminance-
                // modulation washes RGB-matrix art to grey/teal otherwise.
                cv::Mat face_layer;
                cv::Mat gframe = (!eye_active && pn.gif) ? pn.gif->get_frame() : cv::Mat();
                if (eye_active) {
                    // Procedural eye animation owns the whole panel.
                    face_layer = render_eye_animation(eye_anim_, eye_anim_t_, pc.w, pc.h);
                } else if (!gframe.empty()) {
                    if (!cfg_.output_panels.empty() &&
                        (gframe.cols != pc.w || gframe.rows != pc.h)) {
                        // Duplicate the panel-sized GIF onto each physical panel
                        // instead of stretching one copy across the whole canvas.
                        face_layer = cv::Mat::zeros(pc.h, pc.w, CV_8UC4);
                        for (const auto& op : cfg_.output_panels) {
                            cv::Mat g = gframe;
                            if (gframe.cols != op.w || gframe.rows != op.h)
                                cv::resize(gframe, g, cv::Size(op.w, op.h), 0, 0, cv::INTER_NEAREST);
                            // Pre-flip so the per-panel output flip cancels out:
                            // GIFs (which may contain text) read forwards on every
                            // panel regardless of its mounting flip.
                            if (op.flip_x || op.flip_y) {
                                const int code = (op.flip_x && op.flip_y) ? -1 : (op.flip_x ? 1 : 0);
                                cv::Mat tmp; cv::flip(g, tmp, code); g = tmp;
                            }
                            const cv::Rect dst(op.x - pc.x, op.y - pc.y, op.w, op.h);
                            const cv::Rect inter = dst & cv::Rect(0, 0, pc.w, pc.h);
                            if (inter.width > 0 && inter.height > 0)
                                g(cv::Rect(inter.x - dst.x, inter.y - dst.y, inter.width, inter.height))
                                    .copyTo(face_layer(inter));
                        }
                    } else {
                        face_layer = gframe;
                    }
                } else if (!cfg_.effects_enabled || !pn.material) {
                    face_layer = pn.loader->get_frame(*pn.state);
                } else {
                    cv::Mat mat = pn.material->get_frame();
                    cv::Mat face_rgba = pn.loader->get_frame(*pn.state);
                    face_layer = apply_material(face_rgba, mat);
                }

                // continuous_effects un-mirror: flip just the face/GIF layer so
                // the eye still reads mirrored, while the canvas-space particle
                // layer (added below) renders un-flipped and stays continuous.
                if (pn.face_mirror && !face_layer.empty())
                    cv::flip(face_layer, face_layer, 1);

                std::vector<Layer> layers{ Layer{face_layer, Blend::Normal} };
                // Effects are suppressed while a GIF or eye animation plays —
                // the clip owns the whole panel — and resume automatically when
                // it ends and the face animation returns. (The sim keeps running
                // so the field is already settled when it reappears.)
                if (pn.particles && gframe.empty() && !eye_active) {
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

            // Per-panel orientation flips (HUB75 layout's Flip X / Flip Y).
            // Applied last so it covers both self-rendered and mirror panels.
            // flip code: 1 = horizontal (left-right), 0 = vertical (top-bottom),
            // -1 = both (180°). Done in place on the panel's canvas region.
            for (auto& pn : panels_) {
                const PanelCfg& pc = pn.cfg;
                if (!pc.flip_x && !pc.flip_y) continue;
                cv::Rect roi(pc.x, pc.y, pc.w, pc.h);
                if ((roi & cv::Rect(0, 0, canvas.cols, canvas.rows)) != roi) continue;
                const int code = (pc.flip_x && pc.flip_y) ? -1 : (pc.flip_x ? 1 : 0);
                cv::Mat region = canvas(roi), flipped;
                cv::flip(region, flipped, code);
                flipped.copyTo(region);
            }

            // Multi-panel face rendered as one logical canvas: flip each physical
            // panel's slice in place so per-panel mounting flips apply to the
            // whole composited image (face + material + effects + blink alike).
            for (const auto& op : cfg_.output_panels) {
                if (!op.flip_x && !op.flip_y) continue;
                cv::Rect roi(op.x, op.y, op.w, op.h);
                if ((roi & cv::Rect(0, 0, canvas.cols, canvas.rows)) != roi) continue;
                const int code = (op.flip_x && op.flip_y) ? -1 : (op.flip_x ? 1 : 0);
                cv::Mat region = canvas(roi), flipped;
                cv::flip(region, flipped, code);
                flipped.copyTo(region);
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
    if (!cfg_.effects_enabled) return;       // gated by RenderConfig (see face_config.h)
    nlohmann::json cfg = effect_cfg_for_id(effect_id);
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.particles) { pn.particles->set_effect(cfg); pn.particles_spec = cfg; }
    save_state_locked();
}

void NativeFaceController::set_effect_json(const nlohmann::json& spec) {
    if (!cfg_.effects_enabled) return;
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.particles) { pn.particles->set_effect(spec); pn.particles_spec = spec; }
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

void NativeFaceController::set_material_spec(const std::string& spec) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    apply_material_all(spec);
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
        // Multi-colour gradient presets (rendered via GradientMaterial — no PNG
        // asset needed). Smooth horizontal blends, static. Kept in sync with the
        // pf_mats table in main.cpp's Material Color menu.
        case 12: return "gradient:h:s:0:FF8C00-FF3D7F-8A2BE2";  // Sunset
        case 13: return "gradient:h:s:0:00E5FF-0077FF-001F7F";  // Ocean
        case 14: return "gradient:h:s:0:7CFF6B-1E9E3C-0B3D1A";  // Forest
        case 15: return "gradient:h:s:0:FFE000-FF7A00-E01E1E";  // Fire
        case 16: return "gradient:h:s:0:00FFA3-00D0FF-B14BFF";  // Aurora
        case 17: return "gradient:h:s:0:2A0A0A-C81E00-FF8C00";  // Lava
        case 18: return "gradient:h:s:0:2B0B5E-7A1EB4-FF4FD8";  // Galaxy
        case 19: return "gradient:h:s:0:FFB3BA-BAE1FF-BAFFC9";  // Pastel
        case 20: return "gradient:h:s:0:FF4FA3-FFD24F-4FC3FF";  // Candy
        case 21: return "gradient:h:s:0:AEFF00-00FFB3-00A3FF";  // Toxic
        // ── Pride flags ───────────────────────────────────────────────────────
        // Vertical smooth gradients so the colours stack top→bottom like flag
        // stripes. Kept in sync with the pf_pride table in main.cpp.
        case 22: return "gradient:v:s:0:E40303-FF8C00-FFED00-008026-004DFF-750787";          // Rainbow
        case 23: return "gradient:v:s:0:000000-613915-5BCEFA-F5A9B8-FFFFFF-E40303-FF8C00-FFED00-008026-004DFF-750787"; // Progress
        case 24: return "gradient:v:s:0:5BCEFA-F5A9B8-FFFFFF-F5A9B8-5BCEFA";                  // Trans
        case 25: return "gradient:v:s:0:D60270-D60270-9B4F96-0038A8-0038A8";                 // Bisexual
        case 26: return "gradient:v:s:0:FF218C-FFD800-21B1FF";                               // Pansexual
        case 27: return "gradient:v:s:0:D52D00-FF9A56-FFFFFF-D362A4-A30262";                 // Lesbian
        case 28: return "gradient:v:s:0:FCF434-FFFFFF-9C59D1-2C2C2C";                        // Nonbinary
        case 29: return "gradient:v:s:0:000000-A3A3A3-FFFFFF-800080";                        // Asexual
        case 30: return "gradient:v:s:0:FF75A2-FFFFFF-BE18D6-000000-333EBD";                 // Genderfluid
        case 31: return "gradient:v:s:0:B57EDC-FFFFFF-4A8123";                               // Genderqueer
        case 32: return "gradient:v:s:0:3DA542-A7D379-FFFFFF-A9A9A9-000000";                 // Aromantic
        case 33: return "gradient:v:s:0:FFD800-7902AA-FFD800";                               // Intersex
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
    return probe_face_image(expression).exists;
}

// Shared probe behind face_image_exists / face_image_layout. Paths come from
// a brief state_mtx_ hold; the stat + config.json parse run unlocked so the
// face render thread isn't stalled by menu redraws.
NativeFaceController::FaceProbe
NativeFaceController::probe_face_image(const std::string& expression) const {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(probe_mtx_);
        auto it = probe_cache_.find(expression);
        if (it != probe_cache_.end() &&
            now - it->second.t < std::chrono::seconds(1))
            return it->second;
    }

    std::string png, cfg_path;
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        for (const auto& pn : panels_) {
            if (pn.is_mirror || !pn.loader) continue;
            const std::string folder = cfg_.faces_dir + "/" + pn.cfg.face.active;
            png      = folder + "/" + canonical_face_filename(expression);
            cfg_path = folder + "/config.json";
            break;
        }
    }

    FaceProbe p;
    p.t = now;
    if (!png.empty()) {
        std::error_code ec;
        p.exists = std::filesystem::exists(png, ec);
        // Only carry a layout tag when the actual PNG exists — keeps
        // "(empty)" slots from picking up a folder-wide tag they don't own.
        if (p.exists) {
            std::ifstream fr(cfg_path);
            if (fr) {
                try {
                    nlohmann::json j; fr >> j;
                    if (j.is_object() && j.contains("layout") && j["layout"].is_string())
                        p.layout = j["layout"].get<std::string>();
                } catch (...) {}
            }
        }
    }

    std::lock_guard<std::mutex> lk(probe_mtx_);
    probe_cache_[expression] = p;
    return p;
}

void NativeFaceController::invalidate_face_probes() {
    std::lock_guard<std::mutex> lk(probe_mtx_);
    probe_cache_.clear();
}

bool NativeFaceController::import_face_image(const std::string& expression,
                                             const std::string& src_path) {
    invalidate_face_probes();
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

    // Stamp the face folder with the active layout name on first import so
    // the menu can flag mismatches when the user switches layouts. Already-
    // tagged folders are left alone — re-importing into an existing face
    // shouldn't silently re-bind it to whatever the user picked since.
    if (!active_layout_name_.empty()) {
        const std::string cfg_path = folder + "/config.json";
        nlohmann::json jcfg = nlohmann::json::object();
        std::ifstream fr(cfg_path);
        if (fr) { try { fr >> jcfg; } catch (...) { jcfg = nlohmann::json::object(); } }
        if (!jcfg.is_object()) jcfg = nlohmann::json::object();
        bool tagged = jcfg.contains("layout") && jcfg["layout"].is_string() &&
                      !jcfg["layout"].get<std::string>().empty();
        if (!tagged) {
            jcfg["layout"] = active_layout_name_;
            const std::string tmp = cfg_path + ".tmp";
            { std::ofstream fw(tmp); if (fw) fw << jcfg.dump(2); }
            std::error_code rec;
            std::filesystem::rename(tmp, cfg_path, rec);
        }
    }

    // Rebuild every non-mirror panel's loader so the new PNG takes effect on
    // the next render tick. (Cheap — just re-decodes the folder's images.)
    for (auto& pn : panels_) {
        if (pn.is_mirror || !pn.state) continue;
        pn.loader = std::make_unique<FaceLoader>(
            cfg_.faces_dir + "/" + pn.cfg.face.active, pn.cfg.w, pn.cfg.h,
            cfg_.canvas_w, cfg_.canvas_h, pn.cfg.x, pn.cfg.y);
        pn.loader->set_whole_face_blink(!cfg_.output_panels.empty());
    }
    return true;
}

void NativeFaceController::set_active_layout_name(const std::string& name) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    active_layout_name_ = name;
}

std::string NativeFaceController::face_image_layout(const std::string& expression) const {
    // Layout tags live at face-folder scope (faces/<folder>/config.json),
    // so all expressions in the same folder share a tag. We look at the
    // first non-mirror panel's active folder, same as face_image_path.
    return probe_face_image(expression).layout;
}

void NativeFaceController::clear_face_image(const std::string& expression) {
    invalidate_face_probes();
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
            cfg_.faces_dir + "/" + pn.cfg.face.active, pn.cfg.w, pn.cfg.h,
            cfg_.canvas_w, cfg_.canvas_h, pn.cfg.x, pn.cfg.y);
        pn.loader->set_whole_face_blink(!cfg_.output_panels.empty());
    }
}

void NativeFaceController::set_face_by_name(const std::string& expression) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.state) pn.state->set_expression(expression);
    current_expression_ = expression;
    apply_expression_effect_locked(expression);
    save_state_locked();
}

std::string NativeFaceController::current_expression() const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    return current_expression_;
}

void NativeFaceController::next_expression() { cycle_expression(+1); }
void NativeFaceController::prev_expression() { cycle_expression(-1); }

void NativeFaceController::cycle_expression(int dir) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    // Advance the first real panel as the "master" index, read back its new
    // expression name, then mirror it to every panel so multi-panel faces stay
    // in sync (each FaceState tracks its own index; always stepping the same
    // master keeps the set coherent across repeated calls).
    std::string name;
    for (auto& pn : panels_) {
        if (!pn.state) continue;
        if (dir >= 0) pn.state->next_expression();
        else          pn.state->prev_expression();
        name = pn.state->expression();
        break;
    }
    if (name.empty()) return;
    for (auto& pn : panels_)
        if (pn.state) pn.state->set_expression(name);
    current_expression_ = name;
    apply_expression_effect_locked(name);
    save_state_locked();
}

void NativeFaceController::set_expression_effects(bool enabled) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    expr_effects_ = enabled;
    if (enabled) {
        apply_expression_effect_locked(current_expression_);
    } else {
        // Restore each panel's user-chosen base effect.
        for (auto& pn : panels_)
            if (pn.particles) pn.particles->set_effect(pn.particles_spec);
    }
}

void NativeFaceController::apply_expression_effect_locked(const std::string& expr) {
    if (!expr_effects_) return;
    auto it = expr_effect_map_.find(expr);
    const bool mapped = (it != expr_effect_map_.end() && !it->second.empty());
    for (auto& pn : panels_) {
        if (!pn.particles) continue;
        if (mapped) {
            nlohmann::json spec; spec["preset"] = it->second;
            pn.particles->set_effect(spec);     // transient — leaves particles_spec (base) intact
        } else {
            pn.particles->set_effect(pn.particles_spec);   // neutral/unmapped → base
        }
    }
}

void NativeFaceController::trigger_boop(const std::string& expression, double duration_s) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.state) pn.state->trigger_boop(expression, duration_s);
    // Don't save_state_locked() — boop is transient by design; the auto-revert
    // in FaceState::update will bring expression back without our help.
}

void NativeFaceController::play_eye_animation(int type, double speed, double size,
                                              uint8_t r, uint8_t g, uint8_t b,
                                              double duration_s) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    const int n = std::clamp(type, 0, static_cast<int>(EyeAnim::Count) - 1);
    eye_anim_.type       = static_cast<EyeAnim>(n);
    eye_anim_.speed      = (speed > 0.0) ? speed : 1.0;
    eye_anim_.size       = (size  > 0.0) ? size  : 1.0;
    eye_anim_.r = r; eye_anim_.g = g; eye_anim_.b = b;
    eye_anim_.duration_s = duration_s;
    eye_anim_timer_ = (duration_s > 0.0) ? duration_s : 0.0;
    eye_anim_t_     = 0.0;
    // Transient by design — no save_state_locked().
}

// ── Animation tuning ─────────────────────────────────────────────────────────
// These mutate the per-panel FaceState directly; the renderer thread reads them
// in the next update tick. No state-file save here — these are session-scoped
// live tweaks; main.cpp persists the values to config.json on shutdown / save.

void NativeFaceController::set_blink_enabled(bool enabled) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.state) pn.state->set_blink_enabled(enabled);
}

void NativeFaceController::set_blink_timing(double min_s, double max_s, double duration_s) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.state) pn.state->set_blink_timing(min_s, max_s, duration_s);
}

void NativeFaceController::set_expression_fade(double seconds) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.state) pn.state->set_expression_fade(seconds);
}

void NativeFaceController::set_panel_flips(const std::vector<std::array<bool, 2>>& flips) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    if (!cfg_.output_panels.empty()) {
        // Multi-panel face rendered as one canvas: flips live on the physical
        // output slices, applied at the end of the render loop.
        for (size_t i = 0; i < cfg_.output_panels.size() && i < flips.size(); ++i) {
            cfg_.output_panels[i].flip_x = flips[i][0];
            cfg_.output_panels[i].flip_y = flips[i][1];
        }
        return;
    }
    for (size_t i = 0; i < panels_.size() && i < flips.size(); ++i) {
        panels_[i].cfg.flip_x = flips[i][0];
        panels_[i].cfg.flip_y = flips[i][1];
    }
}

void NativeFaceController::set_wiggle(const WiggleCfg& w) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.state) pn.state->set_wiggle(w);
}

void NativeFaceController::set_audio_drive(double volume, double mouth_open) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_) {
        if (pn.state)     pn.state->set_audio(volume, mouth_open);
        if (pn.particles) pn.particles->set_audio(volume);   // audio-reactive layers
    }
    // Transient — no persistence on every audio frame.
}

void NativeFaceController::set_motion(double heading_deg, double yaw_rate,
                                      double pitch_deg, double roll_deg,
                                      double accel_g) {
    MotionInput m;
    m.heading_deg = heading_deg; m.yaw_rate = yaw_rate;
    m.pitch_deg = pitch_deg; m.roll_deg = roll_deg; m.accel_g = accel_g;
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_)
        if (pn.particles) pn.particles->set_motion(m);
    // Transient — no persistence on every motion frame.
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
    invalidate_face_probes();
    std::lock_guard<std::mutex> lk(state_mtx_);
    for (auto& pn : panels_) {
        if (pn.is_mirror || !pn.state) continue;
        pn.loader = std::make_unique<FaceLoader>(
            cfg_.faces_dir + "/" + pn.cfg.face.active, pn.cfg.w, pn.cfg.h,
            cfg_.canvas_w, cfg_.canvas_h, pn.cfg.x, pn.cfg.y);
        pn.loader->set_whole_face_blink(!cfg_.output_panels.empty());
    }
}

void NativeFaceController::push_transient_face(const std::string& expression,
                                                const cv::Mat& rgba_canvas,
                                                double duration_s) {
    if (rgba_canvas.empty() || duration_s <= 0.0) return;
    std::lock_guard<std::mutex> lk(state_mtx_);

    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(duration_s));
    const cv::Rect canvas_bounds(0, 0, rgba_canvas.cols, rgba_canvas.rows);

    for (size_t i = 0; i < panels_.size(); ++i) {
        auto& pn = panels_[i];
        if (pn.is_mirror || !pn.loader) continue;

        const PanelCfg& pc = pn.cfg;
        cv::Rect roi = cv::Rect(pc.x, pc.y, pc.w, pc.h) & canvas_bounds;
        if (roi.area() <= 0) continue;

        cv::Mat crop = rgba_canvas(roi).clone();
        if (crop.cols != pn.loader->panel_width() ||
            crop.rows != pn.loader->panel_height()) {
            cv::Mat resized;
            cv::resize(crop, resized,
                       cv::Size(pn.loader->panel_width(), pn.loader->panel_height()),
                       0, 0, cv::INTER_NEAREST);
            crop = resized;
        }

        // Stash the current image so the deadline pass can restore it.
        // If we already pushed a transient for this panel+expression, keep
        // the *earlier* original (avoids stacking previews from overwriting
        // the user's saved image with another preview).
        bool already = false;
        for (auto& t : transient_faces_) {
            if (t.panel_idx == static_cast<int>(i) && t.expression == expression) {
                t.deadline = deadline;
                already = true;
                break;
            }
        }
        if (!already) {
            TransientFace t;
            t.panel_idx      = static_cast<int>(i);
            t.expression     = expression;
            t.original_image = pn.loader->get_expression_image(expression);
            t.deadline       = deadline;
            transient_faces_.push_back(std::move(t));
        }

        pn.loader->set_expression_image(expression, crop);
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
