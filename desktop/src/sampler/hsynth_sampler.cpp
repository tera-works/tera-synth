// hsynth_sampler - automatic multisampler for HSynth test content.
//
// Plays every note / velocity of an instrument through a sound source, records
// it, trims it, finds seamless loops for sustained sounds, normalises the whole
// instrument and writes WAV files, an SFZ mapping and an HSynth preset.
//
//   hsynth_sampler sf2   --sf2 FILE [--set gm | --program BANK:PROG --name NAME [--title T]]
//   hsynth_sampler live  --midi-out PORT [--capture-device DEV] --program MSB:LSB:PROG --name NAME [--auto]
//   hsynth_sampler probe --midi-out PORT [--capture-device DEV] [--program MSB:LSB:PROG]
//   hsynth_sampler batch --midi-out PORT [--capture-device DEV] --ins FILE [--ins FILE] [--banks LIST]
//   hsynth_sampler list-midi | list-audio | list-sf2 --sf2 FILE
//
// batch samples every patch of the instrument definition (.ins) files: each
// patch is probed first (velocity layers, sustain / decay, release), drum kits
// get exactly the keys named in the file. Finished patches are skipped on the
// next run (resume), progress goes to <out>/catalog.csv.
//
// common options:
//   --out DIR        instrument folder root      (default desktop/data/library/user/gm)
//   --presets DIR    HSynth preset output         (default desktop/data/presets/user)
//   --library DIR    library root for presets     (default desktop/data/library)
//   --low N --high N --step N   sampled note range (default 36..96 step 3; batch 24..108 step 4)
//   --vels 50,110    velocity layers (live / sf2 without --auto)
//   --hold SEC --release SEC    note length / max release tail
//   --loop auto|on|off          --one-shot        --mono      --sr 48000 (live: 0 = device rate)
//   --decimate 2     store at half the capture rate (e.g. capture 96 kHz, store 48 kHz)
//   --channel 1 --drum-channel 10 --volume 100 --wet (keep reverb / chorus sends)
//
// Sampled material is for local testing only. Respect the license of the
// source (MuseScore_General.sf2: MIT; hardware / commercial sounds: check).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "hsynth/fileio.h"
#include "hsynth/fx/convolver.h"
#include "hsynth/wav.h"
#include "ins_file.h"
#include "sampler_core.h"
#include "sources.h"

using namespace sampler;
namespace fs = std::filesystem;

namespace {

struct Args {
    std::vector<std::string> v;
    bool has(const char* k) const {
        for (auto& s : v) if (s == k) return true;
        return false;
    }
    std::string get(const char* k, const std::string& def = "") const {
        for (size_t i = 0; i + 1 < v.size(); ++i) if (v[i] == k) return v[i + 1];
        return def;
    }
    std::vector<std::string> all(const char* k) const {
        std::vector<std::string> out;
        for (size_t i = 0; i + 1 < v.size(); ++i) if (v[i] == k) out.push_back(v[i + 1]);
        return out;
    }
};

std::vector<int> parseList(const std::string& s) {
    std::vector<int> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        const size_t dash = item.find('-');
        if (dash != std::string::npos && dash > 0) {
            const int a = std::atoi(item.substr(0, dash).c_str()), b = std::atoi(item.substr(dash + 1).c_str());
            for (int i = a; i <= b; ++i) out.push_back(i);
        } else {
            out.push_back(std::atoi(item.c_str()));
        }
    }
    return out;
}

// "MSB:LSB:PROG", "BANK:PROG" (bank = MSB*128+LSB) or "PROG" (1-based programs are not used: 0..127)
void parseProgram(const std::string& s, int& bank, int& prog) {
    std::vector<int> p;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ':')) p.push_back(std::atoi(item.c_str()));
    bank = 0;
    prog = 0;
    if (p.size() == 3) { bank = p[0] * 128 + p[1]; prog = p[2]; }
    else if (p.size() == 2) { bank = p[0]; prog = p[1]; }
    else if (p.size() == 1) prog = p[0];
}

std::string safe(const std::string& s) {
    std::string o;
    for (unsigned char c : s) o += (std::isalnum(c) || c == '-' || c == '.') ? (char)c : '_';
    while (o.find("__") != std::string::npos) o.replace(o.find("__"), 2, "_");
    while (!o.empty() && o.back() == '_') o.pop_back();
    return o.empty() ? "x" : o;
}

// "SonicCell PRESET A 1-128" -> "PRESET A"
std::string shortList(std::string s, const std::string& vendor) {
    if (!vendor.empty() && s.rfind(vendor + " ", 0) == 0) s = s.substr(vendor.size() + 1);
    const size_t r = s.find(" 1-");
    if (r != std::string::npos) s = s.substr(0, r);
    return s;
}

void usage() {
    std::printf(
        "usage:\n"
        "  hsynth_sampler sf2   --sf2 FILE --set gm [--only grand_piano,strings]\n"
        "  hsynth_sampler sf2   --sf2 FILE --program 0:48 --name strings [--title \"Strings\"] [--low 36 --high 96 --step 3]\n"
        "  hsynth_sampler live  --midi-out SonicCell --capture-device SonicCell --program 87:64:0 --name rich_grand [--auto]\n"
        "  hsynth_sampler probe --midi-out SonicCell --capture-device SonicCell [--program 87:64:0]\n"
        "  hsynth_sampler batch --midi-out SonicCell --capture-device SonicCell --ins Patch.ins --ins Rhythm.ins\n"
        "                       [--banks 15488,15360,11072,11200-11206] [--programs 0-127] [--max N] [--decimate 2]\n"
        "  hsynth_sampler list-midi | list-audio | list-sf2 --sf2 FILE\n"
        "options: --out DIR --presets DIR --library DIR --prefix \"SC \" --vels 50,110 --hold 3 --release 2\n"
        "         --loop auto|on|off --one-shot --mono --sr 48000 --channel 1 --drum-channel 10 --volume 100 --wet\n");
}

LiveConfig liveConfig(const Args& a) {
    LiveConfig c;
    c.midiOut = a.get("--midi-out", "0");
    c.channel = std::atoi(a.get("--channel", "1").c_str()) - 1;
    c.drumChannel = std::atoi(a.get("--drum-channel", "10").c_str()) - 1;
    c.capture = a.get("--capture", "input");
    c.captureDevice = a.get("--capture-device");
    c.sampleRate = std::atoi(a.get("--sr", "0").c_str());
    c.dry = !a.has("--wet");
    c.volume = std::atoi(a.get("--volume", "100").c_str());
    return c;
}

std::string nowText() {
    char b[32];
    std::time_t t = std::time(nullptr);
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return b;
}

// Energy above `hz` relative to the whole spectrum of a recording (dB).
double energyAboveDb(const Recording& r, int sr, double hz) {
    const int n = 32768;
    if ((int)r.left.size() < n + 4096) return -200;
    double hi = 0, all = 0;
    hsynth::fx::Fft fft;
    fft.init(n);
    for (size_t off = 2048; off + (size_t)n < r.left.size(); off += (size_t)n) {
        std::vector<float> re((size_t)n), im((size_t)n, 0.0f);
        for (int i = 0; i < n; ++i) re[(size_t)i] = r.left[off + (size_t)i] * (0.5f - 0.5f * std::cos(6.2831853f * i / (n - 1)));
        fft.forward(re.data(), im.data());
        for (int k = 1; k < n / 2; ++k) {
            const double e = (double)re[(size_t)k] * re[(size_t)k] + (double)im[(size_t)k] * im[(size_t)k];
            all += e;
            if ((double)k * sr / n > hz) hi += e;
        }
    }
    return 10.0 * std::log10((hi + 1e-30) / (all + 1e-30));
}


// ------------------------------------------------------------ batch jobs

struct Job {
    InsPatch patch;
    std::string density = "full";  // full: 4 semitones, <= 3 layers; lite: 6 semitones, <= 2 layers, shorter holds
    std::string group;             // priority group label (order file)
};

// Job list from --order FILE ("msb lsb program(0-127) density group..." per line,
// '#' comments) or from --banks / all patches of the .ins files.
bool buildJobs(const Args& a, InsFile& ins, std::vector<Job>& jobs, std::string& err) {
    for (auto& f : a.all("--ins"))
        if (!readIns(f, ins, err)) return false;
    if (ins.patches.empty()) { err = "no patches in the .ins files (--ins)"; return false; }
    const std::string density = a.get("--density", "full");
    if (a.has("--order")) {
        std::ifstream f(hsynth::utf8Path(a.get("--order")), std::ios::binary);
        if (!f) { err = "cannot open order file " + a.get("--order"); return false; }
        std::string line;
        int missing = 0;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            std::stringstream ss(line);
            int msb = -1, lsb = -1, pc = -1;
            Job j;
            ss >> msb >> lsb >> pc >> j.density;
            std::getline(ss, j.group);
            while (!j.group.empty() && (j.group[0] == ' ' || j.group[0] == '\t')) j.group.erase(0, 1);
            const int bank = msb * 128 + lsb;
            bool found = false;
            for (auto& p : ins.patches)
                if (p.bank == bank && p.program == pc) { j.patch = p; found = true; break; }
            if (found) jobs.push_back(std::move(j));
            else ++missing;
        }
        if (missing) std::fprintf(stderr, "warning: %d order entries are not in the .ins files\n", missing);
    } else {
        std::vector<InsPatch> sel;
        if (a.has("--banks")) {
            for (int bank : parseList(a.get("--banks")))
                for (auto& p : ins.patches) if (p.bank == bank) sel.push_back(p);
        } else {
            sel = ins.patches;
        }
        for (auto& p : sel) {
            Job j;
            j.patch = p;
            j.density = density;
            j.group = shortList(p.list, a.get("--vendor", "SonicCell"));
            jobs.push_back(std::move(j));
        }
    }
    if (a.has("--programs")) {
        const auto progs = parseList(a.get("--programs"));
        jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [&](const Job& j) {
            return std::find(progs.begin(), progs.end(), j.patch.program) == progs.end(); }), jobs.end());
    }
    if (a.has("--max") && (int)jobs.size() > std::atoi(a.get("--max").c_str())) jobs.resize((size_t)std::atoi(a.get("--max").c_str()));
    if (jobs.empty()) { err = "nothing to sample"; return false; }
    return true;
}

// Where batch writes a patch's SFZ (relative to --out); its presence marks the patch done.
fs::path sfzPathOf(const InsPatch& p, const std::string& vendor) {
    char num[16];
    std::snprintf(num, sizeof num, "%03d", p.program + 1);
    const std::string leaf = std::string(num) + "_" + safe(p.name);
    return hsynth::utf8Path(safe(shortList(p.list, vendor))) / hsynth::utf8Path(leaf) / hsynth::utf8Path(leaf + ".sfz");
}

std::atomic<bool> g_stop{false};
BOOL WINAPI onConsoleCtrl(DWORD type) {
    g_stop = true;
    // closing the window: give the current note a moment to end cleanly
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) Sleep(4000);
    return TRUE;
}
void installStopHandler() { SetConsoleCtrlHandler(onConsoleCtrl, TRUE); }

std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> f;
    std::stringstream ss(line);
    std::string x;
    while (std::getline(ss, x, ',')) f.push_back(x);
    return f;
}

// Progress per priority group and the estimated remaining time (no hardware needed).
int printStatus(const Args& a) {
    InsFile ins;
    std::vector<Job> jobs;
    std::string err;
    if (!buildJobs(a, ins, jobs, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    const std::string vendor = a.get("--vendor", "SonicCell");
    const fs::path out = hsynth::utf8Path(a.get("--out"));
    // average time per patch by density, from the catalog
    double sum[2] = {0, 0};
    int cnt[2] = {0, 0};
    double mb = 0, audio = 0;
    std::string lastTime;
    {
        std::ifstream f(out / "catalog.csv", std::ios::binary);
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto c = splitCsv(line);
            if (c.size() < 18) continue;
            lastTime = c[0];
            if (c[7] == "failed" || c[7] == "silent") continue;
            const int d = c.size() > 18 && c[18] == "lite" ? 1 : 0;
            sum[d] += std::atof(c[16].c_str());
            ++cnt[d];
            mb += std::atof(c[14].c_str());
            audio += std::atof(c[13].c_str());
        }
    }
    const double avg[2] = {cnt[0] ? sum[0] / cnt[0] : 360.0, cnt[1] ? sum[1] / cnt[1] : 150.0};
    std::vector<std::string> order;
    std::map<std::string, std::pair<int, int>> groups;  // done, total
    int done = 0;
    double remaining = 0;
    for (auto& j : jobs) {
        if (!groups.count(j.group)) order.push_back(j.group);
        auto& g = groups[j.group];
        ++g.second;
        if (fs::exists(out / sfzPathOf(j.patch, vendor))) { ++g.first; ++done; }
        else remaining += avg[j.density == "lite" ? 1 : 0];
    }
    std::printf("SonicCell capture: %d / %zu patches done (%.0f %%), %.1f GB\n", done, jobs.size(), 100.0 * done / jobs.size(), mb / 1024.0);
    for (auto& g : order) {
        const auto& v = groups[g];
        std::printf("  %-34s %4d / %-4d %s\n", g.c_str(), v.first, v.second, v.first == v.second ? "done" : v.first ? "in progress" : "");
    }
    std::printf("measured per patch: full %.1f min (%d), lite %.1f min (%d)\n", avg[0] / 60, cnt[0], avg[1] / 60, cnt[1]);
    std::printf("estimated remaining time: %dh %02dm\n", (int)(remaining / 3600), (int)remaining % 3600 / 60);
    if (!lastTime.empty()) std::printf("last patch finished: %s\n", lastTime.c_str());
    if (fs::exists(out / "STOP")) std::printf("STOP requested (soniccell_stop.bat); start again with soniccell_start.bat\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Args a;
    for (int i = 1; i < argc; ++i) a.v.push_back(argv[i]);
    if (a.v.empty()) { usage(); return 2; }
    const std::string cmd = a.v[0];

    if (cmd == "list-midi") {
        auto outs = listMidiOutputs();
        for (size_t i = 0; i < outs.size(); ++i) std::printf("%zu: %s\n", i, outs[i].c_str());
        if (outs.empty()) std::printf("no MIDI outputs\n");
        return 0;
    }
    if (cmd == "list-audio") {
        auto ins = listCaptureDevices();
        for (size_t i = 0; i < ins.size(); ++i) std::printf("%zu: %s\n", i, ins[i].c_str());
        if (ins.empty()) std::printf("no recording devices\n");
        return 0;
    }
    if (cmd == "ins-list") {  // what batch would sample (no hardware needed)
        InsFile ins;
        std::string e;
        for (auto& f : a.all("--ins"))
            if (!readIns(f, ins, e)) { std::fprintf(stderr, "error: %s\n", e.c_str()); return 1; }
        std::map<std::string, int> perList;
        int keys = 0;
        for (auto& p : ins.patches) {
            ++perList[p.list];
            keys += (int)p.keys.size();
            if (a.has("--verbose"))
                std::printf("MSB %3d LSB %3d PC %3d  %-28s %s%s\n", p.bank >> 7, p.bank & 127, p.program + 1, p.list.c_str(), p.name.c_str(),
                            p.drum ? (" [kit, " + std::to_string(p.keys.size()) + " keys]").c_str() : "");
        }
        for (auto& [l, n] : perList) std::printf("%-36s %4d\n", l.c_str(), n);
        std::printf("total %zu patches / kits, %d named drum keys\n", ins.patches.size(), keys);
        return 0;
    }
    if (cmd == "status") return printStatus(a);
    if (cmd == "list-sf2") {
        for (auto& n : listSf2Presets(a.get("--sf2"))) std::printf("%s\n", n.c_str());
        return 0;
    }

    const std::string root = HSYNTH_DESKTOP_DIR;
    Options opt;
    opt.outDir = a.get("--out", root + "/data/library/user/gm");
    opt.presetDir = a.get("--presets", root + "/data/presets/user");
    opt.libraryRoot = a.get("--library", root + "/data/library");
    opt.keepStereo = !a.has("--mono");
    opt.decimate = std::atoi(a.get("--decimate", "1").c_str());
    if (a.has("--prefix")) opt.presetPrefix = a.get("--prefix");

    std::string err;
    std::unique_ptr<Source> src;
    if (cmd == "sf2") {
        const std::string sf2 = a.get("--sf2");
        if (sf2.empty()) { usage(); return 2; }
        std::printf("loading %s ...\n", sf2.c_str());
        src = makeSf2Source(sf2, std::atoi(a.get("--sr", "48000").c_str()), err);
    } else if (cmd == "live" || cmd == "probe" || cmd == "batch" || cmd == "capture") {
        src = makeLiveSource(liveConfig(a), err);
        if (src) std::printf("%s\n", describeLiveSource(*src).c_str());
    } else {
        usage();
        return 2;
    }
    if (!src) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }

    // ------------------------------------------------------------ raw capture (noise analysis)
    if (cmd == "capture") {
        Recording r;
        const double sec = std::atof(a.get("--seconds", "3").c_str());
        if (!captureRaw(*src, sec, r)) { std::fprintf(stderr, "capture needs a live source\n"); return 1; }
        const std::string out = a.get("--wav", "capture.wav");
        if (!hsynth::writeWav(out, r.left, &r.right, src->sampleRate(), true, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        std::printf("wrote %zu frames to %s\n", r.left.size(), out.c_str());
        return 0;
    }

    // ------------------------------------------------------------ probe
    if (cmd == "probe") {
        InstrumentSpec s;
        parseProgram(a.get("--program", "0"), s.bank, s.program);
        s.drum = a.has("--drum");
        if (s.drum) for (int k = 35; k <= 81; ++k) s.keys.push_back(k);
        if (!src->selectProgram(s.bank, s.program, s.drum, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        // a bright, sustained test note for level / bandwidth
        NoteRequest q;
        q.note = s.drum ? 42 : 72;
        q.velocity = 127;
        q.holdSec = 2.0;
        q.maxReleaseSec = 1.0;
        Recording r;
        if (!src->record(q, r, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        float pk = 0;
        for (size_t i = 0; i < r.left.size(); ++i) pk = std::max(pk, std::max(std::fabs(r.left[i]), std::fabs(r.right[i])));
        const int sr = src->sampleRate();
        std::printf("test note: peak %.1f dBFS%s, signal-to-noise %.1f dB\n", 20 * std::log10(pk + 1e-9), r.clipped ? " (CLIPPED)" : "",
                    20 * std::log10((pk + 1e-9) / (src->noisePeak() + 1e-9)));
        for (double hz : {16000.0, 20000.0, 22050.0, 24000.0, 30000.0})
            if (hz < sr / 2.0) std::printf("  energy above %5.0f Hz: %6.1f dB\n", hz, energyAboveDb(r, sr, hz));
        ProbeReport rep;
        if (!probeInstrument(*src, s, rep, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        std::printf("probe: %s\n", rep.summary.c_str());
        return 0;
    }

    // ------------------------------------------------------------ batch
    if (cmd == "batch") {
        InsFile ins;
        std::vector<Job> jobs;
        if (!buildJobs(a, ins, jobs, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        const std::string vendor = a.get("--vendor", "SonicCell");
        fs::create_directories(hsynth::utf8Path(opt.outDir));
        const fs::path outDir = hsynth::utf8Path(opt.outDir);
        const fs::path stopFile = outDir / "STOP";
        const fs::path catalog = outDir / "catalog.csv";
        const bool newCatalog = !fs::exists(catalog) || fs::file_size(catalog) == 0;
        std::ofstream cat(catalog, std::ios::app | std::ios::binary);
        if (newCatalog)
            cat << "time,msb,lsb,program,bank_list,name,folder,type,layers,amp_veltrack,samples,loops,clipped,audio_s,mb,gain_db,elapsed_s,probe,density,group\n";
        std::ofstream logFile;
        if (a.has("--log")) logFile.open(hsynth::utf8Path(a.get("--log")), std::ios::app | std::ios::binary);
        auto log = [&](const std::string& s) {
            std::printf("%s\n", s.c_str());
            if (logFile) { logFile << nowText() << "  " << s << "\n"; logFile.flush(); }
        };
        // graceful stop: STOP file (soniccell_stop.bat) or Ctrl+C / closing the window
        installStopHandler();
        auto stopNow = [&]() { return g_stop.load() || fs::exists(stopFile); };
        opt.stopRequested = stopNow;

        const int low = std::atoi(a.get("--low", "24").c_str()), high = std::atoi(a.get("--high", "108").c_str());
        size_t pending = 0;
        for (auto& j : jobs) if (!fs::exists(outDir / sfzPathOf(j.patch, vendor))) ++pending;
        log("batch: " + std::to_string(jobs.size()) + " patches, " + std::to_string(pending) + " still to sample -> " + opt.outDir);
        const auto batchStart = std::chrono::steady_clock::now();
        int done = 0, failed = 0, silent = 0;
        size_t left = pending;
        double spent = 0;
        bool stopped = false;
        for (size_t j = 0; j < jobs.size(); ++j) {
            if (stopNow()) { stopped = true; break; }
            const Job& job = jobs[j];
            const InsPatch& p = job.patch;
            if (fs::exists(outDir / sfzPathOf(p, vendor))) continue;
            const bool lite = job.density == "lite";
            InstrumentSpec s;
            const std::string list = shortList(p.list, vendor);
            char num[16];
            std::snprintf(num, sizeof num, "%03d", p.program + 1);
            s.name = safe(list) + "/" + num + "_" + safe(p.name);
            s.title = list + " " + num + " " + p.name;
            s.category = vendor;
            s.bank = p.bank;
            s.program = p.program;
            s.drum = p.drum;
            if (p.drum) {
                for (auto& [k, n] : p.keys) { s.keys.push_back(k); s.keyNames.push_back(n); }
                if (s.keys.empty()) for (int k = 35; k <= 81; ++k) s.keys.push_back(k);
                s.presetRelease = 0.05f;
                s.presetReverb = 0.08f;
            } else {
                s.lowNote = low;
                s.highNote = high;
                s.step = lite ? 6 : std::max(1, std::atoi(a.get("--step", "4").c_str()));
            }
            const double eta = done > 0 ? spent / done * (double)left : 0;
            char head[512];
            std::snprintf(head, sizeof head, "[%zu/%zu] %s  (MSB %d LSB %d PC %d, %s, %s)%s", j + 1, jobs.size(), s.title.c_str(), p.bank >> 7,
                          p.bank & 127, p.program + 1, job.density.c_str(), job.group.c_str(),
                          done > 0 ? ("  remaining " + std::to_string(left) + ", ETA " + std::to_string((int)(eta / 3600)) + "h" +
                                      std::to_string((int)eta % 3600 / 60) + "m").c_str() : "");
            log(head);
            const auto t0 = std::chrono::steady_clock::now();
            ProbeReport rep;
            InstrumentResult res;
            bool ok = probeInstrument(*src, s, rep, err);
            if (ok) log("  probe: " + rep.summary);
            const int maxLayers = lite ? 2 : std::atoi(a.get("--max-layers", "3").c_str());
            if ((int)s.velocities.size() > maxLayers)
                s.velocities = maxLayers <= 1 ? std::vector<int>{100} : std::vector<int>{64, 120};
            if (lite && !s.drum && rep.sustained) s.holdSec = 2.0;
            if (lite) s.maxReleaseSec = std::min(s.maxReleaseSec, 3.0);
            std::string type = "failed";
            if (ok && rep.silent) { type = "silent"; ++silent; }
            else if (ok && sampleInstrument(*src, s, opt, err, &res)) type = s.drum ? "kit" : rep.sustained ? "sustained" : "decaying";
            else if (err == kStoppedError) { stopped = true; break; }
            else { log("  error: " + err); ++failed; }
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (type != "failed") { spent += el; ++done; --left; }
            if (type != "failed" && type != "silent") {
                char r[256];
                std::snprintf(r, sizeof r, "  done: %d samples (%d looped), %.0f MB, %.0f s", res.samples, res.loops, res.megabytes, el);
                log(r);
            }
            std::string sum = rep.summary;
            std::replace(sum.begin(), sum.end(), ',', ';');
            std::string name = p.name;
            std::replace(name.begin(), name.end(), ',', ' ');
            char line[1200];
            std::snprintf(line, sizeof line, "%s,%d,%d,%d,%s,%s,%s,%s,%zu,%d,%d,%d,%d,%.1f,%.1f,%.1f,%.0f,%s,%s,%s\n", nowText().c_str(),
                          p.bank >> 7, p.bank & 127, p.program + 1, list.c_str(), name.c_str(), s.name.c_str(), type.c_str(),
                          s.velocities.size(), s.ampVeltrack, res.samples, res.loops, res.clipped, res.seconds, res.megabytes,
                          res.gainDb, el, sum.c_str(), job.density.c_str(), job.group.c_str());
            cat << line;
            cat.flush();
        }
        const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - batchStart).count();
        char fin[256];
        std::snprintf(fin, sizeof fin, "%s: %d sampled, %d silent, %d failed in %.1f h; %zu still to sample", stopped ? "STOPPED" : "batch done",
                      done - silent, silent, failed, total / 3600.0, left);
        log(fin);
        if (stopped) log("resume: run soniccell_start.bat (finished patches are skipped)");
        return failed ? 1 : 0;
    }

    // ------------------------------------------------------------ single instrument / gm set
    std::vector<InstrumentSpec> specs;
    if (a.get("--set") == "gm") {
        specs = gmTestSet();
        const auto only = a.get("--only");
        if (!only.empty()) {
            std::vector<InstrumentSpec> f;
            for (auto& s : specs) if (("," + only + ",").find("," + s.name + ",") != std::string::npos) f.push_back(s);
            specs = f;
        }
    } else {
        InstrumentSpec s;
        parseProgram(a.get("--program", "0:0"), s.bank, s.program);
        s.drum = a.has("--drum");
        s.name = a.get("--name", "instrument");
        s.title = a.get("--title", s.name);
        s.lowNote = std::atoi(a.get("--low", "36").c_str());
        s.highNote = std::atoi(a.get("--high", "96").c_str());
        s.step = std::max(1, std::atoi(a.get("--step", "3").c_str()));
        if (s.drum) for (int k = s.lowNote; k <= s.highNote; ++k) s.keys.push_back(k);
        if (a.has("--vels")) s.velocities = parseList(a.get("--vels"));
        s.holdSec = std::atof(a.get("--hold", "3").c_str());
        s.maxReleaseSec = std::atof(a.get("--release", "2").c_str());
        const std::string loop = a.get("--loop", "auto");
        s.loop = loop == "on" ? InstrumentSpec::Loop::On : loop == "off" ? InstrumentSpec::Loop::Off : InstrumentSpec::Loop::Auto;
        s.oneShot = a.has("--one-shot") || s.drum;
        if (a.has("--auto")) {
            ProbeReport rep;
            if (!probeInstrument(*src, s, rep, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
            std::printf("probe: %s\n", rep.summary.c_str());
        }
        specs.push_back(s);
    }
    if (specs.empty()) { std::fprintf(stderr, "nothing to sample\n"); return 1; }

    int failed = 0;
    for (auto& s : specs) {
        std::printf("[%s] bank %d program %d, notes %d..%d step %d, %zu velocity layer(s)\n", s.title.c_str(), s.bank, s.program,
                    s.keys.empty() ? s.lowNote : s.keys.front(), s.keys.empty() ? s.highNote : s.keys.back(), s.keys.empty() ? s.step : 1,
                    s.velocities.size());
        if (!sampleInstrument(*src, s, opt, err)) {
            std::fprintf(stderr, "  error: %s\n", err.c_str());
            ++failed;
        }
    }
    std::printf("done: %zu instrument(s), %d failed\noutput: %s\npresets: %s\n", specs.size(), failed, opt.outDir.c_str(),
                opt.presetDir.c_str());
    return failed ? 1 : 0;
}
