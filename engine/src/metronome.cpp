#include "hsynth/dsp/metronome.h"

#include <algorithm>
#include <cmath>

#include "performance_json.h"

namespace hsynth {

namespace {
constexpr double kTau = 6.2831853071795864769;
}

MetronomeConfig sanitizeMetronome(MetronomeConfig c) {
    c.numerator = std::clamp<int>(c.numerator, 1, 16);
    if (c.denominator != 2 && c.denominator != 4 && c.denominator != 8 && c.denominator != 16) c.denominator = 4;
    c.subdivision = std::clamp<int>(c.subdivision, 1, 4);
    c.countInBars = std::min<int>(c.countInBars, 4);
    c.route = std::min<int>(c.route, 2);
    c.levelDb = std::isfinite(c.levelDb) ? std::clamp(c.levelDb, -60.0f, 0.0f) : -18.0f;
    c.accentDb = std::isfinite(c.accentDb) ? std::clamp(c.accentDb, 0.0f, 9.0f) : 4.0f;
    return c;
}

Json metronomeToJson(const MetronomeConfig& c) {
    Json json = Json::object();
    json.set("enabled", c.enabled);
    json.set("numerator", int(c.numerator));
    json.set("denominator", int(c.denominator));
    json.set("subdivision", int(c.subdivision));
    json.set("countInBars", int(c.countInBars));
    json.set("route", int(c.route));
    json.set("levelDb", c.levelDb);
    json.set("accentDb", c.accentDb);
    return json;
}

bool metronomeFromJson(const Json& j, MetronomeConfig& config) {
    using performance_json::field;
    auto c = config;
    if (!j.isObject() || !field(j, "enabled", c.enabled) ||
        !field(j, "numerator", c.numerator, 1, 16) || !field(j, "denominator", c.denominator, 2, 16) ||
        !field(j, "subdivision", c.subdivision, 1, 4) || !field(j, "countInBars", c.countInBars, 0, 4) ||
        !field(j, "route", c.route, 0, 2) || !field(j, "levelDb", c.levelDb, -60.0f, 0.0f) ||
        !field(j, "accentDb", c.accentDb, 0.0f, 9.0f)) return false;
    if (c.denominator != 2 && c.denominator != 4 && c.denominator != 8 && c.denominator != 16) return false;
    config = c;
    return true;
}

void Metronome::prepare(float rate) {
    sampleRate_ = std::max(1.0f, rate);
    attack_ = std::max(1, int(0.0005 * sampleRate_));
    duration_ = std::max(1, int(0.016 * sampleRate_));
    decay_ = static_cast<float>(std::exp(-7.0 / duration_));
    const double pitches[] = {1760.0, 1000.0, 750.0};
    for (int i = 0; i < 3; ++i) {
        const double step = kTau * pitches[i] / sampleRate_;
        sineSteps_[i] = std::sin(step);
        cosineSteps_[i] = std::cos(step);
    }
    restart();
}

bool Metronome::configure(const MetronomeConfig& c) {
    const MetronomeConfig next = sanitizeMetronome(c);
    const bool gridChanged = next.numerator != config_.numerator || next.denominator != config_.denominator ||
                             next.subdivision != config_.subdivision;
    config_ = next;
    if (gridChanged && !countingIn()) restart();
    level_ = std::pow(10.0f, 0.05f * config_.levelDb);
    accent_ = std::pow(10.0f, 0.05f * config_.accentDb);
    return gridChanged;
}

void Metronome::restart() {
    untilTick_ = lastPeriod_ = 0.0;
    tick_ = 0;
    beatNow_ = -1;
    click_ = {};
    beat_.store(-1, std::memory_order_relaxed);
}

void Metronome::pause() {
    click_ = {};
    beatNow_ = -1;
    beat_.store(-1, std::memory_order_relaxed);
}

void Metronome::alignToClock(double quarterBeat, float bpm) {
    const int sub = config_.subdivision;
    const int bar = config_.numerator * sub;
    const double grid = quarterBeat * config_.denominator * sub / 4.0;
    const double next = std::ceil(grid - 1e-8);
    tick_ = static_cast<int>(std::fmod(next, bar));
    beatNow_ = static_cast<int>(std::fmod(std::floor(grid), bar)) / sub;
    lastPeriod_ = sampleRate_ * 240.0 / (std::clamp<double>(bpm, 1.0, 1000.0) * config_.denominator * sub);
    untilTick_ = (next - grid) * lastPeriod_;
    click_ = {};
}

void Metronome::beginCountIn() {
    countInNumerator_ = config_.numerator;
    countInDenominator_ = config_.denominator;
    countInSubdivision_ = config_.subdivision;
    countIn_ = config_.countInBars * countInNumerator_ * countInSubdivision_;
    countInTick_ = 0;
    if (countIn_ > 0) restart();
}

void Metronome::cancelCountIn() {
    if (!countingIn()) return;
    countIn_ = 0;
    restart();
}

int Metronome::render(float* click, int frames, float bpm, bool enabled, bool externalStopped) {
    int countInEnd = -1;
    int i = 0;
    while (i < frames && (countingIn() || (enabled && !externalStopped))) {
        const bool countIn = countingIn();
        const int num = countIn ? countInNumerator_ : config_.numerator;
        const int den = countIn ? countInDenominator_ : config_.denominator;
        const int sub = countIn ? countInSubdivision_ : config_.subdivision;
        const double period = sampleRate_ * 240.0 / (std::clamp<double>(bpm, 1.0, 1000.0) * den * sub);
        if (lastPeriod_ != 0.0 && period != lastPeriod_ && untilTick_ > 0.0) untilTick_ *= period / lastPeriod_;
        lastPeriod_ = period;
        for (; i < frames; ++i, untilTick_ -= 1.0) {
            if (untilTick_ <= 1e-8) {
                if (countIn && countInTick_ == countIn_) {
                    countInEnd = i;
                    countIn_ = 0;
                    tick_ = 0;
                    break;
                }
                if (countIn) ++countInTick_;
                beatNow_ = tick_ / sub;
                const int pitch = tick_ == 0 ? 0 : tick_ % sub == 0 ? 1 : 2;
                click_ = {0, level_ * (pitch == 0 ? accent_ : 1.0f), 0.0, 1.0, sineSteps_[pitch], cosineSteps_[pitch]};
                tick_ = (tick_ + 1) % (num * sub);
                untilTick_ += period;
            }
            float sample = 0.0f;
            if (click_.age >= 0) {
                const float attack = std::min(1.0f, static_cast<float>(click_.age) / attack_);
                sample = attack * click_.gain * static_cast<float>(click_.sine);
                const double sine = click_.sine * click_.cosineStep + click_.cosine * click_.sineStep;
                click_.cosine = click_.cosine * click_.cosineStep - click_.sine * click_.sineStep;
                click_.sine = sine;
                click_.gain *= decay_;
                if (++click_.age == duration_) click_.age = -1;
            }
            click[i] = sample;
        }
    }
    if (i < frames) {
        std::fill(click + i, click + frames, 0.0f);
        // Retain the grid on MIDI Stop/Continue. An explicit disable restarts it.
        if (enabled) pause();
        else restart();
    }
    beat_.store(beatNow_, std::memory_order_relaxed);
    return countInEnd;
}

}  // namespace hsynth
