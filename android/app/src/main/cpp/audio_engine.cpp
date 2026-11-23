#include "audio_engine.h"

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#define LOG_TAG "HSynthAudio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
// one stats line every 5 s while audio runs: `adb logcat -s HSynth`
#define LOGS(...) __android_log_print(ANDROID_LOG_INFO, "HSynth", __VA_ARGS__)

namespace {
// Tuning knobs for experiments without rebuilding (read when a stream opens;
// `adb shell setprop debug.hsynth.<name> <value>`, then restart audio):
//   debug.hsynth.cpus.audio    CPUs for the callback thread: "fast" (default:
//                              every core faster than the slowest cluster),
//                              "none" (no pinning) or a list such as "4-7" / "6,7"
//   debug.hsynth.cpus.workers  the same for the engine's render helpers
//   debug.hsynth.hint_pct      ADPF target as % of the callback period (default 50)
std::string prop(const char* name, const char* def) {
    char v[PROP_VALUE_MAX] = {0};
    if (__system_property_get(name, v) > 0) return v;
    return def;
}
}  // namespace

int64_t AudioEngine::nowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

bool AudioEngine::start(int deviceId, bool exclusive, int bufferBursts, int sampleRate, bool alsaDirect) {
    startGen_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(lock_);
    if (alsa_) {
        alsa_->stop();
        alsa_.reset();
    }
    if (stream_) {
        stream_->stop();
        stream_->close();
        stream_.reset();
    }
    closeHintSession();
    deviceId_ = deviceId;
    exclusive_ = exclusive;
    bufferBursts_ = bufferBursts;
    requestedRate_ = sampleRate > 0 ? sampleRate : 48000;
    alsaDirect_ = alsaDirect;
    wantRunning_ = true;
    lastCbNs_.store(0, std::memory_order_relaxed);
    const bool ok = openLocked();
    if (!statsThread_.joinable()) {
        {
            std::lock_guard<std::mutex> l(statsMutex_);
            statsStop_ = false;
        }
        statsThread_ = std::thread([this] { statsLoop(); });
    }
    return ok;
}

std::string AudioEngine::lastBackendNote() {
    std::lock_guard<std::mutex> l(noteMutex_);
    return backendNote_;
}

bool AudioEngine::openLocked(bool tryAlsa) {
    std::string fallbackNote;
    if (alsaDirect_ && tryAlsa) {
        AlsaOut::Config cfg;
        cfg.card = -1;
        cfg.device = -1;
        cfg.sampleRate = requestedRate_;
        cfg.bufferBursts = bufferBursts_;
        cfg.externalGain = &alsaGain_;

        const uint64_t gen = startGen_.load(std::memory_order_relaxed);
        AlsaOut::Callbacks cb;
        cb.onPrepared = [this](int rate, int period) {
            engine_.prepare((float)rate, hsynth::kMaxBlockSize);
            const int pct = std::max(10, std::min(100, atoi(prop("debug.hsynth.hint_pct", "50").c_str())));
            startHintSession((int64_t)(pct * 1e7 * (double)period / (double)rate));
        };
        cb.onThreadStart = [this](int tid) {
            audioTid_.store(tid, std::memory_order_release);
        };
        cb.onRender = [this](float* left, float* right, int frames) {
            if (inputEnabled()) {
                const size_t backlog = inRing_.available();
                if (backlog > (size_t)frames * 4) inRing_.skip(backlog - (size_t)frames * 2);
            }
            int done = 0;
            while (done < frames) {
                const int n = std::min(frames - done, hsynth::kMaxBlockSize);
                renderStereoBlock(left + done, right + done, n);
                done += n;
            }
        };
        cb.onPeriodDone = [this](int rate, int frames, float us, int64_t t0ns) {
            recordPeriodStats(frames, us, t0ns, rate);
        };
        cb.onDisconnected = [this, gen] {
            handleAlsaDisconnect(gen);
        };

        auto alsa = std::make_unique<AlsaOut>();
        std::string err;
        if (alsa->start(cfg, std::move(cb), err)) {
            alsa_ = std::move(alsa);
            minBursts_ = 0;
            tuneXruns_ = -1;
            tuneCountdown_ = 0;
            lastCbNs_.store(0, std::memory_order_relaxed);
            if (wantInput_ && !inStream_) openInputLocked();
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "Direct ALSA: %s (C%dD%dp, %d Hz, %s, burst %d, buf %d)",
                          alsa_->cardName().c_str(), alsa_->card(), alsa_->device(),
                          alsa_->sampleRate(), alsa_->formatName(),
                          alsa_->periodSize(), alsa_->bufferSize());
            {
                std::lock_guard<std::mutex> l(noteMutex_);
                backendNote_ = buf;
            }
            LOGI("%s", buf);
            return true;
        }
        LOGI("Direct ALSA unavailable (%s), falling back to AAudio", err.c_str());
        fallbackNote = "Direct ALSA unavailable (" + err + "), falling back to AAudio";
    }

    oboe::AudioStreamBuilder b;
    b.setDirection(oboe::Direction::Output)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(exclusive_ ? oboe::SharingMode::Exclusive : oboe::SharingMode::Shared)
        ->setFormat(oboe::AudioFormat::Float)
        ->setFormatConversionAllowed(true)
        ->setChannelCount(oboe::ChannelCount::Stereo)
        ->setChannelConversionAllowed(true)
        ->setSampleRate(requestedRate_)
        ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
        ->setUsage(oboe::Usage::Game)
        ->setContentType(oboe::ContentType::Music)
        ->setDataCallback(this)
        ->setErrorCallback(this);
    if (deviceId_ > 0) b.setDeviceId(deviceId_);

    oboe::Result r = b.openStream(stream_);
    if (r != oboe::Result::OK && exclusive_) {
        LOGI("exclusive open failed (%s), retrying shared", oboe::convertToText(r));
        b.setSharingMode(oboe::SharingMode::Shared);
        r = b.openStream(stream_);
    }
    if (r != oboe::Result::OK) {
        LOGE("openStream failed: %s", oboe::convertToText(r));
        stream_.reset();
        if (!fallbackNote.empty()) {
            std::lock_guard<std::mutex> l(noteMutex_);
            backendNote_ = fallbackNote;
        }
        return false;
    }

    // The engine must be prepared for the real stream rate before audio runs.
    engine_.prepare((float)stream_->getSampleRate(), hsynth::kMaxBlockSize);

    const int burst = stream_->getFramesPerBurst();
    // ADPF target: half the callback period. With the whole period as the
    // target the governor sees a light load as slack and lowers the clocks
    // until a callback takes most of the period, so any extra work (a song
    // starting, effects waking up) then overruns it.
    const int pct = std::max(10, std::min(100, atoi(prop("debug.hsynth.hint_pct", "50").c_str())));
    startHintSession((int64_t)(pct * 1e7 * (double)burst / (double)stream_->getSampleRate()));
    if (bufferBursts_ > 0) {
        minBursts_ = 0;
        stream_->setBufferSizeInFrames(burst * bufferBursts_);
    } else {
        // Automatic: start at the lowest latency (one burst with MMAP, where the
        // hardware reads straight from our buffer; two otherwise) and add a burst
        // whenever an underrun occurs; after a long clean stretch it tries one
        // burst less again (backing off if that glitches).
        const bool mmap = stream_->getSharingMode() == oboe::SharingMode::Exclusive;
        minBursts_ = mmap ? 1 : 2;
        stream_->setBufferSizeInFrames(burst * minBursts_);
    }
    tuneXruns_ = -1;
    tuneCountdown_ = 0;
    named_.store(false);
    if (wantInput_ && !inStream_) openInputLocked();
    r = stream_->requestStart();
    if (r != oboe::Result::OK) {
        LOGE("requestStart failed: %s", oboe::convertToText(r));
        stream_->close();
        stream_.reset();
        return false;
    }
    LOGI("stream started: rate %d burst %d buffer %d api %d sharing %d perf %d", stream_->getSampleRate(), burst,
         stream_->getBufferSizeInFrames(), (int)stream_->getAudioApi(), (int)stream_->getSharingMode(),
         (int)stream_->getPerformanceMode());
    {
        std::lock_guard<std::mutex> l(noteMutex_);
        backendNote_ = fallbackNote;
    }
    return true;
}

void AudioEngine::stop() {
    startGen_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> l(statsMutex_);
        statsStop_ = true;
    }
    statsCv_.notify_all();
    if (statsThread_.joinable()) statsThread_.join();
    std::lock_guard<std::mutex> lock(lock_);
    wantRunning_ = false;
    if (alsa_) {
        alsa_->stop();
        alsa_.reset();
    }
    if (stream_) {
        stream_->stop();
        stream_->close();
        stream_.reset();
    }
    inputActive_.store(false, std::memory_order_release);
    if (inStream_) {
        inStream_->stop();
        inStream_->close();
        inStream_.reset();
    }
    inPeak_.store(0.0f, std::memory_order_relaxed);
    inPeakPeek_.store(0.0f, std::memory_order_relaxed);
    engine_.externalMidi().channelPanic(hsynth::kOmniChannel);
    engine_.externalMidi().service();
    closeHintSession();  // the callback thread has ended with the stream
}

void AudioEngine::commitSstWhileStopped() {
    std::lock_guard<std::mutex> lock(lock_);
    if (!alsa_ && !stream_) engine_.commitLiveChangeWhileStopped();
}

void AudioEngine::externalMidiAllOff(int slot) {
    std::lock_guard<std::mutex> lock(lock_);
    engine_.externalMidi().requestPanic(slot);
    if (!alsa_ && !stream_) engine_.externalMidi().service();
}

// ------------------------------------------------------------ input

oboe::DataCallbackResult AudioEngine::InputCallback::onAudioReady(oboe::AudioStream* s, void* data, int32_t n) {
    const float* in = static_cast<const float*>(data);
    const int ch = s->getChannelCount();
    float mono[512];
    float pk = 0.0f;
    for (int done = 0; done < n;) {
        const int m = std::min(n - done, 512);
        for (int i = 0; i < m; ++i) {
            mono[i] = in[(size_t)(done + i) * ch];
            pk = std::max(pk, std::fabs(mono[i]));
        }
        owner->inRing_.write(mono, (size_t)m);
        done += m;
    }
    float previous = owner->inPeak_.load(std::memory_order_relaxed);
    while (pk > previous && !owner->inPeak_.compare_exchange_weak(previous, pk, std::memory_order_relaxed)) {}
    // A decaying meter shared by every non-destructive reader.
    const float decay = std::exp(-(float)n / (0.2f * s->getSampleRate()));
    owner->inPeakPeek_.store(std::max(pk, decay * owner->inPeakPeek_.load(std::memory_order_relaxed)),
                             std::memory_order_relaxed);
    return oboe::DataCallbackResult::Continue;
}

bool AudioEngine::openInputLocked() {
    if (!stream_ && !alsa_) return false;
    const int outRate = alsa_ ? alsa_->sampleRate() : stream_->getSampleRate();
    inCb_.owner = this;
    oboe::AudioStreamBuilder b;
    b.setDirection(oboe::Direction::Input)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Shared)
        ->setFormat(oboe::AudioFormat::Float)
        ->setFormatConversionAllowed(true)
        ->setChannelCount(oboe::ChannelCount::Mono)
        ->setChannelConversionAllowed(true)
        ->setSampleRate(outRate)
        ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
        ->setInputPreset(oboe::InputPreset::Unprocessed)
        ->setDataCallback(&inCb_);
    oboe::Result r = b.openStream(inStream_);
    if (r != oboe::Result::OK) {
        LOGE("input open failed: %s", oboe::convertToText(r));
        inStream_.reset();
        return false;
    }
    inPeak_.store(0.0f, std::memory_order_relaxed);
    inPeakPeek_.store(0.0f, std::memory_order_relaxed);
    if (inStream_->requestStart() != oboe::Result::OK) {
        inStream_->close();
        inStream_.reset();
        return false;
    }
    inputGeneration_.fetch_add(1, std::memory_order_relaxed);
    inputActive_.store(true, std::memory_order_release);
    LOGI("input: %d Hz, %d ch", inStream_->getSampleRate(), inStream_->getChannelCount());
    return true;
}

bool AudioEngine::setInputEnabled(bool on) {
    std::lock_guard<std::mutex> lock(lock_);
    wantInput_ = on;
    if (!on) {
        inputActive_.store(false, std::memory_order_release);
        if (inStream_) {
            inStream_->stop();
            inStream_->close();
            inStream_.reset();
        }
        inPeak_.store(0.0f, std::memory_order_relaxed);
        inPeakPeek_.store(0.0f, std::memory_order_relaxed);
        return true;
    }
    if (inStream_) return true;
    return openInputLocked();
}

// ------------------------------------------------------------ recording

namespace {
void put32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
void put16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }
void wavHeader(FILE* f, int rate, uint32_t frames) {
    const uint32_t bytes = frames * 2 * 4;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 3 /* IEEE float */); put16(f, 2);
    put32(f, (uint32_t)rate); put32(f, (uint32_t)rate * 8); put16(f, 8); put16(f, 32);
    fwrite("data", 1, 4, f); put32(f, bytes);
}
}  // namespace

bool AudioEngine::startRecording(const std::string& path) {
    if (recording_.load()) return false;
    {
        std::lock_guard<std::mutex> lock(lock_);
        recRate_ = alsa_ ? alsa_->sampleRate() : (stream_ ? stream_->getSampleRate() : requestedRate_);
    }
    recFile_ = fopen(path.c_str(), "wb");
    if (!recFile_) return false;
    wavHeader(recFile_, recRate_, 0);
    recFrames_.store(0);
    recRing_.clear();
    recording_.store(true);
    writer_ = std::thread([this] { writerLoop(); });
    return true;
}

void AudioEngine::writerLoop() {
    std::vector<float> buf(48000);
    while (true) {
        const bool more = recording_.load();
        size_t n;
        while ((n = recRing_.read(buf.data(), buf.size() & ~(size_t)1)) > 0) {
            fwrite(buf.data(), sizeof(float), n, recFile_);
            recFrames_.fetch_add((int64_t)(n / 2));
        }
        if (!more) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

double AudioEngine::stopRecording() {
    if (!recording_.exchange(false)) return 0.0;
    if (writer_.joinable()) writer_.join();
    const int64_t frames = recFrames_.load();
    wavHeader(recFile_, recRate_, (uint32_t)frames);
    fclose(recFile_);
    recFile_ = nullptr;
    return (double)frames / recRate_;
}

double AudioEngine::recordedSeconds() const { return (double)recFrames_.load() / recRate_; }

namespace {
// NDK performance hint API, resolved at run time (minSdk 28; the API is 31+)
struct HintApi {
    using GetManager = void* (*)();
    using Create = void* (*)(void*, const int32_t*, size_t, int64_t);
    using Report = int (*)(void*, int64_t);
    using Close = void (*)(void*);
    GetManager getManager = nullptr;
    Create create = nullptr;
    Report report = nullptr;
    Close close = nullptr;
    bool ok = false;
    HintApi() {
        void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return;
        getManager = (GetManager)dlsym(lib, "APerformanceHint_getManager");
        create = (Create)dlsym(lib, "APerformanceHint_createSession");
        report = (Report)dlsym(lib, "APerformanceHint_reportActualWorkDuration");
        close = (Close)dlsym(lib, "APerformanceHint_closeSession");
        ok = getManager && create && report && close;
    }
};
const HintApi& hintApi() {
    static const HintApi api;
    return api;
}
}  // namespace

namespace {
// Cores faster than the slowest cluster (big.LITTLE): the audio callback and the
// render helpers stay off the little cores, where a callback can take 2-3x as
// long and any extra work (a song starting, effects waking up) overruns the
// buffer. Empty when every core is the same.
bool fastCoreMask(cpu_set_t& set) {
    long maxf[CPU_SETSIZE];
    const int n = std::min((int)sysconf(_SC_NPROCESSORS_CONF), (int)CPU_SETSIZE);
    long lo = -1, hi = -1;
    for (int c = 0; c < n; ++c) {
        maxf[c] = -1;
        char path[96];
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        if (FILE* f = std::fopen(path, "r")) {
            if (std::fscanf(f, "%ld", &maxf[c]) != 1) maxf[c] = -1;
            std::fclose(f);
        }
        if (maxf[c] < 0) continue;
        lo = lo < 0 ? maxf[c] : std::min(lo, maxf[c]);
        hi = std::max(hi, maxf[c]);
    }
    if (lo < 0 || lo == hi) return false;
    CPU_ZERO(&set);
    int count = 0;
    for (int c = 0; c < n; ++c)
        if (maxf[c] > lo) { CPU_SET(c, &set); ++count; }
    return count > 0;
}

// "fast" / "none" / "4-7" / "6,7" -> mask; false = leave the thread unpinned
bool cpuMask(const std::string& spec, cpu_set_t& set) {
    if (spec == "none" || spec.empty()) return false;
    if (spec == "fast") return fastCoreMask(set);
    CPU_ZERO(&set);
    const int ncpu = std::min((int)sysconf(_SC_NPROCESSORS_CONF), (int)CPU_SETSIZE);
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t comma = spec.find(',', pos);
        if (comma == std::string::npos) comma = spec.size();
        const std::string tok = spec.substr(pos, comma - pos);
        int a = -1, b = -1;
        if (std::sscanf(tok.c_str(), "%d-%d", &a, &b) < 2) b = a;
        for (int c = std::max(0, a); a >= 0 && c <= b && c < ncpu; ++c) CPU_SET(c, &set);
        pos = comma + 1;
    }
    return CPU_COUNT(&set) > 0;
}
}  // namespace

void AudioEngine::startHintSession(int64_t targetNs) {
    closeHintSession();
    hintCancel_.store(false);
    hintThread_ = std::thread([this, targetNs] {
        pthread_setname_np(pthread_self(), "hsynth-hint");
        // the callback thread exists once audio runs: wait for its tid
        for (int i = 0; i < 300 && !hintCancel_.load() && audioTid_.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (hintCancel_.load() || audioTid_.load() == 0) return;
        int32_t tids[16];
        int n = 0;
        tids[n++] = audioTid_.load();
        int helpers[8];
        const int nh = engine_.renderThreadIds(helpers, 8);
        for (int i = 0; i < nh && n < 16; ++i) tids[n++] = helpers[i];
        // big.LITTLE: keep the callback and the render helpers on the fast cores
        // (see debug.hsynth.cpus.* above for experiments)
        cpu_set_t set;
        if (cpuMask(prop("debug.hsynth.cpus.audio", "fast"), set)) {
            const bool ok = sched_setaffinity(tids[0], sizeof set, &set) == 0;
            LOGI("audio thread %d pinned to %d cores: %s", tids[0], CPU_COUNT(&set), ok ? "ok" : "failed");
        }
        if (n > 1 && cpuMask(prop("debug.hsynth.cpus.workers", "fast"), set)) {
            int pinned = 0;
            for (int i = 1; i < n; ++i) pinned += sched_setaffinity(tids[i], sizeof set, &set) == 0;
            LOGI("pinned %d of %d render helpers to %d cores", pinned, n - 1, CPU_COUNT(&set));
        }
        if (!hintApi().ok) return;
        void* mgr = hintApi().getManager();
        void* s = mgr ? hintApi().create(mgr, tids, (size_t)n, targetNs) : nullptr;
        if (s) {
            hint_.store(s, std::memory_order_release);
            LOGI("performance hint session: %d threads, target %.2f ms", n, (double)targetNs / 1e6);
        } else {
            LOGI("performance hint session not available");
        }
    });
}

void AudioEngine::closeHintSession() {
    hintCancel_.store(true);
    if (hintThread_.joinable()) hintThread_.join();
    if (void* s = hint_.exchange(nullptr)) hintApi().close(s);
    audioTid_.store(0);
}

void AudioEngine::renderStereoBlock(float* left, float* right, int n) {
    if (inputEnabled()) {
        const uint32_t generation = inputGeneration_.load(std::memory_order_relaxed);
        if (generation != seenInputGeneration_) {
            seenInputGeneration_ = generation;
            inRing_.clear();
            engine_.setInputBuffer(nullptr, nullptr, 0);
        }
        const size_t got = inRing_.read(inL_, static_cast<size_t>(n));
        std::fill(inL_ + got, inL_ + n, 0.0f);
        engine_.setInputBuffer(inL_, inL_, static_cast<int>(got));
    } else {
        engine_.setInputBuffer(nullptr, nullptr, 0);
    }
    engine_.process(left, right, n);
    if (recording_.load(std::memory_order_relaxed)) {
        float inter[2 * hsynth::kMaxBlockSize];
        for (int i = 0; i < n; ++i) { inter[2 * i] = left[i]; inter[2 * i + 1] = right[i]; }
        recRing_.write(inter, (size_t)n * 2);  // a full ring drops (the writer is far behind)
    }
}

void AudioEngine::recordPeriodStats(int numFrames, float us, int64_t t0ns, int sampleRate) {
    {
        const int64_t last = lastCbNs_.exchange(t0ns, std::memory_order_relaxed);
        if (last > 0 && t0ns - last > winGapMaxNs_.load(std::memory_order_relaxed))
            winGapMaxNs_.store(t0ns - last, std::memory_order_relaxed);
    }
    const float load = sampleRate > 0 ? us / (1e6f * (float)numFrames / (float)sampleRate) : 0.0f;
    cbUsAvg_.store(cbUsAvg_.load(std::memory_order_relaxed) * 0.98f + us * 0.02f, std::memory_order_relaxed);
    if (us > cbUsMax_.load(std::memory_order_relaxed)) cbUsMax_.store(us, std::memory_order_relaxed);
    if (load > cbLoadMax_.load(std::memory_order_relaxed)) cbLoadMax_.store(load, std::memory_order_relaxed);
    lastFrames_.store(numFrames, std::memory_order_relaxed);
    if (us > winUsMax_.load(std::memory_order_relaxed)) winUsMax_.store(us, std::memory_order_relaxed);
    if (load > 0.5f) winSlow_.fetch_add(1, std::memory_order_relaxed);
    if (load > 1.0f) winOverBudget_.fetch_add(1, std::memory_order_relaxed);
    callbacks_.fetch_add(1, std::memory_order_relaxed);
    // MIDI -> audio: a note received before this callback started was applied
    // at its first sample (events are taken at the start of process())
    if (noteStampNs_.load(std::memory_order_relaxed) != 0) {
        const int64_t ts = noteStampNs_.exchange(0, std::memory_order_relaxed);
        const int64_t w = ts > 0 && ts < t0ns ? (t0ns - ts) / 1000 : 0;
        if (w < 100000) {  // longer: received while the stream was stopped / starting
            waitSumUs_.fetch_add(w, std::memory_order_relaxed);
            waitCount_.fetch_add(1, std::memory_order_relaxed);
            if (w > waitMaxUs_.load(std::memory_order_relaxed)) waitMaxUs_.store(w, std::memory_order_relaxed);
        }
    }
    if (void* s = hint_.load(std::memory_order_acquire)) hintApi().report(s, (int64_t)(us * 1000.0f));
}

oboe::DataCallbackResult AudioEngine::onAudioReady(oboe::AudioStream* stream, void* audioData, int32_t numFrames) {
    const auto t0 = std::chrono::steady_clock::now();
    const int64_t t0ns = nowNs();
    if (!named_.load(std::memory_order_relaxed)) {
        // a fixed name lets root scripts find the thread (agent tuning, perfetto)
        pthread_setname_np(pthread_self(), "hsynth-audio");
        named_.store(true, std::memory_order_relaxed);
    }
    if (audioTid_.load(std::memory_order_relaxed) == 0) audioTid_.store((int)gettid(), std::memory_order_relaxed);
    if (minBursts_ > 0) autoBuffer(stream);
    float* out = static_cast<float*>(audioData);
    const int channels = stream->getChannelCount();
    int done = 0;
    if (inputEnabled()) {
        // keep the input latency bounded: drop what piled up beyond 4 callbacks
        const size_t backlog = inRing_.available();
        if (backlog > (size_t)numFrames * 4) inRing_.skip(backlog - (size_t)numFrames * 2);
    }
    while (done < numFrames) {
        const int n = std::min(numFrames - done, hsynth::kMaxBlockSize);
        renderStereoBlock(left_, right_, n);
        float* o = out + (size_t)done * channels;
        if (channels == 2) {
            for (int i = 0; i < n; ++i) {
                o[2 * i] = left_[i];
                o[2 * i + 1] = right_[i];
            }
        } else {
            for (int i = 0; i < n; ++i) {
                float* f = o + (size_t)i * channels;
                f[0] = channels == 1 ? 0.5f * (left_[i] + right_[i]) : left_[i];
                if (channels > 1) f[1] = right_[i];
                for (int c = 2; c < channels; ++c) f[c] = 0.0f;
            }
        }
        done += n;
    }
    const float us = std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - t0).count();
    recordPeriodStats(numFrames, us, t0ns, stream->getSampleRate());
    return oboe::DataCallbackResult::Continue;
}

// Automatic buffer size (callback thread): one burst more after an underrun;
// after a clean stretch one burst less again, the stretch doubling (up to 16
// minutes) each time the smaller size glitched.
void AudioEngine::autoBuffer(oboe::AudioStream* stream) {
    if (--tuneCountdown_ > 0) return;
    tuneCountdown_ = 16;  // every 16 callbacks (32 ms at 96 frames)
    auto x = stream->getXRunCount();
    if (!x) return;
    const int xr = x.value();
    const int64_t now = lastCbNs_.load(std::memory_order_relaxed);
    const int burst = stream->getFramesPerBurst();
    const int size = stream->getBufferSizeInFrames();
    if (tuneXruns_ < 0) {
        tuneXruns_ = xr;
        tuneCleanSince_ = now;
        return;
    }
    if (xr > tuneXruns_) {
        tuneXruns_ = xr;
        if (size < std::min(stream->getBufferCapacityInFrames(), burst * 8)) {
            stream->setBufferSizeInFrames(size + burst);
            bufferGrown_.fetch_add(1, std::memory_order_relaxed);
            // shrinking failed (glitched soon after): wait longer next time
            if (now - tuneShrunkAt_ < tuneHoldNs_ * 2) tuneHoldNs_ = std::min<int64_t>(tuneHoldNs_ * 2, 960000000000LL);
        }
        tuneCleanSince_ = now;
        return;
    }
    if (size > burst * minBursts_ && now - tuneCleanSince_ > tuneHoldNs_) {
        stream->setBufferSizeInFrames(size - burst);
        bufferShrunk_.fetch_add(1, std::memory_order_relaxed);
        tuneShrunkAt_ = now;
        tuneCleanSince_ = now;
    }
}

void AudioEngine::handleAlsaDisconnect(uint64_t gen) {
    LOGI("Direct ALSA disconnected - falling back to AAudio");
    std::thread([this, gen] {
        std::lock_guard<std::mutex> lock(lock_);
        if (gen != startGen_.load(std::memory_order_relaxed) || !wantRunning_) return;
        if (alsa_) {
            alsa_->stop();
            alsa_.reset();
        }
        closeHintSession();
        restarts_.fetch_add(1);
        // Fall back to AAudio once without clearing alsaDirect_, so replugging
        // a USB DAC later re-engages Direct ALSA automatically.
        if (!openLocked(/*tryAlsa=*/false)) {
            deviceId_ = 0;
            openLocked(/*tryAlsa=*/false);
        }
    }).detach();
}

void AudioEngine::onErrorAfterClose(oboe::AudioStream*, oboe::Result error) {
    // Typically ErrorDisconnected (headphones / USB audio plugged or removed).
    // Reopen on a separate thread: this callback must not block.
    LOGI("stream closed: %s - reopening", oboe::convertToText(error));
    const uint64_t gen = startGen_.load(std::memory_order_relaxed);
    std::thread([this, gen] {
        std::lock_guard<std::mutex> lock(lock_);
        if (gen != startGen_.load(std::memory_order_relaxed) || !wantRunning_) return;
        stream_.reset();
        restarts_.fetch_add(1);
        if (!openLocked()) {
            // fall back to the default device
            deviceId_ = 0;
            openLocked();
        }
    }).detach();
}

AudioStats AudioEngine::stats() {
    AudioStats s;
    std::unique_lock<std::mutex> lock(lock_, std::try_to_lock);
    if (lock.owns_lock()) {
        if (alsa_) {
            s.running = alsa_->running();
            s.sampleRate = alsa_->sampleRate();
            s.framesPerBurst = alsa_->periodSize();
            s.bufferSize = alsa_->bufferSize();
            s.bufferCapacity = alsa_->bufferCapacity();
            s.xruns = alsa_->xruns();
            s.api = 3;  // alsa-direct
            s.exclusive = true;
            s.lowLatency = true;
            s.deviceId = alsa_->card();
            s.latencyMs = s.sampleRate > 0 ? 1000.0 * s.bufferSize / s.sampleRate : -1.0;
        } else if (stream_) {
            s.running = stream_->getState() == oboe::StreamState::Started;
            s.sampleRate = stream_->getSampleRate();
            s.framesPerBurst = stream_->getFramesPerBurst();
            s.bufferSize = stream_->getBufferSizeInFrames();
            s.bufferCapacity = stream_->getBufferCapacityInFrames();
            auto x = stream_->getXRunCount();
            s.xruns = x ? x.value() : -1;
            s.api = stream_->getAudioApi() == oboe::AudioApi::AAudio ? 2 : stream_->getAudioApi() == oboe::AudioApi::OpenSLES ? 1 : 0;
            s.exclusive = stream_->getSharingMode() == oboe::SharingMode::Exclusive;
            s.lowLatency = stream_->getPerformanceMode() == oboe::PerformanceMode::LowLatency;
            s.deviceId = stream_->getDeviceId();
            auto lat = stream_->calculateLatencyMillis();
            s.latencyMs = lat ? lat.value() : -1.0;
        }
        lock.unlock();
        std::lock_guard<std::mutex> sl(statsSnapMutex_);
        cachedStats_ = s;
    } else {
        // Stream open/close/reopen is in progress on another thread; never block UI statsTick.
        std::lock_guard<std::mutex> sl(statsSnapMutex_);
        s = cachedStats_;
    }
    s.lastCallbackFrames = lastFrames_.load();
    s.callbackUsAvg = cbUsAvg_.load();
    s.callbackUsMax = cbUsMax_.load();
    s.callbackLoadMax = cbLoadMax_.load();
    s.restarts = restarts_.load();
    s.minBufferBursts = minBursts_;
    return s;
}

// One line every 5 s while the stream runs, for `adb logcat -s HSynth`:
//   buf      buffer frames (bursts) / burst frames; lat = output latency estimate
//   cb       callback avg / max us in this window, over = callbacks longer than
//            their period, gap = longest time between two callbacks
//   midi     note-ons received -> start of the callback that rendered them
//            (avg / max ms); note->DAC ~ wait avg + output latency
//   hop      Java MidiManager delivery (sender timestamp -> app), avg / max ms
void AudioEngine::statsLoop() {
    pthread_setname_np(pthread_self(), "hsynth-stats");
    int lastXruns = -1;
    int64_t lastCallbacks = 0;
    float lastRenderPeak = 0.0f;
    for (;;) {
        {
            std::unique_lock<std::mutex> l(statsMutex_);
            statsCv_.wait_for(l, std::chrono::seconds(5), [this] { return statsStop_; });
            if (statsStop_) return;
        }
        const AudioStats a = stats();
        const int64_t cbs = callbacks_.load(std::memory_order_relaxed);
        const bool active = cbs != lastCallbacks;
        lastCallbacks = cbs;
        const float usMax = winUsMax_.exchange(0, std::memory_order_relaxed);
        const int over = winOverBudget_.exchange(0, std::memory_order_relaxed);
        const int slow = winSlow_.exchange(0, std::memory_order_relaxed);
        const double gapMs = (double)winGapMaxNs_.exchange(0, std::memory_order_relaxed) / 1e6;
        const int wn = waitCount_.exchange(0, std::memory_order_relaxed);
        const int64_t wsum = waitSumUs_.exchange(0, std::memory_order_relaxed);
        const double wavg = wn ? (double)wsum / wn / 1000.0 : 0.0;
        const double wmax = (double)waitMaxUs_.exchange(0, std::memory_order_relaxed) / 1000.0;
        const int hn = hopCount_.exchange(0, std::memory_order_relaxed);
        const int64_t hsum = hopSumUs_.exchange(0, std::memory_order_relaxed);
        const double havg = hn ? (double)hsum / hn / 1000.0 : 0.0;
        const double hmax = (double)hopMaxUs_.exchange(0, std::memory_order_relaxed) / 1000.0;
        if (!active || !a.running) continue;
        const hsynth::EngineStats e = engine_.stats();
        const int dx = lastXruns >= 0 && a.xruns >= lastXruns ? a.xruns - lastXruns : 0;
        lastXruns = a.xruns;
        const int grown = bufferGrown_.exchange(0), shrunk = bufferShrunk_.exchange(0);
        char midi[160] = "";
        if (wn) std::snprintf(midi, sizeof midi, " | midi n %d wait %.2f/%.2f ms note->DAC ~%.1f ms", wn, wavg, wmax,
                              wavg + std::max(0.0, a.latencyMs));
        char hop[64] = "";
        if (hn) std::snprintf(hop, sizeof hop, " hop %.2f/%.2f ms", havg, hmax);
        // a new slowest engine block: where it spent its time (us: params, swaps+events,
        // voices, inserts, rack, stats; then voices, parallel)
        char prof[128] = "";
        if (e.renderUsPeak > lastRenderPeak + 1.0f) {
            float v[hsynth::Engine::kProfPhases];
            engine_.peakProfile(v);
            int k = std::snprintf(prof, sizeof prof, " | new render peak phases");
            for (int i = 0; i < hsynth::Engine::kProfPhases && k < (int)sizeof prof - 8; ++i)
                k += std::snprintf(prof + k, sizeof prof - k, " %.0f", v[i]);
        }
        lastRenderPeak = e.renderUsPeak;
        char tune[64] = "";
        if (grown || shrunk) std::snprintf(tune, sizeof tune, " | buffer +%d -%d bursts (auto)", grown, shrunk);
        LOGS("audio %s %d Hz buf %d (%.1f bursts%s) burst %d lat %.1f ms | cb %.0f/%.0f us slow %d over %d gap %.1f ms | "
             "xruns %d (+%d) | voices %d peak %d/%d load %.0f%% render %.0f/%.0f us drop %u%s%s%s%s",
             a.api == 3 ? "alsa-direct" : a.api == 2 ? (a.exclusive ? "aaudio-mmap" : "aaudio") : a.api == 1 ? "opensl" : "?",
             a.sampleRate,
             a.bufferSize, a.framesPerBurst ? (double)a.bufferSize / a.framesPerBurst : 0.0, a.minBufferBursts ? " auto" : "",
             a.framesPerBurst, a.latencyMs, a.callbackUsAvg, usMax, slow, over, gapMs, a.xruns, dx, e.activeVoices, e.peakVoices,
             engine_.maxVoices(), e.cpuLoad * 100.0f, e.renderUsAvg, e.renderUsPeak, e.droppedEvents, midi, hop, tune, prof);
    }
}
