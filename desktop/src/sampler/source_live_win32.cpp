// Live source for Windows: sends MIDI through winmm to any MIDI output
// (hardware synth such as a Roland SonicCell, "Microsoft GS Wavetable Synth",
// a virtual cable to a plugin host ...) and records the result with miniaudio
// from a recording device (USB audio of the synth itself or an audio
// interface) or WASAPI loopback (whatever the PC is playing).
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "miniaudio.h"
#include "sources.h"

namespace sampler {

namespace {
void captureCallback(ma_device* dev, void*, const void* input, ma_uint32 frames);

bool containsNoCase(const std::string& hay, const std::string& needle) {
    auto low = [](std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    return low(hay).find(low(needle)) != std::string::npos;
}

void sleepSec(double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); }
}  // namespace

class LiveSource : public Source {
public:
    ~LiveSource() override {
        if (deviceOk_) ma_device_uninit(&device_);
        if (contextOk_) ma_context_uninit(&context_);
        if (midi_) {
            allNotesOff();
            midiOutReset(midi_);
            midiOutClose(midi_);
        }
    }

    bool open(const LiveConfig& cfg, std::string& error) {
        cfg_ = cfg;
        // MIDI output: index or name
        int midiDev = -1;
        const auto outs = listMidiOutputs();
        if (!cfg.midiOut.empty() && std::all_of(cfg.midiOut.begin(), cfg.midiOut.end(), ::isdigit)) midiDev = std::atoi(cfg.midiOut.c_str());
        else
            for (size_t i = 0; i < outs.size(); ++i)
                if (containsNoCase(outs[i], cfg.midiOut)) { midiDev = (int)i; break; }
        if (midiDev < 0 || midiDev >= (int)outs.size()) {
            error = "MIDI output '" + cfg.midiOut + "' not found (see list-midi)";
            return false;
        }
        if (midiOutOpen(&midi_, (UINT)midiDev, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            error = "cannot open MIDI output " + outs[(size_t)midiDev];
            return false;
        }
        midiName_ = outs[(size_t)midiDev];

        ma_backend backends[] = {ma_backend_wasapi};
        if (ma_context_init(backends, 1, nullptr, &context_) != MA_SUCCESS) { error = "WASAPI unavailable"; return false; }
        contextOk_ = true;
        const bool loop = cfg.capture == "loopback";
        dc_ = ma_device_config_init(loop ? ma_device_type_loopback : ma_device_type_capture);
        dc_.capture.format = ma_format_f32;
        dc_.capture.channels = 2;
        dc_.sampleRate = (ma_uint32)cfg.sampleRate;
        dc_.dataCallback = captureCallback;
        dc_.pUserData = this;
        ma_device_info* caps = nullptr;
        ma_uint32 ncap = 0;
        if (!loop && !cfg.captureDevice.empty()) {
            ma_context_get_devices(&context_, nullptr, nullptr, &caps, &ncap);
            bool found = false;
            for (ma_uint32 i = 0; i < ncap; ++i)
                if (containsNoCase(caps[i].name, cfg.captureDevice)) { id_ = caps[i].id; found = true; break; }
            if (!found) { error = "recording device '" + cfg.captureDevice + "' not found (see list-audio)"; return false; }
            dc_.capture.pDeviceID = &id_;
        }
        if (!startDevice(error)) return false;
        captureName_ = device_.capture.name;

        allNotesOff();
        sleepSec(0.3);
        measureNoise();
        return true;
    }

    std::string describe() const {
        char b[512];
        std::snprintf(b, sizeof b, "MIDI out: %s | capture: %s @ %d Hz (native %d Hz, %d ch) | noise floor %.1f dBFS peak",
                      midiName_.c_str(), captureName_.c_str(), sr_, (int)device_.capture.internalSampleRate,
                      (int)device_.capture.internalChannels, 20.0 * std::log10(std::max(noisePeak_, 1e-9f)));
        return b;
    }

    int sampleRate() const override { return sr_; }
    float noisePeak() const override { return noisePeak_; }

    bool reduceLevel() override {
        if (volume_ <= 40) return false;
        volume_ = (int)(volume_ * 0.7);  // about -6 dB on the usual CC7 curve
        send(0xB0 | activeChannel_, 7, volume_);
        sleepSec(0.1);
        return true;
    }

    bool selectProgram(int bank, int program, bool drum, std::string&) override {
        const int ch = drum ? cfg_.drumChannel : cfg_.channel;
        activeChannel_ = ch;
        allNotesOff();
        send(0xB0 | ch, 0, (bank >> 7) & 0x7F);
        send(0xB0 | ch, 32, bank & 0x7F);
        send(0xC0 | ch, program & 0x7F, 0);
        volume_ = cfg_.volume;
        send(0xB0 | ch, 7, volume_);
        send(0xB0 | ch, 11, 127);  // expression
        send(0xB0 | ch, 10, 64);   // pan centre
        send(0xB0 | ch, 1, 0);     // mod wheel
        send(0xE0 | ch, 0, 64);    // pitch bend centre
        if (cfg_.dry) {
            send(0xB0 | ch, 91, 0);  // reverb send
            send(0xB0 | ch, 93, 0);  // chorus send
        }
        sleepSec(0.4);  // patch change and effect re-initialisation
        return true;
    }

    // Old USB audio drivers can stop delivering input after a while: every
    // take is checked against the wall-clock time it took; if frames are
    // missing the capture device is reopened and the note is played again.
    bool record(const NoteRequest& req, Recording& out, std::string& error) override {
        for (int attempt = 0; attempt < 5; ++attempt) {
            const bool ok = recordOnce(req, out, error);
            const double got = (double)out.left.size() / sr_;
            if (ok && got >= 0.95 * wallSec_ - 0.05) return true;
            ++restarts_;
            std::fprintf(stderr, "\n    capture stalled (%.2f of %.2f s received), reopening the recording device\n", got, wallSec_);
            allNotesOff();
            ma_device_uninit(&device_);
            deviceOk_ = false;
            sleepSec(1.0 + attempt);
            if (!startDevice(error)) continue;
            sleepSec(0.5);
        }
        error = "audio capture keeps stalling";
        return false;
    }

    int restarts() const override { return restarts_; }

    bool recordOnce(const NoteRequest& req, Recording& out, std::string& error) {
        sleepSec(0.12);
        const auto wall0 = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bufL_.clear();
            bufR_.clear();
            recording_ = true;
        }
        sleepSec(0.05);  // lead-in (trimmed later)
        send(0x90 | activeChannel_, req.note, req.velocity);
        const auto t0 = std::chrono::steady_clock::now();
        const float floorPeak = std::max(noisePeak_ * 2.0f, 1e-5f);
        const size_t win = (size_t)(0.03 * sr_);
        // hold: until holdSec, or (decaying sounds) until the level fell stopBelowDb under the peak
        float peak = 0;
        size_t scanned = 0;
        for (;;) {
            sleepSec(0.02);
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (el >= req.holdSec) break;
            if (req.stopBelowDb > 0 && el > 0.3) {
                std::lock_guard<std::mutex> lock(mutex_);
                const size_t n = bufL_.size();
                for (; scanned < n; ++scanned) peak = std::max(peak, std::max(std::fabs(bufL_[scanned]), std::fabs(bufR_[scanned])));
                float recent = 0;
                for (size_t i = n > win ? n - win : 0; i < n; ++i) recent = std::max(recent, std::max(std::fabs(bufL_[i]), std::fabs(bufR_[i])));
                const float stop = std::max(floorPeak, peak * (float)std::pow(10.0, -req.stopBelowDb / 20.0));
                if (peak > floorPeak && recent < stop) break;
            }
        }
        send(0x80 | activeChannel_, req.note, 64);
        // release: until the input stays at the noise floor
        const auto relStart = std::chrono::steady_clock::now();
        for (;;) {
            sleepSec(0.03);
            std::lock_guard<std::mutex> lock(mutex_);
            const size_t n = bufL_.size();
            for (; scanned < n; ++scanned) peak = std::max(peak, std::max(std::fabs(bufL_[scanned]), std::fabs(bufR_[scanned])));
            float recent = 0;
            for (size_t i = n > 4 * win ? n - 4 * win : 0; i < n; ++i) recent = std::max(recent, std::max(std::fabs(bufL_[i]), std::fabs(bufR_[i])));
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - relStart).count();
            const float quiet = std::max(floorPeak, peak * 3e-4f);  // -70 dB or the noise floor
            if (el > req.maxReleaseSec || (el > 0.15 && recent < quiet)) break;
        }
        // drain a little so the last callback period is in
        sleepSec(0.03);
        std::lock_guard<std::mutex> lock(mutex_);
        recording_ = false;
        wallSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
        out.left = bufL_;
        out.right = bufR_;
        out.clipped = false;
        for (size_t i = 0; i < out.left.size() && !out.clipped; ++i)
            out.clipped = std::fabs(out.left[i]) >= 0.999f || std::fabs(out.right[i]) >= 0.999f;
        if (out.left.empty()) {
            error = "no audio captured - check the recording device";
            return false;
        }
        return true;
    }

    void captureRaw(double seconds, Recording& out) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bufL_.clear();
            bufR_.clear();
            recording_ = true;
        }
        sleepSec(seconds);
        std::lock_guard<std::mutex> lock(mutex_);
        recording_ = false;
        out.left = bufL_;
        out.right = bufR_;
    }

    void onAudio(const float* in, ma_uint32 frames) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!recording_) return;
        for (ma_uint32 i = 0; i < frames; ++i) {
            bufL_.push_back(in[2 * i]);
            bufR_.push_back(in[2 * i + 1]);
        }
    }

private:
    bool startDevice(std::string& error) {
        if (ma_device_init(&context_, &dc_, &device_) != MA_SUCCESS) {
            error = "cannot open audio capture (" + cfg_.capture + ")";
            return false;
        }
        deviceOk_ = true;
        sr_ = (int)device_.sampleRate;
        ma_device_start(&device_);
        return true;
    }
    void send(int status, int d1, int d2) { midiOutShortMsg(midi_, (DWORD)(status | (d1 << 8) | (d2 << 16))); }
    void allNotesOff() {
        for (int ch = 0; ch < 16; ++ch) {
            send(0xB0 | ch, 64, 0);   // sustain off
            send(0xB0 | ch, 123, 0);  // all notes off
        }
    }
    void measureNoise() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bufL_.clear();
            bufR_.clear();
            recording_ = true;
        }
        sleepSec(0.6);
        std::lock_guard<std::mutex> lock(mutex_);
        recording_ = false;
        noisePeak_ = 0;
        for (size_t i = 0; i < bufL_.size(); ++i) noisePeak_ = std::max(noisePeak_, std::max(std::fabs(bufL_[i]), std::fabs(bufR_[i])));
    }

    LiveConfig cfg_;
    HMIDIOUT midi_ = nullptr;
    std::string midiName_, captureName_;
    ma_context context_{};
    bool contextOk_ = false;
    ma_device device_{};
    ma_device_config dc_{};
    ma_device_id id_{};
    bool deviceOk_ = false;
    double wallSec_ = 0;
    int restarts_ = 0;
    int sr_ = 48000;
    int activeChannel_ = 0, volume_ = 100;
    float noisePeak_ = 0;
    std::mutex mutex_;
    bool recording_ = false;
    std::vector<float> bufL_, bufR_;

    friend std::string describeLiveSource(const Source&);
};

namespace {
void captureCallback(ma_device* dev, void*, const void* input, ma_uint32 frames) {
    static_cast<LiveSource*>(dev->pUserData)->onAudio(static_cast<const float*>(input), frames);
}
}  // namespace

std::string describeLiveSource(const Source& s) {
    auto* l = dynamic_cast<const LiveSource*>(&s);
    return l ? l->describe() : std::string();
}

bool captureRaw(Source& s, double seconds, Recording& out) {
    auto* l = dynamic_cast<LiveSource*>(&s);
    if (!l) return false;
    l->captureRaw(seconds, out);
    return true;
}

std::unique_ptr<Source> makeLiveSource(const LiveConfig& cfg, std::string& error) {
    auto s = std::make_unique<LiveSource>();
    if (!s->open(cfg, error)) return nullptr;
    return s;
}

std::vector<std::string> listMidiOutputs() {
    std::vector<std::string> out;
    const UINT n = midiOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIOUTCAPSW caps{};
        if (midiOutGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, name, sizeof name, nullptr, nullptr);
        out.push_back(name);
    }
    return out;
}

std::vector<std::string> listCaptureDevices() {
    std::vector<std::string> out;
    ma_context ctx;
    ma_backend backends[] = {ma_backend_wasapi};
    if (ma_context_init(backends, 1, nullptr, &ctx) != MA_SUCCESS) return out;
    ma_device_info* caps = nullptr;
    ma_uint32 n = 0;
    if (ma_context_get_devices(&ctx, nullptr, nullptr, &caps, &n) == MA_SUCCESS)
        for (ma_uint32 i = 0; i < n; ++i) out.push_back(caps[i].name);
    ma_context_uninit(&ctx);
    return out;
}

}  // namespace sampler
