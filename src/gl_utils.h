#pragma once
// ── gl_utils.h ────────────────────────────────────────────────────────────────
// Shared OpenGL ES 2 helpers: shader compilation, quad VBO, FBO.
// Include after <GLES2/gl2.h> and <EGL/egl.h>.

#include <GLES2/gl2.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace gl {

// ── Shader compilation ────────────────────────────────────────────────────────

inline std::string load_file(const char* path) {
    std::ifstream f(path);
    if (!f) { std::cerr << "[gl] cannot open shader: " << path << "\n"; return {}; }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::cerr << "[gl] shader compile error:\n" << log << "\n";
        glDeleteShader(s);
        return 0;
    }
    return s;
}

inline GLuint link_program(GLuint vs, GLuint fs) {
    if (!vs || !fs) return 0;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    // Bind canonical attribute locations before linking
    glBindAttribLocation(prog, 0, "a_pos");
    glBindAttribLocation(prog, 1, "a_uv");
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::cerr << "[gl] program link error:\n" << log << "\n";
        glDeleteProgram(prog);
        return 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

inline GLuint build_program(const char* vs_path, const char* fs_path) {
    std::string vsrc = load_file(vs_path);
    std::string fsrc = load_file(fs_path);
    if (vsrc.empty() || fsrc.empty()) return 0;
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   vsrc.c_str());
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fsrc.c_str());
    return link_program(vs, fs);
}

inline GLuint build_program_from_strings(const char* vs_src, const char* fs_src) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    return link_program(vs, fs);
}

// ── Fullscreen quad VBO ───────────────────────────────────────────────────────
// Positions in NDC [-1,1], UVs in [0,1]. Layout: {x, y, u, v}.
// Draw with GL_TRIANGLE_STRIP, 4 vertices.

inline GLuint make_quad_vbo() {
    static const float verts[] = {
        -1.f, -1.f,  0.f, 0.f,
         1.f, -1.f,  1.f, 0.f,
        -1.f,  1.f,  0.f, 1.f,
         1.f,  1.f,  1.f, 1.f,
    };
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return vbo;
}

inline void bind_quad(GLuint vbo) {
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
}

inline void unbind_quad() {
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

inline void draw_quad() {
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// ── Framebuffer object ────────────────────────────────────────────────────────

struct Fbo {
    GLuint fbo = 0;
    GLuint tex = 0;   // RGBA8 color attachment
    GLuint rbo = 0;   // depth renderbuffer
    int    w   = 0;
    int    h   = 0;

    bool valid() const { return fbo != 0; }

    void bind()   const { glBindFramebuffer(GL_FRAMEBUFFER, fbo); glViewport(0, 0, w, h); }
    void unbind() const { glBindFramebuffer(GL_FRAMEBUFFER, 0); }

    void destroy() {
        if (fbo) { glDeleteFramebuffers(1, &fbo);  fbo = 0; }
        if (tex) { glDeleteTextures(1, &tex);       tex = 0; }
        if (rbo) { glDeleteRenderbuffers(1, &rbo);  rbo = 0; }
    }
};

inline Fbo make_fbo(int w, int h) {
    Fbo f;
    f.w = w; f.h = h;

    glGenTextures(1, &f.tex);
    glBindTexture(GL_TEXTURE_2D, f.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenRenderbuffers(1, &f.rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, f.rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glGenFramebuffers(1, &f.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, f.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, f.tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, f.rbo);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[gl] FBO incomplete: 0x" << std::hex << status << std::dec << "\n";
        f.destroy();
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return f;
}

// ── Simple 2D textured quad blit (src tex → current viewport) ─────────────────
// Uses an internal program compiled on first call (not thread-safe).

inline void blit_tex(GLuint tex, GLuint vbo,
                     float x, float y, float w, float h,
                     int screen_w, int screen_h,
                     GLuint blit_prog) {
    glUseProgram(blit_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(blit_prog, "u_tex"), 0);

    // Transform quad to pixel rect [x,y,w,h] in screen space
    float sx =  2.f * w  / screen_w;
    float sy =  2.f * h  / screen_h;
    float tx =  2.f * x  / screen_w - 1.f + sx * 0.5f;  // not right, use model matrix
    float ty = -2.f * y  / screen_h + 1.f - sy * 0.5f;
    // Just pass the NDC rect directly via uniform
    glUniform4f(glGetUniformLocation(blit_prog, "u_rect"),
                2.f * x / screen_w - 1.f,
                1.f - 2.f * (y + h) / screen_h,
                2.f * (x + w) / screen_w - 1.f,
                1.f - 2.f * y / screen_h);

    bind_quad(vbo);
    draw_quad();
    unbind_quad();
    glUseProgram(0);
}

} // namespace gl
