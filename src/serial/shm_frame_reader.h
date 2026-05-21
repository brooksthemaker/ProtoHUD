#pragma once

#include <cstdint>
#include <GLES2/gl2.h>

/**
 * Maps /dev/shm/protoface_frame and polls for new frames written by the
 * Protoface Python process.
 *
 * Shared memory layout (12289 bytes for the current 128×32 canvas):
 *   byte 0           uint8  sequence counter — wraps at 256; reader detects
 *                           new frames by comparing against last seen value
 *   bytes 1-12288    uint8  W×H RGB pixel data, row-major (R G B R G B ...)
 *
 * W and H MUST match the Protoface logical canvas
 * (panel_width × chain_length, panel_height × parallel). The current
 * single-mirrored-face layout (2 panels on port 1) is 128×32. If you change
 * the panel layout in Protoface's config.yaml, update H/W below to match, or
 * the preview will read the wrong number of bytes and look garbled.
 *
 * Call open() once after Protoface starts.  Call poll() each frame; it
 * returns true and updates the GL texture when a new frame is available.
 * Call close() on shutdown.
 */
class ShmFrameReader {
public:
    static constexpr int   W            = 128;
    static constexpr int   H            = 32;
    static constexpr int   SHM_SIZE     = 1 + W * H * 3;   // 12289 bytes
    static constexpr const char* DEFAULT_PATH = "/dev/shm/protoface_frame";

    ShmFrameReader() = default;
    ~ShmFrameReader() { close(); }

    // Disallow copy.
    ShmFrameReader(const ShmFrameReader&)            = delete;
    ShmFrameReader& operator=(const ShmFrameReader&) = delete;

    bool open(const char* path = DEFAULT_PATH);
    void close();

    // Poll for a new frame.  If one is available, uploads it to *tex* and
    // returns true.  *tex* is allocated on the first call (set to 0 before
    // the first call; the caller owns the GLuint lifetime).
    bool poll(GLuint& tex);

    bool is_open() const { return map_ != nullptr; }

private:
    void upload(GLuint& tex, const uint8_t* rgb);

    void*    map_      = nullptr;
    int      fd_       = -1;
    uint8_t  last_seq_ = 0xFF;   // initialised to an impossible value so the
                                 // first frame is always treated as new
};
