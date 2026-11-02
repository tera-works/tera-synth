// SoundFont 2 source: renders notes offline with TinySoundFont (MIT).
// Output is dry (SoundFont chorus/reverb sends are not applied), which is what
// we want for sampling.
#define TSF_IMPLEMENTATION
#include "tsf.h"

#include <algorithm>
#include <cmath>

#include "sources.h"

namespace sampler {

class Sf2Source : public Source {
public:
    Sf2Source(tsf* f, int sr) : f_(f), sr_(sr) {}
    ~Sf2Source() override { tsf_close(f_); }

    int sampleRate() const override { return sr_; }

    bool selectProgram(int bank, int program, bool drum, std::string& error) override {
        if (drum) bank = 128;  // SoundFont percussion bank
        if (tsf_get_presetindex(f_, bank, program) < 0) {
            error = "preset " + std::to_string(bank) + ":" + std::to_string(program) + " not found in the SoundFont";
            return false;
        }
        tsf_reset(f_);
        tsf_channel_set_bank_preset(f_, 0, bank, program);
        return true;
    }

    bool record(const NoteRequest& req, Recording& out, std::string&) override {
        const int note = req.note, velocity = req.velocity;
        const double holdSec = req.holdSec, maxReleaseSec = req.maxReleaseSec;
        tsf_note_off_all(f_);
        // flush any remaining voices
        std::vector<float> scratch(512 * 2);
        for (int i = 0; i < 200; ++i) tsf_render_float(f_, scratch.data(), 512, 0);

        out.left.clear();
        out.right.clear();
        auto render = [&](size_t frames, float& blockPeak) {
            std::vector<float> buf(frames * 2);
            tsf_render_float(f_, buf.data(), (int)frames, 0);
            blockPeak = 0;
            for (size_t i = 0; i < frames; ++i) {
                out.left.push_back(buf[2 * i]);
                out.right.push_back(buf[2 * i + 1]);
                blockPeak = std::max(blockPeak, std::max(std::fabs(buf[2 * i]), std::fabs(buf[2 * i + 1])));
            }
        };
        tsf_channel_note_on(f_, 0, note, (float)velocity / 127.0f);
        float bp = 0, peak = 0;
        const size_t holdFrames = (size_t)(holdSec * sr_);
        const float stop = req.stopBelowDb > 0 ? (float)std::pow(10.0, -req.stopBelowDb / 20.0) : 0.0f;
        for (size_t done = 0; done < holdFrames; done += 1024) {
            render(std::min<size_t>(1024, holdFrames - done), bp);
            peak = std::max(peak, bp);
            if (stop > 0 && done > (size_t)(0.3 * sr_) && bp < peak * stop) break;  // decayed: release early
        }
        tsf_channel_note_off(f_, 0, note);
        // keep recording the release until it is ~-80 dB below the peak
        const size_t maxRel = (size_t)(maxReleaseSec * sr_);
        for (size_t done = 0; done < maxRel; done += 1024) {
            render(1024, bp);
            if (peak > 0 && bp < peak * 1e-4f) break;
        }
        out.clipped = peak >= 0.999f;
        return true;
    }

private:
    tsf* f_;
    int sr_;
};

std::unique_ptr<Source> makeSf2Source(const std::string& path, int sampleRate, std::string& error) {
    tsf* f = tsf_load_filename(path.c_str());
    if (!f) {
        error = "cannot load SoundFont " + path;
        return nullptr;
    }
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, sampleRate, 0.0f);
    tsf_set_max_voices(f, 64);
    return std::make_unique<Sf2Source>(f, sampleRate);
}

std::vector<std::string> listSf2Presets(const std::string& path) {
    std::vector<std::string> out;
    tsf* f = tsf_load_filename(path.c_str());
    if (!f) return out;
    for (int i = 0; i < tsf_get_presetcount(f); ++i) {
        // TinySoundFont exposes name only; bank/program are found by search
        out.push_back(tsf_get_presetname(f, i));
    }
    tsf_close(f);
    return out;
}

}  // namespace sampler
