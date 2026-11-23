#include "hsynth/stft.h"

#include "hsynth/fileio.h"
#include "pcm_file.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>

#if defined(HSYNTH_SIMD) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace hsynth {
namespace {

constexpr int kRate = 44100, kFft = 4096, kHop = 1024, kBins = 1024, kSpectrum = kFft / 2 + 1;
constexpr int kStems = 4, kOther = 3;
constexpr double kTau = 6.28318530717958647692;
constexpr std::array<const char*, kStems> kNames{"vocals", "drums", "bass", "other"};
using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
using Complex = std::complex<float>;
using FftBuffer = std::array<Complex, kFft>;
using Magnitudes = std::array<float, kBins * 2>;
using StemMasks = std::array<Magnitudes, kStems>;

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
              "Spectrogram interchange requires IEEE float32");

File openFile(const std::string& path, const char* mode) {
    return File(fopenUtf8(path, mode), &std::fclose);
}

Json failure(const std::string& error) {
    Json reply = Json::object();
    reply.set("ok", false);
    reply.set("error", error);
    return reply;
}

// Bounded input cache and a polyphase low-pass for sample rate conversion.
class Input {
public:
    bool open(const std::string& path, std::string& error) {
        if (!file_.open(path, error)) return false;
        const auto& in = file_.info();
        if (in.sr <= 0 || in.frames <= 0) {
            error = "Invalid input audio sample rate or frame count";
            return false;
        }
        step_ = double(in.sr) / kRate;
        samples_ = std::max<int64_t>(1, std::llround(in.frames / step_));
        if (in.sr == kRate) return true;

        const double cutoff = 0.94 / std::max(step_, 1.0);
        for (int phase = 0; phase <= kPhases; ++phase) {
            double sum = 0;
            for (int tap = 0; tap < kTaps; ++tap) {
                const double x = tap - kTaps / 2 + 1 - double(phase) / kPhases;
                const double theta = 0.5 * kTau * cutoff * x;
                const double sinc = x == 0 ? cutoff : std::sin(theta) / (0.5 * kTau * x);
                const double window = 0.5 + 0.5 * std::cos(kTau * x / kTaps);
                kernel_[phase][tap] = float(sinc * window);
                sum += kernel_[phase][tap];
            }
            for (auto& v : kernel_[phase]) v /= float(sum);
        }
        return true;
    }

    int64_t samples() const { return samples_; }
    // Match Spleeter's one FFT window of leading zeros and pad_end=True.
    int64_t frames() const { return kFft / kHop + (samples_ + kHop - 1) / kHop; }
    bool good() const { return !failed_; }

    Complex next() {
        if (cursor_ >= samples_) return {};
        const double position = cursor_++ * step_;
        if (step_ == 1) {
            const auto i = int64_t(position);
            if (!cache(i, i)) return {};
            return {left_[i - start_], right_[i - start_]};
        }

        const int64_t index = int64_t(position);
        const int phase = int(std::lround((position - index) * kPhases));
        const auto first = index - kTaps / 2 + 1;
        const auto from = std::max<int64_t>(0, first);
        const auto to = std::min(file_.info().frames - 1, first + kTaps - 1);
        if (to < from || !cache(from, to)) return {};
        float l = 0, r = 0;
        for (auto i = from; i <= to; ++i) {
            const float w = kernel_[phase][i - first];
            l += w * left_[i - start_];
            r += w * right_[i - start_];
        }
        return {l, r};
    }

private:
    bool cache(int64_t first, int64_t last) {
        if (failed_) return false;
        if (first >= start_ && last < start_ + count_) return true;
        start_ = first;
        const int want = int(std::min<int64_t>(left_.size(), file_.info().frames - first));
        count_ = file_.read(first, want, left_.data(), right_.data());
        failed_ = count_ != want;
        for (int i = 0; i < count_; ++i) {
            left_[i] = sane(left_[i]);
            right_[i] = sane(right_[i]);
        }
        return !failed_;
    }

    static float sane(float value) { return std::isfinite(value) ? std::clamp(value, -8.0f, 8.0f) : 0.0f; }
    static constexpr int kPhases = 256, kTaps = 32;
    PcmFile file_;
    int64_t samples_ = 0, cursor_ = 0, start_ = 0;
    int count_ = 0;
    double step_ = 1;
    bool failed_ = false;
    std::array<float, 8192> left_{}, right_{};
    std::array<std::array<float, kTaps>, kPhases + 1> kernel_{};
};

class Fft {
public:
    Fft() {
        for (int i = 1; i < kFft; ++i)
            reverse_[i] = (reverse_[i >> 1] >> 1) | ((i & 1) * (kFft / 2));
        for (int i = 0; i < kFft; ++i) window[i] = float(0.5 - 0.5 * std::cos(kTau * i / kFft));
        for (int half = 1; half < kFft; half *= 2) {
            for (int j = 0; j < half; ++j) {
                const auto angle = kTau * j / (2 * half);
                twiddle_[half + j] = {float(std::cos(angle)), -float(std::sin(angle))};
            }
        }
    }

    template<bool Inverse>
    void transform(FftBuffer& data) const {
        for (int i = 1; i < kFft; ++i)
            if (i < reverse_[i]) std::swap(data[i], data[reverse_[i]]);
        for (int half = 1; half < kFft; half *= 2) {
            const auto* weights = twiddle_.data() + half;
            for (int base = 0; base < kFft; base += 2 * half) {
                auto* lower = data.data() + base;
                auto* upper = lower + half;
                int j = 0;
#if defined(HSYNTH_SIMD) && defined(__ARM_NEON)
                for (; j + 4 <= half; j += 4) {
                    const auto w = vld2q_f32(reinterpret_cast<const float*>(weights + j));
                    const auto z = vld2q_f32(reinterpret_cast<const float*>(upper + j));
                    const auto u = vld2q_f32(reinterpret_cast<const float*>(lower + j));
                    const auto wi = Inverse ? vnegq_f32(w.val[1]) : w.val[1];
                    const auto vr = vsubq_f32(vmulq_f32(w.val[0], z.val[0]), vmulq_f32(wi, z.val[1]));
                    const auto vi = vaddq_f32(vmulq_f32(w.val[0], z.val[1]), vmulq_f32(wi, z.val[0]));
                    const float32x4x2_t lo{vaddq_f32(u.val[0], vr), vaddq_f32(u.val[1], vi)};
                    const float32x4x2_t hi{vsubq_f32(u.val[0], vr), vsubq_f32(u.val[1], vi)};
                    vst2q_f32(reinterpret_cast<float*>(lower + j), lo);
                    vst2q_f32(reinterpret_cast<float*>(upper + j), hi);
                }
#endif
                for (; j < half; ++j) {
                    const auto w = weights[j], z = upper[j];
                    const float wi = Inverse ? -w.imag() : w.imag();
                    const Complex v{w.real()*z.real() - wi*z.imag(), w.real()*z.imag() + wi*z.real()};
                    const auto u = lower[j];
                    lower[j] = u + v;
                    upper[j] = u - v;
                }
            }
        }
    }

    std::array<float, kFft> window{};

private:
    std::array<int, kFft> reverse_{};
    std::array<Complex, kFft> twiddle_{};
};

struct Spectrum {
    std::array<Complex, kSpectrum> left{}, right{};
    std::array<Complex, kHop> original{};
    std::array<float, kBins> magnitude{};
};

class Analysis {
public:
    explicit Analysis(Input& input) : input_(input) {}

    void analyze(int64_t frame, Spectrum& result, bool monoMagnitude) {
        if (frame != 0) {
            std::move(time_.begin() + kHop, time_.end(), time_.begin());
            for (int i = kFft - kHop; i < kFft; ++i) time_[i] = input_.next();
        }
        std::copy_n(time_.begin(), kHop, result.original.begin());
        for (int i = 0; i < kFft; ++i) buffer_[i] = time_[i] * fft.window[i];
        fft.transform<false>(buffer_);

        // A single complex FFT carries both real-valued channels.
        for (int i = 0; i < kSpectrum; ++i) {
            const auto a = buffer_[i];
            const auto b = std::conj(buffer_[(kFft - i) % kFft]);
            result.left[i] = (a + b) * 0.5f;
            const auto d = a - b;
            result.right[i] = {d.imag() * 0.5f, -d.real() * 0.5f};
            if (monoMagnitude && i < kBins)
                result.magnitude[i] = std::sqrt(std::norm(result.left[i]) + std::norm(result.right[i]));
        }
    }

    Fft fft;

private:
    Input& input_;
    FftBuffer time_{}, buffer_{};
};

class MaskFiles {
public:
    bool open(const std::string paths[kStems], int64_t maxFrames, std::string& error) {
        constexpr auto stride = int64_t(sizeof(Magnitudes));
        for (int stem = 0; stem < kStems; ++stem) {
            files_[stem] = openFile(paths[stem], "rb");
            auto* file = files_[stem].get();
            if (!file || !seekFile(file, 0, SEEK_END)) {
                error = "Cannot read mask: " + paths[stem];
                return false;
            }
            const auto size = tellFile(file);
            const auto count = size / stride;
            if (size <= 0 || size % stride || count > maxFrames || (stem && count != frames_) ||
                !seekFile(file, 0)) {
                error = "Masks must have four equal packed [frame, 1024, 2] float32 shapes within the input length";
                return false;
            }
            frames_ = count;
        }
        return true;
    }

    bool read(int64_t frame, StemMasks& masks) {
        if (frame >= frames_) {
            masks = {};
            return true;
        }
        for (int stem = 0; stem < kStems; ++stem) {
            auto& m = masks[stem];
            if (std::fread(m.data(), sizeof(float), m.size(), files_[stem].get()) != m.size()) return false;
            for (auto& v : m) {
                if (!std::isfinite(v) || v < 0) return false;
            }
        }
        return true;
    }

private:
    std::array<File, kStems> files_{{
        {nullptr, &std::fclose}, {nullptr, &std::fclose}, {nullptr, &std::fclose}, {nullptr, &std::fclose}
    }};
    int64_t frames_ = 0;
};

inline float median5(float a, float b, float c, float d, float e) {
    const auto sort = [](float& x, float& y) {
        const float lo = std::min(x, y);
        y = std::max(x, y);
        x = lo;
    };
    sort(a, b); sort(d, e); sort(a, d); sort(b, e);
    sort(b, c); sort(c, d); sort(b, c);
    return c;
}

float smooth(float lo, float hi, float x) {
    const float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
    return t*t*(3 - 2*t);
}

class SpectralMasks {
public:
    SpectralMasks() {
        for (int bin = 0; bin < kBins; ++bin) {
            const float hz = float(bin * kRate) / kFft;
            bass_[bin] = 1 - smooth(120, 220, hz);
            vocals_[bin] = smooth(230, 360, hz) * (1 - smooth(4100, 5200, hz));
        }
    }

    static constexpr int kRadius = 2, kHistory = 2 * kRadius + 1;
    using History = std::array<Spectrum, kHistory>;

    void compute(int64_t frame, int64_t frames, const History& history, StemMasks& masks) const {
        const auto& s = history[frame % kHistory];
        const auto magnitude = [&](int delta) -> const auto& {
            return history[std::clamp<int64_t>(frame + delta, 0, frames - 1) % kHistory].magnitude;
        };
        const auto& time0 = magnitude(-2);
        const auto& time1 = magnitude(-1);
        const auto& time3 = magnitude(1);
        const auto& time4 = magnitude(2);
        for (int bin = 0; bin < kBins; ++bin) {
            const auto& m = s.magnitude;
            const float h = median5(time0[bin], time1[bin], m[bin], time3[bin], time4[bin]);
            const float p = median5(m[std::max(0, bin - 6)], m[std::max(0, bin - 3)], m[bin],
                                    m[std::min(kBins - 1, bin + 3)], m[std::min(kBins - 1, bin + 6)]);
            const float mid = std::norm(s.left[bin] + s.right[bin]);
            const float side = std::norm(s.left[bin] - s.right[bin]);
            const float center = smooth(0.57f, 0.98f, mid / (mid + side + 1e-12f));
            const float bass = bass_[bin] * center;
            const float vocal = vocals_[bin] * center;
            // Competing harmonic and percussive magnitudes. Leave a floor
            // for stereo keys and for bins where both medians are near zero.
            const float harmonic = std::max(h, 0.15f * m[bin]);
            const float estimates[] = {
                harmonic * vocal,
                p * 1.4f * (1 - 0.75f * bass),
                harmonic * bass * 1.8f,
                harmonic * std::max(0.12f, 1 - std::max(bass, vocal))
            };
            for (int stem = 0; stem < kStems; ++stem)
                masks[stem][2*bin] = masks[stem][2*bin + 1] = estimates[stem];
        }
    }

private:
    std::array<float, kBins> bass_{}, vocals_{};
};

std::filesystem::path temporarySibling(std::filesystem::path path) {
    static std::atomic<uint64_t> counter{0};
    path += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
        + "-" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    return path;
}

void removeTemporary(const std::filesystem::path& path) {
    std::error_code ignored;
    if (!path.empty()) std::filesystem::remove(path, ignored);
}

// Write through temporary sibling files, so validation and I/O failures do
// not expose partial WAV files under the advertised stem names.
class StemOutput {
public:
    ~StemOutput() {
        for (int stem = 0; stem < kStems; ++stem) {
            if (files_[stem]) std::fclose(files_[stem]);
            removeTemporary(temporary_[stem]);
        }
    }

    bool open(const std::string& dir, const std::string& inputPath, int64_t samples, std::string& error) {
        if (samples > (int64_t(UINT32_MAX) - 36) / 4) {
            error = "Stem audio exceeds the RIFF/WAV size limit";
            return false;
        }
        if (dir.empty()) {
            error = "A stem output directory is required";
            return false;
        }
        const auto directory = utf8Path(dir);
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            error = "Cannot create stem directory: " + dir;
            return false;
        }
        for (int stem = 0; stem < kStems; ++stem) {
            paths_[stem] = directory / (std::string(kNames[stem]) + ".wav");
            if (std::filesystem::equivalent(paths_[stem], utf8Path(inputPath), ec)) {
                error = "A stem output would overwrite the source audio";
                return false;
            }
            temporary_[stem] = temporarySibling(paths_[stem]);
            auto*& file = files_[stem];
            file = fopenUtf8(utf8String(temporary_[stem]), "wb");
            if (!file || !header(file, uint32_t(samples * 4))) {
                error = "Cannot create stem WAV: " + utf8String(paths_[stem]);
                return false;
            }
        }
        return true;
    }

    bool write(int stem, const Complex* data, int count) {
        for (int i = 0; i < count; ++i) {
            const auto encode = [](float x) { return int16_t(std::lrint(std::clamp(x, -1.0f, 32767.0f / 32768) * 32768)); };
            pcm_[2*i] = encode(data[i].real());
            pcm_[2*i + 1] = encode(data[i].imag());
        }
        return std::fwrite(pcm_.data(), sizeof(int16_t) * 2, count, files_[stem]) == size_t(count);
    }

    Json finish() {
        for (auto*& file : files_) {
            const int rc = std::fclose(file);
            file = nullptr;
            if (rc != 0) return failure("Could not finish writing stem WAVs");
        }
        // Serialize publication so overlapping requests cannot mix stems
        // from two different separations. Keep the old set for rollback.
        static std::mutex publish;
        std::lock_guard<std::mutex> lock(publish);
        std::array<std::filesystem::path, kStems> backups{};
        int published = 0;
        const auto rollback = [&](int stem) {
            for (int i = 0; i < published; ++i) removeTemporary(paths_[i]);
            for (int i = 0; i < kStems; ++i) {
                std::error_code ignored;
                if (!backups[i].empty()) std::filesystem::rename(backups[i], paths_[i], ignored);
            }
            return failure("Could not publish stem WAV: " + utf8String(paths_[stem]));
        };
        for (int stem = 0; stem < kStems; ++stem) {
            std::error_code error;
            const auto type = std::filesystem::symlink_status(paths_[stem], error).type();
            if (type == std::filesystem::file_type::not_found) continue;
            if (error || type != std::filesystem::file_type::regular) return rollback(stem);
            auto backup = temporarySibling(paths_[stem]);
            std::filesystem::rename(paths_[stem], backup, error);
            if (error) return rollback(stem);
            backups[stem] = std::move(backup);
        }
        for (; published < kStems; ++published) {
            std::error_code error;
            std::filesystem::rename(temporary_[published], paths_[published], error);
            if (error) return rollback(published);
            temporary_[published].clear();
        }
        Json reply = Json::object();
        reply.set("ok", true);
        for (int stem = 0; stem < kStems; ++stem) {
            removeTemporary(backups[stem]);
            reply.set(kNames[stem], utf8String(paths_[stem]));
        }
        return reply;
    }

private:
    static bool header(FILE* file, uint32_t bytes) {
        // Supported hosts are little-endian.
        const uint32_t riffSize = bytes + 36;
        const uint32_t fmt[] = {16, 0x00020001, kRate, kRate * 4, 0x00100004};
        return std::fwrite("RIFF", 4, 1, file) == 1 && std::fwrite(&riffSize, 4, 1, file) == 1 &&
            std::fwrite("WAVEfmt ", 8, 1, file) == 1 && std::fwrite(fmt, sizeof(fmt), 1, file) == 1 &&
            std::fwrite("data", 4, 1, file) == 1 && std::fwrite(&bytes, 4, 1, file) == 1;
    }

    std::array<FILE*, kStems> files_{};
    std::array<std::filesystem::path, kStems> paths_{}, temporary_{};
    std::array<int16_t, kHop * 2> pcm_{};
};

class Synthesis {
public:
    Synthesis(const Fft& fft, StemOutput& output) : fft_(fft), output_(output) {}

    bool render(int64_t frame, int64_t samples, const Spectrum& in, const StemMasks& masks) {
        const auto start = frame * kHop - kFft;
        const int count = start < 0 ? 0 : int(std::min<int64_t>(kHop, samples - start));
        const int offset = int(frame % (kFft / kHop)) * kHop;

        // Compute the Wiener denominator once per channel/bin. Remaining
        // energy, including bins above the model band, goes to Other.
        for (int i = 0; i < kBins * 2; ++i) {
            double power[kStems], total = 1e-12;
            for (int stem = 0; stem < kStems; ++stem) {
                const double v = masks[stem][i];
                power[stem] = v * v;
                total += power[stem];
            }
            for (int stem = 0; stem < kOther; ++stem) weights_[stem][i] = float(power[stem] / total);
        }

        auto residual = in.original;
        for (int stem = 0; stem < kOther; ++stem) {
            buffer_.fill({});
            for (int i = 0; i < kBins; ++i) {
                const auto l = in.left[i] * weights_[stem][2*i];
                const auto r = in.right[i] * weights_[stem][2*i + 1];
                buffer_[i] = l + Complex(-r.imag(), r.real());
                if (i != 0) buffer_[kFft - i] = std::conj(l) + Complex(r.imag(), r.real());
            }
            fft_.transform<true>(buffer_);
            auto& sum = overlap_[stem];
            // Four staggered periodic Hann windows sum to 1.5 when squared.
            constexpr float scale = 1.0f / (1.5f * kFft);
            for (int block = 0; block < kFft; block += kHop) {
                auto* dest = sum.data() + (offset + block) % kFft;
                for (int i = 0; i < kHop; ++i)
                    dest[i] += buffer_[block + i] * (fft_.window[block + i] * scale);
            }
            if (!output_.write(stem, sum.data() + offset, count)) return false;
            for (int i = 0; i < count; ++i) residual[i] -= sum[offset + i];
            std::fill_n(sum.begin() + offset, kHop, Complex{});
        }
        return output_.write(kOther, residual.data(), count);
    }

private:
    const Fft& fft_;
    StemOutput& output_;
    std::array<Magnitudes, kOther> weights_{};
    std::array<FftBuffer, kOther> overlap_{};
    FftBuffer buffer_{};
};

Json reconstruct(const std::string& path, const std::string* masks, const std::string& dir) {
    // Put bounded DSP workspaces on the worker heap, including on JNI threads
    // whose stack size can be smaller than the total scratch space.
    const auto input = std::make_unique<Input>();
    std::string error;
    if (!input->open(path, error)) return failure(error);
    MaskFiles model;
    if (masks && !model.open(masks, input->frames(), error)) return failure(error);
    StemOutput output;
    if (!output.open(dir, path, input->samples(), error)) return failure(error);
    const auto analysis = std::make_unique<Analysis>(*input);
    const auto synthesis = std::make_unique<Synthesis>(analysis->fft, output);
    const auto history = std::make_unique<SpectralMasks::History>();
    StemMasks weights{};
    SpectralMasks fast;
    int64_t next = 0;
    for (int64_t frame = 0; frame < input->frames(); ++frame) {
        const auto lookahead = masks ? 0 : SpectralMasks::kRadius;
        while (next < input->frames() && next <= frame + lookahead) {
            analysis->analyze(next, (*history)[next % history->size()], !masks);
            ++next;
        }
        if (!input->good()) return failure("Could not decode source audio");
        if (masks) {
            if (!model.read(frame, weights)) return failure("Could not read finite nonnegative mask magnitudes");
        } else {
            fast.compute(frame, input->frames(), *history, weights);
        }
        if (!synthesis->render(frame, input->samples(), (*history)[frame % history->size()], weights))
            return failure("Could not write stem samples");
    }
    return output.finish();
}

}  // namespace

Json stftExtractMag(const std::string& wavPath, const std::string& outMagFloatPath, int maxFrames) {
    const auto input = std::make_unique<Input>();
    std::string error;
    if (!input->open(wavPath, error)) return failure(error);
    std::error_code ec;
    if (std::filesystem::equivalent(utf8Path(wavPath), utf8Path(outMagFloatPath), ec))
        return failure("Magnitude output would overwrite the source audio");
    const auto target = utf8Path(outMagFloatPath);
    struct PendingMagnitude {
        std::filesystem::path path;
        ~PendingMagnitude() { removeTemporary(path); }
    } pending{temporarySibling(target)};
    auto out = openFile(utf8String(pending.path), "wb");
    if (!out) return failure("Cannot create magnitude file: " + outMagFloatPath);
    const auto analysis = std::make_unique<Analysis>(*input);
    const auto spectrum = std::make_unique<Spectrum>();
    Magnitudes mag{};
    const auto limit = maxFrames > 0 ? std::min<int64_t>(maxFrames, input->frames()) : input->frames();
    for (int64_t frame = 0; frame < limit; ++frame) {
        analysis->analyze(frame, *spectrum, false);
        if (!input->good()) return failure("Could not decode source audio");
        for (int bin = 0; bin < kBins; ++bin) {
            mag[2*bin] = std::abs(spectrum->left[bin]);
            mag[2*bin + 1] = std::abs(spectrum->right[bin]);
        }
        if (std::fwrite(mag.data(), sizeof(float), mag.size(), out.get()) != mag.size())
            return failure("Could not write magnitude file");
    }
    if (std::fclose(out.release()) != 0) return failure("Could not finish magnitude file");
    std::filesystem::rename(pending.path, target, ec);
    if (ec) return failure("Could not publish magnitude file: " + outMagFloatPath);
    pending.path.clear();
    Json result = Json::object();
    result.set("ok", true);
    result.set("frames", double(limit));
    result.set("bins", kBins);
    result.set("channels", 2);
    result.set("sr", kRate);
    result.set("duration", double(input->samples()) / kRate);
    return result;
}

Json stftReconstructStems(const std::string& wavPath, const std::string maskPaths[4], const std::string& outDir) {
    return reconstruct(wavPath, maskPaths, outDir);
}

Json separateStemsFastSpectral(const std::string& wavPath, const std::string& outDir) {
    return reconstruct(wavPath, nullptr, outDir);
}

}  // namespace hsynth
