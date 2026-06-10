#pragma once
// ── gl_async_read.h ───────────────────────────────────────────────────────────
// Double-buffered asynchronous FBO readback. A synchronous glReadPixels into
// client memory drains the whole GPU pipeline (the driver must finish every
// queued command first) — at recording rates that visibly halves the frame
// rate. With a pixel-pack buffer the copy runs GPU-side and we map LAST
// frame's buffer instead, which has had a full capture interval to complete.
//
// Needs an ES 3.0+ context for PBOs; on a plain ES 2.0 context read() falls
// back to the old synchronous path so behaviour degrades, not breaks. (Mesa
// hands back an ES 3.x context even when 2.0 was requested, so the fast path
// is what actually runs on the Pi.)
//
// Render (GL) thread only. Delivered rows are bottom-up exactly as
// glReadPixels produces them — flip on the consumer/worker thread.

#include <GLES3/gl3.h>
#include <cstring>
#include <vector>

#include "gl_utils.h"

namespace gl {

inline bool have_es3() {
    static const bool ok = [] {
        const char* v = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        return v && std::strstr(v, "OpenGL ES 3") != nullptr;
    }();
    return ok;
}

class AsyncFboReader {
public:
    // Kick an async read of `fbo`; deliver the previously kicked frame into
    // `out` when one is ready. Returns true when `out`/`out_w`/`out_h` were
    // filled this call (PBO path: the prior kick's pixels — one interval of
    // latency; ES2 fallback: this frame's, synchronously).
    bool read(Fbo& fbo, std::vector<uint8_t>& out, int& out_w, int& out_h) {
        if (!have_es3()) {
            out.resize(static_cast<size_t>(fbo.w) * fbo.h * 4);
            fbo.bind();
            glReadPixels(0, 0, fbo.w, fbo.h, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
            fbo.unbind();
            out_w = fbo.w; out_h = fbo.h;
            return true;
        }

        if (fbo.w != w_ || fbo.h != h_) reset(fbo.w, fbo.h);

        // Kick this frame's GPU→PBO transfer (returns without waiting).
        fbo.bind();
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[idx_]);
        glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        pending_[idx_] = true;

        // Map the other buffer — kicked a call ago, so it's done (or nearly).
        const int other = idx_ ^ 1;
        bool delivered = false;
        if (pending_[other]) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[other]);
            const size_t bytes = static_cast<size_t>(w_) * h_ * 4;
            void* p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                       static_cast<GLsizeiptr>(bytes),
                                       GL_MAP_READ_BIT);
            if (p) {
                out.resize(bytes);
                std::memcpy(out.data(), p, bytes);
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                out_w = w_; out_h = h_;
                delivered = true;
            }
            pending_[other] = false;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        fbo.unbind();
        idx_ = other;
        return delivered;
    }

    // Drop any in-flight transfer (e.g. recording stopped mid-flight).
    void cancel() { pending_[0] = pending_[1] = false; }

    // Delete the PBOs. GL thread, context still current.
    void release() {
        if (pbo_[0] || pbo_[1]) { glDeleteBuffers(2, pbo_); pbo_[0] = pbo_[1] = 0; }
        w_ = h_ = 0;
        cancel();
    }

private:
    void reset(int w, int h) {
        release();
        w_ = w; h_ = h;
        glGenBuffers(2, pbo_);
        for (int i = 0; i < 2; ++i) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER,
                         static_cast<GLsizeiptr>(static_cast<size_t>(w) * h * 4),
                         nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }

    GLuint pbo_[2]     = {0, 0};
    bool   pending_[2] = {false, false};
    int    w_ = 0, h_ = 0, idx_ = 0;
};

} // namespace gl
