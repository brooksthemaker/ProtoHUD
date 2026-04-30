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

    static const char* kBlitVs =
        "attribute vec2 a_pos; attribute vec2 a_uv;"
        "varying vec2 v_uv;"
        "void main(){ v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }";
    static const char* kBlitFs =
        "precision mediump float;"
        "uniform sampler2D u_tex;"
        "varying vec2 v_uv;"
        "void main(){ gl_FragColor = texture2D(u_tex, v_uv); }";
    blit_prog_ = gl::build_program_from_strings(kBlitVs, kBlitFs);
    if (!blit_prog_)
        std::cerr << "[post_process] blit shader build failed — motion prev-frame copy disabled\n";

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

void PostProcessor::process(GLuint src_tex, gl::Fbo& prev_fbo, const gl::Fbo& dst,
                             const PostProcessConfig& cfg) {
    dst.bind();
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(prog_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glUniform1i(loc_scene_, 0);

    // Bind previous frame for motion detection (unit 2; stays black until second frame)
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, prev_fbo.valid() ? prev_fbo.tex : 0);
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

    // Focus-based sharpness: sensitivity scales with lens proximity (close = narrow DoF = stronger separation)
    const float focus_norm = static_cast<float>(cfg.focus_lens_pos) / 1000.0f;
    glUniform1f(loc_focus_str_,  cfg.focus_str);
    glUniform1f(loc_focus_sens_, 1.0f + focus_norm * 3.0f);
    glUniform1f(loc_gate_scale_, cfg.edge_gate_scale);

    // Motion highlight uniforms
    const float mr = ((cfg.motion_color >>  0) & 0xFF) / 255.f;
    const float mg = ((cfg.motion_color >>  8) & 0xFF) / 255.f;
    const float mb = ((cfg.motion_color >> 16) & 0xFF) / 255.f;
    glUniform1f(loc_motion_str_,    cfg.motion_enabled ? cfg.motion_strength : 0.f);
    glUniform1f(loc_motion_thresh_, cfg.motion_thresh);
    glUniform1f(loc_motion_radius_, cfg.motion_radius);
    glUniform3f(loc_motion_col_,    mr, mg, mb);
    glUniform1f(loc_motion_line_,   cfg.motion_line);

    gl::bind_quad(vbo_);
    gl::draw_quad();
    gl::unbind_quad();

    glUseProgram(0);
    dst.unbind();

    // Copy src_tex → prev_fbo so next frame can compare against this one.
    if (prev_fbo.valid() && blit_prog_) {
        prev_fbo.bind();
        glUseProgram(blit_prog_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src_tex);
        glUniform1i(glGetUniformLocation(blit_prog_, "u_tex"), 0);
        gl::bind_quad(vbo_);
        gl::draw_quad();
        gl::unbind_quad();
        glUseProgram(0);
        prev_fbo.unbind();
    }
}
