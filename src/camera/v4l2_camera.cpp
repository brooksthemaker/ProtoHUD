#include "v4l2_camera.h"

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

#include <glad/glad.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

typedef EGLImage (*PFNEGLCREATEIMAGEKHR)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*);
typedef EGLBoolean (*PFNEGLDESTROYIMAGEKHR)(EGLDisplay, EGLImage);
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOES)(GLenum, GLuint);

static PFNEGLCREATEIMAGEKHR eglCreateImageKHR = nullptr;
static PFNEGLDESTROYIMAGEKHR eglDestroyImageKHR = nullptr;
static PFNGLEGLIMAGETARGETTEXTURE2DOES glEGLImageTargetTexture2DOES = nullptr;

V4L2Camera::V4L2Camera() = default;

V4L2Camera::~V4L2Camera() {
    shutdown();
}

bool V4L2Camera::init(const char* device_path, const Config& cfg, const char* nv12_vs, const char* nv12_fs) {
    width_ = cfg.width;
    height_ = cfg.height;

    device_fd_ = open(device_path, O_RDWR | O_NONBLOCK);
    if (device_fd_ < 0) {
        std::cerr << "[v4l2] cannot open " << device_path << "\n";
        return false;
    }

    v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width_;
    fmt.fmt.pix.height = height_;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    if (ioctl(device_fd_, VIDIOC_S_FMT, &fmt) < 0) {
        std::cerr << "[v4l2] VIDIOC_S_FMT failed\n";
        close(device_fd_);
        device_fd_ = -1;
        return false;
    }

    v4l2_streamparm parm = {};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = cfg.fps;
    ioctl(device_fd_, VIDIOC_S_PARM, &parm);

    v4l2_requestbuffers req = {};
    req.count = 3;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(device_fd_, VIDIOC_REQBUFS, &req) < 0) {
        std::cerr << "[v4l2] VIDIOC_REQBUFS failed\n";
        close(device_fd_);
        device_fd_ = -1;
        return false;
    }

    buffers_.resize(req.count);
    for (uint32_t i = 0; i < req.count; i++) {
        v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(device_fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            std::cerr << "[v4l2] VIDIOC_QUERYBUF failed\n";
            close(device_fd_);
            device_fd_ = -1;
            return false;
        }

        buffers_[i].buf.size = buf.length;
        buffers_[i].buf.data = (uint8_t*)mmap(nullptr, buf.length, PROT_READ, MAP_SHARED, device_fd_, buf.m.offset);
        if (buffers_[i].buf.data == MAP_FAILED) {
            std::cerr << "[v4l2] mmap failed\n";
            close(device_fd_);
            device_fd_ = -1;
            return false;
        }
        buffers_[i].state = SlotState::IDLE;

        v4l2_exportbuffer exp = {};
        exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        exp.index = i;
        exp.flags = O_CLOEXEC;
        if (ioctl(device_fd_, VIDIOC_EXPBUF, &exp) < 0) {
            std::cerr << "[v4l2] VIDIOC_EXPBUF failed\n";
            close(device_fd_);
            device_fd_ = -1;
            return false;
        }
        buffers_[i].buf.fd = exp.fd;
    }

    if (!eglCreateImageKHR)
        eglCreateImageKHR = (PFNEGLCREATEIMAGEKHR)eglGetProcAddress("eglCreateImageKHR");
    if (!eglDestroyImageKHR)
        eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHR)eglGetProcAddress("eglDestroyImageKHR");
    if (!glEGLImageTargetTexture2DOES)
        glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOES)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!eglCreateImageKHR || !eglDestroyImageKHR || !glEGLImageTargetTexture2DOES) {
        std::cerr << "[v4l2] EGL extensions not available\n";
        close(device_fd_);
        device_fd_ = -1;
        return false;
    }

    shader_ = LoadShader(nv12_vs, nv12_fs);
    if (shader_.id == 0) {
        std::cerr << "[v4l2] shader load failed\n";
        close(device_fd_);
        device_fd_ = -1;
        return false;
    }

    glGenTextures(1, &tex_y_);
    glGenTextures(1, &tex_uv_);

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(device_fd_, VIDIOC_STREAMON, &type) < 0) {
        std::cerr << "[v4l2] VIDIOC_STREAMON failed\n";
        close(device_fd_);
        device_fd_ = -1;
        return false;
    }

    for (uint32_t i = 0; i < req.count; i++) {
        v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        ioctl(device_fd_, VIDIOC_QBUF, &buf);
    }

    running_ = true;
    capture_thread_ = std::thread(&V4L2Camera::capture_thread, this);

    is_ok_ = true;
    return true;
}

void V4L2Camera::shutdown() {
    running_ = false;
    if (capture_thread_.joinable()) capture_thread_.join();

    if (device_fd_ >= 0) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(device_fd_, VIDIOC_STREAMOFF, &type);

        for (auto& slot : buffers_) {
            if (slot.buf.data && slot.buf.data != (uint8_t*)MAP_FAILED)
                munmap(slot.buf.data, slot.buf.size);
            if (slot.buf.fd >= 0) close(slot.buf.fd);
        }
        close(device_fd_);
        device_fd_ = -1;
    }

    if (tex_y_ != 0) glDeleteTextures(1, &tex_y_);
    if (tex_uv_ != 0) glDeleteTextures(1, &tex_uv_);
    if (shader_.id != 0) UnloadShader(shader_);
}

void V4L2Camera::capture_thread() {
    while (running_) {
        pollfd pfd { device_fd_, POLLIN, 0 };
        int poll_ret = poll(&pfd, 1, 100);
        if (poll_ret <= 0) continue;

        v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(device_fd_, VIDIOC_DQBUF, &buf) < 0) continue;

        int ready_idx = ready_index_.load();
        if (ready_idx >= 0)
            buffers_[ready_idx].state = SlotState::IDLE;

        ready_index_.store(buf.index);
        buffers_[buf.index].state = SlotState::READY;

        ioctl(device_fd_, VIDIOC_QBUF, &buf);
    }
}

bool V4L2Camera::draw(Rectangle dest) {
    int ready_idx = ready_index_.load();
    if (ready_idx < 0) return false;

    buffers_[ready_idx].state = SlotState::RENDERING;

    EGLDisplay dpy = eglGetCurrentDisplay();
    GLint attribs[] = {
        EGL_WIDTH, width_,
        EGL_HEIGHT, height_ / 2,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8,
        EGL_DMA_BUF_PLANE0_FD_EXT, buffers_[ready_idx].buf.fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, width_,
        EGL_NONE
    };
    EGLImage img_y = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
    if (!img_y) return false;

    int uv_offset = (width_ * height_ * 3) / 2;
    GLint attribs_uv[] = {
        EGL_WIDTH, width_,
        EGL_HEIGHT, height_ / 4,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_RG88,
        EGL_DMA_BUF_PLANE0_FD_EXT, buffers_[ready_idx].buf.fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (int)uv_offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, width_,
        EGL_NONE
    };
    EGLImage img_uv = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs_uv);
    if (!img_uv) {
        eglDestroyImageKHR(dpy, img_y);
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, tex_y_);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img_y);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_uv_);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img_uv);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glActiveTexture(GL_TEXTURE0);

    rl_tex_y_.id = tex_y_;
    rl_tex_y_.width = width_;
    rl_tex_y_.height = height_;
    rl_tex_y_.format = PIXELFORMAT_UNCOMPRESSED_R8;
    rl_tex_y_.mipmaps = 1;

    Rectangle src { 0, 0, (float)width_, (float)height_ };
    BeginShaderMode(shader_);
    DrawTexturePro(rl_tex_y_, src, dest, {0, 0}, 0.0f, WHITE);
    EndShaderMode();

    eglDestroyImageKHR(dpy, img_y);
    eglDestroyImageKHR(dpy, img_uv);

    return true;
}
