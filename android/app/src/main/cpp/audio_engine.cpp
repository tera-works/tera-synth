#include "audio_engine.h"

#include <android/log.h>

#include <chrono>
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
    if (bufferBursts_ > 0) {
        tuner_.reset();
        stream_->setBufferSizeInFrames(burst * bufferBursts_);
    } else {
        stream_->setBufferSizeInFrames(burst * 2);
        tuner_ = std::make_unique<oboe::LatencyTuner>(*stream_);
    }
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
    tuner_.reset();
}

oboe::DataCallbackResult AudioEngine::onAudioReady(oboe::AudioStream* stream, void* audioData, int32_t numFrames) {
    const auto t0 = std::chrono::steady_clock::now();
    if (tuner_) tuner_->tune();  // lock-free, real-time safe
    float* out = static_cast<float*>(audioData);
    const int channels = stream->getChannelCount();
    int done = 0;
    while (done < numFrames) {
        const int n = std::min(numFrames - done, hsynth::kMaxBlockSize);
        engine_.process(left_, right_, n);
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
