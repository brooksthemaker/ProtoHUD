#include "shm_pusher_output.h"

#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <opencv2/imgproc.hpp>

namespace face {

ShmPusherOutput::ShmPusherOutput(int width, int height, std::string path)
    : w_(width), h_(height), path_(std::move(path)) {
    size_ = static_cast<size_t>(1) + static_cast<size_t>(w_) * h_ * 3;
}

ShmPusherOutput::~ShmPusherOutput() { close(); }

bool ShmPusherOutput::open() {
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0660);
    if (fd_ < 0) return false;
    if (::ftruncate(fd_, static_cast<off_t>(size_)) != 0) {
        ::close(fd_); fd_ = -1; return false;
    }
    void* m = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (m == MAP_FAILED) { ::close(fd_); fd_ = -1; return false; }
    map_ = static_cast<uint8_t*>(m);
    return true;
}

void ShmPusherOutput::show(const cv::Mat& rgb) {
    if (!map_) return;

    // Normalise to the expected (h_, w_) CV_8UC3 contiguous buffer.
    cv::Mat canvas = rgb;
    if (canvas.type() != CV_8UC3)
        canvas.convertTo(canvas, CV_8UC3);
    if (canvas.cols != w_ || canvas.rows != h_)
        cv::resize(canvas, canvas, cv::Size(w_, h_), 0, 0, cv::INTER_NEAREST);
    if (!canvas.isContinuous())
        canvas = canvas.clone();

    std::memcpy(map_ + 1, canvas.data, static_cast<size_t>(w_) * h_ * 3);
    map_[0] = ++seq_;   // publish: bump the sequence counter last
}

void ShmPusherOutput::close() {
    if (map_) {
        std::memset(map_ + 1, 0, static_cast<size_t>(w_) * h_ * 3);   // blank
        map_[0] = ++seq_;
        ::munmap(map_, size_);
        map_ = nullptr;
    }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

} // namespace face
