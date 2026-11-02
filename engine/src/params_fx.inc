// Effect rack parameter metadata. Included by params.cpp inside its anonymous
// namespace (uses its Builder).

const char* const kBlockKeys[fx::BlockCount] = {
    "none", "gate", "comp", "mbcomp", "transient", "limiter", "drive", "amp", "cab", "crush", "lofi", "exciter",
    "peq", "geq", "filter", "wah", "chorus", "flanger", "phaser", "tremolo", "vibe", "rotary", "ringmod",
    "pitch", "octaver", "delay", "reverb", "convo", "slicer", "stutter", "stereo", "vocoder"};
const char* const kBlockTitles[fx::BlockCount] = {
    "None", "Noise Gate", "Compressor", "Multiband Comp", "Transient Shaper", "Limiter", "Drive", "Amp", "Cabinet",
    "Bit Crusher", "Lo-Fi", "Exciter", "Parametric EQ", "Graphic EQ", "Filter", "Wah", "Chorus", "Flanger", "Phaser",
    "Tremolo / Pan", "Uni-Vibe", "Rotary", "Ring Mod / Shift", "Pitch Shifter", "Octaver", "Delay", "Reverb",
    "IR Reverb", "Slicer", "Stutter", "Stereo Image", "Vocoder"};

const std::vector<std::string> kSyncNames = {"off", "1/1", "1/2", "1/2.", "1/2T", "1/4", "1/4.", "1/4T", "1/8",
                                             "1/8.", "1/8T", "1/16", "1/16.", "1/16T", "1/32"};
const std::vector<std::string> kFxWaveNames = {"sine", "triangle", "saw up", "saw down", "square", "s&h", "smooth"};
const std::vector<std::string> kFxModSourceNames = {"none", "modwheel", "lfo2", "chanpressure", "expression",
                                                    "breath", "pitchbend", "velocity", "lastnote", "envelope",
                                                    "notesheld", "macro1", "macro2"};

std::vector<int>& fxTargets() {
    static std::vector<int> t;
    return t;
}

// One effect block: names "fx.<key>.<param>", group "FX / <title>".
struct FxB {
    Builder& b;
    int block;
    std::string n, g;
    FxB(Builder& bb, int blk)
        : b(bb), block(blk), n(std::string("fx.") + kBlockKeys[blk] + "."), g(std::string("FX / ") + kBlockTitles[blk]) {
        b.bo(P::fxp(block, 0), n + "on", g, "On", true);
    }
    FxB& fl(int l, const std::string& name, const std::string& label, float mn, float mx, float d, const std::string& u = "") {
        b.fl(P::fxp(block, l), n + name, g, label, mn, mx, d, u);
        return *this;
    }
    FxB& ex(int l, const std::string& name, const std::string& label, float mn, float mx, float d, const std::string& u) {
        b.ex(P::fxp(block, l), n + name, g, label, mn, mx, d, u);
        return *this;
    }
    FxB& in(int l, const std::string& name, const std::string& label, float mn, float mx, float d, const std::string& u = "") {
        b.in(P::fxp(block, l), n + name, g, label, mn, mx, d, u);
        return *this;
    }
    FxB& bo(int l, const std::string& name, const std::string& label, bool d) {
        b.bo(P::fxp(block, l), n + name, g, label, d);
        return *this;
    }
    FxB& en(int l, const std::string& name, const std::string& label, const std::vector<std::string>& o, int d) {
        b.en(P::fxp(block, l), n + name, g, label, o, d);
        return *this;
    }
    FxB& mix(int l, float d) { return fl(l, "mix", "Mix", 0, 1, d); }
    FxB& db(int l, const std::string& name, const std::string& label, float mn, float mx, float d) {
        return fl(l, name, label, mn, mx, d, "dB");
    }
    FxB& sync(int l) { return en(l, "sync", "Sync", kSyncNames, 0); }
};

void buildFx(Builder& b) {
    using namespace P;
    namespace F = fx;
    const std::vector<std::string> blocks(kBlockKeys, kBlockKeys + F::BlockCount);
    const std::string gc = "FX Chain";
    b.fl(fxChain(FxTempo), "fx.tempo", gc, "Tempo", 20, 300, 120, "bpm");
    b.bo(fxChain(FxRetrig), "fx.retrig", gc, "Key Retrigger", true);
    b.fl(fxChain(FxMacro1), "fx.macro1", gc, "Macro 1", 0, 1, 0);
    b.fl(fxChain(FxMacro2), "fx.macro2", gc, "Macro 2", 0, 1, 0);
    b.fl(fxChain(LooperLevel), "looper.level", gc, "Looper Level", 0, 1, 0.8f);
    b.bo(fxChain(FxClockSync), "fx.clocksync", gc, "MIDI Clock Sync", false);
    for (int s = 0; s < F::kFxSlots; ++s) {
        const int def = s == 0 ? F::Chorus : s == 1 ? F::Delay : s == 2 ? F::Reverb : F::None;
        b.en(fxSlot(s), "fx.slot" + std::to_string(s + 1), gc, "Slot " + std::to_string(s + 1), blocks, def);
    }

    // ---- dynamics
    FxB(b, F::Gate)
        .db(F::gate::Thresh, "thresh", "Threshold", -80, 0, -50)
        .db(F::gate::Range, "range", "Range", -90, 0, -80)
        .ex(F::gate::Attack, "attack", "Attack", 0.05f, 50, 0.5f, "ms")
        .fl(F::gate::Hold, "hold", "Hold", 0, 500, 20, "ms")
        .ex(F::gate::Release, "release", "Release", 5, 2000, 100, "ms");
    FxB(b, F::Comp)
        .db(F::comp::Thresh, "thresh", "Threshold", -60, 0, -20)
        .ex(F::comp::Ratio, "ratio", "Ratio", 1, 20, 4, ":1")
        .ex(F::comp::Attack, "attack", "Attack", 0.05f, 200, 5, "ms")
        .ex(F::comp::Release, "release", "Release", 5, 2000, 120, "ms")
        .db(F::comp::Knee, "knee", "Knee", 0, 24, 6)
        .db(F::comp::Makeup, "makeup", "Makeup", -12, 24, 0)
        .bo(F::comp::AutoMakeup, "automakeup", "Auto Makeup", false)
        .en(F::comp::Detector, "detector", "Detector", {"peak", "rms"}, 1)
        .ex(F::comp::ScHpf, "schpf", "Sidechain HPF", 20, 500, 20, "Hz")
        .mix(F::comp::Mix, 1);
    {
        FxB m(b, F::MbComp);
        m.ex(F::mbcomp::XLow, "xlow", "Low X-Over", 40, 1000, 180, "Hz")
            .ex(F::mbcomp::XHigh, "xhigh", "High X-Over", 1000, 12000, 3000, "Hz");
        const char* bands[3] = {"low", "mid", "high"};
        const char* labels[3] = {"Low ", "Mid ", "High "};
        for (int k = 0; k < 3; ++k) {
            const std::string bn = bands[k], bl = labels[k];
            m.db(F::mbcomp::LowThresh + k * 3, bn + ".thresh", bl + "Threshold", -60, 0, -24)
                .ex(F::mbcomp::LowRatio + k * 3, bn + ".ratio", bl + "Ratio", 1, 20, 3, ":1")
                .db(F::mbcomp::LowGain + k * 3, bn + ".gain", bl + "Gain", -12, 24, 0);
        }
        m.ex(F::mbcomp::Attack, "attack", "Attack", 0.1f, 200, 10, "ms")
            .ex(F::mbcomp::Release, "release", "Release", 10, 2000, 150, "ms")
            .mix(F::mbcomp::Mix, 1);
    }
    FxB(b, F::Transient)
        .fl(F::transient::Attack, "attack", "Attack", -1, 1, 0.5f)
        .fl(F::transient::Sustain, "sustain", "Sustain", -1, 1, 0)
        .fl(F::transient::Speed, "speed", "Speed", 0, 1, 0.5f)
        .db(F::transient::Output, "output", "Output", -12, 12, 0);
    FxB(b, F::Limiter)
        .db(F::limiter::Input, "input", "Input Gain", 0, 24, 0)
        .db(F::limiter::Ceiling, "ceiling", "Ceiling", -12, 0, -0.3f)
        .ex(F::limiter::Release, "release", "Release", 1, 1000, 60, "ms")
        .fl(F::limiter::Lookahead, "lookahead", "Lookahead", 0, 5, 1.5f, "ms");

    // ---- drive / tone
    FxB(b, F::Drive)
        .en(F::drive::Type, "type", "Type",
            {"soft", "overdrive", "distortion", "fuzz", "metal", "tube", "tape", "fold", "rectify"}, 1)
        .fl(F::drive::Amount, "amount", "Drive", 0, 1, 0.5f)
        .ex(F::drive::Tight, "tight", "Tight (Low Cut)", 10, 1000, 20, "Hz")
        .fl(F::drive::Bias, "bias", "Bias", -1, 1, 0)
        .fl(F::drive::Tone, "tone", "Tone", 0, 1, 0.6f)
        .db(F::drive::Level, "level", "Level", -24, 12, 0)
        .mix(F::drive::Mix, 1);
    FxB(b, F::Amp)
        .en(F::amp::Model, "model", "Model", {"clean", "crunch", "lead", "higain", "modern", "bass"}, 1)
        .fl(F::amp::Gain, "gain", "Gain", 0, 10, 5)
        .fl(F::amp::Bass, "bass", "Bass", 0, 10, 5)
        .fl(F::amp::Middle, "middle", "Middle", 0, 10, 5)
        .fl(F::amp::Treble, "treble", "Treble", 0, 10, 5)
        .fl(F::amp::Presence, "presence", "Presence", 0, 10, 5)
        .fl(F::amp::Depth, "depth", "Depth", 0, 10, 5)
        .db(F::amp::Level, "level", "Master", -24, 12, 0);
    FxB(b, F::Cab)
        .en(F::cab::Model, "model", "Model",
            {"4x12 british", "4x12 modern", "2x12 open", "1x12 combo", "2x10 vintage", "4x10 bass", "8x10 bass",
             "1x15 bass", "user ir"}, 0)
        .fl(F::cab::Mic, "mic", "Mic Position", 0, 1, 0.3f)
        .ex(F::cab::LowCut, "lowcut", "Low Cut", 20, 400, 60, "Hz")
        .ex(F::cab::HighCut, "highcut", "High Cut", 2000, 20000, 12000, "Hz")
        .db(F::cab::Level, "level", "Level", -24, 12, 0)
        .mix(F::cab::Mix, 1);
    FxB(b, F::Crush)
        .fl(F::crush::Bits, "bits", "Bits", 1, 16, 8)
        .ex(F::crush::Rate, "rate", "Sample Rate", 200, 48000, 12000, "Hz")
        .fl(F::crush::Jitter, "jitter", "Jitter", 0, 1, 0)
        .fl(F::crush::Tone, "tone", "Tone", 0, 1, 0.8f)
        .mix(F::crush::Mix, 1)
        .db(F::crush::Level, "level", "Level", -24, 12, 0);
    FxB(b, F::Lofi)
        .fl(F::lofi::Wow, "wow", "Wow", 0, 1, 0.3f)
        .fl(F::lofi::Flutter, "flutter", "Flutter", 0, 1, 0.2f)
        .fl(F::lofi::Saturation, "saturation", "Saturation", 0, 1, 0.3f)
        .fl(F::lofi::Hiss, "hiss", "Hiss", 0, 1, 0.1f)
        .fl(F::lofi::Crackle, "crackle", "Crackle", 0, 1, 0.1f)
        .ex(F::lofi::Bandwidth, "bandwidth", "Bandwidth", 1000, 20000, 9000, "Hz")
        .ex(F::lofi::LowCut, "lowcut", "Low Cut", 20, 500, 60, "Hz")
        .mix(F::lofi::Mix, 1);
    FxB(b, F::Exciter)
        .ex(F::exciter::Freq, "freq", "Frequency", 1000, 10000, 3000, "Hz")
        .fl(F::exciter::Drive, "drive", "Drive", 0, 1, 0.5f)
        .fl(F::exciter::Amount, "amount", "Amount", 0, 1, 0.3f)
        .fl(F::exciter::BassAmount, "bass", "Bass Enhance", 0, 1, 0)
        .ex(F::exciter::BassFreq, "bassfreq", "Bass Freq", 40, 200, 90, "Hz");

    // ---- eq / filter
    FxB(b, F::Peq)
        .ex(F::peq::LowCut, "lowcut", "Low Cut", 10, 1000, 10, "Hz")
        .ex(F::peq::LsFreq, "ls.freq", "Low Shelf Freq", 30, 1000, 100, "Hz")
        .db(F::peq::LsGain, "ls.gain", "Low Shelf Gain", -18, 18, 0)
        .ex(F::peq::P1Freq, "p1.freq", "Peak 1 Freq", 40, 18000, 250, "Hz")
        .db(F::peq::P1Gain, "p1.gain", "Peak 1 Gain", -18, 18, 0)
        .ex(F::peq::P1Q, "p1.q", "Peak 1 Q", 0.1f, 10, 1, "")
        .ex(F::peq::P2Freq, "p2.freq", "Peak 2 Freq", 40, 18000, 1000, "Hz")
        .db(F::peq::P2Gain, "p2.gain", "Peak 2 Gain", -18, 18, 0)
        .ex(F::peq::P2Q, "p2.q", "Peak 2 Q", 0.1f, 10, 1, "")
        .ex(F::peq::P3Freq, "p3.freq", "Peak 3 Freq", 40, 18000, 4000, "Hz")
        .db(F::peq::P3Gain, "p3.gain", "Peak 3 Gain", -18, 18, 0)
        .ex(F::peq::P3Q, "p3.q", "Peak 3 Q", 0.1f, 10, 1, "")
        .ex(F::peq::HsFreq, "hs.freq", "High Shelf Freq", 1000, 18000, 8000, "Hz")
        .db(F::peq::HsGain, "hs.gain", "High Shelf Gain", -18, 18, 0)
        .ex(F::peq::HighCut, "highcut", "High Cut", 1000, 22000, 22000, "Hz")
        .db(F::peq::Output, "output", "Output", -24, 12, 0);
    {
        FxB g(b, F::Geq);
        const char* names[10] = {"31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"};
        for (int k = 0; k < 10; ++k) g.db(F::geq::B31 + k, names[k], std::string(names[k]) + (k < 5 ? " Hz" : "Hz"), -12, 12, 0);
        g.db(F::geq::Level, "level", "Level", -12, 12, 0);
    }
    FxB(b, F::Filter)
        .en(F::filter::Type, "type", "Type", {"lp12", "lp24", "hp12", "hp24", "bp", "notch", "peak", "formant"}, 1)
        .ex(F::filter::Cutoff, "cutoff", "Cutoff", 20, 20000, 2000, "Hz")
        .fl(F::filter::Reso, "reso", "Resonance", 0, 1, 0.3f)
        .fl(F::filter::Drive, "drive", "Drive", 0, 1, 0)
        .en(F::filter::LfoWave, "lfo.wave", "LFO Wave", kFxWaveNames, 0)
        .ex(F::filter::LfoRate, "lfo.rate", "LFO Rate", 0.01f, 20, 1, "Hz")
        .en(F::filter::LfoSync, "lfo.sync", "LFO Sync", kSyncNames, 0)
        .fl(F::filter::LfoDepth, "lfo.depth", "LFO Depth", 0, 1, 0)
        .fl(F::filter::EnvAmount, "env.amount", "Env Follow", -1, 1, 0)
        .ex(F::filter::EnvAttack, "env.attack", "Env Attack", 0.1f, 200, 5, "ms")
        .ex(F::filter::EnvRelease, "env.release", "Env Release", 5, 2000, 150, "ms")
        .fl(F::filter::Phase, "phase", "Stereo Phase", 0, 180, 0, "deg")
        .mix(F::filter::Mix, 1);
    FxB(b, F::Wah)
        .en(F::wah::Mode, "mode", "Mode", {"manual", "auto", "lfo"}, 1)
        .fl(F::wah::Position, "position", "Pedal", 0, 1, 0.5f)
        .fl(F::wah::Sens, "sens", "Sensitivity", 0, 1, 0.6f)
        .ex(F::wah::Rate, "rate", "LFO Rate", 0.05f, 10, 1, "Hz")
        .fl(F::wah::Reso, "reso", "Resonance", 0, 1, 0.6f)
        .mix(F::wah::Mix, 1);

    // ---- modulation
    FxB(b, F::Chorus)
        .en(F::chorus::Mode, "mode", "Mode", {"classic", "dimension", "ensemble"}, 0)
        .in(F::chorus::Voices, "voices", "Voices", 1, 4, 2)
        .ex(F::chorus::Rate, "rate", "Rate", 0.05f, 8, 0.6f, "Hz")
        .fl(F::chorus::Depth, "depth", "Depth", 0, 1, 0.5f)
        .fl(F::chorus::Delay, "delay", "Delay", 2, 30, 12, "ms")
        .fl(F::chorus::Feedback, "feedback", "Feedback", -0.9f, 0.9f, 0)
        .fl(F::chorus::Width, "width", "Width", 0, 1, 1)
        .mix(F::chorus::Mix, 0.5f);
    FxB(b, F::Flanger)
        .ex(F::flanger::Rate, "rate", "Rate", 0.02f, 10, 0.25f, "Hz")
        .sync(F::flanger::Sync)
        .fl(F::flanger::Depth, "depth", "Depth", 0, 1, 0.7f)
        .ex(F::flanger::Manual, "manual", "Manual", 0.1f, 10, 2, "ms")
        .fl(F::flanger::Feedback, "feedback", "Feedback", -0.95f, 0.95f, 0.6f)
        .bo(F::flanger::ThruZero, "thruzero", "Thru-Zero", false)
        .fl(F::flanger::Phase, "phase", "Stereo Phase", 0, 180, 90, "deg")
        .mix(F::flanger::Mix, 0.5f);
    FxB(b, F::Phaser)
        .en(F::phaser::Stages, "stages", "Stages", {"2", "4", "6", "8", "10", "12"}, 1)
        .ex(F::phaser::Rate, "rate", "Rate", 0.02f, 10, 0.4f, "Hz")
        .sync(F::phaser::Sync)
        .fl(F::phaser::Depth, "depth", "Depth", 0, 1, 0.7f)
        .ex(F::phaser::Center, "center", "Center", 100, 4000, 700, "Hz")
        .fl(F::phaser::Feedback, "feedback", "Feedback", -0.95f, 0.95f, 0.5f)
        .fl(F::phaser::Phase, "phase", "Stereo Phase", 0, 180, 90, "deg")
        .mix(F::phaser::Mix, 0.5f);
    FxB(b, F::Tremolo)
        .en(F::tremolo::Mode, "mode", "Mode", {"tremolo", "autopan", "harmonic"}, 0)
        .en(F::tremolo::Wave, "wave", "Wave", kFxWaveNames, 0)
        .ex(F::tremolo::Rate, "rate", "Rate", 0.05f, 20, 5, "Hz")
        .sync(F::tremolo::Sync)
        .fl(F::tremolo::Depth, "depth", "Depth", 0, 1, 0.6f)
        .fl(F::tremolo::Phase, "phase", "Stereo Phase", 0, 180, 0, "deg");
    FxB(b, F::Vibe)
        .en(F::vibe::Mode, "mode", "Mode", {"chorus", "vibrato"}, 0)
        .ex(F::vibe::Rate, "rate", "Rate", 0.1f, 12, 2, "Hz")
        .sync(F::vibe::Sync)
        .fl(F::vibe::Depth, "depth", "Depth", 0, 1, 0.6f)
        .mix(F::vibe::Mix, 0.5f);
    FxB(b, F::Rotary)
        .bo(F::rotary::Fast, "fast", "Fast", false)
        .ex(F::rotary::Accel, "accel", "Acceleration", 0.1f, 5, 1, "s")
        .fl(F::rotary::Balance, "balance", "Drum<>Horn", 0, 1, 0.6f)
        .fl(F::rotary::Drive, "drive", "Drive", 0, 1, 0.2f)
        .fl(F::rotary::Spread, "spread", "Mic Spread", 0, 1, 0.8f)
        .mix(F::rotary::Mix, 1);
    FxB(b, F::RingMod)
        .en(F::ringmod::Mode, "mode", "Mode", {"ring", "freq shift"}, 0)
        .ex(F::ringmod::Freq, "freq", "Frequency", 0.1f, 5000, 220, "Hz")
        .bo(F::ringmod::KeyTrack, "keytrack", "Key Track", false)
        .ex(F::ringmod::LfoRate, "lfo.rate", "LFO Rate", 0.01f, 20, 0.5f, "Hz")
        .fl(F::ringmod::LfoDepth, "lfo.depth", "LFO Depth", 0, 1, 0)
        .fl(F::ringmod::Spread, "spread", "Stereo Spread", 0, 1, 0)
        .mix(F::ringmod::Mix, 0.5f);

    // ---- pitch
    FxB(b, F::Pitch)
        .in(F::pitch::Semi1, "semi1", "Voice 1 Semi", -24, 24, 12, "st")
        .fl(F::pitch::Fine1, "fine1", "Voice 1 Fine", -50, 50, 0, "ct")
        .fl(F::pitch::Pan1, "pan1", "Voice 1 Pan", -1, 1, 0)
        .in(F::pitch::Semi2, "semi2", "Voice 2 Semi", -24, 24, -12, "st")
        .fl(F::pitch::Fine2, "fine2", "Voice 2 Fine", -50, 50, 0, "ct")
        .fl(F::pitch::Level2, "level2", "Voice 2 Level", 0, 1, 0)
        .fl(F::pitch::Pan2, "pan2", "Voice 2 Pan", -1, 1, 0)
        .ex(F::pitch::Window, "window", "Window", 10, 120, 45, "ms")
        .fl(F::pitch::Feedback, "feedback", "Feedback", 0, 0.9f, 0)
        .mix(F::pitch::Mix, 0.5f);
    FxB(b, F::Octaver)
        .fl(F::octaver::Sub1, "sub1", "-1 Oct", 0, 1, 0.7f)
        .fl(F::octaver::Sub2, "sub2", "-2 Oct", 0, 1, 0)
        .fl(F::octaver::Up, "up", "+1 Oct", 0, 1, 0)
        .fl(F::octaver::Dry, "dry", "Dry", 0, 1, 1)
        .fl(F::octaver::Tone, "tone", "Tone", 0, 1, 0.5f);

    // ---- time / space
    FxB(b, F::Delay)
        .en(F::delay::Type, "type", "Type", {"digital", "analog", "tape", "reverse", "diffuse"}, 0)
        .en(F::delay::Mode, "mode", "Mode", {"stereo", "pingpong", "dual"}, 1)
        .ex(F::delay::Time, "time", "Time", 0.01f, 2, 0.35f, "s")
        .sync(F::delay::Sync)
        .ex(F::delay::Ratio, "ratio", "R Time Ratio", 0.25f, 2, 0.75f, "x")
        .fl(F::delay::Feedback, "feedback", "Feedback", 0, 1, 0.35f)
        .ex(F::delay::LowCut, "lowcut", "Low Cut", 20, 2000, 60, "Hz")
        .ex(F::delay::HighCut, "highcut", "High Cut", 500, 20000, 8000, "Hz")
        .fl(F::delay::ModDepth, "mod.depth", "Mod Depth", 0, 1, 0.15f)
        .ex(F::delay::ModRate, "mod.rate", "Mod Rate", 0.05f, 10, 0.6f, "Hz")
        .fl(F::delay::Ducking, "ducking", "Ducking", 0, 1, 0)
        .fl(F::delay::Width, "width", "Width", 0, 1, 1)
        .mix(F::delay::Mix, 0.3f);
    FxB(b, F::Reverb)
        .en(F::reverb::Type, "type", "Type",
            {"room", "hall", "plate", "chamber", "cathedral", "spring", "shimmer", "ambience"}, 1)
        .fl(F::reverb::Size, "size", "Size", 0, 1, 0.6f)
        .ex(F::reverb::Decay, "decay", "Decay", 0.1f, 30, 3, "s")
        .fl(F::reverb::PreDelay, "predelay", "Pre-Delay", 0, 250, 10, "ms")
        .fl(F::reverb::Damp, "damp", "Damping", 0, 1, 0.4f)
        .ex(F::reverb::LowCut, "lowcut", "Low Cut", 20, 1000, 40, "Hz")
        .fl(F::reverb::Diffusion, "diffusion", "Diffusion", 0, 1, 0.7f)
        .fl(F::reverb::Modulation, "modulation", "Modulation", 0, 1, 0.3f)
        .fl(F::reverb::Width, "width", "Width", 0, 1, 1)
        .fl(F::reverb::Early, "early", "Early Reflections", 0, 1, 0.3f)
        .fl(F::reverb::Shimmer, "shimmer", "Shimmer", 0, 1, 0.5f)
        .bo(F::reverb::Freeze, "freeze", "Freeze", false)
        .mix(F::reverb::Mix, 0.15f);
    FxB(b, F::Convo)
        .en(F::convo::Ir, "ir", "Impulse", {"room", "hall", "plate", "user"}, 1)
        .fl(F::convo::PreDelay, "predelay", "Pre-Delay", 0, 250, 0, "ms")
        .ex(F::convo::LowCut, "lowcut", "Low Cut", 20, 1000, 20, "Hz")
        .ex(F::convo::HighCut, "highcut", "High Cut", 1000, 20000, 20000, "Hz")
        .fl(F::convo::Width, "width", "Width", 0, 1, 1)
        .mix(F::convo::Mix, 0.3f);

    // ---- rhythm / utility
    FxB(b, F::Slicer)
        .en(F::slicer::Division, "division", "Division", {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"}, 3)
        .en(F::slicer::Pattern, "pattern", "Pattern",
            {"straight", "offbeat", "gallop", "rev gallop", "3-3-2", "trance 1", "trance 2", "trance 3", "half",
             "stutter", "sparse", "random"}, 0)
        .fl(F::slicer::Duty, "duty", "Gate Length", 0.05f, 1, 0.6f)
        .fl(F::slicer::Depth, "depth", "Depth", 0, 1, 1)
        .fl(F::slicer::Smooth, "smooth", "Smooth", 0, 1, 0.2f)
        .fl(F::slicer::Swing, "swing", "Swing", 0, 1, 0);
    FxB(b, F::Stutter)
        .bo(F::stutter::Active, "active", "Hold (Repeat)", false)
        .en(F::stutter::Division, "division", "Division", {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/64"}, 3)
        .bo(F::stutter::Reverse, "reverse", "Reverse", false)
        .fl(F::stutter::Gate, "gate", "Gate", 0.05f, 1, 1)
        .mix(F::stutter::Mix, 1);
    FxB(b, F::Stereo)
        .fl(F::stereo::Width, "width", "Width", 0, 2, 1)
        .fl(F::stereo::Pan, "pan", "Pan", -1, 1, 0)
        .fl(F::stereo::Haas, "haas", "Haas Delay", 0, 30, 0, "ms")
        .ex(F::stereo::MonoBass, "monobass", "Mono Bass Below", 20, 400, 20, "Hz")
        .db(F::stereo::Output, "output", "Output", -24, 12, 0);
    FxB(b, F::Vocoder)
        .in(F::vocoder::Bands, "bands", "Bands", 4, 32, 16)
        .ex(F::vocoder::LowFreq, "lowfreq", "Low Band", 50, 500, 100, "Hz")
        .ex(F::vocoder::HighFreq, "highfreq", "High Band", 2000, 12000, 8000, "Hz")
        .fl(F::vocoder::Q, "q", "Band Q", 0, 1, 0.5f)
        .ex(F::vocoder::Attack, "attack", "Attack", 0.5f, 100, 5, "ms")
        .ex(F::vocoder::Release, "release", "Release", 5, 1000, 60, "ms")
        .fl(F::vocoder::Formant, "formant", "Formant Shift", -12, 12, 0, "st")
        .fl(F::vocoder::Unvoiced, "unvoiced", "Sibilance", 0, 1, 0.2f)
        .db(F::vocoder::InputGain, "inputgain", "Input Gain", -24, 24, 0)
        .db(F::vocoder::GateThresh, "gate", "Input Gate", -90, -20, -70)
        .mix(F::vocoder::Mix, 1);

    // Chorus and delay sit in the default chain switched off (matching presets
    // made before the rack existed); every other block is on when inserted.
    b.t[(size_t)fxp(F::Chorus, 0)].def = 0.0f;
    b.t[(size_t)fxp(F::Delay, 0)].def = 0.0f;
}
