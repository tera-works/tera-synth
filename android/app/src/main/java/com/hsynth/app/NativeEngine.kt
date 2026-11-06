package com.hsynth.app

/**
 * Thin Kotlin facade over the C++ engine (libhsynth_jni). All methods are
 * safe to call from the UI thread: they never wait for the audio thread.
 * File-loading methods block the caller and must run on a background thread.
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
    external fun nativePlayerState(): FloatArray
    external fun nativeNoteMasks(): IntArray
    external fun nativeSetPartMix(part: Int, id: Int, value: Float)
    external fun nativeStart(deviceId: Int, exclusive: Boolean, bufferBursts: Int): Boolean
    external fun nativeStop()

    external fun nativeNoteOn(id: Int, key: Int, velocity: Float)
    external fun nativeNoteOff(id: Int, key: Int, velocity: Float)
    external fun nativePerNote(type: Int, id: Int, key: Int, value: Float)
    external fun nativeControl(type: Int, value: Float, controller: Int)
    external fun nativeMidi(data: ByteArray, offset: Int, count: Int, ump: Boolean)
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

    fun noteOn(id: Int, key: Int, velocity: Float) = nativeNoteOn(id, key, velocity)
    fun noteOff(id: Int, key: Int) = nativeNoteOff(id, key, 0.5f)
    fun perNote(type: Int, id: Int, key: Int, value: Float) = nativePerNote(type, id, key, value)
    fun control(type: Int, value: Float, controller: Int = 0) = nativeControl(type, value, controller)
    fun setParam(id: Int, value: Float) = nativeSetParam(id, value)
}
