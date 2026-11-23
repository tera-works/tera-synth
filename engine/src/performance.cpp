#include "hsynth/engine.h"

#include <cmath>

#include "hsynth/json.h"

namespace hsynth {

namespace {
bool consumes(const PerformanceInputConfig& c, int cc) {
    return std::any_of(c.footswitches.begin(), c.footswitches.end(), [cc](const auto& fs) {
        return fs.action != 0 && fs.cc == cc;
    });
}
}  // namespace

void Engine::setPerfOffsets(int transposeSemi, float trimDb) {
    const float db = std::isfinite(trimDb) ? std::clamp(trimDb, -12.0f, 6.0f) : 0.0f;
    perfOffsets_.store({std::clamp(transposeSemi, -12, 12), db}, std::memory_order_relaxed);
}

void Engine::OutputRamp::set(float next, int frames) {
    if (target == next) return;
    target = next;
    remaining = frames;
    step = (target - current) / frames;
}

float Engine::OutputRamp::advance() {
    if (remaining > 0) {
        current += step;
        if (--remaining == 0) current = target;
    }
    return current;
}

MetronomeConfig Engine::metronomeConfig() const {
    auto c = metroConfig_.get();
    c.enabled = metroEnabled_.load(std::memory_order_relaxed) != 0;
    return c;
}

void Engine::setMetronomeConfig(const MetronomeConfig& config) {
    metroConfig_.update([&](auto& c) {
        c = sanitizeMetronome(config);
        setMetronomeEnabled(c.enabled);
        return true;
    });
}

bool Engine::setMetronomeJson(const Json& json) {
    return metroConfig_.update([&](auto& c) {
        if (!metronomeFromJson(json, c)) return false;
        if (json.find("enabled")) setMetronomeEnabled(c.enabled);
        return true;
    });
}

void Engine::processMetronome(int frames) {
    const bool enabled = metroEnabled_.load(std::memory_order_relaxed) != 0;
    phraseCountInEnd_ = -1;
    metroRendered_ = false;
    if (!enabled && !metronome_.countingIn()) {
        if (metroWasRendering_) metronome_.restart();
        metroWasRendering_ = metroClockSynced_ = false;
        return;
    }
    const float externalTempo = midiClockTempo();
    const bool externalStopped = !clockRunning_;
    if (externalStopped && !metronome_.countingIn()) {
        if (metronome_.currentBeat() >= 0) metronome_.pause();
        return;
    }
    metroWasRendering_ = true;
    metroRendered_ = true;
    const float tempo = externalTempo > 0.0f ? externalTempo : p_[P::fxChain(P::FxTempo)];
    const bool sync = externalTempo > 0.0f && !externalStopped && !metronome_.countingIn();
    if (sync && !metroClockSynced_) {
        const uint64_t last = tickTimes_[(tickHead_ + 24) % 25];
        const uint64_t atBlockStart = samplePos_ - frames;
        const double tickFrames = 60.0 * sampleRate_ / (externalTempo * 24.0);
        const double frac = std::min(0.999, static_cast<double>(atBlockStart - last) / tickFrames);
        metronome_.alignToClock((ticksSinceStart_ + frac) / 24.0, tempo);
    }
    metroClockSynced_ = sync;
    phraseCountInEnd_ = metronome_.render(metroClick_, frames, tempo, enabled, externalStopped);
}

void Engine::setInputBuffer(const float* left, const float* right, int validFrames) {
    inL_ = left;
    inR_ = right;
    inputFramesPending_ = left && validFrames < 0;
    if (!inputFramesPending_) {
        if (tuner_.enabled()) tuner_.pushInput(left, right, validFrames);
        liveSampler_.capture(0, left, right, validFrames);
    }
}

void Engine::setPerformanceInputConfig(const PerformanceInputConfig& config) {
    performanceInput_.update([&](auto& input) {
        input.config = sanitizePerformanceInput(config);
        input.prepareCurves();
        return true;
    });
}

bool Engine::setPedalConfigJson(const Json& json) {
    return performanceInput_.update([&](auto& input) {
        if (!performanceInputFromJson(json, input.config)) return false;
        input.prepareCurves();
        return true;
    });
}

void Engine::setFootswitch(int slot, int cc, int action, bool invert) {
    if (slot < 0 || slot >= 4) return;
    performanceInput_.update([&](auto& input) {
        input.config.footswitches[slot] = {std::clamp(cc, -1, 119), std::clamp(action, 0, 10), invert};
        return true;
    });
}

bool Engine::footswitchConsumesCc(int cc) const { return consumes(performanceInputConfig(), cc); }

int Engine::takeHostAction() {
    std::lock_guard<std::mutex> lock(hostActionConsumer_);
    int action = 0;
    hostActions_.pop(action);
    return action;
}

void Engine::syncPerformanceInput() {
    if (!performanceInput_.sync()) return;
    const auto& config = performanceInput_.audioValue().config;
    if (config.sustainInverted != appliedPedalConfig_.sustainInverted ||
        consumes(config, 64) != consumes(appliedPedalConfig_, 64)) {
        suppressPianoPedal_ = true;
        for (int ch = 0; ch < 16; ++ch)
            if ((hardwareSustainDown_ & (1u << ch)) || hardwareHalfParts_[ch]) setHardwareSustain(ch, false);
        suppressPianoPedal_ = false;
    }
    for (size_t i = 0; i < config.footswitches.size(); ++i)
        if (!(config.footswitches[i] == appliedPedalConfig_.footswitches[i])) footswitchChannels_[i] = 0;
    appliedPedalConfig_ = config;
    updatePedalState();
}

void Engine::updatePartPedal(int part) {
    PartState& ps = parts_[part];
    ps.pedal = hardwarePedalChannels_[part] ? 1.0f : ps.logicalPedal;
    const bool half = ps.p[P::PianoHalfDamp] > 0.0f;
    const float amount = ps.p[P::PianoPedalNoise];
    if (half || amount > 0.0f) {
        float position = ps.logicalPedal;
        for (int ch = 0; ch < 16; ++ch)
            if (hardwareHalfParts_[ch] & (1u << part)) position = std::max(position, hardwarePedalValue_[ch]);
        if (half) ps.pedal = position;
        ps.piano.pedal(position, amount, sampleRate_, curOffset_, !suppressPianoPedal_);
    } else {
        ps.piano.pedal(ps.pedal, 0.0f, sampleRate_, curOffset_, false);
    }
    if (half) ps.ctx.pedal = ps.pedal;
    const bool down = ps.pedal >= 0.1f;
    if (ps.sustain && !down) releaseSustained(part);
    ps.sustain = down;
}

void Engine::updatePedalState() {
    int bits = hardwareSustainDown_ ? 1 : 0;
    if (!bits) for (const auto& ps : parts_) if (ps.sustain) { bits = 1; break; }
    for (int i = 0; i < 4; ++i) if (footswitchChannels_[i]) bits |= 2 << i;
    pedalState_.store(bits, std::memory_order_relaxed);
}

void Engine::setHardwareSustain(int channel, bool down, float value) {
    const unsigned bit = 1u << channel;
    unsigned mask = hardwareSustainParts_[channel];
    const unsigned priorHalf = hardwareHalfParts_[channel];
    if (value < 0.0f) value = down ? 1.0f : 0.0f;
    hardwarePedalValue_[channel] = value;
    unsigned routed = 0;
    if (!multiActive_) routed = 1u;
    else for (int part = 0; part < kNumParts; ++part)
        if (static_cast<int>(mix_[part][Mix::Channel]) - 1 == channel) routed |= 1u << part;
    hardwareHalfParts_[channel] = value > 0.0f ? static_cast<uint16_t>(routed | priorHalf) : 0;
    if (down) hardwareSustainDown_ |= bit;
    else hardwareSustainDown_ &= ~bit;
    if (down) mask |= routed;
    const unsigned changed = mask | priorHalf | hardwareHalfParts_[channel];
    for (int part = 0; part < kNumParts; ++part) {
        if (!(changed & (1u << part))) continue;
        if (down && (mask & (1u << part))) hardwarePedalChannels_[part] |= bit;
        else hardwarePedalChannels_[part] &= ~bit;
        updatePartPedal(part);
    }
    hardwareSustainParts_[channel] = down ? mask : 0;
    Event e = Event::control(EventType::Sustain, down ? 1.0f : 0.0f);
    e.channel = static_cast<uint8_t>(channel);
    ghostPedal(e, true);
    updatePedalState();
}

bool Engine::externalCc(Event& e) {
    const int cc = e.rawCc;
    const int channel = e.channel & 15;
    const unsigned bit = 1u << channel;
    lastCc_.store((static_cast<uint32_t>(++ccSequence_) << 16) | (channel << 8) | cc,
                  std::memory_order_relaxed);
    const auto& config = performanceInput_.audioValue().config;
    const float raw = cc == 64 ? e.rawSustainValue : e.value;
    bool consumed = false;
    for (size_t i = 0; i < config.footswitches.size(); ++i) {
        const auto& fs = config.footswitches[i];
        if (fs.action == 0 || fs.cc != cc) continue;
        consumed = true;
        const bool wasDown = (footswitchChannels_[i] & bit) != 0;
        const float value = fs.invert ? 1.0f - raw : raw;
        if (!wasDown && value >= 0.55f) {
            footswitchChannels_[i] |= bit;
            footswitchAction(fs.action);
        } else if (wasDown && value <= 0.45f) {
            footswitchChannels_[i] &= ~bit;
        }
    }
    if (consumed) {
        updatePedalState();
        return true;
    }
    if (cc == 64) {
        const bool down = (e.value >= 0.5f) != config.sustainInverted;
        const float position = config.sustainInverted ? 1.0f - raw : raw;
        setHardwareSustain(channel, down, std::clamp(position, 0.0f, 1.0f));
        // The recorder stores the resulting logical event, with no external CC tag.
        e.type = EventType::Sustain;
        e.value = down ? 1.0f : 0.0f;
    } else if (cc == 11) {
        if (config.expMin != 0.0f || config.expMax != 1.0f)
            e.value = config.expMin + (config.expMax - config.expMin) * std::clamp(e.value, 0.0f, 1.0f);
    } else if (cc == 120 || cc == 121 || cc == 123) {
        suppressPianoPedal_ = true;
        if ((hardwareSustainDown_ & bit) || hardwareHalfParts_[channel]) setHardwareSustain(channel, false);
        suppressPianoPedal_ = false;
        for (auto& mask : footswitchChannels_) mask &= ~bit;
        updatePedalState();
    }
    return false;
}

void Engine::liveEvent(Event e) {
    const bool externalSustain = e.rawCc == 64;
    if (e.rawCc < 128 && externalCc(e)) return;
    e.rawCc = 0xFF;
    const auto& input = performanceInput_.audioValue();
    if (e.type == EventType::NoteOn) {
        if (e.value > 0.0f) {
            e.transpose = static_cast<int8_t>(activePerfOffsets_.transpose);
            e.value = input.noteVelocity(e.value);
        } else {
            e.type = EventType::NoteOff;
            e.value = 0.5f;
        }
    } else if (e.type == EventType::ChannelPressure || e.type == EventType::NotePressure) {
        e.value = input.aftertouch(e.value);
    }
    if (externalSustain) routeZoneControl(e);
    else handleEvent(e);
    phraseCapture(e);
}

void Engine::footswitchAction(int action) {
    if (action >= 1 && action <= 4) {
        hostActions_.push(action);
        return;
    }
    switch (action) {
        case 5: {
            const int id = P::fxp(fx::Rotary, fx::rotary::Fast);
            setParam(id, getParam(id) > 0.5f ? 0.0f : 1.0f);
            break;
        }
        case 6: looper_.command(fx::LooperCmd::Tap); break;
        case 7: tapTempo(); break;
        case 8: metroEnabled_.fetch_xor(1u, std::memory_order_relaxed); break;
        case 9: applyPhraseCommand(CmdRecord); break;
        case 10:
            handleEvent(Event::control(EventType::AllSoundOff, 0.0f));
            break;
        default: break;
    }
}

void Engine::tapTempo() {
    const uint64_t now = samplePos_;
    const uint64_t delta = now - lastTapFrame_;
    if (haveTap_ && delta < static_cast<uint64_t>(0.1 * sampleRate_)) return;
    lastTapFrame_ = now;
    if (!haveTap_ || delta > static_cast<uint64_t>(3.0 * sampleRate_)) {
        haveTap_ = true;
        tapCount_ = tapNext_ = 0;
        return;
    }
    tapIntervals_[tapNext_] = delta;
    tapNext_ = (tapNext_ + 1) % 4;
    tapCount_ = std::min(tapCount_ + 1, 4);
    const uint64_t latestTick = tickCount_ ? tickTimes_[(tickHead_ + 24) % 25] : 0;
    if (tickCount_ && now - latestTick < static_cast<uint64_t>(0.5f * sampleRate_)) return;
    uint64_t sorted[4];
    std::fill(std::begin(sorted), std::end(sorted), UINT64_MAX);
    std::copy_n(tapIntervals_, tapCount_, sorted);
    std::sort(std::begin(sorted), std::end(sorted));
    const double interval = 0.5 * (sorted[(tapCount_ - 1) / 2] + sorted[tapCount_ / 2]);
    setParam(P::fxChain(P::FxTempo), static_cast<float>(60.0 * sampleRate_ / interval));
}

}  // namespace hsynth
