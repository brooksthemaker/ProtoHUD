#pragma once

#include <cstdint>
#include <string>

/**
 * Abstract interface for face display backends.
 *
 * Both TeensyController (ProtoTracer over USB serial) and
 * ProtoFaceController (Protoface daemon over Unix socket) implement this
 * interface so ProtoHUD menus work unchanged regardless of backend.
 */
class IFaceController {
public:
    virtual ~IFaceController() = default;

    virtual bool start() = 0;
    virtual void stop()  = 0;

    virtual bool connected() const = 0;

    virtual void set_color(uint8_t r, uint8_t g, uint8_t b, uint8_t layer = 0) = 0;
    virtual void set_effect(uint8_t effect_id, uint8_t p1 = 0, uint8_t p2 = 0) = 0;
    // Select a face/expression by 0-based index in the backend's face set.
    virtual void set_face(uint8_t face_id) = 0;
    virtual void play_gif(uint8_t gif_id) = 0;
    virtual void set_brightness(uint8_t value) = 0;
    virtual void set_palette(uint8_t palette_id) = 0;
    virtual void set_menu_item(uint8_t menu_index, uint8_t value) = 0;
    virtual void request_status() = 0;
    virtual void release_control() = 0;

    // Persist the current look to the backend's own config (Protoface state.yaml).
    // Default no-op for backends without a save concept (e.g. Teensy/ProtoTracer).
    virtual void save_config() {}

    // Start the backend's program/daemon if it isn't already running.
    // Default no-op (e.g. the Teensy is always-on hardware).
    virtual void launch() {}

    // Restart the backend's program (stop the running one, then launch fresh).
    // Default no-op.
    virtual void restart() {}

    // Slot manifest — stable mapping from a menu slot index (0..N) to a media
    // file in the backend's media folder, persisted by the backend. Lets users
    // import new media into a specific slot without sorted-scan order shifting
    // every other binding. Default impls are no-ops / empty so backends without
    // host-side file storage (Teensy/ProtoTracer, daemon-Protoface) behave as
    // before — only the native in-process Protoface honours bindings.
    virtual std::string gif_slot(uint8_t /*slot*/) const { return {}; }
    virtual void        bind_gif_slot(uint8_t /*slot*/, const std::string& /*filename*/) {}
    virtual void        clear_gif_slot(uint8_t /*slot*/) {}

    // Face expression image management (native Protoface only). Each
    // "expression" (neutral/happy/angry/...) corresponds to a canonical PNG in
    // the active face folder. The Files > Faces menu uses these to import,
    // preview, switch and clear expression PNGs on disk; non-native backends
    // (Teensy/ProtoTracer, daemon-Protoface) return empty/false and ignore
    // mutating calls.
    virtual std::string face_image_path(const std::string& /*expression*/) const { return {}; }
    virtual bool        face_image_exists(const std::string& /*expression*/) const { return false; }
    virtual bool        import_face_image(const std::string& /*expression*/,
                                          const std::string& /*src_path*/) { return false; }
    virtual void        clear_face_image(const std::string& /*expression*/) {}
    // Set the active expression by name (mirror of set_face but by string).
    // Useful for the Files > Faces "Play" action where the index of a slot
    // isn't known statically.
    virtual void        set_face_by_name(const std::string& /*expression*/) {}

    // Trigger a temporary expression that auto-reverts after duration_s
    // seconds. Used by the boop sensor module — no-op on non-native backends
    // (the Teensy/daemon-Protoface paths run their own boop logic on-device).
    virtual void        trigger_boop(const std::string& /*expression*/, double /*duration_s*/) {}

    // Push mic-driven volume + mouth_open intensity (both in [0, 1]) from the
    // audio thread's voice analyzer. Native Protoface forwards to each panel's
    // FaceState::set_audio so the mouth_open.png overlay blends in real time.
    // No-op on non-native backends — those run their own audio reactivity
    // on-device, if any.
    virtual void        set_audio_drive(double /*volume*/, double /*mouth_open*/) {}

    // Pick which viseme overlay (mouth_open / mouth_small / mouth_smile /
    // mouth_round) the FaceLoader blends at the mouth region. Driven by the
    // voice analyzer's spectral-centroid-to-viseme classifier when visemes
    // are enabled.
    virtual void        set_mouth_shape(const std::string& /*shape*/) {}

    // True when the backend has addressable LED regions the face editor
    // can target (today: NativeFaceController with MAX7219 or RGB matrix
    // output). False for HUB75 + daemon + Teensy. Drives the visibility
    // of Files > Faces > <slot> > Edit… in the menu.
    virtual bool        has_led_face_editor() const { return false; }

    // HUB75 named-layout binding. When the user has saved multiple panel
    // layouts ("Default", "MyCM5Setup", …), each face PNG is stamped with
    // the layout that was active when it was created so the menu can flag
    // mismatches at slot-listing time. set_active_layout_name lets main
    // tell the backend which name to stamp on import; face_image_layout
    // reads back the tag for a face folder (empty = untagged / legacy).
    // No-op on non-native backends.
    virtual void        set_active_layout_name(const std::string& /*name*/) {}
    virtual std::string face_image_layout(const std::string& /*expression*/) const { return {}; }
};

/**
 * Proxy that dispatches all calls to whichever IFaceController *active points
 * to at the time of the call.  Lets menu lambdas capture a stable pointer
 * while the active backend is switched at runtime.
 */
class FaceProxy : public IFaceController {
public:
    explicit FaceProxy(IFaceController** active) : active_(active) {}

    bool start()            override { return (*active_)->start(); }
    void stop()             override { (*active_)->stop(); }
    bool connected() const  override { return (*active_)->connected(); }

    void set_color(uint8_t r, uint8_t g, uint8_t b, uint8_t layer = 0) override {
        (*active_)->set_color(r, g, b, layer);
    }
    void set_effect(uint8_t effect_id, uint8_t p1 = 0, uint8_t p2 = 0) override {
        (*active_)->set_effect(effect_id, p1, p2);
    }
    void set_face(uint8_t face_id)             override { (*active_)->set_face(face_id); }
    void play_gif(uint8_t gif_id)              override { (*active_)->play_gif(gif_id); }
    void set_brightness(uint8_t value)         override { (*active_)->set_brightness(value); }
    void set_palette(uint8_t palette_id)       override { (*active_)->set_palette(palette_id); }
    void set_menu_item(uint8_t idx, uint8_t v) override { (*active_)->set_menu_item(idx, v); }
    void request_status()                      override { (*active_)->request_status(); }
    void release_control()                     override { (*active_)->release_control(); }
    void save_config()                         override { (*active_)->save_config(); }
    void launch()                              override { (*active_)->launch(); }
    void restart()                             override { (*active_)->restart(); }
    std::string gif_slot(uint8_t s) const      override { return (*active_)->gif_slot(s); }
    void bind_gif_slot(uint8_t s, const std::string& f) override {
        (*active_)->bind_gif_slot(s, f);
    }
    void clear_gif_slot(uint8_t s)             override { (*active_)->clear_gif_slot(s); }

    std::string face_image_path(const std::string& e) const override {
        return (*active_)->face_image_path(e);
    }
    bool face_image_exists(const std::string& e) const override {
        return (*active_)->face_image_exists(e);
    }
    bool import_face_image(const std::string& e, const std::string& s) override {
        return (*active_)->import_face_image(e, s);
    }
    void clear_face_image(const std::string& e) override { (*active_)->clear_face_image(e); }
    void set_face_by_name(const std::string& e) override { (*active_)->set_face_by_name(e); }
    void trigger_boop(const std::string& e, double d) override { (*active_)->trigger_boop(e, d); }
    void set_audio_drive(double v, double m) override { (*active_)->set_audio_drive(v, m); }
    void set_mouth_shape(const std::string& s) override { (*active_)->set_mouth_shape(s); }
    bool has_led_face_editor() const override { return (*active_)->has_led_face_editor(); }
    void set_active_layout_name(const std::string& n) override {
        (*active_)->set_active_layout_name(n);
    }
    std::string face_image_layout(const std::string& e) const override {
        return (*active_)->face_image_layout(e);
    }

    IFaceController* backend() const { return *active_; }

private:
    IFaceController** active_;
};
