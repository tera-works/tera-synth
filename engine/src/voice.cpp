#include "hsynth/voice.h"

#include "hsynth/dsp/resample.h"

namespace hsynth {

// ---------------------------------------------------------------- PcmPlayer

void PcmPlayer::start(const Region* r, int key, float velocity, float startFrac, float velTrackScale,
                      float sampleRate, float gainDb) {
    region_ = r;
    released_ = false;
    sr_ = sampleRate;
    const Sample* s = r->sample;
    end_ = r->end > 0 ? std::min<int64_t>(r->end, s->frames) : s->frames;
    int64_t off = std::max<int64_t>(0, r->offset);
    off += (int64_t)((double)clampf(startFrac, 0.0f, 1.0f) * (double)std::max<int64_t>(0, end_ - off));
    if (off >= end_) off = std::max<int64_t>(0, end_ - 1);
    pos_ = (double)off;
    looping_ = (r->loopMode == LoopMode::Continuous || r->loopMode == LoopMode::Sustain) && r->loopEnd > r->loopStart + 1 &&
               r->loopStart >= 0 && r->loopEnd <= s->frames;
    xfLen_ = 0;
    if (looping_ && r->loopCrossfade > 0.0f)
        xfLen_ = std::min<int64_t>({(int64_t)(r->loopCrossfade * s->sampleRate), r->loopStart, r->loopEnd - r->loopStart});
    baseRatio_ = std::exp2(((double)(key - r->keycenter) * r->pitchKeytrack / 100.0 + r->transpose) / 12.0 + r->tune / 1200.0);
    const float t = clampf(r->ampVeltrack / 100.0f * velTrackScale, 0.0f, 1.0f);
    const float vg = (1.0f - t) + t * velocity * velocity;
    const float g = dbToGain(r->volumeDb + gainDb + r->ampKeytrack * (float)(key - r->ampKeycenter)) * vg;
    gainL_ = g * std::min(1.0f, 1.0f - r->pan);
    gainR_ = g * std::min(1.0f, 1.0f + r->pan);
    fade_ = 1.0f;
    fadeStep_ = 0.0f;
    delay_ = (int64_t)(std::max(0.0f, r->delay) * sampleRate);
    useEnv_ = r->ampeg.enabled;
    envDelay_ = 0;
    if (useEnv_) {
        env_.setSampleRate(sampleRate);
        env_.set(std::max(0.0005f, r->ampeg.attack), std::max(0.001f, r->ampeg.decay), r->ampeg.sustain,
                 std::max(0.001f, r->ampeg.release));
        env_.setHold(std::max(0.0f, r->ampeg.hold));
        env_.reset();
        env_.gate(true);
        envDelay_ = (int64_t)(std::max(0.0f, r->ampeg.delay) * sampleRate);
    }
    if (r->filter != RegionFilter::None) {
        const float cents = r->filKeytrack * (float)(key - r->filKeycenter) + r->filVeltrack * velocity;
        const float fc = clampf(r->cutoff * std::exp2(cents / 1200.0f), 10.0f, sampleRate * 0.45f);
        // resonance dB -> SVF damping (0 dB = Butterworth)
        const float q = 0.7071f * dbToGain(std::max(0.0f, r->resonance));
        const float res = clampf((2.0f - 1.0f / q) / 1.98f, 0.0f, 1.0f);
        for (auto& f : filt_) { f.reset(); f.setCoefs(fc, res, sampleRate); }
        lp1a_ = 1.0f - std::exp(-kTwoPi * fc / sampleRate);
        lp1_[0] = lp1_[1] = 0.0f;
    }
}

void PcmPlayer::release() {
    if (!region_) return;
    released_ = true;
    if (region_->loopMode == LoopMode::Sustain) looping_ = false;
    if (useEnv_ && region_->loopMode != LoopMode::OneShot) env_.gate(false);
}

void PcmPlayer::cut() {
    if (!region_) return;
    if (region_->offFast || !useEnv_) {
        fadeStep_ = 1.0f / (0.006f * sr_);
        if (fadeStep_ > fade_) fadeStep_ = fade_;
    } else {
        env_.gate(false);
        looping_ = looping_ && region_->loopMode != LoopMode::Sustain;
    }
}

void PcmPlayer::setPitch(float pitchSemis, float engineRate, int quality) {
    if (!region_) return;
    step_ = baseRatio_ * std::exp2((double)pitchSemis / 12.0) * region_->sample->sampleRate / (double)engineRate;
    if (quality <= 0 || step_ <= 1.0) {
        cut_ = -1;  // at or below the recorded pitch the cubic interpolator does not alias
        level_ = 0;
        return;
    }
    // pitched up: read a mip level that brings the step to <= 2, then a sinc
    // whose cutoff keeps the output below Nyquist
    double s = step_;
    int level = 0;
    while (level < region_->sample->mipLevels && s > 2.0) {
        s *= 0.5;
        ++level;
    }
    level_ = level;
    cut_ = resample::cutoffIndex(s);
}

inline float PcmPlayer::atLevel(int c, int64_t j) const {
    if (j < 0) return 0.0f;
    const Sample* s = region_->sample;
    if (looping_) {
        // loop points are level-0 frames: wrap in that domain
        int64_t j0 = j << level_;
        if (j0 >= region_->loopEnd) {
            j0 = region_->loopStart + (j0 - region_->loopEnd) % (region_->loopEnd - region_->loopStart);
            j = j0 >> level_;
        }
    }
    if (j >= s->levelFrames(level_) + Sample::kPadFrames) return 0.0f;
    return s->levelChannel(level_, c)[j];
}

float PcmPlayer::sincAt(int c, double pos) const {
    const double p = level_ ? std::ldexp(pos, -level_) : pos;
    const int64_t idx = (int64_t)p;
    const float fp = (float)(p - (double)idx) * (float)resample::kPhases;
    const int ph = std::min((int)fp, resample::kPhases - 1);
    const float pf = fp - (float)ph;
    const float* k0 = resample::kernel(cut_, ph);
    const float* k1 = k0 + resample::kTaps;
    const int64_t first = idx - (resample::kTaps / 2 - 1);
    const Sample* s = region_->sample;
    const int64_t last = first + resample::kTaps - 1;
    float acc = 0.0f;
    const bool inside = first >= 0 && last < s->levelFrames(level_) &&
                        (!looping_ || (last << level_) < region_->loopEnd);
    if (inside) {
        const float* d = s->levelChannel(level_, c) + first;
        for (int k = 0; k < resample::kTaps; ++k) acc += d[k] * (k0[k] + pf * (k1[k] - k0[k]));
    } else {
        for (int k = 0; k < resample::kTaps; ++k) acc += atLevel(c, first + k) * (k0[k] + pf * (k1[k] - k0[k]));
    }
    return acc;
}

inline float PcmPlayer::at(int c, int64_t idx) const {
    if (idx < 0) return 0.0f;
    if (looping_ && idx >= region_->loopEnd) idx = region_->loopStart + (idx - region_->loopEnd) % (region_->loopEnd - region_->loopStart);
    const Sample* s = region_->sample;
    if (idx >= s->frames + Sample::kPadFrames) return 0.0f;
    return s->channel(c)[idx];
}

inline float PcmPlayer::sampleAt(int c, int64_t idx, float f) const {
    float xm1, x0, x1, x2;
    const int64_t lim = looping_ ? region_->loopEnd : region_->sample->frames + Sample::kPadFrames;
    if (idx >= 1 && idx + 2 < lim) {  // common case: all four taps inside, no wrap
        const float* d = region_->sample->channel(c) + idx;
        xm1 = d[-1]; x0 = d[0]; x1 = d[1]; x2 = d[2];
    } else {
        xm1 = at(c, idx - 1); x0 = at(c, idx); x1 = at(c, idx + 1); x2 = at(c, idx + 2);
    }
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}

void PcmPlayer::render(float* L, float* R, int n, float gain) {
    if (!region_) return;
    const bool stereo = region_->sample->channels == 2;
    const float gl = gainL_ * gain, gr = gainR_ * gain;
    const RegionFilter ft = region_->filter;
    for (int i = 0; i < n; ++i) {
        if (delay_ > 0) { --delay_; continue; }
        const int64_t idx = (int64_t)pos_;
        float l = value(0, pos_);
        float r = stereo ? value(1, pos_) : l;
        if (xfLen_ > 0 && looping_ && idx >= region_->loopEnd - xfLen_) {
            // loop crossfade: blend in the material before the loop start
            const int64_t len = region_->loopEnd - region_->loopStart;
            const float t = (float)((pos_ - (double)(region_->loopEnd - xfLen_)) / (double)xfLen_);
            const float bl = value(0, pos_ - (double)len);
            l += (bl - l) * t;
            r = stereo ? r + (value(1, pos_ - (double)len) - r) * t : l;
        }
        if (ft != RegionFilter::None) {
            for (int c = 0; c < (stereo ? 2 : 1); ++c) {
                float& v = c == 0 ? l : r;
                switch (ft) {
                    case RegionFilter::Lp1: lp1_[c] += lp1a_ * (v - lp1_[c]); v = lp1_[c]; break;
                    case RegionFilter::Hp1: lp1_[c] += lp1a_ * (v - lp1_[c]); v -= lp1_[c]; break;
                    case RegionFilter::Lp2: v = filt_[c].tick(v).lp; break;
                    case RegionFilter::Hp2: v = filt_[c].tick(v).hp; break;
                    case RegionFilter::Bp2: v = filt_[c].tick(v).bp * filt_[c].k(); break;
                    case RegionFilter::Br2: { const Svf::Out o = filt_[c].tick(v); v = o.lp + o.hp; break; }
                    default: break;
                }
            }
            if (!stereo) r = l;
        }
        float e = 1.0f;
        if (useEnv_) {
            if (envDelay_ > 0) { --envDelay_; e = 0.0f; }
            else e = env_.process();
        }
        if (fadeStep_ > 0.0f) {
            fade_ -= fadeStep_;
            if (fade_ <= 0.0f) { region_ = nullptr; return; }
            e *= fade_;
        }
        L[i] += l * gl * e;
        R[i] += r * gr * e;
        pos_ += step_;
        if (looping_) {
            if (pos_ >= (double)region_->loopEnd) pos_ -= (double)(region_->loopEnd - region_->loopStart);
        } else if (pos_ >= (double)end_) {
            region_ = nullptr;
            return;
        }
        if (useEnv_ && envDelay_ == 0 && !env_.active()) {
            region_ = nullptr;
            return;
        }
    }
}

// -------------------------------------------------------------------- Voice

void Voice::prepare(float sampleRate) {
    sampleRate_ = sampleRate;
    ampEnv_.setSampleRate(sampleRate);
    env2_.setSampleRate(sampleRate);
    env3_.setSampleRate(sampleRate);
    for (auto& l : layers_) l.fm.setSampleRate(sampleRate);
    if (!combMem_) {
        combMem_.reset(new float[2 * Comb::kSize]());
        filterL_.setCombBuffer(combMem_.get());
        filterR_.setCombBuffer(combMem_.get() + Comb::kSize);
    }
    active_ = false;
}

void Voice::start(const Event& e, const VoiceContext& ctx, uint64_t age, float random, float glideFrom) {
    const float* p = ctx.params;
    noteId_ = e.noteId;
    key_ = e.key;
    pcmKey_ = e.key;
    glide_ = glideFrom >= 0.0f ? glideFrom - (float)e.key : 0.0f;
    velocity_ = clampf(e.value, 0.0f, 1.0f);
    random_ = random;
    age_ = age;
    active_ = true;
    released_ = stolen_ = sustained = hold = false;
    timeSinceOn_ = 0.0f;
    notePitchT_ = notePressureT_ = noteTimbreT_ = noteXT_ = 0.0f;
    notePitch_ = notePressure_ = noteTimbre_ = noteX_ = 0.0f;
    firstBlock_ = true;
    stereo_ = false;
    rng_ = Rng((uint32_t)(random * 4294967000.0f) + 1u);
    rng3_ = Rng(((uint32_t)(random * 4294967000.0f) ^ 0x9E3779B9u) | 1u);

    ampEnv_.reset();
    ampEnv_.set(p[P::AmpAttack], p[P::AmpDecay], p[P::AmpSustain], p[P::AmpRelease]);
    ampEnv_.gate(true);
    env2_.reset();
    env2_.set(p[P::Env2Attack], p[P::Env2Decay], p[P::Env2Sustain], p[P::Env2Release]);
    env2_.gate(true);
    lfo1_.reset(p[P::Lfo1KeySync] > 0.5f ? 0.0f : random);
    lfo1Value_ = 0.0f;
    env3_.reset();
    env3_.set(p[P::Env3Attack], p[P::Env3Decay], p[P::Env3Sustain], p[P::Env3Release]);
    env3_.gate(true);
    lfo3_.reset(p[P::Lfo3KeySync] > 0.5f ? 0.0f : std::fmod(random * 7.31f, 1.0f));
    lfo3Value_ = 0.0f;
    filterL_.reset();
    filterR_.reset();
    vfx_.reset();

    const int vel127 = std::max(1, std::min(127, (int)std::lround(velocity_ * 127.0f)));
    for (auto& pl : relPcm_) pl.stop();
    relActive_ = false;
    mainDone_ = false;
    for (int l = 0; l < kNumLayers; ++l) {
        sets_[l] = ctx.sampleSets[l];
        seq_[l] = ctx.seq[l];
        keyswitch_[l] = ctx.keyswitch[l];
    }
    for (int l = 0; l < kNumLayers; ++l) {
        Layer& ly = layers_[l];
        ly.type = (LayerType)(int)p[P::layer(l, P::LType)];
        ly.finished = ly.type == LayerType::Off;
        ly.pcmCount = 0;
        switch (ly.type) {
            case LayerType::VA:
                ly.osc1.reset(unisonDetune != 0.0f ? rng_.unipolar() : 0.0f);  // unison: free-running phases
                ly.osc2.reset(rng_.unipolar());
                break;
            case LayerType::FM:
                ly.fm.noteOn(velocity_);
                ly.fm.control(p, l);
                break;
            case LayerType::WT:
                ly.wt.reset(rng_, (int)p[P::layer(l, P::WtUnison)]);
                break;
            case LayerType::PCM: {
                const SampleSet* set = ctx.sampleSets[l];
                const Region* regs[kMaxRegionsPerLayer];
                RegionQuery q;
                q.key = key_;
                q.vel127 = vel127;
                q.rand01 = random_;
                q.seq = ctx.seq[l];
                q.keyswitch = ctx.keyswitch[l];
                const int n = set ? set->findRegions(q, regs, kMaxRegionsPerLayer) : 0;
                for (int i = 0; i < n; ++i)
                    ly.pcm[i].start(regs[i], key_, velocity_, p[P::layer(l, P::PcmStart)], p[P::layer(l, P::PcmVelTrack)],
                                    sampleRate_);
                for (int i = n; i < kMaxRegionsPerLayer; ++i) ly.pcm[i].stop();
                ly.pcmCount = n;
                if (n == 0) ly.finished = true;
                break;
            }
            default: break;
        }
    }
}

void Voice::release() {
    if (!active_ || released_) return;
    released_ = true;
    sustained = false;
    // One-shot samples ignore note-off when they are the only sound source.
    bool allOneShot = true, anyLayer = false;
    for (auto& ly : layers_) {
        if (ly.type == LayerType::Off || ly.finished) continue;
        anyLayer = true;
        if (ly.type != LayerType::PCM) { allOneShot = false; continue; }
        for (int i = 0; i < ly.pcmCount; ++i)
            if (ly.pcm[i].active() && !ly.pcm[i].oneShot()) allOneShot = false;
    }
    if (!(anyLayer && allOneShot)) ampEnv_.gate(false);
    env2_.gate(false);
    env3_.gate(false);
    for (auto& ly : layers_) {
        if (ly.type == LayerType::FM) ly.fm.noteOff();
        if (ly.type == LayerType::PCM)
            for (int i = 0; i < ly.pcmCount; ++i) ly.pcm[i].release();
    }
    startReleaseRegions();
}

void Voice::startReleaseRegions() {
    int used = 0;
    const int vel127 = std::max(1, std::min(127, (int)std::lround(velocity_ * 127.0f)));
    for (int l = 0; l < kNumLayers && used < kMaxReleaseRegions; ++l) {
        const SampleSet* set = sets_[l];
        if (layers_[l].type != LayerType::PCM || !set || !set->hasReleaseRegions) continue;
        RegionQuery q;
        q.key = key_;
        q.vel127 = vel127;
        q.rand01 = random_;
        q.trigger = RegionTrigger::Release;
        q.seq = seq_[l];
        q.keyswitch = keyswitch_[l];
        const Region* regs[kMaxReleaseRegions];
        const int n = set->findRegions(q, regs, kMaxReleaseRegions - used);
        for (int i = 0; i < n; ++i) {
            relPcm_[used].start(regs[i], key_, velocity_, 0.0f, 1.0f, sampleRate_, -regs[i]->rtDecay * timeSinceOn_);
            relLayer_[used++] = l;
        }
    }
    if (used > 0) relActive_ = true;
}

void Voice::legatoTo(const Event& e, bool glide) {
    if (!active_) return;
    const float from = currentPitch();
    noteId_ = e.noteId;
    key_ = e.key;
    glide_ = glide ? from - (float)e.key : 0.0f;
    released_ = sustained = hold = false;
    notePitchT_ = notePressureT_ = noteTimbreT_ = noteXT_ = 0.0f;
}

int Voice::startedGroups(int* out, int max) const {
    int n = 0;
    for (const auto& ly : layers_) {
        if (ly.type != LayerType::PCM) continue;
        for (int i = 0; i < ly.pcmCount && n < max; ++i) {
            const Region* r = ly.pcm[i].region();
            if (!r || r->group == 0) continue;
            bool dup = false;
            for (int k = 0; k < n; ++k) dup = dup || out[k] == r->group;
            if (!dup) out[n++] = r->group;
        }
    }
    return n;
}

void Voice::choke(int group) {
    if (!active_ || group == 0) return;
    for (auto& ly : layers_) {
        if (ly.type != LayerType::PCM) continue;
        for (int i = 0; i < ly.pcmCount; ++i)
            if (ly.pcm[i].active() && ly.pcm[i].region()->offBy == group) ly.pcm[i].cut();
    }
    for (auto& pl : relPcm_)
        if (pl.active() && pl.region()->offBy == group) pl.cut();
}

void Voice::steal() {
    if (!active_) return;
    stolen_ = true;
    released_ = true;
    ampEnv_.kill(0.005f);
    for (auto& pl : relPcm_) {
        if (!pl.active()) continue;
        pl.release();
        pl.cut();
    }
}

void Voice::stopLayer(int l) {
    Layer& ly = layers_[l];
    for (auto& pl : ly.pcm) pl.stop();
    for (int i = 0; i < kMaxReleaseRegions; ++i)
        if (relLayer_[i] == l) relPcm_[i].stop();
    sets_[l] = nullptr;
    if (ly.type == LayerType::PCM) ly.finished = true;
}

void Voice::applyExpression(EventType type, float value) {
    switch (type) {
        case EventType::NotePitch: notePitchT_ = value; break;
        case EventType::NotePressure: notePressureT_ = clampf(value, 0.0f, 1.0f); break;
        case EventType::NoteTimbre: noteTimbreT_ = clampf(value, 0.0f, 1.0f); break;
        case EventType::NoteX: noteXT_ = clampf(value, -1.0f, 1.0f); break;
        default: break;
    }
}

float Voice::modSource(ModSource s, const VoiceContext& ctx) const {
    switch (s) {
        case ModSource::Velocity: return velocity_;
        case ModSource::Key: return (float)(key_ - 60) / 60.0f;
        case ModSource::AmpEnv: return ampEnv_.level();
        case ModSource::Env2: return env2_.level();
        case ModSource::Lfo1: return lfo1Value_;
        case ModSource::Lfo2: return ctx.lfo2;
        case ModSource::ModWheel: return ctx.modWheel;
        case ModSource::PitchBend: return ctx.pitchBendNorm;
        case ModSource::ChannelPressure: return ctx.chanPressure;
        case ModSource::NotePressure: return notePressure_;
        case ModSource::NoteTimbre: return noteTimbre_;
        case ModSource::NoteX: return noteX_;
        case ModSource::NotePitch: return notePitch_ / 12.0f;
        case ModSource::Random: return random_;
        case ModSource::Breath: return ctx.breath;
        case ModSource::Expression: return ctx.expression;
        case ModSource::Cc1:
        case ModSource::Cc2:
        case ModSource::Cc3:
        case ModSource::Cc4: return ctx.cc[(int)s - (int)ModSource::Cc1];
        case ModSource::Lfo3: return lfo3Value_;
        case ModSource::Env3: return env3_.level();
        case ModSource::Constant: return 1.0f;
        default: return 0.0f;
    }
}

void Voice::render(float* L, float* R, int n, const VoiceContext& ctx) {
    if (!active_) return;
    const float* p = ctx.params;
    if (relActive_) {
        // release-trigger regions: own region envelope, layer level / pan / tuning, no amp env / filter
        bool any = false;
        for (int i = 0; i < kMaxReleaseRegions; ++i) {
            PcmPlayer& pl = relPcm_[i];
            if (!pl.active()) continue;
            const int l = relLayer_[i];
            float tl[kControlBlock] = {}, tr[kControlBlock] = {};
            pl.setPitch(notePitch_ + ctx.pitchBendSemis + p[P::layer(l, P::LCoarse)] + p[P::layer(l, P::LFine)] / 100.0f,
                        sampleRate_, ctx.quality);
            pl.render(tl, tr, n, p[P::layer(l, P::LLevel)]);
            const float lpan = p[P::layer(l, P::LPan)];
            const float lgl = std::min(1.0f, 1.0f - lpan), lgr = std::min(1.0f, 1.0f + lpan);
            for (int k = 0; k < n; ++k) {
                L[k] += tl[k] * lgl;
                R[k] += tr[k] * lgr;
            }
            any = any || pl.active();
        }
        relActive_ = any;
        if (mainDone_) {
            if (!relActive_) active_ = false;
            return;
        }
    }
    const float sr = sampleRate_;
    const float dtSec = (float)n / sr;
    const float invN = 1.0f / (float)n;

    // Smooth per-note expression at control rate (~8 ms).
    {
        const float k = dtSec / 0.008f;
        const float a = firstBlock_ ? 1.0f : k / (1.0f + k);
        notePitch_ += a * (notePitchT_ - notePitch_);
        notePressure_ += a * (notePressureT_ - notePressure_);
        noteTimbre_ += a * (noteTimbreT_ - noteTimbre_);
        noteX_ += a * (noteXT_ - noteX_);
    }

    if (!stolen_) ampEnv_.set(p[P::AmpAttack], p[P::AmpDecay], p[P::AmpSustain], p[P::AmpRelease]);
    env2_.set(p[P::Env2Attack], p[P::Env2Decay], p[P::Env2Sustain], p[P::Env2Release]);
    const float e2 = env2_.processBlock(n);
    env3_.set(p[P::Env3Attack], p[P::Env3Decay], p[P::Env3Sustain], p[P::Env3Release]);
    env3_.processBlock(n);

    // Modulation matrix
    float d[(int)ModDest::Count] = {};
    for (int s = 0; s < P::kModSlots; ++s) {
        const int src = (int)p[P::mod(s, P::ModSrc)];
        const int dst = (int)p[P::mod(s, P::ModDst)];
        const float amt = p[P::mod(s, P::ModAmt)];
        if (src <= 0 || dst <= 0 || dst >= (int)ModDest::Count || amt == 0.0f) continue;
        float v = modSource((ModSource)src, ctx) * amt;
        const int via = (int)p[P::mod(s, P::ModVia)];
        if (via > 0) v *= modSource((ModSource)via, ctx);
        d[dst] += v;
    }
    auto D = [&](ModDest x) { return d[(int)x]; };

    // amp envelope time modulation (x8 per unit), only when modulated
    if (!stolen_ && (D(ModDest::AmpAttack) != 0.0f || D(ModDest::AmpDecay) != 0.0f || D(ModDest::AmpRelease) != 0.0f)) {
        auto sc = [&](ModDest x) { return fastExp2(clampf(D(x), -2.0f, 2.0f) * 3.0f); };
        ampEnv_.set(p[P::AmpAttack] * sc(ModDest::AmpAttack), p[P::AmpDecay] * sc(ModDest::AmpDecay), p[P::AmpSustain],
                    p[P::AmpRelease] * sc(ModDest::AmpRelease));
    }

    // LFO1 (per voice) with delayed fade-in
    {
        const float syncB = fx::syncBeats((int)p[P::Lfo1Sync]);
        const float rate = (syncB > 0.0f ? ctx.tempo / 60.0f / syncB : p[P::Lfo1Rate]) *
                           fastExp2(clampf(D(ModDest::Lfo1Rate), -1.0f, 1.0f) * 4.0f);
        const float delay = p[P::Lfo1Delay];
        const float fade = delay <= 0.002f ? 1.0f : clampf(timeSinceOn_ / delay, 0.0f, 1.0f);
        lfo1Value_ = lfo1_.advance((LfoWave)(int)p[P::Lfo1Wave], rate, dtSec, rng_) * fade;
        timeSinceOn_ += dtSec;
        const float sync3 = fx::syncBeats((int)p[P::Lfo3Sync]);
        const float rate3 = (sync3 > 0.0f ? ctx.tempo / 60.0f / sync3 : p[P::Lfo3Rate]) *
                            fastExp2(clampf(D(ModDest::Lfo3Rate), -1.0f, 1.0f) * 4.0f);
        lfo3Value_ = lfo3_.advance((LfoWave)(int)p[P::Lfo3Wave], rate3, dtSec, rng3_);
    }

    float basePitch = (float)key_ + notePitch_ + ctx.pitchBendSemis + D(ModDest::Pitch) * 24.0f;
    {
        const float tune = ctx.masterTune + p[P::TuneC + key_ % 12] * 0.01f;  // master tune + scale tuning
        if (tune != 0.0f) basePitch += tune;
    }
    if (glide_ != 0.0f || unisonDetune != 0.0f) {
        basePitch += glide_ + unisonDetune * clampf(1.0f + D(ModDest::UnisonDetune), 0.0f, 4.0f);
        // exponential glide: ~99 % of the interval within voice.glide seconds
        const float t = p[P::Glide];
        glide_ = t <= 0.0005f ? 0.0f : glide_ * std::exp(-4.6f * dtSec / t);
        if (std::fabs(glide_) < 0.001f) glide_ = 0.0f;
    }

    // Filter
    const auto ftype = (FilterType)(int)p[P::FilterType];
    const float cutoffOct = p[P::FilterKeytrack] * (float)(key_ - 60) / 12.0f + p[P::FilterEnvAmt] * e2 * 6.0f +
                            p[P::FilterVelAmt] * 3.0f * (velocity_ - 1.0f) + D(ModDest::Cutoff) * 5.0f;
    const float cutoff = p[P::FilterCutoff] * fastExp2(clampf(cutoffOct, -12.0f, 12.0f));
    const float reso = clampf(p[P::FilterReso] + D(ModDest::Resonance), 0.0f, 1.0f);
    const float drive = clampf(p[P::FilterDrive] + D(ModDest::Drive), 0.0f, 1.0f);
    const float fmorph = clampf(p[P::FilterMorph] + D(ModDest::FilterMorph), 0.0f, 1.0f);
    const bool hq = ctx.quality >= 2;
    filterL_.set(ftype, cutoff, reso, drive, sr, fmorph, hq);
    if (stereo_) filterR_.copyCoefsFrom(filterL_);
    const auto vfxType = (VfxType)(int)p[P::VfxType];
    if (vfxType != VfxType::Off)
        vfx_.set(vfxType, p[P::VfxAmount] + D(ModDest::VfxAmount), p[P::VfxMix] + D(ModDest::VfxMix),
                 midiToHz(basePitch) * p[P::VfxRatio] / sr, hq);
    else
        vfx_.set(VfxType::Off, 0.0f, 0.0f, 0.0f);

    // Amp / pan
    const float vs = p[P::AmpVelSens];
    const float ampTarget = (1.0f - vs + vs * velocity_ * velocity_) * clampf(1.0f + D(ModDest::Amp), 0.0f, 2.0f);
    const float pan = clampf(D(ModDest::Pan) + unisonPan, -1.0f, 1.0f);
    const float panL = std::min(1.0f, 1.0f - pan), panR = std::min(1.0f, 1.0f + pan);
    const float morph = clampf(p[P::Morph] + D(ModDest::Morph), -1.0f, 1.0f);

    float bufL[kControlBlock] = {};
    float bufR[kControlBlock] = {};
    float tmp[kControlBlock];
    float tmpR[kControlBlock];
    bool stereo = pan != 0.0f;
    bool anyLayerRunning = false;

    for (int l = 0; l < kNumLayers; ++l) {
        Layer& ly = layers_[l];
        if (ly.type == LayerType::Off || ly.finished) continue;
        const float lvs = p[P::layer(l, P::LVelSens)];
        float lvl = p[P::layer(l, P::LLevel)] * (1.0f - lvs + lvs * velocity_) *
                    clampf(1.0f + d[(int)ModDest::LayerALevel + l], 0.0f, 2.0f);
        if (l == 0) lvl *= clampf(1.0f - morph, 0.0f, 1.0f);
        if (l == 1) lvl *= clampf(1.0f + morph, 0.0f, 1.0f);
        const float lpitch = basePitch + p[P::layer(l, P::LCoarse)] + p[P::layer(l, P::LFine)] / 100.0f +
                             d[(int)ModDest::LayerAPitch + l] * 24.0f;
        const float lpanMod = d[(int)ModDest::LayerAPan + l];
        const float lpan = lpanMod != 0.0f ? clampf(p[P::layer(l, P::LPan)] + lpanMod, -1.0f, 1.0f) : p[P::layer(l, P::LPan)];
        const float lgl = std::min(1.0f, 1.0f - lpan), lgr = std::min(1.0f, 1.0f + lpan);
        if (lpan != 0.0f) stereo = true;
        const float g0 = firstBlock_ ? lvl : ly.gain;
        const float gStep = (lvl - g0) * invN;
        bool layerStereo = false;

        switch (ly.type) {
            case LayerType::VA: {
                const float dt1T = midiToHz(lpitch) / sr;
                const float dt2T = midiToHz(lpitch + p[P::layer(l, P::VaOsc2Semi)] + p[P::layer(l, P::VaDetune)] / 100.0f) / sr;
                if (firstBlock_) { ly.dt1 = dt1T; ly.dt2 = dt2T; }
                const float s1 = (dt1T - ly.dt1) * invN, s2 = (dt2T - ly.dt2) * invN;
                const auto w1 = (VaWave)(int)p[P::layer(l, P::VaWave1)];
                const auto w2 = (VaWave)(int)p[P::layer(l, P::VaWave2)];
                const float pw = clampf(p[P::layer(l, P::VaPw)] + D(ModDest::VaPulseWidth) * 0.45f, 0.05f, 0.95f);
                const float mix = clampf(p[P::layer(l, P::VaMix)] + D(ModDest::VaOscMix), 0.0f, 1.0f);
                const float noise = clampf(p[P::layer(l, P::VaNoise)] + D(ModDest::VaNoise), 0.0f, 1.0f);
                for (int i = 0; i < n; ++i) {
                    ly.dt1 += s1;
                    ly.dt2 += s2;
                    float v = ly.osc1.tick(w1, ly.dt1, pw) * (1.0f - mix) + ly.osc2.tick(w2, ly.dt2, pw) * mix;
                    if (noise > 0.0f) v += noise * rng_.bipolar();
                    tmp[i] = v;
                }
                ly.dt1 = dt1T;
                ly.dt2 = dt2T;
                break;
            }
            case LayerType::FM: {
                const float opMod[P::kFmOps] = {D(ModDest::FmOp1Level), D(ModDest::FmOp2Level), D(ModDest::FmOp3Level),
                                                D(ModDest::FmOp4Level), D(ModDest::FmOp5Level), D(ModDest::FmOp6Level)};
                ly.fm.control(p, l, opMod);
                const float dtT = midiToHz(lpitch) / sr;
                if (firstBlock_) ly.dt1 = dtT;
                const float index = clampf(p[P::layer(l, P::FmIndex)] + D(ModDest::FmIndex), 0.0f, 1.0f) * 2.5f;
                const float fb = clampf(p[P::layer(l, P::FmFeedback)] + D(ModDest::FmFeedback), 0.0f, 1.0f);
                std::fill(tmp, tmp + n, 0.0f);
                ly.fm.render(tmp, n, ly.dt1, dtT, index, fb);
                ly.dt1 = dtT;
                if (!ly.fm.active()) ly.finished = true;
                break;
            }
            case LayerType::WT: {
                const int ti = (int)p[P::layer(l, P::WtTable)];
                const Wavetable* t = nullptr;
                if (ti < builtinWavetableCount()) t = &builtinWavetable(ti);
                else if (ctx.sampleSets[l] && ctx.sampleSets[l]->wavetable) t = ctx.sampleSets[l]->wavetable.get();
                else t = &builtinWavetable(0);  // "user" selected but nothing loaded
                const float pos = p[P::layer(l, P::WtPos)] + p[P::layer(l, P::WtPosEnv)] * e2 + D(ModDest::WtPos);
                const float warpAmt = clampf(p[P::layer(l, P::WtWarpAmt)] + D(ModDest::WtWarp), 0.0f, 1.0f);
                layerStereo = ly.wt.render(*t, clampf(pos, 0.0f, 1.0f), midiToHz(lpitch) / sr, (WtWarp)(int)p[P::layer(l, P::WtWarpMode)],
                                           warpAmt, (int)p[P::layer(l, P::WtUnison)], p[P::layer(l, P::WtDetune)],
                                           p[P::layer(l, P::WtSpread)], tmp, tmpR, n);
                break;
            }
            case LayerType::PCM: {
                std::fill(tmp, tmp + n, 0.0f);
                std::fill(tmpR, tmpR + n, 0.0f);
                int running = 0;
                for (int i = 0; i < ly.pcmCount; ++i) {
                    PcmPlayer& pl = ly.pcm[i];
                    if (!pl.active()) continue;
                    if (pl.region()->sample->channels == 2 || pl.region()->pan != 0.0f) layerStereo = true;
                    pl.setPitch(lpitch - (float)pcmKey_, sr, ctx.quality);
                    pl.render(tmp, tmpR, n, 1.0f);
                    if (pl.active()) ++running;
                }
                if (running == 0) ly.finished = true;
                break;
            }
            default: break;
        }

        float g = g0;
        if (layerStereo) {
            stereo = true;
            for (int i = 0; i < n; ++i) {
                g += gStep;
                bufL[i] += tmp[i] * g * lgl;
                bufR[i] += tmpR[i] * g * lgr;
            }
        } else {
            for (int i = 0; i < n; ++i) {
                g += gStep;
                const float v = tmp[i] * g;
                bufL[i] += v * lgl;
                bufR[i] += v * lgr;
            }
        }
        ly.gain = lvl;
        if (!ly.finished) anyLayerRunning = true;
    }

    if (stereo && !stereo_) {
        filterR_.copyFrom(filterL_);  // continue from the mono state without a click
        stereo_ = true;
    }

    const float a0 = firstBlock_ ? ampTarget : ampGain_;
    const float aStep = (ampTarget - a0) * invN;
    float a = a0;
    const float pl0 = firstBlock_ ? panL : panL_, pr0 = firstBlock_ ? panR : panR_;
    const float plStep = (panL - pl0) * invN, prStep = (panR - pr0) * invN;
    float gl = pl0, gr = pr0;
    for (int i = 0; i < n; ++i) {
        a += aStep;
        gl += plStep;
        gr += prStep;
        const float env = ampEnv_.process() * a;
        float l = filterL_.process(bufL[i]);
        float r = stereo_ ? filterR_.process(bufR[i]) : l;
        if (vfx_.active()) {
            vfx_.advance();
            l = vfx_.process(l, 0);
            r = stereo_ ? vfx_.process(r, 1) : l;
        }
        L[i] += l * env * gl;
        R[i] += r * env * gr;
    }
    ampGain_ = ampTarget;
    panL_ = panL;
    panR_ = panR;
    firstBlock_ = false;

    const bool envDone = !ampEnv_.active() || (ampEnv_.stage() == Adsr::Stage::Sustain && ampEnv_.level() <= 0.0f);
    if (envDone || !anyLayerRunning) {
        mainDone_ = true;
        if (!relActive_) active_ = false;
    }
}

}  // namespace hsynth
