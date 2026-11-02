// Android audio output via Oboe. This is the only place that knows about the
// audio API; the DSP engine itself is platform independent.
#pragma once

#include <oboe/Oboe.h>

#include <atomic>
#include <memory>
#include <mutex>

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

private:
    bool openLocked();

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

    float left_[hsynth::kMaxBlockSize];
    float right_[hsynth::kMaxBlockSize];

    std::atomic<float> cbUsAvg_{0}, cbUsMax_{0}, cbLoadMax_{0};
    std::atomic<int> lastFrames_{0};
};
