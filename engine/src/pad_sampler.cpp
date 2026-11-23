#include "hsynth/pad_sampler.h"
#include "hsynth/spsc_queue.h"
#include "pcm_file.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace hsynth {
namespace {
constexpr size_t kResidentBytes = 32u << 20;
constexpr float kHalfPi = 1.5707963267948966f;

bool validPad(int i) { return i >= 0 && i < PadSampler::kPads; }

struct PadClip {
    std::vector<float> left, right;
    int rate = 0;
    std::atomic<int> voices{0};
    size_t bytes() const { return (left.capacity() + right.capacity()) * sizeof(float); }
};

struct PadVoice {
    PadClip* clip = nullptr;
    double pos = 0, step = 0;
    int first = 0, last = 0, splice = 0;
    int age = 0, tail = 0, pad = 0, mode = 0, choke = 0;
    bool midi = false;
    float gain = 0;
};

PcmFrame read(const PadVoice& v, double frame) {
    const auto first = static_cast<int>(frame);
    const auto second = std::min(first + 1, v.last - 1);
    const float x = static_cast<float>(frame - first);
    const auto& l = v.clip->left;
    const auto& r = v.clip->right;
    return {l[first] + x * (l[second] - l[first]), r[first] + x * (r[second] - r[first])};
}
}  // namespace

struct PadSampler::Impl {
    struct Settings {
        float level = 1, pitch = 1, first = 0, last = 1;
        int mode = 0, choke = 0, velocity = 1;
    };
    struct Slot {
        // A writer might interrupt the callback's read. The callback falls
        // back to its last complete snapshot and never spins on the writer.
        std::atomic<uint32_t> version{0};
        std::atomic<float> level{1}, pitch{1}, first{0}, last{1};
        std::atomic<int> mode{0}, choke{0}, velocity{1};
        std::atomic<PadClip*> clip{nullptr};
        std::atomic<float> glow{0};
        Settings cached;
        bool midiHeld = false;

        Settings settings() {
            const auto v = version.load();
            if (v & 1) return cached;
            Settings next{level.load(), pitch.load(), first.load(), last.load(),
                          mode.load(), choke.load(), velocity.load()};
            if (v == version.load()) cached = next;
            return cached;
        }
    };
    enum Action { Hit, Up, Stop };
    struct Command { Action action; int pad; float velocity; int offset = 0; bool midi = false; };
    Slot slots[kPads];
    PadVoice voices[2 * kPads];
    struct MidiHit { Command command{}; PadClip* clip = nullptr; };
    MidiHit midiEvents[256];
    int midiCount = 0;
    std::atomic<bool> readingSlot{false};
    std::atomic<uint32_t> overflowReleases{0};
    uint32_t overflowMidi = 0;
    std::atomic<bool> work{false};
    SpscQueue<Command> events{256};
    mutable std::mutex host;
    std::vector<std::unique_ptr<PadClip>> clips;
    size_t resident = 0;
    float sr = 48000;

    void collect() {
        if (readingSlot.load()) return;
        clips.erase(std::remove_if(clips.begin(), clips.end(), [&](const auto& clip) {
            if (clip->voices.load() != 0) return false;
            for (const auto& s : slots)
                if (s.clip.load() == clip.get()) return false;
            resident -= clip->bytes();
            return true;
        }), clips.end());
    }
    void enqueue(Command e) {
        std::lock_guard<std::mutex> lock(host);
        if (e.action == Hit && events.size() >= 224) return;
        if (!events.push(e)) {
            if (e.action == Up) overflowReleases.fetch_or(1u << e.pad);
            if (e.action == Stop) overflowReleases.fetch_or(1u << kPads);
        }
        work.store(true, std::memory_order_release);
    }
    PadClip* pin(int pad) {
        readingSlot.store(true);
        auto* clip = slots[pad].clip.load();
        if (clip) clip->voices.fetch_add(1);
        readingSlot.store(false);
        return clip;
    }
    static void unpin(PadClip* clip) {
        if (clip) clip->voices.fetch_sub(1);
    }
    static void finish(PadVoice& voice) {
        unpin(voice.clip);
        voice = {};
    }
    int fadeLength() const { return std::max(1, int(sr * 0.004f)); }

    void apply(Command e, PadClip* pinned = nullptr) {
        if (e.action == Stop) {
            for (auto& v : voices) if (v.clip && v.tail == 0) v.tail = fadeLength();
            return;
        }
        auto& own = voices[e.pad];
        if (e.action == Up) {
            if (own.clip && own.mode == 2 && own.midi == e.midi && own.tail == 0) own.tail = fadeLength();
            return;
        }
        auto* clip = pinned ? pinned : pin(e.pad);
        if (!clip) return;
        const Settings s = slots[e.pad].settings();
        if (own.clip) {
            auto* free = std::find_if(voices + kPads, voices + 2*kPads,
                                      [](const auto& v) { return v.clip == nullptr; });
            if (free == voices + 2*kPads) { unpin(clip); return; }
            *free = own;
            if (free->tail == 0) free->tail = fadeLength();
            own = {};
        }
        if (s.choke != 0) {
            for (auto& v : voices)
                if (v.clip && v.choke == s.choke && v.tail == 0) v.tail = fadeLength();
        }
        const int count = static_cast<int>(clip->left.size());
        const int first = std::min(count - 1, int(count * s.first));
        const int last = std::clamp(int(count * s.last), first + 1, count);
        const double step = s.pitch * clip->rate / sr;
        const int splice = std::min((last - first) / 2, int(0.010 * sr * step));
        float vel = std::clamp(e.velocity, 0.0f, 1.0f);
        if (s.velocity == 0) vel = 1;
        if (s.velocity == 2) vel *= vel;
        own = {clip, double(first), step, first, last, splice, 0, 0,
               e.pad, s.mode, s.choke, e.midi, s.level * vel};
    }

    void render(float* left, float* right, int n, float* glow) {
        const int onset = std::max(1, int(sr * 0.0005f));
        const int fade = fadeLength();
        for (auto& v : voices) {
            if (!v.clip) continue;
            if (v.tail == 0 && slots[v.pad].clip.load(std::memory_order_relaxed) != v.clip) v.tail = fade;
            const bool loop = v.mode == 1;
            for (int i = 0; i < n; ++i) {
                auto x = read(v, v.pos);
                if (loop && v.splice > 0 && v.pos >= v.last - v.splice) {
                    const double off = v.pos - (v.last - v.splice);
                    const float angle = kHalfPi * float(off / v.splice);
                    const auto head = read(v, v.first + off);
                    const float a = std::cos(angle), b = std::sin(angle);
                    x = {a*x.l + b*head.l, a*x.r + b*head.r};
                }
                float level = v.gain * (float(v.age) / onset);
                v.age = std::min(v.age + 1, onset);
                if (v.tail) level *= float(v.tail) / fade;
                if (!loop) level *= std::min(1.0, (v.last - v.pos) / (onset * v.step));
                left[i] += x.l * level;
                right[i] += x.r * level;
                glow[v.pad] = std::max(glow[v.pad], level * std::max(std::fabs(x.l), std::fabs(x.r)));
                v.pos += v.step;
                if (v.tail && --v.tail == 0) { finish(v); break; }
                if (v.pos < v.last) continue;
                if (!loop) { finish(v); break; }
                v.pos = v.first + v.splice + std::fmod(v.pos - v.last, v.last - v.first - v.splice);
            }
        }
    }
};

PadSampler::PadSampler() : impl_(std::make_unique<Impl>()) {}
PadSampler::~PadSampler() = default;

Json PadSampler::load(int pad, const std::string& path) {
    auto failure = [](const std::string& error) {
        Json r = Json::object();
        r.set("ok", false); r.set("error", error);
        return r;
    };
    if (!validPad(pad)) return failure("Pad must be 0 through 15");
    PcmFile file;
    std::string error;
    if (!file.open(path, error)) return failure(error);
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.host);
    s.collect();
    const auto& info = file.info();
    if (info.frames > 30ll * info.sr) return failure("Pad audio exceeds 30 seconds");
    const auto bytes = size_t(info.frames) * 2 * sizeof(float);
    auto* replacing = s.slots[pad].clip.load();
    if (bytes > kResidentBytes || bytes + s.resident > kResidentBytes)
        return failure("Pad memory limit reached (32 MiB)");
    auto clip = std::make_unique<PadClip>();
    clip->rate = info.sr;
    clip->left.resize(size_t(info.frames));
    clip->right.resize(size_t(info.frames));
    for (int64_t i = 0; i < info.frames; i += 4096) {
        const int count = int(std::min<int64_t>(4096, info.frames - i));
        if (file.read(i, count, clip->left.data() + i, clip->right.data() + i) != count)
            return failure("Could not decode pad audio");
    }
    for (auto* channel : {&clip->left, &clip->right})
        for (float& x : *channel) x = std::isfinite(x) ? std::clamp(x, -8.0f, 8.0f) : 0.0f;
    auto* value = clip.get();
    s.clips.push_back(std::move(clip));
    s.resident += value->bytes();
    s.slots[pad].clip.store(value);
    if (replacing) s.collect();
    Json r = Json::object();
    r.set("ok", true);
    r.set("frames", double(info.frames));
    r.set("sr", info.sr);
    r.set("dur", info.duration());
    return r;
}

void PadSampler::clear(int pad) {
    if (!validPad(pad)) return;
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.host);
    s.slots[pad].clip.store(nullptr);
    s.collect();
}
void PadSampler::setup(int pad, float level, float pitch, int mode, int choke, int vel, float first, float last) {
    if (!validPad(pad) || !std::isfinite(level) || !std::isfinite(pitch) ||
        !std::isfinite(first) || !std::isfinite(last)) return;
    std::lock_guard<std::mutex> lock(impl_->host);
    auto& s = impl_->slots[pad];
    s.version.fetch_add(1);
    s.level.store(std::clamp(level, 0.0f, 1.5f));
    s.pitch.store(std::exp2(std::clamp(pitch, -12.0f, 12.0f) / 12.0f));
    s.mode.store(std::clamp(mode, 0, 2));
    s.choke.store(std::clamp(choke, 0, 8));
    s.velocity.store(std::clamp(vel, 0, 2));
    s.first.store(std::clamp(first, 0.0f, 1.0f));
    s.last.store(std::clamp(last, 0.0f, 1.0f));
    s.version.fetch_add(1);
}
void PadSampler::trigger(int pad, float vel) {
    if (validPad(pad) && std::isfinite(vel) && vel > 0) impl_->enqueue({Impl::Hit, pad, vel});
}
void PadSampler::release(int pad) {
    if (validPad(pad)) impl_->enqueue({Impl::Up, pad, 0});
}
void PadSampler::stopAll() { impl_->enqueue({Impl::Stop, 0, 0}); }

std::vector<float> PadSampler::peaks(int pad, int bins) const {
    std::lock_guard<std::mutex> lock(impl_->host);
    const auto* clip = validPad(pad) ? impl_->slots[pad].clip.load() : nullptr;
    return clip ? pcmPeaks(clip->left.data(), clip->right.data(), clip->left.size(), bins)
                : std::vector<float>(size_t(clampPeakBins(bins)) * 2);
}
std::array<float, PadSampler::kPads> PadSampler::levels() const {
    std::array<float, kPads> out{};
    for (int i = 0; i < kPads; ++i) out[i] = impl_->slots[i].glow.load(std::memory_order_relaxed);
    return out;
}

void PadSampler::prepare(float sampleRate) {
    auto& s = *impl_;
    for (auto& v : s.voices) s.finish(v);
    for (int i = 0; i < s.midiCount; ++i) s.unpin(s.midiEvents[i].clip);
    s.midiCount = 0;
    s.overflowMidi = 0;
    for (auto& slot : s.slots) {
        slot.glow.store(0);
        slot.midiHeld = false;
    }
    s.work.store(s.events.size() != 0 || s.overflowReleases.load() != 0);
    s.sr = sampleRate;
}

void PadSampler::panic() {
    auto& s = *impl_;
    if (!active()) return;
    for (int i = 0; i < s.midiCount; ++i) s.unpin(s.midiEvents[i].clip);
    s.midiCount = 0;
    s.overflowMidi = 0;
    for (auto& slot : s.slots) slot.midiHeld = false;
    s.overflowReleases.fetch_or(1u << kPads);
    s.work.store(true, std::memory_order_release);
}

bool PadSampler::midi(int pad, bool on, float velocity, int offset) {
    if (!validPad(pad)) return false;
    auto& s = *impl_;
    auto& slot = s.slots[pad];
    auto* clip = on ? s.pin(pad) : nullptr;
    if ((on && !clip) || (!on && !slot.midiHeld)) return false;
    slot.midiHeld = on;
    if (s.midiCount < 256) s.midiEvents[s.midiCount++] = {{on ? Impl::Hit : Impl::Up, pad, velocity, offset, true}, clip};
    else {
        s.unpin(clip);
        if (!on) s.overflowMidi |= 1u << pad;
    }
    s.work.store(true, std::memory_order_release);
    return true;
}

bool PadSampler::active() const {
    const auto& s = *impl_;
    return s.work.load(std::memory_order_acquire);
}

void PadSampler::mix(float* left, float* right, int frames) {
    auto& s = *impl_;
    s.work.exchange(false, std::memory_order_acq_rel);
    Impl::Command cmd;
    for (int i = 0; i < 256 && s.events.pop(cmd); ++i) s.apply(cmd);
    const auto overflow = s.overflowReleases.exchange(0);
    if (overflow & (1u << kPads)) s.apply({Impl::Stop, 0, 0});
    for (int i = 0; i < kPads; ++i)
        if (overflow & (1u << i)) s.apply({Impl::Up, i, 0});

    int offset = 0;
    float glow[kPads]{};
    for (int i = 0; i < s.midiCount; ++i) {
        const auto& hit = s.midiEvents[i];
        const int next = std::clamp(hit.command.offset, offset, frames);
        s.render(left + offset, right + offset, next - offset, glow);
        s.apply(hit.command, hit.clip);
        offset = next;
    }
    s.midiCount = 0;
    for (int i = 0; i < kPads; ++i)
        if (s.overflowMidi & (1u << i)) s.apply({Impl::Up, i, 0, 0, true});
    s.overflowMidi = 0;
    s.render(left + offset, right + offset, frames - offset, glow);
    for (int i = 0; i < kPads; ++i) s.slots[i].glow.store(glow[i], std::memory_order_relaxed);
    if (s.events.size() != 0 ||
        std::any_of(std::begin(s.voices), std::end(s.voices), [](const auto& v) { return v.clip != nullptr; }) ||
        std::any_of(std::begin(glow), std::end(glow), [](float x) { return x > 0; }))
        s.work.store(true, std::memory_order_release);
}

}  // namespace hsynth
