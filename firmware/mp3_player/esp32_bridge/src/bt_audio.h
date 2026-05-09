#pragma once
#include <BluetoothA2DPSource.h>
#include <BluetoothA2DPSink.h>
#include <atomic>
#include <functional>
#include "i2s_dac.h"

enum class BtMode : uint8_t { OFF = 0, SOURCE = 1, SINK = 2 };

// Manages Bluetooth Classic A2DP source and sink.
// Source mode: streams decoded PCM arriving from Pico (via ring buffer) to BT headphones.
// Sink  mode:  receives A2DP audio from a phone, writes directly to PCM5102 via I2sDac.
//
// A2DP source and sink cannot run simultaneously — call stop() before switching.
// Full BT stack deinit/reinit takes ~600 ms; this is built into stop().

class BtAudio {
public:
    explicit BtAudio(I2sDac& dac);

    // Start advertising as an A2DP source device.
    bool start_source(const char* name);

    // Start advertising as an A2DP sink device (receives from phone).
    bool start_sink(const char* name);

    // Orderly shutdown — blocks until BT stack is idle.
    void stop();

    // Feed PCM audio from the Pico into the source ring buffer.
    // Called by UartHandler when CMD_AUDIO_FRAME arrives.
    void push_source_audio(const uint8_t* pcm, size_t bytes);

    BtMode mode()      const { return mode_.load(); }
    bool   connected() const { return connected_.load(); }
    int8_t rssi()      const { return rssi_.load(); }
    void   peer_name(char out[33]) const;

    // Callback invoked on connection state changes.
    std::function<void(bool connected, const uint8_t* addr, const char* name)> on_connection_changed;

private:
    // A2DP source data callback — drains source_ring_.
    static int32_t source_data_cb(Frame* frame, int32_t frame_count);

    // A2DP sink data callback — writes to DAC.
    static void sink_data_cb(const uint8_t* data, uint32_t len);

    // A2DP connection state callback.
    static void connection_state_cb(esp_a2d_connection_state_t state, void* obj);

    I2sDac& dac_;

    BluetoothA2DPSource a2dp_src_;
    BluetoothA2DPSink   a2dp_sink_;

    std::atomic<BtMode> mode_      { BtMode::OFF };
    std::atomic<bool>   connected_ { false };
    std::atomic<int8_t> rssi_      { 0 };

    char peer_name_[33] = {};

    // Ring buffer for source audio (filled by Pico UART frames).
    // ~400 ms of audio at 44100 Hz stereo 16-bit.
    static constexpr size_t SOURCE_RING_BYTES = 44100 * 4 * 2 / 5;
    RingbufHandle_t source_ring_ = nullptr;

    static BtAudio* instance_;
};
