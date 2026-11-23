// Direct Linux ALSA USB-DAC PCM output via <sound/asound.h> ioctls.
// Bypasses Android AudioFlinger / AAudio / HAL for external USB Audio Class
// DACs (card >= 1) on rooted devices with SELinux / /dev/snd access.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

struct AlsaDevice {
    int card = -1;
    int device = -1;
    std::string name;     // friendly card name from /proc/asound/cards
    std::string devPath;  // /dev/snd/pcmC<card>D<dev>p
};

enum class AlsaFormat { None = 0, FloatLe, S32Le, S24_3Le, S16Le };

class AlsaOut {
public:
    struct Config {
        int card = -1;          // -1 = auto-pick first external USB playback device (card >= 1)
        int device = -1;
        int sampleRate = 48000; // requested rate (48000 or 96000)
        int bufferBursts = 0;   // 0 = auto (4 periods); >= 2 = requested period count
        const std::atomic<float>* externalGain = nullptr;  // optional lock-free linear output volume (0..1)
    };

    struct Callbacks {
        std::function<void(int sampleRate, int periodSize)> onPrepared;
        std::function<void(int tid)> onThreadStart;
        std::function<void(float* left, float* right, int frames)> onRender;
        std::function<void(int sampleRate, int frames, float renderUs, int64_t t0ns)> onPeriodDone;
        std::function<void()> onDisconnected;
    };

    AlsaOut() = default;
    ~AlsaOut() { stop(); }
    AlsaOut(const AlsaOut&) = delete;
    AlsaOut& operator=(const AlsaOut&) = delete;

    // Scans /proc/asound/cards and /proc/asound/pcm (plus /dev/snd probe fallback)
    // for external USB playback devices (card >= 1, ignoring card 0 aoc-snd-card).
    static std::vector<AlsaDevice> discoverDevices();

    bool start(const Config& cfg, Callbacks cb, std::string& errorOut);
    void stop();

    bool running() const { return running_.load(std::memory_order_relaxed); }
    int card() const { return card_; }
    int device() const { return device_; }
    const std::string& cardName() const { return cardName_; }
    int sampleRate() const { return sampleRate_; }
    int periodSize() const { return periodSize_; }
    int periods() const { return periods_; }
    int bufferSize() const { return bufferSize_; }
    int bufferCapacity() const { return bufferCapacity_; }
    AlsaFormat format() const { return format_; }
    const char* formatName() const;
    int xruns() const { return xruns_.load(std::memory_order_relaxed); }

    static int64_t nowNs();

private:
    void threadLoop();
    void convertFrames(const float* left, const float* right, int frames, uint8_t* dst) const;

    int fd_ = -1;
    int card_ = -1;
    int device_ = -1;
    std::string cardName_;
    int sampleRate_ = 0;
    int periodSize_ = 0;
    int periods_ = 0;
    int bufferSize_ = 0;
    int bufferCapacity_ = 0;
    int frameBytes_ = 0;
    AlsaFormat format_ = AlsaFormat::None;

    Callbacks cb_;
    std::vector<float> leftBuf_;
    std::vector<float> rightBuf_;
    std::vector<uint8_t> pcmBuf_;

    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::atomic<int> xruns_{0};
    const std::atomic<float>* externalGain_ = nullptr;
    float smoothedGain_ = 1.0f;
    std::thread thread_;
};
