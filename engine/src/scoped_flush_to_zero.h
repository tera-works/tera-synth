#pragma once

#include <cstdint>

#if defined(__SSE__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace hsynth {

// Restore the caller's FP mode when leaving the render block or worker job.
class ScopedFlushToZero {
public:
    ScopedFlushToZero() {
#if defined(__aarch64__)
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(previous_));
        if (!(previous_ & kMask)) __asm__ __volatile__("msr fpcr, %0" : : "r"(previous_ | kMask) : "memory");
#elif defined(__SSE__) || defined(_M_X64)
        previous_ = _mm_getcsr();
        if ((previous_ & kMask) != kMask) _mm_setcsr(previous_ | kMask);
#endif
    }
    ~ScopedFlushToZero() {
#if defined(__aarch64__)
        if (!(previous_ & kMask)) __asm__ __volatile__("msr fpcr, %0" : : "r"(previous_) : "memory");
#elif defined(__SSE__) || defined(_M_X64)
        if ((previous_ & kMask) != kMask) _mm_setcsr(previous_);
#endif
    }
    ScopedFlushToZero(const ScopedFlushToZero&) = delete;
    ScopedFlushToZero& operator=(const ScopedFlushToZero&) = delete;

private:
#if defined(__aarch64__)
    static constexpr uint64_t kMask = 1ull << 24;
    uint64_t previous_;
#elif defined(__SSE__) || defined(_M_X64)
    static constexpr unsigned kMask = 0x8040;  // FTZ | DAZ
    unsigned previous_;
#endif
};

}  // namespace hsynth
