#include "alsa_out.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

// Ensure <sound/asound.h> knows target endianness (Android arm64/x86_64 are little-endian)
#include <sound/asound.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#define LOG_TAG "HSynthAudio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::map<int, std::string> readAsoundCards() {
    std::map<int, std::string> cards;
    FILE* f = std::fopen("/proc/asound/cards", "r");
    if (!f) return cards;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        int card = -1;
        // Format: " 1 [KA13           ]: USB-Audio - FiiO KA13"
        if (std::sscanf(line, " %d [", &card) == 1 && card >= 1) {
            std::string s(line);
            size_t br = s.find("]:");
            std::string name;
            if (br != std::string::npos) {
                name = trim(s.substr(br + 2));
                size_t dash = name.find(" - ");
                if (dash != std::string::npos) name = trim(name.substr(dash + 3));
            }
            if (name.find("aoc-snd-card") != std::string::npos) continue;
            cards[card] = name.empty() ? ("USB Audio " + std::to_string(card)) : name;
        }
    }
    std::fclose(f);
    return cards;
}

// Helpers for raw ALSA snd_pcm_hw_params bitmasks and intervals.
inline snd_mask* hwMask(snd_pcm_hw_params* p, snd_pcm_hw_param_t param) {
    return &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

inline const snd_mask* hwMask(const snd_pcm_hw_params* p, snd_pcm_hw_param_t param) {
    return &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

inline snd_interval* hwInterval(snd_pcm_hw_params* p, snd_pcm_hw_param_t param) {
    return &p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

void maskSetAll(snd_pcm_hw_params* p, snd_pcm_hw_param_t param) {
    std::memset(hwMask(p, param)->bits, 0xff, sizeof(snd_mask::bits));
}

void maskSetSingle(snd_pcm_hw_params* p, snd_pcm_hw_param_t param, unsigned int val) {
    snd_mask* m = hwMask(p, param);
    std::memset(m->bits, 0, sizeof(m->bits));
    if (val < SNDRV_MASK_MAX) m->bits[val / 32] |= (1u << (val % 32));
}

bool maskTest(const snd_pcm_hw_params* p, snd_pcm_hw_param_t param, unsigned int val) {
    if (val >= SNDRV_MASK_MAX) return false;
    return (hwMask(p, param)->bits[val / 32] & (1u << (val % 32))) != 0;
}

void intervalSetRange(snd_pcm_hw_params* p, snd_pcm_hw_param_t param, unsigned int minVal, unsigned int maxVal) {
    snd_interval* it = hwInterval(p, param);
    it->min = minVal;
    it->max = maxVal;
    it->openmin = 0;
    it->openmax = 0;
    it->integer = 0;
    it->empty = 0;
}

void intervalSetExact(snd_pcm_hw_params* p, snd_pcm_hw_param_t param, unsigned int val) {
    snd_interval* it = hwInterval(p, param);
    it->min = val;
    it->max = val;
    it->openmin = 0;
    it->openmax = 0;
    it->integer = 1;
    it->empty = 0;
}

void hwParamsAny(snd_pcm_hw_params* p) {
    std::memset(p, 0, sizeof(*p));
    for (int i = SNDRV_PCM_HW_PARAM_FIRST_MASK; i <= SNDRV_PCM_HW_PARAM_LAST_MASK; ++i)
        maskSetAll(p, (snd_pcm_hw_param_t)i);
    for (int i = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; ++i)
        intervalSetRange(p, (snd_pcm_hw_param_t)i, 0, ~0U);
    p->rmask = ~0U;
    p->cmask = 0;
    p->info = ~0U;
}

int hwRefine(int fd, snd_pcm_hw_params* p) {
    p->rmask = ~0U;
    return ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, p);
}

inline int32_t floatToS32(float x) {
    if (x >= 1.0f) return INT32_MAX;
    if (x <= -1.0f) return INT32_MIN;
    const double d = (double)x * 2147483647.0;
    if (d >= (double)INT32_MAX) return INT32_MAX;
    if (d <= (double)INT32_MIN) return INT32_MIN;
    return (int32_t)std::llrint(d);
}

inline int32_t floatToS24(float x) {
    if (x >= 1.0f) return 8388607;
    if (x <= -1.0f) return -8388608;
    const int32_t v = (int32_t)std::lrintf(x * 8388607.0f);
    return std::max<int32_t>(-8388608, std::min<int32_t>(8388607, v));
}

inline int16_t floatToS16(float x) {
    if (x >= 1.0f) return INT16_MAX;
    if (x <= -1.0f) return INT16_MIN;
    const int32_t v = (int32_t)std::lrintf(x * 32767.0f);
    return (int16_t)std::max<int32_t>(INT16_MIN, std::min<int32_t>(INT16_MAX, v));
}

}  // namespace

int64_t AlsaOut::nowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

const char* AlsaOut::formatName() const {
    switch (format_) {
        case AlsaFormat::FloatLe: return "FLOAT_LE";
        case AlsaFormat::S32Le: return "S32_LE";
        case AlsaFormat::S24_3Le: return "S24_3LE";
        case AlsaFormat::S16Le: return "S16_LE";
        default: return "NONE";
    }
}

std::vector<AlsaDevice> AlsaOut::discoverDevices() {
    std::vector<AlsaDevice> out;
    const auto cards = readAsoundCards();

    if (FILE* f = std::fopen("/proc/asound/pcm", "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), f)) {
            int card = -1, dev = -1;
            // Format: "01-00: USB Audio : USB Audio : playback 1"
            if (std::sscanf(line, "%d-%d:", &card, &dev) == 2 && card >= 1 && dev >= 0) {
                std::string s(line);
                if (s.find("playback") == std::string::npos) continue;
                char path[64];
                std::snprintf(path, sizeof(path), "/dev/snd/pcmC%dD%dp", card, dev);
                if (access(path, F_OK) != 0) continue;
                AlsaDevice d;
                d.card = card;
                d.device = dev;
                d.devPath = path;
                auto it = cards.find(card);
                d.name = (it != cards.end()) ? it->second : ("USB Audio C" + std::to_string(card) + "D" + std::to_string(dev));
                out.push_back(std::move(d));
            }
        }
        std::fclose(f);
    }

    // Fallback: probe /dev/snd/pcmC<card>D<dev>p directly if /proc/asound/pcm was unreadable/empty
    if (out.empty()) {
        for (int card = 1; card <= 7; ++card) {
            for (int dev = 0; dev <= 3; ++dev) {
                char path[64];
                std::snprintf(path, sizeof(path), "/dev/snd/pcmC%dD%dp", card, dev);
                if (access(path, F_OK) == 0) {
                    AlsaDevice d;
                    d.card = card;
                    d.device = dev;
                    d.devPath = path;
                    auto it = cards.find(card);
                    d.name = (it != cards.end()) ? it->second : ("USB Audio C" + std::to_string(card) + "D" + std::to_string(dev));
                    out.push_back(std::move(d));
                }
            }
        }
    }
    return out;
}

bool AlsaOut::start(const Config& cfg, Callbacks cb, std::string& errorOut) {
    stop();

    const auto devices = discoverDevices();
    if (devices.empty()) {
        errorOut = "no external USB ALSA playback device found (card >= 1)";
        return false;
    }

    AlsaDevice chosen = devices.front();
    if (cfg.card >= 1) {
        bool found = false;
        for (const auto& d : devices) {
            if (d.card == cfg.card && (cfg.device < 0 || d.device == cfg.device)) {
                chosen = d;
                found = true;
                break;
            }
        }
        if (!found) {
            errorOut = "requested ALSA card " + std::to_string(cfg.card) + " not found";
            return false;
        }
    }

    // Keep O_NONBLOCK and pace writes via poll(POLLOUT) so USB stalls/disconnects
    // are detected within ~600 ms instead of hanging on ALSA's 10 s kernel timeout.
    int fd = -1;
    int lastErr = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        fd = ::open(chosen.devPath.c_str(), O_RDWR | O_NONBLOCK);
        if (fd >= 0) break;
        lastErr = errno;
        if (lastErr != EBUSY && lastErr != EACCES) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (fd < 0) {
        errorOut = "open(" + chosen.devPath + ") failed: " + std::strerror(lastErr ? lastErr : errno);
        return false;
    }

    int ver = SNDRV_PCM_VERSION;
    ioctl(fd, SNDRV_PCM_IOCTL_USER_PVERSION, &ver);  // best effort for time64/compat

    auto fail = [&](const std::string& msg) -> bool {
        errorOut = msg + " (" + chosen.devPath + ")";
        ::close(fd);
        return false;
    };

    snd_pcm_hw_params hw;
    hwParamsAny(&hw);
    maskSetSingle(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    intervalSetExact(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
    if (hwRefine(fd, &hw) < 0) {
        return fail(std::string("HW_REFINE (2ch interleaved) failed: ") + std::strerror(errno));
    }

    // Negotiate format in priority order: FLOAT_LE -> S32_LE -> S24_3LE -> S16_LE
    struct FmtChoice {
        unsigned int alsaFmt;
        AlsaFormat fmt;
        int sampleBytes;
    };
    static constexpr FmtChoice kFormats[] = {
        {SNDRV_PCM_FORMAT_FLOAT_LE, AlsaFormat::FloatLe, 4},
        {SNDRV_PCM_FORMAT_S32_LE, AlsaFormat::S32Le, 4},
        {SNDRV_PCM_FORMAT_S24_3LE, AlsaFormat::S24_3Le, 3},
        {SNDRV_PCM_FORMAT_S16_LE, AlsaFormat::S16Le, 2},
    };
    AlsaFormat chosenFmt = AlsaFormat::None;
    int sampleBytes = 0;
    for (const auto& fc : kFormats) {
        if (!maskTest(&hw, SNDRV_PCM_HW_PARAM_FORMAT, fc.alsaFmt)) continue;
        snd_pcm_hw_params cand = hw;
        maskSetSingle(&cand, SNDRV_PCM_HW_PARAM_FORMAT, fc.alsaFmt);
        if (hwRefine(fd, &cand) == 0) {
            hw = cand;
            chosenFmt = fc.fmt;
            sampleBytes = fc.sampleBytes;
            break;
        }
    }
    if (chosenFmt == AlsaFormat::None) {
        return fail("no supported stereo sample format (FLOAT_LE/S32_LE/S24_3LE/S16_LE)");
    }

    // Negotiate sample rate: requested first, then nearest standard rate.
    const unsigned int rateCandidates[] = {
        (unsigned int)(cfg.sampleRate > 0 ? cfg.sampleRate : 48000),
        48000u,
        96000u,
        44100u,
        88200u,
        192000u,
    };
    const snd_interval* rateInt = hwInterval(&hw, SNDRV_PCM_HW_PARAM_RATE);
    bool rateOk = false;
    for (unsigned int r : rateCandidates) {
        if (r < rateInt->min || r > rateInt->max) continue;
        snd_pcm_hw_params cand = hw;
        intervalSetExact(&cand, SNDRV_PCM_HW_PARAM_RATE, r);
        if (hwRefine(fd, &cand) == 0) {
            hw = cand;
            rateOk = true;
            break;
        }
    }
    if (!rateOk) {
        const unsigned int clamped = std::max(rateInt->min, std::min(rateInt->max, (unsigned int)cfg.sampleRate));
        snd_pcm_hw_params cand = hw;
        intervalSetExact(&cand, SNDRV_PCM_HW_PARAM_RATE, clamped);
        if (hwRefine(fd, &cand) == 0) {
            hw = cand;
            rateOk = true;
        } else if (hwRefine(fd, &hw) == 0) {
            // Let HW_PARAMS pick a valid rate within hardware range.
            rateOk = true;
        }
    }
    if (!rateOk) {
        return fail(std::string("failed to negotiate sample rate: ") + std::strerror(errno));
    }

    const unsigned int negotiatedRate = hwInterval(&hw, SNDRV_PCM_HW_PARAM_RATE)->min;

    // Target 1 ms period (48 frames @ 48 kHz, 96 frames @ 96 kHz).
    // Auto mode uses 4 periods (4.0 ms total) because Linux snd-usb-audio keeps
    // multiple URBs (~2 ms cadence on xhci-exynos) in flight ahead of hw_ptr;
    // 2 periods (2.0 ms) allows prepare_playback_urb() to read stale buffer data
    // even with xruns=0. Users can still choose 2x/3x/4x/6x/8x explicitly.
    const unsigned int targetPeriod = (negotiatedRate >= 88200u) ? 96u : 48u;
    const unsigned int targetPeriods = (cfg.bufferBursts >= 2) ? (unsigned int)cfg.bufferBursts : 4u;

    const snd_interval* pInt = hwInterval(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
    const unsigned int desiredPeriod = std::max(pInt->min, std::min(pInt->max, targetPeriod));
    {
        snd_pcm_hw_params cand = hw;
        intervalSetExact(&cand, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, desiredPeriod);
        if (hwRefine(fd, &cand) == 0) {
            hw = cand;
        } else {
            cand = hw;
            intervalSetRange(&cand, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, desiredPeriod, pInt->max);
            if (hwRefine(fd, &cand) == 0) {
                hw = cand;
                const unsigned int refinedMin = hwInterval(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min;
                snd_pcm_hw_params exact = hw;
                intervalSetExact(&exact, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, refinedMin);
                if (hwRefine(fd, &exact) == 0) hw = exact;
            }
        }
    }

    const snd_interval* nInt = hwInterval(&hw, SNDRV_PCM_HW_PARAM_PERIODS);
    const unsigned int minPeriodsAllowed = std::max(2u, nInt->min);
    const unsigned int maxPeriodsAllowed = std::max(minPeriodsAllowed, nInt->max);
    const unsigned int desiredPeriods = std::max(minPeriodsAllowed, std::min(maxPeriodsAllowed, targetPeriods));
    {
        snd_pcm_hw_params cand = hw;
        intervalSetExact(&cand, SNDRV_PCM_HW_PARAM_PERIODS, desiredPeriods);
        if (hwRefine(fd, &cand) == 0) {
            hw = cand;
        } else {
            cand = hw;
            intervalSetRange(&cand, SNDRV_PCM_HW_PARAM_PERIODS, desiredPeriods, maxPeriodsAllowed);
            if (hwRefine(fd, &cand) == 0) hw = cand;
        }
    }

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0) {
        return fail(std::string("HW_PARAMS failed: ") + std::strerror(errno));
    }

    const int actualRate = (int)hwInterval(&hw, SNDRV_PCM_HW_PARAM_RATE)->min;
    const int actualPeriod = (int)hwInterval(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min;
    const int actualPeriods = (int)hwInterval(&hw, SNDRV_PCM_HW_PARAM_PERIODS)->min;
    const int actualBufSize = (int)hwInterval(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min;
    const int actualBufCap = (int)hwInterval(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->max;

    if (actualRate <= 0 || actualPeriod <= 0 || actualBufSize <= 0) {
        return fail("invalid negotiated ALSA geometry");
    }

    snd_pcm_sw_params sw{};
    sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sw.tstamp_type = SNDRV_PCM_TSTAMP_TYPE_MONOTONIC;
    sw.proto = SNDRV_PCM_VERSION;
    sw.period_step = 1;
    sw.sleep_min = 0;
    sw.avail_min = (snd_pcm_uframes_t)actualPeriod;
    sw.xfer_align = 1;
    sw.start_threshold = (snd_pcm_uframes_t)actualBufSize;
    sw.stop_threshold = (snd_pcm_uframes_t)actualBufSize;
    sw.silence_threshold = 0;
    sw.silence_size = 0;
    snd_pcm_uframes_t boundary = (snd_pcm_uframes_t)actualBufSize;
    while (boundary > 0 && boundary <= ((snd_pcm_uframes_t)LONG_MAX - (snd_pcm_uframes_t)actualBufSize) / 2)
        boundary *= 2;
    sw.boundary = boundary;

    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
        return fail(std::string("SW_PARAMS failed: ") + std::strerror(errno));
    }

    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        return fail(std::string("PREPARE failed: ") + std::strerror(errno));
    }

    fd_ = fd;
    card_ = chosen.card;
    device_ = chosen.device;
    cardName_ = chosen.name;
    sampleRate_ = actualRate;
    periodSize_ = actualPeriod;
    periods_ = actualPeriods;
    bufferSize_ = actualBufSize;
    bufferCapacity_ = std::max(actualBufSize, actualBufCap);
    format_ = chosenFmt;
    frameBytes_ = 2 * sampleBytes;
    cb_ = std::move(cb);
    externalGain_ = cfg.externalGain;
    smoothedGain_ = externalGain_ ? std::max(0.0f, std::min(1.0f, externalGain_->load(std::memory_order_relaxed))) : 1.0f;

    leftBuf_.assign((size_t)periodSize_, 0.0f);
    rightBuf_.assign((size_t)periodSize_, 0.0f);
    pcmBuf_.assign((size_t)periodSize_ * (size_t)frameBytes_, 0);

    stopRequested_.store(false, std::memory_order_relaxed);
    xruns_.store(0, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);

    LOGI("Direct ALSA opened %s (%s): %d Hz %s period %d x %d (buf %d)",
         cardName_.c_str(), chosen.devPath.c_str(), sampleRate_, formatName(),
         periodSize_, periods_, bufferSize_);

    if (cb_.onPrepared) cb_.onPrepared(sampleRate_, periodSize_);

    thread_ = std::thread([this] { threadLoop(); });
    return true;
}

void AlsaOut::stop() {
    stopRequested_.store(true, std::memory_order_release);
    if (fd_ >= 0) {
        // Wake any writei blocked in the kernel before joining the RT thread.
        ioctl(fd_, SNDRV_PCM_IOCTL_DROP);
    }
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    cb_ = {};
}

void AlsaOut::convertFrames(const float* left, const float* right, int frames, uint8_t* dst) const {
    switch (format_) {
        case AlsaFormat::FloatLe: {
            float* out = reinterpret_cast<float*>(dst);
            for (int i = 0; i < frames; ++i) {
                out[2 * i] = left[i];
                out[2 * i + 1] = right[i];
            }
            break;
        }
        case AlsaFormat::S32Le: {
            int32_t* out = reinterpret_cast<int32_t*>(dst);
            for (int i = 0; i < frames; ++i) {
                out[2 * i] = floatToS32(left[i]);
                out[2 * i + 1] = floatToS32(right[i]);
            }
            break;
        }
        case AlsaFormat::S24_3Le: {
            for (int i = 0; i < frames; ++i) {
                const uint32_t l = (uint32_t)floatToS24(left[i]);
                const uint32_t r = (uint32_t)floatToS24(right[i]);
                uint8_t* p = dst + (size_t)i * 6;
                p[0] = (uint8_t)(l & 0xFFu);
                p[1] = (uint8_t)((l >> 8) & 0xFFu);
                p[2] = (uint8_t)((l >> 16) & 0xFFu);
                p[3] = (uint8_t)(r & 0xFFu);
                p[4] = (uint8_t)((r >> 8) & 0xFFu);
                p[5] = (uint8_t)((r >> 16) & 0xFFu);
            }
            break;
        }
        case AlsaFormat::S16Le: {
            int16_t* out = reinterpret_cast<int16_t*>(dst);
            for (int i = 0; i < frames; ++i) {
                out[2 * i] = floatToS16(left[i]);
                out[2 * i + 1] = floatToS16(right[i]);
            }
            break;
        }
        case AlsaFormat::None:
            break;
    }
}

void AlsaOut::threadLoop() {
    pthread_setname_np(pthread_self(), "hsynth-audio");

    // Attempt SCHED_FIFO priority 2 directly (works when rooted/CAP_SYS_NICE;
    // fails silently otherwise, and 85-audio-tune.sh also promotes hsynth-audio).
    struct sched_param sp{};
    sp.sched_priority = 2;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }

    if (cb_.onThreadStart) cb_.onThreadStart((int)gettid());

    while (!stopRequested_.load(std::memory_order_acquire)) {
        const auto t0 = std::chrono::steady_clock::now();
        const int64_t t0ns = nowNs();

        if (cb_.onRender) cb_.onRender(leftBuf_.data(), rightBuf_.data(), periodSize_);
        // Apply Android hardware-volume gain smoothly (10 ms ramp, click-free)
        if (externalGain_) {
            const float target = std::max(0.0f, std::min(1.0f, externalGain_->load(std::memory_order_relaxed)));
            if (target != 1.0f || smoothedGain_ != 1.0f) {
                const float step = 1.0f / std::max(1.0f, 0.01f * (float)sampleRate_);
                float g = smoothedGain_;
                for (int i = 0; i < periodSize_; ++i) {
                    const float d = target - g;
                    if (d > step) g += step;
                    else if (d < -step) g -= step;
                    else g = target;
                    leftBuf_[i] *= g;
                    rightBuf_[i] *= g;
                }
                smoothedGain_ = g;
            }
        }
        convertFrames(leftBuf_.data(), rightBuf_.data(), periodSize_, pcmBuf_.data());

        const float renderUs = std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - t0).count();
        if (cb_.onPeriodDone) cb_.onPeriodDone(sampleRate_, periodSize_, renderUs, t0ns);

        int written = 0;
        int stallTimeouts = 0;
        while (written < periodSize_ && !stopRequested_.load(std::memory_order_acquire)) {
            struct snd_xferi xfer{};
            xfer.buf = pcmBuf_.data() + (size_t)written * (size_t)frameBytes_;
            xfer.frames = (snd_pcm_uframes_t)(periodSize_ - written);
            xfer.result = 0;

            if (ioctl(fd_, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer) == 0) {
                stallTimeouts = 0;
                if (xfer.result > 0) written += (int)xfer.result;
                else break;
            } else {
                const int err = errno;
                if (err == EINTR) continue;
                if (err == EAGAIN) {
                    struct pollfd pfd{};
                    pfd.fd = fd_;
                    pfd.events = POLLOUT | POLLERR | POLLHUP | POLLNVAL;
                    const int pr = ::poll(&pfd, 1, 200);
                    if (stopRequested_.load(std::memory_order_acquire)) return;
                    if (pr < 0) {
                        if (errno == EINTR) continue;
                        LOGE("Direct ALSA poll error: %s (%d)", std::strerror(errno), errno);
                        running_.store(false, std::memory_order_release);
                        if (cb_.onDisconnected) cb_.onDisconnected();
                        return;
                    }
                    if (pr == 0) {
                        if (++stallTimeouts >= 3) {
                            LOGE("Direct ALSA write stall timeout (600 ms)");
                            running_.store(false, std::memory_order_release);
                            if (cb_.onDisconnected) cb_.onDisconnected();
                            return;
                        }
                    } else if (pfd.revents & (POLLHUP | POLLNVAL)) {
                        LOGE("Direct ALSA device hangup (revents=0x%x)", pfd.revents);
                        running_.store(false, std::memory_order_release);
                        if (cb_.onDisconnected) cb_.onDisconnected();
                        return;
                    }
                    continue;
                }
                if (err == EPIPE) {
                    stallTimeouts = 0;
                    xruns_.fetch_add(1, std::memory_order_relaxed);
                    if (ioctl(fd_, SNDRV_PCM_IOCTL_PREPARE) < 0 && errno == ENODEV) {
                        LOGE("Direct ALSA device disconnected during xrun recovery");
                        running_.store(false, std::memory_order_release);
                        if (!stopRequested_.load(std::memory_order_acquire) && cb_.onDisconnected) cb_.onDisconnected();
                        return;
                    }
                    continue;
                }
                if (err == ESTRPIPE) {
                    stallTimeouts = 0;
                    while (!stopRequested_.load(std::memory_order_acquire) &&
                           ioctl(fd_, SNDRV_PCM_IOCTL_RESUME) < 0 && errno == EAGAIN) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    if (ioctl(fd_, SNDRV_PCM_IOCTL_PREPARE) < 0 && errno == ENODEV) {
                        running_.store(false, std::memory_order_release);
                        if (!stopRequested_.load(std::memory_order_acquire) && cb_.onDisconnected) cb_.onDisconnected();
                        return;
                    }
                    continue;
                }
                if (stopRequested_.load(std::memory_order_acquire)) return;
                LOGE("Direct ALSA write error: %s (%d)", std::strerror(err), err);
                running_.store(false, std::memory_order_release);
                if (cb_.onDisconnected) cb_.onDisconnected();
                return;
            }
        }
    }
}
