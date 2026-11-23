#include "hsynth/formats/dls_gig.h"

#include "hsynth/fileio.h"

#include "../pcm_file.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

namespace hsynth {
namespace {

constexpr uint32_t fourcc(const char (&s)[5]) {
    return uint32_t(s[0]) | uint32_t(s[1]) << 8 | uint32_t(s[2]) << 16 | uint32_t(s[3]) << 24;
}

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | uint16_t(p[1]) << 8); }
uint32_t le32(const uint8_t* p) { return le16(p) | uint32_t(le16(p + 2)) << 16; }
int32_t signed16(const uint8_t* p) { return int32_t(le16(p) ^ 0x8000u) - 0x8000; }
int32_t signed32(const uint8_t* p) {
    return int32_t(int64_t(le32(p) ^ 0x80000000u) - 0x80000000ll);
}

constexpr uint32_t kList = fourcc("LIST");
constexpr uint32_t kRiff = fourcc("RIFF");
constexpr uint32_t kNoWave = 0xffffffffu;
constexpr size_t kMaxChunks = 1u << 19;
constexpr size_t kMaxInstruments = 1u << 14;
constexpr size_t kMaxRegions = 1u << 15;
constexpr uint64_t kResidentLimit = 256ull << 20;
constexpr int kMipPadFrames = 8;  // resample::decimate2
constexpr int kMaxExtension = 99;

struct ParseError : std::runtime_error { using std::runtime_error::runtime_error; };

struct Chunk {
    uint32_t id = 0;
    uint32_t type = 0;
    uint64_t body = 0;
    uint64_t end = 0;

    explicit operator bool() const { return id != 0; }
    uint64_t size() const { return end - body; }
    bool isList(uint32_t listType) const { return id == kList && type == listType; }
};

using Chunks = std::vector<Chunk>;

Chunk find(const Chunks& chunks, uint32_t id, uint32_t type = 0) {
    const auto it = std::find_if(chunks.begin(), chunks.end(), [=](const Chunk& c) {
        return c.id == id && (id != kList || c.type == type);
    });
    return it == chunks.end() ? Chunk{} : *it;
}

bool hasGigMarker(const Chunks& chunks) {
    for (const Chunk& c : chunks) {
        const uint32_t tag = c.id == kList ? c.type : c.id;
        switch (tag) {
            case fourcc("3gix"): case fourcc("3ewg"): case fourcc("3gnl"): case fourcc("3gri"):
            case fourcc("3ewa"): case fourcc("3dnm"): case fourcc("3ddp"): case fourcc("3lnk"):
            case fourcc("3prg"): case fourcc("ewav"): case fourcc("xfil"):
                return true;
        }
    }
    return false;
}

class RiffFile {
public:
    RiffFile(std::string path, size_t& visited)
        : path_(std::move(path)), file_(fopenUtf8(path_, "rb"), &std::fclose), visited_(visited) {
        if (!file_) return;
        if (!seekFile(file_.get(), 0, SEEK_END)) throw ParseError("Cannot seek " + path_);
        const int64_t length = tellFile(file_.get());
        if (length < 0) throw ParseError("Cannot size " + path_);
        size_ = uint64_t(length);
    }

    FILE* handle() const { return file_.get(); }
    const std::string& path() const { return path_; }
    uint64_t size() const { return size_; }

    Chunk root() const {
        const Chunk c = header(0, size_);
        if (c.id != kRiff) throw ParseError("Expected a RIFF file in " + path_);
        return c;
    }

    Chunks children(const Chunk& list) const {
        Chunks result;
        for (uint64_t pos = list.body; pos < list.end;) {
            const Chunk child = header(pos, list.end);
            result.push_back(child);
            pos = child.end + (child.end & 1u);
        }
        return result;
    }

    void read(const Chunk& c, uint64_t offset, uint8_t* dst, size_t size) const {
        if (offset > c.size() || size > c.size() - offset)
            throw ParseError("Truncated DLS/GIG chunk in " + path_);
        readAt(c.body + offset, dst, size);
    }

    template<size_t N>
    std::array<uint8_t, N> read(const Chunk& c, uint64_t offset = 0) const {
        std::array<uint8_t, N> bytes;
        read(c, offset, bytes.data(), N);
        return bytes;
    }

private:
    void readAt(uint64_t pos, uint8_t* dst, size_t bytes) const {
        if (pos > size_ || bytes > size_ - pos || !seekFile(file_.get(), int64_t(pos), SEEK_SET) ||
            std::fread(dst, 1, bytes, file_.get()) != bytes)
            throw ParseError("Cannot read RIFF data in " + path_);
    }

    Chunk header(uint64_t pos, uint64_t limit) const {
        if (++visited_ > kMaxChunks) throw ParseError("DLS/GIG chunk limit exceeded");
        uint8_t bytes[8];
        if (pos > limit || sizeof bytes > limit - pos)
            throw ParseError("Truncated RIFF chunk header in " + path_);
        readAt(pos, bytes, sizeof bytes);
        Chunk c;
        c.id = le32(bytes);
        c.body = pos + sizeof bytes;
        const uint64_t payload = le32(bytes + 4);
        if (payload + (payload & 1u) > limit - c.body)
            throw ParseError("RIFF chunk extends past its container in " + path_);
        c.end = c.body + payload;
        if (c.id == kList || c.id == kRiff) {
            c.type = le32(read<4>(c).data());
            c.body += 4;
        }
        return c;
    }

    std::string path_;
    std::unique_ptr<FILE, int (*)(FILE*)> file_;
    size_t& visited_;
    uint64_t size_ = 0;
};

struct Instrument {
    DlsPresetInfo info;
    Chunk list;
};

struct WaveOptions {
    int unity = 60;
    float tune = 0.0f;
    float gainDb = 0.0f;
    LoopMode loopMode = LoopMode::NoLoop;
    int64_t loopStart = -1;
    int64_t loopEnd = -1;
};

WaveOptions readWsmp(const RiffFile& file, const Chunk& chunk, bool gig) {
    const auto bytes = file.read<20>(chunk);
    WaveOptions wave;
    const int unity = le16(bytes.data() + 4);
    if (unity <= 127) wave.unity = unity;
    wave.tune = float(signed16(bytes.data() + 6));
    wave.gainDb = float(signed32(bytes.data() + 8) / (gig ? -655360.0 : 655360.0));
    uint64_t pos = le32(bytes.data());
    const uint32_t count = le32(bytes.data() + 16);
    if (pos < bytes.size() || pos > chunk.size() || count > (chunk.size() - pos) / 16 || count > 256)
        throw ParseError("Invalid wsmp loop table");
    for (uint32_t i = 0; i < count; ++i) {
        const auto loop = file.read<16>(chunk, pos);
        const uint32_t size = le32(loop.data());
        const uint32_t type = le32(loop.data() + 4);
        const uint32_t length = le32(loop.data() + 12);
        if (size < loop.size() || size > chunk.size() - pos) throw ParseError("Invalid wsmp loop size");
        if (wave.loopMode == LoopMode::NoLoop && type <= 1 && length > 1) {
            wave.loopMode = type == 0 ? LoopMode::Continuous : LoopMode::Sustain;
            wave.loopStart = le32(loop.data() + 8);
            wave.loopEnd = wave.loopStart + length;
        }
        pos += size;
    }
    return wave;
}

WaveOptions readSmpl(const RiffFile& file, const Chunk& chunk) {
    const auto bytes = file.read<36>(chunk);
    WaveOptions wave;
    const uint32_t unity = le32(bytes.data() + 12);
    if (unity <= 127) wave.unity = int(unity);
    wave.tune = float(le32(bytes.data() + 16) * (100.0 / 4294967296.0));
    const uint32_t count = le32(bytes.data() + 28);
    if (count > (chunk.size() - bytes.size()) / 24) throw ParseError("Invalid smpl loop table");
    if (count) {
        const auto loop = file.read<24>(chunk, bytes.size());
        if (le32(loop.data() + 4) == 0) {
            wave.loopStart = le32(loop.data() + 8);
            wave.loopEnd = int64_t(le32(loop.data() + 12)) + 1;
            if (wave.loopEnd > wave.loopStart + 1) wave.loopMode = LoopMode::Continuous;
        }
    }
    return wave;
}

struct Wave {
    const RiffFile* file = nullptr;
    StreamSource pcm;
    double rate = 0.0;
    int64_t frames = 0;
    WaveOptions options;
    bool gig = false;
    bool compressed = false;
    bool keepResident = false;
    int mips = 0;
    std::vector<size_t> regions;
};

bool readWave(const RiffFile& file, const Chunk& list, bool gig, Wave& wave) {
    const Chunks chunks = file.children(list);
    wave.gig = gig || hasGigMarker(chunks);
    wave.compressed = bool(find(chunks, fourcc("ewav")));
    if (wave.compressed) return false;
    const Chunk fmt = find(chunks, fourcc("fmt "));
    const Chunk data = find(chunks, fourcc("data"));
    if (!fmt || !data) return false;
    const auto format = file.read<16>(fmt);
    wave.file = &file;
    wave.pcm.path = file.path();
    wave.pcm.dataOffset = int64_t(data.body);
    wave.pcm.format = le16(format.data());
    wave.pcm.fileChannels = le16(format.data() + 2);
    wave.rate = le32(format.data() + 4);
    wave.pcm.blockAlign = le16(format.data() + 12);
    wave.pcm.bits = le16(format.data() + 14);
    if (wave.pcm.format == 0xfffe && fmt.size() >= 40) {
        const auto extended = file.read<24>(fmt, 16);
        constexpr std::array<uint8_t, 16> pcmGuid{1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
        if (le16(extended.data()) >= 22 && uint64_t(le16(extended.data())) + 18 <= fmt.size() &&
            std::equal(pcmGuid.begin(), pcmGuid.end(), extended.begin() + 8))
            wave.pcm.format = 1;
    }
    const int bits = wave.pcm.bits;
    const int channels = wave.pcm.fileChannels;
    if (wave.pcm.format != 1 || (bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
        channels < 1 || channels > 2 || wave.rate <= 0 || wave.rate > 768000 ||
        wave.pcm.blockAlign != channels * (bits / 8) || data.size() % wave.pcm.blockAlign != 0)
        return false;
    wave.frames = int64_t(data.size() / wave.pcm.blockAlign);
    if (const Chunk wsmp = find(chunks, fourcc("wsmp"))) wave.options = readWsmp(file, wsmp, wave.gig);
    else if (const Chunk smpl = find(chunks, fourcc("smpl"))) wave.options = readSmpl(file, smpl);
    return wave.frames > 0;
}

struct PoolCue {
    uint32_t high = 0;
    uint32_t offset = 0;
};

std::vector<PoolCue> readCues(const RiffFile& file, const Chunk& table) {
    if (!table) return {};
    const auto bytes = file.read<8>(table);
    const uint32_t header = le32(bytes.data());
    const uint32_t count = le32(bytes.data() + 4);
    if (header < bytes.size() || header > table.size() || count > kMaxChunks)
        throw ParseError("Invalid ptbl header");
    const uint64_t remaining = table.size() - header;
    const unsigned width = remaining == uint64_t(count) * 8 ? 8 : 4;
    if (count > remaining / width) throw ParseError("Truncated ptbl cues");
    std::vector<uint8_t> raw(size_t(count) * width);
    if (!raw.empty()) file.read(table, header, raw.data(), raw.size());
    std::vector<PoolCue> cues(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* entry = raw.data() + i * width;
        if (width == 8) cues[i].high = le32(entry);
        cues[i].offset = le32(entry + width - 4);
    }
    return cues;
}

struct Pool {
    Chunk list;
    Chunks waves;

    void index(const RiffFile& file) {
        if (!list) return;
        for (const Chunk& c : file.children(list)) {
            if (c.isList(fourcc("wave"))) waves.push_back(c);
        }
    }

    Chunk at(uint64_t offset) const {
        const auto it = std::lower_bound(waves.begin(), waves.end(), offset, [](const Chunk& c, uint64_t pos) {
            return c.body - 12 < pos;
        });
        return it != waves.end() && it->body - 12 == offset ? *it : Chunk{};
    }
};

struct Extension {
    Extension(std::string path, size_t& visited) : file(std::move(path), visited) {}
    RiffFile file;
    Pool pool;
};

struct WaveRef {
    const RiffFile* file = nullptr;
    Chunk chunk;
    int part = 0;
};

class Bank {
public:
    explicit Bank(const std::string& path) : file(path, visited_) {
        if (!file.handle()) throw ParseError("Cannot open " + path);
        const Chunk root = file.root();
        if (root.type != fourcc("DLS ")) throw ParseError("Expected RIFF DLS form in " + path);
        top_ = file.children(root);
        primary_.list = find(top_, kList, fourcc("wvpl"));
        const auto utf8Ext = utf8Path(path).extension().u8string();
        std::string ext(utf8Ext.begin(), utf8Ext.end());
        for (auto& c : ext) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        gig = ext == ".gig" || hasGigMarker(top_);
        if (const Chunk version = find(top_, fourcc("vers")); version.size() >= 8)
            major = int(le16(file.read<4>(version).data() + 2));
    }

    std::vector<Instrument> instruments() const {
        std::vector<Instrument> result;
        int index = 0;
        const Chunk lins = find(top_, kList, fourcc("lins"));
        for (const Chunk& ins : file.children(lins)) {
            if (!ins.isList(fourcc("ins "))) continue;
            const int instrumentIndex = index++;
            if (size_t(index) > kMaxInstruments) throw ParseError("DLS/GIG instrument limit exceeded");
            const Chunks chunks = file.children(ins);
            const Chunk header = find(chunks, fourcc("insh"));
            if (header.size() < 12) continue;
            const auto bytes = file.read<12>(header);
            const uint32_t bank = le32(bytes.data() + 4);
            const uint32_t program = le32(bytes.data() + 8);
            if (program > 127) continue;
            DlsPresetInfo info{int(((bank >> 8) & 0x7f) * 128 + (bank & 0x7f)), int(program),
                               (bank & 0x80000000u) != 0, {}, instrumentIndex};
            const Chunk name = find(file.children(find(chunks, kList, fourcc("INFO"))), fourcc("INAM"));
            if (name.size()) {
                std::array<uint8_t, 256> buf;
                const size_t len = size_t(std::min<uint64_t>(buf.size(), name.size()));
                file.read(name, 0, buf.data(), len);
                const auto end = std::find(buf.begin(), buf.begin() + len, 0);
                info.name.assign(buf.begin(), end);
            }
            if (info.name.empty()) info.name = "Instrument " + std::to_string(instrumentIndex + 1);
            result.push_back({std::move(info), ins});
        }
        return result;
    }

    void indexPool() {
        const Chunk table = find(top_, fourcc("ptbl"));
        cues_ = readCues(file, table);
        hasCues_ = bool(table);
        primary_.index(file);
    }

    WaveRef resolve(uint32_t index, bool isGig) {
        if (!hasCues_) {
            if (isGig && index < primary_.waves.size()) return {&file, primary_.waves[index], 0};
            return {};
        }
        if (index >= cues_.size()) return {};
        const PoolCue cue = cues_[index];
        if (cue.offset == kNoWave) return {};
        if (cue.high == 0 && primary_.list) {
            const uint64_t pos = primary_.list.body + cue.offset;
            if (const Chunk wave = primary_.at(pos)) return {&file, wave, 0};
            if (isGig && pos >= file.size()) return resolveAcrossParts(pos - file.size());
        } else if (cue.high > 0 && cue.high <= kMaxExtension && isGig) {
            const int part = int(cue.high);
            if (const Extension* ext = extension(part)) {
                if (const Chunk wave = ext->pool.at(ext->pool.list.body + cue.offset))
                    return {&ext->file, wave, part};
            }
        }
        return {};
    }

private:
    const Extension* extension(int part) {
        if (checked_[part]) return extensions_[part].get();
        checked_[part] = true;
        auto path = utf8Path(file.path());
        path.replace_extension((part < 10 ? ".gx0" : ".gx") + std::to_string(part));
        auto ext = std::make_unique<Extension>(utf8String(path), visited_);
        if (!ext->file.handle()) return nullptr;
        const Chunk root = ext->file.root();
        ext->pool.list = root.type == fourcc("wvpl") ? root
            : find(ext->file.children(root), kList, fourcc("wvpl"));
        ext->pool.index(ext->file);
        extensions_[part] = std::move(ext);
        return extensions_[part].get();
    }

    WaveRef resolveAcrossParts(uint64_t pos) {
        for (int part = 1; part <= kMaxExtension; ++part) {
            const Extension* ext = extension(part);
            if (!ext) return {};
            if (pos < ext->file.size()) {
                if (const Chunk wave = ext->pool.at(pos)) return {&ext->file, wave, part};
                return {};
            }
            pos -= ext->file.size();
        }
        return {};
    }

    size_t visited_ = 0;

public:
    RiffFile file;
    bool gig = false;
    int major = 0;

private:
    Chunks top_;
    Pool primary_;
    std::vector<PoolCue> cues_;
    bool hasCues_ = false;
    std::array<bool, kMaxExtension + 1> checked_{};
    std::array<std::unique_ptr<Extension>, kMaxExtension + 1> extensions_;
};

enum Param { Gain, Pitch, Pan, Attack, Decay, Release, Sustain, Delay, Hold, Cutoff, Resonance, ParamCount };

struct Modulation {
    int32_t scale = 0;
    uint16_t transform = 0;
    bool art2 = false;

    double units() const { return scale / 65536.0; }
    double at(double value) const {
        if (art2) {
            if (transform & 0x8000) value = 1.0 - value;
            if (transform & 0x4000) value = 2.0 * value - 1.0;
        } else if (transform == 1) {
            value = 1.0 - value;
        }
        const int curve = art2 ? (transform >> 10) & 0xf : transform;
        const double magnitude = std::abs(value);
        if (curve == 1)
            value = std::copysign(-40.0 / 96.0 * std::log10(std::max(1e-6, 1.0 - magnitude)), value);
        else if (curve == 2)
            value = std::copysign(1.0 + 40.0 / 96.0 * std::log10(std::max(1e-6, magnitude)), value);
        else if (curve == 3)
            value = value >= 0.5 ? 1.0 : (value < 0.0 ? -1.0 : 0.0);
        return units() * std::clamp(value, -1.0, 1.0);
    }
};

struct Connections {
    std::optional<Modulation> fixed, velocity, key;
};

using Articulation = std::array<Connections, ParamCount>;

std::optional<Param> destination(uint16_t id) {
    switch (id) {
        case 0x0001: return Gain;
        case 0x0003: return Pitch;
        case 0x0004: return Pan;
        case 0x0206: return Attack;
        case 0x0207: return Decay;
        case 0x0209: return Release;
        case 0x020a: return Sustain;
        case 0x020b: return Delay;
        case 0x020c: return Hold;
        case 0x0500: return Cutoff;
        case 0x0501: return Resonance;
        default: return std::nullopt;
    }
}

void readArticulators(const RiffFile& file, const Chunks& chunks, Articulation& art) {
    // A Level 2 articulation list supersedes the Level 1 fallback at this scope.
    Chunk list = find(chunks, kList, fourcc("lar2"));
    if (!list) list = find(chunks, kList, fourcc("lart"));
    for (const Chunk& chunk : file.children(list)) {
        const bool art2 = chunk.id == fourcc("art2");
        if (!art2 && chunk.id != fourcc("art1")) continue;
        const auto bytes = file.read<8>(chunk);
        const uint32_t header = le32(bytes.data());
        const uint32_t count = le32(bytes.data() + 4);
        if (header < bytes.size() || header > chunk.size() || count > (chunk.size() - header) / 12 ||
            count > 4096)
            throw ParseError("Invalid DLS articulator connection table");
        std::vector<uint8_t> records(size_t(count) * 12);
        if (!records.empty()) file.read(chunk, header, records.data(), records.size());
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* rec = records.data() + size_t(i) * 12;
            const auto target = destination(le16(rec + 4));
            if (!target) continue;
            const uint16_t source = le16(rec);
            const uint16_t control = le16(rec + 2);
            if (source && control) continue;
            const uint16_t input = source ? source : control;
            uint16_t transform = le16(rec + 6);
            if (art2 && !source && control) transform = uint16_t(transform << 6);
            const Modulation mod{signed32(rec + 8), transform, art2};
            Connections& connection = art[*target];
            if (input == 0) connection.fixed = mod;
            if (input == 2) connection.velocity = mod;
            if (input == 3) connection.key = mod;
        }
    }
}

void warnOnce(std::vector<std::string>& warnings, const char* message) {
    if (std::find(warnings.begin(), warnings.end(), message) == warnings.end()) warnings.emplace_back(message);
}

void applyArticulators(const Articulation& art, Region& r, std::vector<std::string>& warnings) {
    const double key = (r.lokey + r.hikey) / 256.0;
    const double velocity = (r.lovel + r.hivel) / 256.0;
    auto fixed = [&](Param p, double fallback = 0.0) {
        return art[p].fixed ? art[p].fixed->units() : fallback;
    };
    auto value = [&](Param p, double fallback = 0.0) {
        return fixed(p, fallback) + (art[p].key ? art[p].key->at(key) : 0.0) +
               (art[p].velocity ? art[p].velocity->at(velocity) : 0.0);
    };
    auto present = [&](Param p) { return art[p].fixed || art[p].velocity || art[p].key; };
    auto seconds = [&](Param p) {
        if (art[p].key || art[p].velocity)
            warnOnce(warnings, "DLS envelope modulation uses the midpoint of each region's key and velocity ranges.");
        if (art[p].fixed && art[p].fixed->scale == std::numeric_limits<int32_t>::min()) return 0.0f;
        return float(std::exp2(std::clamp(value(p, -12000.0) / 1200.0, -30.0, 8.0)));
    };

    if (present(Attack) || present(Decay) || present(Release) || present(Sustain) ||
        present(Delay) || present(Hold)) {
        r.ampeg.enabled = true;
        if (present(Attack)) r.ampeg.attack = seconds(Attack);
        if (present(Decay)) r.ampeg.decay = seconds(Decay);
        if (present(Release)) r.ampeg.release = seconds(Release);
        if (present(Hold)) r.ampeg.hold = seconds(Hold);
        if (present(Delay)) r.ampeg.delay = seconds(Delay);
        if (present(Sustain)) r.ampeg.sustain = float(std::clamp(value(Sustain, 1000.0) / 1000.0, 0.0, 1.0));
    }

    r.volumeDb += float(fixed(Gain) / 10.0);
    if (art[Gain].key) {
        const auto& mod = *art[Gain].key;
        r.ampKeycenter = 0;
        r.volumeDb += float(mod.at(0.0) / 10.0);
        r.ampKeytrack = float((mod.at(1.0) - mod.at(0.0)) / 1280.0);
    }
    if (art[Gain].velocity)
        r.ampVeltrack = float(std::clamp(std::abs(art[Gain].velocity->units()) / 9.6, 0.0, 100.0));

    r.tune += float(fixed(Pitch) + (art[Pitch].velocity ? art[Pitch].velocity->at(velocity) : 0.0));
    if (art[Pitch].key) {
        const auto& mod = *art[Pitch].key;
        r.pitchKeytrack = float((mod.at(1.0) - mod.at(0.0)) / 128.0);
    }
    if (present(Pan)) r.pan = float(std::clamp(value(Pan) / 500.0, -1.0, 1.0));

    if (present(Cutoff) || present(Resonance)) {
        const double cents = fixed(Cutoff, 13500.0);
        const double keyBase = art[Cutoff].key ? art[Cutoff].key->at(0.0) : 0.0;
        const double velBase = art[Cutoff].velocity ? art[Cutoff].velocity->at(0.0) : 0.0;
        const double octaves = (cents + keyBase + velBase - 6900.0) / 1200.0;
        r.filter = RegionFilter::Lp2;
        r.cutoff = float(440.0 * std::exp2(std::clamp(octaves, -16.0, 12.0)));
        r.resonance = float(std::clamp(value(Resonance) / 10.0, 0.0, 96.0));
        if (art[Cutoff].key) {
            r.filKeycenter = 0;
            r.filKeytrack = float((art[Cutoff].key->at(1.0) - keyBase) / 128.0);
        }
        if (art[Cutoff].velocity)
            r.filVeltrack = float(art[Cutoff].velocity->at(1.0) - velBase);
    }
}

struct GigInstrument {
    int switchLo = 0;
    int switchHi = 0;
    float gainDb = 0.0f;
    float tune = 0.0f;
};

GigInstrument readGigInstrument(const RiffFile& file, const Chunks& chunks, bool& gig) {
    Chunk chunk = find(chunks, fourcc("3ewg"));
    if (!chunk) chunk = find(file.children(find(chunks, kList, fourcc("lart"))), fourcc("3ewg"));
    if (!chunk) return {};
    gig = true;
    const auto bytes = file.read<12>(chunk);
    return {bytes[10] >> 1, std::min(127, int(bytes[11])),
            float(-signed32(bytes.data() + 2) / 655360.0), float(signed16(bytes.data() + 6))};
}

struct GigZone {
    uint32_t cue = kNoWave;
    std::optional<WaveOptions> wave;
    std::array<uint8_t, 148> art{};
    size_t artSize = 0;

    int velocityLimit(int slot, bool v3) const {
        return v3 ? (artSize >= 148 ? art[140 + slot] : 0) : (artSize >= 140 ? art[124] : 0);
    }
};

// Native 3lnk IDs. Several of these share values with other dimensions in
// published summaries, so the on-disk definitions matter when decoding zones.
enum GigDimension {
    SampleChannel = 0x80, Layer = 0x81, Velocity = 0x82, ReleaseTrigger = 0x84,
    KeySwitch = 0x85, RoundRobin = 0x86, Random = 0x87, RoundRobinKeyboard = 0x89
};

struct Dimension {
    int type;
    int slot;
    int shift;
    int mask;
    int zones;
};

struct GigDimensions {
    std::vector<Dimension> axes;
    std::vector<GigZone> regions;
    bool v3 = false;
};

GigDimensions readDimensions(const RiffFile& file, const Chunks& chunks, const Chunk& link, int major) {
    GigDimensions result;
    if (const Chunk names = find(chunks, kList, fourcc("3dnm"))) file.children(names);
    // 3ddp is an opaque compatibility chunk. The 3lnk table defines the zone layout.
    const uint32_t count = le32(file.read<4>(link).data());
    if (count == 0 || count > 256) throw ParseError("Invalid GIG dimension-region count");
    result.v3 = major == 3 || link.size() > 172 || count > 32;
    const size_t slots = result.v3 ? 8 : 5;
    const size_t table = 4 + slots * 8;
    if (link.size() < table + count * 4 || (!result.v3 && count > 32))
        throw ParseError("Truncated GIG dimension table");
    std::array<uint8_t, 68 + 256 * 4> raw;
    file.read(link, 0, raw.data(), table + count * 4);
    int shift = 0;
    for (size_t slot = 0; slot < slots; ++slot) {
        const uint8_t* entry = raw.data() + 4 + slot * 8;
        if (!entry[0]) continue;
        const int bits = entry[1];
        if (bits <= 0 || bits > int(slots) - shift) throw ParseError("Invalid GIG dimension bit count");
        const int width = 1 << bits;
        const int zones = result.v3 && entry[4] != 0 ? entry[4] : width;
        if (zones > width) throw ParseError("Invalid GIG zone count");
        result.axes.push_back({entry[0], int(slot), shift, width - 1, zones});
        shift += bits;
    }
    if (count > (1u << shift)) throw ParseError("Invalid GIG dimension-region layout");

    result.regions.resize(count);
    for (size_t i = 0; i < count; ++i) result.regions[i].cue = le32(raw.data() + table + i * 4);
    const Chunk program = find(chunks, kList, fourcc("3prg"));
    size_t i = 0;
    for (const Chunk& list : file.children(program)) {
        if (!list.isList(fourcc("3ewl"))) continue;
        if (i >= count) throw ParseError("Too many GIG dimension-region articulations");
        GigZone& zone = result.regions[i++];
        const Chunks sub = file.children(list);
        if (const Chunk wsmp = find(sub, fourcc("wsmp"))) zone.wave = readWsmp(file, wsmp, true);
        if (const Chunk art = find(sub, fourcc("3ewa"))) {
            if (art.size() < 140) throw ParseError("Truncated GIG 3ewa articulation");
            zone.artSize = size_t(std::min<uint64_t>(zone.art.size(), art.size()));
            file.read(art, 0, zone.art.data(), zone.artSize);
        }
    }
    return result;
}

void applyGigZone(const GigZone& zone, Region& r) {
    if (!zone.artSize) return;
    const auto& b = zone.art;
    const auto time = [&](size_t offset) { return float(std::pow(1.000000008813822, signed32(b.data() + offset))); };
    r.ampeg.enabled = true;
    r.ampeg.attack = time(28);
    r.ampeg.decay = time(32);
    r.ampeg.sustain = std::min(1.0f, le16(b.data() + 38) / 1000.0f);
    r.ampeg.release = time(40);
    r.offset = le16(b.data() + 104);
    r.pitchKeytrack = (b[108] & 1) ? 0.0f : 100.0f;
    r.pan = b[109] < 64 ? b[109] / 63.0f : std::max(-1.0f, -(b[109] - 63) / 64.0f);
    if (r.trigger == RegionTrigger::Release && b[128] <= 8) {
        // Match the initial slope of the GIG linear amplitude decay in dB/sec.
        r.rtDecay = (20.0f / std::log(10.0f)) * 0.01053f * (256 >> b[128]);
    }
    if (b[132] & 0x80) {
        switch (b[139]) {
            case 1: r.filter = RegionFilter::Bp2; break;
            case 2: r.filter = RegionFilter::Hp2; break;
            case 3: r.filter = RegionFilter::Br2; break;
            default: r.filter = RegionFilter::Lp2; break;
        }
        const float cutoff = ((b[132] & 0x7f) + 29.0f) / 156.0f;
        r.cutoff = 18000.0f * cutoff * cutoff * cutoff * cutoff;
        r.resonance = (b[136] & 0x7f) * (24.0f / 127.0f);
        r.filKeytrack = (b[137] & 0x80) ? 100.0f : 0.0f;
        r.filKeycenter = b[137] & 0x7f;
    }
}

void applyWaveOptions(const WaveOptions& options, int64_t frames, Region& r) {
    r.keycenter = options.unity;
    r.tune += options.tune;
    r.volumeDb += options.gainDb;
    r.loopMode = LoopMode::NoLoop;
    if (options.loopMode != LoopMode::NoLoop && options.loopEnd <= frames &&
        options.loopStart >= 0 && options.loopEnd > options.loopStart + 1) {
        r.loopMode = options.loopMode;
        r.loopStart = options.loopStart;
        r.loopEnd = options.loopEnd;
    }
}

int mipCount(const Wave& wave, const Region& r) {
    const int key = r.pitchKeytrack >= 0 ? r.hikey : r.lokey;
    const double semis = double(key - r.keycenter) * r.pitchKeytrack / 100.0 + r.transpose + r.tune / 100.0;
    const double step = std::exp2((semis + 2.0) / 12.0) * wave.rate / 44100.0;
    return step > 8.0 ? 3 : step > 4.0 ? 2 : step > 2.0 ? 1 : 0;
}

std::unique_ptr<Sample> decode(const Wave& wave, int64_t frames, std::vector<uint8_t>& scratch) {
    auto sample = std::make_unique<Sample>();
    sample->path = wave.pcm.path;
    sample->channels = wave.pcm.fileChannels;
    sample->sampleRate = wave.rate;
    sample->frames = wave.frames;
    sample->unityNote = wave.options.unity;
    if (wave.options.loopMode != LoopMode::NoLoop && wave.options.loopStart >= 0 &&
        wave.options.loopEnd <= wave.frames) {
        sample->loopStart = wave.options.loopStart;
        sample->loopEnd = wave.options.loopEnd;
    }
    if (frames < wave.frames) {
        sample->streamed = true;
        sample->preloadFrames = frames;
        sample->stream = wave.pcm;
    }
    for (int c = 0; c < sample->channels; ++c) sample->data[c].resize(size_t(frames) + Sample::kPadFrames);
    for (int64_t pos = 0; pos < frames;) {
        const int64_t count = std::min<int64_t>(4096, frames - pos);
        float* left = sample->data[0].data() + pos;
        float* right = sample->channels == 2 ? sample->data[1].data() + pos : nullptr;
        if (!readStreamFrames(wave.file->handle(), wave.pcm, pos, count, left, right, scratch))
            throw ParseError("Cannot read wave PCM from " + wave.pcm.path);
        pos += count;
    }
    return sample;
}

struct WaveLink {
    uint32_t cue = kNoWave;
    uint16_t options = 0;
    uint16_t phaseGroup = 0;
    uint32_t channel = 0;
};

WaveLink readWaveLink(const RiffFile& file, const Chunk& chunk) {
    if (!chunk) return {};
    const auto bytes = file.read<12>(chunk);
    return {le32(bytes.data() + 8), le16(bytes.data()), le16(bytes.data() + 2), le32(bytes.data() + 4)};
}

int waveChannel(const WaveLink& link) {
    // Channel 1 is also the legacy DLS mono value. Route it left only when
    // the wave link declares a stereo/phase relationship.
    if (link.channel == 2) return 1;
    if (link.channel == 1 && (link.phaseGroup || (link.options & 3))) return 0;
    return -1;
}

void routeChannel(Region& r, int channel) {
    if (channel < 0) return;
    const float side = channel == 0 ? -1.0f : 1.0f;
    const float balance = std::clamp(1.0f + side * r.pan, 1e-6f, 1.0f);
    r.volumeDb += 20.0f * std::log10(balance);
    r.pan = side;
}

int velocityUpper(const GigDimensions& gig, const Dimension& dim, size_t base, int number) {
    if (number == dim.zones - 1) return 127;
    const size_t index = base + (size_t(number) << dim.shift);
    if (base < gig.regions.size() && index < gig.regions.size() &&
        gig.regions[base].velocityLimit(dim.slot, gig.v3) != 0) {
        const int limit = gig.regions[index].velocityLimit(dim.slot, gig.v3);
        return std::min(127, limit);
    }
    return ((number + 1) * 128 + dim.zones - 1) / dim.zones - 1;
}

class PresetLoader {
public:
    PresetLoader(Bank& bank, const Instrument& ins, std::vector<std::string>& warnings)
        : bank_(bank), instrument_(ins), warnings_(warnings) {}

    SampleSet load() {
        const RiffFile& file = bank_.file;
        set_.name = instrument_.info.name;
        set_.sourcePath = file.path();
        const Chunks chunks = file.children(instrument_.list);
        gig_ = bank_.gig || hasGigMarker(chunks);
        const GigInstrument gigInstrument = readGigInstrument(file, chunks, gig_);
        Articulation art{};
        readArticulators(file, chunks, art);
        const Chunks lists = file.children(find(chunks, kList, fourcc("lrgn")));
        for (const Chunk& list : lists) {
            if (list.isList(fourcc("rgn ")) || list.isList(fourcc("rgn2")))
                readRegion(list, art, gigInstrument);
        }
        if (!set_.regions.empty()) loadPcm();
        return std::move(set_);
    }

private:
    Wave* waveFor(uint32_t cue) {
        if (cue == kNoWave) return nullptr;  // intentional silent GIG zone
        const auto found = waveByCue_.find(cue);
        if (found != waveByCue_.end()) return found->second;
        Wave* result = nullptr;
        const WaveRef ref = bank_.resolve(cue, gig_);
        if (ref.file) {
            auto [it, inserted] = waves_.try_emplace({ref.part, ref.chunk.body});
            auto& wave = it->second;
            if (inserted && !readWave(*ref.file, ref.chunk, gig_, wave)) wave.frames = 0;
            if (wave.frames > 0) result = &wave;
            else if (wave.compressed)
                warnOnce(warnings_, "Skipped a GigaStudio compressed sample (ewav). Only uncompressed PCM is supported.");
            else
                warnOnce(warnings_, "Skipped an empty or unsupported DLS/GIG PCM wave.");
        } else {
            warnOnce(warnings_, "Skipped a DLS/GIG wave with an invalid pool cue or missing .gxNN extension.");
        }
        waveByCue_.emplace(cue, result);
        return result;
    }

    void append(Wave& wave, Region r) {
        if (set_.regions.size() >= kMaxRegions) throw ParseError("DLS/GIG preset region limit exceeded");
        // Bound malformed fixed-point values before they reach the real-time player.
        r.tune = std::clamp(r.tune, -12000.0f, 12000.0f);
        r.pitchKeytrack = std::clamp(r.pitchKeytrack, -256.0f, 256.0f);
        r.volumeDb = std::clamp(r.volumeDb, -600.0f, 48.0f);
        r.ampKeytrack = std::clamp(r.ampKeytrack, -1.0f, 1.0f);
        if (wave.regions.empty()) usedWaves_.push_back(&wave);
        wave.regions.push_back(set_.regions.size());
        wave.mips = std::max(wave.mips, mipCount(wave, r));
        wave.keepResident = wave.keepResident || r.loopMode != LoopMode::NoLoop;
        set_.hasReleaseRegions |= r.trigger == RegionTrigger::Release;
        set_.regions.push_back(r);
    }

    void readRegion(const Chunk& list, Articulation art, const GigInstrument& instrument) {
        const RiffFile& file = bank_.file;
        const Chunks chunks = file.children(list);
        gig_ = gig_ || hasGigMarker(chunks);
        const Chunk header = find(chunks, fourcc("rgnh"));
        if (!header) return;
        const auto bytes = file.read<12>(header);
        Region base;
        base.lokey = le16(bytes.data());
        base.hikey = le16(bytes.data() + 2);
        base.lovel = le16(bytes.data() + 4);
        base.hivel = le16(bytes.data() + 6);
        if (base.lokey > base.hikey || base.hikey > 127 || base.lovel > base.hivel || base.hivel > 127)
            throw ParseError("Invalid DLS/GIG key or velocity range");
        base.group = base.offBy = le16(bytes.data() + 10);
        if (base.group != 0 && (le16(bytes.data() + 8) & 1))
            warnOnce(warnings_, "The engine applies DLS key-group choking to all notes in a group, including repeated notes.");
        std::optional<WaveOptions> override;
        if (const Chunk options = find(chunks, fourcc("wsmp"))) override = readWsmp(file, options, gig_);
        const WaveLink link = readWaveLink(file, find(chunks, fourcc("wlnk")));
        readArticulators(file, chunks, art);

        if (const Chunk dimensionLink = find(chunks, fourcc("3lnk"))) {
            const auto dimensions = readDimensions(file, chunks, dimensionLink, bank_.major);
            for (size_t i = 0; i < dimensions.regions.size(); ++i)
                readZone(dimensions, i, base, override, art, instrument, link);
            return;
        }
        Wave* wave = waveFor(link.cue);
        if (!wave) return;
        if (override && !gig_ && wave->gig) override->gainDb = -override->gainDb;
        applyWaveOptions(override ? *override : wave->options, wave->frames, base);
        applyArticulators(art, base, warnings_);
        routeChannel(base, waveChannel(link));
        base.volumeDb += instrument.gainDb;
        base.tune += instrument.tune;
        append(*wave, base);
    }

    void readZone(const GigDimensions& dimensions, size_t i, Region r, const std::optional<WaveOptions>& override,
                  const Articulation& art, const GigInstrument& instrument, const WaveLink& link) {
        const GigZone& zone = dimensions.regions[i];
        if (zone.cue == kNoWave) return;
        int stereo = -1;
        int switchLo = -1, switchHi = -1;
        for (const Dimension& dim : dimensions.axes) {
            const int number = int(i >> dim.shift) & dim.mask;
            if (number >= dim.zones) return;
            switch (dim.type) {
                case Velocity: {
                    const size_t base = i & ~(size_t(dim.mask) << dim.shift);
                    const int low = number == 0 ? 0 : velocityUpper(dimensions, dim, base, number - 1) + 1;
                    r.lovel = std::max(r.lovel, low);
                    r.hivel = std::min(r.hivel, velocityUpper(dimensions, dim, base, number));
                    if (r.lovel > r.hivel) return;
                    break;
                }
                case SampleChannel:
                    if (number > 1) return;
                    if (dim.zones > 1) stereo = number;
                    break;
                case ReleaseTrigger:
                    if (number > 0) r.trigger = RegionTrigger::Release;
                    break;
                case KeySwitch: {
                    const int length = instrument.switchHi - instrument.switchLo + 1;
                    if (length <= 0 || (length == 1 && instrument.switchLo == 0 && dim.zones > 1)) {
                        warnOnce(warnings_, "GigaStudio keyswitch dimension is missing a usable 3ewg key range.");
                        if (number != 0) return;
                        break;
                    }
                    switchLo = instrument.switchLo + (number * length + dim.zones - 1) / dim.zones;
                    switchHi = instrument.switchLo + ((number + 1) * length + dim.zones - 1) / dim.zones - 1;
                    if (switchHi < switchLo) return;
                    break;
                }
                case RoundRobin:
                case RoundRobinKeyboard:
                case 0x8d:
                    r.seqPosition += number * r.seqLength;
                    r.seqLength *= dim.zones;
                    break;
                case Random: {
                    const float width = (r.hirand - r.lorand) / dim.zones;
                    if (number + 1 < dim.zones) r.hirand = r.lorand + (number + 1) * width;
                    r.lorand += number * width;
                    break;
                }
                case Layer: break;
                default:
                    warnOnce(warnings_, "Unsupported GigaStudio dimensions use their first zone.");
                    if (number != 0) return;
                    break;
            }
        }
        Wave* wave = waveFor(zone.cue);
        if (!wave) return;
        const WaveOptions& options = zone.wave ? *zone.wave : override ? *override : wave->options;
        applyWaveOptions(options, wave->frames, r);
        applyArticulators(art, r, warnings_);
        applyGigZone(zone, r);
        routeChannel(r, stereo >= 0 ? stereo : waveChannel(link));
        r.offset = std::min(r.offset, wave->frames - 1);
        r.volumeDb += instrument.gainDb;
        r.tune += instrument.tune;
        if (switchLo < 0) {
            append(*wave, r);
            return;
        }
        if (set_.swLo < 0) {
            set_.swLo = instrument.switchLo;
            set_.swHi = instrument.switchHi;
            set_.swDefault = switchLo;
        } else {
            set_.swDefault = std::min(set_.swDefault, switchLo);
        }
        for (int key = switchLo; key <= switchHi; ++key) {
            r.swLast = key;
            append(*wave, r);
        }
    }

    void loadPcm() {
        const SampleLoadOptions options = sampleLoadOptions();
        uint64_t pcmBytes = 0;
        for (const Wave* wave : usedWaves_) pcmBytes += uint64_t(wave->frames) * wave->pcm.blockAlign;
        const bool stream = options.mode == StreamMode::Always ||
            (options.mode == StreamMode::Auto && pcmBytes > uint64_t(std::max<int64_t>(0, options.autoThresholdBytes)));
        const int64_t preload = std::max<int64_t>(4096, options.preloadFrames);
        std::vector<int64_t> heads;
        heads.reserve(usedWaves_.size());
        uint64_t total = 0;
        for (const Wave* wave : usedWaves_) {
            bool eligible = stream && !wave->keepResident && wave->mips == 0 && preload < wave->frames - preload;
            for (size_t index : wave->regions) {
                if (set_.regions[index].offset > preload / 2) eligible = false;
            }
            int64_t frames = eligible ? preload : wave->frames;
            heads.push_back(frames);
            uint64_t floats = uint64_t(frames) + Sample::kPadFrames;
            for (int level = 0; level < wave->mips; ++level) {
                frames = (frames + 1) / 2;
                floats += uint64_t(frames) + kMipPadFrames;
            }
            total += floats * wave->pcm.fileChannels * sizeof(float);
            if (total > kResidentLimit) throw ParseError("DLS/GIG preset exceeds the 256 MiB resident PCM limit");
        }

        std::vector<uint8_t> scratch;
        SampleSet mipInput;
        mipInput.samples.reserve(1);
        for (size_t i = 0; i < usedWaves_.size(); ++i) {
            const Wave& wave = *usedWaves_[i];
            auto sample = decode(wave, heads[i], scratch);
            for (size_t index : wave.regions) {
                Region& r = set_.regions[index];
                r.sample = sample.get();
                if (wave.mips) mipInput.regions.push_back(r);
            }
            if (wave.mips) {
                // Keep mip preparation proportional to the number of regions.
                mipInput.samples.push_back(std::move(sample));
                mipInput.prepareMips();
                sample = std::move(mipInput.samples.front());
                mipInput.samples.clear();
                mipInput.regions.clear();
                // prepareMips uses hikey. Negative key tracking peaks at lokey.
                if (sample->mipLevels < wave.mips) sample->buildMips(wave.mips);
            }
            set_.samples.push_back(std::move(sample));
        }
    }

    Bank& bank_;
    const Instrument& instrument_;
    std::vector<std::string>& warnings_;
    SampleSet set_;
    bool gig_ = false;
    std::map<std::pair<int, uint64_t>, Wave> waves_;
    std::map<uint32_t, Wave*> waveByCue_;
    std::vector<Wave*> usedWaves_;
};

}  // namespace

bool listDlsGigPresets(const std::string& path, std::vector<DlsPresetInfo>& out, std::string& error) {
    error.clear();
    try {
        Bank bank(path);
        auto instruments = bank.instruments();
        if (instruments.empty()) throw ParseError("No valid instruments found in " + path);
        std::vector<DlsPresetInfo> result;
        result.reserve(instruments.size());
        for (auto& instrument : instruments) result.push_back(std::move(instrument.info));
        out = std::move(result);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool loadDlsGigPreset(const std::string& path, int bank, int program, bool isDrum,
                      SampleSet& out, std::string& error, std::vector<std::string>& warnings) {
    error.clear();
    warnings.clear();
    try {
        Bank collection(path);
        const auto instruments = collection.instruments();
        const bool automatic = bank < 0 || program < 0;
        bool found = false;
        std::string lastFailure;
        for (const Instrument& instrument : instruments) {
            const DlsPresetInfo& info = instrument.info;
            if (!automatic && (info.bank != bank || info.program != program || info.isDrum != isDrum)) continue;
            if (!found) collection.indexPool();
            found = true;
            try {
                SampleSet result = PresetLoader(collection, instrument, warnings).load();
                if (result.regions.empty()) continue;
                result.warnings = warnings;
                out = std::move(result);
                return true;
            } catch (const ParseError& e) {
                if (!automatic) throw;
                lastFailure = e.what();
                warnOnce(warnings, "Skipped a DLS/GIG instrument that could not be loaded during automatic selection.");
            }
        }
        if (!lastFailure.empty()) throw ParseError(lastFailure);
        throw ParseError(found ? "No playable PCM regions in the requested DLS/GIG instrument" :
                                 "Requested DLS/GIG instrument was not found");
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

}  // namespace hsynth
