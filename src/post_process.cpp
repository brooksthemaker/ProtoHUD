#include "post_process.h"
#include "gl_utils.h"
#include <iostream>

bool PostProcessor::init(int w, int h, const char* vs_path, const char* fs_path) {
    w_ = w;
    h_ = h;

    prog_ = gl::build_program(vs_path, fs_path);
    if (!prog_) {
        std::cerr << "[post_process] shader build failed\n";
        return false;
    }

    loc_scene_         = glGetUniformLocation(prog_, "u_scene");
    loc_texel_         = glGetUniformLocation(prog_, "u_texel");
    loc_edge_str_      = glGetUniformLocation(prog_, "u_edge_str");
    loc_desat_str_     = glGetUniformLocation(prog_, "u_desat_str");
    loc_edge_col_      = glGetUniformLocation(prog_, "u_edge_col");
    loc_threshold_     = glGetUniformLocation(prog_, "u_threshold");
    loc_has_depth_     = glGetUniformLocation(prog_, "u_has_depth");
    loc_edge_scale_    = glGetUniformLocation(prog_, "u_edge_scale");
    loc_edge_thresh_   = glGetUniformLocation(prog_, "u_edge_thresh");
    loc_focus_str_     = glGetUniformLocation(prog_, "u_focus_str");
    loc_focus_sens_    = glGetUniformLocation(prog_, "u_focus_sens");
    loc_gate_scale_    = glGetUniformLocation(prog_, "u_gate_scale");
    loc_prev_frame_    = glGetUniformLocation(prog_, "u_prev_frame");
    loc_motion_str_    = glGetUniformLocation(prog_, "u_motion_str");
    loc_motion_thresh_ = glGetUniformLocation(prog_, "u_motion_thresh");
    loc_motion_radius_ = glGetUniformLocation(prog_, "u_motion_radius");
    loc_motion_col_    = glGetUniformLocation(prog_, "u_motion_col");
    loc_motion_line_   = glGetUniformLocation(prog_, "u_motion_line");
    loc_color_prot_    = glGetUniformLocation(prog_, "u_color_prot");
    loc_edge_dilate_   = glGetUniformLocation(prog_, "u_edge_dilate");

    // EMA blit: blends current frame with previous EMA reference.
    // update_rate=1.0 → hard copy (1-frame trail), lower → longer exponential trail.
    static const char* kBlitVs =
        "attribute vec2 a_pos; attribute vec2 a_uv;"
        "varying vec2 v_uv;"
        "void main(){ v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }";
    static const char* kBlitFs =
        "precision mediump float;"
        "uniform sampler2D u_cur;"
        "uniform sampler2D u_prev;"
        "uniform float u_rate;"
        "varying vec2 v_uv;"
        "void main(){"
        "    vec4 c = texture2D(u_cur,  v_uv);"
        "    vec4 p = texture2D(u_prev, v_uv);"
        "    gl_FragColor = mix(p, c, u_rate);"
        "}";
    blit_prog_ = gl::build_program_from_strings(kBlitVs, kBlitFs);
    if (!blit_prog_) {
        std::cerr << "[post_process] blit shader build failed — motion trail disabled\n";
    } else {
        loc_blit_cur_  = glGetUniformLocation(blit_prog_, "u_cur");
        loc_blit_prev_ = glGetUniformLocation(blit_prog_, "u_prev");
        loc_blit_rate_ = glGetUniformLocation(blit_prog_, "u_rate");
    }

    vbo_ = gl::make_quad_vbo();
    if (!vbo_) {
        std::cerr << "[post_process] VBO creation failed\n";
        glDeleteProgram(prog_);
        prog_ = 0;
        return false;
    }
    return true;
}

void PostProcessor::shutdown() {
    if (prog_)      { glDeleteProgram(prog_);       prog_      = 0; }
    if (blit_prog_) { glDeleteProgram(blit_prog_);  blit_prog_ = 0; }
    if (vbo_)       { glDeleteBuffers(1, &vbo_);    vbo_       = 0; }
}

void PostProcessor::process(GLuint src_tex,
                             const gl::Fbo& prev_read, gl::Fbo& prev_write,
                             const gl::Fbo& dst,
                             const PostProcessConfig& cfg) {
    // ── Main effect pass ──────────────────────────────────────────────────────
    dst.bind();
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(prog_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glUniform1i(loc_scene_, 0);

    // Bind EMA reference frame for motion detection (unit 2)
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, prev_read.valid() ? prev_read.tex : 0);
    glUniform1i(loc_prev_frame_, 2);

    glUniform2f(loc_texel_, 1.f / static_cast<float>(w_),
                             1.f / static_cast<float>(h_));

    glUniform1f(loc_edge_str_,  cfg.edge_enabled  ? cfg.edge_strength  : 0.f);
    glUniform1f(loc_desat_str_, cfg.desat_enabled ? cfg.desat_strength : 0.f);

    // ImU32 is IM_COL32(R,G,B,A) = A<<24 | B<<16 | G<<8 | R<<0
    const float er = ((cfg.edge_color >>  0) & 0xFF) / 255.f;
    const float eg = ((cfg.edge_color >>  8) & 0xFF) / 255.f;
    const float eb = ((cfg.edge_color >> 16) & 0xFF) / 255.f;
    glUniform3f(loc_edge_col_, er, eg, eb);

    glUniform1f(loc_threshold_,   cfg.contrast_threshold);
    glUniform1f(loc_has_depth_,   0.f);
    glUniform1f(loc_edge_scale_,  cfg.edge_scale);
    glUniform1f(loc_edge_thresh_, cfg.edge_threshold);

    const float focus_norm = static_cast<float>(cfg.focus_lens_pos) / 1000.0f;
    glUniform1f(loc_focus_str_,  cfg.focus_str);
    glUniform1f(loc_focus_sens_, 1.0f + focus_norm * 3.0f);
    glUniform1f(loc_gate_scale_, cfg.edge_gate_scale);

    const float mr = ((cfg.motion_color >>  0) & 0xFF) / 255.f;
    const float mg = ((cfg.motion_color >>  8) & 0xFF) / 255.f;
    const float mb = ((cfg.motion_color >> 16) & 0xFF) / 255.f;
    glUniform1f(loc_motion_str_,    cfg.motion_enabled ? cfg.motion_strength : 0.f);
    glUniform1f(loc_motion_thresh_, cfg.motion_thresh);
    glUniform1f(loc_motion_radius_, cfg.motion_radius);
    glUniform3f(loc_motion_col_,    mr, mg, mb);
    glUniform1f(loc_motion_line_,   cfg.motion_line);

    glUniform1f(loc_color_prot_,  cfg.desat_enabled ? cfg.color_protect : 0.f);
    glUniform1f(loc_edge_dilate_,  cfg.desat_enabled ? cfg.edge_dilate   : 0.f);

    gl::bind_quad(vbo_);
    gl::draw_quad();
    gl::unbind_quad();

    glUseProgram(0);
    dst.unbind();

    // ── EMA blit: blend src_tex into prev_write using prev_read as old value ──
    // prev_write becomes the new reference frame for next frame's motion detection.
    // update_rate=1.0 → hard copy (crisp, 1-frame trail)
    // update_rate<1.0 → exponential moving average → smooth, longer trail
    if (prev_write.valid() && blit_prog_) {
        prev_write.bind();
        glUseProgram(blit_prog_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src_tex);
        glUniform1i(loc_blit_cur_, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, prev_read.valid() ? prev_read.tex : src_tex);
        glUniform1i(loc_blit_prev_, 1);
        glUniform1f(loc_blit_rate_, cfg.motion_update_rate);
        gl::bind_quad(vbo_);
        gl::draw_quad();
        gl::unbind_quad();
        glUseProgram(0);
        prev_write.unbind();
    }
}
