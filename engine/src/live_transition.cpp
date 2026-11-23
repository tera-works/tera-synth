#include "hsynth/engine.h"

#include <algorithm>
#include <cmath>

#include "hsynth/fx/ir_load.h"

namespace hsynth {
namespace {
void resampleLiveIr(Engine::LiveChange& c, float rate) {
    if (c.irSampleRate == 0.0f || c.irSampleRate == rate) return;
    for (int t = 0; t < 2; ++t) {
        if (!c.ir[t] || c.ir[t]->path.empty()) continue;
        std::string error;
        auto ir = fx::loadImpulseResponse(c.ir[t]->path, rate, t, error);
        if (ir) c.ir[t] = std::move(ir);
    }
    c.irSampleRate = rate;
}
}  // namespace

struct Engine::LiveFx {
    std::unique_ptr<fx::FxRack> rack = std::make_unique<fx::FxRack>();
    std::unique_ptr<fx::FxBlock> sends[2] = {fx::createBlock(fx::Reverb), fx::createBlock(fx::Delay)};
    int quiet[2] = {-1, -1};
    float level[2] = {};
    fx::FxGlobals globals;
    float wet[2][kMaxBlockSize], send[2][2][kMaxBlockSize];
    bool voices = false;
    int fadeFrames = 0, fadeLength = 1;
};

Engine::LiveChange::LiveChange() = default;
Engine::LiveChange::~LiveChange() = default;

void Engine::setSstTimeMs(float ms) {
    if (std::isfinite(ms)) sstTimeMs_.store(clampf(ms, 50.0f, 2000.0f), std::memory_order_relaxed);
}

bool Engine::stageLiveChange(std::unique_ptr<LiveChange> c, uint32_t* serial) {
    if (!c) return false;
    collectGarbage();
    std::lock_guard<std::mutex> loaderLock(loaderMutex_);
    if (liveIncoming_.load(std::memory_order_acquire)) return false;
    c->sampleEpoch = sampleEpoch_.load(std::memory_order_relaxed);
    const int q = quality_.load(std::memory_order_relaxed);
    const int os = q == (int)Quality::Low ? 1 : q == (int)Quality::High ? 4 : 2;
    const bool hq = q == (int)Quality::High;
    bool any = false;
    for (int k = 0; k < kNumParts; ++k) {
        auto& part = c->parts[k];
        if (!part) continue;
        any = true;
        for (int i = 0; i < P::kPartParams; ++i) part->params[i] = sanitizeParam(i, part->params[i]);
        for (auto* set : part->sets) {
            if (!set || set->streamedSamples() == 0) continue;
            streamer_.start();
            for (auto& sample : set->samples)
                if (sample->streamed) sample->streamer = &streamer_;
        }
        auto insert = std::make_unique<fx::InsertChain>();
        insert->prepare(sampleRate_);
        insert->paramsChanged(part->params, os, hq);
        c->inserts[k] = std::move(insert);
        if (c->multi)
            for (int i = 0; i < Mix::Count; ++i) c->mix[k][i] = sanitizePartMix(i, c->mix[k][i]);
    }
    if (c->hasGlobals) {
        for (int i = 0; i < P::kNumParams; ++i) c->globals[i] = sanitizeParam(i, c->globals[i]);
        resampleLiveIr(*c, sampleRate_);
        c->fx = std::make_unique<LiveFx>();
        c->fx->rack->prepare(sampleRate_, false);
        for (auto& send : c->fx->sends) send->prepare(sampleRate_);
        for (int t = 0; t < 2; ++t) c->fx->rack->setUserIr(t, c->ir[t].get());
        c->fx->rack->paramsChanged(c->globals, os, hq);
    }
    if (!any && !c->hasGlobals) return false;
    {
        std::lock_guard<std::mutex> namesLock(nameMutex_);
        for (int k = 0; k < kNumParts; ++k) {
            if (!c->parts[k]) continue;
            for (int l = 0; l < kNumLayers; ++l) {
                setNames_[k][l] = c->parts[k]->setNames[l];
                setPaths_[k][l] = c->parts[k]->setPaths[l];
            }
        }
        if (c->hasGlobals)
            for (int t = 0; t < 2; ++t) {
                irNames_[t] = c->ir[t] ? c->ir[t]->name : "";
                irPaths_[t] = c->ir[t] ? c->ir[t]->path : "";
            }
    }
    if (++liveSerial_ == 0) ++liveSerial_;
    c->serial = liveSerial_;
    if (serial) *serial = c->serial;
    liveIncoming_.store(c.release(), std::memory_order_release);
    return true;
}

bool Engine::applyLiveChange() {
    if (!liveWaiting_) liveWaiting_ = liveIncoming_.exchange(nullptr, std::memory_order_acq_rel);
    auto* c = liveWaiting_;
    if (!c) return false;
    if (c->sampleEpoch != sampleEpoch_.load(std::memory_order_relaxed)) {
        if (liveRetired_.push(c)) liveWaiting_ = nullptr;
        return false;
    }

    int slot = -1, graphs = 0;
    for (int i = 0; i < kGhosts; ++i) {
        if (!liveRetained_[i]) slot = i;
        else if (liveRetained_[i]->fx) ++graphs;
    }
    if (slot < 0 || (c->fx && graphs >= kLiveGraphs)) return false;

    bool sounding[kNumParts] = {};
    for (const auto& v : voices_)
        if (v.active() && v.liveGhost < 0) sounding[v.part] = true;
    int rows[kNumParts], required = 0;
    std::fill_n(rows, kNumParts, -1);
    for (int k = 0, next = 0; k < kNumParts; ++k) {
        if (!c->parts[k]) continue;
        for (auto* set : currentSets_[k]) required += set != nullptr;
        if (!sounding[k] && !inserts_[k]->busy() && !parts_[k].piano.busy()) continue;
        while (next < kGhosts && ghostUsed_[next]) ++next;
        if (next == kGhosts) return false;
        rows[k] = next++;
    }
    // A commit is all-or-nothing. The outgoing transaction and every old
    // sample remain owned until the last callback that can use them.
    int freeSets = 0;
    for (const auto* set : held_) freeSets += set == nullptr;
    if (required > freeSets) return false;

    auto* oldFx = c->fx.get();
    if (oldFx) {
        for (auto& ghost : liveGhosts_)
            if (ghost.owner && !ghost.fx) ghost.fx = oldFx;
        oldFx->level[0] = p_[P::fxChain(P::SendALevel)];
        oldFx->level[1] = p_[P::fxChain(P::SendBLevel)];
        oldFx->quiet[0] = sendAQuiet_;
        oldFx->quiet[1] = sendBQuiet_;
        oldFx->fadeFrames = sstFxFrames_;
        oldFx->fadeLength = sstFxLength_;
        oldFx->globals.modWheel = parts_[0].ctx.modWheel;
        oldFx->globals.expression = parts_[0].ctx.expression;
        oldFx->globals.chanPressure = parts_[0].ctx.chanPressure;
        oldFx->globals.breath = parts_[0].ctx.breath;
        oldFx->globals.lfo2 = parts_[0].ctx.lfo2;
        oldFx->globals.pitchBend = parts_[0].pitchBend;
    }

    for (int k = 0; k < kNumParts; ++k) {
        auto& part = c->parts[k];
        if (!part) continue;
        auto& ps = parts_[k];
        arpStop(k);
        seqStop(k);
        stopStrum(k);
        ps.stackSize = 0;
        ps.monoKey = -1;
        ps.lastPitch = -1.0f;
        if (rows[k] >= 0) {
            const int g = rows[k];
            ghostUsed_[g] = true;
            auto& ghost = liveGhosts_[g];
            ghost.owner = c;
            ghost.fx = oldFx;
            ghost.part = k;
            ghost.channel = multiActive_ ? (int)mix_[k][Mix::Channel] - 1 : kOmniChannel;
            ghost.ctx = ps.ctx;
            ghost.logicalPedal = ps.logicalPedal;
            ghost.piano = ps.piano;
            ghost.hardwarePedals = hardwarePedalChannels_[k];
            ghost.halfPedalChannels = 0;
            for (int ch = 0; ch < 16; ++ch)
                if (hardwareHalfParts_[ch] & (1u << k)) ghost.halfPedalChannels |= 1u << ch;
            ghost.ctx.tempo = rack_->tempo();
            ghost.ctx.masterTune = p_[P::MasterTune] * 0.01f;
            ghost.ctx.superKnob = p_[P::SuperKnob];
            ghost.ctx.pedal = ps.pedal;
            ghost.lfo = ps.lfo2;
            ghost.rng = k == 0 ? rng_ : ps.rng;
            if (multiActive_) std::copy_n(ps.gain, 4, ghost.gains);
            else {
                ghost.gains[0] = ghost.gains[1] = ps.singleGain;
                ghost.gains[2] = ghost.gains[3] = 0;
            }
            ghost.sendA = std::min(1.0f, ps.p[P::SendA] + ps.revSend);
            ghost.sendB = std::min(1.0f, ps.p[P::SendB] + ps.dlySend);
            float* params = ghostParams_.get() + (size_t)g * P::kPartParams;
            std::copy_n(ps.p, P::kPartParams, params);
            ghost.ctx.params = params;
            for (auto& v : voices_) {
                if (!v.active() || v.part != k || v.liveGhost >= 0) continue;
                if (!v.paramsOverride) v.paramsOverride = params;
                v.liveGhost = g;
            }
        }
        ps.piano = {};
        ps.piano.pedal(ps.pedal, 0.0f, sampleRate_, 0, false);
        c->inserts[k].swap(inserts_[k]);
        for (int l = 0; l < kNumLayers; ++l) {
            auto*& current = currentSets_[k][l];
            if (current) {
                auto** dest = std::find(std::begin(held_), std::end(held_), nullptr);
                *dest = current;
            }
            current = part->sets[l];
            part->sets[l] = nullptr;
            keyswitch_[k][l] = -1;
            seqCount_[k][l] = 0;
        }
        auto* shared = k == 0 ? sharedParams_.get() : partShared_.get() + (size_t)(k - 1) * P::kPartParams;
        auto* dest = k == 0 ? p_ : partP_.get() + (size_t)(k - 1) * P::kPartParams;
        for (int i = 0; i < P::kPartParams; ++i) {
            if (!isPartParam(i)) continue;
            dest[i] = part->params[i];
            shared[i].store(dest[i], std::memory_order_relaxed);
        }
        applyStagedSound(k, part->sound);
        programRevision_[k].fetch_add(1, std::memory_order_release);
        appliedProg_[k].store(-1, std::memory_order_relaxed);
        if (stage_[k] && retiredStages_.push(stage_[k])) {
            stage_[k] = nullptr;
            stagedProg_[k].store(-1, std::memory_order_relaxed);
        }
        if (c->multi)
            for (int i = 0; i < Mix::Count; ++i) {
                mix_[k][i] = c->mix[k][i];
                mixShared_[k][i].store(mix_[k][i], std::memory_order_relaxed);
            }
    }
    if (oldFx) {
        const double beat = rack_->beat();
        rack_.swap(oldFx->rack);
        rack_->setBeat(beat);
        sendRev_.swap(oldFx->sends[0]);
        sendDly_.swap(oldFx->sends[1]);
        sendAQuiet_ = sendBQuiet_ = -1;
        sendTempo_ = 0.0f;
        for (int t = 0; t < 2; ++t) {
            auto* incoming = c->ir[t].release();
            c->ir[t].reset(currentIr_[t]);  // the outgoing rack still borrows this IR
            currentIr_[t] = incoming;
        }
        for (int i = 0; i < P::kNumParams; ++i) {
            if (isPartParam(i)) continue;
            p_[i] = c->globals[i];
            sharedParams_[i].store(p_[i], std::memory_order_relaxed);
        }
        updateEffects();
    }
    if (c->single) {
        multiActive_ = false;
        multi_.store(false, std::memory_order_relaxed);
        selectPart(0);
    } else if (c->multi) {
        zoneProgramsPending_ = (1u << kNumParts) - 1;
        multiActive_ = true;
        multi_.store(true, std::memory_order_relaxed);
        selectPart(c->selectedPart);
        updatePartTargets();
        for (int k = 0; k < kNumParts; ++k)
            std::copy_n(parts_[k].target, 4, parts_[k].gain);
    }
    if (c->single || c->multi) syncZoneConfig();
    liveWaiting_ = nullptr;
    liveRetained_[slot] = c;
    hasLiveTails_ = true;
    sstLength_ = std::max(1, (int)std::lround(sstTimeMs() * sampleRate_ * 0.001f));
    sstFrames_ = sstLength_;
    if (oldFx) sstFxFrames_ = sstFxLength_ = sstLength_;
    sstProgress_.store(0.0f, std::memory_order_relaxed);
    liveApplied_.store(c->serial, std::memory_order_release);
    return true;
}

void Engine::commitLiveChangeWhileStopped() {
    if (applyLiveChange()) {
        extMidiOut_.service();
        commitZonePrograms(zoneProgramSeen_);
    }
}

void Engine::liveNoteOff(const Event& e) {
    for (auto& v : voices_) {
        if (!v.active() || v.liveGhost < 0 || v.released() || v.sustained) continue;
        const auto& g = liveGhosts_[v.liveGhost];
        if (e.noteId ? v.noteId() != e.noteId : v.matchKey != e.key) continue;
        if (!e.noteId && e.channel != kOmniChannel && g.channel != kOmniChannel && g.channel != e.channel) continue;
        v.setReleaseVelocity(e.type == EventType::NoteOff ? e.value : 0.0f);
        if (g.ctx.pedal >= 0.1f || v.hold) v.sustained = true;
        else releaseVoice(v);
    }
}

void Engine::renderLiveGhosts(float* left, float* right, int n) {
    if (!hasLiveTails_) return;
    for (auto* c : liveRetained_) {
        if (!c || !c->fx) continue;
        auto& f = *c->fx;
        f.voices = false;
        for (auto& bus : f.wet) std::fill_n(bus, n, 0.0f);
        for (auto& send : f.send)
            for (auto& bus : send) std::fill_n(bus, n, 0.0f);
    }
    int heads[kGhosts], next[VoicePool::kSize];
    std::fill_n(heads, kGhosts, -1);
    for (int v = VoicePool::kSize - 1; v >= 0; --v) {
        if (!voices_[v].active() || voices_[v].liveGhost < 0) continue;
        const int i = voices_[v].liveGhost;
        next[v] = heads[i];
        heads[i] = v;
    }
    float l[kMaxBlockSize], r[kMaxBlockSize];
    for (int i = 0; i < kGhosts; ++i) {
        auto& g = liveGhosts_[i];
        if (!g.owner) continue;
        auto& insert = *g.owner->inserts[g.part];
        std::fill_n(l, n, 0.0f);
        std::fill_n(r, n, 0.0f);
        const auto& ps = parts_[g.part];
        g.ctx.pitchBendNorm = ps.pitchBend;
        g.ctx.pitchBendSemis = ps.pitchBend * (ps.rpnBend >= 0.0f ? ps.rpnBend : g.ctx.params[P::BendRange]);
        const auto& rack = g.fx ? *g.fx->rack : *rack_;
        const float sync = fx::syncBeats((int)g.ctx.params[P::Lfo2Sync]);
        const float rate = sync > 0.0f ? g.ctx.tempo / 60.0f / sync : g.ctx.params[P::Lfo2Rate];
        bool used = false;
        for (int off = 0; off < n; off += kControlBlock) {
            const int m = std::min(kControlBlock, n - off);
            g.ctx.renderFrame = blockFrame_ + off;
            g.ctx.beat = rack.beat() + (double)g.ctx.tempo * off / (60.0 * sampleRate_);
            g.ctx.lfo2 = g.lfo.advance((LfoWave)(int)g.ctx.params[P::Lfo2Wave], rate, (float)m / sampleRate_, g.rng);
            for (int index = heads[i]; index >= 0; index = next[index]) {
                auto& v = voices_[index];
                if (!v.active() || v.startDelay > off) continue;
                if (v.releaseAt >= 0 && v.releaseAt <= off) { v.release(); v.releaseAt = -1; }
                used = true;
                v.render(l + off, r + off, m, g.ctx);
            }
        }
        if (g.piano.busy()) used = true;
        if (used) g.piano.process(l, r, n, g.ctx.params, sampleRate_);
        if (used || insert.busy()) {
            if (!insert.empty()) {
                insert.setTempo(g.ctx.tempo, rack.beat());
                insert.process(l, r, n);
            }
            float* wetL = g.fx ? g.fx->wet[0] : left;
            float* wetR = g.fx ? g.fx->wet[1] : right;
            auto& a = g.fx ? g.fx->send[0] : sendA_;
            auto& b = g.fx ? g.fx->send[1] : sendB_;
            if (g.fx) g.fx->voices = true;
            for (int j = 0; j < n; ++j) {
                wetL[j] += l[j] * g.gains[0];
                wetR[j] += r[j] * g.gains[1];
                dryL_[j] += l[j] * g.gains[2];
                dryR_[j] += r[j] * g.gains[3];
                const float gl = l[j] * (g.gains[0] + g.gains[2]);
                const float gr = r[j] * (g.gains[1] + g.gains[3]);
                a[0][j] += gl * g.sendA;
                a[1][j] += gr * g.sendA;
                b[0][j] += gl * g.sendB;
                b[1][j] += gr * g.sendB;
            }
        }
        bool alive = insert.busy() || g.piano.busy();
        for (int index = heads[i]; index >= 0 && !alive; index = next[index]) alive = voices_[index].active();
        if (!alive) g = {};
    }
}

void Engine::processLiveTails(float* l, float* r, int n) {
    sstFxFrames_ = std::max(0, sstFxFrames_ - n);
    if (sstFrames_ > 0) {
        sstFrames_ = std::max(0, sstFrames_ - n);
        sstProgress_.store(sstFrames_ ? 1.0f - (float)sstFrames_ / (float)sstLength_ : -1.0f,
                           std::memory_order_relaxed);
    }
    if (!hasLiveTails_) return;
    hasLiveTails_ = false;
    for (auto*& c : liveRetained_) {
        if (!c) continue;
        bool referenced = false;
        for (const auto& g : liveGhosts_)
            referenced = referenced || g.owner == c || (c->fx && g.fx == c->fx.get());
        // Freeze and generated FX noise can otherwise keep an outgoing
        // graph alive indefinitely. Start this limit after its last voice.
        const int tailLimit = (int)(20.0f * sampleRate_);
        const int fade = std::max(1, (int)(sstTimeMs() * sampleRate_ * 0.001f));
        bool rings = false;
        if (auto* f = c->fx.get()) {
            const bool feed = f->voices || referenced;
            const bool rackActive = f->rack->hasChain() && !f->rack->isIdle();
            const bool sendsActive = f->quiet[0] >= 0 || f->quiet[1] >= 0;
            if (feed || rackActive || sendsActive) {
                f->globals.notesHeld = feed;
                f->rack->process(f->wet[0], f->wet[1], n, f->globals, nullptr, nullptr, nullptr,
                                 f->fadeFrames, f->fadeLength);
                fx::FxContext ctx;
                ctx.sampleRate = sampleRate_;
                ctx.tempo = f->rack->tempo();
                for (int b = 0; b < 2; ++b) {
                    if (f->quiet[b] < 0 && !feed) continue;
                    if (feed && f->quiet[b] < 0) f->quiet[b] = 0;
                    auto& bus = f->send[b];
                    f->sends[b]->process(bus[0], bus[1], n, ctx);
                    auto& quiet = f->quiet[b];
                    quiet = f->sends[b]->tailActive() ? 0 : quiet + n;
                    if (quiet > 4.1f * sampleRate_) quiet = -1;
                }
                for (int j = 0; j < n; ++j) {
                    const float gain = feed ? 1.0f : clampf((float)(tailLimit - c->tailFrames - j) / fade, 0.0f, 1.0f);
                    const float wet = 1.0f - (float)std::max(0, f->fadeFrames - j - 1) / f->fadeLength;
                    l[j] += gain * (f->wet[0][j] + wet * (f->level[0] * f->send[0][0][j] + f->level[1] * f->send[1][0][j]));
                    r[j] += gain * (f->wet[1][j] + wet * (f->level[0] * f->send[0][1][j] + f->level[1] * f->send[1][1][j]));
                }
                f->fadeFrames = std::max(0, f->fadeFrames - n);
                c->tailFrames = feed ? 0 : c->tailFrames + n;
                rings = c->tailFrames < tailLimit;
            }
        }
        if (!referenced && !rings && liveRetired_.push(c)) c = nullptr;
        hasLiveTails_ = hasLiveTails_ || c != nullptr;
    }
}

void Engine::changeLiveSampleRate(float rate) {
    // prepare() runs with rendering stopped. Keep accepted pending commits
    // queued and rebuild their DSP at the new stream rate off the callback.
    auto* incoming = liveIncoming_.exchange(nullptr, std::memory_order_acq_rel);
    auto* waiting = liveWaiting_;
    liveWaiting_ = nullptr;
    for (auto& v : voices_) if (v.liveGhost >= 0) v.hardStop();
    clearLiveChanges();
    const auto epoch = sampleEpoch_.fetch_add(1, std::memory_order_relaxed) + 1;
    const int q = quality_.load(std::memory_order_relaxed);
    const int os = q == (int)Quality::Low ? 1 : q == (int)Quality::High ? 4 : 2;
    const bool hq = q == (int)Quality::High;
    for (auto* c : {waiting, incoming}) {
        if (!c) continue;
        c->sampleEpoch = epoch;
        for (int k = 0; k < kNumParts; ++k) {
            if (!c->inserts[k]) continue;
            c->inserts[k]->prepare(rate);
            c->inserts[k]->paramsChanged(c->parts[k]->params, os, hq);
        }
        if (!c->fx) continue;
        resampleLiveIr(*c, rate);
        c->fx->rack->prepare(rate, false);
        for (auto& send : c->fx->sends) send->prepare(rate);
        for (int t = 0; t < 2; ++t) c->fx->rack->setUserIr(t, c->ir[t].get());
        c->fx->rack->paramsChanged(c->globals, os, hq);
    }
    liveWaiting_ = waiting;
    liveIncoming_.store(incoming, std::memory_order_release);
}

void Engine::clearLiveChanges() {
    delete liveIncoming_.exchange(nullptr);
    delete liveWaiting_;
    liveWaiting_ = nullptr;
    for (auto*& c : liveRetained_) {
        delete c;
        c = nullptr;
    }
    for (int i = 0; i < kGhosts; ++i) {
        if (liveGhosts_[i].owner) ghostUsed_[i] = false;
        liveGhosts_[i] = {};
    }
    sstFrames_ = sstFxFrames_ = 0;
    sstProgress_.store(-1.0f, std::memory_order_relaxed);
    hasLiveTails_ = false;
}

}  // namespace hsynth
