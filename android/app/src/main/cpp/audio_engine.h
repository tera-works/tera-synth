// Android audio output via Oboe. This is the only place that knows about the
// audio API; the DSP engine itself is platform independent.
#pragma once

#include <oboe/Oboe.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <thread>

#include "hsynth/engine.h"

struct AudioStats {
    bool running = false;
    int sampleRate = 0;
    int framesPerBurst = 0;
    int bufferSize = 0;
    int bufferCapacity = 0;
    int xruns = 0;
    int api = 0;            // 0 unspecified, 1 OpenSL ES, 2 AAudio
    bool exclusive = false;
    bool lowLatency = false;
    int deviceId = 0;
    int lastCallbackFrames = 0;
    float callbackUsAvg = 0;
    float callbackUsMax = 0;
    float callbackLoadMax = 0;  // callback time / buffer duration
    double latencyMs = -1;
    int restarts = 0;
};

// Single-producer / single-consumer float ring (lock-free; real-time safe on both ends).
class FloatRing {
public:
    explicit FloatRing(size_t capacity) : buf_(capacity) {}
    size_t available() const { return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_relaxed); }
    size_t space() const { return buf_.size() - (w_.load(std::memory_order_relaxed) - r_.load(std::memory_order_acquire)); }
    size_t write(const float* d, size_t n) {
        n = std::min(n, space());
        size_t w = w_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) buf_[(w + i) % buf_.size()] = d[i];
        w_.store(w + n, std::memory_order_release);
        return n;
    }
    size_t read(float* d, size_t n) {
        n = std::min(n, available());
        size_t r = r_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) d[i] = buf_[(r + i) % buf_.size()];
        r_.store(r + n, std::memory_order_release);
        return n;
    }
    void skip(size_t n) { r_.store(r_.load(std::memory_order_relaxed) + std::min(n, available()), std::memory_order_release); }
    void clear() { r_.store(w_.load()); }
private:
    std::vector<float> buf_;
    std::atomic<size_t> w_{0}, r_{0};
};

class AudioEngine : public oboe::AudioStreamDataCallback, public oboe::AudioStreamErrorCallback {
public:
    explicit AudioEngine(hsynth::Engine& engine) : engine_(engine) {}
    ~AudioEngine() override { stop(); }

    // bufferBursts: 0 = automatic (LatencyTuner), otherwise burst multiple.
    bool start(int deviceId, bool exclusive, int bufferBursts, int sampleRate);
    void stop();
    AudioStats stats();
    void resetPeaks() { cbUsMax_.store(0); cbLoadMax_.store(0); }

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream, void* audioData, int32_t numFrames) override;
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override;

    // Audio input (microphone / USB input) for the vocoder and the looper.
    // Needs the RECORD_AUDIO permission. Returns false if no input opened.
    bool setInputEnabled(bool on);
    bool inputEnabled() const { return inStream_ != nullptr; }
    float inputPeak() { return inPeak_.exchange(0.0f); }

    // Recording the output to a 32-bit float WAV file (writer thread).
    bool startRecording(const std::string& path);
    double stopRecording();  // seconds written
    bool recording() const { return recording_.load(); }
    double recordedSeconds() const;

private:
    bool openLocked();
    // ADPF performance hint (Android 12+): the audio callback thread and the
    // engine's render helpers report their work so the scheduler keeps their
    // cores at a suitable clock (fewer spikes when playing starts after idle).
    void startHintSession(int64_t targetNs);
    void closeHintSession();
    std::atomic<int> audioTid_{0};
    std::atomic<void*> hint_{nullptr};
    std::thread hintThread_;
    std::atomic<bool> hintCancel_{false};

    hsynth::Engine& engine_;
    std::mutex lock_;
    std::shared_ptr<oboe::AudioStream> stream_;
    std::unique_ptr<oboe::LatencyTuner> tuner_;
    int deviceId_ = 0;
    bool exclusive_ = true;
    int bufferBursts_ = 2;
    int requestedRate_ = 48000;
    bool wantRunning_ = false;
    std::atomic<int> restarts_{0};

    // input
    struct InputCallback : oboe::AudioStreamDataCallback {
        AudioEngine* owner = nullptr;
        oboe::DataCallbackResult onAudioReady(oboe::AudioStream* s, void* data, int32_t n) override;
    } inCb_;
    bool openInputLocked();
    std::shared_ptr<oboe::AudioStream> inStream_;
    bool wantInput_ = false;
    FloatRing inRing_{48000};
    std::atomic<float> inPeak_{0.0f};
    float inL_[hsynth::kMaxBlockSize];
    float inR_[hsynth::kMaxBlockSize];
    // recording
    void writerLoop();
    FloatRing recRing_{48000 * 2 * 4};
    std::atomic<bool> recording_{false};
    std::thread writer_;
    FILE* recFile_ = nullptr;
    std::atomic<int64_t> recFrames_{0};
    int recRate_ = 48000;
    float left_[hsynth::kMaxBlockSize];
    float right_[hsynth::kMaxBlockSize];

    std::atomic<float> cbUsAvg_{0}, cbUsMax_{0}, cbLoadMax_{0};
    std::atomic<int> lastFrames_{0};
};
