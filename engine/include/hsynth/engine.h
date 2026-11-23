// HSynth engine: platform-independent real-time synthesizer core.
//
// Threading contract
//   * process() is called from exactly one real-time audio thread. It never
//     allocates, locks, or touches the file system.
//   * Every other public method may be called from any non-audio thread.
//     Events go through a lock-free queue, parameters through atomics, and
//     sample sets are swapped by pointer and retired to a garbage queue that
//     collectGarbage() frees on a non-audio thread.
//
// Multi-timbral operation
//   The engine has kNumParts (16) parts. A part is a complete patch: its own
//   copy of the part parameters (isPartParam(): voice, layers, mod matrix) and
//   its own three sample sets / user wavetables. The effect rack, looper,
//   master volume and limiter are global and shared by all parts; the voice
//   pool is shared too (fair stealing across parts).
//   * Single-part mode (default): only part 0 ("part 1" in UIs) plays, every
//     event goes to it regardless of its MIDI channel and the mix settings are
//     not applied, i.e. the engine behaves exactly like the single-timbral one.
//   * Multi-timbral mode: events with a MIDI channel (Event::channel) go to
//     every part whose Mix::Channel matches; events with kOmniChannel go to the
//     selected part. Each part is mixed with volume / pan / mute, filtered by
//     key range and transpose, and split by FX send between the effect rack
//     and a dry bus that is added after the rack (before master + limiter).
//   The legacy single-part API (setParam, setSampleSet, ...) addresses part 0.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "hsynth/dsp/master_stage.h"
#include "hsynth/dsp/piano.h"
#include "hsynth/morph.h"
#include "hsynth/performance_sequence.h"
#include "hsynth/common.h"
#include "hsynth/dsp/effects.h"
#include "hsynth/dsp/lfo.h"
#include "hsynth/event.h"
#include "hsynth/backing_player.h"
#include "hsynth/npu_morph.h"
#include "hsynth/live_sampler.h"
#include "hsynth/midi_zone.h"
#include "hsynth/pad_sampler.h"
#include "hsynth/fx/block.h"
#include "hsynth/fx/convolver.h"
#include "hsynth/fx/insert.h"
#include "hsynth/fx/rack.h"
#include "hsynth/params.h"
#include "hsynth/sample_set.h"
#include "hsynth/smf.h"
#include "hsynth/spsc_queue.h"
#include "hsynth/audio_tap.h"
#include "hsynth/dsp/metronome.h"
#include "hsynth/performance_input.h"
#include "hsynth/tuner.h"
#include "hsynth/voice.h"
#include "hsynth/worker_pool.h"

namespace hsynth {

enum class Quality : int { Low = 0, Normal = 1, High = 2 };

struct EngineStats {
    int activeVoices = 0;
    int peakVoices = 0;
    float cpuLoad = 0.0f;       // smoothed render time / buffer duration (0..1+)
    float cpuLoadPeak = 0.0f;   // since last reset
    float renderUsAvg = 0.0f;   // microseconds per process() call
    float renderUsPeak = 0.0f;
    uint64_t blocks = 0;
    uint32_t droppedEvents = 0;
    int lastBlockSize = 0;
    float outputPeak = 0.0f;    // since last reset
    // disk streaming (streaming.h)
    int streamVoices = 0;       // sample players reading from disk now
    uint32_t streamUnderruns = 0;  // blocks in which a player had to play silence (disk too slow), since start
    uint64_t streamBytesRead = 0;
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Not real-time safe. Call before audio starts or with audio stopped.
    void prepare(float sampleRate, int maxBlockSize = kMaxBlockSize);
    void setMasterStage(const MasterStageConfig& config) { masterStage_.setConfig(config); }
    bool updateMasterStage(const std::function<bool(MasterStageConfig&)>& edit) { return masterStage_.updateConfig(edit); }
    MasterStageConfig getMasterStage() const { return masterStage_.getConfig(); }
    float masterCompGrDb() const { return masterStage_.gainReductionDb(); }
    float sampleRate() const { return sampleRateHost_.load(std::memory_order_relaxed); }

    BackingPlayer& backingPlayer() { return backing_; }
    NpuMorph& npuMorph() { return npuMorph_; }
    PadSampler& padSampler() { return pads_; }
    LiveSampler& liveSampler() { return liveSampler_; }
    ZoneNoteLedger& externalMidi() { return extMidiOut_; }
    void setBackingRoute(bool direct);
    void requestZonePrograms() { zoneProgramVersion_.fetch_add(1, std::memory_order_release); }

    // Audio thread. Renders n frames into non-interleaved buffers (overwrites).
    void process(float* left, float* right, int n);

    // ---- any non-audio thread ----
    bool sendEvent(const Event& e);
    void setParam(int id, float value);
    float getParam(int id) const;
    void setAllParams(const float* values);  // kNumParams values
    void getAllParams(float* out) const;

    struct PartSoundOptions {
        MorphProgram morph;
        ArpPattern arpPattern;
    };
    PartSoundOptions partSoundOptions(int part) const;
    void setPartSoundOptions(int part, const PartSoundOptions& options);
    void morphSetA(int part);
    int morphCaptureB(int part);
    bool morphAssign(int part, int id, float a, float b);
    void morphClear(int part, int id);
    void morphClearAll(int part);
    float morphNow(int part, int id) const;
    bool setArpPatternJson(int part, const Json& json);
    ArpPattern arpPattern(int part) const;
    int arpStep(int part) const;
    bool setRhythmJson(const Json& json);
    Json rhythmJson() const;
    void setRhythmFill(bool on) { rhythmFill_.store(on, std::memory_order_relaxed); }
    int physModes(int part, int layer, float* out, int capacity) const;

    // Takes ownership. Pass an empty SampleSet (or nullptr) to unload.
    void setSampleSet(int layer, std::unique_ptr<SampleSet> set) { setPartSampleSet(0, layer, std::move(set)); }
    std::string sampleSetName(int layer) const { return partSampleSetName(0, layer); }
    std::string sampleSetPath(int layer) const { return partSampleSetPath(0, layer); }
    // Free sample sets retired by the audio thread.
    void collectGarbage();

    // ---- multi-timbral (any non-audio thread) ----
    // Switching modes releases nothing in part 0; voices of other parts are
    // stopped when going back to single-part mode.
    void setMultiTimbral(bool on) { multi_.store(on); }
    bool multiTimbral() const { return multi_.load(); }
    // Part that receives events without a MIDI channel (kOmniChannel) in
    // multi-timbral mode (touch keyboard / host UI).
    void selectPart(int part) { selectedPart_.store(std::max(0, std::min(kNumParts - 1, part))); }
    int selectedPart() const { return selectedPart_.load(); }
    // Parameters of part N. For ids that are not part parameters (effect rack,
    // master volume, limiter) these forward to the global setParam/getParam.
    void setPartParam(int part, int id, float value);
    float getPartParam(int part, int id) const;
    // kNumParams-sized arrays. set: only the part parameters are used. get:
    // the part parameters of `part`, everything else from the global array.
    void setPartParams(int part, const float* values);
    void setPartTuning(int part, const float* cents);
    void getPartParams(int part, float* out) const;
    // Only the non-part parameters (rack, master volume, limiter).
    void setGlobalParams(const float* values);
    // Mix settings (Mix::Volume ...); used in multi-timbral mode only.
    void setPartMix(int part, int id, float value);
    // Standard controllers (CC1 vibrato, CC5, CC11, CC71, CC74, CC91, CC94): a CC that MIDI learn
    // maps to a parameter of a part replaces its standard response on that part (MidiLearn sets it).
    static constexpr int kStandardCcs[7] = {1, 5, 11, 71, 74, 91, 94};
    static int standardCcIndex(int cc);  // -1 = not a standard controller
    void setStandardCcOverride(int part, int cc, bool on);
    bool standardCcOverridden(int part, int cc) const;
    float getPartMix(int part, int id) const;
    void setPartSampleSet(int part, int layer, std::unique_ptr<SampleSet> set);
    std::string partSampleSetName(int part, int layer) const;
    std::string partSampleSetPath(int part, int layer) const;
    int partActiveVoices(int part) const {
        return part >= 0 && part < kNumParts ? statPartVoices_[part].load(std::memory_order_relaxed) : 0;
    }

    void setMaxVoices(int n);
    int maxVoices() const { return maxVoices_.load(); }
    void setQuality(Quality q) { quality_.store((int)q); }
    Quality quality() const { return (Quality)quality_.load(); }

    // ---- multi-core rendering ----
    // Number of threads that render voices (the audio thread plus n - 1 pool
    // threads), 1..kMaxRenderThreads; 0 = auto (autoRenderThreads()). 1 (the
    // default) renders everything on the audio thread. Any non-audio thread;
    // the pool is created on first use and lives until the engine is destroyed.
    // Rendering in parallel changes the order in which voices are summed, so
    // the output may differ from single-threaded rendering by float rounding.
    static constexpr int kMaxRenderThreads = WorkerPool::kMaxHelpers + 1;
    void setRenderThreads(int n);
    int renderThreads() const { return renderThreads_.load(); }
    // OS thread ids of the render helpers (Linux / Android), for performance
    // hint sessions. Non-audio thread.
    int renderThreadIds(int* out, int max) const {
        const WorkerPool* p = pool_.load(std::memory_order_acquire);
        return p ? p->threadIds(out, max) : 0;
    }

    // ---- effect rack ----
    // User impulse response for the cabinet (target 0) or IR reverb (target
    // 1); nullptr unloads. Build it with loadImpulseResponse() (fx/ir_load.h).
    void setFxImpulse(int target, std::unique_ptr<fx::ConvIr> ir);
    std::string fxImpulseName(int target) const;
    std::string fxImpulsePath(int target) const;
    void looperCommand(fx::LooperCmd c) {
        Event e;
        e.type = EventType::Looper;
        e.controller = (uint16_t)c;
        sendEvent(e);
    }
    fx::LooperStatus looperStatus() const { return looper_.status(); }
    // Blocks in processing order; returns count (<= fx::kFxSlots).
    int fxChain(int* blocks) const;

    // Audio thread, before process(): external input for the vocoder for the
    // next process() call (n frames, or nullptr). The pointers are not kept.
    // validFrames excludes any zero padding in the borrowed buffers. Legacy
    // callers without a length are resolved at the beginning of process().
    void setInputBuffer(const float* left, const float* right, int validFrames = -1);

    void setPerfOffsets(int transposeSemi, float trimDb);
    int perfTranspose() const { return perfOffsets_.load(std::memory_order_relaxed).transpose; }
    float perfTrimDb() const { return perfOffsets_.load(std::memory_order_relaxed).trimDb; }

    MetronomeConfig metronomeConfig() const;
    void setMetronomeConfig(const MetronomeConfig& config);
    bool setMetronomeJson(const Json& config);
    void setMetronomeEnabled(bool on) { metroEnabled_.store(on, std::memory_order_relaxed); }
    int clickBeat() const {
        return metroEnabled_.load(std::memory_order_relaxed) || phraseCountInPending_.load(std::memory_order_relaxed)
            ? metronome_.currentBeat() : -1;
    }

    Tuner& tuner() { return tuner_; }
    const Tuner& tuner() const { return tuner_; }
    AudioTap& audioTap() { return audioTap_; }

    PerformanceInputConfig performanceInputConfig() const { return performanceInput_.get().config; }
    void setPerformanceInputConfig(const PerformanceInputConfig& config);
    bool setPedalConfigJson(const Json& config);
    void setFootswitch(int slot, int cc, int action, bool invert);
    bool footswitchConsumesCc(int cc) const;
    int takeHostAction();
    uint32_t lastCc() const { return lastCc_.load(std::memory_order_relaxed); }
    int pedalState() const { return pedalState_.load(std::memory_order_relaxed); }

    // MIDI clock state (from EventType::Clock): measured tempo, 0 if no clock
    // was received in the last 0.5 s. Any thread.
    float midiClockTempo() const { return statClockBpm_.load(std::memory_order_relaxed); }

    // Offline rendering (export / tests): streamed samples wait for the disk
    // instead of playing silence when it falls behind. Not for real-time use.
    void setOfflineRender(bool on) { streamer_.setOffline(on); }

    // ---- transport / step sequencer ----
    // Free-running sequencers (seq.mode = free) play while the transport runs.
    // MIDI clock Start / Stop also start / stop it. Any thread.
    void setTransport(bool playing) { transport_.store(playing); }
    bool transportPlaying() const { return transport_.load(); }
    // Step the part's sequencer played last (-1 = not running), for UIs.
    int seqPosition(int part) const {
        return part >= 0 && part < kNumParts ? statSeqStep_[part].load(std::memory_order_relaxed) : -1;
    }

    // ---- song (Standard MIDI File) playback ----
    // The song's events are played sample-accurately into the parts (by MIDI
    // channel: use multi-timbral mode, part N listens to channel N+1). The
    // host loads the presets for the song's programs (gm_bank.h). Any
    // non-audio thread; setSong() stops playback.
    void setSong(std::unique_ptr<Song> song);
    void songPlay(bool on);
    void songSeek(double seconds);
    bool songPlaying() const { return songPlay_.load(); }
    // increments every time a song reaches its end on its own (playlists advance on it)
    uint32_t songEndCount() const { return songEnds_.load(std::memory_order_relaxed); }

    // ---- phrase recorder (any non-audio thread) ----
    // Records what is played live (keyboard, MIDI in: notes, controllers, bend,
    // pressure; not songs, arps or sequencers) as a loop in beats, at the rack
    // tempo, and plays it back into the parts: Record starts the first pass,
    // Play closes it (length rounded to bars, or set beforehand with
    // setPhraseBars) and loops it, Record while playing overdubs (each overdub is
    // a pass that Undo removes). Notes held when a pass ends get their note-off.
    enum class PhraseState : int { Empty = 0, Recording, Playing, Overdub, Stopped };
    struct PhraseEvent {
        double beat = 0.0;  // position in the loop
        Event event;
        uint16_t pass = 0;
    };
    static constexpr int kMaxPhraseEvents = 8192;
    bool phraseCommand(int command);  // false for an unknown command or a full queue
    void phraseRecord();   // Empty: record the first pass; Playing: overdub on; Overdub: off; Stopped: overdub from the top
    void phrasePlay();     // Recording: close the first pass and loop; Stopped: play from the top
    void phraseStop();
    void phraseClear();
    void phraseUndo();     // the last pass (the first pass: everything)
    void setPhraseBars(int bars) { phraseBars_.store(std::max(0, std::min(64, bars))); }  // 0 = set by Play
    int phraseBars() const { return phraseBars_.load(); }
    PhraseState phraseState() const { return (PhraseState)phraseStateA_.load(std::memory_order_relaxed); }
    double phraseLength() const { return phraseLenA_.load(std::memory_order_relaxed); }    // beats (0 until closed)
    double phrasePosition() const { return phrasePosA_.load(std::memory_order_relaxed); }  // beats
    int phraseEventCount() const { return phraseCountA_.load(std::memory_order_relaxed); }
    // Copy of the phrase (quantize, export): request, then take it after the next audio block.
    uint32_t requestPhraseSnapshot() { return snapRequest_.fetch_add(1, std::memory_order_release) + 1; }
    bool takePhraseSnapshot(std::vector<PhraseEvent>& out, double& lengthBeats, uint32_t* token = nullptr);
    // Replaces the phrase (e.g. quantized); it keeps playing from the same position.
    void replacePhrase(const std::vector<PhraseEvent>& events, double lengthBeats);

    // ---- song program changes (any non-audio thread) ----
    // Mid-song program changes switch a part's sound without loading anything on
    // the audio thread: the host (SongProgramLoader, gm_bank.h) loads the next
    // program's preset ahead of time and stages it here. The engine applies it
    // at the song's program change to that program on the part's channel, or at
    // once if the song already passed it. Voices still sounding fade out on the
    // sample layers that change. One staged program per part; a newer one
    // replaces it.
    struct StagedProgram {
        PartSoundOptions sound;
        int program = -1;
        bool keepSound = false;               // same preset: only the program number changes
        float params[P::kNumParams] = {};     // the part parameters (isPartParam) are used
        SampleSet* sets[kNumLayers] = {};     // owned until applied (nullptr = empty layer)
        std::string setNames[kNumLayers], setPaths[kNumLayers];
        int appliedPart = -1;
        uint32_t revision = 0;                 // set by the engine
        StagedProgram() = default;
        StagedProgram(const StagedProgram&) = delete;
        StagedProgram& operator=(const StagedProgram&) = delete;
        ~StagedProgram() { for (SampleSet* s : sets) delete s; }
    };
    void stagePartProgram(int part, std::unique_ptr<StagedProgram> staged);

    // Prepare off the audio thread, commit atomically at a render boundary.
    // Explicit live commits are independent of the host's automatic-load policy.
    struct LiveFx;
    struct LiveChange {
        LiveChange();
        ~LiveChange();
        std::unique_ptr<StagedProgram> parts[kNumParts];
        bool hasGlobals = false, multi = false, single = false;
        float globals[P::kNumParams] = {};
        std::unique_ptr<fx::ConvIr> ir[2];
        float irSampleRate = 0.0f;
        float mix[kNumParts][Mix::Count] = {};
        int selectedPart = 0;

    private:
        friend class Engine;
        std::unique_ptr<LiveFx> fx;
        std::unique_ptr<fx::InsertChain> inserts[kNumParts];
        uint32_t sampleEpoch = 0;
        uint32_t serial = 0;
        int tailFrames = 0;
    };
    bool stageLiveChange(std::unique_ptr<LiveChange> change, uint32_t* serial = nullptr);
    // Only when the host has stopped and joined its audio callback.
    void commitLiveChangeWhileStopped();
    bool liveChangeApplied(uint32_t serial) const {
        return (int32_t)(liveApplied_.load(std::memory_order_acquire) - serial) >= 0;
    }
    void setSstEnabled(bool on) { sstEnabled_.store(on, std::memory_order_relaxed); }
    bool sstEnabled() const { return sstEnabled_.load(std::memory_order_relaxed); }
    void setSstTimeMs(float ms);
    float sstTimeMs() const { return sstTimeMs_.load(std::memory_order_relaxed); }
    float sstProgress() const { return sstProgress_.load(std::memory_order_relaxed); }
    // the program the part plays (-1 unknown), as set by setPartProgram() or a staged program
    int partProgram(int part) const { return validPart(part) ? appliedProg_[part].load(std::memory_order_relaxed) : -1; }
    // the latest program change the song passed on the part's channel (-1 none)
    int songProgram(int part) const { return validPart(part) ? songProg_[part].load(std::memory_order_relaxed) : -1; }
    // the staged program waiting on the audio thread (-1 none)
    int stagedProgram(int part) const { return validPart(part) ? stagedProg_[part].load(std::memory_order_relaxed) : -1; }
    // after loading a preset for `program` into the part (song start): what it plays now
    void setPartProgram(int part, int program);

    // Live state of the parts for player / mixer UIs (any thread).
    struct PartMeter {
        float level = 0;       // output peak since the previous call
        float volume = 1;      // CC7, 0..1
        float expression = 1;  // CC11
        float pan = 0;         // CC10 as -1..1
        float bend = 0;        // -1..1
        float mod = 0;         // mod wheel 0..1
        float reverb = 0;      // CC91 0..1 (adds to send A)
        float chorus = 0;      // CC93 0..1 (shown only)
        int voices = 0;
        uint32_t notes[4] = {};  // keys sounding (bit k of word k/32)
    };
    void partMeters(PartMeter* out) const;  // kNumParts entries; reading resets the level peaks
    // bend of a part in semitones as of the last block (tests; not thread-safe)
    float debugPitchBendSemis(int part) const { return parts_[part].ctx.pitchBendSemis; }
    double songPosition() const { return statSongPos_.load(std::memory_order_relaxed); }
    double songLength() const { return songLen_.load(std::memory_order_relaxed); }

    EngineStats stats() const;
    void resetPeakStats();
    // audio thread / tests: true when engine is in zero-cost silent-idle fast path
    bool isSilentIdle() const { return silentIdle_; }
    // Where the slowest block since resetPeakStats() spent its time (us):
    // params, swaps+events+song, voices, part inserts, rack+sends+master, stats.
    static constexpr int kProfPhases = 8;  // + active voices, rendered in parallel (0/1)
    void peakProfile(float out[kProfPhases]) const;
    // Phase 4 duration for the latest block, read on the render thread.
    float lastFxMicros() const {
        return std::chrono::duration<float, std::micro>(profMark_[5] - profMark_[4]).count();
    }

private:
    // Audio-thread state of one part.
    struct PartState {
        const float* p = nullptr;  // base or effective parameter row
        PianoBusState piano;
        StrumState strum;
        VoiceContext ctx;
        float pitchBend = 0.0f;
        // scale lock / chord mode: the notes each held key started (released with it)
        struct PerfNote {
            bool used = false;
            uint32_t id = 0;     // the key's note id (0: matched by key)
            uint8_t key = 0;
            uint8_t count = 0;
            uint8_t keys[6] = {};
            uint32_t ids[6] = {};
        };
        static constexpr int kPerfNotes = 32;
        PerfNote perf[kPerfNotes];
        bool inPerf = false;
        // bend range set by MIDI (RPN 0, semitones; -1 = the part's master.bendrange).
        // Moving master.bendrange clears it (knobBend remembers the value it was set at).
        float rpnBend = -1.0f;
        float knobBend = -1.0f;
        uint8_t rpnMsb = 127, rpnLsb = 127;  // selected RPN (127/127 = none)
        bool sustain = false;
        float logicalPedal = 0.0f;
        Lfo lfo2;
        Rng rng;                   // lfo2 S&H of parts 1..15 (part 0 uses the engine rng)
        float ccVolume = 1.0f;     // CC7 (multi mode)
        float ccPan = 0.0f;        // CC10 as -1..1 offset (multi mode)
        float pedal = 0.0f;        // sustain pedal 0..1 (CC64, half pedal)
        float ccExpr = 1.0f;       // CC11 as part level (unless the preset routes "expression" itself)
        float exprLevel = 1.0f;    // the level CC11 gives this block (1 when the preset routes it)
        float singleGain = 1.0f;   // smoothed expression level of single-part mode
        float ccRev = 0.0f;        // CC91 reverb send (adds to send A)
        float ccCho = 0.0f;        // CC93
        // standard controllers for every preset (see partEvent): CC94 delay send, CC5 portamento
        // time, CC74 brightness, CC71 resonance, CC1 vibrato (when the preset ignores the wheel)
        float ccDly = 0.0f;
        float ccGlide = -1.0f;     // seconds, < 0 = the preset's own voice.glide
        float ccCutoffOct = 0.0f;
        float ccReso = 0.0f;
        float vibPhase = 0.0f;
        float revSend = 0.0f, dlySend = 0.0f;  // CC91 / CC94 added to send A / B this block
        float gain[4] = {};        // smoothed send L/R, dry L/R
        float target[4] = {};
        // mono / legato note stack (last note priority) and glide source
        static constexpr int kStack = 16;
        Event stack[kStack];
        int stackSize = 0;
        uint32_t monoId = 0;       // note id of the sounding mono note (its unison voices share it)
        int monoKey = -1;
        float lastPitch = -1.0f;   // pitch of the previous note (portamento source)
        // arpeggiator
        struct Arp {
            static constexpr int kMax = 16;
            Event held[kMax];      // keys currently held (or latched), in played order
            int heldCount = 0;
            int physical = 0;      // keys physically down (latch bookkeeping)
            bool running = false;
            double origin = 0.0;   // beat of step 0
            int64_t step = 0;      // next step index
            int index = 0, dir = 1;
            struct Pending { uint32_t id; uint8_t key; double offBeat; };
            Pending pend[kMax];
            int pendCount = 0;
            bool pattern = false;
            uint32_t nextId = 1;
            Rng rng{0xA5A5u};
        } arp;
        // step sequencer
        struct Seq {
            static constexpr int kMax = 32;
            bool running = false;
            double origin = 0.0;
            int64_t step = 0;
            struct Pending { uint32_t id; uint8_t key; double offBeat; };
            Pending pend[kMax];
            int pendCount = 0;
            Event held[8];           // keys held (key mode: the newest transposes)
            int heldCount = 0;
            uint32_t nextId = 1;
        } seq;
    };

    void handleEvent(const Event& e);
    void syncPerformanceInput();
    void liveEvent(Event e);
    bool externalCc(Event& e);
    void setHardwareSustain(int channel, bool down, float value = -1.0f);
    void updatePartPedal(int part);
    void updatePedalState();
    void ghostPedal(const Event& e, bool hardware = false);
    void footswitchAction(int action);
    void tapTempo();
    void processMetronome(int frames);
    void partEvent(int part, const Event& e);
    void routeZoneControl(const Event& e);
    void syncZoneConfig();
    void commitZonePrograms(uint32_t version);
    void mixBacking(float* left, float* right, int frames, const fx::FxGlobals& globals, bool active);
    void noteOn(int part, const Event& e);
    void startNote(int part, const Event& e, float glideFrom, float splitGain = 1.0f);  // allocate + start (unison) voices
    bool monoNoteOff(int part, const Event& e);                  // true if handled by the mono logic
    // arpeggiator: true if the event was taken by the part's arpeggiator
    bool arpInput(int part, const Event& e);
    void runArps(int frameOffset, int frames, bool patternOnly = false);                   // once per control block
    void arpStop(int part);
    // step sequencer (runs per control block like the arpeggiator)
    bool seqInput(int part, const Event& e);
    void runSeqs(int frameOffset, int frames);
    void seqStop(int part);
    bool seqActive() const;
    // events of the arpeggiator / sequencer inside a block start voices with
    // a delay (Voice::startDelay) and release them at Voice::releaseAt, so the
    // whole block can still be rendered in parallel
    int curOffset_ = 0;
    void releaseVoice(Voice& v) {
        if (curOffset_ > 0) v.releaseAt = curOffset_;
        else v.release();
    }
    void scheduleControl(int n, bool interleaved);
    void scheduleSlice(int off, int frames);
    void finishSchedule(int n);
    bool sliceArp_ = false, slicePattern_ = false, sliceSeq_ = false, sliceSong_ = false, slicePhrase_ = false;
    double blockArpBeat_ = 0.0;
    int sequenceRenderSlice(int off, int n, bool interleaved);
    bool needsSequenceSlices() const;
    bool strumInput(int part, const Event& e);
    void stopStrum(int part);
    void arpRelease(int part, uint32_t id, int key);
    uint64_t blockFrame_ = 0;
    bool startingStrum_ = false;

    struct SoundBank {
        PartSoundOptions options;
        uint32_t serial = 0;
        bool resetMorph = false;
    };
    struct PartSoundState {
        PerformanceMailbox<SoundBank> hostToAudio;
        // A second triple buffer publishes audio-committed programs (SST/MIDI
        // program changes) to serialized host readers without callback locks.
        std::array<SoundBank, 3> outgoing{};
        mutable std::atomic<unsigned> middle{1};
        unsigned writer = 0;
        mutable unsigned reader = 2;
        std::atomic<uint32_t> serial{0};
        mutable std::mutex hostMutex;
        SoundBank current;
        std::array<float, P::kPartParams> baseA{}, effective{};
        std::array<std::atomic<float>, P::kPartParams> display;
        float position = 0.0f, target = 0.0f, increment = 0.0f;
        int remaining = 0;
        bool haveA = false, copied = false, evaluate = false, initialize = false, insertDirty = false;
        uint32_t baselineStage = 0;

        PartSoundState();
        SoundBank hostValue() const;
        void publish(const PartSoundOptions& options, bool resetMorph);
        void publishStage();
    };
    std::unique_ptr<PartSoundState[]> sound_;
    std::atomic<uint32_t> soundPending_{0};
    uint32_t morphParts_ = 0;
    void publishSound(int part, const PartSoundOptions& options, bool resetMorph = false);
    const float* basePartParams(int part) const;
    bool syncSoundOptions();
    void applySoundOptions(int part, const SoundBank& bank);
    void applyStagedSound(int part, const PartSoundOptions& options);
    void invalidateEffective(int part);
    void evaluateMorph(int part, int frames);
    void updateMorphedInsert(int part);
    PerformanceMailbox<RhythmConfig> rhythmConfig_;
    RhythmPlayer rhythm_;
    double rhythmBeat_ = 0.0;
    bool rhythmWasEnabled_ = false;
    std::atomic<bool> rhythmFill_{false};
    std::atomic<int> statArpStep_[kNumParts];
    PhysicalPool physicalPool_;
    // song playback (audio thread state)
    std::atomic<Song*> pendingSong_{nullptr};
    std::atomic<bool> songUnload_{false};
    Song* song_ = nullptr;
    SpscQueue<Song*> retiredSongs_{8};
    // song program changes (stagePartProgram)
    static bool validPart(int part) { return part >= 0 && part < kNumParts; }
    std::atomic<StagedProgram*> stageIn_[kNumParts] = {};
    StagedProgram* stage_[kNumParts] = {};   // audio thread
    SpscQueue<StagedProgram*> retiredStages_{64};
    std::atomic<int> appliedProg_[kNumParts], songProg_[kNumParts], stagedProg_[kNumParts];
    std::atomic<uint32_t> programRevision_[kNumParts] = {};
    // phrase recorder (audio thread unless noted)
    enum PhraseCmd : int { CmdRecord = 1, CmdPlay, CmdStop, CmdClear, CmdUndo };
    SpscQueue<int> phraseCmds_{64};
    std::atomic<int> phraseBars_{0};
    std::vector<PhraseEvent> phrase_;        // sorted by beat, capacity kMaxPhraseEvents
    PhraseState phraseState_ = PhraseState::Empty;
    double phraseLen_ = 0.0, phrasePos_ = 0.0;
    size_t phraseIdx_ = 0;                   // next event to play
    uint16_t phrasePass_ = 0;
    struct HeldNote { uint32_t id; uint8_t key, channel; int8_t transpose = 0; };
    static constexpr int kPhraseHeld = 64;
    HeldNote recHeld_[kPhraseHeld];          // recorded note-ons of this pass without a note-off yet
    int recHeldCount_ = 0;
    HeldNote playHeld_[kPhraseHeld];         // notes the playback started that sound now
    int playHeldCount_ = 0;
    std::atomic<int> phraseStateA_{0}, phraseCountA_{0};
    std::atomic<double> phraseLenA_{0.0}, phrasePosA_{0.0};
    std::mutex phraseProducerMutex_;
    std::atomic<uint32_t> snapRequest_{0};
    std::atomic<bool> snapReady_{false};
    uint32_t snapToken_ = 0;
    std::vector<PhraseEvent> snapBuf_;       // capacity kMaxPhraseEvents
    double snapLen_ = 0.0;
    struct PhraseSwap {
        std::vector<PhraseEvent> events;
        double length = 0.0;
    };
    std::atomic<PhraseSwap*> pendingPhrase_{nullptr};
    SpscQueue<PhraseSwap*> retiredPhrases_{8};
    void phraseBlockStart();                 // commands, snapshot, swap
    void applyPhraseCommand(int cmd, bool allowCountIn = true);
    std::atomic<bool> phraseCountInPending_{false};
    int phraseCountInEnd_ = -1;
    bool metroRendered_ = false;
    void phraseCapture(const Event& e);      // a live event while recording
    void phraseInsert(const PhraseEvent& pe, bool atPlayhead);
    void phraseClosePass(double offBeat);    // note-offs for notes held when a pass ends
    void phraseDispatch(const Event& e);
    void phraseSilence();                    // ends the notes the playback started
    void runPhrase(int frames);              // one control block
    void takeStagedPrograms();               // block start
    void songProgramChange(int part, int program);
    void performanceNote(int part, const Event& e);  // scale lock / chord mode
    void applyStage(int part);
    // notes sounding at a program change keep their sound: a copy of the old part
    // parameters (ghost) and the old sample sets (held) until they end
    static constexpr int kGhosts = 32;
    struct LiveGhost {
        LiveChange* owner = nullptr;
        LiveFx* fx = nullptr;  // null feeds the current global rack
        int part = 0, channel = kOmniChannel;
        float logicalPedal = 0.0f;
        PianoBusState piano;
        uint16_t hardwarePedals = 0, halfPedalChannels = 0;
        VoiceContext ctx;
        Lfo lfo;
        Rng rng{1};
        float gains[4] = {}, sendA = 0.0f, sendB = 0.0f;
    };
    LiveGhost liveGhosts_[kGhosts];
    static constexpr int kLiveGraphs = 4;
    LiveChange* liveRetained_[kGhosts] = {};
    std::atomic<LiveChange*> liveIncoming_{nullptr};
    LiveChange* liveWaiting_ = nullptr;
    SpscQueue<LiveChange*> liveRetired_{64};
    std::atomic<bool> sstEnabled_{false};
    std::atomic<float> sstTimeMs_{300.0f}, sstProgress_{-1.0f};
    uint32_t liveSerial_ = 0;
    std::atomic<uint32_t> liveApplied_{0}, sampleEpoch_{0};
    int sstFrames_ = 0, sstLength_ = 1, sstFxFrames_ = 0, sstFxLength_ = 1;
    bool hasLiveTails_ = false;
    bool applyLiveChange();
    void renderLiveGhosts(float* left, float* right, int frames);
    void processLiveTails(float* left, float* right, int frames);
    void liveNoteOff(const Event& event);
    void clearLiveChanges();
    void changeLiveSampleRate(float sampleRate);
    std::mutex garbageMutex_;
    std::unique_ptr<float[]> ghostParams_;   // kGhosts x kPartParams
    bool ghostUsed_[kGhosts] = {};
    static constexpr int kHeld = kGhosts * kNumLayers;
    SampleSet* held_[kHeld] = {};
    void releaseHeld();                      // block end: frees ghosts / sets no voice uses any more
    std::atomic<bool> songPlay_{false};
    bool songWasPlaying_ = false;
    std::atomic<int64_t> songSeekReq_{-1};  // frames, -1 = none
    int64_t songFrames_ = 0;
    size_t songIdx_ = 0;
    std::atomic<double> statSongPos_{0.0}, songLen_{0.0};
    std::atomic<uint32_t> songEnds_{0};
    // part meters (written by the audio thread once per block)
    mutable std::atomic<float> meterPeak_[kNumParts];
    std::atomic<uint32_t> ccOverride_[kNumParts] = {};  // bit i = kStandardCcs[i] is learned on the part
    std::atomic<float> meterCc_[kNumParts][7];  // volume, expression, pan, bend, mod, reverb, chorus
    std::atomic<uint32_t> meterNotes_[kNumParts][4];
    float blockPeak_[kNumParts] = {};
    void songUpdate();               // block start: swaps / seeks / start-stop
    void runSong(int frameOffset, int frames);
    void songNotesOff();
    void songChase();                // controllers, bend and bend ranges in force at songIdx_
    std::atomic<bool> transport_{false};
    std::atomic<int> statSeqStep_[kNumParts];
    // send / return buses (A = reverb, B = delay)
    void updateSends();
    void processEffects(float* L, float* R, int n, const fx::FxGlobals& globals);
    std::unique_ptr<fx::FxBlock> sendRev_, sendDly_;
    std::vector<float> sendRevP_, sendDlyP_;
    float sendA_[2][kMaxBlockSize] = {}, sendB_[2][kMaxBlockSize] = {};
    int sendAQuiet_ = -1;  // negative when the bus is idle
    float sendTempo_ = 0.0f;
    int sendBQuiet_ = -1;
    double jobBeat_ = 0.0;
    float jobBpf_ = 0.0f;  // beats per frame
    double arpBeat_ = 0.0;                                       // free-running arp beat (no clock)
    // part < 0: any part (note-id matched events in multi mode)
    void noteOff(const Event& e, int part);
    void perNote(const Event& e, int part);
    void releaseSustained(int part);
    Voice* allocateVoice(int part);
    void updateEffects();
    float lfo2Rate(const float* p) const {
        const float b = fx::syncBeats((int)p[P::Lfo2Sync]);
        return b > 0.0f ? rack_->tempo() / 60.0f / b : p[P::Lfo2Rate];
    }
    void updateInserts(int part);
    void updatePartTargets();
    // CC1 vibrato / CC11 level / CC5, CC71, CC74 into the part's voice context (every block)
    void standardControllers(int part, int n);
    void processMulti(float* L, float* R, int n);
    void swapSampleSet(int part, int layer, SampleSet* s, bool playing);
    // Parallel voice rendering of a whole block (no arpeggiator running):
    // renders every active voice into the per-thread buses and returns false
    // if the block should be rendered serially instead. multi: per-part buses
    // (bus(k, c)) are filled and `used` marked; single: added into L / R.
    bool renderParallel(float* L, float* R, int n, bool multi, bool* used);
    static void renderJob(void* self, int worker, int item);
    static void insertJob(void* self, int worker, int item);  // inserts of part jobInsParts_[item]

    float sampleRate_ = 48000.0f;
    std::atomic<float> sampleRateHost_{48000.0f};
    bool prepared_ = false;

    // parameters
    std::unique_ptr<std::atomic<float>[]> sharedParams_;
    std::atomic<uint32_t> paramVersion_{1};
    uint32_t seenParamVersion_ = 0;
    float p_[P::kNumParams] = {};

    // events
    SpscQueue<Event> events_{2048};
    std::mutex producerMutex_;
    std::atomic<uint32_t> droppedEvents_{0};

    // parts 1..15 (part 0 lives in sharedParams_ / p_)
    std::unique_ptr<std::atomic<float>[]> partShared_;     // (kNumParts-1) * kPartParams
    std::atomic<uint32_t> partVersion_[kNumParts];
    uint32_t seenPartVersion_[kNumParts] = {};
    std::unique_ptr<float[]> partP_;                       // audio-thread copy
    std::atomic<float> mixShared_[kNumParts][Mix::Count];
    std::atomic<uint32_t> mixVersion_{1};
    uint32_t seenMixVersion_ = 0;
    float mix_[kNumParts][Mix::Count] = {};
    std::atomic<bool> multi_{false};
    std::atomic<int> selectedPart_{0};
    bool multiActive_ = false;  // audio thread
    PartState parts_[kNumParts];
    uint32_t seqCount_[kNumParts][kNumLayers] = {};  // SFZ round robin counters
    int keyswitch_[kNumParts][kNumLayers];            // active SFZ keyswitch (-1 = set default)
    std::unique_ptr<float[]> partBus_;  // kNumParts x 2 x kMaxBlockSize (multi mode)
    float* bus(int part, int ch) { return partBus_.get() + ((size_t)part * 2 + (size_t)ch) * kMaxBlockSize; }
    std::unique_ptr<fx::InsertChain> inserts_[kNumParts];  // per part
    float dryL_[kMaxBlockSize] = {}, dryR_[kMaxBlockSize] = {};
    float gainStep_ = 0.0f;  // per sample

    // sample sets
    std::atomic<SampleSet*> pendingSets_[kNumParts][kNumLayers];
    SampleSet* currentSets_[kNumParts][kNumLayers] = {};
    SpscQueue<SampleSet*> retired_{128};
    // Swapped-out sets stay alive while their voices fade (Voice::fadeLayer),
    // then go to retired_. Audio thread only.
    struct Retiring { SampleSet* set = nullptr; int frames = 0; };
    static constexpr int kRetiring = kNumParts * kNumLayers;  // a whole multi can swap at once
    Retiring retiring_[kRetiring];
    void advanceRetiring(int n);
    std::mutex loaderMutex_;
    mutable std::mutex nameMutex_;
    std::string setNames_[kNumParts][kNumLayers];
    std::string setPaths_[kNumParts][kNumLayers];

    // disk streaming of large samples (started with the first streamed set)
    DiskStreamer streamer_;

    // voices
    // fixed-size pool on the heap (allocated once in the constructor): 272
    // voices are ~1.7 MB, too large for an Engine on a 1 MB thread stack
    struct VoicePool {
        static constexpr int kSize = kMaxVoices + kVoicePoolExtra;
        std::unique_ptr<Voice[]> v{new Voice[kSize]};
        Voice* begin() { return v.get(); }
        Voice* end() { return v.get() + kSize; }
        const Voice* begin() const { return v.get(); }
        const Voice* end() const { return v.get() + kSize; }
        Voice& operator[](int i) { return v[i]; }
        const Voice& operator[](int i) const { return v[i]; }
    };
    VoicePool voices_;
    std::atomic<int> maxVoices_{kDefaultVoices};
    // multi-core rendering
    std::atomic<int> renderThreads_{1};
    std::mutex poolMutex_;
    std::unique_ptr<WorkerPool> poolOwner_;
    std::atomic<WorkerPool*> pool_{nullptr};
    std::unique_ptr<float[]> workBus_;  // kMaxRenderThreads x kNumParts x 2 x kMaxBlockSize
    float* workBus(int w, int part, int ch) {
        return workBus_.get() + (((size_t)w * kNumParts + (size_t)part) * 2 + (size_t)ch) * kMaxBlockSize;
    }
    uint32_t workUsed_[kMaxRenderThreads] = {};  // parts written by each worker in the current job
    static constexpr int kMaxSubBlocks = kMaxBlockSize / kControlBlock;
    float jobLfo2_[kNumParts][kMaxSubBlocks] = {};
    // Preserve the original 1088-byte job queue footprint in Engine.
    static constexpr int kJobVoiceCapacity = 544;
    static_assert(VoicePool::kSize <= kJobVoiceCapacity);
    alignas(int) uint16_t jobVoices_[kJobVoiceCapacity] = {};
    int jobInsParts_[kNumParts] = {};
    int jobN_ = 0;
    bool jobMulti_ = false;
    std::atomic<int> quality_{(int)Quality::Normal};
    uint64_t ageCounter_ = 0;
    Rng rng_{0xC0FFEEu};

    // effects
    std::unique_ptr<fx::FxRack> rack_;
    std::atomic<int> rackChain_[fx::kFxSlots] = {};
    std::atomic<int> rackChainLength_{0};
    fx::Looper looper_;
    BackingPlayer backing_;
    NpuMorph npuMorph_;
    PadSampler pads_;
    LiveSampler liveSampler_;
    ZoneNoteLedger extMidiOut_;
    std::atomic<uint32_t> zoneProgramVersion_{0};
    uint32_t zoneProgramSeen_ = 0, zoneProgramsPending_ = 0;
    bool zonesEnabled_ = false;
    std::mutex backingFxMutex_;
    std::unique_ptr<fx::FxRack> backingFxOwner_;
    std::atomic<fx::FxRack*> backingFx_{nullptr};
    float backingFxRate_ = 48000;
    bool backingFxDirty_ = true, backingFxTail_ = false;
    float backingFxBus_[2][kMaxBlockSize];
    Limiter limiter_;
    int effectsQuality_ = -1;
    const float* inL_ = nullptr;
    const float* inR_ = nullptr;
    bool inputFramesPending_ = false;

    struct PerfOffsets {
        int transpose = 0;
        float trimDb = 0.0f;
    };
    static_assert(std::atomic<PerfOffsets>::is_always_lock_free);
    std::atomic<PerfOffsets> perfOffsets_{PerfOffsets{}};
    PerfOffsets activePerfOffsets_;
    struct OutputRamp {
        float current = 1.0f, target = 1.0f, step = 0.0f;
        int remaining = 0;
        void set(float next, int frames);
        float advance();
    } perfTrim_, tunerGain_;
    int performanceRampFrames_ = 1;

    PerformanceMailbox<MetronomeConfig> metroConfig_;
    std::atomic<unsigned> metroEnabled_{0};
    Metronome metronome_;
    bool metroWasRendering_ = false, metroClockSynced_ = false;
    float metroClick_[kMaxBlockSize];
    AudioTap audioTap_;
    Tuner tuner_;

    PerformanceMailbox<PreparedPerformanceInput> performanceInput_;
    PerformanceInputConfig appliedPedalConfig_;
    uint16_t hardwareSustainDown_ = 0;
    uint16_t hardwareSustainParts_[16]{};
    uint16_t hardwareHalfParts_[16]{};
    float hardwarePedalValue_[16]{};
    bool suppressPianoPedal_ = false;
    uint16_t hardwarePedalChannels_[kNumParts]{};
    uint16_t footswitchChannels_[4]{};
    uint16_t ccSequence_ = 0;
    std::atomic<uint32_t> lastCc_{0};
    std::atomic<int> pedalState_{0};
    SpscQueue<int> hostActions_{32};
    std::mutex hostActionConsumer_;
    uint64_t lastTapFrame_ = 0;
    bool haveTap_ = false;
    uint64_t tapIntervals_[4]{};
    int tapCount_ = 0, tapNext_ = 0;
    // pending user IR: nullptr = nothing pending, &unloadIr_ = unload request
    std::atomic<fx::ConvIr*> pendingIr_[2];
    fx::ConvIr unloadIr_;
    fx::ConvIr* currentIr_[2] = {};
    SpscQueue<fx::ConvIr*> retiredIr_{16};
    std::string irNames_[2], irPaths_[2];
    MasterStage masterStage_;
    Smoother masterGain_;

    // stats (written by audio thread)
    std::atomic<int> statActive_{0}, statPeakVoices_{0}, statBlockSize_{0};
    std::atomic<float> statLoad_{0}, statLoadPeak_{0}, statUs_{0}, statUsPeak_{0}, statOutPeak_{0};
    std::chrono::steady_clock::time_point profMark_[kProfPhases + 1];
    std::atomic<float> profPeak_[kProfPhases] = {};
    float profPeakTotal_ = 0.0f;  // audio thread only
    std::atomic<bool> profReset_{false};
    bool profParallel_ = false;
    void profMark(int i) { profMark_[i] = std::chrono::steady_clock::now(); }
    std::atomic<uint64_t> statBlocks_{0};
    std::atomic<int> statPartVoices_[kNumParts];
    std::atomic<float> statClockBpm_{0.0f};

    // MIDI clock (audio thread)
    void clockEvent(ClockMsg m);
    void updateClock(int n);
    uint64_t samplePos_ = 0;           // samples rendered since prepare()
    uint64_t tickTimes_[25] = {};      // sample position of the last 25 ticks (ring)
    int tickHead_ = 0, tickCount_ = 0; // ring head / valid entries
    uint64_t ticksSinceStart_ = 0;
    float clockBpm_ = 0.0f;
    bool clockRunning_ = true;

    // silent-idle fast path (audio thread): skips voice/FX/meter scans after
    // 0.5 s of complete silence with 0 active voices and idle FX tails; wakes
    // on the very same block as any incoming MIDI/parameter/song/audio event.
    bool silentIdle_ = false;
    int silentFrames_ = 0;
};

}  // namespace hsynth
