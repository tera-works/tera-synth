#include "hsynth/engine.h"

#include <chrono>
#include <cstring>

#if defined(__SSE__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace hsynth {

namespace {
// Denormal floats (decaying filter / reverb states) are extremely slow on many
// CPUs. Enable flush-to-zero on the calling (audio) thread.
void enableFlushToZero() {
#if defined(__aarch64__)
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    if (!(fpcr & (1ull << 24))) __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));
#elif defined(__SSE__) || defined(_M_X64)
    _mm_setcsr(_mm_getcsr() | 0x8040);  // FTZ | DAZ
#endif
}

void atomicMax(std::atomic<float>& a, float v) {
    float cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
void atomicMaxI(std::atomic<int>& a, int v) {
    int cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
}  // namespace

Engine::Engine() : sharedParams_(new std::atomic<float>[P::kNumParams]) {
    float defs[P::kNumParams];
    defaultParams(defs);
    for (int i = 0; i < P::kNumParams; ++i) sharedParams_[i].store(defs[i]);
    std::memcpy(p_, defs, sizeof p_);
    for (auto& s : pendingSets_) s.store(nullptr);
    for (int t = 0; t < 2; ++t) {
        pendingIr_[t].store(nullptr);
    }
}

Engine::~Engine() {
    for (int l = 0; l < kNumLayers; ++l) {
        delete pendingSets_[l].exchange(nullptr);
        delete currentSets_[l];
        currentSets_[l] = nullptr;
    }
    for (int t = 0; t < 2; ++t) {
        fx::ConvIr* p = pendingIr_[t].exchange(nullptr);
        if (p != &unloadIr_) delete p;
        delete currentIr_[t];
        currentIr_[t] = nullptr;
    }
    collectGarbage();
}

void Engine::prepare(float sampleRate, int maxBlockSize) {
    (void)maxBlockSize;
    sampleRate_ = sampleRate;
    for (auto& v : voices_) v.prepare(sampleRate);
    builtinWavetable(0);  // generate the shared tables now, not on the audio thread
    rack_.prepare(sampleRate);
    for (int t = 0; t < 2; ++t) rack_.setUserIr(t, currentIr_[t]);
    effectsQuality_ = -1;
    limiter_.prepare(sampleRate, kMaxBlockSize);
    masterGain_.setTime(0.02f, sampleRate);
    masterGain_.reset(p_[P::MasterVolume]);
    ctx_.sampleRate = sampleRate;
    seenParamVersion_ = 0;
    prepared_ = true;
}

// --------------------------------------------------------- non-audio API

bool Engine::sendEvent(const Event& e) {
    std::lock_guard<std::mutex> lock(producerMutex_);
    if (!events_.push(e)) {
        droppedEvents_.fetch_add(1);
        return false;
    }
    return true;
}

void Engine::setParam(int id, float value) {
    if (id < 0 || id >= P::kNumParams) return;
    sharedParams_[id].store(sanitizeParam(id, value), std::memory_order_relaxed);
    paramVersion_.fetch_add(1, std::memory_order_release);
}

float Engine::getParam(int id) const {
    if (id < 0 || id >= P::kNumParams) return 0.0f;
    return sharedParams_[id].load(std::memory_order_relaxed);
}

void Engine::setAllParams(const float* values) {
    for (int i = 0; i < P::kNumParams; ++i) sharedParams_[i].store(sanitizeParam(i, values[i]), std::memory_order_relaxed);
    paramVersion_.fetch_add(1, std::memory_order_release);
}

void Engine::getAllParams(float* out) const {
    for (int i = 0; i < P::kNumParams; ++i) out[i] = sharedParams_[i].load(std::memory_order_relaxed);
}

void Engine::setSampleSet(int layer, std::unique_ptr<SampleSet> set) {
    if (layer < 0 || layer >= kNumLayers) return;
    if (!set) set = std::make_unique<SampleSet>();
    {
        std::lock_guard<std::mutex> lock(nameMutex_);
        setNames_[layer] = set->name;
        setPaths_[layer] = set->sourcePath;
    }
    std::lock_guard<std::mutex> lock(loaderMutex_);
    SampleSet* old = pendingSets_[layer].exchange(set.release(), std::memory_order_acq_rel);
    delete old;  // never reached the audio thread
    if (!prepared_) {
        // No audio thread yet: publish immediately.
        SampleSet* s = pendingSets_[layer].exchange(nullptr);
        delete currentSets_[layer];
        currentSets_[layer] = s;
    }
}

std::string Engine::sampleSetName(int layer) const {
    std::lock_guard<std::mutex> lock(nameMutex_);
    return layer >= 0 && layer < kNumLayers ? setNames_[layer] : std::string();
}

std::string Engine::sampleSetPath(int layer) const {
    std::lock_guard<std::mutex> lock(nameMutex_);
    return layer >= 0 && layer < kNumLayers ? setPaths_[layer] : std::string();
}

void Engine::collectGarbage() {
    SampleSet* s;
    while (retired_.pop(s)) delete s;
    fx::ConvIr* ir;
    while (retiredIr_.pop(ir)) delete ir;
}

void Engine::setFxImpulse(int target, std::unique_ptr<fx::ConvIr> ir) {
    if (target < 0 || target > 1) return;
    {
        std::lock_guard<std::mutex> lock(nameMutex_);
        irNames_[target] = ir ? ir->name : std::string();
        irPaths_[target] = ir ? ir->path : std::string();
    }
    std::lock_guard<std::mutex> lock(loaderMutex_);
    if (!prepared_) {
        delete currentIr_[target];
        currentIr_[target] = ir.release();
        return;
    }
    fx::ConvIr* next = ir ? ir.release() : &unloadIr_;
    fx::ConvIr* old = pendingIr_[target].exchange(next, std::memory_order_acq_rel);
    if (old != &unloadIr_) delete old;  // never reached the audio thread
}

std::string Engine::fxImpulseName(int target) const {
    std::lock_guard<std::mutex> lock(nameMutex_);
    return target >= 0 && target < 2 ? irNames_[target] : std::string();
}

std::string Engine::fxImpulsePath(int target) const {
    std::lock_guard<std::mutex> lock(nameMutex_);
    return target >= 0 && target < 2 ? irPaths_[target] : std::string();
}

void Engine::setMaxVoices(int n) { maxVoices_.store(std::max(1, std::min(kMaxVoices, n))); }

EngineStats Engine::stats() const {
    EngineStats s;
    s.activeVoices = statActive_.load();
    s.peakVoices = statPeakVoices_.load();
    s.cpuLoad = statLoad_.load();
    s.cpuLoadPeak = statLoadPeak_.load();
    s.renderUsAvg = statUs_.load();
    s.renderUsPeak = statUsPeak_.load();
    s.blocks = statBlocks_.load();
    s.droppedEvents = droppedEvents_.load();
    s.lastBlockSize = statBlockSize_.load();
    s.outputPeak = statOutPeak_.load();
    return s;
}

void Engine::resetPeakStats() {
    statPeakVoices_.store(0);
    statLoadPeak_.store(0);
    statUsPeak_.store(0);
    statOutPeak_.store(0);
}

// ------------------------------------------------------------ audio thread

Voice* Engine::allocateVoice() {
    const int limit = maxVoices_.load(std::memory_order_relaxed);
    int playing = 0;
    for (auto& v : voices_)
        if (v.active() && !v.stolen()) ++playing;
    if (playing >= limit) {
        // steal: released voice with lowest level first, otherwise the oldest
        Voice* victim = nullptr;
        for (auto& v : voices_) {
            if (!v.active() || v.stolen()) continue;
            if (!victim) { victim = &v; continue; }
            const bool vr = v.released(), cr = victim->released();
            if (vr != cr) { if (vr) victim = &v; continue; }
            if (vr ? v.level() < victim->level() : v.age() < victim->age()) victim = &v;
        }
        if (victim) victim->steal();
    }
    for (auto& v : voices_)
        if (!v.active()) return &v;
    // pool exhausted: reuse the quietest fading voice
    Voice* q = nullptr;
    for (auto& v : voices_)
        if (v.stolen() && (!q || v.level() < q->level())) q = &v;
    if (!q) q = &voices_[0];
    q->hardStop();
    return q;
}

void Engine::noteOn(const Event& e) {
    if (e.value <= 0.0f) {  // velocity 0 => note off
        noteOff(Event::noteOff(e.noteId, e.key));
        return;
    }
    if (e.noteId != 0) {
        for (auto& v : voices_)
            if (v.active() && !v.released() && v.noteId() == e.noteId) v.release();
    }
    bool held = false;
    for (auto& v : voices_)
        if (v.active() && !v.released()) { held = true; break; }
    rack_.noteOn(e.key, e.value, !held);
    Voice* v = allocateVoice();
    v->start(e, ctx_, ++ageCounter_, rng_.unipolar());
}

void Engine::noteOff(const Event& e) {
    for (auto& v : voices_) {
        if (!v.active() || v.released() || v.sustained) continue;
        const bool match = e.noteId != 0 ? v.noteId() == e.noteId : v.key() == e.key;
        if (!match) continue;
        if (sustain_ || v.hold) v.sustained = true;
        else v.release();
    }
}

void Engine::releaseSustained() {
    for (auto& v : voices_)
        if (v.active() && v.sustained && !v.hold) v.release();
}

void Engine::handleEvent(const Event& e) {
    switch (e.type) {
        case EventType::NoteOn: noteOn(e); break;
        case EventType::NoteOff: noteOff(e); break;
        case EventType::NotePitch:
        case EventType::NotePressure:
        case EventType::NoteTimbre:
        case EventType::NoteX:
            for (auto& v : voices_) {
                if (!v.active() || v.stolen()) continue;
                if (e.noteId != 0 ? v.noteId() == e.noteId : v.key() == e.key) v.applyExpression(e.type, e.value);
            }
            break;
        case EventType::NoteHold:
            for (auto& v : voices_) {
                if (!v.active() || v.stolen()) continue;
                if (e.noteId != 0 ? v.noteId() != e.noteId : v.key() != e.key) continue;
                v.hold = e.value > 0.5f;
                if (!v.hold && v.sustained && !sustain_) v.release();
            }
            break;
        case EventType::PitchBend: pitchBend_ = clampf(e.value, -1.0f, 1.0f); break;
        case EventType::ModWheel: ctx_.modWheel = clampf(e.value, 0.0f, 1.0f); break;
        case EventType::ChannelPressure: ctx_.chanPressure = clampf(e.value, 0.0f, 1.0f); break;
        case EventType::Sustain: {
            const bool on = e.value >= 0.5f;
            if (sustain_ && !on) { sustain_ = false; releaseSustained(); }
            sustain_ = on;
            break;
        }
        case EventType::Controller:
            switch (e.controller) {
                case 1: ctx_.modWheel = clampf(e.value, 0.0f, 1.0f); break;
                case 2: ctx_.breath = clampf(e.value, 0.0f, 1.0f); break;
                case 11: ctx_.expression = clampf(e.value, 0.0f, 1.0f); break;
                case 64: handleEvent(Event::control(EventType::Sustain, e.value)); break;
                case 120: for (auto& v : voices_) v.hardStop(); break;
                case 123: for (auto& v : voices_) if (v.active()) v.release(); break;
                default: break;
            }
            break;
        case EventType::AllNotesOff:
            sustain_ = false;
            for (auto& v : voices_) if (v.active()) v.release();
            break;
        case EventType::AllSoundOff:
            for (auto& v : voices_) v.hardStop();
            break;
        case EventType::Looper: rack_.looperCommand((fx::LooperCmd)e.controller); break;
        case EventType::ProgramChange:  // handled by the host
        default: break;
    }
}

void Engine::updateEffects() {
    const int q = quality_.load(std::memory_order_relaxed);
    effectsQuality_ = q;
    // Low: no oversampling, Normal: 2x, High: 4x (drive / amp stages)
    rack_.paramsChanged(p_, q == (int)Quality::Low ? 1 : q == (int)Quality::High ? 4 : 2, q == (int)Quality::High);
    limiter_.enabled = p_[P::Limiter] > 0.5f;
}

void Engine::process(float* L, float* R, int n) {
    const auto t0 = std::chrono::steady_clock::now();
    enableFlushToZero();
    if (!prepared_ || n <= 0) {
        if (n > 0) { std::memset(L, 0, sizeof(float) * n); std::memset(R, 0, sizeof(float) * n); }
        return;
    }

    // 1. parameters
    const uint32_t ver = paramVersion_.load(std::memory_order_acquire);
    if (ver != seenParamVersion_) {
        seenParamVersion_ = ver;
        for (int i = 0; i < P::kNumParams; ++i) p_[i] = sharedParams_[i].load(std::memory_order_relaxed);
        updateEffects();
    } else if (effectsQuality_ != quality_.load(std::memory_order_relaxed)) {
        updateEffects();
    }
    for (int t = 0; t < 2; ++t) {
        if (!pendingIr_[t].load(std::memory_order_acquire)) continue;
        if (currentIr_[t] && !retiredIr_.push(currentIr_[t])) continue;  // garbage queue full: retry next block
        fx::ConvIr* p = pendingIr_[t].exchange(nullptr, std::memory_order_acq_rel);
        currentIr_[t] = p == &unloadIr_ ? nullptr : p;
        rack_.setUserIr(t, currentIr_[t]);
    }

    // 2. sample set swaps
    for (int l = 0; l < kNumLayers; ++l) {
        SampleSet* s = pendingSets_[l].exchange(nullptr, std::memory_order_acq_rel);
        if (!s) continue;
        for (auto& v : voices_) v.stopLayer(l);
        if (currentSets_[l] && !retired_.push(currentSets_[l])) {
            // garbage queue full (collectGarbage not called): keep the new set
            // pending-free path by leaking is unacceptable, so swap back.
            SampleSet* expected = nullptr;
            if (!pendingSets_[l].compare_exchange_strong(expected, s)) retired_.push(s);
            continue;
        }
        currentSets_[l] = s;
    }

    // 3. events (voices started here read ctx_)
    ctx_.params = p_;
    for (int l = 0; l < kNumLayers; ++l) ctx_.sampleSets[l] = currentSets_[l];
    Event e;
    int budget = 512;
    while (budget-- > 0 && events_.pop(e)) handleEvent(e);

    // 4. render
    ctx_.pitchBendNorm = pitchBend_;
    ctx_.pitchBendSemis = pitchBend_ * p_[P::BendRange];

    std::memset(L, 0, sizeof(float) * n);
    std::memset(R, 0, sizeof(float) * n);
    for (int off = 0; off < n; off += kControlBlock) {
        const int m = std::min(kControlBlock, n - off);
        ctx_.lfo2 = lfo2_.advance((LfoWave)(int)p_[P::Lfo2Wave], p_[P::Lfo2Rate], (float)m / sampleRate_, rng_);
        for (auto& v : voices_)
            if (v.active()) v.render(L + off, R + off, m, ctx_);
    }

    // 5. effect rack, then master
    bool held = false;
    for (auto& v : voices_)
        if (v.active() && !v.released()) { held = true; break; }
    fx::FxGlobals g;
    g.modWheel = ctx_.modWheel;
    g.lfo2 = ctx_.lfo2;
    g.chanPressure = ctx_.chanPressure;
    g.expression = ctx_.expression;
    g.breath = ctx_.breath;
    g.pitchBend = pitchBend_;
    g.notesHeld = held;
    rack_.process(L, R, n, g, inL_, inR_);
    inL_ = inR_ = nullptr;
    const float target = p_[P::MasterVolume] * p_[P::MasterVolume];  // perceptual taper
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float g = masterGain_.process(target);
        L[i] *= g;
        R[i] *= g;
    }
    limiter_.process(L, R, n);
    for (int i = 0; i < n; ++i) peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));

    // 6. stats
    int active = 0;
    for (auto& v : voices_)
        if (v.active()) ++active;
    statActive_.store(active, std::memory_order_relaxed);
    atomicMaxI(statPeakVoices_, active);
    atomicMax(statOutPeak_, peak);
    statBlockSize_.store(n, std::memory_order_relaxed);
    const float us = std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - t0).count();
    const float load = us / (1e6f * (float)n / sampleRate_);
    statUs_.store(statUs_.load(std::memory_order_relaxed) * 0.95f + us * 0.05f, std::memory_order_relaxed);
    statLoad_.store(statLoad_.load(std::memory_order_relaxed) * 0.95f + load * 0.05f, std::memory_order_relaxed);
    atomicMax(statUsPeak_, us);
    atomicMax(statLoadPeak_, load);
    statBlocks_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace hsynth
