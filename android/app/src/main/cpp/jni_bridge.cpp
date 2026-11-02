// JNI bridge between the Kotlin UI and the HSynth engine.
// All calls here run on UI / loader / MIDI threads; none of them block the
// audio callback (engine API is lock-free towards the audio thread).
#include <android/log.h>
#include <jni.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio_engine.h"
#include "hsynth/engine.h"
#include "hsynth/fx/ir_load.h"
#include "hsynth/host_utils.h"
#include "hsynth/json.h"
#include "hsynth/midi.h"
#include "hsynth/preset.h"
#include "hsynth/sample_set.h"

using namespace hsynth;

namespace {

std::unique_ptr<Engine> gEngine;
std::unique_ptr<AudioEngine> gAudio;
MidiTranslator gMidi;
std::mutex gMidiMutex;
std::atomic<int> gPendingProgram{-1};
std::atomic<int> gMidiCount{0};

struct EngineSink : EventSink {
    void onEvent(const Event& e) override {
        gEngine->sendEvent(e);
        gMidiCount.fetch_add(1);
    }
    void onProgramChange(int, int program) override { gPendingProgram.store(program); }
};

std::string str(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r(c);
    env->ReleaseStringUTFChars(s, c);
    return r;
}

jstring jstr(JNIEnv* env, const std::string& s) { return env->NewStringUTF(s.c_str()); }

Json warningsJson(const std::vector<std::string>& w) {
    Json a = Json::array();
    for (auto& s : w) a.push(s);
    return a;
}

}  // namespace

#define JNI(ret, name) extern "C" JNIEXPORT ret JNICALL Java_com_hsynth_app_NativeEngine_##name

JNI(void, nativeCreate)(JNIEnv*, jobject) {
    if (gEngine) return;
    gEngine = std::make_unique<Engine>();
    gEngine->prepare(48000.0f);
    gAudio = std::make_unique<AudioEngine>(*gEngine);
}

JNI(jboolean, nativeStart)(JNIEnv*, jobject, jint deviceId, jboolean exclusive, jint bufferBursts) {
    if (!gAudio) return JNI_FALSE;
    return gAudio->start(deviceId, exclusive, bufferBursts, 48000) ? JNI_TRUE : JNI_FALSE;
}

JNI(void, nativeStop)(JNIEnv*, jobject) {
    if (gAudio) gAudio->stop();
}

JNI(void, nativeNoteOn)(JNIEnv*, jobject, jint id, jint key, jfloat vel) {
    gEngine->sendEvent(Event::noteOn((uint32_t)id, key, vel));
}

JNI(void, nativeNoteOff)(JNIEnv*, jobject, jint id, jint key, jfloat vel) {
    gEngine->sendEvent(Event::noteOff((uint32_t)id, key, vel));
}

// type: 0 pitch(semitones) 1 pressure 2 timbre 3 x 4 hold
JNI(void, nativePerNote)(JNIEnv*, jobject, jint type, jint id, jint key, jfloat value) {
    static const EventType map[] = {EventType::NotePitch, EventType::NotePressure, EventType::NoteTimbre,
                                    EventType::NoteX, EventType::NoteHold};
    if (type < 0 || type > 4) return;
    gEngine->sendEvent(Event::perNote(map[type], (uint32_t)id, key, value));
}

// type: 0 pitchbend 1 modwheel 2 chanpressure 3 sustain 4 cc 5 all notes off 6 all sound off
JNI(void, nativeControl)(JNIEnv*, jobject, jint type, jfloat value, jint ctrl) {
    static const EventType map[] = {EventType::PitchBend, EventType::ModWheel, EventType::ChannelPressure,
                                    EventType::Sustain, EventType::Controller, EventType::AllNotesOff,
                                    EventType::AllSoundOff};
    if (type < 0 || type > 6) return;
    gEngine->sendEvent(Event::control(map[type], value, ctrl));
}

JNI(void, nativeMidi)(JNIEnv* env, jobject, jbyteArray data, jint offset, jint count, jboolean ump) {
    if (count <= 0) return;
    std::vector<uint8_t> buf((size_t)count);
    env->GetByteArrayRegion(data, offset, count, reinterpret_cast<jbyte*>(buf.data()));
    std::lock_guard<std::mutex> lock(gMidiMutex);
    EngineSink sink;
    if (ump) gMidi.processUmpBytes(buf.data(), buf.size(), sink);
    else gMidi.processMidi1(buf.data(), buf.size(), sink);
}

JNI(void, nativeSetMpe)(JNIEnv*, jobject, jboolean on) {
    std::lock_guard<std::mutex> lock(gMidiMutex);
    gMidi.setMpeEnabled(on);
}

JNI(jint, nativeTakeProgramChange)(JNIEnv*, jobject) { return gPendingProgram.exchange(-1); }
JNI(jint, nativeMidiEventCount)(JNIEnv*, jobject) { return gMidiCount.load(); }

JNI(void, nativeSetParam)(JNIEnv*, jobject, jint id, jfloat v) { gEngine->setParam(id, v); }
JNI(jfloat, nativeGetParam)(JNIEnv*, jobject, jint id) { return gEngine->getParam(id); }

JNI(jfloatArray, nativeGetAllParams)(JNIEnv* env, jobject) {
    std::vector<float> v((size_t)P::kNumParams);
    gEngine->getAllParams(v.data());
    jfloatArray a = env->NewFloatArray(P::kNumParams);
    env->SetFloatArrayRegion(a, 0, P::kNumParams, v.data());
    return a;
}

JNI(jstring, nativeParamMetadata)(JNIEnv* env, jobject) { return jstr(env, paramMetadataJson()); }

JNI(jstring, nativeLoadPreset)(JNIEnv* env, jobject, jstring path, jstring libRoot) {
    Json r = Json::object();
    Preset p;
    std::string err;
    if (!loadPresetFile(str(env, path), p, err)) {
        r.set("ok", false);
        r.set("error", err);
        return jstr(env, r.dump(0));
    }
    std::vector<std::string> warnings;
    applyPreset(*gEngine, p, str(env, libRoot), warnings);
    r.set("ok", true);
    r.set("name", p.name);
    r.set("category", p.category);
    r.set("comment", p.comment);
    r.set("warnings", warningsJson(warnings));
    return jstr(env, r.dump(0));
}

JNI(jstring, nativeSavePreset)(JNIEnv* env, jobject, jstring path, jstring name, jstring category, jstring libRoot) {
    Preset p = capturePreset(*gEngine, str(env, name), str(env, libRoot));
    p.category = str(env, category);
    p.author = "user";
    std::string err;
    if (!savePresetFile(str(env, path), p, err)) return jstr(env, err);
    return jstr(env, "");
}

JNI(jstring, nativePresetJson)(JNIEnv* env, jobject, jstring name, jstring libRoot) {
    Preset p = capturePreset(*gEngine, str(env, name), str(env, libRoot));
    return jstr(env, presetToJson(p));
}

JNI(jstring, nativeLoadSamples)(JNIEnv* env, jobject, jint layer, jstring path) {
    Json r = Json::object();
    std::string err;
    std::vector<std::string> warnings;
    const bool ok = loadLayerSamples(*gEngine, layer, str(env, path), err, warnings);
    r.set("ok", ok);
    if (!ok) r.set("error", err);
    r.set("warnings", warningsJson(warnings));
    return jstr(env, r.dump(0));
}

// Blocking (WAV decode + FFT): call from the loader thread.
JNI(jstring, nativeLoadWavetable)(JNIEnv* env, jobject, jint layer, jstring path) {
    Json r = Json::object();
    std::string err;
    const bool ok = loadLayerWavetable(*gEngine, layer, str(env, path), err);
    r.set("ok", ok);
    if (!ok) r.set("error", err);
    return jstr(env, r.dump(0));
}

JNI(void, nativeUnloadSamples)(JNIEnv*, jobject, jint layer) {
    gEngine->setSampleSet(layer, nullptr);
    gEngine->collectGarbage();
}

JNI(jstring, nativeSampleSetName)(JNIEnv* env, jobject, jint layer) { return jstr(env, gEngine->sampleSetName(layer)); }

// Inspect an SFZ/WAV without loading it into the engine (library screen).
JNI(jstring, nativeInspectSamples)(JNIEnv* env, jobject, jstring path) {
    Json r = Json::object();
    std::string err;
    auto set = loadSampleSet(str(env, path), err);
    r.set("ok", set != nullptr);
    if (!set) {
        r.set("error", err);
    } else {
        r.set("regions", (int)set->regions.size());
        r.set("samples", (int)set->samples.size());
        r.set("bytes", (double)set->memoryBytes());
        r.set("warnings", warningsJson(set->warnings));
    }
    return jstr(env, r.dump(0));
}

JNI(jstring, nativeSupportedSfzOpcodes)(JNIEnv* env, jobject) {
    std::string s;
    for (auto& o : supportedSfzOpcodes()) s += (s.empty() ? "" : ", ") + o;
    return jstr(env, s);
}

JNI(void, nativeSetMaxVoices)(JNIEnv*, jobject, jint n) { gEngine->setMaxVoices(n); }
JNI(void, nativeSetQuality)(JNIEnv*, jobject, jint q) { gEngine->setQuality((Quality)q); }

// [0] activeVoices [1] peakVoices [2] engineLoad [3] engineLoadPeak [4] renderUsAvg [5] renderUsPeak
// [6] droppedEvents [7] outputPeak [8] running [9] sampleRate [10] framesPerBurst [11] bufferSize
// [12] bufferCapacity [13] xruns [14] api [15] exclusive [16] lowLatency [17] deviceId
// [18] callbackFrames [19] cbUsAvg [20] cbUsMax [21] cbLoadMax [22] latencyMs [23] restarts [24] maxVoices
JNI(jfloatArray, nativeGetStats)(JNIEnv* env, jobject) {
    const EngineStats e = gEngine->stats();
    const AudioStats a = gAudio ? gAudio->stats() : AudioStats{};
    const float v[] = {(float)e.activeVoices, (float)e.peakVoices, e.cpuLoad, e.cpuLoadPeak, e.renderUsAvg,
                       e.renderUsPeak, (float)e.droppedEvents, e.outputPeak, a.running ? 1.0f : 0.0f,
                       (float)a.sampleRate, (float)a.framesPerBurst, (float)a.bufferSize, (float)a.bufferCapacity,
                       (float)a.xruns, (float)a.api, a.exclusive ? 1.0f : 0.0f, a.lowLatency ? 1.0f : 0.0f,
                       (float)a.deviceId, (float)a.lastCallbackFrames, a.callbackUsAvg, a.callbackUsMax,
                       a.callbackLoadMax, (float)a.latencyMs, (float)a.restarts, (float)gEngine->maxVoices()};
    const int n = (int)(sizeof v / sizeof v[0]);
    jfloatArray arr = env->NewFloatArray(n);
    env->SetFloatArrayRegion(arr, 0, n, v);
    return arr;
}

JNI(void, nativeResetPeaks)(JNIEnv*, jobject) {
    gEngine->resetPeakStats();
    if (gAudio) gAudio->resetPeaks();
}

JNI(void, nativeCollectGarbage)(JNIEnv*, jobject) { gEngine->collectGarbage(); }

// ---- effect rack -----------------------------------------------------------

// Blocking (WAV decode + FFT): call from the loader thread.
JNI(jstring, nativeLoadImpulse)(JNIEnv* env, jobject, jint target, jstring path) {
    Json r = Json::object();
    std::string err;
    auto ir = fx::loadImpulseResponse(str(env, path), gEngine->sampleRate(), target, err);
    r.set("ok", ir != nullptr);
    if (!ir) r.set("error", err);
    else {
        r.set("length", ir->length);
        gEngine->setFxImpulse(target, std::move(ir));
    }
    return jstr(env, r.dump(0));
}

JNI(void, nativeUnloadImpulse)(JNIEnv*, jobject, jint target) { gEngine->setFxImpulse(target, nullptr); }
JNI(jstring, nativeImpulseName)(JNIEnv* env, jobject, jint target) { return jstr(env, gEngine->fxImpulseName(target)); }
JNI(void, nativeLooperCommand)(JNIEnv*, jobject, jint cmd) { gEngine->looperCommand((fx::LooperCmd)cmd); }

// [state, length s, position s, canUndo]
JNI(jfloatArray, nativeLooperStatus)(JNIEnv* env, jobject) {
    const auto s = gEngine->looperStatus();
    const float v[4] = {(float)s.state, s.lengthSec, s.positionSec, s.canUndo ? 1.0f : 0.0f};
    jfloatArray a = env->NewFloatArray(4);
    env->SetFloatArrayRegion(a, 0, 4, v);
    return a;
}

// Blocks currently processed, in order.
JNI(jintArray, nativeFxChain)(JNIEnv* env, jobject) {
    int chain[fx::kFxSlots];
    const int n = gEngine->fxChain(chain);
    jintArray a = env->NewIntArray(n);
    env->SetIntArrayRegion(a, 0, n, chain);
    return a;
}
