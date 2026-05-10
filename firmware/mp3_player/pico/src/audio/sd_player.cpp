#include "sd_player.h"
#include <pico/mutex.h>
#include <cstring>
#include <cctype>

// minimp3 — header-only MP3 decoder (vendored).
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "vendor/minimp3_ex.h"

// dr_flac — header-only FLAC decoder (vendored).
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "vendor/dr_flac.h"

SdPlayer::SdPlayer(AppState& state) : state_(state) {
    mutex_init(&queue_mtx_);
}

void SdPlayer::run() {
    for (;;) {
        if (!playing_.load()) {
            sleep_ms(10);
            continue;
        }

        // Reload queue if requested from Core 0.
        if (queue_dirty_.load()) {
            queue_dirty_.store(false);
            // queue_ was updated under queue_mtx_ by load_queue().
        }

        if (queue_.empty()) {
            playing_.store(false);
            sleep_ms(10);
            continue;
        }

        const std::string path = queue_.current();
        if (open_track(path)) {
            // Identify format by extension.
            const char* ext = strrchr(path.c_str(), '.');
            bool is_flac = ext && (strcasecmp(ext, ".flac") == 0);

            File f = SD.open(path.c_str(), FILE_READ);
            if (!f) { queue_.next(state_.playback.repeat); continue; }

            if (is_flac) decode_flac(f);
            else         decode_mp3(f);
            f.close();
        }

        if (stop_req_.load())   { stop_req_.store(false); playing_.store(false); continue; }
        if (skip_next_.load())  { skip_next_.store(false); }
        if (skip_prev_.load())  { skip_prev_.store(false); queue_.prev(); queue_.prev(); }

        // Advance queue on natural end-of-track.
        if (!queue_.next(state_.playback.repeat))
            playing_.store(false);
    }
}

void SdPlayer::decode_mp3(File& f) {
    mp3dec_t dec;
    mp3dec_init(&dec);

    static uint8_t  file_buf[16384];
    static int16_t  pcm_buf[DECODE_BUF_SAMPLES];
    size_t buf_filled = 0;

    for (;;) {
        if (stop_req_.load() || skip_next_.load() || skip_prev_.load()) break;

        if (!playing_.load()) { sleep_ms(5); continue; }

        // Refill file buffer.
        const size_t space = sizeof(file_buf) - buf_filled;
        if (space > 0 && f.available()) {
            const size_t got = f.read(file_buf + buf_filled, space);
            buf_filled += got;
        }
        if (buf_filled == 0) break;  // EOF

        mp3dec_frame_info_t info;
        const int samples = mp3dec_decode_frame(&dec, file_buf, static_cast<int>(buf_filled),
                                                 pcm_buf, &info);
        if (info.frame_bytes == 0) break;  // sync error / EOF
        buf_filled -= info.frame_bytes;
        memmove(file_buf, file_buf + info.frame_bytes, buf_filled);

        if (samples > 0) {
            apply_volume(pcm_buf, static_cast<size_t>(samples) * info.channels);
            if (on_pcm_frame) on_pcm_frame(pcm_buf, static_cast<size_t>(samples));
        }

        // Update position.
        if (info.hz > 0 && samples > 0) {
            mutex_enter_blocking(&state_.mtx);
            state_.playback.current.position_s =
                state_.playback.current.position_s + samples / info.hz;
            mutex_exit(&state_.mtx);
        }
    }
}

// dr_flac read/seek callbacks backed by Arduino SD File.
static size_t drflac_read_cb(void* ud, void* out, size_t bytes) {
    return static_cast<File*>(ud)->read(static_cast<uint8_t*>(out), bytes);
}
static drflac_bool32 drflac_seek_cb(void* ud, int offset, drflac_seek_origin origin) {
    auto* f = static_cast<File*>(ud);
    if (origin == drflac_seek_origin_start) return f->seek(offset) ? DRFLAC_TRUE : DRFLAC_FALSE;
    return f->seek(f->position() + offset) ? DRFLAC_TRUE : DRFLAC_FALSE;
}

void SdPlayer::decode_flac(File& f) {
    drflac* pFlac = drflac_open(drflac_read_cb, drflac_seek_cb, &f, nullptr);
    if (!pFlac) return;

    static int16_t pcm_buf[DECODE_BUF_SAMPLES];
    constexpr drflac_uint64 CHUNK = DECODE_BUF_SAMPLES / 2;  // sample pairs

    for (;;) {
        if (stop_req_.load() || skip_next_.load() || skip_prev_.load()) break;
        if (!playing_.load()) { sleep_ms(5); continue; }

        const drflac_uint64 got = drflac_read_pcm_frames_s16(pFlac, CHUNK, pcm_buf);
        if (got == 0) break;

        apply_volume(pcm_buf, static_cast<size_t>(got) * pFlac->channels);
        if (on_pcm_frame) on_pcm_frame(pcm_buf, static_cast<size_t>(got));

        mutex_enter_blocking(&state_.mtx);
        if (pFlac->sampleRate > 0)
            state_.playback.current.position_s += static_cast<uint32_t>(got / pFlac->sampleRate);
        mutex_exit(&state_.mtx);
    }
    drflac_close(pFlac);
}

bool SdPlayer::open_track(const std::string& path) {
    TrackInfo info;
    strncpy(info.path, path.c_str(), sizeof(info.path) - 1);
    read_tags(path, info);
    extract_cover_art(path);  // fills state_.cover_art before mutex section

    mutex_enter_blocking(&state_.mtx);
    state_.playback.current = info;
    state_.playback.queue_index = static_cast<uint16_t>(queue_.current_index());
    mutex_exit(&state_.mtx);

    if (on_track_changed) on_track_changed(info);
    return SD.exists(path.c_str());
}

void SdPlayer::apply_volume(int16_t* pcm, size_t samples) {
    const uint8_t vol = volume_.load();
    if (vol >= 100) return;
    for (size_t i = 0; i < samples; ++i)
        pcm[i] = static_cast<int16_t>(static_cast<int32_t>(pcm[i]) * vol / 100);
}

void SdPlayer::read_tags(const std::string& path, TrackInfo& out) {
    // Minimal ID3v1 (last 128 bytes of file): TAG + 30-byte title + 30-byte artist + ...
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) return;
    const size_t fsize = f.size();
    if (fsize >= 128) {
        f.seek(fsize - 128);
        uint8_t tag[128];
        f.read(tag, 128);
        if (tag[0] == 'T' && tag[1] == 'A' && tag[2] == 'G') {
            memcpy(out.title,  tag +  3, 30);  out.title[30]  = '\0';
            memcpy(out.artist, tag + 33, 30);  out.artist[30] = '\0';
            memcpy(out.album,  tag + 63, 30);  out.album[30]  = '\0';
        }
    }
    f.close();

    // Fallback: use filename stripped of extension as title.
    if (out.title[0] == '\0') {
        const char* slash = strrchr(path.c_str(), '/');
        const char* name  = slash ? slash + 1 : path.c_str();
        const char* dot   = strrchr(name, '.');
        const size_t len  = dot ? static_cast<size_t>(dot - name) : strlen(name);
        strncpy(out.title, name, len < sizeof(out.title) - 1 ? len : sizeof(out.title) - 1);
    }
}

void SdPlayer::extract_cover_art(const std::string& track_path) {
    CoverArtBuf& art = state_.cover_art;

    // Compute the directory containing this track.
    const size_t slash = track_path.rfind('/');
    const std::string dir = (slash != std::string::npos)
                            ? track_path.substr(0, slash)
                            : std::string("/");

    // Common cover art filenames, tried in order.
    static const char* const k_names[] = {
        "cover.jpg", "Cover.jpg", "folder.jpg", "Folder.jpg",
        "album.jpg", "Album.jpg", nullptr
    };

    for (const char* const* n = k_names; *n; ++n) {
        const std::string p = dir + "/" + *n;
        File f = SD.open(p.c_str(), FILE_READ);
        if (!f) continue;

        const size_t got = f.read(art.data, CoverArtBuf::MAX_BYTES);
        f.close();

        // Validate JPEG SOI marker (FF D8).
        if (got >= 4 && art.data[0] == 0xFF && art.data[1] == 0xD8) {
            art.len.store(static_cast<int32_t>(got), std::memory_order_relaxed);
            art.generation.fetch_add(1, std::memory_order_release);
            return;
        }
    }

    // No art found for this track.
    art.len.store(-1, std::memory_order_relaxed);
    art.generation.fetch_add(1, std::memory_order_release);
}

// ── Thread-safe controls ───────────────────────────────────────────────────

void SdPlayer::play()   { playing_.store(true); }
void SdPlayer::pause()  { playing_.store(false); }
void SdPlayer::toggle() { playing_.store(!playing_.load()); }
void SdPlayer::stop()   { stop_req_.store(true); playing_.store(false); }

void SdPlayer::skip_next() { skip_next_.store(true); }
void SdPlayer::skip_prev() { skip_prev_.store(true); }

void SdPlayer::set_volume(uint8_t vol) {
    volume_.store(vol > 100 ? 100 : vol);
    mutex_enter_blocking(&state_.mtx);
    state_.playback.volume = volume_.load();
    mutex_exit(&state_.mtx);
}

void SdPlayer::load_queue(TrackQueue q, bool play_immediately) {
    mutex_enter_blocking(&queue_mtx_);
    queue_ = std::move(q);
    queue_dirty_.store(true);
    mutex_exit(&queue_mtx_);
    if (play_immediately) play();
}

void SdPlayer::jump_to(size_t index) {
    mutex_enter_blocking(&queue_mtx_);
    queue_.jump(index);
    queue_dirty_.store(true);
    mutex_exit(&queue_mtx_);
    skip_next_.store(true);
}
