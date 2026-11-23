package com.hsynth.app

/**
 * Thin Kotlin facade over the C++ engine (libhsynth_jni). All methods are
 * Run file I/O, sample analysis and export, capture stop/discard, JSON remote
 * requests and MIDI polling on a background thread.
 */
object NativeEngine {
    init {
        System.loadLibrary("hsynth_jni")
        nativeCreate()
    }

    object PerNote { const val PITCH = 0; const val PRESSURE = 1; const val TIMBRE = 2; const val X = 3; const val HOLD = 4 }
    object Control { const val PITCH_BEND = 0; const val MOD_WHEEL = 1; const val CHANNEL_PRESSURE = 2; const val SUSTAIN = 3; const val CC = 4; const val ALL_NOTES_OFF = 5; const val ALL_SOUND_OFF = 6 }

    external fun nativeCreate()
    // songs (Standard MIDI Files): load = JSON {ok, name, length, report[] | error}; state = {pos, len, playing}
    external fun nativeSongLoad(path: String, libraryRoot: String, presetDirs: String): String
    external fun nativeSongPlay(on: Boolean)
    external fun nativeSongSeek(seconds: Double)
    external fun nativeSongState(): DoubleArray
    /** Preset playing on each channel after mid-song program changes ("" = as loaded). */
    external fun nativeSongPartPresets(): Array<String>
    external fun nativePlayerState(): FloatArray
    external fun nativeNoteMasks(): IntArray
    external fun nativeSetPartMix(part: Int, id: Int, value: Float)
    external fun nativeStart(
        deviceId: Int,
        exclusive: Boolean,
        bufferBursts: Int,
        sampleRate: Int = 48000,
        alsaDirect: Boolean = false
    ): Boolean
    external fun nativeAudioBackendNote(): String
    external fun nativeSetAlsaGain(gain: Float)
    external fun nativeStop()

    external fun nativeNoteOn(id: Int, key: Int, velocity: Float)
    external fun nativeNoteOff(id: Int, key: Int, velocity: Float)
    external fun nativePerNote(type: Int, id: Int, key: Int, value: Float)
    external fun nativeControl(type: Int, value: Float, controller: Int)
    /** [timestampNs]: MidiReceiver timestamp (System.nanoTime of the sender), 0 = none; only measured. */
    external fun nativeMidi(data: ByteArray, offset: Int, count: Int, ump: Boolean, source: Int, timestampNs: Long)
    // SysEx remote control (docs/SYSEX.md)
    external fun nativeRemoteInit(libraryRoot: String, presetDirs: String, userPresets: String, midiDir: String, version: String)
    external fun nativeRemoteNext(timeoutMs: Int): ByteArray?
    external fun nativeRemoteSource(): Int
    external fun nativeRemoteChanges(): Int
    external fun nativeRemoteSongCount(): Int
    external fun nativeRemoteSongPath(): String
    /** Synchronous JSON request, using the same operations as SysEx. Call after nativeRemoteInit. */
    external fun nativeRemoteHandle(json: String): String

    // Backing tracks. Load returns {ok,name,dur,sr,ch} or {ok:false,error}.
    external fun nativeAudioLoad(path: String): String
    external fun nativeAudioLoadStems(vocals: String, drums: String, bass: String, other: String): String
    external fun nativeAudioSetStem(stem: Int, gain: Float, mute: Boolean, solo: Boolean)
    external fun nativeAudioStemState(): String

    // File operations run synchronously on the calling worker.
    external fun nativeStftExtract(wavPath: String, outMagPath: String, maxFrames: Int): String
    external fun nativeStftReconstruct(wavPath: String, maskPaths: Array<String>, outDir: String): String
    external fun nativeSeparateStemsFast(wavPath: String, outDir: String): String
    external fun nativeAudioPlay(on: Boolean)
    external fun nativeAudioSeek(sec: Double)
    /** [position seconds, duration seconds, playing, loop A seconds, loop B seconds, loop enabled]. */
    external fun nativeAudioState(): DoubleArray
    external fun nativeAudioSetLoop(a: Double, b: Double, on: Boolean)
    external fun nativeAudioSetVolume(v: Float)
    external fun nativeAudioSetSpeed(r: Float)
    external fun nativeAudioSetPitch(semis: Float)
    external fun nativeAudioSetRoute(direct: Boolean)
    external fun nativeAudioPeaks(bins: Int): FloatArray

    // Pads are zero-based. Load returns {ok,frames,sr,dur} or {ok:false,error}.
    external fun nativePadLoad(pad: Int, path: String): String
    external fun nativePadSetup(pad: Int, level: Float, pitchSemis: Float, mode: Int, choke: Int,
                                velSens: Int, startFrac: Float, endFrac: Float)
    external fun nativePadTrigger(pad: Int, vel: Float)
    external fun nativePadRelease(pad: Int)
    external fun nativePadStopAll()
    external fun nativePadClear(pad: Int)
    external fun nativePadPeaks(pad: Int, bins: Int): FloatArray
    external fun nativePadLevels(out: FloatArray)

    // Source 0 uses the enabled audio input. Source 1 resamples the internal master output.
    external fun nativeSamplerStart(src: Int, maxSec: Float, armDb: Float): Boolean
    external fun nativeSamplerStop(): Double
    /** [state, recorded seconds, peak dBFS]. State is Idle, Armed, Recording or Ready (0..3). */
    external fun nativeSamplerState(): FloatArray
    external fun nativeSamplerPeaks(bins: Int): FloatArray
    /** [startFrame, endFrame, gainDb, loopStart, loopEnd, rootMidi, sampleRate]. Ends are exclusive. */
    external fun nativeSamplerAnalyze(): FloatArray
    external fun nativeSamplerAudition(on: Boolean, loop: Boolean, startFrame: Int, endFrame: Int)
    /** Empty string on success, or the file export error. */
    external fun nativeSamplerExport(path: String, startFrame: Int, endFrame: Int, gainDb: Float,
                                     loopStart: Int, loopEnd: Int, xfadeMs: Float, rootMidi: Int): String
    external fun nativeSamplerDiscard()

    /** Worker-only. Repeated [slot, length, MIDI bytes...] records, or null on timeout. */
    external fun nativeExtMidiNext(timeoutMs: Int): ByteArray?
    external fun nativeExtMidiAllOff(slot: Int)
    external fun nativeExtSend(slot: Int, bytes: ByteArray)

    // Bluetooth LE MIDI client and server (BleMidi.kt)
    external fun nativeBlePacket(data: ByteArray, source: Int)
    external fun nativeBleReset(source: Int)
    external fun nativeBleEncode(msg: ByteArray, maxPacket: Int, timeMs: Int): Array<ByteArray>
    external fun nativeBleAggressiveInterval(): Int
    external fun nativeBleVerifiedBoot(): Boolean
    external fun nativeSetMpe(enabled: Boolean)
    external fun nativeTakeProgramChange(): Int
    external fun nativeMidiEventCount(): Int

    external fun nativeSetParam(id: Int, value: Float)
    external fun nativeGetParam(id: Int): Float
    external fun nativeGetAllParams(): FloatArray
    external fun nativeParamMetadata(): String

    external fun nativeLoadPreset(path: String, libraryRoot: String): String
    external fun nativeSavePreset(path: String, name: String, category: String, libraryRoot: String): String
    external fun nativePresetJson(name: String, libraryRoot: String): String
    external fun nativeLoadSamples(layer: Int, path: String): String
    external fun nativeUnloadSamples(layer: Int)
    external fun nativeLoadWavetable(layer: Int, path: String): String
    external fun nativeListSoundBankPresets(path: String): String
    external fun nativeLoadSoundBankPreset(layer: Int, path: String, bank: Int, program: Int, isDrum: Boolean): String
    external fun nativeLoadDecentSampler(layer: Int, dspresetPath: String): String
    external fun nativeInspectDx7Sysex(path: String): String
    external fun nativeLoadDx7Voice(path: String, index: Int): String
    external fun nativeImportDx7Bank(path: String, outDir: String, prefix: String): String
    external fun nativeLoadAnyWavetable(layer: Int, path: String): String
    external fun nativeWavetablePreview(layer: Int, pos: Float, outSamples: FloatArray): Boolean
    external fun nativeSampleSetName(layer: Int): String
    external fun nativeInspectSamples(path: String): String
    external fun nativeSupportedSfzOpcodes(): String

    external fun nativeSetMaxVoices(n: Int)
    external fun nativeSetQuality(q: Int)
    external fun nativeGetStats(): FloatArray
    external fun nativeResetPeaks()
    external fun nativePeakProfile(): FloatArray
    external fun nativeCollectGarbage()

    // effect rack
    object Looper { const val TAP = 1; const val STOP = 2; const val CLEAR = 3; const val UNDO = 4; const val PLAY = 5 }
    external fun nativeLoadImpulse(target: Int, path: String): String
    external fun nativeUnloadImpulse(target: Int)
    external fun nativeImpulseName(target: Int): String
    external fun nativeLooperCommand(cmd: Int)
    external fun nativeLooperStatus(): FloatArray
    external fun nativeFxChain(): IntArray
    external fun nativeFmAlgorithm(index: Int): IntArray
    external fun nativeSetAllParams(values: FloatArray)
    external fun nativeSetTransport(on: Boolean)
    external fun nativeSetInput(on: Boolean): Boolean
    external fun nativeInputEnabled(): Boolean
    external fun nativeSeqPosition(): Int

    // Fills up to 16 triples (frequency ratio, relative gain, T60 seconds).
    external fun nativePhysModes(part: Int, layer: Int, out: FloatArray): Int
    // Morph operations target the current edit part. Dump is [id, a, b, ...].
    external fun nativeMorphSetA()
    external fun nativeMorphCaptureB(): Int
    external fun nativeMorphAssign(id: Int, a: Float, b: Float)
    external fun nativeMorphClear(id: Int)
    external fun nativeMorphClearAll()
    external fun nativeMorphDump(): FloatArray
    external fun nativeMorphNow(id: Int): Float

    // AI morph uses slots 0..15 and targets the current edit part. Compile
    // and evaluation can block on NNAPI and should run on a worker.
    external fun nativeNpuStatus(): String
    external fun nativeNpuMorphCaptureAnchor(slot: Int, x: Float, y: Float, name: String): Boolean
    external fun nativeNpuMorphLoadPresetAnchor(slot: Int, x: Float, y: Float, path: String, libRoot: String): String
    external fun nativeNpuMorphRemoveAnchor(slot: Int)
    external fun nativeNpuMorphClearAnchors()
    external fun nativeNpuMorphCompile(): String
    external fun nativeNpuMorphEval(x: Float, y: Float): Float
    external fun nativeNpuMorphAnchorsJson(): String

    external fun nativeSetArpPatternJson(part: Int, json: String): Boolean
    external fun nativeGetArpPatternJson(part: Int): String
    external fun nativeArpStep(): Int
    external fun nativeSetRhythmJson(json: String): Boolean
    external fun nativeGetRhythmJson(): String
    external fun nativeSetRhythmFill(on: Boolean)
    external fun nativeRhythmPatternsJson(): String
    external fun nativeInputPeak(): Float
    external fun nativeRecordStart(path: String): Boolean
    external fun nativeRecordStop(): Double
    external fun nativeRecordSeconds(): Double
    external fun nativeTransport(): Boolean
    external fun nativeSetMulti(on: Boolean)
    external fun nativeIsMulti(): Boolean
    external fun nativeSetEditPart(part: Int)
    external fun nativeEditPart(): Int
    external fun nativeGetPartMix(part: Int, id: Int): Float
    external fun nativeSaveMulti(path: String, name: String, libraryRoot: String): String
    external fun nativeLoadMulti(path: String, libraryRoot: String): String

    external fun nativeSetSst(on: Boolean, ms: Int)
    external fun nativeSstProgress(): Float
    external fun nativeLoadPresetSst(path: String, libRoot: String): String
    external fun nativeLoadMultiSst(path: String, libRoot: String): String
    external fun nativeMixMetadata(): String

    external fun nativePhraseCommand(cmd: Int) // 1 record, 2 play, 3 stop, 4 clear, 5 undo
    external fun nativePhraseSetBars(bars: Int)
    external fun nativePhraseStatus(): FloatArray // state, lengthBeats, positionBeats, eventCount, bars
    // Snapshot, file and preset I/O calls belong on the loader thread.
    external fun nativePhraseQuantize(gridBeats: Float, strength: Float): Boolean
    external fun nativePhraseExport(path: String, repeats: Int): String
    external fun nativeApplyScala(part: Int, path: String, root: Int): String
    external fun nativeScalaInfo(path: String): String
    external fun nativeInputPeakPeek(): Float

    external fun nativeSetMasterStageJson(json: String): Boolean
    external fun nativeGetMasterStageJson(): String
    external fun nativeSetMasterStageBypass(bypass: Boolean)
    external fun nativeSetMasterEqBand(band: Int, freq: Float, gainDb: Float, q: Float, enabled: Boolean)
    external fun nativeSetMasterComp(
        on: Boolean, threshDb: Float, ratio: Float, attackMs: Float, releaseMs: Float, makeupDb: Float
    )
    external fun nativeMasterCompGrDb(): Float
    /** Setup file (engine setup.h): state + MIDI learn + [hostJson] app settings; [names] = JSON array of part names. */
    external fun nativeSetPerfOffsets(transpose: Int, trimDb: Float)
    external fun nativeGetPerfTranspose(): Int
    external fun nativeGetPerfTrimDb(): Float

    external fun nativeSetMetronomeJson(json: String): Boolean
    external fun nativeGetMetronomeJson(): String
    external fun nativeSetMetronomeEnabled(on: Boolean)
    external fun nativeClickBeat(): Int // -1 stopped, zero-based beat otherwise

    external fun nativeTunerEnable(on: Boolean)
    external fun nativeTunerConfig(a4Hz: Float, muteOut: Boolean)
    external fun nativeTunerRead(out: FloatArray): Boolean // Hz, cents, fractional MIDI note, confidence

    external fun nativeScopeEnable(on: Boolean)
    external fun nativeScopeFrame(wave: FloatArray, spec: FloatArray): Float // fundamental in Hz, 0 if unknown

    external fun nativeSetPedalConfigJson(json: String): Boolean
    external fun nativeGetPedalConfigJson(): String
    external fun nativeSetFootswitch(slot: Int, cc: Int, action: Int, invert: Boolean) // slot 0..3
    external fun nativeTakeHostAction(): Int // 0 when empty
    external fun nativeLastCc(): Int // cc | (channel << 8) | (sequence << 16)
    external fun nativePedalState(): Int // bit 0 sustain, bits 1..4 switches

    external fun nativeSetupJson(name: String, libraryRoot: String, hostJson: String, names: String): String
    /** Applies a setup file's text; returns {ok, error, name, state, multi, midiLearn, parts[], host{}, warnings[]}. */
    external fun nativeSetupApply(text: String, libraryRoot: String): String
    external fun nativeLearnArm(param: Int)
    external fun nativeLearnCancel()
    external fun nativeLearnArmed(): Int
    external fun nativeLearnLast(): Int
    external fun nativeLearnRemove(param: Int)
    external fun nativeLearnDescribe(param: Int): String
    external fun nativeLearnSave(path: String): String
    external fun nativeLearnLoad(path: String): String

    fun noteOn(id: Int, key: Int, velocity: Float) = nativeNoteOn(id, key, velocity)
    fun noteOff(id: Int, key: Int) = nativeNoteOff(id, key, 0.5f)
    fun perNote(type: Int, id: Int, key: Int, value: Float) = nativePerNote(type, id, key, value)
    fun control(type: Int, value: Float, controller: Int = 0) = nativeControl(type, value, controller)
    fun setParam(id: Int, value: Float) = nativeSetParam(id, value)
}
