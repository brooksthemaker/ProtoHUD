#pragma once

#include <memory>
#include <atomic>
#include <thread>
#include <raylib.h>

class V4L2Camera {
public:
    struct Config {
        int width  = 1280;
        int height = 800;
        int fps    = 120;
    };

    V4L2Camera();
    ~V4L2Camera();

    bool init(const char* device_path, const Config& cfg, const char* nv12_vs, const char* nv12_fs);
    void shutdown();

    bool draw(Rectangle dest);
    bool is_ok() const { return is_ok_; }

private:
    void capture_thread();

    enum class SlotState { IDLE, CAPTURING, READY, RENDERING };

    struct DmaBuf {
        int fd;
        size_t size;
        uint8_t* data;
    };

    struct BufferSlot {
        DmaBuf buf;
        SlotState state;
    };

    int device_fd_ = -1;
    std::vector<BufferSlot> buffers_;
    std::atomic<int> capture_index_ { 0 };
    std::atomic<int> ready_index_ { -1 };
    std::atomic<int> render_index_ { -1 };

    GLuint tex_y_ = 0, tex_uv_ = 0;
    Texture2D rl_tex_y_ { 0 };
    Shader shader_;

    std::atomic<bool> is_ok_ { false };
    std::atomic<bool> running_ { false };
    std::thread capture_thread_;

    int width_, height_;
};
