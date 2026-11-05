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

Engine::Engine()
    : sharedParams_(new std::atomic<float>[P::kNumParams]),
      partShared_(new std::atomic<float>[(size_t)(kNumParts - 1) * P::kPartParams]),
      partP_(new float[(size_t)(kNumParts - 1) * P::kPartParams]),
      partBus_(new float[(size_t)kNumParts * 2 * kMaxBlockSize]()),
      inserts_(new fx::InsertChain[kNumParts]) {
    float defs[P::kNumParams];
    defaultParams(defs);
    for (int i = 0; i < P::kNumParams; ++i) sharedParams_[i].store(defs[i]);
    std::memcpy(p_, defs, sizeof p_);
    for (int k = 1; k < kNumParts; ++k)
        for (int i = 0; i < P::kPartParams; ++i) {
            const size_t idx = (size_t)(k - 1) * P::kPartParams + (size_t)i;
            partShared_[idx].store(defs[i]);
            partP_[idx] = defs[i];
        }
    for (int k = 0; k < kNumParts; ++k) {
        partVersion_[k].store(1);
        statPartVoices_[k].store(0);
        float mix[Mix::Count];
        defaultPartMix(k, mix);
        for (int i = 0; i < Mix::Count; ++i) {
            mixShared_[k][i].store(mix[i]);
            mix_[k][i] = mix[i];
        }
        parts_[k].p = k == 0 ? p_ : partP_.get() + (size_t)(k - 1) * P::kPartParams;
        parts_[k].rng = Rng(0x9E3779B9u * (uint32_t)(k + 1));
        for (auto& ps : pendingSets_[k]) ps.store(nullptr);
        for (auto& ks : keyswitch_[k]) ks = -1;
    }
    for (int t = 0; t < 2; ++t) {
        pendingIr_[t].store(nullptr);
    }
}

Engine::~Engine() {
    for (int k = 0; k < kNumParts; ++k)
        for (int l = 0; l < kNumLayers; ++l) {
            delete pendingSets_[k][l].exchange(nullptr);
            delete currentSets_[k][l];
            currentSets_[k][l] = nullptr;
        }
    for (auto& r : retiring_) {
        delete r.set;
        r.set = nullptr;
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
    for (int k = 0; k < kNumParts; ++k) inserts_[k].prepare(sampleRate);
    for (int t = 0; t < 2; ++t) rack_.setUserIr(t, currentIr_[t]);
    effectsQuality_ = -1;
    limiter_.prepare(sampleRate, kMaxBlockSize);
    masterGain_.setTime(0.02f, sampleRate);
    masterGain_.reset(p_[P::MasterVolume]);
    for (auto& ps : parts_) ps.ctx.sampleRate = sampleRate;
    gainStep_ = 1.0f / (0.01f * sampleRate);  // mix gain ramps: full scale in 10 ms
    seenParamVersion_ = 0;
    for (auto& v : seenPartVersion_) v = 0;
    seenMixVersion_ = 0;
    samplePos_ = 0;
    tickCount_ = 0;
    clockBpm_ = 0.0f;
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

void Engine::setPartSampleSet(int part, int layer, std::unique_ptr<SampleSet> set) {
    if (part < 0 || part >= kNumParts || layer < 0 || layer >= kNumLayers) return;
    if (!set) set = std::make_unique<SampleSet>();
    {
        std::lock_guard<std::mutex> lock(nameMutex_);
        setNames_[part][layer] = set->name;
        setPaths_[part][layer] = set->sourcePath;
    }
    std::lock_guard<std::mutex> lock(loaderMutex_);
    SampleSet* old = pendingSets_[part][layer].exchange(set.release(), std::memory_order_acq_rel);
    delete old;  // never reached the audio thread
    if (!prepared_) {
        // No audio thread yet: publish immediately.
        SampleSet* s = pendingSets_[part][layer].exchange(nullptr);
        delete currentSets_[part][layer];
        currentSets_[part][layer] = s;
    }
}

std::string Engine::partSampleSetName(int part, int layer) const {
    std::lock_guard<std::mutex> lock(nameMutex_);
    return part >= 0 && part < kNumParts && layer >= 0 && layer < kNumLayers ? setNames_[part][layer] : std::string();
}

std::string Engine::partSampleSetPath(int part, int layer) const {
    std::lock_guard<std::mutex> lock(nameMutex_);
    return part >= 0 && part < kNumParts && layer >= 0 && layer < kNumLayers ? setPaths_[part][layer] : std::string();
}

void Engine::setPartParam(int part, int id, float value) {
    if (part < 0 || part >= kNumParts || id < 0 || id >= P::kNumParams) return;
    if (part == 0 || !isPartParam(id)) { setParam(id, value); return; }
    partShared_[(size_t)(part - 1) * P::kPartParams + (size_t)id].store(sanitizeParam(id, value), std::memory_order_relaxed);
    partVersion_[part].fetch_add(1, std::memory_order_release);
}

float Engine::getPartParam(int part, int id) const {
    if (part < 0 || part >= kNumParts || id < 0 || id >= P::kNumParams) return 0.0f;
    if (part == 0 || !isPartParam(id)) return getParam(id);
    return partShared_[(size_t)(part - 1) * P::kPartParams + (size_t)id].load(std::memory_order_relaxed);
}

void Engine::setPartParams(int part, const float* values) {
    if (part < 0 || part >= kNumParts) return;
    for (int i = 0; i < P::kPartParams; ++i) {
        if (!isPartParam(i)) continue;
        const float v = sanitizeParam(i, values[i]);
        if (part == 0) sharedParams_[i].store(v, std::memory_order_relaxed);
        else partShared_[(size_t)(part - 1) * P::kPartParams + (size_t)i].store(v, std::memory_order_relaxed);
    }
    if (part == 0) paramVersion_.fetch_add(1, std::memory_order_release);
    else partVersion_[part].fetch_add(1, std::memory_order_release);
}

void Engine::getPartParams(int part, float* out) const {
    getAllParams(out);
    if (part <= 0 || part >= kNumParts) return;
    for (int i = 0; i < P::kPartParams; ++i)
        if (isPartParam(i)) out[i] = partShared_[(size_t)(part - 1) * P::kPartParams + (size_t)i].load(std::memory_order_relaxed);
}

void Engine::setGlobalParams(const float* values) {
    for (int i = 0; i < P::kNumParams; ++i)
        if (!isPartParam(i)) sharedParams_[i].store(sanitizeParam(i, values[i]), std::memory_order_relaxed);
    paramVersion_.fetch_add(1, std::memory_order_release);
}

void Engine::setPartMix(int part, int id, float value) {
    if (part < 0 || part >= kNumParts || id < 0 || id >= Mix::Count) return;
    mixShared_[part][id].store(sanitizePartMix(id, value), std::memory_order_relaxed);
    mixVersion_.fetch_add(1, std::memory_order_release);
}

float Engine::getPartMix(int part, int id) const {
    if (part < 0 || part >= kNumParts || id < 0 || id >= Mix::Count) return 0.0f;
    return mixShared_[part][id].load(std::memory_order_relaxed);
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

Voice* Engine::allocateVoice(int part) {
    const int limit = maxVoices_.load(std::memory_order_relaxed);
    int playing = 0;
    int count[kNumParts] = {};
    for (auto& v : voices_)
        if (v.active() && !v.stolen()) { ++playing; ++count[v.part]; }
    if (playing >= limit) {
        // steal: a released voice first (lowest level), otherwise the oldest
        // held voice. Across parts the part that would hold the most voices
        // after this note-on loses one, so a busy part cannot starve the
        // others. With a single part this is the plain single-timbral rule.
        ++count[part];
        Voice* victim = nullptr;
        for (auto& v : voices_) {
            if (!v.active() || v.stolen()) continue;
            if (!victim) { victim = &v; continue; }
            const bool vr = v.released(), cr = victim->released();
            if (vr != cr) { if (vr) victim = &v; continue; }
            const int vc = count[v.part], cc = count[victim->part];
            if (vc != cc) { if (vc > cc) victim = &v; continue; }
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

void Engine::noteOn(int part, const Event& in) {
    if (in.value <= 0.0f) {  // velocity 0 => note off
        noteOff(Event::noteOff(in.noteId, in.key), part);
        return;
    }
    Event e = in;
    float send = 1.0f;
    {
        const float curve = parts_[part].p[P::VelCurve];
        if (curve != 0.0f) e.value = std::pow(clampf(e.value, 0.0f, 1.0f), std::exp2(curve * 1.5f));  // >0: harder
    }
    if (multiActive_) {
        const float* m = mix_[part];
        if (m[Mix::Mute] > 0.5f || in.key < (int)m[Mix::KeyLow] || in.key > (int)m[Mix::KeyHigh]) return;
        e.key = (uint8_t)std::max(0, std::min(127, (int)in.key + (int)m[Mix::Transpose]));
        send = m[Mix::FxSend];
    }
    if (e.noteId != 0) {
        for (auto& v : voices_)
            if (v.active() && !v.released() && v.noteId() == e.noteId && v.part == part) v.release();
    }
    bool held = false;
    for (auto& v : voices_)
        if (v.active() && !v.released()) { held = true; break; }
    if (send > 0.0f) rack_.noteOn(e.key, e.value, !held);
    if (!inserts_[part].empty()) {
        bool partHeld = false;
        for (auto& v : voices_)
            if (v.active() && !v.released() && v.part == part) { partHeld = true; break; }
        inserts_[part].noteOn(e.key, !partHeld);
    }
    PartState& ps = parts_[part];
    const auto mode = (VoiceMode)(int)ps.p[P::VoiceMode];
    const bool glideOn = ps.p[P::Glide] > 0.0005f;
    const bool glideLegatoOnly = ps.p[P::GlideMode] > 0.5f;
    if (mode != VoiceMode::Poly) {
        // note stack: last note priority; a repeated key moves to the top
        int w = 0;
        for (int i = 0; i < ps.stackSize; ++i)
            if (ps.stack[i].key != e.key) ps.stack[w++] = ps.stack[i];
        ps.stackSize = w;
        if (ps.stackSize == PartState::kStack) {
            for (int i = 1; i < ps.stackSize; ++i) ps.stack[i - 1] = ps.stack[i];
            --ps.stackSize;
        }
        Event st = e;
        st.channel = in.key;  // remember the untransposed key for key-matched note-offs
        ps.stack[ps.stackSize++] = st;
        bool sounding = false;
        float from = ps.lastPitch;
        for (auto& v : voices_)
            if (v.active() && !v.released() && v.part == part && v.noteId() == ps.monoId && v.matchKey == ps.monoKey) {
                sounding = true;
                from = v.currentPitch();  // key + glide (unison detune is not part of it)
                break;
            }
        const bool glide = glideOn && from >= 0.0f && (!glideLegatoOnly || sounding);
        if (mode == VoiceMode::Legato && sounding) {
            for (auto& v : voices_)
                if (v.active() && !v.released() && v.part == part && v.noteId() == ps.monoId && v.matchKey == ps.monoKey) {
                    v.legatoTo(e, glide);
                    v.matchKey = in.key;
                }
        } else {
            for (auto& v : voices_)
                if (v.active() && v.part == part && !v.stolen()) v.steal();  // mono: one note at a time
            startNote(part, e, glide ? from : -1.0f);
            for (auto& v : voices_)
                if (v.active() && v.part == part && v.noteId() == e.noteId && !v.stolen()) v.matchKey = in.key;
        }
        ps.monoId = e.noteId;
        ps.monoKey = in.key;
        ps.lastPitch = (float)e.key;
        return;
    }
    bool partHeld = false;
    for (auto& v : voices_)
        if (v.active() && !v.released() && v.part == part) { partHeld = true; break; }
    const bool glide = glideOn && ps.lastPitch >= 0.0f && (!glideLegatoOnly || partHeld);
    startNote(part, e, glide ? ps.lastPitch : -1.0f);
    for (auto& v : voices_)
        if (v.active() && v.part == part && v.noteId() == e.noteId && v.age() == ageCounter_) v.matchKey = in.key;
    ps.lastPitch = (float)e.key;
}

void Engine::startNote(int part, const Event& e, float glideFrom) {
    // SFZ keyswitches and round-robin counters (per part and layer)
    PartState& ps = parts_[part];
    for (int l = 0; l < kNumLayers; ++l) {
        const SampleSet* set = currentSets_[part][l];
        if (!set) continue;
        if (set->isKeyswitch(e.key)) keyswitch_[part][l] = e.key;
        ps.ctx.keyswitch[l] = keyswitch_[part][l];
        ps.ctx.seq[l] = seqCount_[part][l]++;
    }
    const int unison = std::max(1, std::min(8, (int)ps.p[P::Unison]));
    const uint64_t age = ++ageCounter_;  // unison voices share one age (stolen together, oldest first)
    for (int u = 0; u < unison; ++u) {
        const float x = unison > 1 ? (float)u / (float)(unison - 1) * 2.0f - 1.0f : 0.0f;  // -1..1
        Voice* v = allocateVoice(part);
        v->unisonDetune = x * 0.5f * ps.p[P::UnisonDetune] / 100.0f;
        v->unisonPan = x * ps.p[P::UnisonSpread];
        v->start(e, ps.ctx, age, rng_.unipolar(), glideFrom);
        v->part = part;
        v->matchKey = e.key;
        int groups[8];
        const int ng = v->startedGroups(groups, 8);
        for (int g = 0; g < ng; ++g)
            for (auto& o : voices_)
                if (&o != v && o.active() && o.part == part && o.age() != age) o.choke(groups[g]);
    }
}

bool Engine::monoNoteOff(int part, const Event& e) {
    PartState& ps = parts_[part];
    if ((VoiceMode)(int)ps.p[P::VoiceMode] == VoiceMode::Poly || ps.stackSize == 0) return false;
    int found = -1;
    for (int i = 0; i < ps.stackSize; ++i) {
        const Event& s = ps.stack[i];
        if (e.noteId != 0 ? s.noteId == e.noteId : s.channel == e.key) { found = i; break; }
    }
    if (found < 0) return false;
    const bool wasTop = found == ps.stackSize - 1;
    for (int i = found + 1; i < ps.stackSize; ++i) ps.stack[i - 1] = ps.stack[i];
    --ps.stackSize;
    if (!wasTop) return true;  // a note under the sounding one: just forget it
    if (ps.stackSize > 0) {
        // return to the previous held note (legato in both modes, gliding if enabled)
        const Event& top = ps.stack[ps.stackSize - 1];
        const int rawKey = top.channel;  // the stack keeps the untransposed key in .channel
        Event again = top;
        again.channel = kOmniChannel;
        const bool glideOn = ps.p[P::Glide] > 0.0005f;
        bool any = false;
        for (auto& v : voices_)
            if (v.active() && !v.released() && v.part == part && v.noteId() == ps.monoId) {
                v.legatoTo(again, glideOn);
                v.matchKey = rawKey;
                any = true;
            }
        ps.monoId = again.noteId;
        ps.monoKey = rawKey;
        ps.lastPitch = (float)again.key;
        if (!any) {
            // the voice was stolen meanwhile: start the returning note afresh
            startNote(part, again, -1.0f);
            for (auto& v : voices_)
                if (v.active() && v.part == part && v.noteId() == again.noteId && v.age() == ageCounter_) v.matchKey = rawKey;
        }
        return true;
    }
    // last key released: normal note-off for the sounding voices
    for (auto& v : voices_) {
        if (!v.active() || v.released() || v.sustained || v.part != part || v.noteId() != ps.monoId) continue;
        if (ps.sustain || v.hold) v.sustained = true;
        else v.release();
    }
    return true;
}

void Engine::noteOff(const Event& e, int part) {
    bool mono[kNumParts] = {};  // part handled the note-off through its note stack
    for (int k = 0; k < kNumParts; ++k) {
        if (part >= 0 && k != part) continue;
        if ((VoiceMode)(int)parts_[k].p[P::VoiceMode] == VoiceMode::Poly) continue;
        // notes the stack does not know (started before a switch to mono) are
        // released by the normal path below
        mono[k] = monoNoteOff(k, e);
    }
    for (auto& v : voices_) {
        if (!v.active() || v.released() || v.sustained) continue;
        if (part >= 0 && v.part != part) continue;
        if (mono[v.part] && v.noteId() == parts_[v.part].monoId) continue;  // handled by the note stack
        const bool match = e.noteId != 0 ? v.noteId() == e.noteId : v.matchKey == e.key;
        if (!match) continue;
        if (parts_[v.part].sustain || v.hold) v.sustained = true;
        else v.release();
    }
}

void Engine::perNote(const Event& e, int part) {
    for (auto& v : voices_) {
        if (!v.active() || v.stolen()) continue;
        if (part >= 0 && v.part != part) continue;
        if (e.noteId != 0 ? v.noteId() != e.noteId : v.matchKey != e.key) continue;
        if (e.type == EventType::NoteHold) {
            v.hold = e.value > 0.5f;
            if (!v.hold && v.sustained && !parts_[v.part].sustain) v.release();
        } else {
            v.applyExpression(e.type, e.value);
        }
    }
}

void Engine::releaseSustained(int part) {
    for (auto& v : voices_)
        if (v.active() && v.part == part && v.sustained && !v.hold) v.release();
}

void Engine::partEvent(int part, const Event& e) {
    PartState& ps = parts_[part];
    if ((e.type == EventType::NoteOn || e.type == EventType::NoteOff) && arpInput(part, e)) return;
    switch (e.type) {
        case EventType::NoteOn: noteOn(part, e); break;
        case EventType::NoteOff: noteOff(e, part); break;
        case EventType::NotePitch:
        case EventType::NotePressure:
        case EventType::NoteTimbre:
        case EventType::NoteX:
        case EventType::NoteHold: perNote(e, part); break;
        case EventType::PitchBend: ps.pitchBend = clampf(e.value, -1.0f, 1.0f); break;
        case EventType::ModWheel:
            ps.ctx.modWheel = clampf(e.value, 0.0f, 1.0f);
            for (int i = 0; i < 4; ++i)
                if ((int)ps.p[P::CtlCc1 + i] == 1) ps.ctx.cc[i] = ps.ctx.modWheel;
            break;
        case EventType::ChannelPressure: ps.ctx.chanPressure = clampf(e.value, 0.0f, 1.0f); break;
        case EventType::Sustain: {
            const bool on = e.value >= 0.5f;
            if (ps.sustain && !on) { ps.sustain = false; releaseSustained(part); }
            ps.sustain = on;
            break;
        }
        case EventType::Controller:
            for (int i = 0; i < 4; ++i)
                if ((int)ps.p[P::CtlCc1 + i] == (int)e.controller) ps.ctx.cc[i] = clampf(e.value, 0.0f, 1.0f);
            switch (e.controller) {
                case 1: ps.ctx.modWheel = clampf(e.value, 0.0f, 1.0f); break;
                case 2: ps.ctx.breath = clampf(e.value, 0.0f, 1.0f); break;
                case 7: if (multiActive_) ps.ccVolume = clampf(e.value, 0.0f, 1.0f); break;
                case 10: if (multiActive_) ps.ccPan = clampf(e.value * 2.0f - 1.0f, -1.0f, 1.0f); break;
                case 11: ps.ctx.expression = clampf(e.value, 0.0f, 1.0f); break;
                case 64: partEvent(part, Event::control(EventType::Sustain, e.value)); break;
                case 120: arpStop(part); ps.stackSize = 0; for (auto& v : voices_) if (v.part == part) v.hardStop(); break;
                case 123: arpStop(part); ps.stackSize = 0; for (auto& v : voices_) if (v.active() && v.part == part) v.release(); break;
                default: break;
            }
            break;
        case EventType::AllNotesOff:
            arpStop(part);
            ps.sustain = false;
            ps.stackSize = 0;
            for (auto& v : voices_) if (v.active() && v.part == part) v.release();
            break;
        case EventType::AllSoundOff:
            arpStop(part);
            ps.stackSize = 0;
            for (auto& v : voices_) if (v.part == part) v.hardStop();
            break;
        case EventType::ProgramChange:  // handled by the host
        default: break;
    }
}

void Engine::handleEvent(const Event& e) {
    if (e.type == EventType::Looper) { rack_.looperCommand((fx::LooperCmd)e.controller); return; }
    if (e.type == EventType::Clock) { clockEvent((ClockMsg)e.controller); return; }
    if (!multiActive_) { partEvent(0, e); return; }
    const bool panic = e.type == EventType::AllNotesOff || e.type == EventType::AllSoundOff;
    const bool omni = e.channel == kOmniChannel;
    // Note-id matched note-offs / expression find their voices in any part, so
    // notes end correctly even if the part's channel (or the selected part for
    // omni input) changed while they were held.
    const bool byId = e.noteId != 0 && e.type != EventType::NoteOn && e.type <= EventType::NoteHold;
    if (byId) {
        if (e.type == EventType::NoteOff) {
            for (int k = 0; k < kNumParts; ++k)
                if (omni || (int)mix_[k][Mix::Channel] - 1 == (e.channel & 15)) arpInput(k, e);
            noteOff(e, -1);
        } else {
            perNote(e, -1);
        }
        return;
    }
    if (omni) {
        if (panic) for (int k = 0; k < kNumParts; ++k) partEvent(k, e);
        else partEvent(selectedPart_.load(std::memory_order_relaxed), e);
        return;
    }
    for (int k = 0; k < kNumParts; ++k)
        if ((int)mix_[k][Mix::Channel] - 1 == (e.channel & 15)) partEvent(k, e);
}

// ------------------------------------------------------------ arpeggiator

bool Engine::arpInput(int part, const Event& e) {
    PartState& ps = parts_[part];
    PartState::Arp& a = ps.arp;
    if (ps.p[P::ArpOn] <= 0.5f) {
        if (a.running || a.heldCount) arpStop(part);
        return false;
    }
    const bool latch = ps.p[P::ArpLatch] > 0.5f;
    if (e.type == EventType::NoteOn && e.value > 0.0f) {
        if (latch && a.physical == 0) a.heldCount = 0;  // new chord replaces the latched one
        ++a.physical;
        int w = 0;
        for (int i = 0; i < a.heldCount; ++i)
            if (a.held[i].key != e.key) a.held[w++] = a.held[i];
        a.heldCount = w;
        if (a.heldCount < PartState::Arp::kMax) a.held[a.heldCount++] = e;
        return true;
    }
    // note-off (or velocity 0)
    bool found = false;
    for (int i = 0; i < a.heldCount; ++i) {
        const Event& h = a.held[i];
        if (e.noteId != 0 ? h.noteId == e.noteId : h.key == e.key) { found = true; break; }
    }
    if (!found) return false;  // a note from before the arp was switched on
    a.physical = std::max(0, a.physical - 1);
    if (!latch) {
        int w = 0;
        for (int i = 0; i < a.heldCount; ++i) {
            const Event& h = a.held[i];
            if (!(e.noteId != 0 ? h.noteId == e.noteId : h.key == e.key)) a.held[w++] = h;
        }
        a.heldCount = w;
    }
    return true;
}

void Engine::arpStop(int part) {
    PartState::Arp& a = parts_[part].arp;
    for (int i = 0; i < a.pendCount; ++i) noteOff(Event::noteOff(a.pend[i].id, a.pend[i].key), part);
    a.pendCount = 0;
    a.heldCount = a.physical = 0;
    a.running = false;
}

void Engine::runArps(int frameOffset, int frames) {
    const float tempo = rack_.tempo();
    const double beatsPerFrame = (double)tempo / 60.0 / (double)sampleRate_;
    const bool clock = rack_.clockSyncEnabled() && statClockBpm_.load(std::memory_order_relaxed) > 0.0f;
    const double now = clock ? rack_.beat() + beatsPerFrame * frameOffset : arpBeat_;
    const int parts = multiActive_ ? kNumParts : 1;
    for (int k = 0; k < parts; ++k) {
        PartState& ps = parts_[k];
        PartState::Arp& a = ps.arp;
        if (ps.p[P::ArpOn] <= 0.5f) {
            if (a.running || a.heldCount || a.pendCount) arpStop(k);
            continue;
        }
        // gate ends
        for (int i = 0; i < a.pendCount;) {
            if (now >= a.pend[i].offBeat) {
                noteOff(Event::noteOff(a.pend[i].id, a.pend[i].key), k);
                a.pend[i] = a.pend[--a.pendCount];
            } else {
                ++i;
            }
        }
        // latch switched off while nothing is held: drop the latched notes
        if (ps.p[P::ArpLatch] <= 0.5f && a.physical == 0) a.heldCount = 0;
        // MIDI clock transport stopped: the beat stands still, so stop playing
        if (clock && !clockRunning_) {
            for (int i = 0; i < a.pendCount; ++i) noteOff(Event::noteOff(a.pend[i].id, a.pend[i].key), k);
            a.pendCount = 0;
            a.running = false;
            continue;
        }
        if (a.heldCount == 0) { a.running = false; continue; }
        const float stepBeats = fx::syncBeats((int)ps.p[P::ArpRate] + 1);  // arp.rate has no "off" entry
        if (a.running) {
            // time base jumped (clock start / appearing / lost): restart the pattern grid
            const double next = a.origin + (double)a.step * stepBeats;
            if (now < next - 2.0 * stepBeats || now > next + 4.0 * stepBeats) {
                a.running = false;
                // gate ends refer to the old time base: end those notes now
                for (int i = 0; i < a.pendCount; ++i) noteOff(Event::noteOff(a.pend[i].id, a.pend[i].key), k);
                a.pendCount = 0;
            }
        }
        if (!a.running) {
            a.running = true;
            a.origin = clock ? std::floor(now / stepBeats) * stepBeats : now;  // MIDI clock: stay on the grid
            if (clock && now - a.origin > 1e-6) a.origin += stepBeats;          // ... from the next step
            a.step = 0;
            a.index = 0;
            a.dir = 1;
        }
        const double swing = ps.p[P::ArpSwing];
        for (int guard = 0; guard < 8; ++guard) {  // a few steps per control block at most
            const double t = a.origin + (double)a.step * stepBeats + ((a.step & 1) ? swing * stepBeats : 0.0);
            if (now + 1e-9 < t) break;
            // pattern: held notes x octaves
            int keys[PartState::Arp::kMax * 4];
            float vels[PartState::Arp::kMax * 4];
            int n = 0;
            const auto mode = (ArpMode)(int)ps.p[P::ArpMode];
            Event sorted[PartState::Arp::kMax];
            for (int i = 0; i < a.heldCount; ++i) sorted[i] = a.held[i];
            if (mode != ArpMode::Order)
                std::sort(sorted, sorted + a.heldCount, [](const Event& x, const Event& y) { return x.key < y.key; });
            const int oct = std::max(1, std::min(4, (int)ps.p[P::ArpOctaves]));
            for (int o = 0; o < oct; ++o)
                for (int i = 0; i < a.heldCount; ++i) {
                    const int key = sorted[i].key + 12 * o;
                    if (key > 127) continue;
                    keys[n] = key;
                    vels[n++] = sorted[i].value;
                }
            int chosen[PartState::Arp::kMax * 4];
            int nc = 0;
            if (n > 0) {
                switch (mode) {
                    case ArpMode::Down: chosen[nc++] = n - 1 - (a.index % n); ++a.index; break;
                    case ArpMode::UpDown: {
                        if (n == 1) { chosen[nc++] = 0; break; }
                        const int period = 2 * n - 2;
                        const int ph = a.index % period;
                        chosen[nc++] = ph < n ? ph : period - ph;
                        ++a.index;
                        break;
                    }
                    case ArpMode::Random: chosen[nc++] = (int)(a.rng.unipolar() * (float)n) % n; break;
                    case ArpMode::Chord:
                        for (int i = 0; i < n && i < PartState::Arp::kMax; ++i) chosen[nc++] = i;
                        break;
                    default: chosen[nc++] = a.index % n; ++a.index; break;  // up / order
                }
            }
            const double len = std::max(0.02, (double)ps.p[P::ArpGate]) * stepBeats;
            for (int c = 0; c < nc && a.pendCount < PartState::Arp::kMax; ++c) {
                const uint32_t id = 0xA0000000u | ((uint32_t)k << 24) | (a.nextId++ & 0xFFFFFFu);
                Event on = Event::noteOn(id, keys[chosen[c]], vels[chosen[c]]);
                on.channel = kOmniChannel;
                noteOn(k, on);
                a.pend[a.pendCount++] = {id, (uint8_t)keys[chosen[c]], t + len};
            }
            ++a.step;
        }
    }
    if (!clock) arpBeat_ += beatsPerFrame * frames;
}

void Engine::clockEvent(ClockMsg m) {
    switch (m) {
        case ClockMsg::Tick:
            tickTimes_[tickHead_] = samplePos_;
            tickHead_ = (tickHead_ + 1) % 25;
            tickCount_ = std::min(25, tickCount_ + 1);
            if (clockRunning_) ++ticksSinceStart_;
            if (tickCount_ == 25) {
                // 24 ticks = one quarter note; timestamps are block-quantised, so
                // measure over a whole beat and smooth
                const uint64_t newest = tickTimes_[(tickHead_ + 24) % 25], oldest = tickTimes_[tickHead_];
                if (newest > oldest) {
                    const float bpm = 60.0f * sampleRate_ / (float)(newest - oldest);
                    clockBpm_ = clockBpm_ <= 0.0f || std::fabs(bpm - clockBpm_) > 0.1f * clockBpm_ ? bpm
                                                                                                  : clockBpm_ + 0.2f * (bpm - clockBpm_);
                }
            }
            break;
        case ClockMsg::Start: ticksSinceStart_ = 0; clockRunning_ = true; break;
        case ClockMsg::Continue: clockRunning_ = true; break;
        case ClockMsg::Stop: clockRunning_ = false; break;
    }
}

void Engine::updateClock(int n) {
    const uint64_t last = tickCount_ ? tickTimes_[(tickHead_ + 24) % 25] : 0;
    const bool active = tickCount_ == 25 && clockBpm_ > 0.0f && samplePos_ - last < (uint64_t)(0.5f * sampleRate_);
    if (!active && tickCount_ > 0 && samplePos_ - last >= (uint64_t)(0.5f * sampleRate_)) {
        tickCount_ = 0;  // clock lost: measure afresh when it comes back
        clockBpm_ = 0.0f;
    }
    // beat position: whole ticks since start plus the time since the last tick
    const double tickLen = active ? 60.0 * sampleRate_ / (clockBpm_ * 24.0) : 1.0;
    const double frac = active ? std::min(0.999, (double)(samplePos_ - last) / tickLen) : 0.0;
    rack_.setExternalClock(active, clockBpm_, ((double)ticksSinceStart_ + frac) / 24.0);
    statClockBpm_.store(active ? clockBpm_ : 0.0f, std::memory_order_relaxed);
    samplePos_ += (uint64_t)n;
}

void Engine::updateEffects() {
    const int q = quality_.load(std::memory_order_relaxed);
    effectsQuality_ = q;
    // Low: no oversampling, Normal: 2x, High: 4x (drive / amp stages)
    rack_.paramsChanged(p_, q == (int)Quality::Low ? 1 : q == (int)Quality::High ? 4 : 2, q == (int)Quality::High);
    limiter_.enabled = p_[P::Limiter] > 0.5f;
    for (int k = 0; k < kNumParts; ++k) updateInserts(k);
}

void Engine::updateInserts(int part) {
    const int q = effectsQuality_;
    inserts_[part].paramsChanged(parts_[part].p, q == (int)Quality::Low ? 1 : q == (int)Quality::High ? 4 : 2,
                                 q == (int)Quality::High);
}

void Engine::updatePartTargets() {
    for (int k = 0; k < kNumParts; ++k) {
        PartState& ps = parts_[k];
        const float* m = mix_[k];
        const float vol = m[Mix::Mute] > 0.5f ? 0.0f : m[Mix::Volume] * m[Mix::Volume] * ps.ccVolume * ps.ccVolume;
        const float pan = clampf(m[Mix::Pan] + ps.ccPan, -1.0f, 1.0f);
        const float gl = vol * (pan > 0.0f ? 1.0f - pan : 1.0f);
        const float gr = vol * (pan < 0.0f ? 1.0f + pan : 1.0f);
        const float send = m[Mix::FxSend];
        ps.target[0] = gl * send;
        ps.target[1] = gr * send;
        ps.target[2] = gl * (1.0f - send);
        ps.target[3] = gr * (1.0f - send);
    }
}

void Engine::swapSampleSet(int part, int layer, SampleSet* s) {
    SampleSet* old = currentSets_[part][layer];
    keyswitch_[part][layer] = -1;
    seqCount_[part][layer] = 0;
    if (!old) {
        currentSets_[part][layer] = s;
        return;
    }
    bool playing = false;
    for (auto& v : voices_) playing = playing || (v.part == part && v.playsLayer(layer));
    // Voices still playing the old set fade out (no click) while it stays alive
    // in retiring_; with nothing playing it, or no free slot, it goes to the
    // garbage queue right away (sounding voices then stop at once).
    int slot = -1;
    for (int i = 0; i < kRetiring && playing; ++i)
        if (!retiring_[i].set) { slot = i; break; }
    if (slot >= 0) {
        for (auto& v : voices_)
            if (v.part == part) v.fadeLayer(layer);
        retiring_[slot].set = old;
        retiring_[slot].frames = (int)(2.0f * Voice::kSwapFadeSeconds * sampleRate_) + kControlBlock;
        currentSets_[part][layer] = s;
        return;
    }
    if (!retired_.push(old)) {
        // garbage queue full (collectGarbage not called): put the new set back
        // and retry next block
        SampleSet* expected = nullptr;
        if (!pendingSets_[part][layer].compare_exchange_strong(expected, s)) retired_.push(s);
        return;
    }
    for (auto& v : voices_)
        if (v.part == part) v.stopLayer(layer);
    currentSets_[part][layer] = s;
}

void Engine::advanceRetiring(int n) {
    for (auto& r : retiring_) {
        if (!r.set) continue;
        r.frames -= n;
        if (r.frames <= 0 && retired_.push(r.set)) r.set = nullptr;  // queue full: retry next block
    }
}

// Multi-timbral render: voices go into per-part buses, which run through the
// part's insert effects and are mixed (volume / pan / FX send) into the rack
// bus (L/R) and the dry bus.
void Engine::processMulti(float* L, float* R, int n) {
    std::memset(dryL_, 0, sizeof(float) * n);
    std::memset(dryR_, 0, sizeof(float) * n);
    bool used[kNumParts] = {};
    bool arpActive = false;
    for (const auto& ps : parts_) arpActive = arpActive || ps.p[P::ArpOn] > 0.5f || ps.arp.pendCount > 0;
    for (int off = 0; off < n; off += kControlBlock) {
        const int m = std::min(kControlBlock, n - off);
        if (arpActive) runArps(off, m);
        for (int k = 0; k < kNumParts; ++k) {
            PartState& ps = parts_[k];
            ps.ctx.lfo2 = ps.lfo2.advance((LfoWave)(int)ps.p[P::Lfo2Wave], lfo2Rate(ps.p), (float)m / sampleRate_,
                                          k == 0 ? rng_ : ps.rng);
        }
        for (auto& v : voices_) {
            if (!v.active()) continue;
            const int k = v.part;
            if (!used[k]) {
                used[k] = true;
                std::memset(bus(k, 0), 0, sizeof(float) * n);
                std::memset(bus(k, 1), 0, sizeof(float) * n);
            }
            v.render(bus(k, 0) + off, bus(k, 1) + off, m, parts_[k].ctx);
        }
    }
    const float step = gainStep_;
    for (int k = 0; k < kNumParts; ++k) {
        PartState& ps = parts_[k];
        fx::InsertChain& ins = inserts_[k];
        if (!used[k] && ins.busy()) {  // let insert tails ring out
            used[k] = true;
            std::memset(bus(k, 0), 0, sizeof(float) * n);
            std::memset(bus(k, 1), 0, sizeof(float) * n);
        }
        if (!used[k]) {
            for (int c = 0; c < 4; ++c) ps.gain[c] += clampf(ps.target[c] - ps.gain[c], -step * n, step * n);
            continue;
        }
        float* bl = bus(k, 0);
        float* br = bus(k, 1);
        if (!ins.empty()) {
            ins.setTempo(rack_.tempo(), rack_.beat());
            ins.process(bl, br, n);
        }
        bool steady = true;
        for (int c = 0; c < 4; ++c) steady = steady && ps.gain[c] == ps.target[c];
        if (steady) {
            const float* g = ps.gain;
            for (int i = 0; i < n; ++i) {
                L[i] += bl[i] * g[0];
                R[i] += br[i] * g[1];
                dryL_[i] += bl[i] * g[2];
                dryR_[i] += br[i] * g[3];
            }
            continue;
        }
        float* g = ps.gain;
        const float* t = ps.target;
        for (int i = 0; i < n; ++i) {  // linear ramps toward the targets
            for (int c = 0; c < 4; ++c) g[c] += clampf(t[c] - g[c], -step, step);
            L[i] += bl[i] * g[0];
            R[i] += br[i] * g[1];
            dryL_[i] += bl[i] * g[2];
            dryR_[i] += br[i] * g[3];
        }
    }
}

void Engine::process(float* L, float* R, int n) {
    const auto t0 = std::chrono::steady_clock::now();
    enableFlushToZero();
    if (!prepared_ || n <= 0) {
        if (n > 0) { std::memset(L, 0, sizeof(float) * n); std::memset(R, 0, sizeof(float) * n); }
        return;
    }
    if (n > kMaxBlockSize && (multiActive_ || multi_.load(std::memory_order_relaxed))) {
        // the dry bus holds kMaxBlockSize frames
        for (int off = 0; off < n; off += kMaxBlockSize) process(L + off, R + off, std::min(kMaxBlockSize, n - off));
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
    for (int k = 1; k < kNumParts; ++k) {
        const uint32_t pv = partVersion_[k].load(std::memory_order_acquire);
        if (pv == seenPartVersion_[k]) continue;
        seenPartVersion_[k] = pv;
        float* dst = partP_.get() + (size_t)(k - 1) * P::kPartParams;
        const std::atomic<float>* src = partShared_.get() + (size_t)(k - 1) * P::kPartParams;
        for (int i = 0; i < P::kPartParams; ++i) dst[i] = src[i].load(std::memory_order_relaxed);
        updateInserts(k);
    }
    const uint32_t mv = mixVersion_.load(std::memory_order_acquire);
    if (mv != seenMixVersion_) {
        seenMixVersion_ = mv;
        for (int k = 0; k < kNumParts; ++k)
            for (int i = 0; i < Mix::Count; ++i) mix_[k][i] = mixShared_[k][i].load(std::memory_order_relaxed);
    }
    const bool multi = multi_.load(std::memory_order_relaxed);
    if (multi != multiActive_) {
        multiActive_ = multi;
        if (!multi) {
            // back to single-part mode: only part 0 may sound
            for (auto& v : voices_) if (v.part != 0) v.hardStop();
        }
        for (auto& ps : parts_) {
            ps.ccVolume = 1.0f;
            ps.ccPan = 0.0f;
        }
        updatePartTargets();
        for (auto& ps : parts_) std::memcpy(ps.gain, ps.target, sizeof ps.gain);
    }
    for (int t = 0; t < 2; ++t) {
        if (!pendingIr_[t].load(std::memory_order_acquire)) continue;
        if (currentIr_[t] && !retiredIr_.push(currentIr_[t])) continue;  // garbage queue full: retry next block
        fx::ConvIr* p = pendingIr_[t].exchange(nullptr, std::memory_order_acq_rel);
        currentIr_[t] = p == &unloadIr_ ? nullptr : p;
        rack_.setUserIr(t, currentIr_[t]);
    }

    // 2. sample set swaps
    for (int k = 0; k < kNumParts; ++k)
        for (int l = 0; l < kNumLayers; ++l) {
            if (!pendingSets_[k][l].load(std::memory_order_relaxed)) continue;
            SampleSet* s = pendingSets_[k][l].exchange(nullptr, std::memory_order_acq_rel);
            if (s) swapSampleSet(k, l, s);
        }

    // 3. events (voices started here read the part contexts)
    for (int k = 0; k < kNumParts; ++k) {
        parts_[k].ctx.params = parts_[k].p;
        for (int l = 0; l < kNumLayers; ++l) parts_[k].ctx.sampleSets[l] = currentSets_[k][l];
    }
    Event e;
    int budget = 512;
    while (budget-- > 0 && events_.pop(e)) handleEvent(e);

    updateClock(n);

    // 4. render
    for (auto& ps : parts_) {
        ps.ctx.tempo = rack_.tempo();
        ps.ctx.quality = quality_.load(std::memory_order_relaxed);
        ps.ctx.masterTune = p_[P::MasterTune] * 0.01f;
        ps.ctx.pitchBendNorm = ps.pitchBend;
        ps.ctx.pitchBendSemis = ps.pitchBend * ps.p[P::BendRange];
    }
    VoiceContext& ctx0 = parts_[0].ctx;

    std::memset(L, 0, sizeof(float) * n);
    std::memset(R, 0, sizeof(float) * n);
    if (multiActive_) {
        updatePartTargets();
        processMulti(L, R, n);
    } else {
        const bool arpActive = p_[P::ArpOn] > 0.5f || parts_[0].arp.pendCount > 0;
        for (int off = 0; off < n; off += kControlBlock) {
            const int m = std::min(kControlBlock, n - off);
            if (arpActive) runArps(off, m);
            ctx0.lfo2 = parts_[0].lfo2.advance((LfoWave)(int)p_[P::Lfo2Wave], lfo2Rate(p_), (float)m / sampleRate_, rng_);
            for (auto& v : voices_)
                if (v.active()) v.render(L + off, R + off, m, ctx0);
        }
        if (!inserts_[0].empty()) {
            inserts_[0].setTempo(rack_.tempo(), rack_.beat());
            inserts_[0].process(L, R, n);
        }
    }

    // 5. effect rack, then master. The rack's FX modulation follows part 0's
    // controllers (mod wheel, LFO2, ...).
    bool held = false;
    for (auto& v : voices_)
        if (v.active() && !v.released()) { held = true; break; }
    fx::FxGlobals g;
    g.modWheel = ctx0.modWheel;
    g.lfo2 = ctx0.lfo2;
    g.chanPressure = ctx0.chanPressure;
    g.expression = ctx0.expression;
    g.breath = ctx0.breath;
    g.pitchBend = parts_[0].pitchBend;
    g.notesHeld = held;
    rack_.process(L, R, n, g, inL_, inR_);
    inL_ = inR_ = nullptr;
    if (multiActive_) {
        for (int i = 0; i < n; ++i) {
            L[i] += dryL_[i];
            R[i] += dryR_[i];
        }
    }
    const float target = p_[P::MasterVolume] * p_[P::MasterVolume];  // perceptual taper
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float mg = masterGain_.process(target);
        L[i] *= mg;
        R[i] *= mg;
    }
    limiter_.process(L, R, n);
    for (int i = 0; i < n; ++i) peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));

    advanceRetiring(n);

    // 6. stats
    int active = 0;
    int perPart[kNumParts] = {};
    for (auto& v : voices_)
        if (v.active()) { ++active; ++perPart[v.part]; }
    for (int k = 0; k < kNumParts; ++k) statPartVoices_[k].store(perPart[k], std::memory_order_relaxed);
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
