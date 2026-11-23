// Android audio output via Oboe. This is the only place that knows about the
// audio API; the DSP engine itself is platform independent.
#pragma once

#include <oboe/Oboe.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "alsa_out.h"
#include "hsynth/engine.h"

struct AudioStats {
    bool running = false;
    int sampleRate = 0;
    int framesPerBurst = 0;
    int bufferSize = 0;
    int bufferCapacity = 0;
    int xruns = 0;
    int api = 0;            // 0 unspecified, 1 OpenSL ES, 2 AAudio, 3 alsa-direct
    bool exclusive = false;
    bool lowLatency = false;
    int deviceId = 0;
    int lastCallbackFrames = 0;
    float callbackUsAvg = 0;
    float callbackUsMax = 0;
    float callbackLoadMax = 0;  // callback time / buffer duration
    double latencyMs = -1;
    int restarts = 0;
    int minBufferBursts = 0;    // automatic mode: the buffer starts here and grows on xruns
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

    // bufferBursts: 0 = automatic (starts at the lowest size, grows on underruns),
    // otherwise a fixed burst multiple.
    bool start(int deviceId, bool exclusive, int bufferBursts, int sampleRate, bool alsaDirect = false);
    void stop();
    AudioStats stats();
    void commitSstWhileStopped();
    void externalMidiAllOff(int slot);
    std::string lastBackendNote();
    void setAlsaGain(float gain) {
        if (gain < 0.0f) gain = 0.0f;
        else if (gain > 1.0f) gain = 1.0f;
        alsaGain_.store(gain, std::memory_order_relaxed);
    }
    void resetPeaks() { cbUsMax_.store(0); cbLoadMax_.store(0); }

    // MIDI -> audio latency probe: the time a note-on was received (CLOCK_MONOTONIC
    // ns, any thread). The next callback, which applies it, measures the wait.
    void noteReceived(int64_t ns) {
        int64_t z = 0;
        noteStampNs_.compare_exchange_strong(z, ns, std::memory_order_relaxed);
    }
    // Java MidiManager hop (sender timestamp -> receiver), reported by MidiInput.kt.
    void midiHop(int64_t ns) {
        hopSumUs_.fetch_add(ns / 1000, std::memory_order_relaxed);
        hopCount_.fetch_add(1, std::memory_order_relaxed);
        int64_t m = hopMaxUs_.load(std::memory_order_relaxed);
        while (ns / 1000 > m && !hopMaxUs_.compare_exchange_weak(m, ns / 1000, std::memory_order_relaxed)) {}
    }
    static int64_t nowNs();

    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream, void* audioData, int32_t numFrames) override;
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override;

    // Audio input (microphone / USB input) for the vocoder and the looper.
    // Needs the RECORD_AUDIO permission. Returns false if no input opened.
    bool setInputEnabled(bool on);
    bool inputEnabled() const { return inputActive_.load(std::memory_order_acquire); }
    float inputPeak() { return inPeak_.exchange(0.0f); }
    float inputPeakPeek() const { return inPeakPeek_.load(std::memory_order_relaxed); }

    // Recording the output to a 32-bit float WAV file (writer thread).
    bool startRecording(const std::string& path);
    double stopRecording();  // seconds written
    bool recording() const { return recording_.load(); }
    double recordedSeconds() const;

private:
    bool openLocked(bool tryAlsa = true);
    void renderStereoBlock(float* left, float* right, int numFrames);
    void recordPeriodStats(int numFrames, float us, int64_t t0ns, int sampleRate);
    void handleAlsaDisconnect(uint64_t gen);
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
    std::mutex statsSnapMutex_;
    AudioStats cachedStats_;
    std::shared_ptr<oboe::AudioStream> stream_;
    std::unique_ptr<AlsaOut> alsa_;
    int deviceId_ = 0;
    bool exclusive_ = true;
    int bufferBursts_ = 0;
    int requestedRate_ = 48000;
    bool alsaDirect_ = false;
    std::atomic<float> alsaGain_{1.0f};
    bool wantRunning_ = false;
    std::atomic<uint64_t> startGen_{0};
    std::atomic<int> restarts_{0};
    std::mutex noteMutex_;
    std::string backendNote_;

    // input
    struct InputCallback : oboe::AudioStreamDataCallback {
        AudioEngine* owner = nullptr;
        oboe::DataCallbackResult onAudioReady(oboe::AudioStream* s, void* data, int32_t n) override;
    } inCb_;
    bool openInputLocked();
    std::shared_ptr<oboe::AudioStream> inStream_;
    bool wantInput_ = false;
    FloatRing inRing_{48000};
    std::atomic<bool> inputActive_{false};
    std::atomic<uint32_t> inputGeneration_{0};
    uint32_t seenInputGeneration_ = 0;  // output callback only
    std::atomic<float> inPeak_{0.0f}, inPeakPeek_{0.0f};
    float inL_[hsynth::kMaxBlockSize];
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

    // periodic logcat line (tag HSynth, every 5 s while the stream runs)
    void statsLoop();
    std::thread statsThread_;
    std::mutex statsMutex_;
    std::condition_variable statsCv_;
    bool statsStop_ = false;
    // windowed callback figures (reset by the stats thread)
    std::atomic<float> winUsMax_{0};
    std::atomic<int64_t> winGapMaxNs_{0};      // longest time between two callbacks
    std::atomic<int> winOverBudget_{0};        // callbacks longer than their period
    std::atomic<int> winSlow_{0};              // ... longer than half of it
    std::atomic<int64_t> lastCbNs_{0};
    std::atomic<int64_t> callbacks_{0};
    // MIDI -> callback wait
    std::atomic<int64_t> noteStampNs_{0};
    std::atomic<int64_t> waitSumUs_{0}, waitMaxUs_{0};
    std::atomic<int> waitCount_{0};
    std::atomic<int64_t> hopSumUs_{0}, hopMaxUs_{0};
    std::atomic<int> hopCount_{0};
    // automatic buffer: xrun-driven growth from minBursts_ (callback thread only)
    int minBursts_ = 0;
    void autoBuffer(oboe::AudioStream* stream);
    int tuneXruns_ = -1;
    int tuneCountdown_ = 0;
    int64_t tuneCleanSince_ = 0, tuneShrunkAt_ = 0;
    int64_t tuneHoldNs_ = 60000000000LL;  // 60 s clean before trying one burst less
    std::atomic<int> bufferGrown_{0}, bufferShrunk_{0};
    std::atomic<bool> named_{false};
};
