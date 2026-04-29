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

    loc_scene_       = glGetUniformLocation(prog_, "u_scene");
    loc_texel_       = glGetUniformLocation(prog_, "u_texel");
    loc_edge_str_    = glGetUniformLocation(prog_, "u_edge_str");
    loc_desat_str_   = glGetUniformLocation(prog_, "u_desat_str");
    loc_edge_col_    = glGetUniformLocation(prog_, "u_edge_col");
    loc_threshold_   = glGetUniformLocation(prog_, "u_threshold");
    loc_has_depth_   = glGetUniformLocation(prog_, "u_has_depth");
    loc_edge_scale_  = glGetUniformLocation(prog_, "u_edge_scale");
    loc_edge_thresh_ = glGetUniformLocation(prog_, "u_edge_thresh");

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
    if (prog_) { glDeleteProgram(prog_); prog_ = 0; }
    if (vbo_)  { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
}

void PostProcessor::process(GLuint src_tex, const gl::Fbo& dst,
                             const PostProcessConfig& cfg) {
    dst.bind();
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(prog_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glUniform1i(loc_scene_, 0);

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

    gl::bind_quad(vbo_);
    gl::draw_quad();
    gl::unbind_quad();

    glUseProgram(0);
    dst.unbind();
}
