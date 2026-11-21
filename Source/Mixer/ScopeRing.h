#pragma once
// Wait-free "latest samples" tap from the audio thread to the analyzer on the UI thread.
//   Producer (audio): push() copies samples in and publishes the write position. Never blocks, never allocates.
//   Consumer (UI):    latest() copies the newest n frames. If the producer lapped the reader mid-copy the copy is
//                     simply retried once; an analyzer can tolerate the odd torn frame, so it never waits.
#include <atomic>
#include <cstdint>
#include <memory>

namespace pb
{
class ScopeRing
{
public:
    static constexpr uint32_t kCapacity = 1u << 15;        // frames (0.68 s at 48 kHz), power of two
    static constexpr uint32_t kMask = kCapacity - 1;

    // Samples are relaxed atomics: no cost on x86/ARM, and a reader racing the writer is then well-defined (it may see a
    // mix of old and new frames, which an analyzer tolerates) instead of undefined behaviour.
    ScopeRing() : l (new std::atomic<float>[kCapacity]), r (new std::atomic<float>[kCapacity])
    {
        for (uint32_t i = 0; i < kCapacity; ++i) { l[i].store (0.0f, std::memory_order_relaxed); r[i].store (0.0f, std::memory_order_relaxed); }
    }

    void push (const float* inL, const float* inR, int n) noexcept
    {
        uint64_t w = writePos.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i, ++w) { l[(size_t) (w & kMask)].store (inL[i], std::memory_order_relaxed); r[(size_t) (w & kMask)].store (inR[i], std::memory_order_relaxed); }
        writePos.store (w, std::memory_order_release);
    }

    /** Sample-by-sample variant for taps that compute their values inside the mixer loop. */
    void pushOne (float a, float b) noexcept
    {
        const uint64_t w = writePos.load (std::memory_order_relaxed);
        l[(size_t) (w & kMask)].store (a, std::memory_order_relaxed); r[(size_t) (w & kMask)].store (b, std::memory_order_relaxed);
        writePos.store (w + 1, std::memory_order_release);
    }

    uint64_t written() const noexcept { return writePos.load (std::memory_order_acquire); }

    /** Copies the newest `n` (<= kCapacity / 2) frames, oldest first. Returns false if fewer than n have ever been written. */
    bool latest (float* outL, float* outR, uint32_t n) const noexcept
    {
        if (n > kCapacity / 2) return false;
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const uint64_t w = writePos.load (std::memory_order_acquire);
            if (w < n) return false;
            const uint64_t start = w - n;
            for (uint32_t i = 0; i < n; ++i) { outL[i] = l[(size_t) ((start + i) & kMask)].load (std::memory_order_relaxed); outR[i] = r[(size_t) ((start + i) & kMask)].load (std::memory_order_relaxed); }
            if (writePos.load (std::memory_order_acquire) - start <= kCapacity - 64) return true;   // not overwritten while copying
        }
        return true;
    }

private:
    std::unique_ptr<std::atomic<float>[]> l, r;
    std::atomic<uint64_t> writePos { 0 };
};
}
