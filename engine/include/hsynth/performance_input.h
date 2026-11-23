#pragma once

#include <array>
#include <atomic>
#include <mutex>

namespace hsynth {

class Json;

// Three ownership slots let serialized host writers replace a pending
// configuration without ever modifying the audio thread's current copy.
template <typename T>
class PerformanceMailbox {
public:
    template <typename Edit>
    bool update(Edit edit) {
        std::lock_guard<std::mutex> lock(mutex_);
        T next = host_;
        if (!edit(next)) return false;
        host_ = next;
        slots_[writer_] = next;
        writer_ = middle_.exchange(writer_ | kDirty, std::memory_order_acq_rel) & ~kDirty;
        return true;
    }

    T get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return host_;
    }

    bool sync() {
        if (!(middle_.load(std::memory_order_acquire) & kDirty)) return false;
        reader_ = middle_.exchange(reader_, std::memory_order_acq_rel) & ~kDirty;
        return true;
    }

    const T& audioValue() const { return slots_[reader_]; }

private:
    static constexpr unsigned kDirty = 4;
    mutable std::mutex mutex_;
    T host_{};
    std::array<T, 3> slots_{};
    unsigned writer_ = 0, reader_ = 2;
    std::atomic<unsigned> middle_{1};
};

struct FootswitchBinding {
    int cc = -1;
    int action = 0;
    bool invert = false;

    bool operator==(const FootswitchBinding& other) const {
        return cc == other.cc && action == other.action && invert == other.invert;
    }
};

struct PerformanceInputConfig {
    bool sustainInverted = false;
    float velocityCurve = 0.0f;
    int velocityFixed = 0;
    float aftertouchCurve = 0.0f;
    float expMin = 0.0f, expMax = 1.0f;
    std::array<FootswitchBinding, 4> footswitches{};
};

PerformanceInputConfig sanitizePerformanceInput(PerformanceInputConfig config);
Json performanceInputToJson(const PerformanceInputConfig& config);
bool performanceInputFromJson(const Json& json, PerformanceInputConfig& config);

struct PreparedPerformanceInput {
    static constexpr int kCurveSteps = 256;
    PerformanceInputConfig config;
    std::array<float, kCurveSteps + 1> velocity{}, pressure{};

    void prepareCurves();
    float noteVelocity(float value) const;
    float aftertouch(float value) const;
};

}  // namespace hsynth
