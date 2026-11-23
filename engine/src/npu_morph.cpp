#include "hsynth/npu_morph.h"

#include "hsynth/engine.h"
#include "hsynth/fileio.h"
#include "hsynth/params.h"
#include "hsynth/preset.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <vector>

#ifdef __ANDROID__
#include <android/NeuralNetworks.h>
#include <dlfcn.h>
#endif

namespace hsynth {
namespace {

constexpr int kFeatures = 16, kHidden = 64;
constexpr double kPi = 3.14159265358979323846;
constexpr float kAnchorTolerance = 0.001f;
using Features = std::array<float, kFeatures>;
using Hidden = std::array<float, kHidden>;
using Outputs = std::array<float, NpuMorph::kMaxParams>;

Features features(float x, float y) {
    const auto sine = [](double v) { return float(0.5 + 0.5 * std::sin(kPi * v)); };
    const auto cosine = [](double v) { return float(0.5 + 0.5 * std::cos(kPi * v)); };
    Features result{1, x, y, x*x, y*y, x*y, sine(x), cosine(x), sine(y), cosine(y)};
    for (int i = 0; i < 6; ++i) {
        const float dx = x - 0.5f * (i % 3), dy = y - float(i / 3);
        result[10 + i] = std::exp(-5.0f * (dx*dx + dy*dy));
    }
    return result;
}

const std::vector<int>& allowedParams() {
    static const auto ids = [] {
        std::vector<int> result;
        for (int id = 0; id < P::kPartParams; ++id) {
            if (!isPartParam(id) || paramTable()[id].kind != ParamKind::Float) continue;
            const bool voice = (id >= P::FilterCutoff && id <= P::Lfo2Rate) || id == P::FilterMorph ||
                (id >= P::VfxAmount && id <= P::VfxMix) || id == P::Glide ||
                (id >= P::UnisonDetune && id <= P::Env3Release) || id == P::VelCurve ||
                (id >= P::Macro1 && id <= P::SendB) || (id >= P::Filter2Cutoff && id <= P::FilterBalance) ||
                (id >= P::VoiceVintage && id <= P::VoicePanSpread) ||
                (id >= P::PianoPedalNoise && id <= P::PianoHalfDamp);
            if (voice || (id >= P::kLayerBase && id < P::kInsBase)) result.push_back(id);
        }
        return result;
    }();
    return ids;
}

constexpr std::array<int, 32> kDefaultParams{
    P::FilterCutoff, P::FilterReso, P::FilterDrive, P::FilterEnvAmt,
    P::Filter2Cutoff, P::Filter2Reso, P::FilterBalance, P::VfxAmount, P::VfxMix,
    P::AmpAttack, P::AmpDecay, P::AmpSustain, P::AmpRelease,
    P::Env2Attack, P::Env2Release, P::Env3Attack, P::Env3Release,
    P::Lfo1Rate, P::Lfo2Rate, P::Lfo3Rate,
    P::layer(0, P::VaPw), P::layer(0, P::VaDetune), P::layer(0, P::VaMix),
    P::layer(0, P::FmIndex), P::layer(0, P::WtPos), P::layer(0, P::PhysBright),
    P::PianoLid, P::PianoDynComp, P::Macro1, P::Macro2, P::Macro3, P::Macro4
};

float normalize(int id, float value) {
    const auto& p = paramTable()[id];
    value = sanitizeParam(id, value);
    if (p.curve == ParamCurve::Exp && p.min > 0)
        return float(std::log(double(value) / p.min) / std::log(double(p.max) / p.min));
    return (value - p.min) / (p.max - p.min);
}

float denormalize(int id, float value) {
    const auto& p = paramTable()[id];
    const double n = std::clamp(value, 0.0f, 1.0f);
    if (p.curve == ParamCurve::Exp && p.min > 0)
        return float(p.min * std::exp(n * std::log(double(p.max) / p.min)));
    return float(p.min + n * (p.max - p.min));
}

struct Anchor {
    bool active = false;
    float x = 0, y = 0;
    std::string name;
    std::vector<float> normParams;
};

struct Network {
    std::array<Features, kHidden> w1{};
    Hidden b1{};
    std::array<Hidden, NpuMorph::kMaxParams> w2{};
    Outputs b2{};
    int count = 0;

    Hidden hidden(const Features& input) const {
        Hidden result{};
        for (int i = 0; i < kHidden; ++i) {
            float v = b1[i];
            for (int j = 0; j < kFeatures; ++j) v += w1[i][j] * input[j];
            result[i] = std::max(v, 0.0f);
        }
        return result;
    }

    void infer(const Features& input, Outputs& out) const {
        const auto h = hidden(input);
        for (int k = 0; k < count; ++k) {
            float v = b2[k];
            for (int i = 0; i < kHidden; ++i) v += w2[k][i] * h[i];
            out[k] = v;
        }
    }
};

using ActiveAnchors = std::vector<const Anchor*>;
using Matrix = std::array<std::array<double, NpuMorph::kAnchors>, NpuMorph::kAnchors>;

bool fit(Network& net, const ActiveAnchors& anchors, const std::vector<int>& indices) {
    const int n = int(anchors.size());
    net = {};
    net.count = int(indices.size());
    for (int k = 0; k < net.count; ++k) {
        double total = 0;
        for (const auto* anchor : anchors) total += anchor->normParams[indices[k]];
        net.b2[k] = float(total / n);
    }
    if (n == 1) return true;

    uint32_t random = 0x71d23b19u;
    const auto coefficient = [&] {
        random = random * 1664525u + 1013904223u;
        return (float(random >> 8) / 16777216.0f - 0.5f) * 2;
    };
    for (int i = 0; i < kHidden - n; ++i) {
        for (auto& w : net.w1[i]) w = coefficient() * 0.5f;
        net.b1[i] = coefficient() * 0.6f;
    }

    // Compact features stabilize the ridge solve for clustered anchors.
    // Evaluate their squared distance in local coordinates via x² and y².
    for (int i = 0; i < n; ++i) {
        const double x = anchors[i]->x, y = anchors[i]->y;
        double nearest2 = 2;
        for (int j = 0; j < n; ++j) {
            if (i == j) continue;
            const double dx = x - anchors[j]->x, dy = y - anchors[j]->y;
            nearest2 = std::min(nearest2, dx*dx + dy*dy);
        }
        if (nearest2 < 1e-14) return false;
        // Avoid cancellation in float32 for nearly coincident pad points.
        const double radius2 = std::max(0.6 * nearest2, 1e-5);
        auto& row = net.w1[kHidden - n + i];
        row[1] = float(2*x / radius2);
        row[2] = float(2*y / radius2);
        row[3] = row[4] = float(-1 / radius2);
        net.b1[kHidden - n + i] = float(1 - (x*x + y*y) / radius2);
    }

    std::array<Hidden, NpuMorph::kAnchors> h{};
    for (int i = 0; i < n; ++i) h[i] = net.hidden(features(anchors[i]->x, anchors[i]->y));

    // Dual ridge regression. Solve the small (H H^T + lambda I) system
    // once via Cholesky and back-project each output onto the hidden units.
    Matrix l{};
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j <= i; ++j) {
            double v = i == j ? 1e-9 : 0;
            for (int k = 0; k < kHidden; ++k) v += double(h[i][k]) * h[j][k];
            for (int k = 0; k < j; ++k) v -= l[i][k] * l[j][k];
            if (i == j && v <= 0) return false;
            l[i][j] = i == j ? std::sqrt(v) : v / l[j][j];
        }
    }
    for (int k = 0; k < net.count; ++k) {
        std::array<double, NpuMorph::kAnchors> a{};
        for (int i = 0; i < n; ++i) {
            double v = anchors[i]->normParams[indices[k]] - net.b2[k];
            for (int j = 0; j < i; ++j) v -= l[i][j] * a[j];
            a[i] = v / l[i][i];
        }
        for (int i = n - 1; i >= 0; --i) {
            double v = a[i];
            for (int j = i + 1; j < n; ++j) v -= l[j][i] * a[j];
            a[i] = v / l[i][i];
        }
        for (int i = 0; i < kHidden; ++i) {
            double v = 0;
            for (int j = 0; j < n; ++j) v += a[j] * h[j][i];
            net.w2[k][i] = float(v);
        }
    }
    return true;
}

template<class Infer>
bool reconstructs(const ActiveAnchors& anchors, const std::vector<int>& indices, Infer infer) {
    for (const auto* anchor : anchors) {
        Outputs out{};
        if (!infer(features(anchor->x, anchor->y), out)) return false;
        for (size_t k = 0; k < indices.size(); ++k) {
            const float want = anchor->normParams[indices[k]];
            if (!std::isfinite(out[k]) || std::abs(std::clamp(out[k], 0.0f, 1.0f) - want) >= kAnchorTolerance)
                return false;
        }
    }
    return true;
}

#ifdef __ANDROID__

// Every NNAPI symbol is late-bound, including the destructors.
#define HSYNTH_NNAPI_FUNCTIONS(X) \
    X(ANeuralNetworksModel_create) X(ANeuralNetworksModel_free) X(ANeuralNetworksModel_addOperand) \
    X(ANeuralNetworksModel_setOperandValue) X(ANeuralNetworksModel_addOperation) \
    X(ANeuralNetworksModel_identifyInputsAndOutputs) X(ANeuralNetworksModel_finish) \
    X(ANeuralNetworksCompilation_create) X(ANeuralNetworksCompilation_setPreference) \
    X(ANeuralNetworksCompilation_finish) X(ANeuralNetworksCompilation_free) \
    X(ANeuralNetworksExecution_create) X(ANeuralNetworksExecution_setInput) X(ANeuralNetworksExecution_setOutput) \
    X(ANeuralNetworksExecution_free)

// Keep the app's API 28 minimum. Explicit function pointer signatures avoid
// referencing unavailable API 29 declarations at build time.
#define HSYNTH_NNAPI_29(X) \
    X(ANeuralNetworks_getDeviceCount, uint32_t*) \
    X(ANeuralNetworks_getDevice, uint32_t, ANeuralNetworksDevice**) \
    X(ANeuralNetworksDevice_getName, const ANeuralNetworksDevice*, const char**) \
    X(ANeuralNetworksDevice_getType, const ANeuralNetworksDevice*, int32_t*) \
    X(ANeuralNetworksCompilation_createForDevices, ANeuralNetworksModel*, const ANeuralNetworksDevice* const*, \
      uint32_t, ANeuralNetworksCompilation**) \
    X(ANeuralNetworksExecution_compute, ANeuralNetworksExecution*)

struct Quantization {
    float scale = 0;
    int zero = 0;

    static Quantization range(float low, float high) {
        if (low == 0 && high == 0) return {1.0f / 127, 0};
        const float scale = std::max((std::max(0.0f, high) - std::min(0.0f, low)) / 255, 1e-7f);
        return {scale, int(std::lround(-std::min(low, 0.0f) / scale))};
    }

    uint8_t encode(float x) const {
        return uint8_t(std::lround(std::clamp(x / scale + zero, 0.0f, 255.0f)));
    }
};

template<class Rows>
Quantization weightQuantization(const Rows& weights, int size) {
    float low = 0, high = 0;
    for (int i = 0; i < size; ++i)
        for (float x : weights[i]) {
            low = std::min(low, x);
            high = std::max(high, x);
        }
    return Quantization::range(low, high);
}

// An 8-bit output is often too coarse to land directly on a patch value.
// Calibrate the small measured errors with compact, smooth spatial corrections.
// Every correction is zero at all other anchors and outside its local radius.
class AnchorCalibration {
public:
    template<class Infer>
    bool fit(const ActiveAnchors& anchors, const std::vector<int>& indices, Infer infer) {
        count_ = int(anchors.size());
        params_ = int(indices.size());
        for (int i = 0; i < count_; ++i) {
            const auto& a = *anchors[i];
            auto& point = points_[i];
            point.x = a.x;
            point.y = a.y;
            point.radius2 = 2.5f;
            for (int j = 0; j < count_; ++j) {
                if (j == i) continue;
                const float dx = a.x - anchors[j]->x, dy = a.y - anchors[j]->y;
                point.radius2 = std::min(point.radius2, 0.65f * (dx*dx + dy*dy));
            }
            Outputs hardware{};
            if (!infer(features(a.x, a.y), hardware)) return false;
            for (int k = 0; k < params_; ++k) {
                point.delta[k] = a.normParams[indices[k]] - hardware[k];
                if (!std::isfinite(point.delta[k]) || std::abs(point.delta[k]) > 0.08f) return false;
            }
        }
        return true;
    }

    void apply(float x, float y, Outputs& out) const {
        for (int i = 0; i < count_; ++i) {
            const auto& p = points_[i];
            const float dx = x - p.x, dy = y - p.y;
            const float t = 1 - (dx*dx + dy*dy) / p.radius2;
            if (t <= 0) continue;
            for (int k = 0; k < params_; ++k) out[k] += t * t * p.delta[k];
        }
    }

private:
    struct Point {
        float x = 0, y = 0, radius2 = 0;
        Outputs delta{};
    };
    int count_ = 0, params_ = 0;
    std::array<Point, NpuMorph::kAnchors> points_{};
};

class Nnapi {
public:
    struct Device {
        ANeuralNetworksDevice* handle;
        std::string name;
        bool edge;
    };

    Nnapi() {
        // Keep the runtime loaded for process lifetime. Its Binder and
        // execution threads can outlive an individual model or engine.
        static void* const library = dlopen("libneuralnetworks.so", RTLD_NOW | RTLD_LOCAL);
        if (!library) return;
#define RESOLVE(name) name = reinterpret_cast<decltype(name)>(dlsym(library, #name)); if (!name) return;
#define RESOLVE_29(name, ...) RESOLVE(name)
        HSYNTH_NNAPI_FUNCTIONS(RESOLVE)
        HSYNTH_NNAPI_29(RESOLVE_29)
#undef RESOLVE_29
#undef RESOLVE
        available_ = true;
    }

    ~Nnapi() { clear(); }

    bool available() const { return available_; }

    std::vector<Device> devices() const {
        uint32_t count = 0;
        std::vector<Device> out;
        if (!available_ || ANeuralNetworks_getDeviceCount(&count) != ANEURALNETWORKS_NO_ERROR) return out;
        for (uint32_t i = 0; i < count; ++i) {
            ANeuralNetworksDevice* device = nullptr;
            const char* name = nullptr;
            int32_t type = 0;
            if (ANeuralNetworks_getDevice(i, &device) != ANEURALNETWORKS_NO_ERROR ||
                ANeuralNetworksDevice_getName(device, &name) != ANEURALNETWORKS_NO_ERROR || !name ||
                ANeuralNetworksDevice_getType(device, &type) != ANEURALNETWORKS_NO_ERROR) continue;
            const std::string label(name);
            const bool edge = label.find("edgetpu") != std::string::npos;
            if (edge || type == ANEURALNETWORKS_DEVICE_ACCELERATOR) out.push_back({device, label, edge});
        }
        return out;
    }

    void clear() {
        if (compilation_) ANeuralNetworksCompilation_free(compilation_);
        if (model_) ANeuralNetworksModel_free(model_);
        compilation_ = nullptr;
        model_ = nullptr;
        constants_.clear();
    }

    bool build(const Network& net, const Device* device, bool quantized) {
        clear();
        if (!available_ || !ok(ANeuralNetworksModel_create(&model_))) return false;
        quantized_ = quantized;
        count_ = net.count;
        nextOperand_ = 0;

        auto outputWeights = net.w2;
        auto outputBias = net.b2;
        const auto w1 = quantized ? weightQuantization(net.w1, kHidden) : Quantization{};
        input_ = quantized ? Quantization::range(0, 1) : Quantization{};
        Quantization middle{};
        output_ = {};
        if (quantized) {
            float maxHidden = 0;
            Outputs ranges{};
            for (int y = 0; y <= 12; ++y) {
                for (int x = 0; x <= 12; ++x) {
                    const auto input = features(x / 12.0f, y / 12.0f);
                    const auto h = net.hidden(input);
                    maxHidden = std::max(maxHidden, *std::max_element(h.begin(), h.end()));
                    Outputs out{};
                    net.infer(input, out);
                    for (int k = 0; k < count_; ++k)
                        ranges[k] = std::max(ranges[k], std::abs(out[k] - net.b2[k]));
                }
            }
            // Decode each output around its own mean and range. Small
            // changes then get the same uint8 resolution as large changes.
            for (int k = 0; k < count_; ++k) {
                offset_[k] = net.b2[k];
                gain_[k] = std::max(1e-6f, ranges[k] * 1.1f);
                for (auto& w : outputWeights[k]) w /= gain_[k];
                outputBias[k] = 0;
            }
            middle = Quantization::range(0, maxHidden * 1.05f);
            output_ = {1.0f / 127, 128};
        }
        const auto w2 = quantized ? weightQuantization(outputWeights, count_) : Quantization{};
        const auto type = quantized ? ANEURALNETWORKS_TENSOR_QUANT8_ASYMM : ANEURALNETWORKS_TENSOR_FLOAT32;
        const uint32_t input = nextOperand_;
        if (!operand(type, {1, kFeatures}, input_) ||
            !layer(input, net.w1, net.b1, kHidden, kFeatures, w1, input_, middle, ANEURALNETWORKS_FUSED_RELU) ||
            !layer(nextOperand_ - 1, outputWeights, outputBias, count_, kHidden, w2, middle, output_,
                   ANEURALNETWORKS_FUSED_NONE))
            return false;

        const uint32_t output = nextOperand_ - 1;
        if (!ok(ANeuralNetworksModel_identifyInputsAndOutputs(model_, 1, &input, 1, &output)) ||
            !ok(ANeuralNetworksModel_finish(model_))) return false;
        const int result = device ?
            ANeuralNetworksCompilation_createForDevices(model_, &device->handle, 1, &compilation_) :
            ANeuralNetworksCompilation_create(model_, &compilation_);
        return ok(result) &&
            ok(ANeuralNetworksCompilation_setPreference(compilation_, ANEURALNETWORKS_PREFER_FAST_SINGLE_ANSWER)) &&
            ok(ANeuralNetworksCompilation_finish(compilation_));
    }

    bool infer(const Features& input, Outputs& output) const {
        ANeuralNetworksExecution* execution = nullptr;
        if (!ok(ANeuralNetworksExecution_create(compilation_, &execution))) return false;
        std::unique_ptr<ANeuralNetworksExecution, decltype(ANeuralNetworksExecution_free)>
            owner(execution, ANeuralNetworksExecution_free);
        if (!quantized_) return run(execution, input.data(), input.size() * sizeof(float),
                                    output.data(), count_ * sizeof(float));

        std::array<uint8_t, kFeatures> input8{};
        std::array<uint8_t, NpuMorph::kMaxParams> output8{};
        for (int i = 0; i < kFeatures; ++i) input8[i] = input_.encode(input[i]);
        if (!run(execution, input8.data(), input8.size(), output8.data(), count_)) return false;
        for (int k = 0; k < count_; ++k)
            output[k] = offset_[k] + gain_[k] * (int(output8[k]) - output_.zero) * output_.scale;
        return true;
    }

private:
    static bool ok(int code) { return code == ANEURALNETWORKS_NO_ERROR; }

    bool run(ANeuralNetworksExecution* exec, const void* input, size_t inputBytes, void* output, size_t outputBytes) const {
        return ok(ANeuralNetworksExecution_setInput(exec, 0, nullptr, input, inputBytes)) &&
            ok(ANeuralNetworksExecution_setOutput(exec, 0, nullptr, output, outputBytes)) &&
            ok(ANeuralNetworksExecution_compute(exec));
    }

    bool operand(int32_t type, std::initializer_list<uint32_t> dims, Quantization q = {},
                 const void* data = nullptr, size_t bytes = 0) {
        const ANeuralNetworksOperandType desc{
            type, uint32_t(dims.size()), dims.size() ? dims.begin() : nullptr, q.scale, q.zero
        };
        if (!ok(ANeuralNetworksModel_addOperand(model_, &desc))) return false;
        if (data) {
            // NNAPI may borrow large constant buffers until Model_finish.
            const auto* begin = static_cast<const uint8_t*>(data);
            const auto& value = constants_.emplace_back(begin, begin + bytes);
            if (!ok(ANeuralNetworksModel_setOperandValue(model_, int32_t(nextOperand_), value.data(), bytes)))
                return false;
        }
        ++nextOperand_;
        return true;
    }

    template<class Rows, class Bias>
    bool layer(uint32_t input, const Rows& weights, const Bias& biases, uint32_t outputs, uint32_t inputs,
               Quantization weightQ, Quantization inputQ, Quantization outputQ, int32_t activation) {
        const uint32_t weight = nextOperand_;
        const uint32_t bias = weight + 1, fuse = bias + 1, output = fuse + 1;
        if (quantized_) {
            std::vector<uint8_t> encoded(size_t(outputs) * inputs);
            std::vector<int32_t> bias32(outputs);
            const Quantization biasQ{inputQ.scale * weightQ.scale, 0};
            for (uint32_t i = 0; i < outputs; ++i) {
                for (uint32_t j = 0; j < inputs; ++j) encoded[i * inputs + j] = weightQ.encode(weights[i][j]);
                const double v = double(biases[i]) / biasQ.scale;
                bias32[i] = int32_t(std::llround(std::clamp(v, -2147483648.0, 2147483647.0)));
            }
            if (!operand(ANEURALNETWORKS_TENSOR_QUANT8_ASYMM, {outputs, inputs}, weightQ, encoded.data(), encoded.size()) ||
                !operand(ANEURALNETWORKS_TENSOR_INT32, {outputs}, biasQ, bias32.data(), bias32.size() * sizeof(int32_t)))
                return false;
        } else {
            std::vector<float> flat;
            flat.reserve(size_t(outputs) * inputs);
            for (uint32_t i = 0; i < outputs; ++i) flat.insert(flat.end(), weights[i].begin(), weights[i].end());
            if (!operand(ANEURALNETWORKS_TENSOR_FLOAT32, {outputs, inputs}, {}, flat.data(), flat.size() * sizeof(float)) ||
                !operand(ANEURALNETWORKS_TENSOR_FLOAT32, {outputs}, {}, biases.data(), outputs * sizeof(float)))
                return false;
        }
        const uint32_t args[] = {input, weight, bias, fuse};
        const auto type = quantized_ ? ANEURALNETWORKS_TENSOR_QUANT8_ASYMM : ANEURALNETWORKS_TENSOR_FLOAT32;
        return operand(ANEURALNETWORKS_INT32, {}, {}, &activation, sizeof(activation)) &&
            operand(type, {1, outputs}, outputQ) &&
            ok(ANeuralNetworksModel_addOperation(model_, ANEURALNETWORKS_FULLY_CONNECTED, 4, args, 1, &output));
    }

#define MEMBER(name) decltype(&::name) name = nullptr;
    HSYNTH_NNAPI_FUNCTIONS(MEMBER)
#undef MEMBER
#define MEMBER_29(name, ...) int (*name)(__VA_ARGS__) = nullptr;
    HSYNTH_NNAPI_29(MEMBER_29)
#undef MEMBER_29
    ANeuralNetworksModel* model_ = nullptr;
    ANeuralNetworksCompilation* compilation_ = nullptr;
    bool available_ = false, quantized_ = false;
    int count_ = 0;
    uint32_t nextOperand_ = 0;
    Quantization input_{}, output_{};
    Outputs offset_{}, gain_{};
    std::vector<std::vector<uint8_t>> constants_;
};
#undef HSYNTH_NNAPI_FUNCTIONS
#undef HSYNTH_NNAPI_29
#endif

bool validAnchor(int slot, float x, float y) {
    return slot >= 0 && slot < NpuMorph::kAnchors && std::isfinite(x) && std::isfinite(y);
}

Json loadResult(bool ok, const std::string& message) {
    Json result = Json::object();
    result.set("ok", ok);
    result.set(ok ? "name" : "error", message);
    return result;
}

}  // namespace

struct NpuMorph::Impl {
    mutable std::mutex mutex;
    std::array<Anchor, kAnchors> anchors{};
    std::vector<int> selected;
    Network network;
    bool compiled = false;
    std::string backend = "cpu", device = "CPU", dtype = "fp32", error;
    float x = 0.5f, y = 0.5f;
    double lastUs = 0;
#ifdef __ANDROID__
    std::unique_ptr<Nnapi> nnapi;
    AnchorCalibration calibration;
#endif

    void invalidate() {
        compiled = false;
        selected.clear();
        lastUs = 0;
        error.clear();
        backend = "cpu";
        device = "CPU";
        dtype = "fp32";
#ifdef __ANDROID__
        if (nnapi) nnapi->clear();
#endif
    }

    Json status() const {
        Json result = Json::object();
        result.set("available", true);
        result.set("backend", backend);
        result.set("device", device);
        result.set("dtype", dtype);
        result.set("compiled", compiled);
        result.set("anchors", int(std::count_if(anchors.begin(), anchors.end(), [](const auto& a) { return a.active; })));
        result.set("params", int(selected.size()));
        result.set("lastUs", lastUs);
        result.set("x", x);
        result.set("y", y);
        result.set("error", error);
        return result;
    }
};

NpuMorph::NpuMorph() : impl_(std::make_unique<Impl>()) {}
NpuMorph::~NpuMorph() = default;

bool NpuMorph::captureAnchor(const Engine& engine, int part, int slot, float x, float y, const std::string& name) {
    if (!validAnchor(slot, x, y) || part < 0 || part >= kNumParts) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Anchor anchor{true, std::clamp(x, 0.0f, 1.0f), std::clamp(y, 0.0f, 1.0f), name, {}};
    for (int id : allowedParams()) anchor.normParams.push_back(normalize(id, engine.getPartParam(part, id)));
    impl_->anchors[slot] = std::move(anchor);
    impl_->invalidate();
    return true;
}

Json NpuMorph::loadPresetAnchor(int slot, float x, float y, const std::string& path, const std::string& libraryRoot) {
    if (!validAnchor(slot, x, y)) return loadResult(false, "Invalid anchor slot or coordinates");
    const auto file = utf8Path(path);
    Preset preset;
    std::string error;
    if (!loadPresetFile(utf8String(file.is_absolute() ? file : utf8Path(libraryRoot) / file), preset, error))
        return loadResult(false, error);
    Anchor anchor{true, std::clamp(x, 0.0f, 1.0f), std::clamp(y, 0.0f, 1.0f), preset.name, {}};
    for (int id : allowedParams()) anchor.normParams.push_back(normalize(id, preset.params[id]));
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->anchors[slot] = std::move(anchor);
    impl_->invalidate();
    return loadResult(true, preset.name);
}

void NpuMorph::removeAnchor(int slot) {
    if (slot < 0 || slot >= kAnchors) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->anchors[slot].active) return;
    impl_->anchors[slot] = {};
    impl_->invalidate();
}

void NpuMorph::clearAnchors() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->anchors = {};
    impl_->invalidate();
}

Json NpuMorph::anchorsJson() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Json result = Json::array();
    for (int i = 0; i < kAnchors; ++i) {
        const auto& a = impl_->anchors[i];
        if (!a.active) continue;
        Json entry = Json::object();
        entry.set("slot", i);
        entry.set("x", a.x);
        entry.set("y", a.y);
        entry.set("name", a.name);
        result.push(std::move(entry));
    }
    return result;
}

Json NpuMorph::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->status();
}

Json NpuMorph::compile() {
    auto& state = *impl_;
    std::lock_guard<std::mutex> lock(state.mutex);
    state.invalidate();

    ActiveAnchors anchors;
    for (const auto& anchor : state.anchors) {
        if (!anchor.active) continue;
        const auto same = std::find_if(anchors.begin(), anchors.end(), [&](const auto* a) {
            return anchor.x == a->x && anchor.y == a->y;
        });
        if (same == anchors.end()) anchors.push_back(&anchor);
        else if ((*same)->normParams != anchor.normParams) {
            state.error = "Different anchors share the same coordinates";
            return state.status();
        }
    }
    if (anchors.empty()) {
        state.error = "Capture or load an anchor first";
        return state.status();
    }

    const auto& ids = allowedParams();
    std::vector<std::pair<float, int>> variable;
    for (int i = 0; i < int(ids.size()); ++i) {
        float low = 1, high = 0;
        for (const auto* a : anchors) {
            low = std::min(low, a->normParams[i]);
            high = std::max(high, a->normParams[i]);
        }
        if (high > low) variable.emplace_back(high - low, i);
    }
    std::stable_sort(variable.begin(), variable.end(), [](auto a, auto b) { return a.first > b.first; });
    std::vector<int> indices;
    const auto add = [&](int i) {
        if (i < int(ids.size()) && indices.size() < kMaxParams &&
            std::find(indices.begin(), indices.end(), i) == indices.end()) indices.push_back(i);
    };
    for (const auto& v : variable) add(v.second);
    if ((anchors.size() <= 2 && indices.size() < kDefaultParams.size()) || indices.empty()) {
        for (int id : kDefaultParams) {
            if (indices.size() >= kDefaultParams.size()) break;
            add(int(std::find(ids.begin(), ids.end(), id) - ids.begin()));
        }
    }

    if (!fit(state.network, anchors, indices) ||
        !reconstructs(anchors, indices, [&](const Features& in, Outputs& out) {
            state.network.infer(in, out);
            return true;
        })) {
        state.error = "Could not fit anchors to the required accuracy; space them farther apart";
        return state.status();
    }
    for (int i : indices) state.selected.push_back(ids[i]);
    state.compiled = true;

#ifdef __ANDROID__
    if (!state.nnapi) state.nnapi = std::make_unique<Nnapi>();
    auto& nnapi = *state.nnapi;
    if (nnapi.available()) {
        const auto devices = nnapi.devices();
        const auto attempt = [&](const Nnapi::Device* dev, bool quantized) {
            if (!nnapi.build(state.network, dev, quantized)) return false;
            const auto infer = [&](const Features& in, Outputs& out) { return nnapi.infer(in, out); };
            if (quantized) {
                if (!state.calibration.fit(anchors, indices, infer)) return false;
                // Check interpolation between anchors as well as their exact
                // values, since the driver may saturate hidden activations.
                for (int y = 0; y <= 4; ++y) {
                    for (int x = 0; x <= 4; ++x) {
                        const float px = x * 0.25f, py = y * 0.25f;
                        const auto in = features(px, py);
                        Outputs reference{}, hardware{};
                        state.network.infer(in, reference);
                        if (!infer(in, hardware)) return false;
                        state.calibration.apply(px, py, hardware);
                        for (int k = 0; k < state.network.count; ++k)
                            if (std::abs(std::clamp(reference[k], 0.0f, 1.0f) -
                                         std::clamp(hardware[k], 0.0f, 1.0f)) > 0.06f) return false;
                    }
                }
            } else if (!reconstructs(anchors, indices, infer)) {
                return false;
            }
            state.backend = dev && dev->edge ? "edgetpu" : "nnapi";
            state.device = dev ? dev->name : "NNAPI runtime";
            state.dtype = quantized ? "uint8" : "fp32";
            return true;
        };
        for (const auto& dev : devices)
            if (dev.edge && attempt(&dev, true)) return state.status();
        for (bool edge : {true, false})
            for (const auto& dev : devices)
                if (dev.edge == edge && attempt(&dev, false)) return state.status();
        if (attempt(nullptr, false)) return state.status();
        nnapi.clear();
        state.error = "NNAPI compilation or anchor validation failed; using CPU";
    } else {
        state.error = "NNAPI is unavailable; using CPU";
    }
#endif
    return state.status();
}

float NpuMorph::evaluate(Engine& engine, int part, float x, float y) {
    auto& state = *impl_;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.compiled || !std::isfinite(x) || !std::isfinite(y) || part < 0 || part >= kNumParts) return -1;
    state.x = std::clamp(x, 0.0f, 1.0f);
    state.y = std::clamp(y, 0.0f, 1.0f);
    const auto input = features(state.x, state.y);
    Outputs out{};
    const auto start = std::chrono::steady_clock::now();
    bool inferred = false;
#ifdef __ANDROID__
    if (state.backend != "cpu") {
        inferred = state.nnapi->infer(input, out) &&
            std::all_of(out.begin(), out.begin() + state.network.count, [](float v) { return std::isfinite(v); });
        if (!inferred) {
            state.nnapi->clear();
            state.backend = "cpu";
            state.device = "CPU";
            state.dtype = "fp32";
            state.error = "NNAPI execution failed; using CPU";
        }
    }
#endif
    if (!inferred) state.network.infer(input, out);
    state.lastUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
#ifdef __ANDROID__
    if (inferred && state.dtype == "uint8") state.calibration.apply(state.x, state.y, out);
#endif
    for (size_t k = 0; k < state.selected.size(); ++k) {
        const auto id = state.selected[k];
        engine.setPartParam(part, id, denormalize(id, out[k]));
    }
    return float(state.lastUs / 1000);
}

}  // namespace hsynth
