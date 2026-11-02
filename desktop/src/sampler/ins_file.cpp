#include "ins_file.h"

#include <cstdlib>
#include <fstream>
#include <set>

#include "hsynth/fileio.h"

namespace sampler {

namespace {
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
    return s.substr(a);
}
// "12:909 808 Kit" -> "909 808 Kit" (Roland lists prefix the display number)
std::string stripNumber(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
    return i > 0 && i < s.size() && s[i] == ':' ? s.substr(i + 1) : s;
}
}  // namespace

bool readIns(const std::string& path, InsFile& out, std::string& error) {
    std::ifstream f(hsynth::utf8Path(path), std::ios::binary);
    if (!f) { error = "cannot open " + path; return false; }
    enum class Part { None, Patches, Notes, Defs, Other } part = Part::None;
    std::string section, line;
    // per instrument definition
    std::map<int, std::string> bankList;                 // bank -> patch list
    std::map<std::pair<int, int>, std::string> keyList;  // (bank, prog|-1) -> note list
    std::set<std::pair<int, int>> drums;                 // (bank, prog|-1)
    auto flushDef = [&]() {
        for (auto& [bank, listName] : bankList) {
            auto it = out.patchLists.find(listName);
            if (it == out.patchLists.end()) continue;
            for (auto& [prog, name] : it->second) {
                InsPatch p;
                p.bank = bank;
                p.program = prog;
                p.name = stripNumber(name);
                p.list = listName;
                p.drum = drums.count({bank, prog}) || drums.count({bank, -1});
                auto kl = keyList.find({bank, prog});
                if (kl == keyList.end()) kl = keyList.find({bank, -1});
                if (kl != keyList.end()) {
                    auto nl = out.noteLists.find(kl->second);
                    if (nl != out.noteLists.end())
                        for (auto& [k, n] : nl->second) {
                            const std::string t = trim(n);
                            if (!t.empty()) p.keys[k] = t;
                        }
                }
                out.patches.push_back(std::move(p));
            }
        }
        bankList.clear();
        keyList.clear();
        drums.clear();
    };
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '.') {
            if (part == Part::Defs) flushDef();
            part = line.rfind(".Patch Names", 0) == 0 ? Part::Patches
                   : line.rfind(".Note Names", 0) == 0 ? Part::Notes
                   : line.rfind(".Instrument Definitions", 0) == 0 ? Part::Defs
                   : Part::Other;
            section.clear();
            continue;
        }
        if (line[0] == '[') {
            if (part == Part::Defs) flushDef();
            section = line.substr(1, line.find(']') - 1);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || section.empty()) continue;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (part == Part::Patches || part == Part::Notes) {
            if (key.empty() || !std::isdigit((unsigned char)key[0])) continue;  // BasedOn= etc.
            (part == Part::Patches ? out.patchLists : out.noteLists)[section][std::atoi(key.c_str())] = value;
        } else if (part == Part::Defs) {
            // Patch[bank]=list   Key[bank,prog]=notes   Drum[bank,prog]=1   (prog may be *)
            const size_t lb = key.find('['), rb = key.find(']');
            if (lb == std::string::npos || rb == std::string::npos) continue;
            const std::string kind = key.substr(0, lb), args = key.substr(lb + 1, rb - lb - 1);
            const size_t comma = args.find(',');
            const int bank = std::atoi(args.substr(0, comma).c_str());
            const int prog = comma == std::string::npos || args.substr(comma + 1) == "*" ? -1 : std::atoi(args.substr(comma + 1).c_str());
            if (kind == "Patch") bankList[bank] = value;
            else if (kind == "Key") keyList[{bank, prog}] = value;
            else if (kind == "Drum" && value == "1") drums.insert({bank, prog});
        }
    }
    if (part == Part::Defs) flushDef();
    return true;
}

}  // namespace sampler
