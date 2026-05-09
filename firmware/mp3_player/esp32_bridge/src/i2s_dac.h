#pragma once
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>

// PCM5102A I2S DAC output.
// I2S port I2S_NUM_0, master transmit, 44100 Hz 16-bit stereo.
// XSMT pin (mute, active low) driven by mute() / unmute().
//
// Thread-safe: write() may be called from the UART handler task or the A2DP
// sink callback (both on Core 0). I2S DMA feeds from the ring buffer.

static constexpr i2s_port_t DAC_I2S_PORT = I2S_NUM_0;
static constexpr int PIN_I2S_BCLK = 26;
static constexpr int PIN_I2S_LRC  = 25;
static constexpr int PIN_I2S_DOUT = 22;
static constexpr int PIN_PCM_XSMT = 27;  // HIGH = unmuted, LOW = muted

// Ring buffer holds ~200 ms of 44100 Hz stereo s16le audio before I2S drains it.
static constexpr size_t DAC_RING_BUF_BYTES = 44100 * 4 / 5;  // 200 ms

class I2sDac {
public:
    bool begin();

    // Write raw PCM s16le stereo bytes into the ring buffer.
    // Returns bytes written (may be less than len if buffer is full).
    size_t write(const uint8_t* pcm, size_t len);

    void mute();
    void unmute();

    // Drain remaining ring buffer samples and mute when empty.
    void flush_and_mute();

    bool is_muted() const { return muted_; }

private:
    bool         muted_    = true;
    RingbufHandle_t ring_  = nullptr;
    TaskHandle_t drain_task_ = nullptr;

    static void drain_task_fn(void* param);
};
