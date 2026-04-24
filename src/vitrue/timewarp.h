#pragma once
// ── timewarp.h ────────────────────────────────────────────────────────────────
// Async rotational timewarp using a GLSL homography warp shader.
// Reduces perceived rotational latency from ~16 ms to ~4 ms by re-projecting
// a previously rendered eye frame using the latest IMU pose.

#include "../app_state.h"
#include "../gl_utils.h"

#include <GLES2/gl2.h>
#include <cmath>
#include <cstring>
#include <string>

// ── 3×3 matrix (row-major) ────────────────────────────────────────────────────

struct Vec3 { float x, y, z; };

struct Mat3 {
    float m[9] = {};

    Mat3() = default;
    Mat3(float v00, float v01, float v02,
         float v10, float v11, float v12,
         float v20, float v21, float v22) {
        m[0] = v00; m[1] = v01; m[2] = v02;
        m[3] = v10; m[4] = v11; m[5] = v12;
        m[6] = v20; m[7] = v21; m[8] = v22;
    }

    Mat3  operator*(const Mat3& rhs) const;
    Mat3  inverse()                   const;
    Vec3  transform(const Vec3& v)    const;
};

// ── Camera intrinsics ─────────────────────────────────────────────────────────

struct CameraIntrinsics {
    float fx, fy;   // focal length in pixels
    float cx, cy;   // principal point in pixels
};

// ── AsyncTimewarp ─────────────────────────────────────────────────────────────

class AsyncTimewarp {
public:
    AsyncTimewarp(const CameraIntrinsics& K,
                  const char* vs_path = "assets/shaders/timewarp.vs",
                  const char* fs_path = "assets/shaders/timewarp.fs");
    ~AsyncTimewarp();

    // Call once from the render thread after the GL context is current.
    bool init();

    // Record the IMU pose at the start of eye rendering.
    void begin_frame(const ImuPose& pose);

    // Warp src_tex (src_w × src_h) into the currently bound framebuffer
    // (filling the current GL viewport) using the latest IMU pose.
    void warp_frame(GLuint src_tex, int src_w, int src_h,
                    const ImuPose& current_pose);

private:
    Mat3 rotation_matrix(float roll_deg, float pitch_deg, float yaw_deg) const;
    Mat3 compute_homography(const ImuPose& render_pose, const ImuPose& current_pose,
                            int width, int height) const;

    CameraIntrinsics K_;
    ImuPose          render_pose_ {};
    std::string      vs_path_, fs_path_;

    GLuint prog_     = 0;
    GLuint quad_vbo_ = 0;
    GLint  loc_H_    = -1;   // warp_matrix uniform
    GLint  loc_tex_  = -1;   // u_tex uniform
};
