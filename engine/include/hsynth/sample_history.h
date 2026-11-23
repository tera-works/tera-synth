#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace hsynth {

// One producer and one polling consumer. Each slot carries its sequence number,
// so a reader racing a wrap can reject the frame without data races or retries
// on the producer. A generation change invalidates the preceding input session.
template <size_t Capacity>
class SampleHistory {
    static_assert((Capacity & (Capacity - 1)) == 0);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static constexpr uint64_t kIndexMask = (uint64_t{1} << 32) - 1;

public:
    void reset() { generation_.fetch_add(1, std::memory_order_acq_rel); }

    void push(const float* left, const float* right, int frames) {
        const uint32_t generation = generation_.load(std::memory_order_acquire);
        if (generation != writingGeneration_) {
            writingGeneration_ = generation;
            count_ = 0;
        }
        frames = std::max(0, frames);
        const int first = std::max(0, frames - static_cast<int>(Capacity));
        for (int i = first; i < frames; ++i) {
            const float sample = !left ? 0.0f : right && right != left
                ? 0.5f * left[i] + 0.5f * right[i] : left[i];
            uint32_t bits;
            std::memcpy(&bits, &sample, sizeof bits);
            const uint32_t index = end_ + static_cast<uint32_t>(i);
            slots_[index & (Capacity - 1)].store(pack(index, bits), std::memory_order_relaxed);
        }
        end_ += static_cast<uint32_t>(frames);
        count_ = std::min(static_cast<uint32_t>(Capacity), count_ + static_cast<uint32_t>(frames));
        countPublished_.store(pack(end_, count_), std::memory_order_relaxed);
        published_.store(pack(generation, end_), std::memory_order_release);
    }

    int latest(float* out, int maxFrames, uint64_t& stamp) const {
        const uint64_t published = published_.load(std::memory_order_acquire);
        const uint32_t generation = static_cast<uint32_t>(published >> 32);
        if (generation != generation_.load(std::memory_order_acquire)) return 0;
        const uint32_t end = static_cast<uint32_t>(published & kIndexMask);
        const uint64_t countStamp = countPublished_.load(std::memory_order_relaxed);
        if (static_cast<uint32_t>(countStamp >> 32) != end) return 0;
        const int count = std::min(maxFrames, static_cast<int>(countStamp & kIndexMask));
        for (int i = 0; i < count; ++i) {
            const uint32_t index = end - static_cast<uint32_t>(count - i);
            const uint64_t slot = slots_[index & (Capacity - 1)].load(std::memory_order_relaxed);
            if (static_cast<uint32_t>(slot >> 32) != index) return 0;
            const uint32_t bits = static_cast<uint32_t>(slot & kIndexMask);
            std::memcpy(out + i, &bits, sizeof bits);
        }
        if (generation != generation_.load(std::memory_order_acquire)) return 0;
        stamp = published;
        return count;
    }

private:
    static uint64_t pack(uint32_t high, uint32_t low) { return (uint64_t{high} << 32) | low; }

    std::array<std::atomic<uint64_t>, Capacity> slots_{};
    std::atomic<uint32_t> generation_{1};
    std::atomic<uint64_t> countPublished_{0};
    std::atomic<uint64_t> published_{0};
    uint32_t writingGeneration_ = 0, end_ = 0, count_ = 0;
};

}  // namespace hsynth
