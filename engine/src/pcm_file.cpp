#include "pcm_file.h"
#include "hsynth/fileio.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <sys/mman.h>
#endif

namespace hsynth {

int64_t tellFile(FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return ftello(f);
#endif
}

bool seekFile(FILE* f, int64_t offset, int origin) {
#ifdef _WIN32
    return _fseeki64(f, offset, origin) == 0;
#else
    return fseeko(f, offset, origin) == 0;
#endif
}

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool readWavInfo(FILE* f, WavInfo& w) {
    uint8_t h[12];
    if (std::fread(h, 1, 12, f) != 12 || std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0) return false;
    if (!seekFile(f, 0, SEEK_END)) return false;
    const int64_t size = tellFile(f);
    int64_t pos = 12, dataSize = 0;
    while (pos + 8 <= size) {
        if (!seekFile(f, pos)) return false;
        uint8_t ck[8];
        if (std::fread(ck, 1, 8, f) != 8) break;
        const int64_t ckSize = (int64_t)le32(ck + 4);
        const int64_t body = std::min(ckSize, size - pos - 8);
        if (std::memcmp(ck, "fmt ", 4) == 0 && body >= 16) {
            uint8_t b[40] = {};
            if (std::fread(b, 1, (size_t)std::min<int64_t>(body, 40), f) < 16) return false;
            w.format = b[0] | b[1] << 8;
            w.fileChannels = b[2] | b[3] << 8;
            w.rate = (double)le32(b + 4);
            w.blockAlign = b[12] | b[13] << 8;
            w.bits = b[14] | b[15] << 8;
            if (w.format == 0xFFFE && body >= 40) w.format = b[24] | b[25] << 8;
        } else if (std::memcmp(ck, "data", 4) == 0) {
            w.dataOffset = pos + 8;
            dataSize = body;
        } else if (std::memcmp(ck, "smpl", 4) == 0 && body >= 36) {
            uint8_t b[36];
            if (std::fread(b, 1, 36, f) == 36) {
                w.unity = (int)le32(b + 12);
                w.hasLoop = le32(b + 28) > 0;
            }
        }
        pos += 8 + ckSize + (ckSize & 1);
    }
    const bool fmtOk = (w.format == 1 && (w.bits == 8 || w.bits == 16 || w.bits == 24 || w.bits == 32)) ||
                       (w.format == 3 && (w.bits == 32 || w.bits == 64));
    if (w.dataOffset < 0 || !fmtOk || w.fileChannels <= 0 || w.rate <= 0 || w.blockAlign < w.bits / 8 * w.fileChannels)
        return false;
    w.frames = dataSize / w.blockAlign;
    return true;
}


PcmFile::~PcmFile() {
    if (!mapped_) return;
#ifdef _WIN32
    UnmapViewOfFile(mapped_);
#else
    munmap(const_cast<uint8_t*>(mapped_), mapSize_);
#endif
}

bool PcmFile::open(const std::string& path, std::string& error) {
    file_.reset(fopenUtf8(path, "rb"));
    if (!file_) { error = "Could not open audio file"; return false; }
    char magic[4];
    if (std::fread(magic, 1, sizeof magic, file_.get()) != sizeof magic || !seekFile(file_.get(), 0)) {
        error = "Could not read audio header";
        return false;
    }
    info_.name = path.substr(path.find_last_of("/\\") + 1);
    if (std::memcmp(magic, "fLaC", 4) == 0) {
        if (!seekFile(file_.get(), 0, SEEK_END)) { error = "Could not size FLAC"; return false; }
        const auto size = tellFile(file_.get());
        if (size <= 0 || uint64_t(size) > SIZE_MAX) { error = "Unsupported FLAC file size"; return false; }
        mapSize_ = static_cast<size_t>(size);
#ifdef _WIN32
        HANDLE mapping = CreateFileMappingW(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file_.get()))),
                                            nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping) {
            mapped_ = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
            CloseHandle(mapping);
        }
#else
        void* mapping = mmap(nullptr, mapSize_, PROT_READ, MAP_PRIVATE, fileno(file_.get()), 0);
        if (mapping != MAP_FAILED) mapped_ = static_cast<const uint8_t*>(mapping);
#endif
        if (!mapped_) { error = "Could not map FLAC file"; return false; }
        if (!flac_.open(mapped_, mapSize_, error)) return false;
        info_.frames = flac_.totalFrames();
        info_.ch = flac_.channels();
        info_.sr = flac_.sampleRate();
        index_.push_back({0, flac_.bytePosition()});
        // STREAMINFO may omit the frame count. Walk frame headers and samples
        // once in that case so duration and bounds remain consistent.
        if (info_.frames == 0) {
            int got = 0;
            while ((got = flac_.next(decodedL_, decodedR_, error)) > 0) {
                if (flac_.position() - index_.back().frame >= int64_t(info_.sr) * 4)
                    index_.push_back({flac_.position(), flac_.bytePosition()});
            }
            if (got < 0) return false;
            info_.frames = flac_.position();
            flac_.seekFrame(index_.front().byte, 0);
            decodedL_.clear();
        } else {
            // Seek-table offsets are relative to the first FLAC audio frame.
            // Leave room between checkpoints even on files with dense tables.
            const size_t audioStart = flac_.bytePosition();
            auto be64 = [](const uint8_t* p) {
                uint64_t n = 0;
                for (int i = 0; i < 8; ++i) n = (n << 8) | p[i];
                return n;
            };
            for (size_t pos = 4; pos + 4 <= audioStart;) {
                const auto* block = mapped_ + pos;
                const size_t bytes = size_t(block[1]) << 16 | size_t(block[2]) << 8 | block[3];
                pos += 4;
                if (bytes > audioStart - pos) break;
                if ((block[0] & 0x7f) == 3) {
                    for (size_t i = pos; i + 18 <= pos + bytes; i += 18) {
                        const uint64_t frame = be64(mapped_ + i);
                        const uint64_t offset = be64(mapped_ + i + 8);
                        if (frame >= uint64_t(info_.frames) || offset >= mapSize_ - audioStart ||
                            frame < uint64_t(index_.back().frame) + info_.sr) continue;
                        index_.push_back({int64_t(frame), audioStart + size_t(offset)});
                    }
                }
                pos += bytes;
            }
        }
    } else {
        WavInfo w;
        if (!readWavInfo(file_.get(), w)) { error = "Unsupported WAV/FLAC file"; return false; }
        wav_.dataOffset = w.dataOffset;
        wav_.format = w.format;
        wav_.bits = w.bits;
        wav_.blockAlign = w.blockAlign;
        wav_.fileChannels = w.fileChannels;
        info_.frames = w.frames;
        info_.sr = static_cast<int>(std::min(w.rate, 1000000.0));
        info_.ch = std::min(w.fileChannels, 2);
    }
    if (info_.sr < 1000 || info_.sr > 768000 || info_.frames <= 0) {
        error = "Unsupported audio rate or empty file";
        return false;
    }
    return true;
}

int PcmFile::read(int64_t first, int count, float* left, float* right) {
    if (first < 0 || first >= info_.frames || count <= 0) return 0;
    count = static_cast<int>(std::min<int64_t>(count, info_.frames - first));
    if (!mapped_) {
        if (!readStreamFrames(file_.get(), wav_, first, count, left, info_.ch == 2 ? right : nullptr, scratch_))
            return 0;
        if (info_.ch == 1) std::copy_n(left, count, right);
        return count;
    }
    int out = 0;
    const auto point = std::upper_bound(index_.begin(), index_.end(), first,
                                        [](int64_t n, const Checkpoint& c) { return n < c.frame; });
    const auto& checkpoint = *std::prev(point);
    if (first < decodedStart_ || (first >= flac_.position() && checkpoint.frame > flac_.position())) {
        flac_.seekFrame(checkpoint.byte, checkpoint.frame);
        decodedStart_ = checkpoint.frame;
        decodedL_.clear();
        flacFailed_ = false;
    }
    while (out < count && !flacFailed_) {
        const int64_t target = first + out;
        if (target >= decodedStart_ + int64_t(decodedL_.size())) {
            decodedStart_ = flac_.position();
            if (decodedStart_ - index_.back().frame >= int64_t(info_.sr) * 4)
                index_.push_back({decodedStart_, flac_.bytePosition()});
            std::string error;
            if (flac_.next(decodedL_, decodedR_, error) <= 0) { flacFailed_ = true; break; }
            continue;
        }
        const auto offset = static_cast<size_t>(target - decodedStart_);
        const int size = std::min(count - out, static_cast<int>(decodedL_.size() - offset));
        std::copy_n(decodedL_.data() + offset, size, left + out);
        const auto& r = info_.ch == 2 ? decodedR_ : decodedL_;
        std::copy_n(r.data() + offset, size, right + out);
        out += size;
    }
    return out;
}

int clampPeakBins(int bins) { return std::clamp(bins, 0, 16384); }

std::vector<float> pcmPeaks(const float* left, const float* right, int64_t frames, int bins) {
    bins = clampPeakBins(bins);
    std::vector<float> out(size_t(bins) * 2, 0.0f);
    for (int i = 0; i < bins && frames > 0; ++i) {
        const int64_t first = i * frames / bins;
        const int64_t end = std::max(first + 1, (i + 1) * frames / bins);
        float lo = left[first], hi = lo;
        for (auto j = first; j < end; ++j) {
            lo = std::min({lo, left[j], right[j]});
            hi = std::max({hi, left[j], right[j]});
        }
        out[2*i] = lo;
        out[2*i+1] = hi;
    }
    return out;
}

}  // namespace hsynth
