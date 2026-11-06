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

// status
@property(nonatomic, readonly) int activeVoices;
@property(nonatomic, readonly) float cpuLoad;
@property(nonatomic, readonly) NSArray<NSString*>* midiInputs;
- (void)refreshMidi;

@end

NS_ASSUME_NONNULL_END
