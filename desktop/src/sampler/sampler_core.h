// Auto-sampler core: turns a note-by-note recording source into a multisampled
// instrument (WAV + SFZ + HSynth preset). Source independent; see
// source_sf2.cpp (SoundFont rendering) and source_live_win32.cpp (MIDI out +
// audio capture).
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace sampler {

struct Recording {
    std::vector<float> left, right;
    bool clipped = false;  // input reached full scale
};

struct NoteRequest {
    int note = 60, velocity = 100;
    double holdSec = 3.0;        // maximum time the key is held
    double maxReleaseSec = 2.0;  // maximum recording time after note-off
    // > 0: release the key early once the sound has decayed this many dB
    // below its peak (decaying sounds: piano, plucks, drums)
    double stopBelowDb = 0.0;
};

// A sound source that can play one note and return the recorded audio.
class Source {
public:
    virtual ~Source() = default;
    virtual int sampleRate() const = 0;
    // Select the sound to sample. bank = MSB * 128 + LSB; drum kits play on the
    // source's drum channel (SoundFonts: bank 128).
    virtual bool selectProgram(int bank, int program, bool drum, std::string& error) = 0;
    // Play the note, release, and keep recording until the tail reaches the
    // noise floor or maxReleaseSec.
    virtual bool record(const NoteRequest& req, Recording& out, std::string& error) = 0;
    // Peak level of the input with nothing playing (0 for rendered sources).
    virtual float noisePeak() const { return 0.0f; }
    // Lower the output level after clipping (live sources); false if not possible.
    virtual bool reduceLevel() { return false; }
};

struct InstrumentSpec {
    std::string name;          // folder / file name, e.g. "grand_piano"
    std::string title;         // preset title, e.g. "GM Grand Piano"
    std::string category = "GM Test";
    int bank = 0, program = 0;
    bool drum = false;
    int lowNote = 36, highNote = 96, step = 3;  // sampled notes
    std::vector<int> keys;     // explicit key list (drum kits); overrides low/high/step
    std::vector<std::string> keyNames;  // optional names for `keys` (comments in the SFZ)
    std::vector<int> velocities = {60, 115};
    double holdSec = 3.0;
    double maxReleaseSec = 2.0;
    double stopBelowDb = 0.0;  // see NoteRequest
    bool keyScaledHold = false;  // lower notes are held longer (decaying sounds)
    enum class Loop { Auto, On, Off } loop = Loop::Auto;
    bool oneShot = false;      // drums: play to the end, ignore note-off
    int ampVeltrack = 35;      // SFZ amp_veltrack written to <global>
    float presetRelease = 0.3f;
    float presetReverb = 0.15f;
};

struct Options {
    std::string outDir;        // instrument folders are created here
    std::string presetDir;     // HSynth presets (empty = none)
    std::string presetPrefix = "GM ";
    std::string libraryRoot;   // presets reference samples relative to this
    float targetPeakDb = -1.0f;
    double silenceDb = -70.0;  // tail / lead-in threshold relative to note peak
    bool keepStereo = true;    // false: always mono
    int decimate = 1;          // 2: store at half the capture rate (half-band filter)
    bool verbose = true;
    // Polled between notes; returning true abandons the instrument (nothing is
    // written, so a resumed batch samples it again from the start).
    std::function<bool()> stopRequested;
};

struct InstrumentResult {
    int samples = 0, loops = 0, clipped = 0;
    double seconds = 0;   // recorded audio
    double megabytes = 0;
    float gainDb = 0;     // normalisation applied
};

// Sample one instrument. Returns false on fatal errors (message in error).
bool sampleInstrument(Source& src, const InstrumentSpec& spec, const Options& opt, std::string& error,
                      InstrumentResult* result = nullptr);

// Plays a few test notes and fills in the sampling plan of `spec`: velocity
// layers (from how much the timbre changes with velocity), amp_veltrack,
// loop / hold strategy (sustained or decaying), release length. For drum kits
// only the velocity layers and tail length are chosen (keys come from spec).
struct ProbeReport {
    bool silent = false, sustained = false;
    int probeNote = 60;
    double decaySec = 0, releaseSec = 0;  // to -50 dB while held / after note-off
    double timbreDb = 0, levelDb = 0;      // soft vs hard: spectral and level difference
    std::string summary;
};
bool probeInstrument(Source& src, InstrumentSpec& spec, ProbeReport& report, std::string& error);

// Error text used when Options::stopRequested ended an instrument early.
constexpr const char* kStoppedError = "stopped by request";

// Curated General MIDI set used by `hsynth_sampler sf2 --set gm`.
std::vector<InstrumentSpec> gmTestSet();

}  // namespace sampler
