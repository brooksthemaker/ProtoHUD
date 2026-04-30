#pragma once
#include "app_state.h"
#include "gl_utils.h"
#include <GLES2/gl2.h>

// ── PostProcessor ─────────────────────────────────────────────────────────────
// Single fullscreen GLES 2.0 post-processing pass: Sobel edge highlight and
// local-contrast background desaturation. Both effects are independently gated
// by PostProcessConfig::edge_enabled / desat_enabled.
//
// Usage (render thread only):
//   if (!post_proc.init(eye_w, eye_h, vs_path, fs_path)) { ... }
//   ...
//   // Each frame:
//   if (post_proc.any_enabled(pp_cfg))
//       post_proc.process(src_tex, dst_fbo, pp_cfg);
//   xr.composite(dst_fbo.tex or src_tex);
//   ...
//   post_proc.shutdown();

class PostProcessor {
public:
    // Allocate the output FBO and compile shaders. Must be called after the GL
    // context is current. Returns false on any GL error.
    bool init(int w, int h, const char* vs_path, const char* fs_path);
    void shutdown();

    // Apply the post-process pass: read src_tex, write result to dst.
    // prev_read: EMA-smoothed reference frame used for motion detection.
    // prev_write: receives the new EMA frame (prev_read blended with src_tex).
    // Use a ping-pong pair per eye; swap read/write each frame.
    void process(GLuint src_tex,
                 const gl::Fbo& prev_read, gl::Fbo& prev_write,
                 const gl::Fbo& dst,
                 const PostProcessConfig& cfg);

    bool any_enabled(const PostProcessConfig& cfg) const {
        return cfg.edge_enabled || cfg.desat_enabled || cfg.motion_enabled;
    }

private:
    GLuint prog_      = 0;
    GLuint blit_prog_ = 0;   // EMA blit: blends src_tex with prev to produce new reference
    GLuint vbo_       = 0;

    // Cached blit uniform locations
    GLint  loc_blit_cur_  = -1;
    GLint  loc_blit_prev_ = -1;
    GLint  loc_blit_rate_ = -1;

    // Cached uniform locations (set at init time)
    GLint loc_scene_         = -1;
    GLint loc_texel_         = -1;
    GLint loc_edge_str_      = -1;
    GLint loc_desat_str_     = -1;
    GLint loc_edge_col_      = -1;
    GLint loc_threshold_     = -1;
    GLint loc_has_depth_     = -1;
    GLint loc_edge_scale_    = -1;
    GLint loc_edge_thresh_   = -1;
    GLint loc_focus_str_     = -1;
    GLint loc_focus_sens_    = -1;
    GLint loc_gate_scale_    = -1;
    GLint loc_prev_frame_    = -1;
    GLint loc_motion_str_    = -1;
    GLint loc_motion_thresh_ = -1;
    GLint loc_motion_radius_ = -1;
    GLint loc_motion_col_    = -1;
    GLint loc_motion_line_   = -1;

    int w_ = 0, h_ = 0;
};
