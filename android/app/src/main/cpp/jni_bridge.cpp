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
#include "hsynth/gm_bank.h"
#include "hsynth/preset_index.h"
#include "hsynth/smf.h"
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
    gEngine->setRenderThreads(0);  // auto: voices spread over about half the cores
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

void songPartForgotten(int part);

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
    songPartForgotten(0);  // part 1 no longer holds the song's sound
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

// Slowest block since the last peak reset, per phase (see Engine::peakProfile).
JNI(jfloatArray, nativePeakProfile)(JNIEnv* env, jobject) {
    float v[Engine::kProfPhases];
    gEngine->peakProfile(v);
    jfloatArray a = env->NewFloatArray(Engine::kProfPhases);
    env->SetFloatArrayRegion(a, 0, Engine::kProfPhases, v);
    return a;
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

// ------------------------------------------------------------ songs (MIDI files)

namespace {
std::mutex gSongMutex;
std::string gSongLoaded[kNumParts];  // preset file per part (skips reloading between songs)
}  // namespace

void songPartForgotten(int part) {
    std::lock_guard<std::mutex> lock(gSongMutex);
    if (part >= 0 && part < kNumParts) gSongLoaded[part].clear();
}

// Loads a Standard MIDI File, picks GM sounds from the preset folders
// (";"-separated) and loads them into the parts. Blocking: call from the
// loader thread. Returns JSON {ok, name, length, report[] | error}.
JNI(jstring, nativeSongLoad)(JNIEnv* env, jobject, jstring path, jstring libRoot, jstring presetDirs) {
    std::lock_guard<std::mutex> lock(gSongMutex);
    Json r = Json::object();
    auto song = std::make_unique<Song>();
    std::string err;
    if (!loadSmf(str(env, path), *song, err)) {
        r.set("ok", false);
        r.set("error", err);
        return jstr(env, r.dump(0));
    }
    std::vector<std::pair<std::string, bool>> folders;
    const std::string dirs = str(env, presetDirs);
    for (size_t a = 0; a <= dirs.size();) {
        size_t b = dirs.find(';', a);
        if (b == std::string::npos) b = dirs.size();
        if (b > a) folders.push_back({dirs.substr(a, b - a), false});
        a = b + 1;
    }
    PresetIndex idx;
    idx.setFolders(folders);
    idx.rescan();
    GmBank bank;
    bank.build(idx.all());
    gEngine->songPlay(false);
    std::vector<std::string> report;
    std::vector<SongPart> parts;
    prepareSong(*gEngine, *song, bank, str(env, libRoot), gSongLoaded, report, &parts);
    gMidi.setMultiTimbral(true);
    r.set("ok", true);
    r.set("name", song->name);
    r.set("length", song->length);
    r.set("tempo", song->initialTempo);
    r.set("report", warningsJson(report));
    Json ch = Json::array();
    for (const SongPart& p : parts) {
        Json o = Json::object();
        o.set("ch", p.channel);
        o.set("drum", p.drum);
        o.set("gm", p.gmName);
        o.set("preset", p.presetName);
        ch.push(o);
    }
    r.set("channels", ch);
    Json tempos = Json::array();
    for (const auto& t : song->tempos) {
        Json o = Json::array();
        o.push(t.time);
        o.push(t.bpm);
        tempos.push(o);
    }
    r.set("tempos", tempos);
    gEngine->setSong(std::move(song));
    return jstr(env, r.dump(0));
}

JNI(void, nativeSongPlay)(JNIEnv*, jobject, jboolean on) { gEngine->songPlay(on); }
JNI(void, nativeSongSeek)(JNIEnv*, jobject, jdouble seconds) { gEngine->songSeek(seconds); }
// {position s, length s, playing 0/1}
JNI(jdoubleArray, nativeSongState)(JNIEnv* env, jobject) {
    const double v[3] = {gEngine->songPosition(), gEngine->songLength(), gEngine->songPlaying() ? 1.0 : 0.0};
    jdoubleArray a = env->NewDoubleArray(3);
    env->SetDoubleArrayRegion(a, 0, 3, v);
    return a;
}

// Player screen state: {pos, len, playing, endCount, masterVolume} then per
// part (16 x 12): level, cc volume, expression, pan, bend, mod, reverb,
// chorus, voices, mix volume, mute, mix pan.
JNI(jfloatArray, nativePlayerState)(JNIEnv* env, jobject) {
    static float v[5 + kNumParts * 12];
    Engine::PartMeter m[kNumParts];
    gEngine->partMeters(m);
    v[0] = (float)gEngine->songPosition();
    v[1] = (float)gEngine->songLength();
    v[2] = gEngine->songPlaying() ? 1.0f : 0.0f;
    v[3] = (float)gEngine->songEndCount();
    v[4] = gEngine->getParam(P::MasterVolume);
    for (int k = 0; k < kNumParts; ++k) {
        float* o = v + 5 + k * 12;
        o[0] = m[k].level;
        o[1] = m[k].volume;
        o[2] = m[k].expression;
        o[3] = m[k].pan;
        o[4] = m[k].bend;
        o[5] = m[k].mod;
        o[6] = m[k].reverb;
        o[7] = m[k].chorus;
        o[8] = (float)m[k].voices;
        o[9] = gEngine->getPartMix(k, Mix::Volume);
        o[10] = gEngine->getPartMix(k, Mix::Mute);
        o[11] = gEngine->getPartMix(k, Mix::Pan);
    }
    jfloatArray a = env->NewFloatArray(5 + kNumParts * 12);
    env->SetFloatArrayRegion(a, 0, 5 + kNumParts * 12, v);
    return a;
}

// Keys sounding per part: 16 x 4 words (bit k of word k/32).
JNI(jintArray, nativeNoteMasks)(JNIEnv* env, jobject) {
    Engine::PartMeter m[kNumParts];
    gEngine->partMeters(m);  // (the level peaks are read by nativePlayerState first)
    jint v[kNumParts * 4];
    for (int k = 0; k < kNumParts; ++k)
        for (int w = 0; w < 4; ++w) v[k * 4 + w] = (jint)m[k].notes[w];
    jintArray a = env->NewIntArray(kNumParts * 4);
    env->SetIntArrayRegion(a, 0, kNumParts * 4, v);
    return a;
}

JNI(void, nativeSetPartMix)(JNIEnv*, jobject, jint part, jint id, jfloat value) { gEngine->setPartMix(part, id, value); }
