#pragma once

#include "hsynth/flac.h"
#include "hsynth/streaming.h"

namespace hsynth {

struct WavInfo {
    int format = 0, bits = 0, fileChannels = 0, blockAlign = 0;
    double rate = 0;
    int64_t dataOffset = -1, frames = 0;
    int unity = -1;
    bool hasLoop = false;
};

int64_t tellFile(FILE* f);
bool seekFile(FILE* f, int64_t offset, int origin = SEEK_SET);
bool readWavInfo(FILE* f, WavInfo& info);

struct PcmFrame {
    float l = 0, r = 0;
};

struct ClipInfo {
    std::string name;
    int64_t frames = 0;
    int sr = 0, ch = 0;
    double duration() const { return sr > 0 ? double(frames) / sr : 0; }
};

// Worker-side only. WAV uses bounded disk reads. FLAC uses file-backed virtual
// memory and decodes one frame at a time with sparse seek checkpoints.
class PcmFile {
public:
    ~PcmFile();
    bool open(const std::string& path, std::string& error);
    int read(int64_t first, int count, float* left, float* right);
    const ClipInfo& info() const { return info_; }

private:
    const uint8_t* mapped_ = nullptr;
    size_t mapSize_ = 0;
    std::unique_ptr<FILE, int (*)(FILE*)> file_{nullptr, &std::fclose};
    ClipInfo info_;
    StreamSource wav_;
    std::vector<uint8_t> scratch_;
    FlacFrameDecoder flac_;
    struct Checkpoint { int64_t frame; size_t byte; };
    std::vector<Checkpoint> index_;
    std::vector<float> decodedL_, decodedR_;
    int64_t decodedStart_ = 0;
    bool flacFailed_ = false;
};

int clampPeakBins(int bins);
std::vector<float> pcmPeaks(const float* left, const float* right, int64_t frames, int bins);

}  // namespace hsynth
