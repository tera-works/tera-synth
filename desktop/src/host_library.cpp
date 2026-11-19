#include "host_library.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "hsynth/json.h"

std::string pathToUtf8(const fs::path& p) {
    const auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

fs::path utf8ToPath(const std::string& s) { return fs::u8path(s); }

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string ext(const fs::path& p) { return lower(pathToUtf8(p.extension())); }

fs::path uniquePath(const fs::path& dir, const std::string& name) {
    fs::path p = dir / utf8ToPath(name);
    if (!fs::exists(p)) return p;
    const std::string stem = pathToUtf8(utf8ToPath(name).stem());
    const std::string e = pathToUtf8(utf8ToPath(name).extension());
    for (int i = 2;; ++i) {
        p = dir / utf8ToPath(stem + " (" + std::to_string(i) + ")" + e);
        if (!fs::exists(p)) return p;
    }
}

// The entries of one folder: name + directory flag. On Windows FindFirstFileEx hands the
// attributes over with the names; std::filesystem asks the file system once per file
// there (a library of 90,000 samples took 40 s to list that way).
struct DirItem { fs::path path; bool dir; };
std::vector<DirItem> listDir(const fs::path& d) {
    std::vector<DirItem> out;
#ifdef _WIN32
    WIN32_FIND_DATAW fd;
    const std::wstring pattern = (d / L"*").wstring();
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // no junction loops
        out.push_back({d / n, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    std::error_code ec;
    for (auto it = fs::directory_iterator(d, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (it->is_symlink(e2)) continue;
        out.push_back({it->path(), it->is_directory(e2)});
    }
#endif
    return out;
}

std::string presetTitle(const fs::path& f) {
    std::ifstream in(f, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    hsynth::Json j;
    std::string err;
    if (hsynth::Json::parse(ss.str(), j, err) && j.isObject())
        if (auto n = j.find("name"); n && n->isString()) return n->string();
    return pathToUtf8(f.stem()) + " (invalid)";
}
}  // namespace

HostLibrary::HostLibrary(fs::path dataDir, fs::path factorySource)
    : data_(std::move(dataDir)), factorySrc_(std::move(factorySource)) {}

std::string HostLibrary::syncFactory() {
    std::error_code ec;
    fs::create_directories(data_ / "library" / "user", ec);
    fs::create_directories(userPresetDir(), ec);
    if (!fs::exists(factorySrc_ / "library" / "factory")) return "factory content not found at " + pathToUtf8(factorySrc_);
    // Copy only newer files, in place (deleting first can fail on Windows while a scanner
    // holds a file open) and keep going past individual failures.
    std::string errors;
    for (auto& [src, dst] : {std::pair{factorySrc_ / "library" / "factory", data_ / "library" / "factory"},
                             std::pair{factorySrc_ / "presets", data_ / "presets" / "factory"}}) {
        std::error_code e2;
        fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::update_existing, e2);
        if (e2) errors += "copy " + pathToUtf8(src.filename()) + " failed: " + e2.message() + "; ";
    }
    return errors;
}

std::vector<HostLibrary::Entry> HostLibrary::presets() const {
    std::vector<Entry> f, u;
    for (auto& [dir, list, user] : {std::tuple{data_ / "presets" / "factory", &f, false}, std::tuple{userPresetDir(), &u, true}}) {
        for (const auto& e : listDir(dir))
            if (!e.dir && ext(e.path) == ".json") list->push_back({e.path, presetTitle(e.path), user});
    }
    std::sort(f.begin(), f.end(), [](auto& a, auto& b) { return a.file.filename() < b.file.filename(); });
    std::sort(u.begin(), u.end(), [](auto& a, auto& b) { return lower(a.title) < lower(b.title); });
    f.insert(f.end(), u.begin(), u.end());
    return f;
}

std::vector<HostLibrary::Entry> HostLibrary::sampleSources() const {
    std::vector<Entry> out;
    const fs::path root = libraryRoot();
    const fs::path userRoot = root / "user";
    std::error_code ec;
    if (!fs::exists(root)) return out;
    const std::string rootStr = pathToUtf8(root) + "/";
    const std::string userStr = pathToUtf8(userRoot);
    // Every SFZ, and the WAVs that do not belong to an SFZ instrument (one in the same
    // folder or one level up). One listing per folder, depth first.
    std::vector<std::pair<fs::path, bool>> stack{{root, false}};  // folder, its parent has an SFZ
    while (!stack.empty()) {
        const auto [dir, parentSfz] = stack.back();
        stack.pop_back();
        const std::vector<DirItem> items = listDir(dir);
        bool hasSfz = false;
        for (const auto& it : items)
            if (!it.dir && ext(it.path) == ".sfz") { hasSfz = true; break; }
        for (const auto& it : items) {
            if (it.dir) { stack.push_back({it.path, hasSfz}); continue; }
            const std::string e = ext(it.path);
            if (e != ".sfz" && (e != ".wav" || hasSfz || parentSfz)) continue;
            // lexical prefix strip: fs::relative() queries the file system per call
            std::string rel = pathToUtf8(it.path);
            const bool user = rel.rfind(userStr, 0) == 0;
            if (rel.rfind(rootStr, 0) == 0) rel = rel.substr(rootStr.size());
            out.push_back({it.path, rel, user});
        }
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) {
        return a.user != b.user ? !a.user : lower(a.title) < lower(b.title);
    });
    return out;
}

std::string HostLibrary::safeName(const std::string& name) {
    std::string s;
    for (unsigned char c : name) {
        if (c >= 0x80 || std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == ' ' || c == '(' || c == ')') s.push_back((char)c);
        else s.push_back('_');
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s.empty() ? "untitled" : s;
}

fs::path HostLibrary::importWav(const fs::path& src) {
    if (ext(src) != ".wav") throw std::runtime_error(pathToUtf8(src.filename()) + " is not a .wav file");
    const fs::path dir = libraryRoot() / "user" / "wav";
    fs::create_directories(dir);
    const fs::path dst = uniquePath(dir, safeName(pathToUtf8(src.filename())));
    fs::copy_file(src, dst);
    return dst;
}

fs::path HostLibrary::importIr(const fs::path& src) {
    if (ext(src) != ".wav") throw std::runtime_error(pathToUtf8(src.filename()) + " is not a .wav file");
    const std::string root = pathToUtf8(libraryRoot()) + "/";
    if (pathToUtf8(src).rfind(root, 0) == 0) return src;
    const fs::path dir = libraryRoot() / "user" / "ir";
    fs::create_directories(dir);
    const fs::path same = dir / utf8ToPath(safeName(pathToUtf8(src.filename())));
    std::error_code ec;
    if (fs::exists(same) && fs::file_size(same, ec) == fs::file_size(src, ec)) return same;  // imported before
    const fs::path dst = uniquePath(dir, safeName(pathToUtf8(src.filename())));
    fs::copy_file(src, dst);
    return dst;
}

std::vector<fs::path> HostLibrary::importFolder(const fs::path& src, int& filesCopied) {
    filesCopied = 0;
    std::vector<fs::path> sfz;
    if (!fs::is_directory(src)) throw std::runtime_error("not a folder: " + pathToUtf8(src));
    const fs::path dstRoot = uniquePath(libraryRoot() / "user", safeName(pathToUtf8(src.filename())));
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(src, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it.depth() > 8) { it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file()) continue;
        const std::string e = ext(it->path());
        if (e != ".sfz" && e != ".wav" && e != ".txt" && e != ".sfzh") continue;
        const fs::path dst = dstRoot / fs::relative(it->path(), src);
        fs::create_directories(dst.parent_path());
        fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing);
        ++filesCopied;
        if (e == ".sfz") sfz.push_back(dst);
    }
    if (filesCopied == 0) fs::remove_all(dstRoot, ec);
    return sfz;
}

fs::path HostLibrary::importPreset(const fs::path& src) {
    std::ifstream in(src, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    hsynth::Json j;
    std::string err;
    if (!hsynth::Json::parse(ss.str(), j, err)) throw std::runtime_error("invalid JSON: " + err);
    fs::create_directories(userPresetDir());
    std::string name = safeName(pathToUtf8(src.filename()));
    if (ext(utf8ToPath(name)) != ".json") name += ".json";
    const fs::path dst = uniquePath(userPresetDir(), name);
    fs::copy_file(src, dst);
    return dst;
}

fs::path HostLibrary::userPresetFile(const std::string& name) const {
    fs::create_directories(userPresetDir());
    return userPresetDir() / utf8ToPath(safeName(name) + ".json");
}
