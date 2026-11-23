#include "hsynth/backing_player.h"

#include "pcm_file.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <thread>

namespace hsynth {
namespace {
constexpr double kHalfPi = 1.5707963267948966;

constexpr int kStems = 4;
using SourceFiles = std::array<std::unique_ptr<PcmFile>, kStems>;
using SourcePaths = std::array<std::string, kStems>;

struct BackingFrame {
    std::array<PcmFrame, kStems> pcm{};
    double seconds = 0;
};

constexpr uint32_t kMute = 1u << 31, kSolo = 1u << 30, kUnity = 0x3f800000;
static_assert(sizeof(float) == sizeof(uint32_t) && std::atomic<uint32_t>::is_always_lock_free);

float stemGain(uint32_t mix) {
    mix &= ~(kMute | kSolo);
    float gain;
    std::memcpy(&gain, &mix, sizeof(gain));
    return gain;
}

class StereoPcmRing {
public:
    static constexpr uint32_t kCapacity = 16384, kStemCapacity = 8192;
    bool stems = false;
    enum State { Free, Filling, Ready, Reading };
    std::atomic<int> state{Free};
    uint64_t generation = 0;
    std::atomic<bool> ended{false};

    void reset(uint64_t epoch, bool useStems) {
        stems = useStems;
        if (stems && !extra_)
            extra_ = std::make_unique<std::array<PcmFrame, kStemCapacity>[]>(kStems - 1);
        generation = epoch;
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
        ended.store(false, std::memory_order_relaxed);
    }
    uint32_t size() const {
        const auto read = read_.load(std::memory_order_acquire);
        return write_.load(std::memory_order_acquire) - read;
    }
    void push(const BackingFrame& value) {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto i = write % (stems ? kStemCapacity : kCapacity);
        data_[i] = {value.pcm[0], value.seconds};
        if (stems)
            for (int stem = 1; stem < kStems; ++stem) extra_[stem - 1][i] = value.pcm[stem];
        write_.store(write + 1, std::memory_order_release);
    }
    bool pop(BackingFrame& value) {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        const auto i = read % (stems ? kStemCapacity : kCapacity);
        value.pcm[0] = data_[i].pcm;
        value.seconds = data_[i].seconds;
        if (stems)
            for (int stem = 1; stem < kStems; ++stem) value.pcm[stem] = extra_[stem - 1][i];
        read_.store(read + 1, std::memory_order_release);
        return true;
    }

private:
    struct TimedPcm { PcmFrame pcm; double seconds; };
    std::array<TimedPcm, kCapacity> data_;
    std::unique_ptr<std::array<PcmFrame, kStemCapacity>[]> extra_;
    alignas(64) std::atomic<uint32_t> read_{0};
    alignas(64) std::atomic<uint32_t> write_{0};
};

struct Cache {
    static constexpr int kFrames = 4096;
    int64_t first = -1;
    int count = 0;
    float left[kFrames], right[kFrames];

    PcmFrame read(PcmFile& f, int64_t frame) {
        if (first < 0 || frame < first || frame >= first + kFrames) {
            first = frame / kFrames * kFrames;
            count = f.read(first, kFrames, left, right);
        }
        const auto offset = frame - first;
        if (offset >= count) return {};
        const auto sane = [](float v) { return std::isfinite(v) ? std::clamp(v, -8.0f, 8.0f) : 0.0f; };
        return {sane(left[offset]), sane(right[offset])};
    }
};

PcmFrame blend(PcmFrame a, PcmFrame b, float ga, float gb) {
    return {a.l * ga + b.l * gb, a.r * ga + b.r * gb};
}

}  // namespace

struct BackingPlayer::Impl {
    struct Transport {
        uint64_t generation = 0;
        double start = 0, a = 0, b = 0;
        double speed = 1, pitch = 1, rate = 48000;
        bool loop = false;
    };

    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool quit = false;
    Transport config;
    SourceFiles pending;
    SourcePaths paths;
    std::atomic<bool> stemsLoaded{false};
    std::array<std::atomic<uint32_t>, kStems> stemMix{{kUnity, kUnity, kUnity, kUnity}};
    std::atomic<double> duration{0};
    std::unique_ptr<StereoPcmRing[]> banks;
    std::atomic<uint64_t> requested{0}, applied{0}, firstFileGeneration{0};
    std::atomic<double> position{0};
    std::atomic<bool> playing{false}, audible{false}, direct{true};
    std::atomic<float> volume{1}, sampleRate{48000};

    // Audio-thread-only cursor and transition state.
    int current = -1, previous = -1, fade = 0;
    float gain = 0;
    std::array<float, kStems> stemLevel{1, 1, 1, 1}, stemTarget{1, 1, 1, 1}, stemDelta{};
    std::array<int, kStems> stemRemaining{};
    int signalFrames = 0;
    PcmFrame last{}, oldLast{};

    ~Impl() {
        { std::lock_guard<std::mutex> lock(mutex); quit = true; }
        wake.notify_one();
        if (worker.joinable()) worker.join();
    }

    double transportPosition() const {
        return applied.load(std::memory_order_acquire) == config.generation ? position.load() : config.start;
    }

    void revise(double start) {
        config.start = std::clamp(start, 0.0, duration.load());
        requested.store(++config.generation, std::memory_order_release);
        wake.notify_one();
    }

    Json install(SourceFiles files, SourcePaths sourcePaths) {
        const auto info = files[0]->info();
        const bool useStems = files[1] != nullptr;
        std::lock_guard<std::mutex> lock(mutex);
        playing.store(false);
        position.store(0);
        paths = std::move(sourcePaths);
        duration = info.duration();
        config.a = 0;
        config.b = duration.load();
        config.loop = false;
        pending = std::move(files);
        stemsLoaded.store(useStems, std::memory_order_release);
        firstFileGeneration.store(config.generation + 1, std::memory_order_release);
        if (!worker.joinable()) {
            banks = std::make_unique<StereoPcmRing[]>(2);
            worker = std::thread([this] { run(); });
        }
        revise(0);
        Json reply = Json::object();
        reply.set("ok", true);
        reply.set("name", useStems ? "4 stems" : info.name);
        reply.set("dur", duration.load());
        reply.set("sr", info.sr);
        reply.set("ch", info.ch);
        return reply;
    }

    void run() {
        SourceFiles files;
        auto cache = std::make_unique<std::array<Cache, 4>[]>(kStems);
        int sourceCount = 0;
        Transport t;
        int bank = -1;
        int64_t outputFrame = 0;
        double source = 0, a = 0, b = 0, splice = 0, step = 1, pitchStep = 1;
        double grainOffset[2] = {};
        bool buffered = false, ended = false;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                const auto changed = [&] { return quit || pending[0] || config.generation != t.generation; };
                if (ended) wake.wait(lock, changed);
                else if (buffered && !playing.load(std::memory_order_relaxed))
                    wake.wait(lock, [&] { return changed() || playing.load(std::memory_order_relaxed); });
                else
                    wake.wait_for(lock, std::chrono::milliseconds(2), [&] { return changed(); });
                if (quit) return;
                if (pending[0]) {
                    files = std::move(pending);
                    sourceCount = files[1] ? kStems : 1;
                    for (int stem = 0; stem < sourceCount; ++stem)
                        for (auto& c : cache[stem]) { c.first = -1; c.count = 0; }
                }
                if (!files[0]) continue;
                if (t.generation != config.generation) {
                    if (bank >= 0 && banks[bank].state.load() == StereoPcmRing::Filling)
                        banks[bank].state.store(StereoPcmRing::Free, std::memory_order_release);
                    bank = -1;
                    buffered = ended = false;
                    for (int i = 0; i < 2; ++i) {
                        // A ready bank can be reclaimed only if the callback
                        // has not claimed it for reading.
                        int ready = StereoPcmRing::Ready;
                        banks[i].state.compare_exchange_strong(ready, StereoPcmRing::Free);
                        if (banks[i].state.load(std::memory_order_acquire) == StereoPcmRing::Free) bank = i;
                    }
                    if (bank < 0) continue;
                    t = config;
                    auto& ring = banks[bank];
                    ring.reset(t.generation, sourceCount == kStems);
                    ring.state.store(StereoPcmRing::Filling, std::memory_order_release);
                    const auto sr = files[0]->info().sr;
                    a = t.a * sr;
                    b = t.loop ? t.b * sr : double(files[0]->info().frames);
                    splice = t.loop ? std::min(0.010 * sr * t.speed, (b - a) * 0.5) : 0;
                    source = t.start * sr;
                    if (t.loop && (source < a || source >= b)) source = a;
                    step = sr / t.rate * t.speed;
                    pitchStep = sr / t.rate * t.pitch;
                    outputFrame = 0;
                    grainOffset[0] = grainOffset[1] = 0;
                    buffered = ended = false;
                }
            }
            if (bank < 0) continue;
            auto& ring = banks[bank];
            if (ring.state.load(std::memory_order_acquire) == StereoPcmRing::Free) continue;
            const auto linear = [](PcmFile& file, double pos, Cache& c) {
                pos = std::clamp(pos, 0.0, double(file.info().frames - 1));
                const auto first = static_cast<int64_t>(pos);
                const float fraction = static_cast<float>(pos - first);
                const auto x = c.read(file, first);
                if (fraction == 0 || first + 1 == file.info().frames) return x;
                return blend(x, c.read(file, first + 1), 1 - fraction, fraction);
            };
            const auto sample = [&](int stem, double pos, int lane) {
                if (t.loop) {
                    const double size = b - a - splice;
                    if (pos >= b) pos = a + splice + std::fmod(pos - b, size);
                    if (pos < a) pos = a;
                    if (pos >= b - splice) {
                        const auto angle = kHalfPi * (pos - (b - splice)) / splice;
                        return blend(linear(*files[stem], pos, cache[stem][2*lane]),
                                     linear(*files[stem], a + pos - (b - splice), cache[stem][2*lane+1]),
                                     float(std::cos(angle)), float(std::sin(angle)));
                    }
                }
                return linear(*files[stem], pos, cache[stem][2*lane]);
            };
            const auto referenceMix = [&](double pos, int lane) {
                auto sum = sample(0, pos, lane);
                for (int stem = 1; stem < sourceCount; ++stem) {
                    const auto pcm = sample(stem, pos, lane);
                    sum.l += pcm.l;
                    sum.r += pcm.r;
                }
                return sum;
            };
            const int grain = std::max(2, int(t.rate * 0.030) & ~1);
            const int hop = grain / 2;
            const int searchRadius = int(files[0]->info().sr * 0.004);
            const double difference = pitchStep - step;
            const auto alignGrain = [&](int lane) {
                // Align the two grain waveforms through the overlap. The search
                // adjusts PCM reads only; source still advances at the speed ratio.
                constexpr int kProbeCount = 32;
                const double stride = pitchStep * hop / (2 * kProbeCount);
                const double outgoing = source + grainOffset[1 - lane];
                const double incoming = source - hop * difference;
                PcmFrame reference[kProbeCount];
                for (int j = 0; j < kProbeCount; ++j)
                    reference[j] = referenceMix(outgoing + j * stride, 1 - lane);

                double bestScore = 0;
                int bestOffset = 0;
                const auto consider = [&](int offset) {
                    double cross = 0, energy = 0;
                    for (int j = 0; j < kProbeCount; ++j) {
                        const auto x = referenceMix(incoming + offset + j * stride, lane);
                        cross += reference[j].l * x.l + reference[j].r * x.r;
                        energy += x.l * x.l + x.r * x.r;
                    }
                    const double score = cross / std::sqrt(energy + 1e-12);
                    if (score > bestScore || (score == bestScore && std::abs(offset) < std::abs(bestOffset))) {
                        bestScore = score;
                        bestOffset = offset;
                    }
                };
                consider(0);
                constexpr int kCoarseStep = 8;
                for (int offset = kCoarseStep; offset <= searchRadius; offset += kCoarseStep) {
                    consider(-offset);
                    consider(offset);
                }
                const int coarse = bestOffset;
                for (int offset = std::max(-searchRadius, coarse - kCoarseStep + 1);
                     offset <= std::min(searchRadius, coarse + kCoarseStep - 1); ++offset)
                    consider(offset);
                grainOffset[lane] = bestOffset;
            };
            const int highWater = ring.stems ? 7800 : 12000;
            const int fill = std::min(2048, std::max(0, highWater - int(ring.size())));
            for (int i = 0; i < fill; ++i) {
                if (source >= b) { ring.ended.store(true, std::memory_order_release); break; }
                BackingFrame frame;
                frame.seconds = source / files[0]->info().sr;
                if (pitchStep == step) {
                    for (int stem = 0; stem < sourceCount; ++stem) frame.pcm[stem] = sample(stem, source, 0);
                } else {
                    const int phase = int(outputFrame % grain);
                    const int other = (phase + hop) % grain;
                    if (phase == 0) alignGrain(0);
                    if (other == 0) alignGrain(1);
                    const float w = float(0.5 - 0.5 * std::cos(4 * kHalfPi * phase / grain));
                    for (int stem = 0; stem < sourceCount; ++stem)
                        frame.pcm[stem] = blend(sample(stem, source + (phase - hop) * difference + grainOffset[0], 0),
                                               sample(stem, source + (other - hop) * difference + grainOffset[1], 1),
                                               w, 1 - w);
                }
                if (!t.loop) {
                    const float endGain = float(std::min(1.0, (b - source) / (step * t.rate * 0.004)));
                    for (int stem = 0; stem < sourceCount; ++stem) {
                        frame.pcm[stem].l *= endGain;
                        frame.pcm[stem].r *= endGain;
                    }
                }
                ring.push(frame);
                ++outputFrame;
                source += step;
                if (t.loop && source >= b) source = a + splice + std::fmod(source - b, b - a - splice);
            }
            if (ring.state.load(std::memory_order_acquire) == StereoPcmRing::Filling &&
                (ring.size() >= 2048 || ring.ended.load()))
                ring.state.store(StereoPcmRing::Ready, std::memory_order_release);
            buffered = ring.size() >= (ring.stems ? 6500u : 10000u);
            ended = ring.ended.load(std::memory_order_acquire);
        }
    }
};

BackingPlayer::BackingPlayer() : impl_(std::make_unique<Impl>()) {}
BackingPlayer::~BackingPlayer() = default;

Json BackingPlayer::load(const std::string& path) {
    auto file = std::make_unique<PcmFile>();
    std::string error;
    Json reply = Json::object();
    if (!file->open(path, error)) {
        reply.set("ok", false);
        reply.set("error", error);
        return reply;
    }
    float l, r;
    if (file->read(0, 1, &l, &r) != 1) {
        reply.set("ok", false);
        reply.set("error", "Could not decode audio");
        return reply;
    }
    SourceFiles files;
    files[0] = std::move(file);
    return impl_->install(std::move(files), {path});
}

Json BackingPlayer::loadStems(const std::string paths[4]) {
    SourceFiles files;
    SourcePaths sourcePaths;
    const auto failure = [](const std::string& error) {
        Json reply = Json::object();
        reply.set("ok", false);
        reply.set("error", error);
        return reply;
    };
    for (int i = 0; i < kStems; ++i) {
        auto file = std::make_unique<PcmFile>();
        std::string error;
        if (!file->open(paths[i], error)) return failure(error);
        const auto& info = file->info();
        const auto& first = i == 0 ? info : files[0]->info();
        if (info.ch != 2 || info.sr != first.sr || info.frames != first.frames)
            return failure("Stem files must be stereo with the same sample rate and frame count");
        float l, r;
        if (file->read(0, 1, &l, &r) != 1) return failure("Could not decode stem: " + paths[i]);
        sourcePaths[i] = paths[i];
        files[i] = std::move(file);
    }
    return impl_->install(std::move(files), std::move(sourcePaths));
}

void BackingPlayer::setStemMix(int stem, float gain, bool mute, bool solo) {
    if (stem < 0 || stem >= kStems || !std::isfinite(gain)) return;
    gain = std::clamp(gain, 0.0f, 1.5f) + 0.0f; // Canonicalize negative zero before packing flags.
    uint32_t mix;
    std::memcpy(&mix, &gain, sizeof(mix));
    if (mute) mix |= kMute;
    if (solo) mix |= kSolo;
    impl_->stemMix[stem].store(mix, std::memory_order_relaxed);
}

bool BackingPlayer::isStemsLoaded() const {
    return impl_->stemsLoaded.load(std::memory_order_acquire);
}

Json BackingPlayer::stemStateJson() const {
    Json reply = Json::object(), stems = Json::array();
    reply.set("active", isStemsLoaded());
    for (const auto& mix : impl_->stemMix) {
        const auto value = mix.load(std::memory_order_relaxed);
        Json stem = Json::object();
        stem.set("gain", stemGain(value));
        stem.set("mute", (value & kMute) != 0);
        stem.set("solo", (value & kSolo) != 0);
        stems.push(std::move(stem));
    }
    reply.set("stems", std::move(stems));
    return reply;
}

void BackingPlayer::play(bool on) {
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.duration == 0) return;
    if (on && !s.playing.load() && s.position.load() >= s.duration) s.revise(0);
    s.playing.store(on, std::memory_order_release);
    if (on) s.audible.store(true, std::memory_order_release);
    s.wake.notify_one();
}

void BackingPlayer::seek(double sec) {
    if (!std::isfinite(sec)) return;
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.revise(sec);
    if (!s.playing.load()) s.position.store(s.config.start);
}

void BackingPlayer::setLoop(double a, double b, bool on) {
    if (!std::isfinite(a) || !std::isfinite(b)) return;
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.config.a = std::clamp(a, 0.0, s.duration.load());
    s.config.b = std::clamp(b, s.config.a, s.duration.load());
    s.config.loop = on && s.config.b - s.config.a >= 0.001;
    s.revise(s.transportPosition());
}
void BackingPlayer::setVolume(float v) {
    if (std::isfinite(v)) impl_->volume.store(std::clamp(v, 0.0f, 1.5f));
}
void BackingPlayer::setSpeed(float v) {
    if (!std::isfinite(v)) return;
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    const double speed = std::clamp(v, 0.5f, 2.0f);
    if (s.config.speed == speed) return;
    s.config.speed = speed;
    s.revise(s.transportPosition());
}
void BackingPlayer::setPitch(float v) {
    if (!std::isfinite(v)) return;
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    const double pitch = std::exp2(std::clamp(v, -12.0f, 12.0f) / 12.0);
    if (s.config.pitch == pitch) return;
    s.config.pitch = pitch;
    s.revise(s.transportPosition());
}
void BackingPlayer::setRoute(bool direct) { impl_->direct.store(direct); }
bool BackingPlayer::direct() const { return impl_->direct.load(std::memory_order_relaxed); }

void BackingPlayer::prepare(float rate) {
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (rate == s.config.rate) return;
    s.config.rate = rate;
    s.sampleRate.store(rate);
    s.revise(s.transportPosition());
}
bool BackingPlayer::active() const { return impl_->audible.load(std::memory_order_acquire); }

std::array<double, 6> BackingPlayer::state() const {
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    return {s.position.load(), s.duration, s.playing.load() ? 1.0 : 0.0,
            s.config.a, s.config.b, s.config.loop ? 1.0 : 0.0};
}

std::vector<float> BackingPlayer::peaks(int bins) const {
    bins = clampPeakBins(bins);
    std::vector<float> out(size_t(bins) * 2, 0.0f);
    if (bins == 0) return out;
    SourcePaths paths;
    { std::lock_guard<std::mutex> lock(impl_->mutex); paths = impl_->paths; }
    const int count = paths[1].empty() ? 1 : kStems;
    SourceFiles files;
    auto cache = std::make_unique<Cache[]>(count);
    for (int stem = 0; stem < count; ++stem) {
        std::string error;
        files[stem] = std::make_unique<PcmFile>();
        if (paths[stem].empty() || !files[stem]->open(paths[stem], error)) return out;
    }
    const auto sample = [&](int64_t frame) {
        auto sum = cache[0].read(*files[0], frame);
        for (int stem = 1; stem < count; ++stem) {
            const auto pcm = cache[stem].read(*files[stem], frame);
            sum.l += pcm.l;
            sum.r += pcm.r;
        }
        return sum;
    };
    const auto frames = files[0]->info().frames;
    for (int bin = 0; bin < bins; ++bin) {
        const auto first = bin * frames / bins;
        const auto end = std::max(first + 1, (bin + 1) * frames / bins);
        auto x = sample(first);
        float lo = std::min(x.l, x.r), hi = std::max(x.l, x.r);
        for (auto j = first + 1; j < end; ++j) {
            x = sample(j);
            lo = std::min({lo, x.l, x.r});
            hi = std::max({hi, x.l, x.r});
        }
        out[2*bin] = lo;
        out[2*bin+1] = hi;
    }
    return out;
}

void BackingPlayer::mix(float* L, float* R, int frames) {
    auto& s = *impl_;
    const bool playing = s.playing.load(std::memory_order_acquire);
    const auto desired = s.requested.load(std::memory_order_acquire);
    const int transition = std::max(1, int(s.sampleRate.load(std::memory_order_relaxed) * 0.010f));
    const float step = 1.0f / transition;
    const auto release = [&](int& index) {
        if (index < 0) return;
        s.banks[index].state.store(StereoPcmRing::Free, std::memory_order_release);
        index = -1;
    };
    const auto firstGeneration = s.firstFileGeneration.load(std::memory_order_acquire);
    if (s.current >= 0 && s.banks[s.current].generation < firstGeneration) {
        release(s.current);
        release(s.previous);
    }
    if (playing && s.previous < 0) {
        for (int i = 0; i < 2; ++i) {
            int ready = StereoPcmRing::Ready;
            if (!s.banks[i].state.compare_exchange_strong(ready, StereoPcmRing::Reading)) continue;
            if (s.banks[i].generation != desired) {
                // The worker may have published a newer bank after we read requested.
                const auto state = s.banks[i].generation > desired ? StereoPcmRing::Ready : StereoPcmRing::Free;
                s.banks[i].state.store(state, std::memory_order_release);
                continue;
            }
            s.previous = s.current;
            s.oldLast = s.last;
            s.current = i;
            s.fade = s.previous >= 0 ? transition : 0;
            break;
        }
    }
    const float target = s.volume.load(std::memory_order_relaxed);
    const bool useStems = s.current >= 0 && s.banks[s.current].stems;
    if (useStems) {
        std::array<uint32_t, kStems> mix{};
        bool anySolo = false;
        for (int stem = 0; stem < kStems; ++stem) {
            mix[stem] = s.stemMix[stem].load(std::memory_order_relaxed);
            anySolo |= (mix[stem] & kSolo) != 0;
        }
        for (int stem = 0; stem < kStems; ++stem) {
            const auto value = mix[stem];
            const float next = (value & kMute) || (anySolo && !(value & kSolo)) ? 0.0f : stemGain(value);
            if (next == s.stemTarget[stem]) continue;
            s.stemTarget[stem] = next;
            s.stemRemaining[stem] = transition;
            s.stemDelta[stem] = (next - s.stemLevel[stem]) * step;
        }
    }
    const auto mixFrame = [&](const BackingFrame& frame) {
        if (!useStems) return frame.pcm[0];
        PcmFrame sum{};
        for (int stem = 0; stem < kStems; ++stem) {
            sum.l += frame.pcm[stem].l * s.stemLevel[stem];
            sum.r += frame.pcm[stem].r * s.stemLevel[stem];
        }
        return sum;
    };
    double position = -1;
    bool ended = false;
    for (int i = 0; i < frames; ++i) {
        BackingFrame frame, old;
        const bool got = playing && !ended && s.current >= 0 && s.banks[s.current].pop(frame);
        if (!got && playing && s.current >= 0) {
            auto& ring = s.banks[s.current];
            ended = ring.generation == desired && ring.ended.load(std::memory_order_acquire) && ring.size() == 0;
        }
        if (useStems) {
            for (int stem = 0; stem < kStems; ++stem) {
                if (s.stemRemaining[stem] == 0) continue;
                s.stemLevel[stem] = --s.stemRemaining[stem] ?
                    s.stemLevel[stem] + s.stemDelta[stem] : s.stemTarget[stem];
            }
        }
        if (got) {
            position = frame.seconds;
            auto pcm = mixFrame(frame);
            if (s.previous >= 0) {
                if (s.banks[s.previous].pop(old)) s.oldLast = mixFrame(old);
                const double angle = kHalfPi * (1 - double(s.fade) / transition);
                pcm = blend(s.oldLast, pcm, float(std::cos(angle)), float(std::sin(angle)));
            }
            s.last = pcm;
        }
        if (s.previous >= 0 && --s.fade <= 0) release(s.previous);
        s.signalFrames = std::clamp(s.signalFrames + (got ? 1 : -1), 0, transition);
        const float signalGain = s.signalFrames * step;
        s.gain += std::clamp(target - s.gain, -step, step);
        L[i] += s.last.l * signalGain * s.gain;
        R[i] += s.last.r * signalGain * s.gain;
    }
    if (playing && position >= 0 && s.playing.load(std::memory_order_relaxed)) {
        s.position.store(position, std::memory_order_relaxed);
        s.applied.store(s.banks[s.current].generation, std::memory_order_release);
    }
    if (ended && s.signalFrames == 0 && desired == s.requested.load(std::memory_order_acquire)) {
        s.position.store(s.duration.load(std::memory_order_relaxed));
        s.playing.store(false, std::memory_order_relaxed);
        release(s.current);
        release(s.previous);
    }
    if (!s.playing.load(std::memory_order_relaxed) && s.signalFrames == 0) {
        release(s.previous);
        s.audible.store(false, std::memory_order_release);
        // Recheck in case play() raced the end of this block.
        if (s.playing.load(std::memory_order_acquire)) s.audible.store(true, std::memory_order_release);
    }
}

}  // namespace hsynth
