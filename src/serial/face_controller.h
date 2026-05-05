#pragma once

#include <cstdint>

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
    virtual void play_gif(uint8_t gif_id) = 0;
    virtual void set_brightness(uint8_t value) = 0;
    virtual void set_palette(uint8_t palette_id) = 0;
    virtual void set_menu_item(uint8_t menu_index, uint8_t value) = 0;
    virtual void request_status() = 0;
    virtual void release_control() = 0;
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
    void play_gif(uint8_t gif_id)              override { (*active_)->play_gif(gif_id); }
    void set_brightness(uint8_t value)         override { (*active_)->set_brightness(value); }
    void set_palette(uint8_t palette_id)       override { (*active_)->set_palette(palette_id); }
    void set_menu_item(uint8_t idx, uint8_t v) override { (*active_)->set_menu_item(idx, v); }
    void request_status()                      override { (*active_)->request_status(); }
    void release_control()                     override { (*active_)->release_control(); }

    IFaceController* backend() const { return *active_; }

private:
    IFaceController** active_;
};
