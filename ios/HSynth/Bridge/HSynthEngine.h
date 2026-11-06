// Objective-C face of the C++ engine for the SwiftUI app: audio (miniaudio /
// CoreAudio), MIDI in (CoreMIDI), presets and parameters by name.
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface HSynthEngine : NSObject

/// factoryDir holds "presets" and "library" (bundled); userDir is writable (Documents).
- (instancetype)initWithFactoryDir:(NSString*)factoryDir userDir:(NSString*)userDir;

- (BOOL)startAudio;
- (void)stopAudio;
- (void)setAudioInput:(BOOL)on;   // microphone for the vocoder / looper (asks for permission)

// playing
- (void)noteOn:(int)key velocity:(float)velocity NS_SWIFT_NAME(noteOn(_:velocity:));
- (void)noteOff:(int)key;
- (void)pitchBend:(float)value;   // -1..1
- (void)modWheel:(float)value;    // 0..1
- (void)sustain:(BOOL)on;
- (void)allSoundOff;

// presets
@property(nonatomic, readonly) NSArray<NSString*>* presetNames;
@property(nonatomic, readonly) NSArray<NSString*>* presetCategories;
@property(nonatomic, readonly) NSInteger currentPreset;
@property(nonatomic, readonly) NSString* currentPresetName;
- (void)rescanPresets;
- (BOOL)loadPreset:(NSInteger)index;
- (BOOL)savePresetNamed:(NSString*)name;

// parameters (names as in presets: "filter.cutoff", "amp.attack", ...)
- (int)paramId:(NSString*)name;
- (NSString*)paramLabel:(int)param;
- (NSString*)paramGroup:(int)param;
- (NSInteger)paramCount;
/// 0 float, 1 int, 2 enum, 3 bool
- (int)paramKind:(int)param;
- (NSArray<NSString*>*)paramOptions:(int)param;
- (float)paramValue:(int)param;
- (void)setParam:(int)param value:(float)value;
/// knob position 0..1 (exponential parameters map like the knobs on the other hosts)
- (float)paramNormalized:(int)param;
- (void)setParam:(int)param normalized:(float)t;
- (float)paramDefault:(int)param;
- (NSString*)formatParam:(int)param;

// songs (Standard MIDI Files): GM sounds from every preset (factory + user, e.g. GM test / SonicCell)
/// Loads the file and the sounds of its channels into the 16 parts (multi mode). nil on success, else the error.
- (nullable NSString*)loadSong:(NSString*)path;
@property(nonatomic, readonly) NSString* songName;
@property(nonatomic, readonly) double songLength;
@property(nonatomic, readonly) double songPosition;
@property(nonatomic, readonly) BOOL songPlaying;
/// Counts natural song ends (the player advances the playlist when it changes).
@property(nonatomic, readonly) NSInteger songEndCount;
- (void)songPlay:(BOOL)on;
- (void)songSeek:(double)seconds;
/// Tempo at a time (tempo map of the loaded song).
- (double)songTempoAt:(double)seconds;
/// Channel (= part) 0..15 used by the song; preset and GM names of its sound.
- (BOOL)songChannelUsed:(int)ch;
- (NSString*)songChannelPreset:(int)ch;
- (NSString*)songChannelGm:(int)ch;
/// Per part meters, packed: [level, volume, expression, pan, bend, mod, reverb, chorus, voices] x 16 (level resets on read).
- (NSArray<NSNumber*>*)partMeters;
/// Keys sounding per part: 16 x 4 words of 32 bits.
- (NSArray<NSNumber*>*)noteMasks;
/// Mix of a part: 0 volume, 1 pan, 2 mute.
- (void)setPartMix:(int)part id:(int)mixId value:(float)value;
- (float)partMix:(int)part id:(int)mixId;

// folders
/// Documents/library: user samples ("gm", "soniccell", ...); "factory" links to the bundle
@property(nonatomic, readonly) NSString* libraryDir;
/// Documents/midi
@property(nonatomic, readonly) NSString* midiDir;

// status
@property(nonatomic, readonly) int activeVoices;
@property(nonatomic, readonly) float cpuLoad;
@property(nonatomic, readonly) NSArray<NSString*>* midiInputs;
- (void)refreshMidi;

// audio timing (autotest): peaks since resetPeaks
@property(nonatomic) BOOL muted;
@property(nonatomic, readonly) NSString* audioDevice;   // "backend, rate Hz, period x periods"
@property(nonatomic, readonly) int peakVoices;
@property(nonatomic, readonly) float cpuLoadPeak;       // engine process() time / block time
@property(nonatomic, readonly) float callbackLoadMax;   // whole callback time / callback period
@property(nonatomic, readonly) float callbackUsAvg;
@property(nonatomic, readonly) int lateCallbacks;       // callback came > 2.5 periods late (likely audible)
@property(nonatomic, readonly) int callbackFrames;
- (void)resetPeaks;
/// Warnings of the last loadPreset (missing samples, ...) or the report of the last loadSong.
@property(nonatomic, readonly) NSArray<NSString*>* lastWarnings;

@end

NS_ASSUME_NONNULL_END
