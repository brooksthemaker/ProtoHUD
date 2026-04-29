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
    // dst must be a valid gl::Fbo with matching dimensions.
    void process(GLuint src_tex, const gl::Fbo& dst, const PostProcessConfig& cfg);

    bool any_enabled(const PostProcessConfig& cfg) const {
        return cfg.edge_enabled || cfg.desat_enabled;
    }

private:
    GLuint prog_ = 0;
    GLuint vbo_  = 0;

    // Cached uniform locations (set at init time)
    GLint loc_scene_      = -1;
    GLint loc_texel_      = -1;
    GLint loc_edge_str_   = -1;
    GLint loc_desat_str_  = -1;
    GLint loc_edge_col_   = -1;
    GLint loc_threshold_  = -1;
    GLint loc_has_depth_  = -1;
    GLint loc_edge_scale_  = -1;
    GLint loc_edge_thresh_ = -1;
    GLint loc_focus_str_   = -1;
    GLint loc_focus_sens_  = -1;

    int w_ = 0, h_ = 0;
};
