#include "hsynth/live_sampler.h"
#include "hsynth/fileio.h"
#include "hsynth/host_utils.h"
#include "hsynth/tuner.h"
#include "hsynth/wav.h"
#include "pcm_file.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

namespace hsynth {
namespace {
constexpr float kHalfPi = 1.5707963267948966f;

struct Take {
    std::vector<float> left, right;
    int rate = 48000, source = 0;
    float threshold = 0;
    std::atomic<int> written{0};
    std::atomic<float> peak{0};
};

float sample(float x) { return std::isfinite(x) ? std::clamp(x, -8.0f, 8.0f) : 0.0f; }
float db(float x) { return 20.0f * std::log10(std::max(1e-6f, x)); }

struct AudioUse {
    explicit AudioUse(std::atomic<bool>& reader) : reader_(reader) { reader_.store(true); }
    ~AudioUse() { reader_.store(false); }
private:
    std::atomic<bool>& reader_;
};
}  // namespace

struct LiveSampler::Impl {
    enum Status { Idle, Armed, Recording, Ready };
    mutable std::mutex host;
    std::unique_ptr<Take> take;
    std::atomic<Take*> published{nullptr};
    std::atomic<bool> reader{false}, accepting{false};
    std::atomic<int> status{Idle}, inputSource{-1};
    float sr = 48000;

    std::atomic<bool> previewOn{false}, loop{false};
    std::atomic<int> first{0}, last{0};
    std::atomic<uint32_t> revision{0};
    uint32_t seen = 0;
    int cursor = 0, previewStart = 0, previewEnd = 0;
    bool previewLoop = false;

    void fence() const {
        while (reader.load()) std::this_thread::yield();
    }
    void stopCapture() {
        accepting.store(false);
        inputSource.store(-1);
        fence();
        if (take) status.store(Ready);
    }
    void reset() {
        accepting.store(false);
        inputSource.store(-1);
        previewOn.store(false);
        published.store(nullptr);
        fence();
        take.reset();
        status.store(Idle);
    }
};

LiveSampler::LiveSampler() : impl_(std::make_unique<Impl>()) {}
LiveSampler::~LiveSampler() = default;

bool LiveSampler::start(int source, float maxSec, float armDb) {
    if (source < 0 || source > 1 || !std::isfinite(maxSec) || maxSec <= 0 || !std::isfinite(armDb)) return false;
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.host);
    if (s.accepting.load()) return false;
    auto take = std::make_unique<Take>();
    take->rate = int(s.sr);
    take->source = source;
    take->threshold = armDb > -90.0f ? std::pow(10.0f, std::clamp(armDb, -90.0f, 0.0f) / 20) : 0.0f;
    const int frames = std::max(1, int(std::min(s.sr * std::min(30.0f, maxSec), 30.0f * 48000)));
    try {
        take->left.resize(frames);
        take->right.resize(frames);
    } catch (const std::bad_alloc&) {
        return false;
    }
    s.reset();
    s.take = std::move(take);
    s.published.store(s.take.get());
    s.status.store(s.take->threshold > 0 ? Impl::Armed : Impl::Recording);
    s.accepting.store(true);
    s.inputSource.store(source);
    return true;
}

double LiveSampler::stop() {
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.host);
    s.stopCapture();
    return s.take ? double(s.take->written.load()) / s.take->rate : 0.0;
}
void LiveSampler::discard() {
    std::lock_guard<std::mutex> lock(impl_->host);
    impl_->reset();
}
void LiveSampler::prepare(float rate) {
    std::lock_guard<std::mutex> lock(impl_->host);
    if (rate != impl_->sr) {
        impl_->stopCapture();
        impl_->previewOn.store(false);
    }
    impl_->sr = rate;
}

std::array<float, 3> LiveSampler::state() const {
    const auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.host);
    return {float(s.status.load()), s.take ? float(s.take->written.load()) / s.take->rate : 0.0f,
            s.take ? db(s.take->peak.load()) : -120.0f};
}

std::vector<float> LiveSampler::peaks(int bins) const {
    const auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.host);
    if (!s.take) return std::vector<float>(size_t(clampPeakBins(bins)) * 2);
    const auto n = s.take->written.load(std::memory_order_acquire);
    return pcmPeaks(s.take->left.data(), s.take->right.data(), n, bins);
}

std::array<float, 7> LiveSampler::analyze() const {
    const auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.host);
    if (!s.take) return {0, 0, 0, -1, -1, -1, s.sr};
    const auto& take = *s.take;
    const int n = take.written.load(std::memory_order_acquire);
    std::vector<float> l(take.left.begin(), take.left.begin() + n), r(take.right.begin(), take.right.begin() + n);
    int64_t start = 0, end = 0, a = -1, b = -1;
    sampleBounds(l, r, -50.0f, start, end);
    const float gain = std::clamp(db(normalizeGain(l, r, start, end, -1.0f)), -60.0f, 60.0f);
    findLoop(l, r, start, end, a, b);
    int root = -1;
    if (end > start) {
        Tuner tuner;
        tuner.prepare(float(take.rate));
        tuner.setEnabled(true);
        const int count = int(std::min<int64_t>(8192, end - start));
        const int64_t mid = (start + end - count) / 2;
        tuner.pushInput(l.data() + mid, r.data() + mid, count);
        TunerReading pitch;
        if (tuner.analyze(pitch)) root = std::clamp(int(std::lround(pitch.midiNoteFloat)), 0, 127);
    }
    return {float(start), float(end), gain, float(a), float(b), float(root), float(take.rate)};
}

void LiveSampler::capture(int source, const float* left, const float* right, int frames) {
    auto& s = *impl_;
    if (s.inputSource.load(std::memory_order_relaxed) != source || !left || frames <= 0) return;
    AudioUse use(s.reader);
    if (!s.accepting.load()) return;
    auto* take = s.published.load();
    if (!take || take->source != source) return;
    if (!right) right = left;
    int first = 0;
    float peak = take->peak.load(std::memory_order_relaxed);
    if (s.status.load(std::memory_order_relaxed) == Impl::Armed) {
        while (first < frames) {
            const float value = std::max(std::fabs(sample(left[first])), std::fabs(sample(right[first])));
            peak = std::max(peak, value);
            if (value >= take->threshold) break;
            ++first;
        }
        if (first == frames) { take->peak.store(peak, std::memory_order_relaxed); return; }
        s.status.store(Impl::Recording);
    }
    int written = take->written.load(std::memory_order_relaxed);
    const int count = std::min(frames - first, int(take->left.size()) - written);
    for (int i = 0; i < count; ++i, ++written) {
        const float l = sample(left[first+i]), r = sample(right[first+i]);
        take->left[written] = l;
        take->right[written] = r;
        peak = std::max({peak, std::fabs(l), std::fabs(r)});
    }
    take->peak.store(peak, std::memory_order_relaxed);
    take->written.store(written, std::memory_order_release);
    if (written == int(take->left.size())) {
        s.inputSource.store(-1);
        s.accepting.store(false);
        s.status.store(Impl::Ready);
    }
}

void LiveSampler::audition(bool on, bool shouldLoop, int start, int end) {
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.host);
    const int n = s.take ? s.take->written.load(std::memory_order_acquire) : 0;
    if (!on || n == 0 || s.sr != s.take->rate) { s.previewOn.store(false); return; }
    start = std::clamp(start, 0, n - 1);
    end = end < 0 ? n : std::clamp(end, start + 1, n);
    s.revision.fetch_add(1);
    s.first.store(start);
    s.last.store(end);
    s.loop.store(shouldLoop);
    s.revision.fetch_add(1);
    s.previewOn.store(true);
}
bool LiveSampler::auditioning() const { return impl_->previewOn.load(std::memory_order_relaxed); }

void LiveSampler::mixAudition(float* left, float* right, int frames) {
    auto& s = *impl_;
    AudioUse use(s.reader);
    if (!s.previewOn.load()) return;
    const auto* take = s.published.load();
    if (!take) return;
    const auto rev = s.revision.load();
    if (rev != s.seen) {
        if (rev & 1) return;
        const int first = s.first.load(), last = s.last.load();
        const bool loop = s.loop.load();
        if (rev != s.revision.load()) return;
        s.seen = rev;
        s.cursor = s.previewStart = first;
        s.previewEnd = last;
        s.previewLoop = loop;
    }
    // A replaced take can be published between an old preview's flag and
    // pointer reads. Validate the committed data before using its cursor.
    if (s.previewEnd > take->written.load(std::memory_order_acquire)) return;
    const int fade = std::max(1, take->rate / 200);
    for (int i = 0; i < frames; ++i) {
        if (s.cursor >= s.previewEnd) {
            if (!s.previewLoop) {
                if (rev == s.revision.load()) s.previewOn.store(false);
                break;
            }
            s.cursor = s.previewStart;
        }
        const float gain = float(std::min({fade, s.cursor - s.previewStart, s.previewEnd - s.cursor})) / fade;
        left[i] += take->left[s.cursor] * gain;
        right[i] += take->right[s.cursor] * gain;
        ++s.cursor;
    }
}

std::string LiveSampler::exportWav(const std::string& path, int start, int end, float gainDb,
                                   int a, int b, float xfadeMs, int root) const {
    const auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.host);
    if (!s.take || path.empty() || !std::isfinite(gainDb) || !std::isfinite(xfadeMs))
        return "Invalid capture export";
    const auto& take = *s.take;
    const int n = take.written.load(std::memory_order_acquire);
    start = std::clamp(start, 0, n);
    end = end < 0 ? n : std::clamp(end, start, n);
    if (end - start < 2) return "No audio in the selected range";
    if (a < start || b <= a || b > end) { a = -1; b = -1; }
    const int fade = std::min((end - start) / 4, take.rate / 200);
    const float gain = std::pow(10.0f, std::clamp(gainDb, -60.0f, 60.0f) / 20);
    std::vector<float> l(end - start), r(end - start);
    for (int i = start; i < end; ++i) {
        const float edge = fade == 0 ? 1.0f : float(std::min({fade, i-start, end-1-i})) / fade;
        l[i-start] = std::clamp(take.left[i] * gain * edge, -1.0f, 1.0f);
        r[i-start] = std::clamp(take.right[i] * gain * edge, -1.0f, 1.0f);
    }
    if (a >= 0) {
        // Fade the end into the pre-roll immediately preceding the loop
        // start, so the exported loop meets the unmodified start sample.
        const int count = std::min((b - a) / 2,
            int(std::clamp(xfadeMs, 0.0f, 1000.0f) * take.rate / 1000));
        for (int i = 0; i < count; ++i) {
            const float angle = kHalfPi * float(i+1) / count;
            const float x = std::cos(angle), y = std::sin(angle);
            const int head = std::max(0, a - start - count + i), tail = b - start - count + i;
            l[tail] = std::clamp(x*l[tail] + y*l[head], -1.0f, 1.0f);
            r[tail] = std::clamp(x*r[tail] + y*r[head], -1.0f, 1.0f);
        }
    }
    static std::atomic<uint32_t> nonce{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temp = path + ".tmp." + std::to_string(stamp) + "." + std::to_string(nonce.fetch_add(1));
    std::string error;
    if (writeWav(temp, l, &r, take.rate, true, error, root < 0 ? 60 : std::min(root, 127),
                 a < 0 ? -1 : a-start, b < 0 ? -1 : b-start-1)) {
        std::error_code ec;
        std::filesystem::rename(utf8Path(temp), utf8Path(path), ec);
        if (!ec) return {};
        error = "Could not publish WAV: " + ec.message();
    }
    std::error_code ignored;
    std::filesystem::remove(utf8Path(temp), ignored);
    return error;
}

}  // namespace hsynth
