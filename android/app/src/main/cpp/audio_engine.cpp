#include "audio_engine.h"

#include <dlfcn.h>
#include <sched.h>
#include <unistd.h>

#include <android/log.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#define LOG_TAG "HSynthAudio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

bool AudioEngine::start(int deviceId, bool exclusive, int bufferBursts, int sampleRate) {
    std::lock_guard<std::mutex> lock(lock_);
    if (stream_) {
        stream_->stop();
        stream_->close();
        stream_.reset();
    }
    deviceId_ = deviceId;
    exclusive_ = exclusive;
    bufferBursts_ = bufferBursts;
    requestedRate_ = sampleRate;
    wantRunning_ = true;
    return openLocked();
}

bool AudioEngine::openLocked() {
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
        return false;
    }

    // The engine must be prepared for the real stream rate before audio runs.
    engine_.prepare((float)stream_->getSampleRate(), hsynth::kMaxBlockSize);

    const int burst = stream_->getFramesPerBurst();
    // ADPF target: half the callback period. With the whole period as the
    // target the governor sees a light load as slack and lowers the clocks
    // until a callback takes most of the period, so any extra work (a song
    // starting, effects waking up) then overruns it.
    startHintSession((int64_t)(0.5e9 * (double)burst / (double)stream_->getSampleRate()));
    if (bufferBursts_ > 0) {
        tuner_.reset();
        stream_->setBufferSizeInFrames(burst * bufferBursts_);
    } else {
        stream_->setBufferSizeInFrames(burst * 2);
        tuner_ = std::make_unique<oboe::LatencyTuner>(*stream_);
    }
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
    return true;
}

void AudioEngine::stop() {
    std::lock_guard<std::mutex> lock(lock_);
    wantRunning_ = false;
    if (stream_) {
        stream_->stop();
        stream_->close();
        stream_.reset();
    }
    if (inStream_) {
        inStream_->stop();
        inStream_->close();
        inStream_.reset();
    }
    tuner_.reset();
    closeHintSession();  // the callback thread has ended with the stream
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
    if (pk > owner->inPeak_.load(std::memory_order_relaxed)) owner->inPeak_.store(pk, std::memory_order_relaxed);
    return oboe::DataCallbackResult::Continue;
}

bool AudioEngine::openInputLocked() {
    if (!stream_) return false;
    inCb_.owner = this;
    oboe::AudioStreamBuilder b;
    b.setDirection(oboe::Direction::Input)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Shared)
        ->setFormat(oboe::AudioFormat::Float)
        ->setFormatConversionAllowed(true)
        ->setChannelCount(oboe::ChannelCount::Mono)
        ->setChannelConversionAllowed(true)
        ->setSampleRate(stream_->getSampleRate())
        ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
        ->setInputPreset(oboe::InputPreset::Unprocessed)
        ->setDataCallback(&inCb_);
    oboe::Result r = b.openStream(inStream_);
    if (r != oboe::Result::OK) {
        LOGE("input open failed: %s", oboe::convertToText(r));
        inStream_.reset();
        return false;
    }
    inRing_.clear();
    if (inStream_->requestStart() != oboe::Result::OK) {
        inStream_->close();
        inStream_.reset();
        return false;
    }
    LOGI("input: %d Hz, %d ch", inStream_->getSampleRate(), inStream_->getChannelCount());
    return true;
}

bool AudioEngine::setInputEnabled(bool on) {
    std::lock_guard<std::mutex> lock(lock_);
    wantInput_ = on;
    if (!on) {
        if (inStream_) {
            auto s = inStream_;
            inStream_.reset();  // the output callback stops reading first
            engine_.setInputBuffer(nullptr, nullptr);
            s->stop();
            s->close();
        }
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
        recRate_ = stream_ ? stream_->getSampleRate() : requestedRate_;
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
}  // namespace

void AudioEngine::startHintSession(int64_t targetNs) {
    closeHintSession();
    hintCancel_.store(false);
    hintThread_ = std::thread([this, targetNs] {
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
        cpu_set_t fast;
        if (fastCoreMask(fast)) {
            int pinned = 0;
            for (int i = 0; i < n; ++i) pinned += sched_setaffinity(tids[i], sizeof fast, &fast) == 0;
            LOGI("pinned %d of %d audio threads to %d fast cores", pinned, n, CPU_COUNT(&fast));
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

oboe::DataCallbackResult AudioEngine::onAudioReady(oboe::AudioStream* stream, void* audioData, int32_t numFrames) {
    const auto t0 = std::chrono::steady_clock::now();
    if (audioTid_.load(std::memory_order_relaxed) == 0) audioTid_.store((int)gettid(), std::memory_order_relaxed);
    if (tuner_) tuner_->tune();  // lock-free, real-time safe
    float* out = static_cast<float*>(audioData);
    const int channels = stream->getChannelCount();
    int done = 0;
    const bool input = inStream_ != nullptr;
    if (input) {
        // keep the input latency bounded: drop what piled up beyond 4 callbacks
        const size_t backlog = inRing_.available();
        if (backlog > (size_t)numFrames * 4) inRing_.skip(backlog - (size_t)numFrames * 2);
    }
    while (done < numFrames) {
        const int n = std::min(numFrames - done, hsynth::kMaxBlockSize);
        if (input) {
            const size_t got = inRing_.read(inL_, (size_t)n);
            for (size_t i = got; i < (size_t)n; ++i) inL_[i] = 0.0f;
            std::copy(inL_, inL_ + n, inR_);
            engine_.setInputBuffer(inL_, inR_);
        }
        engine_.process(left_, right_, n);
        if (recording_.load(std::memory_order_relaxed)) {
            float inter[2 * hsynth::kMaxBlockSize];
            for (int i = 0; i < n; ++i) { inter[2 * i] = left_[i]; inter[2 * i + 1] = right_[i]; }
            recRing_.write(inter, (size_t)n * 2);  // a full ring drops (the writer is far behind)
        }
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
    const float load = us / (1e6f * (float)numFrames / (float)stream->getSampleRate());
    cbUsAvg_.store(cbUsAvg_.load(std::memory_order_relaxed) * 0.98f + us * 0.02f, std::memory_order_relaxed);
    if (us > cbUsMax_.load(std::memory_order_relaxed)) cbUsMax_.store(us, std::memory_order_relaxed);
    if (load > cbLoadMax_.load(std::memory_order_relaxed)) cbLoadMax_.store(load, std::memory_order_relaxed);
    lastFrames_.store(numFrames, std::memory_order_relaxed);
    if (void* s = hint_.load(std::memory_order_acquire)) hintApi().report(s, (int64_t)(us * 1000.0f));
    return oboe::DataCallbackResult::Continue;
}

void AudioEngine::onErrorAfterClose(oboe::AudioStream*, oboe::Result error) {
    // Typically ErrorDisconnected (headphones / USB audio plugged or removed).
    // Reopen on a separate thread: this callback must not block.
    LOGI("stream closed: %s - reopening", oboe::convertToText(error));
    std::thread([this] {
        std::lock_guard<std::mutex> lock(lock_);
        stream_.reset();
        tuner_.reset();
        if (!wantRunning_) return;
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
    std::lock_guard<std::mutex> lock(lock_);
    if (stream_) {
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
    s.lastCallbackFrames = lastFrames_.load();
    s.callbackUsAvg = cbUsAvg_.load();
    s.callbackUsMax = cbUsMax_.load();
    s.callbackLoadMax = cbLoadMax_.load();
    s.restarts = restarts_.load();
    return s;
}
