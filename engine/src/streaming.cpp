#include "hsynth/streaming.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "hsynth/fileio.h"
#include "hsynth/sample_set.h"

namespace hsynth {

namespace {

std::mutex g_optMutex;
SampleLoadOptions g_opt;

int seek64(FILE* f, int64_t off) {
#ifdef _WIN32
    return _fseeki64(f, off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

inline float decode(const uint8_t* p, int format, int bits) {
    if (format == 3) {
        if (bits == 32) { float f; std::memcpy(&f, p, 4); return f; }
        double d; std::memcpy(&d, p, 8); return (float)d;
    }
    switch (bits) {
        case 8: return ((int)p[0] - 128) / 128.0f;
        case 16: return (int16_t)rd16(p) / 32768.0f;
        case 24: return (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) / 2147483648.0f;
        default: return (int32_t)rd32(p) / 2147483648.0f;
    }
}

}  // namespace

void setSampleLoadOptions(const SampleLoadOptions& o) {
    std::lock_guard<std::mutex> lock(g_optMutex);
    g_opt = o;
    g_opt.preloadFrames = std::max<int64_t>(4096, o.preloadFrames);
}

SampleLoadOptions sampleLoadOptions() {
    std::lock_guard<std::mutex> lock(g_optMutex);
    return g_opt;
}

bool readStreamFrames(FILE* f, const StreamSource& src, int64_t start, int64_t count, float* left, float* right,
                      std::vector<uint8_t>& scratch) {
    if (count <= 0) return true;
    const size_t bytes = (size_t)count * (size_t)src.blockAlign;
    if (scratch.size() < bytes) scratch.resize(bytes);
    size_t got = 0;
    if (f && seek64(f, src.dataOffset + start * src.blockAlign) == 0) got = std::fread(scratch.data(), 1, bytes, f);
    const int64_t frames = (int64_t)(got / (size_t)src.blockAlign);
    const int b = src.bits / 8;
    for (int64_t i = 0; i < frames; ++i) {
        const uint8_t* p = scratch.data() + (size_t)i * (size_t)src.blockAlign;
        left[i] = decode(p, src.format, src.bits);
        if (right) right[i] = src.fileChannels >= 2 ? decode(p + b, src.format, src.bits) : left[i];
    }
    for (int64_t i = frames; i < count; ++i) {
        left[i] = 0.0f;
        if (right) right[i] = 0.0f;
    }
    return frames == count;
}

// ------------------------------------------------------------------ streamer

void DiskStreamer::start() {
    if (running_.load()) return;
    if (!slots_) {
        slots_.reset(new StreamSlot[kSlots]);
        for (int i = 0; i < kSlots; ++i)
            for (auto& r : slots_[i].ring) r.reset(new float[StreamSlot::kRing + StreamSlot::kMirror]());
    }
    quit_.store(false);
    running_.store(true);
    thread_ = std::thread([this] { threadMain(); });
}

void DiskStreamer::stop() {
    if (!running_.load()) return;
    quit_.store(true);
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
    for (int i = 0; slots_ && i < kSlots; ++i) {
        StreamSlot& s = slots_[i];
        if (s.file) std::fclose(s.file);
        s.file = nullptr;
        if (s.state.load() == StreamSlot::kReleased) s.state.store(StreamSlot::kFree);
    }
}

StreamSlot* DiskStreamer::claim(const Sample* smp, int64_t startFrame) {
    if (!slots_ || !running_.load(std::memory_order_relaxed)) return nullptr;
    for (int k = 0; k < kSlots; ++k) {
        const int i = (claimHint_ + k) % kSlots;
        StreamSlot& s = slots_[i];
        if (s.state.load(std::memory_order_acquire) != StreamSlot::kFree) continue;
        s.sample = smp;
        s.start = startFrame;
        s.written.store(startFrame, std::memory_order_relaxed);
        s.low.store(startFrame, std::memory_order_relaxed);
        s.state.store(StreamSlot::kActive, std::memory_order_release);
        claimHint_ = (i + 1) % kSlots;
        active_.fetch_add(1, std::memory_order_relaxed);
        pending_.store(true, std::memory_order_release);
        return &s;
    }
    return nullptr;
}

void DiskStreamer::release(StreamSlot* slot) {
    if (!slot) return;
    slot->state.store(StreamSlot::kReleased, std::memory_order_release);
    active_.fetch_sub(1, std::memory_order_relaxed);
    pending_.store(true, std::memory_order_release);
}

bool DiskStreamer::waitUnused(const SampleSet& set) {
    if (!slots_) return true;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        bool used = false;
        for (int i = 0; i < kSlots && !used; ++i) {
            const StreamSlot& s = slots_[i];
            if (s.state.load(std::memory_order_acquire) == StreamSlot::kFree) continue;
            for (const auto& sp : set.samples)
                if (sp.get() == s.sample) { used = true; break; }
        }
        if (!used || !running_.load()) return true;
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(2)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void DiskStreamer::waitFor(const StreamSlot& s, int64_t frameEnd) const {
    while (running_.load(std::memory_order_relaxed) && s.written.load(std::memory_order_acquire) < frameEnd)
        std::this_thread::yield();
}

bool DiskStreamer::fill(StreamSlot& s) {
    constexpr int64_t kChunk = 4096;
    const Sample* smp = s.sample;
    if (!s.file) {
        s.file = fopenUtf8(smp->stream.path, "rb");
        if (!s.file) {
            // unreadable: mark everything as arrived (silence) so the voice ends normally
            s.written.store(smp->frames, std::memory_order_release);
            return false;
        }
    }
    const int64_t w = s.written.load(std::memory_order_relaxed);
    const int64_t low = std::max(s.start, s.low.load(std::memory_order_acquire));
    const int64_t limit = std::min(smp->frames, low + StreamSlot::kRing);
    if (w >= limit) return false;
    const int64_t n = std::min(kChunk, limit - w);
    for (auto& t : tmp_) if ((int64_t)t.size() < n) t.resize((size_t)n);
    const bool stereo = smp->channels == 2;
    readStreamFrames(s.file, smp->stream, w, n, tmp_[0].data(), stereo ? tmp_[1].data() : nullptr, scratch_);
    bytesRead_.fetch_add((uint64_t)(n * smp->stream.blockAlign), std::memory_order_relaxed);
    for (int c = 0; c < (stereo ? 2 : 1); ++c) {
        float* ring = s.ring[c].get();
        for (int64_t i = 0; i < n;) {
            const int64_t f = w + i;
            const int idx = (int)(f % StreamSlot::kRing);
            const int run = (int)std::min<int64_t>(n - i, StreamSlot::kRing - idx);
            std::memcpy(ring + idx, tmp_[c].data() + i, sizeof(float) * (size_t)run);
            if (idx < StreamSlot::kMirror) {
                const int m = std::min(run, StreamSlot::kMirror - idx);
                std::memcpy(ring + StreamSlot::kRing + idx, tmp_[c].data() + i, sizeof(float) * (size_t)m);
            }
            i += run;
        }
    }
    s.written.store(w + n, std::memory_order_release);
    return true;
}

void DiskStreamer::threadMain() {
    while (!quit_.load()) {
        bool did = false;
        // the slot that is closest to running dry goes first
        for (int pass = 0; pass < 2; ++pass) {
            for (int i = 0; i < kSlots; ++i) {
                StreamSlot& s = slots_[i];
                const int st = s.state.load(std::memory_order_acquire);
                if (st == StreamSlot::kReleased) {
                    if (s.file) std::fclose(s.file);
                    s.file = nullptr;
                    s.sample = nullptr;
                    s.state.store(StreamSlot::kFree, std::memory_order_release);
                    continue;
                }
                if (st != StreamSlot::kActive) continue;
                const int64_t lead = s.written.load(std::memory_order_relaxed) - s.low.load(std::memory_order_relaxed);
                if (pass == 0 && lead > StreamSlot::kRing / 2) continue;  // urgent ones first
                if (fill(s)) did = true;
            }
        }
        if (!did) {
            std::unique_lock<std::mutex> lock(wakeMutex_);
            if (!pending_.exchange(false)) wake_.wait_for(lock, std::chrono::milliseconds(2));
        }
    }
}

}  // namespace hsynth
