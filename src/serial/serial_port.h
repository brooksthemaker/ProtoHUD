#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

// Callback type: invoked on the reader thread for each complete, CRC-validated frame.
using FrameCallback = std::function<void(uint8_t cmd, const uint8_t* payload, uint8_t len)>;

// POSIX serial port with async frame reader.
// Frame format: [0xAA][0x55][CMD][LEN][PAYLOAD...][CRC8]
class SerialPort {
public:
    explicit SerialPort(std::string device, int baud);
    ~SerialPort();

    bool open();
    void close();
    bool is_open() const;

    // Register callback invoked for each valid inbound frame.
    void set_frame_callback(FrameCallback cb) { callback_ = std::move(cb); }

    // Send a framed packet.
    bool send(uint8_t cmd, const uint8_t* payload = nullptr, uint8_t len = 0);

    const std::string& device() const { return device_; }

private:
    void reader_thread();

    std::string       device_;
    int               baud_;
    int               fd_ = -1;
    std::atomic<bool> running_ { false };
    std::thread       thread_;
    FrameCallback     callback_;
};
