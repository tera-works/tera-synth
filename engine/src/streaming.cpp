#include "hsynth/streaming.h"

#include "hsynth/flac.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>

#include "hsynth/fileio.h"
#include "hsynth/sample_set.h"

namespace hsynth {

namespace {

std::mutex g_optMutex;
SampleLoadOptions g_opt;

int64_t tell64(FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return (int64_t)ftello(f);
#endif
}

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
        if (s.state.load() == StreamSlot::kReleased) s.state.store(StreamSlot::kFree);
    }
}

StreamSlot* DiskStreamer::claim(const Sample* smp, int64_t startFrame) {
    if (!slots_ || !running_.load(std::memory_order_relaxed)) return nullptr;
    for (int k = 0; k < kSlots; ++k) {
        const int i = (claimHint_ + k) % kSlots;
        StreamSlot& s = slots_[i];
        if (s.state.load(std::memory_order_acquire) != StreamSlot::kFree) continue;
        s.sample.store(smp, std::memory_order_relaxed);
        s.failed.store(false, std::memory_order_relaxed);
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
            const Sample* sample = s.sample.load(std::memory_order_relaxed);
            for (const auto& sp : set.samples)
                if (sp.get() == sample) { used = true; break; }
        }
        if (!used || !running_.load()) return true;
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(2)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void DiskStreamer::waitFor(const StreamSlot& s, int64_t frameEnd) const {
    while (running_.load(std::memory_order_relaxed) && !s.failed.load(std::memory_order_acquire) &&
           s.written.load(std::memory_order_acquire) < frameEnd)
        std::this_thread::yield();
}

// Only the disk thread uses this cache. Sharing decoded FLAC frames makes
// overlapping notes and retriggers pay for decoding each frame once while it
// is cached. Voices still consume their own lock-free PCM rings.
class StreamFileCache {
public:
    bool readWav(const StreamSource& stream, int64_t first, int64_t count, float* left, float* right,
                 std::vector<uint8_t>& scratch) {
        auto found = wavByPath_.find(stream.path);
        if (found == wavByPath_.end()) {
            if (wavFiles_.size() == kMaxWavFiles) {
                wavByPath_.erase(wavFiles_.back().path);
                wavFiles_.pop_back();
            }
            File file(fopenUtf8(stream.path, "rb"), &std::fclose);
            if (!file) return false;
            wavFiles_.push_front({stream.path, std::move(file)});
            found = wavByPath_.emplace(stream.path, wavFiles_.begin()).first;
        } else {
            wavFiles_.splice(wavFiles_.begin(), wavFiles_, found->second);
        }
        return readStreamFrames(found->second->file.get(), stream, first, count, left, right, scratch);
    }

    bool readFlac(const StreamSource& stream, int64_t first, int64_t count, float* left, float* right,
              std::atomic<uint64_t>& bytesRead) {
        Source* source = findSource(stream, bytesRead);
        if (!source) return false;
        int64_t copied = 0;
        while (copied < count) {
            const int64_t frame = first + copied;
            const auto next = source->index.upper_bound(frame);
            if (next == source->index.begin()) return false;
            auto point = std::prev(next);
            if (!point->second.cached) {
                if (decoderSource_ != source) {
                    decoderSource_ = nullptr;
                    if (!decoder_.open(source->bytes.data(), source->bytes.size(), error_)) return false;
                    decoderSource_ = source;
                }
                decoder_.seekFrame(point->second.byte, point->first);
                // Extend the frame index if a region starts far beyond the head.
                // Intermediate frames need no PCM cache entry.
                int frames;
                do {
                    frames = decoder_.next(decoded_[0], decoded_[1], error_);
                    if (frames <= 0) return false;
                    const int64_t end = decoder_.position();
                    auto following = source->index.try_emplace(end, decoder_.bytePosition()).first;
                    if (frame < end) break;
                    point = following;
                } while (true);

                const size_t bytes = (decoded_[0].size() + decoded_[1].size()) * sizeof(float);
                while (pcmBytes_ + bytes > kPcmBudget && !pcm_.empty()) evictFrame(std::prev(pcm_.end()));
                pcm_.push_front({&point->second, {decoded_[0], decoded_[1]}, bytes});
                point->second.cached = pcm_.begin();
                pcmBytes_ += bytes;
            }
            auto it = *point->second.cached;
            pcm_.splice(pcm_.begin(), pcm_, it);
            const size_t offset = (size_t)(frame - point->first);
            const size_t n = std::min((size_t)(count - copied), it->data[0].size() - offset);
            std::memcpy(left + copied, it->data[0].data() + offset, n * sizeof(float));
            if (right) {
                if (it->data[1].empty()) std::fill_n(right + copied, n, 0.0f);
                else std::memcpy(right + copied, it->data[1].data() + offset, n * sizeof(float));
            }
            copied += (int64_t)n;
        }
        return true;
    }

private:
    static constexpr size_t kCompressedBudget = 64u << 20;
    static constexpr size_t kPcmBudget = 48u << 20;
    static constexpr size_t kMaxSources = 256;
    static constexpr size_t kMaxWavFiles = 64;

    using File = std::unique_ptr<FILE, int (*)(FILE*)>;
    struct WavFile {
        std::string path;
        File file;
    };
    std::list<WavFile> wavFiles_;
    std::unordered_map<std::string, std::list<WavFile>::iterator> wavByPath_;

    struct Checkpoint;
    struct Frame {
        Checkpoint* point;
        std::vector<float> data[2];
        size_t bytes;
    };
    struct Checkpoint {
        explicit Checkpoint(size_t offset) : byte(offset) {}
        size_t byte;
        std::optional<std::list<Frame>::iterator> cached;
    };
    struct Source {
        uint64_t id;
        std::vector<uint8_t> bytes;
        std::map<int64_t, Checkpoint> index;
    };

    Source* findSource(const StreamSource& stream, std::atomic<uint64_t>& bytesRead) {
        const auto found = byId_.find(stream.flacId);
        if (found != byId_.end()) {
            sources_.splice(sources_.begin(), sources_, found->second);
            return &*found->second;
        }
        std::unique_ptr<FILE, int (*)(FILE*)> f(fopenUtf8(stream.path, "rb"), &std::fclose);
        if (!f || std::fseek(f.get(), 0, SEEK_END) != 0) return nullptr;
        const int64_t size = tell64(f.get());
        if (size <= 0 || seek64(f.get(), 0) != 0) return nullptr;
        while (!sources_.empty() && (compressedBytes_ + (size_t)size > kCompressedBudget ||
                                     sources_.size() >= kMaxSources)) {
            Source& last = sources_.back();
            if (&last == decoderSource_) decoderSource_ = nullptr;
            for (auto& point : last.index)
                if (point.second.cached) evictFrame(*point.second.cached);
            byId_.erase(last.id);
            compressedBytes_ -= last.bytes.size();
            sources_.pop_back();
        }
        std::vector<uint8_t> data((size_t)size);
        if (std::fread(data.data(), 1, data.size(), f.get()) != data.size()) return nullptr;
        sources_.push_front({stream.flacId, std::move(data), {}});
        Source& source = sources_.front();
        source.index.try_emplace(stream.flacStartFrame, stream.flacStartByte);
        byId_.emplace(stream.flacId, sources_.begin());
        compressedBytes_ += source.bytes.size();
        bytesRead.fetch_add((uint64_t)size, std::memory_order_relaxed);
        return &source;
    }

    void evictFrame(std::list<Frame>::iterator it) {
        it->point->cached.reset();
        pcmBytes_ -= it->bytes;
        pcm_.erase(it);
    }

    size_t compressedBytes_ = 0, pcmBytes_ = 0;
    std::list<Source> sources_;
    std::unordered_map<uint64_t, std::list<Source>::iterator> byId_;
    std::list<Frame> pcm_;
    Source* decoderSource_ = nullptr;
    FlacFrameDecoder decoder_;
    std::string error_;
    std::vector<float> decoded_[2];
};

namespace {
constexpr int64_t kRefillFrames = 2048;
}

bool DiskStreamer::fill(StreamSlot& s, StreamFileCache& cache) {
    const Sample* smp = s.sample.load(std::memory_order_relaxed);
    const int64_t written = s.written.load(std::memory_order_relaxed);
    if (written >= smp->frames || s.failed.load(std::memory_order_relaxed)) return false;
    const int64_t low = std::max(s.start, s.low.load(std::memory_order_acquire));
    const int64_t w = std::max(written, low);
    const int64_t n = std::min(kRefillFrames, smp->frames - w);
    // Refill in batches, including the last short chunk at EOF. A voice that
    // ran dry can skip past frames it no longer reads.
    if (n <= 0 || w + n > low + StreamSlot::kRing) return false;

    const bool stereo = smp->channels == 2;
    for (auto& t : tmp_) if ((int64_t)t.size() < n) t.resize((size_t)n);
    if (smp->stream.flac) {
        if (!cache.readFlac(smp->stream, w, n, tmp_[0].data(), stereo ? tmp_[1].data() : nullptr, bytesRead_)) {
            s.failed.store(true, std::memory_order_release);
            return false;
        }
    } else {
        if (!cache.readWav(smp->stream, w, n, tmp_[0].data(), stereo ? tmp_[1].data() : nullptr, scratch_)) {
            s.failed.store(true, std::memory_order_release);
            return false;
        }
        bytesRead_.fetch_add((uint64_t)(n * smp->stream.blockAlign), std::memory_order_relaxed);
    }
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
    StreamFileCache cache;
    while (!quit_.load()) {
        bool did = false;
        // the slot that is closest to running dry goes first
        for (int pass = 0; pass < 2; ++pass) {
            for (int i = 0; i < kSlots; ++i) {
                StreamSlot& s = slots_[i];
                const int st = s.state.load(std::memory_order_acquire);
                if (st == StreamSlot::kReleased) {
                    s.sample.store(nullptr, std::memory_order_relaxed);
                    s.state.store(StreamSlot::kFree, std::memory_order_release);
                    continue;
                }
                if (st != StreamSlot::kActive) continue;
                const int64_t lead = s.written.load(std::memory_order_relaxed) - s.low.load(std::memory_order_relaxed);
                if (pass == 0 && lead >= 2 * kRefillFrames) continue;
                if (fill(s, cache)) did = true;
            }
        }
        if (!did) {
            std::unique_lock<std::mutex> lock(wakeMutex_);
            if (!pending_.exchange(false)) {
                const int ms = active_.load(std::memory_order_relaxed) > 0 ? 1 : 4;
                wake_.wait_for(lock, std::chrono::milliseconds(ms));
            }
        }
    }
}

}  // namespace hsynth
