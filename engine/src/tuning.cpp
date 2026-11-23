#include "hsynth/tuning.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "hsynth/fileio.h"

namespace hsynth {

namespace {
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}
}  // namespace

bool parseScala(const std::string& text, ScalaScale& out, std::string& error) {
    std::istringstream in(text);
    std::string line;
    int stage = 0;  // 0 = description, 1 = count, 2 = pitches
    long count = 0;
    ScalaScale s;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (!t.empty() && t[0] == '!') continue;
        if (stage == 0) { s.description = t; stage = 1; continue; }
        if (t.empty()) continue;
        if (stage == 1) {
            char* end;
            count = std::strtol(t.c_str(), &end, 10);
            if (count <= 0 || count > 1024 || (*end && *end != ' ' && *end != '\t')) { error = "bad note count"; return false; }
            stage = 2;
            continue;
        }
        // the pitch is the first token; anything after it is a comment
        const std::string tok = t.substr(0, t.find_first_of(" \t"));
        double cents;
        char* end;
        if (tok.find('.') != std::string::npos && tok.find('/') == std::string::npos) {
            cents = std::strtod(tok.c_str(), &end);
            if (end == tok.c_str() || *end) { error = "bad pitch '" + tok + "'"; return false; }
        } else {
            const size_t slash = tok.find('/');
            const char* start = tok.c_str();
            const double num = std::strtod(start, &end);
            if (end == start || (size_t)(end - start) != (slash == std::string::npos ? tok.size() : slash)) {
                error = "bad ratio '" + tok + "'";
                return false;
            }
            const char* denominator = slash == std::string::npos ? end : start + slash + 1;
            const double den = slash == std::string::npos ? 1.0 : std::strtod(denominator, &end);
            if (*end || (slash != std::string::npos && end == denominator) || num <= 0.0 || den <= 0.0) { error = "bad ratio '" + tok + "'"; return false; }
            cents = 1200.0 * std::log2(num / den);
        }
        if (!std::isfinite(cents)) { error = "non-finite pitch '" + tok + "'"; return false; }
        s.cents.push_back(cents);
        if ((long)s.cents.size() == count) break;
    }
    if (stage < 2 || (long)s.cents.size() != count) { error = "truncated scale"; return false; }
    out = std::move(s);
    return true;
}

bool loadScalaFile(const std::string& path, ScalaScale& out, std::string& error) {
    std::ifstream f(utf8Path(path), std::ios::binary);
    if (!f) { error = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    if (!parseScala(ss.str(), out, error)) { error = path + ": " + error; return false; }
    return true;
}

bool scalaToOffsets(const ScalaScale& s, int root, float offsets[12], std::string& error) {
    if (s.cents.size() != 12 || std::fabs(s.cents.back() - 1200.0) > 0.5) {
        error = "only 12-note scales with a 1200-cent octave can be mapped to the keyboard (" +
                std::to_string(s.cents.size()) + " notes)";
        return false;
    }
    double previous = 0.0;
    for (double cents : s.cents) {
        if (!std::isfinite(cents) || cents <= previous) {
            error = "scale degrees must be finite and strictly ascending for keyboard mapping";
            return false;
        }
        previous = cents;
    }
    root = ((root % 12) + 12) % 12;
    for (int k = 0; k < 12; ++k) {
        const double c = k == 0 ? 0.0 : s.cents[(size_t)k - 1];
        const double off = c - 100.0 * k;
        offsets[(root + k) % 12] = (float)std::max(-100.0, std::min(100.0, off));
        if (std::fabs(off) > 100.0) error = "degree " + std::to_string(k) + " is more than 100 cents from 12-TET (clamped)";
    }
    return true;
}

}  // namespace hsynth
