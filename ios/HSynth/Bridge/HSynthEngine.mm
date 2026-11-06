#import "HSynthEngine.h"
#import <AVFoundation/AVFoundation.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "audio_out.h"
#include "hsynth/engine.h"
#include "hsynth/fileio.h"
#include "hsynth/gm_bank.h"
#include "hsynth/smf.h"
#include "hsynth/host_utils.h"
#include "hsynth/params.h"
#include "hsynth/preset.h"
#include "hsynth/preset_index.h"
#include "midi_in.h"

using namespace hsynth;

namespace {
NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }
std::string str(NSString* s) { return s ? std::string(s.UTF8String) : std::string(); }

// knob position <-> value (same mapping as ParamMeta on Android)
float fromNormalized(const ParamInfo& p, float t) {
    t = std::fmin(1.0f, std::fmax(0.0f, t));
    float v = (p.curve == ParamCurve::Exp && p.min > 0.0f) ? p.min * std::pow(p.max / p.min, t) : p.min + t * (p.max - p.min);
    if (p.kind != ParamKind::Float) v = std::round(v);
    return v;
}
float toNormalized(const ParamInfo& p, float v) {
    if (p.max <= p.min) return 0.0f;
    v = std::fmin(p.max, std::fmax(p.min, v));
    return (p.curve == ParamCurve::Exp && p.min > 0.0f) ? std::log(v / p.min) / std::log(p.max / p.min) : (v - p.min) / (p.max - p.min);
}
}  // namespace

@implementation HSynthEngine {
    std::unique_ptr<Engine> _engine;
    std::unique_ptr<AudioOut> _audio;
    std::unique_ptr<MidiIn> _midi;
    PresetIndex _index;
    std::string _factoryDir, _userDir;
    NSInteger _current;
    NSString* _currentName;
    BOOL _input;
    std::string _libraryDir;              // Documents/library (factory -> bundle link + user samples)
    std::string _songLoaded[kNumParts];   // preset file per part (no reload between songs)
    std::vector<SongPart> _songParts;
    std::vector<std::pair<double, double>> _tempos;
    NSString* _songName;
    NSArray<NSString*>* _lastWarnings;   // preset load warnings / song report (autotest)
}

- (instancetype)initWithFactoryDir:(NSString*)factoryDir userDir:(NSString*)userDir {
    if ((self = [super init])) {
        _factoryDir = str(factoryDir);
        _userDir = str(userDir);
        NSFileManager* fm = [NSFileManager defaultManager];
        for (NSString* d in @[@"presets", @"library", @"midi"])
            [fm createDirectoryAtPath:[userDir stringByAppendingPathComponent:d] withIntermediateDirectories:YES attributes:nil error:nil];
        // One library root for every preset: Documents/library/factory points into the app bundle (its path
        // changes with each install, so the link is renewed at start); the user's samples sit next to it.
        _libraryDir = _userDir + "/library";
        NSString* link = ns(_libraryDir + "/factory");
        [fm removeItemAtPath:link error:nil];
        [fm createSymbolicLinkAtPath:link withDestinationPath:ns(_factoryDir + "/library/factory") error:nil];
        _songName = @"";
        _engine = std::make_unique<Engine>();
        _engine->prepare(48000.0f);
        _engine->setMaxVoices(128);
        _engine->setRenderThreads(0);  // auto: helpers on the performance cores
        _audio = std::make_unique<AudioOut>(*_engine);
        _midi = std::make_unique<MidiIn>(*_engine);
        _current = -1;
        _currentName = @"Init";
        [self rescanPresets];
    }
    return self;
}

- (void)configureSession:(BOOL)input {
    AVAudioSession* s = [AVAudioSession sharedInstance];
    NSError* err = nil;
    if (input)
        [s setCategory:AVAudioSessionCategoryPlayAndRecord
                  mode:AVAudioSessionModeMeasurement
               options:AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionAllowBluetoothA2DP
                 error:&err];
    else
        [s setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&err];
    [s setPreferredIOBufferDuration:0.005 error:&err];  // ~256 frames at 48 kHz
    [s setPreferredSampleRate:48000 error:&err];
    [s setActive:YES error:&err];
}

- (BOOL)startAudio {
    [self configureSession:_input];
    std::string err;
    const bool ok = _audio->start(-1, 48000, 256, false, _input ? -1 : -2, err);
    if (!ok) NSLog(@"HSynth audio: %s", err.c_str());
    [self refreshMidi];
    return ok;
}

- (void)stopAudio { _audio->stop(); }

- (void)setAudioInput:(BOOL)on {
    if (on) {
        [[AVAudioSession sharedInstance] requestRecordPermission:^(BOOL granted) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (!granted) return;
                self->_input = YES;
                self->_audio->stop();
                [self startAudio];
            });
        }];
    } else {
        _input = NO;
        _audio->stop();
        [self startAudio];
    }
}

// ---------------------------------------------------------------- playing

- (void)noteOn:(int)key velocity:(float)velocity { _engine->sendEvent(Event::noteOn((uint32_t)(key + 1), key, velocity)); }
- (void)noteOff:(int)key { _engine->sendEvent(Event::noteOff((uint32_t)(key + 1), key, 0.0f)); }
- (void)pitchBend:(float)value { _engine->sendEvent(Event::control(EventType::PitchBend, value)); }
- (void)modWheel:(float)value { _engine->sendEvent(Event::control(EventType::ModWheel, value)); }
- (void)sustain:(BOOL)on { _engine->sendEvent(Event::control(EventType::Sustain, on ? 1.0f : 0.0f)); }
- (void)allSoundOff { _engine->sendEvent(Event::control(EventType::AllSoundOff, 0.0f)); }

// ---------------------------------------------------------------- presets

- (void)rescanPresets {
    _index.setFolders({{_factoryDir + "/presets", false}, {_userDir + "/presets", true}});
    _index.rescan();
}

- (NSArray<NSString*>*)presetNames {
    NSMutableArray* a = [NSMutableArray array];
    for (const auto& p : _index.all()) [a addObject:ns(p.name)];
    return a;
}

- (NSArray<NSString*>*)presetCategories {
    NSMutableArray* a = [NSMutableArray array];
    for (const auto& p : _index.all()) [a addObject:ns(p.category.empty() ? (p.user ? "User" : "Factory") : p.category)];
    return a;
}

- (NSInteger)currentPreset { return _current; }
- (NSString*)currentPresetName { return _currentName; }

- (BOOL)loadPreset:(NSInteger)index {
    const auto& all = _index.all();
    if (index < 0 || index >= (NSInteger)all.size()) return NO;
    Preset p;
    std::string err;
    if (!loadPresetFile(all[(size_t)index].file, p, err)) {
        NSLog(@"HSynth preset: %s", err.c_str());
        return NO;
    }
    std::vector<std::string> warnings;
    applyPreset(*_engine, p, _libraryDir, warnings);
    _lastWarnings = [self strings:warnings];
    _engine->setMultiTimbral(false);  // a single preset: leave the song's multi mode
    _midi->setMultiTimbral(false);
    _engine->collectGarbage();
    _current = index;
    _currentName = ns(p.name);
    return YES;
}

- (BOOL)savePresetNamed:(NSString*)name {
    Preset p = capturePreset(*_engine, str(name), _libraryDir);
    p.category = "User";
    p.author = "user";
    std::string safe;
    for (char c : str(name)) safe.push_back(std::isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_' ? c : '_');
    std::string err;
    const bool ok = savePresetFile(_userDir + "/presets/" + (safe.empty() ? "untitled" : safe) + ".json", p, err);
    if (ok) [self rescanPresets];
    return ok;
}

// ---------------------------------------------------------------- parameters

- (int)paramId:(NSString*)name {
    const std::string n = str(name);
    const auto& t = paramTable();
    for (size_t i = 0; i < t.size(); ++i)
        if (t[i].name == n) return (int)i;
    return -1;
}
- (NSInteger)paramCount { return (NSInteger)paramTable().size(); }
- (NSString*)paramLabel:(int)param { return param >= 0 ? ns(paramTable()[(size_t)param].label) : @""; }
- (NSString*)paramGroup:(int)param { return param >= 0 ? ns(paramTable()[(size_t)param].group) : @""; }
- (int)paramKind:(int)param { return param >= 0 ? (int)paramTable()[(size_t)param].kind : 0; }
- (NSArray<NSString*>*)paramOptions:(int)param {
    NSMutableArray* a = [NSMutableArray array];
    if (param >= 0) for (const auto& o : paramTable()[(size_t)param].options) [a addObject:ns(o)];
    return a;
}
- (float)paramValue:(int)param { return param >= 0 ? _engine->getParam(param) : 0.0f; }
- (void)setParam:(int)param value:(float)value { if (param >= 0) _engine->setParam(param, value); }
- (float)paramNormalized:(int)param {
    return param >= 0 ? toNormalized(paramTable()[(size_t)param], _engine->getParam(param)) : 0.0f;
}
- (void)setParam:(int)param normalized:(float)t {
    if (param >= 0) _engine->setParam(param, fromNormalized(paramTable()[(size_t)param], t));
}
- (float)paramDefault:(int)param { return param >= 0 ? paramTable()[(size_t)param].def : 0.0f; }

- (NSString*)formatParam:(int)param {
    if (param < 0) return @"";
    const ParamInfo& p = paramTable()[(size_t)param];
    const float v = _engine->getParam(param);
    char buf[64];
    switch (p.kind) {
        case ParamKind::Enum: {
            const int i = (int)std::lround(v);
            return i >= 0 && i < (int)p.options.size() ? ns(p.options[(size_t)i]) : ns(std::to_string(i));
        }
        case ParamKind::Bool: return v > 0.5f ? @"on" : @"off";
        case ParamKind::Int: snprintf(buf, sizeof buf, "%d %s", (int)std::lround(v), p.unit.c_str()); break;
        default:
            if (p.unit == "Hz" && v >= 1000.0f) snprintf(buf, sizeof buf, "%.2f kHz", v / 1000.0f);
            else if (p.unit == "s" && v < 1.0f) snprintf(buf, sizeof buf, "%.0f ms", v * 1000.0f);
            else if (std::fabs(v) >= 100.0f) snprintf(buf, sizeof buf, "%.0f %s", v, p.unit.c_str());
            else if (std::fabs(v) >= 10.0f) snprintf(buf, sizeof buf, "%.1f %s", v, p.unit.c_str());
            else snprintf(buf, sizeof buf, "%.3f %s", v, p.unit.c_str());
    }
    return [ns(buf) stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
}

// ---------------------------------------------------------------- songs

- (NSString*)libraryDir { return ns(_libraryDir); }
- (NSString*)midiDir { return ns(_userDir + "/midi"); }

- (nullable NSString*)loadSong:(NSString*)path {
    auto song = std::make_unique<Song>();
    std::string err;
    if (!loadSmf(str(path), *song, err)) return ns(err);
    [self rescanPresets];      // samples / presets copied in since the last scan
    GmBank bank;
    bank.build(_index.all());  // factory + user presets (GM test, SonicCell, ...)
    _engine->songPlay(false);
    std::vector<std::string> report;
    _songParts.clear();
    prepareSong(*_engine, *song, bank, _libraryDir, _songLoaded, report, &_songParts);
    _lastWarnings = [self strings:report];
    _midi->setMultiTimbral(true);
    _tempos.clear();
    for (const auto& t : song->tempos) _tempos.push_back({t.time, t.bpm});
    if (_tempos.empty()) _tempos.push_back({0.0, song->initialTempo});
    _songName = ns(song->name);
    _engine->setSong(std::move(song));
    _engine->songSeek(0.0);
    return nil;
}
- (NSString*)songName { return _songName; }
- (double)songLength { return _engine->songLength(); }
- (double)songPosition { return _engine->songPosition(); }
- (BOOL)songPlaying { return _engine->songPlaying(); }
- (NSInteger)songEndCount { return (NSInteger)_engine->songEndCount(); }
- (void)songPlay:(BOOL)on { _engine->songPlay(on); }
- (void)songSeek:(double)seconds { _engine->songSeek(seconds); }
- (double)songTempoAt:(double)t {
    double b = _tempos.empty() ? 120.0 : _tempos.front().second;
    for (const auto& x : _tempos) {
        if (x.first > t) break;
        b = x.second;
    }
    return b;
}
- (const SongPart*)songPart:(int)ch {
    for (const auto& p : _songParts)
        if (p.channel == ch) return &p;
    return nullptr;
}
- (BOOL)songChannelUsed:(int)ch { return [self songPart:ch] != nullptr; }
- (NSString*)songChannelPreset:(int)ch {
    const SongPart* p = [self songPart:ch];
    return p ? ns(p->presetName) : @"";
}
- (NSString*)songChannelGm:(int)ch {
    const SongPart* p = [self songPart:ch];
    return p ? (p->drum ? @"Drums" : ns(p->gmName)) : @"";
}
- (NSArray<NSNumber*>*)partMeters {
    Engine::PartMeter m[kNumParts];
    _engine->partMeters(m);
    NSMutableArray* a = [NSMutableArray arrayWithCapacity:kNumParts * 9];
    for (int k = 0; k < kNumParts; ++k) {
        const float v[9] = {m[k].level, m[k].volume, m[k].expression, m[k].pan, m[k].bend,
                            m[k].mod, m[k].reverb, m[k].chorus, (float)m[k].voices};
        for (float x : v) [a addObject:@(x)];
    }
    return a;
}
- (NSArray<NSNumber*>*)noteMasks {
    Engine::PartMeter m[kNumParts];
    _engine->partMeters(m);
    NSMutableArray* a = [NSMutableArray arrayWithCapacity:kNumParts * 4];
    for (int k = 0; k < kNumParts; ++k)
        for (int w = 0; w < 4; ++w) [a addObject:@(m[k].notes[w])];
    return a;
}
- (void)setPartMix:(int)part id:(int)mixId value:(float)value { _engine->setPartMix(part, mixId, value); }
- (float)partMix:(int)part id:(int)mixId { return _engine->getPartMix(part, mixId); }

// ---------------------------------------------------------------- status

- (int)activeVoices { return _engine->stats().activeVoices; }
- (float)cpuLoad { return _engine->stats().cpuLoad; }

- (NSArray<NSString*>*)midiInputs {
    NSMutableArray* a = [NSMutableArray array];
    for (const auto& n : _midi->openPorts()) [a addObject:ns(n)];
    return a;
}
- (void)refreshMidi { _midi->openAll(); }

- (BOOL)muted { return _audio->muted(); }
- (void)setMuted:(BOOL)m { _audio->setMuted(m); }
- (NSString*)audioDevice {
    const AudioOutStats s = _audio->stats();
    AVAudioSession* a = [AVAudioSession sharedInstance];
    return [NSString stringWithFormat:@"%s, %d Hz, period %d x %d (session %.1f ms, output latency %.1f ms)",
                                      s.backend.c_str(), s.sampleRate, s.periodFrames, s.periods,
                                      a.IOBufferDuration * 1000.0, a.outputLatency * 1000.0];
}
- (int)peakVoices { return _engine->stats().peakVoices; }
- (float)cpuLoadPeak { return _engine->stats().cpuLoadPeak; }
- (float)callbackLoadMax { return _audio->stats().callbackLoadMax; }
- (float)callbackUsAvg { return _audio->stats().callbackUsAvg; }
- (int)lateCallbacks { return _audio->stats().lateCallbacks; }
- (int)callbackFrames { return _audio->stats().lastCallbackFrames; }
- (NSArray<NSString*>*)strings:(const std::vector<std::string>&)v {
    NSMutableArray* a = [NSMutableArray array];
    for (const auto& x : v) [a addObject:ns(x)];
    return a;
}
- (NSArray<NSString*>*)lastWarnings { return _lastWarnings ?: @[]; }
- (void)resetPeaks {
    _engine->resetPeakStats();
    _audio->resetPeaks();
}

@end
