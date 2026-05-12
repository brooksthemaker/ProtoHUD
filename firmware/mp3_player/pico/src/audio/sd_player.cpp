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

// ── Syncsafe / big-endian helpers ─────────────────────────────────────────

static uint32_t syncsafe_to_uint32(const uint8_t b[4]) {
    return ((uint32_t)b[0] << 21) | ((uint32_t)b[1] << 14) |
           ((uint32_t)b[2] << 7)  | b[3];
}

static uint32_t be32(const uint8_t b[4]) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  | b[3];
}

// ── ID3v2 text decoding ────────────────────────────────────────────────────
static void decode_id3_text(uint8_t enc, const uint8_t* raw, size_t len,
                             char* dst, size_t cap) {
    if (cap == 0) return;
    size_t di = 0;

    auto emit_utf8 = [&](uint32_t cp) {
        if (cp < 0x80) {
            if (di + 1 < cap) dst[di++] = static_cast<char>(cp);
        } else if (cp < 0x800) {
            if (di + 2 < cap) {
                dst[di++] = static_cast<char>(0xC0 | (cp >> 6));
                dst[di++] = static_cast<char>(0x80 | (cp & 0x3F));
            }
        } else if (cp < 0x10000) {
            if (di + 3 < cap) {
                dst[di++] = static_cast<char>(0xE0 | (cp >> 12));
                dst[di++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                dst[di++] = static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
    };

    if (enc == 0) {
        for (size_t i = 0; i < len; ++i) {
            uint8_t c = raw[i];
            if (c == 0) break;
            emit_utf8(c);
        }
    } else if (enc == 3) {
        for (size_t i = 0; i < len; ++i) {
            if (raw[i] == 0) break;
            if (di + 1 < cap) dst[di++] = static_cast<char>(raw[i]);
        }
    } else {
        size_t start = 0;
        bool be_order = (enc == 2);
        if (enc == 1 && len >= 2) {
            if      (raw[0] == 0xFE && raw[1] == 0xFF) { be_order = true;  start = 2; }
            else if (raw[0] == 0xFF && raw[1] == 0xFE) { be_order = false; start = 2; }
        }
        for (size_t i = start; i + 1 < len; i += 2) {
            uint16_t cp = be_order
                ? (static_cast<uint16_t>(raw[i]) << 8 | raw[i + 1])
                : (static_cast<uint16_t>(raw[i + 1]) << 8 | raw[i]);
            if (cp == 0) break;
            emit_utf8(cp);
        }
    }
    dst[di] = '\0';
}

// ── MP3 duration estimation ────────────────────────────────────────────────
static const uint16_t k_mpeg1_l3_kbps[16] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
};

static uint32_t estimate_mp3_duration(File& f, uint32_t audio_start,
                                      uint32_t file_size) {
    if (!f.seek(audio_start)) return 0;

    static uint8_t scan_buf[4096];
    const size_t n = f.read(scan_buf, sizeof(scan_buf));
    if (n < 4) return 0;

    uint8_t  hdr[4] = {};
    uint32_t frame_file_offset = audio_start;
    bool found = false;
    for (size_t i = 0; i + 4 <= n; ++i) {
        if (scan_buf[i] != 0xFF) continue;
        const uint8_t b1 = scan_buf[i + 1];
        if ((b1 & 0xE0) != 0xE0) continue;
        if ((b1 & 0x06) != 0x02) continue;
        if ((b1 & 0x18) == 0x08) continue;
        const uint8_t br_idx = (scan_buf[i + 2] >> 4) & 0x0F;
        if (br_idx == 0 || br_idx == 15) continue;
        if (((scan_buf[i + 2] >> 2) & 0x03) == 3) continue;
        memcpy(hdr, scan_buf + i, 4);
        frame_file_offset = audio_start + static_cast<uint32_t>(i);
        found = true;
        break;
    }
    if (!found) return 0;

    const uint8_t mpeg_ver = (hdr[1] >> 3) & 0x03;
    const bool    is_mono  = ((hdr[3] >> 6) & 0x03) == 3;
    const uint8_t sr_idx   = (hdr[2] >> 2) & 0x03;

    const uint32_t k_sr1[4]  = { 44100, 48000, 32000, 0 };
    const uint32_t k_sr2[4]  = { 22050, 24000, 16000, 0 };
    const uint32_t k_sr25[4] = { 11025, 12000,  8000, 0 };
    const uint32_t sample_rate = (mpeg_ver == 3) ? k_sr1[sr_idx]
                               : (mpeg_ver == 2) ? k_sr2[sr_idx]
                               : k_sr25[sr_idx];
    if (sample_rate == 0) return 0;

    const uint8_t si_sz = (mpeg_ver == 3) ? (is_mono ? 17u : 32u)
                                           : (is_mono ?  9u : 17u);

    if (!f.seek(frame_file_offset + 4 + si_sz)) return 0;
    uint8_t xhdr[8];
    if (f.read(xhdr, 8) != 8) return 0;

    if (memcmp(xhdr, "Xing", 4) == 0 || memcmp(xhdr, "Info", 4) == 0) {
        const uint32_t xflags = be32(xhdr + 4);
        if (xflags & 0x01) {
            uint8_t tf[4];
            if (f.read(tf, 4) == 4) {
                const uint32_t total_frames = be32(tf);
                if (total_frames > 0)
                    return static_cast<uint32_t>((uint64_t)total_frames * 1152 / sample_rate);
            }
        }
    }

    const uint8_t  br_idx     = (hdr[2] >> 4) & 0x0F;
    const uint32_t br_kbps    = k_mpeg1_l3_kbps[br_idx];
    if (br_kbps == 0) return 0;
    const uint32_t audio_bytes = (file_size > frame_file_offset)
                                 ? (file_size - frame_file_offset) : 0;
    return static_cast<uint32_t>((uint64_t)audio_bytes * 8 / (br_kbps * 1000));
}

// ── SdPlayer implementation ────────────────────────────────────────────────

SdPlayer::SdPlayer(AppState& state) : state_(state) {
    mutex_init(&queue_mtx_);
}

void SdPlayer::run() {
    for (;;) {
        if (!playing_.load()) {
            sleep_ms(10);
            continue;
        }

        if (queue_dirty_.load()) {
            queue_dirty_.store(false);
        }

        if (queue_.empty()) {
            playing_.store(false);
            sleep_ms(10);
            continue;
        }

        const std::string path = queue_.current();
        if (open_track(path)) {
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

        if (!queue_.next(state_.playback.repeat))
            playing_.store(false);
    }
}

void SdPlayer::decode_mp3(File& f) {
    mp3dec_t dec;
    mp3dec_init(&dec);

    static uint8_t  file_buf[16384];
    static int16_t  pcm_buf[DECODE_BUF_SAMPLES];
    size_t   buf_filled = 0;
    uint64_t total_pcm_samples = 0;
    uint32_t last_hz = 0;

    for (;;) {
        if (stop_req_.load() || skip_next_.load() || skip_prev_.load()) break;

        if (!playing_.load()) { sleep_ms(5); continue; }

        const size_t space = sizeof(file_buf) - buf_filled;
        if (space > 0 && f.available()) {
            const size_t got = f.read(file_buf + buf_filled, space);
            buf_filled += got;
        }
        if (buf_filled == 0) break;

        mp3dec_frame_info_t info;
        const int samples = mp3dec_decode_frame(&dec, file_buf,
                                                static_cast<int>(buf_filled),
                                                pcm_buf, &info);
        if (info.frame_bytes == 0) break;
        buf_filled -= info.frame_bytes;
        memmove(file_buf, file_buf + info.frame_bytes, buf_filled);

        // Sync EQ sample rate on first frame or sample-rate change.
        if (info.hz != 0 && info.hz != last_hz) {
            last_hz = info.hz;
            eq_.set_sample_rate(info.hz);
        }

        // Apply any pending EQ preset change from Core 0.
        const uint8_t preq = eq_preset_req_.exchange(0xFF);
        if (preq != 0xFF) eq_.apply_preset(static_cast<EqPreset>(preq));

        if (samples > 0) {
            apply_volume(pcm_buf, static_cast<size_t>(samples) * info.channels);
            eq_.process(pcm_buf, static_cast<size_t>(samples), info.channels);
            if (on_pcm_frame) on_pcm_frame(pcm_buf, static_cast<size_t>(samples));

            total_pcm_samples += static_cast<uint64_t>(samples);
            if (info.hz > 0) {
                mutex_enter_blocking(&state_.mtx);
                state_.playback.current.position_s =
                    static_cast<uint32_t>(total_pcm_samples / info.hz);
                mutex_exit(&state_.mtx);
            }
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
    constexpr drflac_uint64 CHUNK = DECODE_BUF_SAMPLES / 2;
    uint64_t total_pcm_frames = 0;

    // Prime EQ sample rate from STREAMINFO.
    if (pFlac->sampleRate > 0) eq_.set_sample_rate(pFlac->sampleRate);

    for (;;) {
        if (stop_req_.load() || skip_next_.load() || skip_prev_.load()) break;
        if (!playing_.load()) { sleep_ms(5); continue; }

        // Apply any pending EQ preset change from Core 0.
        const uint8_t preq = eq_preset_req_.exchange(0xFF);
        if (preq != 0xFF) eq_.apply_preset(static_cast<EqPreset>(preq));

        const drflac_uint64 got = drflac_read_pcm_frames_s16(pFlac, CHUNK, pcm_buf);
        if (got == 0) break;

        apply_volume(pcm_buf, static_cast<size_t>(got) * pFlac->channels);
        eq_.process(pcm_buf, static_cast<size_t>(got), pFlac->channels);
        if (on_pcm_frame) on_pcm_frame(pcm_buf, static_cast<size_t>(got));

        total_pcm_frames += got;
        if (pFlac->sampleRate > 0) {
            mutex_enter_blocking(&state_.mtx);
            state_.playback.current.position_s =
                static_cast<uint32_t>(total_pcm_frames / pFlac->sampleRate);
            mutex_exit(&state_.mtx);
        }
    }
    drflac_close(pFlac);
}

bool SdPlayer::open_track(const std::string& path) {
    TrackInfo info;
    strncpy(info.path, path.c_str(), sizeof(info.path) - 1);
    read_tags(path, info);

    const char* ext = strrchr(path.c_str(), '.');
    if (ext && strcasecmp(ext, ".flac") == 0 && info.duration_s == 0) {
        File fmeta = SD.open(path.c_str(), FILE_READ);
        if (fmeta) {
            drflac* pf = drflac_open(drflac_read_cb, drflac_seek_cb, &fmeta, nullptr);
            if (pf) {
                if (pf->sampleRate > 0)
                    info.duration_s = static_cast<uint32_t>(
                        pf->totalPCMFrameCount / pf->sampleRate);
                drflac_close(pf);
            }
            fmeta.close();
        }
    }

    extract_cover_art(path);

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
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) return;
    const uint32_t file_size = static_cast<uint32_t>(f.size());

    const char* dot = strrchr(path.c_str(), '.');
    const bool is_mp3 = dot && strcasecmp(dot, ".mp3") == 0;

    uint32_t id3v2_end = 0;

    uint8_t id3_hdr[10];
    if (f.read(id3_hdr, 10) == 10 &&
        id3_hdr[0] == 'I' && id3_hdr[1] == 'D' && id3_hdr[2] == '3') {

        const uint8_t  ver    = id3_hdr[3];
        const uint8_t  flags  = id3_hdr[5];
        const uint32_t tag_sz = syncsafe_to_uint32(id3_hdr + 6);
        id3v2_end = 10 + tag_sz;

        if (ver == 3 || ver == 4) {
            uint32_t pos = 10;

            if (flags & 0x40) {
                uint8_t exhdr[4];
                if (f.read(exhdr, 4) == 4) {
                    const uint32_t exsz = (ver == 4) ? syncsafe_to_uint32(exhdr)
                                                     : (4 + be32(exhdr));
                    f.seek(pos + exsz);
                    pos += exsz;
                }
            }

            static uint8_t payload[512];

            while (pos + 10 <= id3v2_end) {
                uint8_t fhdr[10];
                if (f.read(fhdr, 10) != 10) break;
                if (fhdr[0] == 0) break;

                const uint32_t fsize = (ver == 4) ? syncsafe_to_uint32(fhdr + 4)
                                                   : be32(fhdr + 4);
                pos += 10;
                if (fsize == 0 || pos + fsize > id3v2_end) {
                    f.seek(pos + fsize);
                    pos += fsize;
                    continue;
                }

                const bool is_tit2 = memcmp(fhdr, "TIT2", 4) == 0;
                const bool is_tpe1 = memcmp(fhdr, "TPE1", 4) == 0;
                const bool is_talb = memcmp(fhdr, "TALB", 4) == 0;
                const bool is_tlen = memcmp(fhdr, "TLEN", 4) == 0;

                if ((is_tit2 || is_tpe1 || is_talb || is_tlen) && fsize >= 2) {
                    const size_t to_read = fsize < sizeof(payload) ? fsize : sizeof(payload);
                    const size_t got = f.read(payload, to_read);
                    if (fsize > sizeof(payload)) f.seek(pos + fsize);

                    if (got >= 2) {
                        char tmp[128];
                        decode_id3_text(payload[0], payload + 1, got - 1, tmp, sizeof(tmp));

                        if (is_tit2 && out.title[0]  == '\0')
                            strncpy(out.title,  tmp, sizeof(out.title)  - 1);
                        if (is_tpe1 && out.artist[0] == '\0')
                            strncpy(out.artist, tmp, sizeof(out.artist) - 1);
                        if (is_talb && out.album[0]  == '\0')
                            strncpy(out.album,  tmp, sizeof(out.album)  - 1);
                        if (is_tlen && out.duration_s == 0) {
                            const long ms = atol(tmp);
                            if (ms > 0) out.duration_s = static_cast<uint32_t>(ms / 1000);
                        }
                    }
                } else {
                    f.seek(pos + fsize);
                }
                pos += fsize;
            }
        }
    }

    if ((out.title[0] == '\0' || out.artist[0] == '\0' || out.album[0] == '\0')
        && file_size >= 128) {
        f.seek(file_size - 128);
        uint8_t tag[128];
        if (f.read(tag, 128) == 128 &&
            tag[0] == 'T' && tag[1] == 'A' && tag[2] == 'G') {
            auto strip = [](char* s, size_t n) {
                for (int i = static_cast<int>(n) - 1; i >= 0; --i) {
                    if (s[i] == ' ' || s[i] == '\0') s[i] = '\0'; else break;
                }
            };
            if (out.title[0]  == '\0') {
                memcpy(out.title,  tag +  3, 30); out.title[30]  = '\0'; strip(out.title,  30);
            }
            if (out.artist[0] == '\0') {
                memcpy(out.artist, tag + 33, 30); out.artist[30] = '\0'; strip(out.artist, 30);
            }
            if (out.album[0]  == '\0') {
                memcpy(out.album,  tag + 63, 30); out.album[30]  = '\0'; strip(out.album,  30);
            }
        }
    }

    if (is_mp3 && out.duration_s == 0)
        out.duration_s = estimate_mp3_duration(f, id3v2_end, file_size);

    f.close();

    if (out.title[0] == '\0') {
        const char* slash = strrchr(path.c_str(), '/');
        const char* name  = slash ? slash + 1 : path.c_str();
        const char* ddot  = strrchr(name, '.');
        const size_t len  = ddot ? static_cast<size_t>(ddot - name) : strlen(name);
        const size_t cap  = sizeof(out.title) - 1;
        strncpy(out.title, name, len < cap ? len : cap);
    }
}

void SdPlayer::extract_cover_art(const std::string& track_path) {
    CoverArtBuf& art = state_.cover_art;

    const size_t slash = track_path.rfind('/');
    const std::string dir = (slash != std::string::npos)
                            ? track_path.substr(0, slash)
                            : std::string("/");

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

        if (got >= 4 && art.data[0] == 0xFF && art.data[1] == 0xD8) {
            art.len.store(static_cast<int32_t>(got), std::memory_order_relaxed);
            art.generation.fetch_add(1, std::memory_order_release);
            return;
        }
    }

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

void SdPlayer::set_eq_preset(EqPreset p) {
    eq_preset_req_.store(static_cast<uint8_t>(p));
    mutex_enter_blocking(&state_.mtx);
    state_.playback.eq_preset = p;
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
