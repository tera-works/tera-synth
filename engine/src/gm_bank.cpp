#include "hsynth/gm_bank.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "hsynth/host_utils.h"
#include "hsynth/preset.h"

namespace hsynth {

namespace {

const char* const kGmNames[128] = {
    "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1",
    "Electric Piano 2", "Harpsichord", "Clavinet", "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba",
    "Xylophone", "Tubular Bells", "Dulcimer", "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ",
    "Reed Organ", "Accordion", "Harmonica", "Tango Accordion", "Acoustic Guitar (nylon)", "Acoustic Guitar (steel)",
    "Electric Guitar (jazz)", "Electric Guitar (clean)", "Electric Guitar (muted)", "Overdriven Guitar",
    "Distortion Guitar", "Guitar Harmonics", "Acoustic Bass", "Electric Bass (finger)", "Electric Bass (pick)",
    "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2", "Violin", "Viola", "Cello",
    "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani", "String Ensemble 1",
    "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice",
    "Orchestra Hit", "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1",
    "Synth Brass 2", "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon",
    "Clarinet", "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)", "Lead 4 (chiff)", "Lead 5 (charang)",
    "Lead 6 (voice)", "Lead 7 (fifths)", "Lead 8 (bass + lead)", "Pad 1 (new age)", "Pad 2 (warm)",
    "Pad 3 (polysynth)", "Pad 4 (choir)", "Pad 5 (bowed)", "Pad 6 (metallic)", "Pad 7 (halo)", "Pad 8 (sweep)",
    "FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)", "FX 4 (atmosphere)", "FX 5 (brightness)", "FX 6 (goblins)",
    "FX 7 (echoes)", "FX 8 (sci-fi)", "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
    "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause",
    "Gunshot"};

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// The sampled "GM Test" instruments (exact names).
struct Alias { const char* name; int program; };
const Alias kGmTest[] = {{"grand piano", 0},    {"e.piano", 4},         {"vibraphone", 11}, {"drawbar organ", 16},
                         {"nylon guitar", 24},  {"finger bass", 33},    {"harp", 46},       {"string ensemble", 48},
                         {"choir aahs", 52},    {"trumpet", 56},        {"brass section", 61}, {"alto sax", 65},
                         {"flute", 73},         {"warm pad", 89}};
// Factory presets that stand in for GM programs (keyword in the name).
const Alias kFactory[] = {{"tonewheel organ", 16}, {"drawbar organ", 16}, {"rotary organ", 18}, {"fm grand", 0},
                          {"glass piano", 0},      {"hybrid piano", 4},   {"fm6 tine", 4},      {"fm e.piano", 5},
                          {"lo-fi keys", 4},       {"fm bell", 14},       {"crystal keys", 98}, {"auto-wah clav", 7},
                          {"comb pluck", 45},      {"ensemble strings", 48}, {"formant choir", 91}, {"va brass", 62},
                          {"fm6 brass", 61},       {"fm bass", 38},       {"ladder bass", 38},  {"acid bass", 38},
                          {"sync bass", 39},       {"square lead", 80},   {"supersaw", 81},     {"sync supersaw", 81},
                          {"expressive", 81},      {"amp stack lead", 84}, {"ring lead", 83},   {"talk box", 85},
                          {"formant lead", 85},    {"va pad", 90},        {"hybrid pad", 88},   {"warm pad", 89},
                          {"shimmer pad", 94},     {"granular", 94},      {"morph pad", 95},    {"trance gate", 99},
                          {"pressure", 81}};

}  // namespace

const char* gmProgramName(int program) { return kGmNames[std::max(0, std::min(127, program))]; }

void GmBank::build(const std::vector<PresetInfo>& presets) {
    entries_.clear();
    for (const PresetInfo& p : presets) {
        const std::string n = lower(p.name), cat = lower(p.category);
        Entry e;
        e.file = p.file;
        e.name = p.name;
        const bool kit = n.find("rhythm") != std::string::npos || n.find("drum kit") != std::string::npos ||
                         n.find("kit") != std::string::npos || cat.find("drum") != std::string::npos;
        if (const size_t c = n.find("capital "); c != std::string::npos && !kit) {
            const int num = std::atoi(n.c_str() + c + 8);
            if (num >= 1 && num <= 128) {
                e.program = num - 1;
                e.quality = 3;
                entries_.push_back(e);
            }
            continue;
        }
        if (kit) {
            e.drum = true;
            e.quality = n.find("gm2 standard") != std::string::npos ? 4
                      : n.find("standard drum kit") != std::string::npos ? 3
                      : n.find("standard") != std::string::npos ? 2 : 1;
            entries_.push_back(e);
            continue;
        }
        if (cat.find("gm test") != std::string::npos) {
            for (const Alias& a : kGmTest)
                if (n == a.name) {
                    e.program = a.program;
                    e.quality = 2;
                    entries_.push_back(e);
                }
            continue;
        }
        for (const Alias& a : kFactory)
            if (n.find(a.name) != std::string::npos) {
                e.program = a.program;
                e.quality = 1;
                entries_.push_back(e);
                break;
            }
    }
}

const GmBank::Entry* GmBank::pick(int program, bool drum) const {
    const Entry* best = nullptr;
    if (drum) {
        for (const Entry& e : entries_)
            if (e.drum && (!best || e.quality > best->quality)) best = &e;
        return best;
    }
    program = std::max(0, std::min(127, program));
    // 1. the program itself
    for (const Entry& e : entries_)
        if (!e.drum && e.program == program && (!best || e.quality > best->quality)) best = &e;
    if (best) return best;
    // 2. the same GM family (8 programs), nearest first; 3. related families
    static const int kFallback[16][3] = {
        {1, 2, 3},   {0, 2, 3},   {0, 1, 11},  {0, 1, 13},  // piano, chromatic, organ, guitar
        {3, 0, 1},   {6, 11, 0},  {5, 11, 0},  {11, 6, 5},  // bass, strings, ensemble, brass
        {9, 7, 6},   {8, 6, 0},   {11, 7, 0},  {6, 5, 10},  // reed, pipe, synth lead, synth pad
        {11, 6, 1},  {3, 1, 0},   {1, 0, 3},   {11, 1, 0}}; // synth fx, ethnic, percussive, sound fx
    const int fam = program / 8;
    int families[4] = {fam, kFallback[fam][0], kFallback[fam][1], kFallback[fam][2]};
    for (int f : families) {
        int bestDist = 1000;
        for (const Entry& e : entries_) {
            if (e.drum || e.program / 8 != f) continue;
            const int dist = std::abs(e.program - program) * 4 - e.quality;
            if (dist < bestDist) {
                bestDist = dist;
                best = &e;
            }
        }
        if (best) return best;
    }
    for (const Entry& e : entries_)
        if (!e.drum && (!best || e.quality > best->quality)) best = &e;
    return best;
}

bool prepareSong(Engine& engine, const Song& song, const GmBank& bank, const std::string& libraryRoot,
                 std::string loaded[kNumParts], std::vector<std::string>& report, std::vector<SongPart>* parts) {
    engine.setMultiTimbral(true);
    const float tempo = std::max(20.0f, std::min(300.0f, song.initialTempo));
    engine.setParam(P::fxChain(P::FxTempo), tempo);
    bool any = false;
    for (int ch = 0; ch < 16; ++ch) {
        if (!song.channelUsed[ch]) continue;
        any = true;
        const bool drum = ch == 9;
        const int program = song.initialProgram[ch] < 0 ? 0 : song.initialProgram[ch];
        const GmBank::Entry* e = bank.pick(program, drum);
        // the part listens to the channel, full range, centred
        float mix[Mix::Count];
        defaultPartMix(ch, mix);
        for (int i = 0; i < Mix::Count; ++i) engine.setPartMix(ch, i, mix[i]);
        const std::string want = drum ? "drums" : gmProgramName(program);
        SongPart sp;
        sp.channel = ch;
        sp.drum = drum;
        sp.program = program;
        sp.gmName = drum ? "Drum Kit" : gmProgramName(program);
        if (!e) {
            report.push_back("ch " + std::to_string(ch + 1) + ": no preset for " + want);
            if (parts) parts->push_back(sp);
            continue;
        }
        if (loaded[ch] != e->file) {
            Preset p;
            std::string err;
            std::vector<std::string> warnings;
            if (!loadPresetFile(e->file, p, err) || !applyPartPreset(engine, ch, p, libraryRoot, warnings)) {
                report.push_back("ch " + std::to_string(ch + 1) + ": " + err);
                if (parts) parts->push_back(sp);
                continue;
            }
            loaded[ch] = e->file;
        }
        engine.setPartParam(ch, P::BendRange, 2.0f);  // GM default
        engine.setPartParam(ch, P::ArpOn, 0.0f);
        engine.setPartParam(ch, P::SeqOn, 0.0f);
        report.push_back("ch " + std::to_string(ch + 1) + ": " + want + " -> " + e->name);
        sp.presetName = e->name;
        if (parts) parts->push_back(sp);
    }
    return any;
}

}  // namespace hsynth
