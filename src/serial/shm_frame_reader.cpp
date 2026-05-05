#include "shm_frame_reader.h"

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

bool ShmFrameReader::open(const char* path) {
    fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) {
        // Not an error during normal startup — FacePanel may not be running yet.
        return false;
    }

    map_ = ::mmap(nullptr, SHM_SIZE, PROT_READ, MAP_SHARED, fd_, 0);
    if (map_ == MAP_FAILED) {
        std::fprintf(stderr, "[shm] mmap failed: %s\n", std::strerror(errno));
        ::close(fd_);
        fd_  = -1;
        map_ = nullptr;
        return false;
    }
    return true;
}

void ShmFrameReader::close() {
    if (map_ != nullptr) {
        ::munmap(map_, SHM_SIZE);
        map_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    last_seq_ = 0xFF;
}

bool ShmFrameReader::poll(GLuint& tex) {
    if (map_ == nullptr) return false;

    const uint8_t* base = static_cast<const uint8_t*>(map_);
    uint8_t seq = base[0];
    if (seq == last_seq_) return false;   // no new frame

    // Copy the pixel data into a local buffer to avoid a torn read while
    // Python is writing.  At 6144 bytes this is ~1µs — negligible.
    uint8_t rgb[W * H * 3];
    std::memcpy(rgb, base + 1, sizeof(rgb));

    last_seq_ = seq;
    upload(tex, rgb);
    return true;
}

void ShmFrameReader::upload(GLuint& tex, const uint8_t* rgb) {
    // Convert RGB → RGBA (ImGui's GL backend expects RGBA internally even
    // when we use GL_RGB on the CPU side, so keep it consistent).
    uint8_t rgba[W * H * 4];
    for (int i = 0; i < W * H; ++i) {
        rgba[i * 4 + 0] = rgb[i * 3 + 0];
        rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
    }

    if (tex == 0) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        // GL_NEAREST is essential — this is a 64×32 pixel-art image; linear
        // interpolation would blur the crisp LED pixel boundaries.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    } else {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}
