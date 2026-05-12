#include "bt_audio.h"
#include <Arduino.h>
#include <cstring>

BtAudio* BtAudio::instance_ = nullptr;

BtAudio::BtAudio(I2sDac& dac) : dac_(dac) {
    instance_ = this;
    source_ring_ = xRingbufferCreate(SOURCE_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
}

bool BtAudio::start_source(const char* name) {
    if (mode_.load() != BtMode::OFF) return false;
    a2dp_src_.set_auto_reconnect(false);
    a2dp_src_.set_on_connection_state_changed(connection_state_cb, this);
    a2dp_src_.start(name, source_data_cb);
    mode_.store(BtMode::SOURCE);
    dac_.unmute();
    return true;
}

bool BtAudio::start_sink(const char* name) {
    if (mode_.load() != BtMode::OFF) return false;
    a2dp_sink_.set_auto_reconnect(false);
    a2dp_sink_.set_on_connection_state_changed(connection_state_cb, this);
    a2dp_sink_.set_stream_reader(sink_data_cb, false);
    a2dp_sink_.start(name);
    mode_.store(BtMode::SINK);
    dac_.unmute();
    return true;
}

void BtAudio::stop() {
    BtMode m = mode_.load();
    if (m == BtMode::OFF) return;

    dac_.flush_and_mute();

    if (m == BtMode::SOURCE) {
        a2dp_src_.disconnect();
        const uint32_t deadline = millis() + 2000;
        while (a2dp_src_.get_connection_state() != ESP_A2D_CONNECTION_STATE_DISCONNECTED
               && millis() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        a2dp_src_.end(false);
    } else {
        a2dp_sink_.disconnect();
        const uint32_t deadline = millis() + 2000;
        while (a2dp_sink_.get_connection_state() != ESP_A2D_CONNECTION_STATE_DISCONNECTED
               && millis() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        a2dp_sink_.end(false);
    }

    mode_.store(BtMode::OFF);
    connected_.store(false);
    vTaskDelay(pdMS_TO_TICKS(600));
}

void BtAudio::push_source_audio(const uint8_t* pcm, size_t bytes) {
    if (source_ring_)
        xRingbufferSend(source_ring_, pcm, bytes, 0);
}

void BtAudio::peer_name(char out[33]) const {
    strncpy(out, peer_name_, 32);
    out[32] = '\0';
}

// ── Static callbacks ──────────────────────────────────────────────────────────

int32_t BtAudio::source_data_cb(Frame* frame, int32_t frame_count) {
    if (!instance_ || !instance_->source_ring_) return 0;
    const size_t wanted = static_cast<size_t>(frame_count) * sizeof(Frame);
    size_t item_size;
    void* item = xRingbufferReceiveUpTo(instance_->source_ring_, &item_size,
                                        0, wanted);
    if (!item) {
        // Underrun: send silence.
        memset(frame, 0, wanted);
        return frame_count;
    }
    const size_t frames_got = item_size / sizeof(Frame);
    memcpy(frame, item, frames_got * sizeof(Frame));
    vRingbufferReturnItem(instance_->source_ring_, item);
    if (frames_got < static_cast<size_t>(frame_count))
        memset(frame + frames_got, 0, (frame_count - frames_got) * sizeof(Frame));
    return frame_count;
}

void BtAudio::sink_data_cb(const uint8_t* data, uint32_t len) {
    if (instance_)
        instance_->dac_.write(data, len);
}

void BtAudio::connection_state_cb(esp_a2d_connection_state_t state, void* obj) {
    auto* self = static_cast<BtAudio*>(obj);
    const bool connected = (state == ESP_A2D_CONNECTION_STATE_CONNECTED);
    self->connected_.store(connected);
    if (self->on_connection_changed)
        self->on_connection_changed(connected, nullptr, self->peer_name_);
}
