#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include <GLES2/gl2.h>

typedef void* XRCameraProviderHandle;

// Captures the Beast's built-in passthrough camera (1920×1080@30fps MJPEG)
// via the VITURE camera provider SDK.  Frames are delivered on a dedicated
// SDK thread, decoded from MJPEG to RGBA, and buffered for the render thread.
//
// Usage:
//   VitureCamera cam;
//   if (cam.start(glasses_product_id)) { ... }
//   // in render loop:
//   GLuint tex = 0;
//   cam.get_frame(tex);  // creates/updates GL texture
//   cam.stop();
class VitureCamera {
public:
    VitureCamera();
    ~VitureCamera();

    // glasses_product_id: value returned by xr_device_provider on the connected glasses.
    // The SDK derives the camera VID/PID from this.
    bool start(int glasses_product_id);
    void stop();

    bool is_streaming() const;

    // Upload the latest frame to a GL texture (creates/resizes as needed).
    // out receives the texture id (0 if never populated).
    // Returns true if a new frame was available.
    // Must be called from the render thread.
    bool get_frame(GLuint& out);

    int frame_width()  const { return frame_w_; }
    int frame_height() const { return frame_h_; }

private:
    static void s_frame_cb(const void* frame, void* user_data);
    void on_frame(const void* frame);
    void upload(GLuint& tex);

    XRCameraProviderHandle handle_  = nullptr;
    std::atomic<bool>      running_ { false };

    struct Slot {
        std::mutex              mtx;
        std::vector<uint8_t>    buf;    // RGBA
        int                     w = 0, h = 0;
        bool                    dirty = false;
    } slot_;

    std::atomic<int> frame_w_ { 1920 };
    std::atomic<int> frame_h_ { 1080 };

    // Allocated texture dimensions (render thread only)
    int tex_w_ = 0;
    int tex_h_ = 0;
};
