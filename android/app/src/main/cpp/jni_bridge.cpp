// JNI bridge between the Kotlin UI and the HSynth engine.
// All calls here run on UI / loader / MIDI threads; none of them block the
// audio callback (engine API is lock-free towards the audio thread).
#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <sys/system_properties.h>

#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <thread>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "audio_engine.h"
#include "hsynth/ble_midi.h"
#include "hsynth/dsp/fm.h"
#include "hsynth/engine.h"
#include "hsynth/fileio.h"
#include "hsynth/formats/dls_gig.h"
#include "hsynth/formats/dx7_sysex.h"
#include "hsynth/formats/wavetable_ext.h"
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
#include "hsynth/stft.h"

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
std::mutex gRemoteInitMutex;
std::atomic<RemoteControl*> gRemoteReady{nullptr};
std::mutex gRemoteOutMutex;
std::condition_variable gRemoteOutCv;
std::deque<std::vector<uint8_t>> gRemoteOut;
std::atomic<int> gRemoteSource{-1};
std::unique_ptr<RemoteControl> gRemote;
struct SysExState {
    std::vector<uint8_t> buf;
    bool in = false;
};
std::map<int, SysExState> gSysEx;  // per source device (under gMidiMutex)

void sysexDone(int source, const uint8_t* data, size_t size) {
    auto* remote = gRemoteReady.load(std::memory_order_acquire);
    if (!remote) return;
    sysex::Frame f;
    if (!sysex::parseFrame(data, size, f) || f.type != sysex::Request) return;
    gRemoteSource.store(source);
    remote->onSysEx(data, size);
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
            sysexDone(source, st.buf.data(), st.buf.size());
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
                sysexDone(source, st.buf.data(), st.buf.size());
                st.buf.clear();
            }
        }
        i += (size_t)std::max(1, len);
    }
}

struct EngineSink : EventSink {
    void onEvent(const Event& e) override {
        // MIDI -> audio latency probe (stamped before the event is queued)
        if (e.type == EventType::NoteOn && gAudio) gAudio->noteReceived(AudioEngine::nowNs());
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

std::string utf8(JNIEnv* env, jstring input);
jstring jsonError(JNIEnv* env, const std::string& error);

OggDecodeFn androidOggDecodeCallback(JNIEnv* env) {
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK || env->PushLocalFrame(4) != JNI_OK) return {};

    const jclass helper = env->FindClass("com/hsynth/app/OggVorbisHelper");
    const jfieldID instance = helper ? env->GetStaticFieldID(helper, "INSTANCE", "Lcom/hsynth/app/OggVorbisHelper;") : nullptr;
    const jmethodID decode = instance ? env->GetMethodID(helper, "decodeEx", "([B)Lcom/hsynth/app/OggVorbisHelper$Decoded;") : nullptr;
    const jclass decoded = decode ? env->FindClass("com/hsynth/app/OggVorbisHelper$Decoded") : nullptr;
    const jmethodID samples = decoded ? env->GetMethodID(decoded, "getSamples", "()[F") : nullptr;
    const jmethodID rate = samples ? env->GetMethodID(decoded, "getSampleRate", "()I") : nullptr;
    const jmethodID channels = rate ? env->GetMethodID(decoded, "getChannels", "()I") : nullptr;
    const jobject local = channels ? env->GetStaticObjectField(helper, instance) : nullptr;
    const jobject decoder = local ? env->NewGlobalRef(local) : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->PopLocalFrame(nullptr);
    if (!decoder) {
        __android_log_print(ANDROID_LOG_ERROR, "HSynth", "Could not initialize the Ogg Vorbis JNI decoder");
        return {};
    }

    // The decoder singleton lives for the lifetime of the native engine.
    return [vm, decoder, decode, samples, rate, channels](const uint8_t* data, size_t size,
                                                          std::vector<float>& mono, int& sampleRate) {
        if (!size || size > static_cast<size_t>(std::numeric_limits<jsize>::max())) return false;
        struct DecoderThread {
            JavaVM* vm;
            JNIEnv* env = nullptr;
            bool attached = false;
            bool localFrame = false;
            ~DecoderThread() {
                if (env && env->ExceptionCheck()) env->ExceptionClear();
                if (localFrame) env->PopLocalFrame(nullptr);
                if (attached) vm->DetachCurrentThread();
            }
        } thread{vm};
        void* currentEnv = nullptr;
        const jint status = vm->GetEnv(&currentEnv, JNI_VERSION_1_6);
        if (status == JNI_OK) {
            thread.env = static_cast<JNIEnv*>(currentEnv);
        } else if (status == JNI_EDETACHED && vm->AttachCurrentThread(&thread.env, nullptr) == JNI_OK) {
            thread.attached = true;
        } else {
            return false;
        }
        JNIEnv* jni = thread.env;
        if (jni->ExceptionCheck()) return false;
        if (jni->PushLocalFrame(3) != JNI_OK) return false;
        thread.localFrame = true;
        const auto bytes = jni->NewByteArray(static_cast<jsize>(size));
        if (!bytes) return false;
        jni->SetByteArrayRegion(bytes, 0, static_cast<jsize>(size), reinterpret_cast<const jbyte*>(data));
        if (jni->ExceptionCheck()) return false;
        const jobject pcm = jni->CallObjectMethod(decoder, decode, bytes);
        if (jni->ExceptionCheck() || !pcm) return false;
        const auto values = static_cast<jfloatArray>(jni->CallObjectMethod(pcm, samples));
        if (jni->ExceptionCheck() || !values) return false;
        const int hz = jni->CallIntMethod(pcm, rate);
        if (jni->ExceptionCheck() || hz <= 0) return false;
        const int count = jni->CallIntMethod(pcm, channels);
        if (jni->ExceptionCheck() || (count != 1 && count != 2)) return false;
        const jsize length = jni->GetArrayLength(values);
        if (length <= 0 || length % count != 0) return false;
        mono.resize(static_cast<size_t>(length));
        jni->GetFloatArrayRegion(values, 0, length, mono.data());
        if (jni->ExceptionCheck()) return false;
        if (count == 2) {
            for (jsize i = 0; i < length / 2; ++i) mono[i] = 0.5f * (mono[2 * i] + mono[2 * i + 1]);
            mono.resize(static_cast<size_t>(length / 2));
        }
        sampleRate = hz;
        return true;
    };
}

}  // namespace

namespace {
void probeLoop();
}  // namespace

#define JNI(ret, name) extern "C" JNIEXPORT ret JNICALL Java_com_hsynth_app_NativeEngine_##name

JNI(void, nativeCreate)(JNIEnv* env, jobject) {
    if (gEngine) return;
    setHostOggDecoder(androidOggDecodeCallback(env));
    gEngine = std::make_unique<Engine>();
    gEngine->setSstEnabled(true);  // automatic live loads on Android
    gEngine->prepare(48000.0f);
    gEngine->setRenderThreads(0);  // auto: voices spread over about half the cores
    gAudio = std::make_unique<AudioEngine>(*gEngine);
    std::thread(probeLoop).detach();
}

JNI(jboolean, nativeStart)(JNIEnv*, jobject, jint deviceId, jboolean exclusive, jint bufferBursts,
                             jint sampleRate, jboolean alsaDirect) {
    if (!gAudio) return JNI_FALSE;
    return gAudio->start(deviceId, exclusive, bufferBursts, sampleRate, alsaDirect) ? JNI_TRUE : JNI_FALSE;
}

JNI(jstring, nativeAudioBackendNote)(JNIEnv* env, jobject) {
    return jstr(env, gAudio ? gAudio->lastBackendNote() : std::string());
}

JNI(void, nativeSetAlsaGain)(JNIEnv*, jobject, jfloat gain) {
    if (gAudio) gAudio->setAlsaGain(gain);
}

JNI(void, nativeStop)(JNIEnv*, jobject) {
    if (gAudio) gAudio->stop();
}

// The on-screen keyboard plays on the edit part's MIDI channel in multi mode,
// so every part on that channel sounds (layers) within its key range (splits).
// In single mode its events stay omni.
static Event keyboardEvent(Event e) {
    e.fromKeyboard = true;
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
void dispatchMidi1(const uint8_t* data, size_t n) {
    EngineSink sink;
    gLearn.onMidi(data, n, *gEngine);  // learned CCs set their parameters
    gMidi.processMidi1(data, n, sink);
}

// MIDI 1.0 bytes from a source (gMidiMutex held)
void midi1(int source, const uint8_t* data, size_t n) {
    const std::vector<uint8_t> rest = takeSysEx(source, data, n);
    if (!rest.empty()) dispatchMidi1(rest.data(), rest.size());
}
std::map<int, ble::Decoder> gBle;  // BLE MIDI connections (under gMidiMutex)

// Latency probe for measurements without a MIDI device: while the system property
// debug.hsynth.probe is a MIDI channel 1..16 (`adb shell setprop debug.hsynth.probe 1`;
// 0 / unset = off) a short note is played through the regular native MIDI path
// about three times a second at random phase to the audio callbacks; the HSynth
// stats line then reports the receive -> callback wait.
void probeLoop() {
    pthread_setname_np(pthread_self(), "hsynth-probe");
    constexpr int kProbeSource = 0x7F00'0000;
    for (unsigned k = 1;; ++k) {
        char v[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.hsynth.probe", v);
        const int ch = atoi(v);
        if (ch < 1 || ch > 16) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        // debug.hsynth.probe.pc = 1: also a program change every 8th note (preset loads
        // while playing, to check that loading never stalls the audio callback)
        char pc[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.hsynth.probe.pc", pc);
        if (pc[0] == '1' && k % 8 == 0) {
            const uint8_t m[2] = {(uint8_t)(0xC0 | (ch - 1)), (uint8_t)((k / 8) % 8)};
            std::lock_guard<std::mutex> lock(gMidiMutex);
            midi1(kProbeSource, m, 2);
        }
        const uint8_t on[3] = {(uint8_t)(0x90 | (ch - 1)), (uint8_t)(60 + k % 12), 64};
        const uint8_t off[3] = {(uint8_t)(0x80 | (ch - 1)), on[1], 0};
        {
            std::lock_guard<std::mutex> lock(gMidiMutex);
            midi1(kProbeSource, on, 3);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        {
            std::lock_guard<std::mutex> lock(gMidiMutex);
            midi1(kProbeSource, off, 3);
        }
        // 200 ms + 0..2.9 ms: random phase against the 2 ms callbacks
        const auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
        std::this_thread::sleep_for(std::chrono::microseconds(200000 + (ns / 1000) % 2900));
    }
}
}  // namespace

// A BLE MIDI notification or GATT server write (BleMidi.kt).
JNI(void, nativeBlePacket)(JNIEnv* env, jobject, jbyteArray data, jint source) {
    const jsize n = env->GetArrayLength(data);
    if (n <= 0) return;
    uint8_t stack[512];  // maximum ATT attribute value length
    std::vector<uint8_t> overflow;
    uint8_t* buf = stack;
    if ((size_t)n > sizeof stack) {
        overflow.resize((size_t)n);
        buf = overflow.data();
    }
    env->GetByteArrayRegion(data, 0, n, reinterpret_cast<jbyte*>(buf));
    std::lock_guard<std::mutex> lock(gMidiMutex);
    gBle[source].packet(buf, (size_t)n, [source](const uint8_t* m, size_t len) {
        // The BLE decoder already emits complete MIDI messages and assembles SysEx.
        if (m[0] == 0xF0) sysexDone(source, m, len);
        else dispatchMidi1(m, len);
    });
}

JNI(void, nativeBleReset)(JNIEnv*, jobject, jint source) {
    std::lock_guard<std::mutex> lock(gMidiMutex);
    gBle.erase(source);
}

JNI(jint, nativeBleAggressiveInterval)(JNIEnv*, jobject) {
    char value[PROP_VALUE_MAX];
    if (__system_property_get("bluetooth.core.le.max_connection_interval_aggressive", value) <= 0) return 0;
    return std::atoi(value);
}

JNI(jboolean, nativeBleVerifiedBoot)(JNIEnv*, jobject) {
    char value[PROP_VALUE_MAX];
    return __system_property_get("ro.boot.verifiedbootstate", value) > 0 && std::strcmp(value, "green") == 0;
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

JNI(void, nativeMidi)(JNIEnv* env, jobject, jbyteArray data, jint offset, jint count, jboolean ump, jint source,
                     jlong timestampNs) {
    if (count <= 0) return;
    // MidiManager timestamps are System.nanoTime() (CLOCK_MONOTONIC) of the sender
    // (for Bluetooth: the BLE packet's arrival in the system MIDI service)
    if (gAudio && timestampNs > 0) {
        const int64_t hop = AudioEngine::nowNs() - (int64_t)timestampNs;
        if (hop >= 0 && hop < 1000000000LL) gAudio->midiHop(hop);
    }
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

namespace {
bool sstCommitPending(uint32_t serial) {
    if (!serial || gEngine->liveChangeApplied(serial)) return false;
    if (gAudio) gAudio->commitSstWhileStopped();
    if (gEngine->liveChangeApplied(serial)) return false;
    if (!gAudio || !gAudio->stats().running) return true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(125);
    while (!gEngine->liveChangeApplied(serial) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return !gEngine->liveChangeApplied(serial);
}

jstring loadPreset(JNIEnv* env, jstring path, jstring libRoot, bool forceSst) {
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
    const bool single = part == 0 && !gEngine->multiTimbral();
    const bool live = forceSst || gEngine->sstEnabled();
    const std::string root = str(env, libRoot);
    uint32_t serial = 0;
    const bool ok = live ? (single ? applyPresetSst(*gEngine, p, root, warnings, &serial)
                                   : applyPartPresetSst(*gEngine, part, p, root, warnings, &serial))
                         : (single ? applyPreset(*gEngine, p, root, warnings)
                                   : applyPartPreset(*gEngine, part, p, root, warnings));
    if (!ok) {
        r.set("ok", false);
        r.set("error", "could not stage the preset");
        return jstr(env, r.dump(0));
    }
    if (live) r.set("pending", sstCommitPending(serial));
    songPartForgotten(part);  // the part no longer holds the song's sound
    r.set("ok", true);
    r.set("name", p.name);
    r.set("category", p.category);
    r.set("comment", p.comment);
    r.set("warnings", warningsJson(warnings));
    return jstr(env, r.dump(0));
}

}  // namespace

JNI(jstring, nativeLoadPreset)(JNIEnv* env, jobject, jstring path, jstring libRoot) {
    return loadPreset(env, path, libRoot, false);
}

JNI(jstring, nativeLoadPresetSst)(JNIEnv* env, jobject, jstring path, jstring libRoot) {
    return loadPreset(env, path, libRoot, true);
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
    const bool ok = loadPartSamples(*gEngine, gEditPart.load(), layer, utf8(env, path), err, warnings);
    r.set("ok", ok);
    if (!ok) r.set("error", err);
    r.set("warnings", warningsJson(warnings));
    return jstr(env, sysex::asciiJson(r));
}

namespace {
bool validFormatLayer(int layer) { return gEngine && layer >= 0 && layer < kNumLayers; }

std::string formatExtension(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

jstring formatSuccess(JNIEnv* env, const char* key, Json value) {
    Json reply = Json::object();
    reply.set("ok", true);
    reply.set(key, std::move(value));
    return jstr(env, sysex::asciiJson(reply));
}

std::string safeFilenameComponent(const std::string& name, const char* fallback) {
    std::string safe;
    for (unsigned char c : name) {
        const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alnum || c == '-' || c == '_') safe.push_back(static_cast<char>(c));
        else if (!safe.empty() && safe.back() != '_') safe.push_back('_');
        if (safe.size() == 80) break;
    }
    while (!safe.empty() && safe.back() == '_') safe.pop_back();
    return safe.empty() ? fallback : safe;
}

struct WavetablePreview {
    std::string path;
    Wavetable table;
};

std::mutex gWavetablePreviewMutex;
std::array<std::array<WavetablePreview, kNumLayers>, kNumParts> gWavetablePreviews;

// Blocking (audio decode + FFT): call from the loader thread.
jstring loadEditorWavetable(JNIEnv* env, jint layer, jstring path, bool selectUserTable) {
    if (!validFormatLayer(layer)) return jsonError(env, "invalid layer or engine is not initialized");
    const int part = gEditPart.load();
    WavetablePreview preview;
    preview.path = utf8(env, path);
    std::string error;
    if (!loadPartWavetable(*gEngine, part, layer, preview.path, error, &preview.table))
        return jsonError(env, error);
    const int frames = preview.table.frames;
    {
        std::lock_guard<std::mutex> lock(gWavetablePreviewMutex);
        gWavetablePreviews[part][layer] = std::move(preview);
    }
    if (selectUserTable) {
        gEngine->setPartParam(part, P::layer(layer, P::WtTable), static_cast<float>(builtinWavetableCount()));
        gEngine->setPartParam(part, P::layer(layer, P::LType), static_cast<float>(LayerType::WT));
        return formatSuccess(env, "frames", frames);
    }
    Json reply = Json::object();
    reply.set("ok", true);
    return jstr(env, reply.dump(0));
}
}

JNI(jstring, nativeLoadWavetable)(JNIEnv* env, jobject, jint layer, jstring path) {
    return loadEditorWavetable(env, layer, path, false);
}

JNI(jstring, nativeListSoundBankPresets)(JNIEnv* env, jobject, jstring path) {
    const auto file = utf8(env, path);
    const auto ext = formatExtension(file);
    Json presets = Json::array();
    std::string error;
    const auto append = [&](int bank, int program, bool isDrum, const std::string& name, int index) {
        Json row = Json::object();
        row.set("bank", bank);
        row.set("program", program);
        row.set("isDrum", isDrum);
        row.set("name", name);
        row.set("index", index);
        presets.push(std::move(row));
    };
    if (ext == ".sf2" || ext == ".sf3") {
        std::vector<Sf2PresetInfo> entries;
        if (!listSf2Presets(file, entries, error)) return jsonError(env, error);
        for (const auto& p : entries) append(p.bank, p.program, p.bank == 128, p.name, p.presetIndex);
    } else if (ext == ".dls" || ext == ".gig") {
        std::vector<DlsPresetInfo> entries;
        if (!listDlsGigPresets(file, entries, error)) return jsonError(env, error);
        for (const auto& p : entries) append(p.bank, p.program, p.isDrum, p.name, p.instrumentIndex);
    } else {
        return jsonError(env, "unsupported sound bank type: " + file);
    }
    return formatSuccess(env, "presets", std::move(presets));
}

JNI(jstring, nativeLoadSoundBankPreset)(JNIEnv* env, jobject, jint layer, jstring path, jint bank, jint program,
                                        jboolean isDrum) {
    if (!validFormatLayer(layer)) return jsonError(env, "invalid layer or engine is not initialized");
    const int part = gEditPart.load();
    std::string error;
    std::vector<std::string> warnings;
    if (!loadPartSoundBankPreset(*gEngine, part, layer, utf8(env, path), bank, program, isDrum == JNI_TRUE, error, warnings))
        return jsonError(env, error);
    gEngine->setPartParam(part, P::layer(layer, P::LType), static_cast<float>(LayerType::PCM));
    return formatSuccess(env, "warnings", warningsJson(warnings));
}

JNI(jstring, nativeLoadDecentSampler)(JNIEnv* env, jobject, jint layer, jstring path) {
    if (!validFormatLayer(layer)) return jsonError(env, "invalid layer or engine is not initialized");
    const auto file = utf8(env, path);
    if (formatExtension(file) != ".dspreset") return jsonError(env, "expected a .dspreset file");
    const int part = gEditPart.load();
    std::string error;
    std::vector<std::string> warnings;
    if (!loadPartSamples(*gEngine, part, layer, file, error, warnings)) return jsonError(env, error);
    gEngine->setPartParam(part, P::layer(layer, P::LType), static_cast<float>(LayerType::PCM));
    return formatSuccess(env, "warnings", warningsJson(warnings));
}

JNI(jstring, nativeInspectDx7Sysex)(JNIEnv* env, jobject, jstring path) {
    std::vector<Preset> presets;
    std::vector<Dx7VoiceSummary> summaries;
    std::string error;
    if (!loadDx7SysexFile(utf8(env, path), presets, error, &summaries)) return jsonError(env, error);
    Json voices = Json::array();
    for (const auto& voice : summaries) {
        Json row = Json::object();
        row.set("index", voice.index);
        row.set("name", voice.name);
        row.set("algo", voice.algorithm);
        voices.push(std::move(row));
    }
    return formatSuccess(env, "voices", std::move(voices));
}

JNI(jstring, nativeLoadDx7Voice)(JNIEnv* env, jobject, jstring path, jint index) {
    if (!gEngine) return jsonError(env, "engine is not initialized");
    const int part = gEditPart.load();
    std::vector<Preset> presets;
    std::string error;
    if (!loadDx7SysexFile(utf8(env, path), presets, error)) return jsonError(env, error);
    if (index < 0 || static_cast<size_t>(index) >= presets.size()) return jsonError(env, "DX7 voice index out of range");
    const auto& preset = presets[index];
    std::vector<std::string> warnings;
    uint32_t serial = 0;
    const bool live = gEngine->sstEnabled();
    const bool ok = live ? applyPartPresetSst(*gEngine, part, preset, "", warnings, &serial)
                         : applyPartPreset(*gEngine, part, preset, "", warnings);
    if (!ok) return jsonError(env, "could not apply the DX7 voice");
    if (live) sstCommitPending(serial);
    songPartForgotten(part);
    return formatSuccess(env, "name", preset.name);
}

JNI(jstring, nativeImportDx7Bank)(JNIEnv* env, jobject, jstring path, jstring outDir, jstring prefix) {
    std::vector<Preset> presets;
    std::string error;
    if (!loadDx7SysexFile(utf8(env, path), presets, error)) return jsonError(env, error);
    const auto dir = utf8(env, outDir);
    if (dir.empty() || dir.find('\0') != std::string::npos) return jsonError(env, "invalid preset output directory");
    const auto folder = std::filesystem::path(dir);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) return jsonError(env, "could not create preset output directory: " + ec.message());
    const std::string base = safeFilenameComponent(utf8(env, prefix), "DX7");
    Json names = Json::array();
    for (size_t i = 0; i < presets.size(); ++i) {
        const std::string number = (i < 9 ? "0" : "") + std::to_string(i + 1);
        const auto filename = base + "_" + number + "_" + safeFilenameComponent(presets[i].name, "Voice") + ".json";
        if (!savePresetFile((folder / filename).string(), presets[i], error)) return jsonError(env, error);
        names.push(presets[i].name);
    }
    Json reply = Json::object();
    reply.set("ok", true);
    reply.set("count", static_cast<int>(presets.size()));
    reply.set("names", std::move(names));
    return jstr(env, sysex::asciiJson(reply));
}

JNI(jstring, nativeLoadAnyWavetable)(JNIEnv* env, jobject, jint layer, jstring path) {
    return loadEditorWavetable(env, layer, path, true);
}

JNI(jboolean, nativeWavetablePreview)(JNIEnv* env, jobject, jint layer, jfloat pos, jfloatArray outSamples) {
    if (!validFormatLayer(layer) || !outSamples || !std::isfinite(pos)) return JNI_FALSE;
    const int part = gEditPart.load();
    const int index = static_cast<int>(gEngine->getPartParam(part, P::layer(layer, P::WtTable)));
    const jsize count = env->GetArrayLength(outSamples);
    if (count <= 0) return JNI_FALSE;
    std::vector<float> values(static_cast<size_t>(count));
    const bool userTable = index >= builtinWavetableCount();
    const std::string path = userTable ? gEngine->partSampleSetPath(part, layer) : "";
    if (path.empty()) {
        sampleWavetablePreview(builtinWavetable(userTable ? 0 : index), pos, count, values.data());
    } else {
        std::lock_guard<std::mutex> lock(gWavetablePreviewMutex);
        auto& preview = gWavetablePreviews[part][layer];
        if (path != preview.path) {
            Wavetable table;
            std::string error;
            if (!loadAnyWavetableFile(path, table, error)) return JNI_FALSE;
            preview.table.frames = table.frames;
            preview.table.size[0] = table.size[0];
            preview.table.data[0] = std::move(table.data[0]);
            preview.path = path;
        }
        sampleWavetablePreview(preview.table, pos, count, values.data());
    }
    env->SetFloatArrayRegion(outSamples, 0, count, values.data());
    return env->ExceptionCheck() ? JNI_FALSE : JNI_TRUE;
}

JNI(void, nativeUnloadSamples)(JNIEnv*, jobject, jint layer) {
    if (!validFormatLayer(layer)) return;
    const int part = gEditPart.load();
    gEngine->setPartSampleSet(part, layer, nullptr);
    gEngine->collectGarbage();
    std::lock_guard<std::mutex> lock(gWavetablePreviewMutex);
    gWavetablePreviews[part][layer] = {};
}

JNI(jstring, nativeSampleSetName)(JNIEnv* env, jobject, jint layer) { return jstr(env, gEngine->partSampleSetName(gEditPart.load(), layer)); }

// Inspect an SFZ/WAV without loading it into the engine (library screen).
JNI(jstring, nativeInspectSamples)(JNIEnv* env, jobject, jstring path) {
    Json r = Json::object();
    std::string err;
    std::vector<std::string> warnings;
    auto set = loadHostSamples(utf8(env, path), err, warnings);
    r.set("ok", set != nullptr);
    if (!set) {
        r.set("error", err);
    } else {
        r.set("regions", (int)set->regions.size());
        r.set("samples", (int)set->samples.size());
        r.set("bytes", (double)set->memoryBytes());
        r.set("warnings", warningsJson(warnings));
    }
    return jstr(env, sysex::asciiJson(r));
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

namespace {
jstring loadMulti(JNIEnv* env, jstring path, jstring libRoot, bool forceSst) {
    Json r = Json::object();
    MultiPreset m;
    std::string err;
    if (!loadMultiFile(str(env, path), m, err)) {
        r.set("ok", false);
        r.set("error", err);
        return jstr(env, r.dump(0));
    }
    std::vector<std::string> warnings;
    const std::string root = str(env, libRoot);
    const bool live = forceSst || gEngine->sstEnabled();
    uint32_t serial = 0;
    const bool ok = live ? applyMultiSst(*gEngine, m, root, warnings, &serial)
                         : applyMulti(*gEngine, m, root, warnings);
    if (!ok) {
        r.set("ok", false);
        r.set("error", "could not stage the multi");
        return jstr(env, r.dump(0));
    }
    if (live) r.set("pending", sstCommitPending(serial));
    {
        std::lock_guard<std::mutex> lock(gMidiMutex);
        gMidi.setMultiTimbral(true);
    }
    for (int k = 0; k < kNumParts; ++k) songPartForgotten(k);
    gEditPart.store(m.selectedPart);
    r.set("ok", true);
    r.set("name", m.name);
    Json names = Json::array();
    for (int k = 0; k < kNumParts; ++k) names.push(m.parts[k].name);
    r.set("parts", names);
    r.set("warnings", warningsJson(warnings));
    return jstr(env, r.dump(0));
}

}  // namespace

JNI(jstring, nativeLoadMulti)(JNIEnv* env, jobject, jstring path, jstring libRoot) {
    return loadMulti(env, path, libRoot, false);
}

JNI(jstring, nativeLoadMultiSst)(JNIEnv* env, jobject, jstring path, jstring libRoot) {
    return loadMulti(env, path, libRoot, true);
}

JNI(void, nativeSetSst)(JNIEnv*, jobject, jboolean on, jint ms) {
    if (!gEngine) return;
    gEngine->setSstTimeMs((float)ms);
    gEngine->setSstEnabled(on);
}

JNI(jfloat, nativeSstProgress)(JNIEnv*, jobject) { return gEngine ? gEngine->sstProgress() : -1.0f; }

JNI(jstring, nativeMixMetadata)(JNIEnv* env, jobject) { return jstr(env, mixMetadataJson()); }

JNI(void, nativePhraseCommand)(JNIEnv*, jobject, jint cmd) {
    if (gEngine) gEngine->phraseCommand(cmd);
}

JNI(void, nativePhraseSetBars)(JNIEnv*, jobject, jint bars) {
    if (gEngine) gEngine->setPhraseBars(bars);
}

JNI(jfloatArray, nativePhraseStatus)(JNIEnv* env, jobject) {
    jfloat values[5] = {};
    if (gEngine) {
        values[0] = (float)gEngine->phraseState();
        values[1] = (float)gEngine->phraseLength();
        values[2] = (float)gEngine->phrasePosition();
        values[3] = (float)gEngine->phraseEventCount();
        values[4] = (float)gEngine->phraseBars();
    }
    auto result = env->NewFloatArray(5);
    if (result) env->SetFloatArrayRegion(result, 0, 5, values);
    return result;
}

JNI(jboolean, nativePhraseQuantize)(JNIEnv*, jobject, jfloat grid, jfloat strength) {
    std::string error;
    return gEngine && quantizeRecordedPhrase(*gEngine, grid, strength, error) ? JNI_TRUE : JNI_FALSE;
}

JNI(jstring, nativePhraseExport)(JNIEnv* env, jobject, jstring path, jint repeats) {
    if (!gEngine) return jstr(env, "the engine is not initialized");
    std::string error;
    return jstr(env, exportRecordedPhrase(*gEngine, str(env, path), repeats, error) ? "" : error);
}

JNI(jstring, nativeApplyScala)(JNIEnv* env, jobject, jint part, jstring path, jint root) {
    if (!gEngine) return jstr(env, R"({"ok":false,"error":"the engine is not initialized"})");
    return jstr(env, sysex::asciiJson(scalaApplyJson(*gEngine, part, str(env, path), root)));
}

JNI(jstring, nativeScalaInfo)(JNIEnv* env, jobject, jstring path) {
    return jstr(env, sysex::asciiJson(scalaInfoJson(str(env, path))));
}

JNI(void, nativeSetPerfOffsets)(JNIEnv*, jobject, jint transpose, jfloat trimDb) {
    if (gEngine) gEngine->setPerfOffsets(transpose, trimDb);
}

JNI(jint, nativeGetPerfTranspose)(JNIEnv*, jobject) { return gEngine ? gEngine->perfTranspose() : 0; }
JNI(jfloat, nativeGetPerfTrimDb)(JNIEnv*, jobject) { return gEngine ? gEngine->perfTrimDb() : 0.0f; }

JNI(jint, nativePhysModes)(JNIEnv* env, jobject, jint part, jint layer, jfloatArray out) {
    if (!gEngine || !out) return 0;
    jfloat modes[16 * 3];
    const int count = gEngine->physModes(part, layer, modes, env->GetArrayLength(out) / 3);
    if (count) env->SetFloatArrayRegion(out, 0, count * 3, modes);
    return count;
}

JNI(void, nativeMorphSetA)(JNIEnv*, jobject) {
    if (gEngine) gEngine->morphSetA(gEditPart.load());
}
JNI(jint, nativeMorphCaptureB)(JNIEnv*, jobject) {
    return gEngine ? gEngine->morphCaptureB(gEditPart.load()) : 0;
}
JNI(void, nativeMorphAssign)(JNIEnv*, jobject, jint id, jfloat a, jfloat b) {
    if (gEngine) gEngine->morphAssign(gEditPart.load(), id, a, b);
}
JNI(void, nativeMorphClear)(JNIEnv*, jobject, jint id) {
    if (gEngine && id >= 0) gEngine->morphClear(gEditPart.load(), id);
}
JNI(void, nativeMorphClearAll)(JNIEnv*, jobject) {
    if (gEngine) gEngine->morphClearAll(gEditPart.load());
}
JNI(jfloatArray, nativeMorphDump)(JNIEnv* env, jobject) {
    const auto program = gEngine ? gEngine->partSoundOptions(gEditPart.load()).morph : MorphProgram{};
    jfloat values[MorphProgram::kCapacity * 3];
    for (int i = 0; i < program.count; ++i) {
        values[i * 3] = program.routes[i].paramId;
        values[i * 3 + 1] = program.routes[i].a;
        values[i * 3 + 2] = program.routes[i].b;
    }
    jfloatArray out = env->NewFloatArray(program.count * 3);
    if (out && program.count) env->SetFloatArrayRegion(out, 0, program.count * 3, values);
    return out;
}
JNI(jfloat, nativeMorphNow)(JNIEnv*, jobject, jint id) {
    return gEngine ? gEngine->morphNow(gEditPart.load(), id) : 0.0f;
}

JNI(jboolean, nativeSetArpPatternJson)(JNIEnv* env, jobject, jint part, jstring json) {
    Json config;
    std::string error;
    return gEngine && Json::parse(str(env, json), config, error) && gEngine->setArpPatternJson(part, config);
}
JNI(jstring, nativeGetArpPatternJson)(JNIEnv* env, jobject, jint part) {
    return jstr(env, arpPatternToJson(gEngine ? gEngine->arpPattern(part) : ArpPattern{}).dump(0));
}
JNI(jint, nativeArpStep)(JNIEnv*, jobject) {
    return gEngine ? gEngine->arpStep(gEditPart.load()) : -1;
}

JNI(jboolean, nativeSetRhythmJson)(JNIEnv* env, jobject, jstring json) {
    Json config;
    std::string error;
    return gEngine && Json::parse(str(env, json), config, error) && gEngine->setRhythmJson(config);
}
JNI(jstring, nativeGetRhythmJson)(JNIEnv* env, jobject) {
    return jstr(env, (gEngine ? gEngine->rhythmJson() : rhythmToJson(RhythmConfig{})).dump(0));
}
JNI(void, nativeSetRhythmFill)(JNIEnv*, jobject, jboolean on) {
    if (gEngine) gEngine->setRhythmFill(on);
}
JNI(jstring, nativeRhythmPatternsJson)(JNIEnv* env, jobject) {
    return jstr(env, rhythmPatternsJson().dump(0));
}

JNI(jboolean, nativeSetMetronomeJson)(JNIEnv* env, jobject, jstring json) {
    Json config;
    std::string error;
    return gEngine && Json::parse(str(env, json), config, error) && gEngine->setMetronomeJson(config);
}

JNI(jstring, nativeGetMetronomeJson)(JNIEnv* env, jobject) {
    return jstr(env, metronomeToJson(gEngine ? gEngine->metronomeConfig() : MetronomeConfig{}).dump(0));
}

JNI(void, nativeSetMetronomeEnabled)(JNIEnv*, jobject, jboolean on) {
    if (gEngine) gEngine->setMetronomeEnabled(on);
}

JNI(jint, nativeClickBeat)(JNIEnv*, jobject) { return gEngine ? gEngine->clickBeat() : -1; }

JNI(void, nativeTunerEnable)(JNIEnv*, jobject, jboolean on) {
    if (gEngine) gEngine->tuner().setEnabled(on);
}

JNI(void, nativeTunerConfig)(JNIEnv*, jobject, jfloat a4Hz, jboolean muteOut) {
    if (gEngine) gEngine->tuner().configure(a4Hz, muteOut);
}

JNI(jboolean, nativeTunerRead)(JNIEnv* env, jobject, jfloatArray out) {
    if (!out || env->GetArrayLength(out) < 4) return JNI_FALSE;
    TunerReading reading;
    const bool valid = gEngine && gEngine->tuner().analyze(reading);
    const float values[] = {reading.freqHz, reading.centsOff, reading.midiNoteFloat, reading.confidence};
    env->SetFloatArrayRegion(out, 0, 4, values);
    return valid ? JNI_TRUE : JNI_FALSE;
}

JNI(void, nativeScopeEnable)(JNIEnv*, jobject, jboolean on) {
    if (gEngine) gEngine->audioTap().setEnabled(on);
}

JNI(jfloat, nativeScopeFrame)(JNIEnv* env, jobject, jfloatArray wave, jfloatArray spec) {
    float waveBuffer[AudioTap::kCapacity], specBuffer[AudioTap::kFftSize];
    const int waveLen = wave ? env->GetArrayLength(wave) : 0;
    const int specLen = spec ? env->GetArrayLength(spec) : 0;
    // The usual UI sizes use fixed scratch. Honor larger destination arrays as well.
    std::vector<float> largeWave, largeSpec;
    if (waveLen > AudioTap::kCapacity) largeWave.resize(waveLen);
    if (specLen > AudioTap::kFftSize) largeSpec.resize(specLen);
    float* w = largeWave.empty() ? waveBuffer : largeWave.data();
    float* s = largeSpec.empty() ? specBuffer : largeSpec.data();
    float hz = 0.0f;
    if (gEngine) hz = gEngine->audioTap().readFrame(w, waveLen, s, specLen);
    else {
        std::fill_n(w, waveLen, 0.0f);
        std::fill_n(s, specLen, 0.0f);
    }
    if (waveLen) env->SetFloatArrayRegion(wave, 0, waveLen, w);
    if (specLen) env->SetFloatArrayRegion(spec, 0, specLen, s);
    return hz;
}

JNI(jboolean, nativeSetPedalConfigJson)(JNIEnv* env, jobject, jstring json) {
    Json config;
    std::string error;
    return gEngine && Json::parse(str(env, json), config, error) && gEngine->setPedalConfigJson(config);
}

JNI(jstring, nativeGetPedalConfigJson)(JNIEnv* env, jobject) {
    return jstr(env, performanceInputToJson(gEngine ? gEngine->performanceInputConfig() : PerformanceInputConfig{}).dump(0));
}

JNI(void, nativeSetFootswitch)(JNIEnv*, jobject, jint slot, jint cc, jint action, jboolean invert) {
    if (gEngine) gEngine->setFootswitch(slot, cc, action, invert);
}

JNI(jint, nativeTakeHostAction)(JNIEnv*, jobject) { return gEngine ? gEngine->takeHostAction() : 0; }
JNI(jint, nativeLastCc)(JNIEnv*, jobject) { return gEngine ? static_cast<jint>(gEngine->lastCc()) : 0; }
JNI(jint, nativePedalState)(JNIEnv*, jobject) { return gEngine ? gEngine->pedalState() : 0; }

JNI(jboolean, nativeSetMasterStageJson)(JNIEnv* env, jobject, jstring json) {
    Json j;
    std::string error;
    if (!gEngine || !Json::parse(str(env, json), j, error)) return JNI_FALSE;
    return gEngine->updateMasterStage([&](MasterStageConfig& c) { return masterStageFromJson(j, c); })
        ? JNI_TRUE : JNI_FALSE;
}

JNI(jstring, nativeGetMasterStageJson)(JNIEnv* env, jobject) {
    return jstr(env, masterStageToJson(gEngine ? gEngine->getMasterStage() : MasterStageConfig{}).dump(0));
}

JNI(void, nativeSetMasterStageBypass)(JNIEnv*, jobject, jboolean bypass) {
    if (gEngine) gEngine->updateMasterStage([&](MasterStageConfig& c) {
        c.bypass = bypass;
        return true;
    });
}

JNI(void, nativeSetMasterEqBand)(JNIEnv*, jobject, jint band, jfloat freq, jfloat gainDb, jfloat q, jboolean enabled) {
    if (gEngine && band >= 0 && band < 5) gEngine->updateMasterStage([&](MasterStageConfig& c) {
        c.bands[(size_t)band] = {freq, gainDb, q, enabled != 0};
        return true;
    });
}

JNI(void, nativeSetMasterComp)(JNIEnv*, jobject, jboolean on, jfloat threshold, jfloat ratio, jfloat attack,
                              jfloat release, jfloat makeup) {
    if (gEngine) gEngine->updateMasterStage([&](MasterStageConfig& c) {
        c.compEnabled = on;
        c.comp = {threshold, ratio, attack, release, c.comp.kneeDb, makeup};
        return true;
    });
}

JNI(jfloat, nativeMasterCompGrDb)(JNIEnv*, jobject) { return gEngine ? gEngine->masterCompGrDb() : 0.0f; }

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
JNI(jfloat, nativeInputPeakPeek)(JNIEnv*, jobject) { return gAudio ? gAudio->inputPeakPeek() : 0.0f; }
JNI(jboolean, nativeRecordStart)(JNIEnv* env, jobject, jstring path) {
    return gAudio && gAudio->startRecording(str(env, path)) ? JNI_TRUE : JNI_FALSE;
}
JNI(jdouble, nativeRecordStop)(JNIEnv*, jobject) { return gAudio ? gAudio->stopRecording() : 0.0; }
JNI(jdouble, nativeRecordSeconds)(JNIEnv*, jobject) { return gAudio && gAudio->recording() ? gAudio->recordedSeconds() : -1.0; }
JNI(jboolean, nativeInputEnabled)(JNIEnv*, jobject) { return gAudio && gAudio->inputEnabled() ? JNI_TRUE : JNI_FALSE; }
JNI(jint, nativeSeqPosition)(JNIEnv*, jobject) { return gEngine->seqPosition(gEditPart.load()); }

// ------------------------------------------------------------ workstation audio and MIDI

namespace {
// JNI's Modified UTF-8 represents supplementary characters as two surrogate
// encodings. The filesystem and JSON parser use standard UTF-8.
std::string utf8(JNIEnv* env, jstring input) {
    if (!input) return {};
    const auto length = env->GetStringLength(input);
    std::vector<jchar> chars(length);
    if (length) env->GetStringRegion(input, 0, length, chars.data());
    std::string result;
    result.reserve(length);
    for (jsize i = 0; i < length; ++i) {
        uint32_t c = chars[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < length &&
            chars[i+1] >= 0xdc00 && chars[i+1] <= 0xdfff) {
            c = 0x10000 + ((c - 0xd800) << 10) + (chars[++i] - 0xdc00);
        } else if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;
        if (c < 0x80) {
            result.push_back(char(c));
        } else {
            const int bits = c < 0x800 ? 6 : c < 0x10000 ? 12 : 18;
            result.push_back(char((bits == 6 ? 0xc0 : bits == 12 ? 0xe0 : 0xf0) | (c >> bits)));
            for (int shift = bits - 6; shift >= 0; shift -= 6)
                result.push_back(char(0x80 | ((c >> shift) & 0x3f)));
        }
    }
    return result;
}

template<class Values>
jfloatArray floats(JNIEnv* env, const Values& values) {
    const auto n = static_cast<jsize>(values.size());
    auto array = env->NewFloatArray(n);
    if (array && n) env->SetFloatArrayRegion(array, 0, n, values.data());
    return array;
}

jstring jsonError(JNIEnv* env, const std::string& error) {
    Json reply = Json::object();
    reply.set("ok", false);
    reply.set("error", error);
    return jstr(env, sysex::asciiJson(reply));
}
}

JNI(jstring, nativeNpuStatus)(JNIEnv* env, jobject) {
    return jstr(env, sysex::asciiJson(gEngine->npuMorph().status()));
}
JNI(jboolean, nativeNpuMorphCaptureAnchor)(JNIEnv* env, jobject, jint slot, jfloat x, jfloat y, jstring name) {
    return gEngine->npuMorph().captureAnchor(*gEngine, gEditPart.load(), slot, x, y, utf8(env, name));
}
JNI(jstring, nativeNpuMorphLoadPresetAnchor)(JNIEnv* env, jobject, jint slot, jfloat x, jfloat y,
                                             jstring path, jstring libRoot) {
    const auto reply = gEngine->npuMorph().loadPresetAnchor(slot, x, y, utf8(env, path), utf8(env, libRoot));
    return jstr(env, sysex::asciiJson(reply));
}
JNI(void, nativeNpuMorphRemoveAnchor)(JNIEnv*, jobject, jint slot) { gEngine->npuMorph().removeAnchor(slot); }
JNI(void, nativeNpuMorphClearAnchors)(JNIEnv*, jobject) { gEngine->npuMorph().clearAnchors(); }
JNI(jstring, nativeNpuMorphCompile)(JNIEnv* env, jobject) {
    return jstr(env, sysex::asciiJson(gEngine->npuMorph().compile()));
}
JNI(jfloat, nativeNpuMorphEval)(JNIEnv*, jobject, jfloat x, jfloat y) {
    return gEngine->npuMorph().evaluate(*gEngine, gEditPart.load(), x, y);
}
JNI(jstring, nativeNpuMorphAnchorsJson)(JNIEnv* env, jobject) {
    return jstr(env, sysex::asciiJson(gEngine->npuMorph().anchorsJson()));
}

JNI(jstring, nativeAudioLoadStems)(JNIEnv* env, jobject, jstring vocals, jstring drums, jstring bass, jstring other) {
    const std::string paths[] = {utf8(env, vocals), utf8(env, drums), utf8(env, bass), utf8(env, other)};
    return jstr(env, sysex::asciiJson(gEngine->backingPlayer().loadStems(paths)));
}
JNI(void, nativeAudioSetStem)(JNIEnv*, jobject, jint stem, jfloat gain, jboolean mute, jboolean solo) {
    gEngine->backingPlayer().setStemMix(stem, gain, mute, solo);
}
JNI(jstring, nativeAudioStemState)(JNIEnv* env, jobject) {
    return jstr(env, sysex::asciiJson(gEngine->backingPlayer().stemStateJson()));
}
JNI(jstring, nativeStftExtract)(JNIEnv* env, jobject, jstring wav, jstring magnitudes, jint maxFrames) {
    return jstr(env, sysex::asciiJson(stftExtractMag(utf8(env, wav), utf8(env, magnitudes), maxFrames)));
}
JNI(jstring, nativeStftReconstruct)(JNIEnv* env, jobject, jstring wav, jobjectArray masks, jstring dir) {
    if (!masks || env->GetArrayLength(masks) != 4)
        return jsonError(env, "Expected four mask paths: vocals, drums, bass, other");
    std::string paths[4];
    for (jsize i = 0; i < 4; ++i) {
        const auto item = static_cast<jstring>(env->GetObjectArrayElement(masks, i));
        if (!item) return jsonError(env, "Every mask path must be a string");
        paths[i] = utf8(env, item);
        env->DeleteLocalRef(item);
    }
    return jstr(env, sysex::asciiJson(stftReconstructStems(utf8(env, wav), paths, utf8(env, dir))));
}
JNI(jstring, nativeSeparateStemsFast)(JNIEnv* env, jobject, jstring wav, jstring dir) {
    return jstr(env, sysex::asciiJson(separateStemsFastSpectral(utf8(env, wav), utf8(env, dir))));
}

JNI(jstring, nativeAudioLoad)(JNIEnv* env, jobject, jstring path) {
    return jstr(env, sysex::asciiJson(gEngine->backingPlayer().load(utf8(env, path))));
}
JNI(void, nativeAudioPlay)(JNIEnv*, jobject, jboolean on) { gEngine->backingPlayer().play(on); }
JNI(void, nativeAudioSeek)(JNIEnv*, jobject, jdouble sec) { gEngine->backingPlayer().seek(sec); }
JNI(jdoubleArray, nativeAudioState)(JNIEnv* env, jobject) {
    const auto state = gEngine->backingPlayer().state();
    auto array = env->NewDoubleArray(state.size());
    if (array) env->SetDoubleArrayRegion(array, 0, state.size(), state.data());
    return array;
}
JNI(void, nativeAudioSetLoop)(JNIEnv*, jobject, jdouble a, jdouble b, jboolean on) {
    gEngine->backingPlayer().setLoop(a, b, on);
}
JNI(void, nativeAudioSetVolume)(JNIEnv*, jobject, jfloat v) { gEngine->backingPlayer().setVolume(v); }
JNI(void, nativeAudioSetSpeed)(JNIEnv*, jobject, jfloat r) { gEngine->backingPlayer().setSpeed(r); }
JNI(void, nativeAudioSetPitch)(JNIEnv*, jobject, jfloat semis) { gEngine->backingPlayer().setPitch(semis); }
JNI(void, nativeAudioSetRoute)(JNIEnv*, jobject, jboolean direct) { gEngine->setBackingRoute(direct); }
JNI(jfloatArray, nativeAudioPeaks)(JNIEnv* env, jobject, jint bins) {
    return floats(env, gEngine->backingPlayer().peaks(bins));
}

JNI(jstring, nativePadLoad)(JNIEnv* env, jobject, jint pad, jstring path) {
    return jstr(env, sysex::asciiJson(gEngine->padSampler().load(pad, utf8(env, path))));
}
JNI(void, nativePadSetup)(JNIEnv*, jobject, jint pad, jfloat level, jfloat pitch, jint mode, jint choke,
                           jint velSens, jfloat start, jfloat end) {
    gEngine->padSampler().setup(pad, level, pitch, mode, choke, velSens, start, end);
}
JNI(void, nativePadTrigger)(JNIEnv*, jobject, jint pad, jfloat vel) { gEngine->padSampler().trigger(pad, vel); }
JNI(void, nativePadRelease)(JNIEnv*, jobject, jint pad) { gEngine->padSampler().release(pad); }
JNI(void, nativePadStopAll)(JNIEnv*, jobject) { gEngine->padSampler().stopAll(); }
JNI(void, nativePadClear)(JNIEnv*, jobject, jint pad) { gEngine->padSampler().clear(pad); }
JNI(jfloatArray, nativePadPeaks)(JNIEnv* env, jobject, jint pad, jint bins) {
    return floats(env, gEngine->padSampler().peaks(pad, bins));
}
JNI(void, nativePadLevels)(JNIEnv* env, jobject, jfloatArray out) {
    if (!out) return;
    const auto levels = gEngine->padSampler().levels();
    const int n = std::min(env->GetArrayLength(out), static_cast<jsize>(levels.size()));
    if (n) env->SetFloatArrayRegion(out, 0, n, levels.data());
}

JNI(jboolean, nativeSamplerStart)(JNIEnv*, jobject, jint source, jfloat maxSec, jfloat armDb) {
    if (source == 0 && (!gAudio || !gAudio->inputEnabled())) return JNI_FALSE;
    return gEngine->liveSampler().start(source, maxSec, armDb);
}
JNI(jdouble, nativeSamplerStop)(JNIEnv*, jobject) { return gEngine->liveSampler().stop(); }
JNI(jfloatArray, nativeSamplerState)(JNIEnv* env, jobject) {
    return floats(env, gEngine->liveSampler().state());
}
JNI(jfloatArray, nativeSamplerPeaks)(JNIEnv* env, jobject, jint bins) {
    return floats(env, gEngine->liveSampler().peaks(bins));
}
JNI(jfloatArray, nativeSamplerAnalyze)(JNIEnv* env, jobject) {
    return floats(env, gEngine->liveSampler().analyze());
}
JNI(void, nativeSamplerAudition)(JNIEnv*, jobject, jboolean on, jboolean loop, jint first, jint last) {
    gEngine->liveSampler().audition(on, loop, first, last);
}
JNI(jstring, nativeSamplerExport)(JNIEnv* env, jobject, jstring path, jint first, jint last, jfloat gainDb,
                                  jint loopFirst, jint loopLast, jfloat fadeMs, jint rootMidi) {
    return jstr(env, gEngine->liveSampler().exportWav(utf8(env, path), first, last, gainDb,
                                                    loopFirst, loopLast, fadeMs, rootMidi));
}
JNI(void, nativeSamplerDiscard)(JNIEnv*, jobject) { gEngine->liveSampler().discard(); }

JNI(jbyteArray, nativeExtMidiNext)(JNIEnv* env, jobject, jint timeoutMs) {
    const auto data = gEngine->externalMidi().next(timeoutMs);
    if (data.empty()) return nullptr;
    auto out = env->NewByteArray(data.size());
    if (out) env->SetByteArrayRegion(out, 0, data.size(), reinterpret_cast<const jbyte*>(data.data()));
    return out;
}
JNI(void, nativeExtMidiAllOff)(JNIEnv*, jobject, jint slot) {
    if (gAudio) gAudio->externalMidiAllOff(slot);
}
JNI(void, nativeExtSend)(JNIEnv* env, jobject, jint slot, jbyteArray bytes) {
    if (!bytes) return;
    const auto n = env->GetArrayLength(bytes);
    if (n == 0 || n > 12288) return;
    std::vector<uint8_t> data(n);
    env->GetByteArrayRegion(bytes, 0, n, reinterpret_cast<jbyte*>(data.data()));
    gEngine->externalMidi().send(slot, data.data(), data.size());
}

JNI(jstring, nativeRemoteHandle)(JNIEnv* env, jobject, jstring json) {
    auto* remote = gRemoteReady.load(std::memory_order_acquire);
    if (!remote) return jsonError(env, "Remote control is not initialized");
    try {
        Json request;
        std::string error;
        if (!Json::parse(utf8(env, json), request, error)) return jsonError(env, error);
        if (!request.isObject()) return jsonError(env, "Expected a JSON object");
        return jstr(env, sysex::asciiJson(remote->handle(request)));
    } catch (const std::exception& e) {
        return jsonError(env, e.what());
    }
}

// ------------------------------------------------------------ SysEx remote control

// Starts the remote control (once). presetDirs: ";"-separated.
JNI(void, nativeRemoteInit)(JNIEnv* env, jobject, jstring libRoot, jstring presetDirs, jstring userPresets, jstring midiDir,
                            jstring version) {
    std::lock_guard<std::mutex> init(gRemoteInitMutex);
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
    gRemoteReady.store(gRemote.get(), std::memory_order_release);
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
JNI(jint, nativeRemoteChanges)(JNIEnv*, jobject) {
    auto* remote = gRemoteReady.load(std::memory_order_acquire);
    return remote ? (jint)remote->changeCount() : 0;
}
// Songs loaded remotely: count, and the path of the last one.
JNI(jint, nativeRemoteSongCount)(JNIEnv*, jobject) { return (jint)gRemoteSongs.load(); }
JNI(jstring, nativeRemoteSongPath)(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lock(gSongMutex);
    return jstr(env, gRemoteSongPath);
}
