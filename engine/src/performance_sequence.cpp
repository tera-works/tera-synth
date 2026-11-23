#include "hsynth/performance_sequence.h"
#include "hsynth/json.h"
#include "hsynth/params.h"

#include <algorithm>
#include <cmath>

namespace hsynth {
namespace {
bool integer(const Json& json, const char* key, int min, int max, int& result) {
    const auto* v = json.find(key);
    if (!v) return true;
    const double n = v->number();
    if (!v->isNumber() || !std::isfinite(n) || n != std::floor(n) || n < min || n > max) return false;
    result = static_cast<int>(n);
    return true;
}
bool real(const Json& json, const char* key, float min, float max, float& result) {
    const auto* v = json.find(key);
    if (!v) return true;
    const double n = v->number();
    if (!v->isNumber() || !std::isfinite(n) || n < min || n > max) return false;
    result = static_cast<float>(n);
    return true;
}
bool boolean(const Json& json, const char* key, bool& result) {
    const auto* v = json.find(key);
    if (!v) return true;
    if (!v->isBool()) return false;
    result = v->boolean();
    return true;
}
uint64_t framesForMs(float ms, float sr) {
    return static_cast<uint64_t>(std::lround(ms * sr * 0.001f));
}
bool matches(const Event& off, const Event& on) {
    return off.noteId != 0 ? off.noteId == on.noteId :
           off.key == on.key && (off.channel == kOmniChannel || off.channel == on.channel);
}

struct Groove { const char* name; uint16_t hits[8]; };
constexpr int kRhythmSteps = 16;
// Bit zero is the downbeat. Lanes are kick, snare, closed/open hat,
// clap, low/high tom and rim.
constexpr Groove grooves[] = {
    {"Rock 1",       {0x0101, 0x1010, 0x5555, 0x4000, 0,      0,      0,      0}},
    {"Rock 2",       {0x0941, 0x1010, 0x1555, 0x4400, 0,      0,      0x8000, 0}},
    {"Pop",          {0x0501, 0x1010, 0x5555, 0x4000, 0x1010, 0,      0,      0}},
    {"Funk",         {0x0989, 0x1090, 0x7f77, 0x4080, 0,      0,      0,      0x0208}},
    {"Jazz Swing",   {0x0101, 0x1080, 0x9999, 0x0404, 0,      0,      0,      0x1010}},
    {"Bossa",        {0x4141, 0,      0x5555, 0,      0,      0,      0,      0x2245}},
    {"Latin",        {0x0909, 0x1040, 0x5555, 0x4004, 0,      0x0082, 0x8008, 0x2449}},
    {"Reggae",       {0x0100, 0,      0x5555, 0x4040, 0,      0,      0,      0x0100}},
    {"House",        {0x1111, 0,      0xaaab, 0x4444, 0x1010, 0,      0,      0}},
    {"Techno",       {0x1111, 0x1010, 0xffff, 0x4444, 0,      0x8000, 0,      0x0200}},
    {"Breakbeat",    {0x8241, 0x2120, 0x7777, 0x0800, 0,      0,      0x8000, 0}},
    {"Trap",         {0x4089, 0x0100, 0xef55, 0x0040, 0x0100, 0x2000, 0,      0}},
    {"Shuffle",      {0x0101, 0x1010, 0xdddd, 0x8000, 0,      0,      0,      0}},
    {"Waltz",        {0x0001, 0,      0x0841, 0x2000, 0,      0,      0,      0x0840}},
    {"Ballad",       {0x0101, 0x1010, 0x1111, 0x4000, 0,      0,      0,      0x0040}},
    {"Click Groove", {0x0001, 0,      0,      0,      0,      0,      0,      0x1111}}
};
}  // namespace

bool ArpPattern::isDefault() const {
    if (length != 16) return false;
    for (const auto& step : steps)
        if (step.gate != 1 || step.accent != 100 || step.oct != 0 || step.chord) return false;
    return true;
}

Json arpPatternToJson(const ArpPattern& pattern) {
    Json root = Json::object(), steps = Json::array();
    root.set("length", static_cast<int>(pattern.length));
    for (const auto& step : pattern.steps) {
        Json row = Json::object();
        row.set("gate", static_cast<int>(step.gate));
        row.set("accent", static_cast<int>(step.accent));
        row.set("oct", static_cast<int>(step.oct));
        row.set("chord", step.chord);
        steps.push(std::move(row));
    }
    root.set("steps", std::move(steps));
    return root;
}

bool arpPatternFromJson(const Json& json, ArpPattern& pattern) {
    if (!json.isObject()) return false;
    ArpPattern parsed = pattern;
    int length = parsed.length;
    if (!integer(json, "length", 1, 16, length)) return false;
    parsed.length = static_cast<uint8_t>(length);
    if (const auto* steps = json.find("steps")) {
        if (!steps->isArray() || steps->size() > parsed.steps.size()) return false;
        for (size_t i = 0; i < steps->size(); ++i) {
            const auto& row = steps->at(i);
            auto& step = parsed.steps[i];
            int gate = step.gate, accent = step.accent, oct = step.oct;
            bool chord = step.chord, tie = false;
            if (!row.isObject() || !integer(row, "gate", 0, 2, gate) || !integer(row, "accent", 0, 200, accent) ||
                !integer(row, "oct", -2, 2, oct) || !boolean(row, "chord", chord) || !boolean(row, "tie", tie)) return false;
            step = {static_cast<uint8_t>(tie ? 2 : gate), static_cast<uint8_t>(accent), static_cast<int8_t>(oct), chord};
        }
    }
    pattern = parsed;
    return true;
}

bool StrumState::collect(const Event& event, uint64_t frame, const float* p, float sr) {
    if (count + gathered >= kCapacity) return false;
    if (gathered && (frame > deadline || gathered == static_cast<int>(gathering.size()))) flush(frame, p, sr);
    if (!gathered) deadline = frame + framesForMs(p[P::StrumWindow], sr);
    gathering[gathered++] = event;
    return true;
}
void StrumState::chord(Event* notes, int size, uint64_t frame, const float* p, float sr) {
    if (gathered) flush(frame, p, sr);
    space(notes, std::min(size, kCapacity - count), frame, p, sr);
}
void StrumState::flush(uint64_t frame, const float* p, float sr) {
    if (!gathered) return;
    space(gathering.data(), gathered, frame, p, sr);
    gathered = 0;
}
void StrumState::cancel(const Event& event) {
    int kept = 0;
    for (int i = 0; i < gathered; ++i)
        if (!matches(event, gathering[i])) gathering[kept++] = gathering[i];
    gathered = kept;
    kept = 0;
    for (int i = 0; i < count; ++i)
        if (!matches(event, pending[i].event)) pending[kept++] = pending[i];
    count = kept;
}
void StrumState::space(Event* notes, int size, uint64_t frame, const float* p, float sr) {
    if (size == 0) return;
    const int order = static_cast<int>(p[P::StrumOrder]);
    if (order != 2) {
        const bool down = order == 1 || (order == 3 && alternate);
        if (order == 3 && size > 1) alternate = !alternate;
        for (int i = 1; i < size; ++i) {
            const Event event = notes[i];
            int j = i;
            while (j > 0 && (down ? notes[j - 1].key < event.key : notes[j - 1].key > event.key)) {
                notes[j] = notes[j - 1];
                --j;
            }
            notes[j] = event;
        }
    }
    uint64_t last = frame;
    for (int i = 0; i < size; ++i) {
        const float jitter = p[P::StrumJitter] != 0.0f ? rng.bipolar() * p[P::StrumJitter] : 0.0f;
        last = std::max(last, frame + framesForMs(std::max(0.0f, i * p[P::StrumSpacing] + jitter), sr));
        pending[count++] = {notes[i], last};
    }
}

Json rhythmToJson(const RhythmConfig& config, bool fill) {
    Json root = Json::object();
    root.set("enabled", config.enabled);
    root.set("pattern", config.pattern);
    root.set("levelDb", config.levelDb);
    root.set("swing", config.swing);
    root.set("fill", fill);
    return root;
}

bool rhythmFromJson(const Json& json, RhythmConfig& config, bool& fill) {
    if (!json.isObject()) return false;
    RhythmConfig next = config;
    bool nextFill = fill;
    if (!boolean(json, "on", next.enabled) || !boolean(json, "enabled", next.enabled) ||
        !integer(json, "pattern", 0, 15, next.pattern) || !real(json, "volume", -60.0f, 0.0f, next.levelDb) ||
        !real(json, "levelDb", -60.0f, 0.0f, next.levelDb) || !real(json, "swing", 0.0f, 0.5f, next.swing) ||
        !boolean(json, "fill", nextFill)) return false;
    config = next;
    fill = nextFill;
    return true;
}

Json rhythmPatternsJson() {
    Json names = Json::array();
    for (const auto& groove : grooves) names.push(groove.name);
    return names;
}

void RhythmPlayer::reset(float sr) {
    for (auto& d : drums_) d = {};
    sr_ = sr;
    gain_ = 0;
    step_ = 0;
    pattern_ = 0;
    previousBeat_ = 0.0;
    running_ = false;
    rng_ = Rng(0x122db7u);
}

void RhythmPlayer::hit(int lane, float velocity) {
    static constexpr float frequency[] = {53, 166, 5310, 6197, 402, 94, 147, 624};
    static constexpr float times[] = {0.22f, 0.14f, 0.044f, 0.27f, 0.12f, 0.20f, 0.16f, 0.067f};
    static constexpr float levels[] = {0.82f, 0.56f, 0.27f, 0.26f, 0.39f, 0.55f, 0.48f, 0.36f};
    auto& d = drums_[lane];
    d = {};
    d.inc = std::min(frequency[lane] / sr_, 0.44f);
    d.env = velocity * levels[lane];
    d.sweep = lane == 0 ? 2.5f : (lane == 5 || lane == 6) ? 0.7f : 0.0f;
    d.decay = std::exp(-6.9077553f / (times[lane] * sr_));
    if (lane == 2 && drums_[3].env > 0.0f) drums_[3].choke = std::max(1, static_cast<int>(0.003f * sr_));
}

float RhythmPlayer::tick(int lane, Drum& d) {
    if (d.env < 1e-5f) { d.env = 0.0f; return 0.0f; }
    d.phase += d.inc * (1.0f + d.sweep);
    if (d.phase >= 1.0f) d.phase -= 1.0f;
    d.sweep *= 1.0f - 140.0f / sr_;
    const float tone = std::sin(6.2831853f * d.phase);
    const float raw = rng_.bipolar();
    d.noise += 0.32f * (raw - d.noise);
    float x = tone;
    switch (lane) {
        case 0: x = tone + raw * 0.14f * d.sweep; break;
        case 1: x = tone * 0.32f + (raw - d.noise * 0.45f) * 0.76f; break;
        case 2: case 3: x = (raw - d.noise) * 0.86f + tone * 0.14f; break;
        case 4: {
            const int ms = static_cast<int>(1000.0f * d.age / sr_);
            x = (raw - d.noise) * ((ms < 24 && ms % 8 > 3) ? 0.18f : 1.0f);
            break;
        }
        case 7: x = 0.65f * tone + 0.35f * (raw - d.noise); break;
        default: x = tone + raw * 0.08f; break;
    }
    ++d.age;
    float out = x * d.env;
    d.env *= d.decay;
    if (d.choke > 0) {
        out *= std::min(1.0f, static_cast<float>(d.choke) / (0.003f * sr_));
        if (--d.choke == 0) d.env = 0.0f;
    }
    return out;
}

void RhythmPlayer::process(float* l, float* r, int n, double beat, double bpf,
                           bool running, const RhythmConfig& config, bool fill) {
    running = running && config.enabled;
    if (!running && gain_ == 0.0f) { running_ = false; return; }
    const auto& groove = grooves[config.pattern];
    const double division = config.pattern == 13 ? 3.0 / kRhythmSteps : 0.25;
    const float feel = config.pattern == 12 ? 1.0f / 3.0f : config.pattern == 4 ? 0.25f : 0.0f;
    const float swing = std::max(feel, config.swing);
    const double grid = beat / division;
    if (running && (!running_ || (pattern_ != config.pattern && (pattern_ == 13 || config.pattern == 13)) ||
                    beat < previousBeat_ - division * 0.5 ||
                    beat - previousBeat_ > division * 2.0)) {
        step_ = std::max<int64_t>(0, static_cast<int64_t>(std::floor(grid + 1e-8)));
        const double due = step_ + ((step_ & 1) ? swing : 0.0);
        if (grid > due + 1e-8) ++step_;
    }
    running_ = running;
    pattern_ = config.pattern;
    const float target = running ? dbToGain(config.levelDb) : 0.0f;
    const float ramp = 1.0f / std::max(1.0f, 0.008f * sr_);
    for (int i = 0; i < n; ++i) {
        const double now = (beat + i * bpf) / division;
        if (running && now + 1e-8 >= step_ + ((step_ & 1) ? swing : 0.0f)) {
            const int step = static_cast<int>(step_ % kRhythmSteps);
            const uint16_t mask = static_cast<uint16_t>(1u << step);
            for (int lane = 7; lane >= 0; --lane) {
                const bool fillHit = fill && step >= kRhythmSteps - 4 &&
                                     lane == (step & 1 ? 5 : 6);
                if ((groove.hits[lane] & mask) || fillHit)
                    hit(lane, fillHit || step == 0 ? 1.0f : (step % 4 == 0 ? 0.85f : 0.62f));
            }
            ++step_;
        }
        gain_ += clampf(target - gain_, -ramp, ramp);
        float x = 0.0f;
        for (int lane = 0; lane < 8; ++lane) x += tick(lane, drums_[lane]);
        l[i] += x * gain_;
        r[i] += x * gain_;
    }
    if (!running && gain_ == 0.0f) for (auto& d : drums_) d = {};
    previousBeat_ = beat + n * bpf;
}

}  // namespace hsynth
