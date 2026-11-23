#include "hsynth/formats/dspreset.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hsynth/common.h"
#include "hsynth/fileio.h"
#include "hsynth/flac.h"
#include "hsynth/wav.h"

namespace hsynth {

namespace {

using AttrMap = std::unordered_map<std::string, std::string>;

struct XmlNode {
    std::string tag;
    AttrMap attrs;
    std::vector<XmlNode> children;
};

std::string toLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trimStr(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string unescapeXml(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out.push_back(s[i]);
            continue;
        }
        const size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) {
            out.push_back('&');
            continue;
        }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out.push_back('&');
        else if (ent == "quot") out.push_back('"');
        else if (ent == "apos") out.push_back('\'');
        else if (ent == "lt") out.push_back('<');
        else if (ent == "gt") out.push_back('>');
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = 0;
            if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                cp = std::strtoul(ent.c_str() + 2, nullptr, 16);
            else
                cp = std::strtoul(ent.c_str() + 1, nullptr, 10);
            if (cp > 0 && cp < 0x80) out.push_back((char)cp);
            else if (cp >= 0x80 && cp < 0x800) {
                out.push_back((char)(0xC0 | (cp >> 6)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
            } else if (cp >= 0x800 && cp < 0x10000) {
                out.push_back((char)(0xE0 | (cp >> 12)));
                out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
            }
        } else {
            out.append(s, i, semi - i + 1);
        }
        i = semi;
    }
    return out;
}

class XmlParser {
public:
    explicit XmlParser(const std::string& text) : s_(text) {
        if (s_.size() >= 3 && (uint8_t)s_[0] == 0xEF && (uint8_t)s_[1] == 0xBB && (uint8_t)s_[2] == 0xBF)
            i_ = 3;
    }

    bool parseRoot(XmlNode& root, std::string& error) {
        while (skipSpaceAndComments()) {
            if (i_ >= s_.size()) break;
            if (s_[i_] == '<') {
                if (i_ + 1 < s_.size() && (s_[i_ + 1] == '?' || s_[i_ + 1] == '!')) {
                    skipDeclOrComment();
                    continue;
                }
                return parseElement(root, error);
            }
            ++i_;
        }
        error = "empty XML document";
        return false;
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    void skipWs() {
        while (i_ < s_.size() && std::isspace((unsigned char)s_[i_])) ++i_;
    }

    bool skipDeclOrComment() {
        if (s_.compare(i_, 4, "<!--") == 0) {
            const size_t end = s_.find("-->", i_ + 4);
            i_ = end == std::string::npos ? s_.size() : end + 3;
            return true;
        }
        if (s_.compare(i_, 2, "<?") == 0) {
            const size_t end = s_.find("?>", i_ + 2);
            i_ = end == std::string::npos ? s_.size() : end + 2;
            return true;
        }
        if (s_.compare(i_, 2, "<!") == 0) {
            const size_t end = s_.find('>', i_ + 2);
            i_ = end == std::string::npos ? s_.size() : end + 1;
            return true;
        }
        return false;
    }

    bool skipSpaceAndComments() {
        for (;;) {
            skipWs();
            if (i_ < s_.size() && s_[i_] == '<' && i_ + 1 < s_.size() &&
                (s_[i_ + 1] == '!' || s_[i_ + 1] == '?')) {
                skipDeclOrComment();
                continue;
            }
            break;
        }
        return i_ < s_.size();
    }

    bool parseElement(XmlNode& node, std::string& error) {
        if (i_ >= s_.size() || s_[i_] != '<') {
            error = "expected '<' in XML";
            return false;
        }
        ++i_;
        const size_t tagStart = i_;
        while (i_ < s_.size() && !std::isspace((unsigned char)s_[i_]) && s_[i_] != '>' && s_[i_] != '/') ++i_;
        if (i_ == tagStart) {
            error = "empty XML tag name";
            return false;
        }
        node.tag = s_.substr(tagStart, i_ - tagStart);

        // Attributes
        for (;;) {
            skipWs();
            if (i_ >= s_.size()) {
                error = "unterminated XML tag <" + node.tag + ">";
                return false;
            }
            if (s_[i_] == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '>') {
                i_ += 2;
                return true;
            }
            if (s_[i_] == '>') {
                ++i_;
                break;
            }
            const size_t kStart = i_;
            while (i_ < s_.size() && s_[i_] != '=' && !std::isspace((unsigned char)s_[i_]) && s_[i_] != '>' && s_[i_] != '/')
                ++i_;
            const std::string key = s_.substr(kStart, i_ - kStart);
            skipWs();
            std::string val;
            if (i_ < s_.size() && s_[i_] == '=') {
                ++i_;
                skipWs();
                if (i_ < s_.size() && (s_[i_] == '"' || s_[i_] == '\'')) {
                    const char quote = s_[i_++];
                    const size_t vStart = i_;
                    while (i_ < s_.size() && s_[i_] != quote) ++i_;
                    val = unescapeXml(s_.substr(vStart, i_ - vStart));
                    if (i_ < s_.size()) ++i_;
                } else {
                    const size_t vStart = i_;
                    while (i_ < s_.size() && !std::isspace((unsigned char)s_[i_]) && s_[i_] != '>') ++i_;
                    val = unescapeXml(s_.substr(vStart, i_ - vStart));
                }
            }
            if (!key.empty()) node.attrs[key] = std::move(val);
        }

        // Children until matching </tag>
        for (;;) {
            while (i_ < s_.size() && s_[i_] != '<') ++i_;
            if (i_ >= s_.size()) return true;
            if (s_.compare(i_, 2, "</") == 0) {
                const size_t close = s_.find('>', i_ + 2);
                i_ = close == std::string::npos ? s_.size() : close + 1;
                return true;
            }
            if (s_.compare(i_, 2, "<!") == 0 || s_.compare(i_, 2, "<?") == 0) {
                skipDeclOrComment();
                continue;
            }
            XmlNode child;
            if (!parseElement(child, error)) return false;
            node.children.push_back(std::move(child));
        }
    }
};

bool parseNoteValue(const std::string& raw, int& out) {
    const std::string v = trimStr(raw);
    if (v.empty()) return false;
    if (std::isdigit((unsigned char)v[0]) || v[0] == '-') {
        out = std::atoi(v.c_str());
        return true;
    }
    static const int base[7] = {9, 11, 0, 2, 4, 5, 7};
    const char c = (char)std::tolower((unsigned char)v[0]);
    if (c < 'a' || c > 'g') return false;
    int n = base[c - 'a'];
    size_t i = 1;
    if (i < v.size() && (v[i] == '#' || v[i] == 's')) {
        ++n;
        ++i;
    } else if (i < v.size() && v[i] == 'b') {
        --n;
        ++i;
    }
    if (i >= v.size()) return false;
    const int octave = std::atoi(v.c_str() + i);
    out = (octave + 1) * 12 + n;
    return true;
}

bool parseBool(const std::string& v) {
    const std::string l = toLower(trimStr(v));
    return l == "true" || l == "1" || l == "yes";
}

float parseVolumeDb(const std::string& raw) {
    const std::string l = toLower(trimStr(raw));
    if (l.empty()) return 0.0f;
    if (l.find("db") != std::string::npos) return (float)std::atof(l.c_str());
    const float lin = (float)std::atof(l.c_str());
    if (lin <= 0.0f) return -144.0f;
    return 20.0f * std::log10(lin);
}

std::string dirName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

std::string stemName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string b = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = b.find_last_of('.');
    return dot == std::string::npos ? b : b.substr(0, dot);
}

std::string lowerExt(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    return toLower(path.substr(dot + 1));
}

bool readAllBytes(const std::string& path, std::vector<uint8_t>& out, std::string& error) {
    std::unique_ptr<FILE, int (*)(FILE*)> f(fopenUtf8(path, "rb"), &std::fclose);
    if (!f) {
        error = "cannot open file: " + path;
        return false;
    }
    std::fseek(f.get(), 0, SEEK_END);
    const long sz = std::ftell(f.get());
    if (sz <= 0) {
        error = "empty or unreadable file: " + path;
        return false;
    }
    std::rewind(f.get());
    out.resize((size_t)sz);
    if (std::fread(out.data(), 1, out.size(), f.get()) != out.size()) {
        error = "read error: " + path;
        return false;
    }
    return true;
}

std::unique_ptr<Sample> decodeAudioFile(const std::string& fullPath, const OggDecodeFn& oggDecode,
                                        std::string& error) {
    const std::string ext = lowerExt(fullPath);
    if (ext == "ogg") {
        if (!oggDecode) {
            error = "OGG sample skipped (no Ogg decoder provided): " + fullPath;
            return nullptr;
        }
        std::vector<uint8_t> bytes;
        if (!readAllBytes(fullPath, bytes, error)) return nullptr;
        std::vector<float> mono;
        int rate = 44100;
        if (!oggDecode(bytes.data(), bytes.size(), mono, rate) || mono.empty()) {
            error = "failed to decode OGG sample: " + fullPath;
            return nullptr;
        }
        auto s = std::make_unique<Sample>();
        s->path = fullPath;
        s->channels = 1;
        s->sampleRate = rate > 0 ? (double)rate : 44100.0;
        s->frames = (int64_t)mono.size();
        s->data[0] = std::move(mono);
        s->data[0].resize((size_t)(s->frames + Sample::kPadFrames), 0.0f);
        return s;
    }

    WavData w;
    const bool ok = (ext == "flac") ? readFlac(fullPath, w, error) : readWav(fullPath, w, error);
    if (!ok) return nullptr;

    auto s = std::make_unique<Sample>();
    s->path = fullPath;
    s->channels = w.channels;
    s->sampleRate = w.sampleRate;
    s->frames = w.frames;
    s->unityNote = w.unityNote;
    if (w.loopStart >= 0) {
        s->loopStart = w.loopStart;
        s->loopEnd = w.loopEnd + 1;
    }
    s->data[0] = std::move(w.left);
    s->data[0].resize((size_t)(w.frames + Sample::kPadFrames), 0.0f);
    if (w.channels == 2) {
        s->data[1] = std::move(w.right);
        s->data[1].resize((size_t)(w.frames + Sample::kPadFrames), 0.0f);
    }
    return s;
}

std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        tok = trimStr(tok);
        if (!tok.empty()) out.push_back(tok);
    }
    return out;
}

}  // namespace

bool loadDecentSamplerPreset(const std::string& dspresetPath, SampleSet& out, std::string& error,
                             std::vector<std::string>& warnings, OggDecodeFn oggDecode) {
    out = SampleSet{};
    out.sourcePath = dspresetPath;
    out.name = stemName(dspresetPath);
    warnings.clear();

    std::ifstream ifs(utf8Path(dspresetPath), std::ios::binary);
    if (!ifs) {
        error = "cannot open dspreset: " + dspresetPath;
        return false;
    }
    std::stringstream buf;
    buf << ifs.rdbuf();
    const std::string xmlText = buf.str();

    XmlNode root;
    XmlParser parser(xmlText);
    if (!parser.parseRoot(root, error)) return false;

    const std::string rootDir = dirName(dspresetPath);
    std::unordered_map<std::string, Sample*> sampleCache;
    std::unordered_map<std::string, bool> failedPaths;
    std::unordered_map<std::string, int> tagIds;
    int nextTagId = 1;
    auto tagIdFor = [&](const std::string& tag) -> int {
        auto it = tagIds.find(tag);
        if (it != tagIds.end()) return it->second;
        tagIds[tag] = nextTagId;
        return nextTagId++;
    };

    auto processSample = [&](const AttrMap& inherited, const XmlNode& sampleNode) {
        AttrMap attrs = inherited;
        for (const auto& kv : sampleNode.attrs) attrs[kv.first] = kv.second;

        auto get = [&](const char* k) -> const std::string* {
            auto it = attrs.find(k);
            return it == attrs.end() ? nullptr : &it->second;
        };

        const std::string* pathAttr = get("path");
        if (!pathAttr || pathAttr->empty()) return;

        // Trigger handling
        RegionTrigger trigger = RegionTrigger::Attack;
        if (const std::string* trig = get("trigger")) {
            const std::string t = toLower(trimStr(*trig));
            if (t == "release") trigger = RegionTrigger::Release;
            else if (t != "attack" && !t.empty()) {
                warnings.push_back("trigger='" + *trig + "' skipped (unsupported)");
                return;
            }
        }

        std::string rel = *pathAttr;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        const bool isAbs = (!rel.empty() && rel[0] == '/') || (rel.size() > 1 && rel[1] == ':');
        const std::string fullPath = isAbs ? rel : rootDir + rel;

        Sample* sample = nullptr;
        auto cached = sampleCache.find(fullPath);
        if (cached != sampleCache.end()) {
            sample = cached->second;
        } else if (!failedPaths.count(fullPath)) {
            std::string decErr;
            auto s = decodeAudioFile(fullPath, oggDecode, decErr);
            if (!s) {
                failedPaths[fullPath] = true;
                warnings.push_back(decErr);
            } else {
                sample = s.get();
                sampleCache[fullPath] = sample;
                out.samples.push_back(std::move(s));
            }
        }
        if (!sample) return;

        Region r;
        r.sample = sample;
        r.trigger = trigger;
        r.keycenter = (sample->unityNote >= 0 && sample->unityNote < 128) ? sample->unityNote : 60;

        if (const std::string* v = get("rootNote")) {
            int n = 60;
            if (parseNoteValue(*v, n)) r.keycenter = n;
        }
        if (const std::string* v = get("loNote")) {
            int n = 0;
            if (parseNoteValue(*v, n)) r.lokey = n;
        }
        if (const std::string* v = get("hiNote")) {
            int n = 127;
            if (parseNoteValue(*v, n)) r.hikey = n;
        }
        r.lokey = std::max(0, std::min(127, r.lokey));
        r.hikey = std::max(0, std::min(127, r.hikey));

        if (const std::string* v = get("loVel")) r.lovel = std::max(1, std::min(127, std::atoi(v->c_str())));
        if (const std::string* v = get("hiVel")) r.hivel = std::max(1, std::min(127, std::atoi(v->c_str())));
        if (r.lovel > r.hivel) r.lovel = r.hivel;

        if (const std::string* v = get("volume")) r.volumeDb = parseVolumeDb(*v);
        if (const std::string* v = get("pan"))
            r.pan = clampf((float)std::atof(v->c_str()) / 100.0f, -1.0f, 1.0f);
        if (const std::string* v = get("tuning"))
            r.tune = (float)std::atof(v->c_str()) * 100.0f;  // semitones -> cents
        if (const std::string* v = get("pitchKeyTrack"))
            r.pitchKeytrack = (float)std::atof(v->c_str()) * 100.0f;

        if (const std::string* v = get("start"))
            r.offset = std::max<int64_t>(0, std::min<int64_t>(sample->frames - 1, std::atoll(v->c_str())));
        if (const std::string* v = get("end")) {
            const int64_t e = std::atoll(v->c_str());
            if (e > r.offset && e <= sample->frames) r.end = e;
        }

        // Loop settings
        const bool loopEnabled = get("loopEnabled") ? parseBool(*get("loopEnabled")) : false;
        if (loopEnabled) {
            r.loopMode = LoopMode::Continuous;
            r.loopStart = get("loopStart") ? std::atoll(get("loopStart")->c_str()) : sample->loopStart;
            r.loopEnd = get("loopEnd") ? std::atoll(get("loopEnd")->c_str()) : sample->loopEnd;
            if (r.loopStart < 0 && r.loopEnd < 0) {
                r.loopStart = 0;
                r.loopEnd = sample->frames;
            }
            if (r.loopStart < 0 || r.loopEnd <= r.loopStart + 1 || r.loopEnd > sample->frames) {
                r.loopMode = LoopMode::NoLoop;
            } else if (const std::string* xf = get("loopCrossfade")) {
                const float xfSamples = std::max(0.0f, (float)std::atof(xf->c_str()));
                if (sample->sampleRate > 0.0) r.loopCrossfade = xfSamples / (float)sample->sampleRate;
            }
        }

        // Amp envelope & velocity tracking
        if (get("attack") || get("decay") || get("sustain") || get("release")) {
            r.ampeg.enabled = true;
            r.ampeg.sustain = 1.0f;
            if (const std::string* v = get("attack")) r.ampeg.attack = std::max(0.0f, (float)std::atof(v->c_str()));
            if (const std::string* v = get("decay")) r.ampeg.decay = std::max(0.0f, (float)std::atof(v->c_str()));
            if (const std::string* v = get("sustain")) r.ampeg.sustain = clampf((float)std::atof(v->c_str()), 0.0f, 1.0f);
            if (const std::string* v = get("release")) r.ampeg.release = std::max(0.0f, (float)std::atof(v->c_str()));
        }
        if (const std::string* v = get("ampVelTrack"))
            r.ampVeltrack = clampf((float)std::atof(v->c_str()), 0.0f, 1.0f) * 100.0f;

        // Round-robin / random sequence
        const int seqLen = get("seqLength") ? std::max(1, std::atoi(get("seqLength")->c_str())) : 1;
        const int seqPos = get("seqPosition") ? std::max(1, std::atoi(get("seqPosition")->c_str())) : 1;
        if (seqLen > 1) {
            const std::string mode = get("seqMode") ? toLower(trimStr(*get("seqMode"))) : "round_robin";
            if (mode == "random" || mode == "true_random") {
                const int idx = std::min(seqLen, seqPos) - 1;
                r.lorand = (float)idx / (float)seqLen;
                r.hirand = (float)(idx + 1) / (float)seqLen;
            } else {
                r.seqLength = seqLen;
                r.seqPosition = std::min(seqLen, seqPos);
            }
        }

        // Tag-based choke groups
        if (const std::string* v = get("tags")) {
            const auto tags = splitCsv(*v);
            if (!tags.empty()) r.group = tagIdFor(tags.front());
        }
        if (const std::string* v = get("silencedByTags")) {
            const auto stags = splitCsv(*v);
            if (!stags.empty()) {
                r.offBy = tagIdFor(stags.front());
                r.offFast = true;
            }
        }

        if (r.trigger == RegionTrigger::Release) out.hasReleaseRegions = true;
        out.regions.push_back(r);
    };

    auto walkGroups = [&](auto&& self, const XmlNode& node, const AttrMap& inherited) -> void {
        if (node.tag == "groups" || node.tag == "group") {
            AttrMap merged = inherited;
            for (const auto& kv : node.attrs) merged[kv.first] = kv.second;
            for (const auto& ch : node.children) self(self, ch, merged);
        } else if (node.tag == "sample") {
            processSample(inherited, node);
        } else {
            for (const auto& ch : node.children) self(self, ch, inherited);
        }
    };

    AttrMap emptyAttrs;
    walkGroups(walkGroups, root, emptyAttrs);

    out.warnings = warnings;
    if (out.regions.empty()) {
        error = "no playable samples in " + dspresetPath;
        return false;
    }

    out.prepareMips();
    return true;
}

}  // namespace hsynth
