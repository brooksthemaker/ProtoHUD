#include "timewarp.h"
#include <iostream>

// ── Mat3 implementation ───────────────────────────────────────────────────────

Mat3 Mat3::operator*(const Mat3& rhs) const {
    Mat3 result;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            result.m[i*3+j] = 0.f;
            for (int k = 0; k < 3; k++)
                result.m[i*3+j] += m[i*3+k] * rhs.m[k*3+j];
        }
    return result;
}

Mat3 Mat3::inverse() const {
    float det = m[0]*(m[4]*m[8]-m[5]*m[7])
              - m[1]*(m[3]*m[8]-m[5]*m[6])
              + m[2]*(m[3]*m[7]-m[4]*m[6]);
    if (det == 0.f) return Mat3();

    Mat3 inv;
    inv.m[0] = (m[4]*m[8]-m[5]*m[7]) / det;
    inv.m[1] = -(m[1]*m[8]-m[2]*m[7]) / det;
    inv.m[2] = (m[1]*m[5]-m[2]*m[4]) / det;
    inv.m[3] = -(m[3]*m[8]-m[5]*m[6]) / det;
    inv.m[4] = (m[0]*m[8]-m[2]*m[6]) / det;
    inv.m[5] = -(m[0]*m[5]-m[2]*m[3]) / det;
    inv.m[6] = (m[3]*m[7]-m[4]*m[6]) / det;
    inv.m[7] = -(m[0]*m[7]-m[1]*m[6]) / det;
    inv.m[8] = (m[0]*m[4]-m[1]*m[3]) / det;
    return inv;
}

Vec3 Mat3::transform(const Vec3& v) const {
    return {
        m[0]*v.x + m[1]*v.y + m[2]*v.z,
        m[3]*v.x + m[4]*v.y + m[5]*v.z,
        m[6]*v.x + m[7]*v.y + m[8]*v.z
    };
}

// ── AsyncTimewarp ─────────────────────────────────────────────────────────────

AsyncTimewarp::AsyncTimewarp(const CameraIntrinsics& K,
                             const char* vs_path, const char* fs_path)
    : K_(K), vs_path_(vs_path), fs_path_(fs_path) {}

AsyncTimewarp::~AsyncTimewarp() {
    if (prog_)     { glDeleteProgram(prog_);          prog_     = 0; }
    if (quad_vbo_) { glDeleteBuffers(1, &quad_vbo_);  quad_vbo_ = 0; }
}

bool AsyncTimewarp::init() {
    if (prog_) return true;

    prog_ = gl::build_program(vs_path_.c_str(), fs_path_.c_str());
    if (!prog_) {
        std::cerr << "[timewarp] shader load failed\n";
        return false;
    }

    loc_H_   = glGetUniformLocation(prog_, "warp_matrix");
    loc_tex_ = glGetUniformLocation(prog_, "u_tex");

    // Tell the shader which texture unit to sample
    glUseProgram(prog_);
    if (loc_tex_ >= 0) glUniform1i(loc_tex_, 0);
    glUseProgram(0);

    quad_vbo_ = gl::make_quad_vbo();
    return true;
}

// ── IMU pose capture ──────────────────────────────────────────────────────────

void AsyncTimewarp::begin_frame(const ImuPose& pose) {
    render_pose_ = pose;
}

// ── Homography math ───────────────────────────────────────────────────────────

Mat3 AsyncTimewarp::rotation_matrix(float roll_deg, float pitch_deg, float yaw_deg) const {
    float r = roll_deg  * (3.14159265f / 180.f);
    float p = pitch_deg * (3.14159265f / 180.f);
    float y = yaw_deg   * (3.14159265f / 180.f);

    float cr = cosf(r), sr = sinf(r);
    float cp = cosf(p), sp = sinf(p);
    float cy = cosf(y), sy = sinf(y);

    return Mat3(
         cy*cp,   cy*sp*sr - sy*cr,   cy*sp*cr + sy*sr,
         sy*cp,   sy*sp*sr + cy*cr,   sy*sp*cr - cy*sr,
        -sp,      cp*sr,               cp*cr
    );
}

Mat3 AsyncTimewarp::compute_homography(const ImuPose& render_pose,
                                        const ImuPose& current_pose,
                                        int w, int h) const {
    Mat3 R_render  = rotation_matrix(render_pose.roll,  render_pose.pitch,  render_pose.yaw);
    Mat3 R_current = rotation_matrix(current_pose.roll, current_pose.pitch, current_pose.yaw);

    // Calibration matrix (pixel space)
    Mat3 K(K_.fx, 0,     K_.cx,
           0,     K_.fy, K_.cy,
           0,     0,     1.f);
    Mat3 K_inv = K.inverse();

    // Rotational homography: H = K * R_current * R_render^-1 * K^-1
    Mat3 delta_R = R_current * R_render.inverse();
    Mat3 H       = K * delta_R * K_inv;

    // Convert to [0,1] UV space: scale_out * H * scale_in
    float fw = static_cast<float>(w);
    float fh = static_cast<float>(h);
    Mat3 scale_in(fw,  0,   0,
                   0,   fh,  0,
                   0,   0,   1.f);
    Mat3 scale_out(1.f/fw,  0,      0,
                    0,       1.f/fh, 0,
                    0,       0,      1.f);

    return scale_out * H * scale_in;
}

// ── warp_frame ────────────────────────────────────────────────────────────────
// Warps src_tex into the currently bound framebuffer using the homography
// computed from the difference between render_pose_ and current_pose.

void AsyncTimewarp::warp_frame(GLuint src_tex, int src_w, int src_h,
                                const ImuPose& current_pose) {
    if (!prog_ || !quad_vbo_) return;

    Mat3 H = compute_homography(render_pose_, current_pose, src_w, src_h);

    glUseProgram(prog_);

    // Upload 3×3 homography as a mat3 uniform.
    // GLES2 glUniformMatrix3fv expects column-major order.
    // Our Mat3 is row-major, so we transpose.
    float col_major[9] = {
        H.m[0], H.m[3], H.m[6],
        H.m[1], H.m[4], H.m[7],
        H.m[2], H.m[5], H.m[8],
    };
    if (loc_H_ >= 0)
        glUniformMatrix3fv(loc_H_, 1, GL_FALSE, col_major);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);

    gl::bind_quad(quad_vbo_);
    gl::draw_quad();
    gl::unbind_quad();

    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}
