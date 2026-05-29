#include "remote_control_server.h"

#include <cstdio>
#include <mutex>

// cpp-httplib is header-only; keep it confined to this translation unit.
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "../app_state.h"

using json = nlohmann::json;

namespace {
// Bumped when the wire contract in docs/COMPANION_API.md changes incompatibly.
constexpr const char* kApiVersion = "1";

// Clamp a JSON number to a byte, tolerating missing / out-of-range values.
uint8_t jbyte(const json& j, const char* key, uint8_t def = 0) {
    if (!j.contains(key) || !j[key].is_number()) return def;
    long v = j[key].get<long>();
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    return static_cast<uint8_t>(v);
}
}  // namespace

struct RemoteControlServer::Impl {
    httplib::Server svr;
};

RemoteControlServer::RemoteControlServer(AppState& state, FaceProxy* face, Config cfg)
    : state_(state), face_(face), cfg_(std::move(cfg)), impl_(std::make_unique<Impl>()) {}

RemoteControlServer::~RemoteControlServer() { stop(); }

bool RemoteControlServer::start() {
    if (!cfg_.enabled) return false;
    if (running_.load()) return true;
    running_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
}

void RemoteControlServer::stop() {
    if (!running_.exchange(false)) return;
    impl_->svr.stop();
    if (thread_.joinable()) thread_.join();
}

bool RemoteControlServer::dispatch_pad(const std::string& button) {
    // Mirrors WirelessController's button vocabulary. Returns false for unknown
    // buttons so the handler can reply 400.
    const Cb* cb = nullptr;
    if      (button == "select")   cb = &select_cb_;
    else if (button == "back")     cb = &back_cb_;
    else if (button == "menu")     cb = &menu_cb_;
    else if (button == "af")       cb = &af_cb_;
    else if (button == "capture")  cb = &capture_cb_;
    else if (button == "nav_up")   cb = &nav_up_cb_;
    else if (button == "nav_down") cb = &nav_down_cb_;
    else if (button == "nav_left") cb = &nav_left_cb_;
    else if (button == "nav_right")cb = &nav_right_cb_;
    else return false;

    if (*cb) (*cb)();
    return true;
}

void RemoteControlServer::run() {
    auto& svr = impl_->svr;

    // ── CORS — lets a browser PWA on the same AP/USB link talk to us too ──────
    svr.set_default_headers({
        {"Access-Control-Allow-Origin",  "*"},
        {"Access-Control-Allow-Headers", "Content-Type, X-ProtoHUD-Token"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
    });
    svr.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    // ── Optional shared-secret gate ───────────────────────────────────────────
    svr.set_pre_routing_handler(
        [this](const httplib::Request& req, httplib::Response& res) {
            if (cfg_.token.empty() || req.method == "OPTIONS")
                return httplib::Server::HandlerResponse::Unhandled;
            if (req.get_header_value("X-ProtoHUD-Token") == cfg_.token)
                return httplib::Server::HandlerResponse::Unhandled;
            res.status = 401;
            res.set_content(R"({"ok":false,"error":"unauthorized"})", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        });

    auto reply = [](httplib::Response& res, const json& j, int status = 200) {
        res.status = status;
        res.set_content(j.dump(), "application/json");
    };

    // Parse a JSON body, tolerating an empty body (returns {}).
    auto body = [](const httplib::Request& req) -> json {
        if (req.body.empty()) return json::object();
        return json::parse(req.body, nullptr, /*allow_exceptions=*/false);
    };

    // ── GET /api/v1/status ────────────────────────────────────────────────────
    svr.Get("/api/v1/status", [&](const httplib::Request&, httplib::Response& res) {
        json face_j;
        {
            std::lock_guard<std::mutex> lk(state_.mtx);
            const auto& f = state_.face;
            face_j = {
                {"connected",   f.connected},
                {"hud_control", f.hud_control},
                {"r", f.r}, {"g", f.g}, {"b", f.b},
                {"brightness",  f.brightness},
                {"effect_id",   f.effect_id},
                {"gif_id",      f.gif_id},
                {"palette_id",  f.palette_id},
                {"playing_gif", f.playing_gif},
                {"face_index",  f.face_index},
            };
        }
        reply(res, {
            {"ok", true},
            {"name", "ProtoHUD"},
            {"api", kApiVersion},
            {"face", face_j},
        });
    });

    // ── POST /api/v1/face/color  {r,g,b,layer?} ──────────────────────────────
    svr.Post("/api/v1/face/color", [&](const httplib::Request& req, httplib::Response& res) {
        json b = body(req);
        if (!face_) return reply(res, {{"ok", false}, {"error", "no_face"}}, 503);
        face_->set_color(jbyte(b, "r"), jbyte(b, "g"), jbyte(b, "b"), jbyte(b, "layer", 0));
        reply(res, {{"ok", true}});
    });

    // ── POST /api/v1/face/effect {effect_id,p1?,p2?} ─────────────────────────
    svr.Post("/api/v1/face/effect", [&](const httplib::Request& req, httplib::Response& res) {
        json b = body(req);
        if (!face_) return reply(res, {{"ok", false}, {"error", "no_face"}}, 503);
        face_->set_effect(jbyte(b, "effect_id"), jbyte(b, "p1", 0), jbyte(b, "p2", 0));
        reply(res, {{"ok", true}});
    });

    // ── POST /api/v1/face/gif {gif_id} ───────────────────────────────────────
    svr.Post("/api/v1/face/gif", [&](const httplib::Request& req, httplib::Response& res) {
        json b = body(req);
        if (!face_) return reply(res, {{"ok", false}, {"error", "no_face"}}, 503);
        face_->play_gif(jbyte(b, "gif_id"));
        reply(res, {{"ok", true}});
    });

    // ── POST /api/v1/face/brightness {value} ─────────────────────────────────
    svr.Post("/api/v1/face/brightness", [&](const httplib::Request& req, httplib::Response& res) {
        json b = body(req);
        if (!face_) return reply(res, {{"ok", false}, {"error", "no_face"}}, 503);
        face_->set_brightness(jbyte(b, "value", 200));
        reply(res, {{"ok", true}});
    });

    // ── POST /api/v1/face/palette {palette_id} ───────────────────────────────
    svr.Post("/api/v1/face/palette", [&](const httplib::Request& req, httplib::Response& res) {
        json b = body(req);
        if (!face_) return reply(res, {{"ok", false}, {"error", "no_face"}}, 503);
        face_->set_palette(jbyte(b, "palette_id"));
        reply(res, {{"ok", true}});
    });

    // ── POST /api/v1/face/expression {index} | {name} ────────────────────────
    svr.Post("/api/v1/face/expression", [&](const httplib::Request& req, httplib::Response& res) {
        json b = body(req);
        if (!face_) return reply(res, {{"ok", false}, {"error", "no_face"}}, 503);
        if (b.contains("name") && b["name"].is_string())
            face_->set_face_by_name(b["name"].get<std::string>());
        else
            face_->set_face(jbyte(b, "index"));
        reply(res, {{"ok", true}});
    });

    // ── POST /api/v1/face/release ────────────────────────────────────────────
    // Hand the face back to the backend's autonomous mode.
    svr.Post("/api/v1/face/release", [&](const httplib::Request&, httplib::Response& res) {
        if (!face_) return reply(res, {{"ok", false}, {"error", "no_face"}}, 503);
        face_->release_control();
        reply(res, {{"ok", true}});
    });

    // ── POST /api/v1/pad {button} ────────────────────────────────────────────
    // Remote control pad. button ∈ select|back|menu|af|capture|nav_{up,down,left,right}.
    svr.Post("/api/v1/pad", [&](const httplib::Request& req, httplib::Response& res) {
        json b = body(req);
        if (!b.contains("button") || !b["button"].is_string())
            return reply(res, {{"ok", false}, {"error", "missing_button"}}, 400);
        const std::string button = b["button"].get<std::string>();
        if (!dispatch_pad(button))
            return reply(res, {{"ok", false}, {"error", "unknown_button"}, {"button", button}}, 400);
        reply(res, {{"ok", true}, {"button", button}});
    });

    printf("[remote] companion control server listening on %s:%d%s\n",
           cfg_.host.c_str(), cfg_.port, cfg_.token.empty() ? "" : " (token required)");
    fflush(stdout);

    // Blocks until stop() calls svr.stop(). listen() returns false if the bind
    // fails (e.g. port already in use) — surface that rather than spinning.
    if (!svr.listen(cfg_.host.c_str(), cfg_.port)) {
        fprintf(stderr, "[remote] failed to bind %s:%d — companion server disabled\n",
                cfg_.host.c_str(), cfg_.port);
    }
    running_.store(false);
}
