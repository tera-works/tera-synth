// JNI bridge between the Kotlin UI and the HSynth engine.
// All calls here run on UI / loader / MIDI threads; none of them block the
// audio callback (engine API is lock-free towards the audio thread).
#include <android/log.h>
#include <jni.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio_engine.h"
#include "hsynth/ble_midi.h"
#include "hsynth/dsp/fm.h"
#include "hsynth/engine.h"
#include "hsynth/fx/ir_load.h"
#include "hsynth/gm_bank.h"
#include "hsynth/preset_index.h"
#include "hsynth/smf.h"
#include "hsynth/host_utils.h"
#include "hsynth/json.h"
#include "hsynth/midi_learn.h"
#include "hsynth/midi.h"
#include "hsynth/preset.h"
#include "hsynth/remote.h"
#include "hsynth/setup.h"
#include "hsynth/sample_set.h"

using namespace hsynth;

namespace {

std::unique_ptr<Engine> gEngine;
std::unique_ptr<AudioEngine> gAudio;
MidiTranslator gMidi;
MidiLearn gLearn;
std::atomic<int> gEditPart{0};  // part the editor works on (0 in single mode)
std::mutex gMidiMutex;
std::atomic<int> gPendingProgram{-1};
std::atomic<int> gMidiCount{0};

// SysEx remote control (docs/SYSEX.md): SysEx is taken out of the MIDI streams per
// source device, requests go to gRemote, replies wait in gRemoteOut for the
// Kotlin sender thread (nativeRemoteNext), which writes them to the device the
// last request came from (gRemoteSource).
std::unique_ptr<RemoteControl> gRemote;
std::mutex gRemoteOutMutex;
std::condition_variable gRemoteOutCv;
std::deque<std::vector<uint8_t>> gRemoteOut;
std::atomic<int> gRemoteSource{-1};
struct SysExState {
    std::vector<uint8_t> buf;
    bool in = false;
};
std::map<int, SysExState> gSysEx;  // per source device (under gMidiMutex)

void sysexDone(int source, const std::vector<uint8_t>& m) {
    if (!gRemote) return;
    sysex::Frame f;
    if (!sysex::parseFrame(m.data(), m.size(), f) || f.type != sysex::Request) return;
    gRemoteSource.store(source);
    gRemote->onSysEx(m.data(), m.size());
}

// MIDI 1.0 bytes: SysEx (possibly split over several calls) is removed and assembled;
// everything else (realtime bytes inside SysEx too) is returned for the translator.
std::vector<uint8_t> takeSysEx(int source, const uint8_t* d, size_t n) {
    SysExState& st = gSysEx[source];
    std::vector<uint8_t> rest;
    rest.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = d[i];
        if (c >= 0xF8) { rest.push_back(c); continue; }  // realtime: anywhere
        if (c == 0xF0) { st.in = true; st.buf.assign(1, c); continue; }
        if (!st.in) { rest.push_back(c); continue; }
        if (c == 0xF7) {
            st.buf.push_back(c);
            st.in = false;
            sysexDone(source, st.buf);
            st.buf.clear();
        } else if (c & 0x80) {  // a status byte ends an unterminated SysEx
            st.in = false;
            st.buf.clear();
            rest.push_back(c);
        } else {
            st.buf.push_back(c);
            if (st.buf.size() > (1u << 20)) { st.in = false; st.buf.clear(); }
        }
    }
    return rest;
}

// UMP SysEx7 packets (message type 3)
void umpSysEx(int source, const uint32_t* w, size_t count) {
    SysExState& st = gSysEx[source];
    for (size_t i = 0; i < count;) {
        const int len = MidiTranslator::umpWordCount(w[i]);
        if ((w[i] >> 28) == 0x3 && i + 1 < count) {
            const int status = (int)((w[i] >> 20) & 0xF), nb = std::min(6, (int)((w[i] >> 16) & 0xF));
            const uint8_t d[6] = {(uint8_t)(w[i] >> 8), (uint8_t)w[i], (uint8_t)(w[i + 1] >> 24),
                                  (uint8_t)(w[i + 1] >> 16), (uint8_t)(w[i + 1] >> 8), (uint8_t)w[i + 1]};
            if (status == 0 || status == 1) { st.buf.assign(1, 0xF0); st.in = true; }
            for (int k = 0; k < nb && st.in; ++k) st.buf.push_back(d[k] & 0x7F);
            if ((status == 0 || status == 3) && st.in) {
                st.buf.push_back(0xF7);
                st.in = false;
                sysexDone(source, st.buf);
                st.buf.clear();
            }
        }
        i += (size_t)std::max(1, len);
    }
}

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

// The on-screen keyboard plays on the edit part's MIDI channel in multi mode,
// so every part on that channel sounds (layers) within its key range (splits).
// In single mode its events stay omni.
static Event keyboardEvent(Event e) {
    if (gEngine->multiTimbral()) e.channel = (uint8_t)std::max(0, (int)gEngine->getPartMix(gEditPart.load(), Mix::Channel) - 1);
    return e;
}

JNI(void, nativeNoteOn)(JNIEnv*, jobject, jint id, jint key, jfloat vel) {
    gEngine->sendEvent(keyboardEvent(Event::noteOn((uint32_t)id, key, vel)));
}

JNI(void, nativeNoteOff)(JNIEnv*, jobject, jint id, jint key, jfloat vel) {
    gEngine->sendEvent(keyboardEvent(Event::noteOff((uint32_t)id, key, vel)));
}

// type: 0 pitch(semitones) 1 pressure 2 timbre 3 x 4 hold
JNI(void, nativePerNote)(JNIEnv*, jobject, jint type, jint id, jint key, jfloat value) {
    static const EventType map[] = {EventType::NotePitch, EventType::NotePressure, EventType::NoteTimbre,
                                    EventType::NoteX, EventType::NoteHold};
    if (type < 0 || type > 4) return;
    gEngine->sendEvent(keyboardEvent(Event::perNote(map[type], (uint32_t)id, key, value)));
}

// type: 0 pitchbend 1 modwheel 2 chanpressure 3 sustain 4 cc 5 all notes off 6 all sound off
JNI(void, nativeControl)(JNIEnv*, jobject, jint type, jfloat value, jint ctrl) {
    static const EventType map[] = {EventType::PitchBend, EventType::ModWheel, EventType::ChannelPressure,
                                    EventType::Sustain, EventType::Controller, EventType::AllNotesOff,
                                    EventType::AllSoundOff};
    if (type < 0 || type > 6) return;
    const Event e = Event::control(map[type], value, ctrl);
    gEngine->sendEvent(type >= 5 ? e : keyboardEvent(e));  // panic stays omni (all parts)
}

namespace {
// MIDI 1.0 bytes from a source (gMidiMutex held)
void midi1(int source, const uint8_t* data, size_t n) {
    EngineSink sink;
    const std::vector<uint8_t> rest = takeSysEx(source, data, n);
    if (rest.empty()) return;
    gLearn.onMidi(rest.data(), rest.size(), *gEngine);  // learned CCs set their parameters
    gMidi.processMidi1(rest.data(), rest.size(), sink);
}
std::map<int, ble::Decoder> gBle;  // BLE MIDI peripheral connections (under gMidiMutex)
}  // namespace

// A BLE MIDI packet written by a central connected to the phone (BleMidi.kt).
JNI(void, nativeBlePacket)(JNIEnv* env, jobject, jbyteArray data, jint source) {
    const jsize n = env->GetArrayLength(data);
    if (n <= 0) return;
    std::vector<uint8_t> buf((size_t)n);
    env->GetByteArrayRegion(data, 0, n, reinterpret_cast<jbyte*>(buf.data()));
    std::lock_guard<std::mutex> lock(gMidiMutex);
    gBle[source].packet(buf.data(), buf.size(), [source](const uint8_t* m, size_t len) { midi1(source, m, len); });
}

JNI(void, nativeBleReset)(JNIEnv*, jobject, jint source) {
    std::lock_guard<std::mutex> lock(gMidiMutex);
    gBle.erase(source);
    gSysEx.erase(source);
}

// One complete MIDI message -> BLE MIDI packets of at most maxPacket bytes.
JNI(jobjectArray, nativeBleEncode)(JNIEnv* env, jobject, jbyteArray data, jint maxPacket, jint timeMs) {
    const jsize n = env->GetArrayLength(data);
    std::vector<uint8_t> buf((size_t)std::max(0, (int)n));
    if (n > 0) env->GetByteArrayRegion(data, 0, n, reinterpret_cast<jbyte*>(buf.data()));
    const auto packets = ble::encode(buf.data(), buf.size(), (size_t)std::max(5, (int)maxPacket), (uint32_t)timeMs);
    jobjectArray a = env->NewObjectArray((jsize)packets.size(), env->FindClass("[B"), nullptr);
    for (size_t i = 0; i < packets.size(); ++i) {
        jbyteArray p = env->NewByteArray((jsize)packets[i].size());
        env->SetByteArrayRegion(p, 0, (jsize)packets[i].size(), reinterpret_cast<const jbyte*>(packets[i].data()));
        env->SetObjectArrayElement(a, (jsize)i, p);
        env->DeleteLocalRef(p);
    }
    return a;
}

JNI(void, nativeMidi)(JNIEnv* env, jobject, jbyteArray data, jint offset, jint count, jboolean ump, jint source) {
    if (count <= 0) return;
    std::vector<uint8_t> buf((size_t)count);
    env->GetByteArrayRegion(data, offset, count, reinterpret_cast<jbyte*>(buf.data()));
    std::lock_guard<std::mutex> lock(gMidiMutex);
    EngineSink sink;
    if (ump) {
        // big-endian words -> host order for MIDI learn
        std::vector<uint32_t> words(buf.size() / 4);
        for (size_t i = 0; i < words.size(); ++i)
            words[i] = (uint32_t)buf[i * 4] << 24 | (uint32_t)buf[i * 4 + 1] << 16 | (uint32_t)buf[i * 4 + 2] << 8 | buf[i * 4 + 3];
        umpSysEx(source, words.data(), words.size());
        gLearn.onUmp(words.data(), words.size(), *gEngine);
        gMidi.processUmpBytes(buf.data(), buf.size(), sink);
    } else {
        midi1(source, buf.data(), buf.size());
    }
}

JNI(void, nativeSetMpe)(JNIEnv*, jobject, jboolean on) {
    std::lock_guard<std::mutex> lock(gMidiMutex);
    gMidi.setMpeEnabled(on);
}

JNI(jint, nativeTakeProgramChange)(JNIEnv*, jobject) { return gPendingProgram.exchange(-1); }
JNI(jint, nativeMidiEventCount)(JNIEnv*, jobject) { return gMidiCount.load(); }

// Parameters of the edit part (part parameters) or global ones.
JNI(void, nativeSetParam)(JNIEnv*, jobject, jint id, jfloat v) { gEngine->setPartParam(gEditPart.load(), id, v); }
JNI(jfloat, nativeGetParam)(JNIEnv*, jobject, jint id) { return gEngine->getPartParam(gEditPart.load(), id); }

JNI(void, nativeSetAllParams)(JNIEnv* env, jobject, jfloatArray values) {
    if (env->GetArrayLength(values) != P::kNumParams) return;
    std::vector<float> v((size_t)P::kNumParams);
    env->GetFloatArrayRegion(values, 0, P::kNumParams, v.data());
    const int part = gEditPart.load();
    for (int i = 0; i < P::kNumParams; ++i) gEngine->setPartParam(part, i, v[(size_t)i]);
}

JNI(jfloatArray, nativeGetAllParams)(JNIEnv* env, jobject) {
    std::vector<float> v((size_t)P::kNumParams);
    gEngine->getPartParams(gEditPart.load(), v.data());
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
    const int part = gEditPart.load();
    if (part == 0 && !gEngine->multiTimbral()) applyPreset(*gEngine, p, str(env, libRoot), warnings);
    else applyPartPreset(*gEngine, part, p, str(env, libRoot), warnings);
    songPartForgotten(part);  // the part no longer holds the song's sound
    r.set("ok", true);
    r.set("name", p.name);
    r.set("category", p.category);
    r.set("comment", p.comment);
    r.set("warnings", warningsJson(warnings));
    return jstr(env, r.dump(0));
}

JNI(jstring, nativeSavePreset)(JNIEnv* env, jobject, jstring path, jstring name, jstring category, jstring libRoot) {
    Preset p = capturePartPreset(*gEngine, gEditPart.load(), str(env, name), str(env, libRoot));
    p.category = str(env, category);
    p.author = "user";
    std::string err;
    if (!savePresetFile(str(env, path), p, err)) return jstr(env, err);
    return jstr(env, "");
}

JNI(jstring, nativePresetJson)(JNIEnv* env, jobject, jstring name, jstring libRoot) {
    Preset p = capturePartPreset(*gEngine, gEditPart.load(), str(env, name), str(env, libRoot));
    return jstr(env, presetToJson(p));
}

JNI(jstring, nativeLoadSamples)(JNIEnv* env, jobject, jint layer, jstring path) {
    Json r = Json::object();
    std::string err;
    std::vector<std::string> warnings;
    const bool ok = loadPartSamples(*gEngine, gEditPart.load(), layer, str(env, path), err, warnings);
    r.set("ok", ok);
    if (!ok) r.set("error", err);
    r.set("warnings", warningsJson(warnings));
    return jstr(env, r.dump(0));
}

// Blocking (WAV decode + FFT): call from the loader thread.
JNI(jstring, nativeLoadWavetable)(JNIEnv* env, jobject, jint layer, jstring path) {
    Json r = Json::object();
    std::string err;
    const bool ok = loadPartWavetable(*gEngine, gEditPart.load(), layer, str(env, path), err);
    r.set("ok", ok);
    if (!ok) r.set("error", err);
    return jstr(env, r.dump(0));
}

JNI(void, nativeUnloadSamples)(JNIEnv*, jobject, jint layer) {
    gEngine->setPartSampleSet(gEditPart.load(), layer, nullptr);
    gEngine->collectGarbage();
}

JNI(jstring, nativeSampleSetName)(JNIEnv* env, jobject, jint layer) { return jstr(env, gEngine->partSampleSetName(gEditPart.load(), layer)); }

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
SongProgramLoader gPrograms;          // follows mid-song program changes of the loaded song
}  // namespace

void songPartForgotten(int part) {
    std::lock_guard<std::mutex> lock(gSongMutex);
    if (part >= 0 && part < kNumParts) gSongLoaded[part].clear();
}

// Loads a Standard MIDI File, picks GM sounds from the preset folders
// (";"-separated) and loads them into the parts. Blocking: call from the
// loader thread. Returns JSON {ok, name, length, report[] | error}.
Json songLoad(const std::string& path, const std::string& libRoot, const std::string& dirs);

JNI(jstring, nativeSongLoad)(JNIEnv* env, jobject, jstring path, jstring libRoot, jstring presetDirs) {
    return jstr(env, songLoad(str(env, path), str(env, libRoot), str(env, presetDirs)).dump(0));
}

std::atomic<uint32_t> gRemoteSongs{0};  // songs loaded remotely (the player shows them)
std::string gRemoteSongPath;

Json songLoad(const std::string& path, const std::string& libRootS, const std::string& dirs) {
    std::lock_guard<std::mutex> lock(gSongMutex);
    gPrograms.stop();  // before prepareSong: it must not stage into the parts being loaded
    Json r = Json::object();
    auto song = std::make_unique<Song>();
    std::string err;
    if (!loadSmf(path, *song, err)) {
        r.set("ok", false);
        r.set("error", err);
        return r;
    }
    std::vector<std::pair<std::string, bool>> folders;
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
    prepareSong(*gEngine, *song, bank, libRootS, gSongLoaded, report, &parts);
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
    gPrograms.start(*gEngine, *song, bank, libRootS);  // mid-song program changes
    gEngine->setSong(std::move(song));
    return r;
}

// Preset playing on each channel after mid-song program changes ("" = as loaded).
JNI(jobjectArray, nativeSongPartPresets)(JNIEnv* env, jobject) {
    jobjectArray a = env->NewObjectArray(16, env->FindClass("java/lang/String"), nullptr);
    for (int c = 0; c < 16; ++c) env->SetObjectArrayElement(a, c, jstr(env, gPrograms.presetName(c)));
    return a;
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

// FM algorithm for the diagram: {numOps, carriers mask, feedback op, modulator mask of op 0..7}.
JNI(jintArray, nativeFmAlgorithm)(JNIEnv* env, jobject, jint index) {
    const FmAlgorithm& a = fmAlgorithm(std::max(0, std::min(kNumFmAlgorithms - 1, (int)index)));
    jint v[3 + P::kFmOps];
    v[0] = a.numOps;
    v[1] = a.carriers;
    v[2] = a.feedbackOp;
    for (int i = 0; i < P::kFmOps; ++i) v[3 + i] = a.modulators[i];
    jintArray r = env->NewIntArray(3 + P::kFmOps);
    env->SetIntArrayRegion(r, 0, 3 + P::kFmOps, v);
    return r;
}

// ------------------------------------------------------------ multi / parts / scenes

JNI(void, nativeSetMulti)(JNIEnv*, jobject, jboolean on) {
    gEngine->setMultiTimbral(on);
    std::lock_guard<std::mutex> lock(gMidiMutex);
    gMidi.setMultiTimbral(on);
    if (!on) gEditPart.store(0);
}
JNI(jboolean, nativeIsMulti)(JNIEnv*, jobject) { return gEngine->multiTimbral() ? JNI_TRUE : JNI_FALSE; }
JNI(void, nativeSetEditPart)(JNIEnv*, jobject, jint part) {
    const int k = std::max(0, std::min(kNumParts - 1, (int)part));
    gEditPart.store(k);
    gEngine->selectPart(k);
}
JNI(jint, nativeEditPart)(JNIEnv*, jobject) { return gEditPart.load(); }
JNI(jfloat, nativeGetPartMix)(JNIEnv*, jobject, jint part, jint id) { return gEngine->getPartMix(part, id); }

// Name of the sound of every part ({names[16]}): the parts' layer A sample set or "".
JNI(jstring, nativeSaveMulti)(JNIEnv* env, jobject, jstring path, jstring name, jstring libRoot) {
    MultiPreset m = captureMulti(*gEngine, str(env, name), str(env, libRoot));
    std::string err;
    if (!saveMultiFile(str(env, path), m, err)) return jstr(env, err);
    return jstr(env, "");
}

// Setup files (setup.h): the whole playing setup. hostJson = the app's own settings,
// names = JSON array of the part names shown in the app.
JNI(jstring, nativeSetupJson)(JNIEnv* env, jobject, jstring name, jstring libRoot, jstring hostJson, jstring names) {
    Json host, n;
    std::string err;
    if (!Json::parse(str(env, hostJson), host, err)) host = Json::object();
    std::vector<std::string> partNames;
    if (Json::parse(str(env, names), n, err) && n.isArray())
        for (size_t i = 0; i < n.size(); ++i) partNames.push_back(n.at(i).isString() ? n.at(i).string() : "");
    return jstr(env, makeSetupJson(*gEngine, str(env, name), str(env, libRoot), &gLearn, host, "", partNames));
}

JNI(jstring, nativeSetupApply)(JNIEnv* env, jobject, jstring text, jstring libRoot) {
    Json r = Json::object();
    SetupResult res;
    std::string err;
    if (!applySetupJson(*gEngine, str(env, text), str(env, libRoot), &gLearn, res, err)) {
        r.set("ok", false);
        r.set("error", err);
        return jstr(env, r.dump(0));
    }
    if (res.hasState) {
        {
            std::lock_guard<std::mutex> lock(gMidiMutex);
            gMidi.setMultiTimbral(res.multi);
        }
        for (int k = 0; k < kNumParts; ++k) songPartForgotten(k);
        gEditPart.store(res.multi ? gEngine->selectedPart() : 0);
    }
    r.set("ok", true);
    r.set("name", res.name);
    r.set("state", res.hasState);
    r.set("multi", res.multi);
    r.set("midiLearn", res.hasMidiLearn);
    Json names = Json::array();
    for (const auto& s : res.partNames) names.push(s);
    r.set("parts", names);
    r.set("host", res.host);
    r.set("warnings", warningsJson(res.warnings));
    return jstr(env, r.dump(0));
}

JNI(jstring, nativeLoadMulti)(JNIEnv* env, jobject, jstring path, jstring libRoot) {
    Json r = Json::object();
    MultiPreset m;
    std::string err;
    if (!loadMultiFile(str(env, path), m, err)) {
        r.set("ok", false);
        r.set("error", err);
        return jstr(env, r.dump(0));
    }
    std::vector<std::string> warnings;
    applyMulti(*gEngine, m, str(env, libRoot), warnings);
    {
        std::lock_guard<std::mutex> lock(gMidiMutex);
        gMidi.setMultiTimbral(true);
    }
    for (int k = 0; k < kNumParts; ++k) songPartForgotten(k);
    gEditPart.store(gEngine->selectedPart());
    r.set("ok", true);
    r.set("name", m.name);
    Json names = Json::array();
    for (int k = 0; k < kNumParts; ++k) names.push(m.parts[k].name);
    r.set("parts", names);
    r.set("warnings", warningsJson(warnings));
    return jstr(env, r.dump(0));
}

// ------------------------------------------------------------ MIDI learn

JNI(void, nativeLearnArm)(JNIEnv*, jobject, jint param) { gLearn.arm(param, gEditPart.load()); }
JNI(void, nativeLearnCancel)(JNIEnv*, jobject) { gLearn.cancel(); }
JNI(jint, nativeLearnArmed)(JNIEnv*, jobject) { return gLearn.armedParam(); }
JNI(jint, nativeLearnLast)(JNIEnv*, jobject) { return gLearn.lastLearned(); }
JNI(void, nativeLearnRemove)(JNIEnv*, jobject, jint param) { gLearn.removeParam(param); }
JNI(jstring, nativeLearnDescribe)(JNIEnv* env, jobject, jint param) { return jstr(env, gLearn.describe(param)); }
JNI(jstring, nativeLearnSave)(JNIEnv* env, jobject, jstring path) {
    std::string err;
    return jstr(env, gLearn.save(str(env, path), err) ? "" : err);
}
JNI(jstring, nativeLearnLoad)(JNIEnv* env, jobject, jstring path) {
    std::string err;
    return jstr(env, gLearn.load(str(env, path), err) ? "" : err);
}

JNI(void, nativeSetTransport)(JNIEnv*, jobject, jboolean on) { gEngine->setTransport(on); }
JNI(jboolean, nativeTransport)(JNIEnv*, jobject) { return gEngine->transportPlaying() ? JNI_TRUE : JNI_FALSE; }

// ------------------------------------------------------------ audio input / recording

JNI(jboolean, nativeSetInput)(JNIEnv*, jobject, jboolean on) { return gAudio && gAudio->setInputEnabled(on) ? JNI_TRUE : JNI_FALSE; }
JNI(jfloat, nativeInputPeak)(JNIEnv*, jobject) { return gAudio ? gAudio->inputPeak() : 0.0f; }
JNI(jboolean, nativeRecordStart)(JNIEnv* env, jobject, jstring path) {
    return gAudio && gAudio->startRecording(str(env, path)) ? JNI_TRUE : JNI_FALSE;
}
JNI(jdouble, nativeRecordStop)(JNIEnv*, jobject) { return gAudio ? gAudio->stopRecording() : 0.0; }
JNI(jdouble, nativeRecordSeconds)(JNIEnv*, jobject) { return gAudio && gAudio->recording() ? gAudio->recordedSeconds() : -1.0; }
JNI(jboolean, nativeInputEnabled)(JNIEnv*, jobject) { return gAudio && gAudio->inputEnabled() ? JNI_TRUE : JNI_FALSE; }
JNI(jint, nativeSeqPosition)(JNIEnv*, jobject) { return gEngine->seqPosition(gEditPart.load()); }

// ------------------------------------------------------------ SysEx remote control

// Starts the remote control (once). presetDirs: ";"-separated.
JNI(void, nativeRemoteInit)(JNIEnv* env, jobject, jstring libRoot, jstring presetDirs, jstring userPresets, jstring midiDir,
                            jstring version) {
    if (gRemote || !gEngine) return;
    RemoteControl::Config cfg;
    cfg.deviceName = "HSynth Android";
    cfg.version = str(env, version);
    cfg.libraryRoot = str(env, libRoot);
    const std::string dirs = str(env, presetDirs);
    for (size_t a = 0; a <= dirs.size();) {
        size_t b = dirs.find(';', a);
        if (b == std::string::npos) b = dirs.size();
        if (b > a) cfg.presetDirs.push_back(dirs.substr(a, b - a));
        a = b + 1;
    }
    cfg.userPresetDir = str(env, userPresets);
    cfg.midiDir = str(env, midiDir);
    cfg.learn = &gLearn;
    RemoteControl::Hooks hooks;
    hooks.setMultiTimbral = [](bool on) {
        std::lock_guard<std::mutex> lock(gMidiMutex);
        gMidi.setMultiTimbral(on);
    };
    hooks.setMpe = [](bool on) {
        std::lock_guard<std::mutex> lock(gMidiMutex);
        gMidi.setMpeEnabled(on);
    };
    const std::string root = cfg.libraryRoot;
    hooks.loadSong = [root, dirs](const std::string& path, Json& info) -> std::string {
        Json r = songLoad(path, root, dirs);
        if (!r.find("ok") || !r.find("ok")->boolean()) return r.find("error") ? r.find("error")->string() : "failed";
        for (const auto& m : r.members())
            if (m.first != "ok") info.set(m.first, m.second);
        {
            std::lock_guard<std::mutex> lock(gSongMutex);
            gRemoteSongPath = path;
        }
        gRemoteSongs.fetch_add(1);
        return "";
    };
    gRemote = std::make_unique<RemoteControl>(*gEngine, cfg, hooks);
    gRemote->setOutput([](const std::vector<uint8_t>& m) {
        std::lock_guard<std::mutex> lock(gRemoteOutMutex);
        if (gRemoteOut.size() < 20000) gRemoteOut.push_back(m);
        gRemoteOutCv.notify_one();
    });
}

// The next reply / notification to send (waits up to timeoutMs), or null.
JNI(jbyteArray, nativeRemoteNext)(JNIEnv* env, jobject, jint timeoutMs) {
    std::vector<uint8_t> m;
    {
        std::unique_lock<std::mutex> lock(gRemoteOutMutex);
        gRemoteOutCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] { return !gRemoteOut.empty(); });
        if (gRemoteOut.empty()) return nullptr;
        m = std::move(gRemoteOut.front());
        gRemoteOut.pop_front();
    }
    jbyteArray a = env->NewByteArray((jsize)m.size());
    env->SetByteArrayRegion(a, 0, (jsize)m.size(), reinterpret_cast<const jbyte*>(m.data()));
    return a;
}

// MIDI device id the last request came from (replies go there), -1 none.
JNI(jint, nativeRemoteSource)(JNIEnv*, jobject) { return gRemoteSource.load(); }
// Bumped by every remote change (the UI refreshes).
JNI(jint, nativeRemoteChanges)(JNIEnv*, jobject) { return gRemote ? (jint)gRemote->changeCount() : 0; }
// Songs loaded remotely: count, and the path of the last one.
JNI(jint, nativeRemoteSongCount)(JNIEnv*, jobject) { return (jint)gRemoteSongs.load(); }
JNI(jstring, nativeRemoteSongPath)(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lock(gSongMutex);
    return jstr(env, gRemoteSongPath);
}
