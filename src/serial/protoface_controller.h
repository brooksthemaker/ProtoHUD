#pragma once
#include "face_controller.h"
#include "shm_frame_reader.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <GLES2/gl2.h>

/**
 * Protoface backend: talks to the Protoface Python daemon via a Unix domain
 * socket at /run/protoface.sock.  Each command is a newline-terminated JSON
 * object: {"cmd":"set_effect","effect_id":3}
 *
 * Connection is kept alive; auto-reconnects on drop.
 */
class ProtoFaceController : public IFaceController {
public:
    explicit ProtoFaceController(const std::string& socket_path = "/run/protoface.sock");
    ~ProtoFaceController() override;

    bool start()           override;
    void stop()            override;
    bool connected() const override;

    void set_color(uint8_t r, uint8_t g, uint8_t b, uint8_t layer = 0) override;
    void set_effect(uint8_t effect_id, uint8_t p1 = 0, uint8_t p2 = 0) override;
    void play_gif(uint8_t gif_id)               override;
    void set_brightness(uint8_t value)          override;
    void set_palette(uint8_t palette_id)        override;
    void set_menu_item(uint8_t menu_index, uint8_t value) override;
    void request_status()                       override;
    void release_control()                      override;

    // Path used to detect whether Protoface is available.
    static bool socket_exists(const std::string& path = "/run/protoface.sock");

    // Panel preview — polls shared memory and uploads to GL texture if a new
    // frame is available.  Returns true when the texture was updated.
    // tex is created on the first call (initialise to 0).
    bool get_frame_texture(GLuint& tex);

    // Open the shared memory segment written by the Protoface Python process.
    // Called automatically from start(); can be retried if Protoface starts late.
    bool open_shm(const char* path = ShmFrameReader::DEFAULT_PATH);

private:
    void reconnect_loop();
    bool try_connect();
    bool send(const std::string& json);

    std::string          socket_path_;
    int                  fd_  { -1 };
    std::atomic<bool>    connected_ { false };
    std::atomic<bool>    running_   { false };
    std::thread          reconnect_thread_;
    mutable std::mutex   mtx_;

    ShmFrameReader       shm_;
    GLuint               panel_tex_ { 0 };
};
