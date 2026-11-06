#include "midi_out.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <chrono>

namespace {
std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)std::max(0, n - 1), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
}  // namespace

MidiOut::~MidiOut() {
    setClock(false, nullptr);
    close();
}

std::vector<std::string> MidiOut::ports() {
    std::vector<std::string> out;
    const UINT n = midiOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIOUTCAPSW caps{};
        if (midiOutGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR) out.push_back(narrow(caps.szPname));
    }
    return out;
}

bool MidiOut::open(const std::string& name, std::string& error) {
    close();
    if (name.empty()) return true;
    const auto list = ports();
    for (UINT i = 0; i < (UINT)list.size(); ++i) {
        if (list[i] != name) continue;
        HMIDIOUT h = nullptr;
        if (midiOutOpen(&h, i, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            error = "cannot open MIDI output " + name;
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        handle_ = h;
        name_ = name;
        return true;
    }
    error = "MIDI output not found: " + name;
    return false;
}

void MidiOut::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle_) {
        midiOutReset((HMIDIOUT)handle_);
        midiOutClose((HMIDIOUT)handle_);
    }
    handle_ = nullptr;
    name_.clear();
}

void MidiOut::send(uint8_t status, uint8_t d1, uint8_t d2) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle_) midiOutShortMsg((HMIDIOUT)handle_, (DWORD)status | ((DWORD)d1 << 8) | ((DWORD)d2 << 16));
}

void MidiOut::sendRealtime(uint8_t status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle_) midiOutShortMsg((HMIDIOUT)handle_, (DWORD)status);
}

void MidiOut::setClock(bool on, std::function<float()> tempo) {
    if (clockRun_.load()) {
        clockRun_.store(false);
        if (clock_.joinable()) clock_.join();
    }
    if (!on || !tempo) return;
    clockRun_.store(true);
    clock_ = std::thread([this, tempo] {
        timeBeginPeriod(1);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        auto next = std::chrono::steady_clock::now();
        while (clockRun_.load()) {
            const double bpm = std::max(20.0f, std::min(300.0f, tempo()));
            next += std::chrono::nanoseconds((long long)(60.0e9 / (bpm * 24.0)));
            // sleep most of the way, then spin for a precise tick
            std::this_thread::sleep_until(next - std::chrono::milliseconds(1));
            while (std::chrono::steady_clock::now() < next) std::this_thread::yield();
            sendRealtime(0xF8);
        }
        timeEndPeriod(1);
    });
}
