#include "serial_port.h"
#include "../protocols.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <array>

static speed_t baud_constant(int baud) {
    switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        default:     throw std::invalid_argument("unsupported baud rate");
    }
}

SerialPort::SerialPort(std::string device, int baud)
    : device_(std::move(device)), baud_(baud) {}

SerialPort::~SerialPort() { close(); }

bool SerialPort::open() {
    fd_ = ::open(device_.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
    if (fd_ < 0) return false;

    termios tty {};
    tcgetattr(fd_, &tty);

    speed_t spd = baud_constant(baud_);
    cfsetispeed(&tty, spd);
    cfsetospeed(&tty, spd);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);

    tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL | INLCR);
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_oflag &= ~OPOST;

    tty.c_cc[VMIN]  = 1;
    tty.c_cc[VTIME] = 1;  // 100ms timeout

    tcsetattr(fd_, TCSANOW, &tty);
    tcflush(fd_, TCIOFLUSH);

    running_ = true;
    thread_ = std::thread(&SerialPort::reader_thread, this);
    return true;
}

void SerialPort::close() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

bool SerialPort::is_open() const { return fd_ >= 0; }

bool SerialPort::send(uint8_t cmd, const uint8_t* payload, uint8_t len) {
    if (fd_ < 0) return false;
    std::array<uint8_t, PROTO_MAX_FRAME> buf;
    size_t sz = proto_build(buf.data(), cmd, payload, len);
    ssize_t written = write(fd_, buf.data(), sz);
    return written == static_cast<ssize_t>(sz);
}

void SerialPort::reader_thread() {
    // Parser state machine
    enum class State { WAIT_SYNC_A, WAIT_SYNC_B, READ_CMD, READ_LEN, READ_PAYLOAD, READ_CRC };
    State state = State::WAIT_SYNC_A;

    uint8_t  cmd = 0, expected_len = 0, crc_rx = 0;
    std::vector<uint8_t> payload;
    payload.reserve(256);

    uint8_t byte = 0;

    while (running_) {
        ssize_t n = read(fd_, &byte, 1);
        if (n <= 0) {
            if (!running_) break;
            continue;
        }

        switch (state) {
        case State::WAIT_SYNC_A:
            if (byte == PROTO_SYNC_A) state = State::WAIT_SYNC_B;
            break;

        case State::WAIT_SYNC_B:
            state = (byte == PROTO_SYNC_B) ? State::READ_CMD : State::WAIT_SYNC_A;
            break;

        case State::READ_CMD:
            cmd   = byte;
            state = State::READ_LEN;
            break;

        case State::READ_LEN:
            expected_len = byte;
            payload.clear();
            state = (expected_len > 0) ? State::READ_PAYLOAD : State::READ_CRC;
            break;

        case State::READ_PAYLOAD:
            payload.push_back(byte);
            if (payload.size() == expected_len) state = State::READ_CRC;
            break;

        case State::READ_CRC:
            crc_rx = byte;
            {
                // CRC covers [CMD][LEN][PAYLOAD]
                std::vector<uint8_t> crc_data;
                crc_data.reserve(2 + payload.size());
                crc_data.push_back(cmd);
                crc_data.push_back(expected_len);
                crc_data.insert(crc_data.end(), payload.begin(), payload.end());

                uint8_t crc_calc = proto_crc8(crc_data.data(), crc_data.size());
                if (crc_calc == crc_rx && callback_) {
                    callback_(cmd, payload.data(), static_cast<uint8_t>(payload.size()));
                }
            }
            state = State::WAIT_SYNC_A;
            break;
        }
    }
}
