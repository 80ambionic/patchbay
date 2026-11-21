#pragma once
// Rate-converting, drift-correcting bridge between a capture thread and the audio callback. Pure C++, unit-tested.
//
//   capture thread --write()--> lock-free stereo ring --process()--> Catmull-Rom resampler --> audio callback
//                                    |                                      ^
//                                    +-- fill level --> DriftController ----+  (ratio ~ 1 +/- 500 ppm)
//
// The capture side (e.g. a Core Audio process tap) runs on its own clock and block size, usually at a different
// sample rate from the output device. The resampler step is  srcRate / deviceRate * driftRatio : the first factor
// converts rates exactly, the second absorbs the small clock difference so the ring neither drains nor grows.
//
// Threading: write() on ONE capture thread, process()/prepare() on ONE audio thread, setSourceRate()/status() anywhere.
// No allocation, locks or syscalls after construction.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>
#include "../Net/DriftController.h"

namespace pb
{
class CaptureStream
{
public:
    static constexpr double kMinRate = 8000.0, kMaxRate = 384000.0;

    struct Status
    {
        bool primed = false;
        double fillFrames = 0, targetFrames = 0, ratioPpm = 0;
        uint64_t underruns = 0, overflows = 0, trims = 0;
    };

    /** capacityFrames must be a power of two (frames of stereo audio the ring can hold). */
    explicit CaptureStream (int capacityFrames = 1 << 15)
        : cap ((uint64_t) std::max (16, capacityFrames)), mask (cap - 1), ring ((size_t) cap * 2, 0.0f)
    {
        if ((cap & mask) != 0) { cap = 1; while (cap < (uint64_t) capacityFrames) cap <<= 1; mask = cap - 1; ring.assign ((size_t) cap * 2, 0.0f); }
    }

    // ---- control side ---------------------------------------------------------------------------------------------
    /** The capture device's sample rate. Out-of-range or non-finite values are ignored. */
    void setSourceRate (double hz) noexcept
    {
        if (std::isfinite (hz) && hz >= kMinRate && hz <= kMaxRate) srcRate.store (hz, std::memory_order_relaxed);
    }
    double getSourceRate() const noexcept { return srcRate.load (std::memory_order_relaxed); }

    /** Latency target in milliseconds of source audio (default 50): more rides out capture-thread scheduling jitter. */
    void setTargetMs (double ms) noexcept { if (std::isfinite (ms)) targetMs.store (std::clamp (ms, 10.0, 500.0), std::memory_order_relaxed); }

    // ---- capture thread -------------------------------------------------------------------------------------------
    /** Interleaved input with `channels` channels: mono is duplicated, 3+ channels use the first two. Frames that do not fit are dropped. */
    void write (const float* interleaved, int frames, int channels) noexcept
    {
        if (interleaved == nullptr || frames <= 0 || channels <= 0) return;
        const uint64_t w = wr.load (std::memory_order_relaxed);
        const uint64_t free = cap - (w - rd.load (std::memory_order_acquire));
        const uint64_t n = std::min<uint64_t> ((uint64_t) frames, free);
        if (n < (uint64_t) frames) overflows.fetch_add (1, std::memory_order_relaxed);

        for (uint64_t i = 0; i < n; ++i)
        {
            const float* in = interleaved + (size_t) i * (size_t) channels;
            float* dst = &ring[(size_t) ((w + i) & mask) * 2];
            dst[0] = clean (in[0]);
            dst[1] = channels >= 2 ? clean (in[1]) : dst[0];
        }
        wr.store (w + n, std::memory_order_release);
    }

    // ---- audio thread ---------------------------------------------------------------------------------------------
    /** Set the output device rate. Call when the device (re)starts, never concurrently with process(). */
    void prepare (double deviceRate)
    {
        dstRate = std::isfinite (deviceRate) && deviceRate >= kMinRate && deviceRate <= kMaxRate ? deviceRate : 48000.0;
        primed = false;
        phase = 0.0;
        hist.fill (0.0f);
        underrunPending = false;
        drift = net::DriftController (2400.0, dstRate, 30.0);   // its time base is the device rate
        pubPrimed.store (false, std::memory_order_relaxed);
    }

    /** Writes exactly `frames` interleaved stereo frames (silence while buffering or after an underrun). */
    void process (float* out, int frames) noexcept
    {
        const double sr = srcRate.load (std::memory_order_relaxed);
        if (sr <= 0.0 || dstRate <= 0.0)
        {
            std::fill (out, out + (size_t) frames * 2, 0.0f);
            return;
        }

        const double target = std::max (64.0, targetMs.load (std::memory_order_relaxed) * 0.001 * sr);
        uint64_t avail = wr.load (std::memory_order_acquire) - rd.load (std::memory_order_relaxed);

        // A big backlog (the audio thread stalled, or the source rate was misreported) is cheaper to drop than to play late.
        if (avail > (uint64_t) (4.0 * target) && avail > 1024)
        {
            rd.store (rd.load (std::memory_order_relaxed) + (avail - (uint64_t) target), std::memory_order_release);
            avail = (uint64_t) target;
            trims.fetch_add (1, std::memory_order_relaxed);
        }

        if (! primed)
        {
            if ((double) avail < target) { std::fill (out, out + (size_t) frames * 2, 0.0f); publish (avail, target); return; }
            for (int i = 0; i < 4; ++i) shiftIn();                                  // fill the interpolation history with real audio
            phase = 0.0;
            drift.reset ((double) (wr.load (std::memory_order_acquire) - rd.load (std::memory_order_relaxed)));
            primed = true;
        }

        drift.setTarget (target);
        drift.update ((double) (wr.load (std::memory_order_acquire) - rd.load (std::memory_order_relaxed)), frames);
        const double step = (sr / dstRate) * drift.ratio();

        for (int i = 0; i < frames; ++i)
        {
            while (phase >= 1.0) { shiftIn(); phase -= 1.0; }
            const float t = (float) phase;
            for (int c = 0; c < 2; ++c)
                out[(size_t) i * 2 + (size_t) c] = catmullRom (hist[(size_t) c], hist[(size_t) (2 + c)], hist[(size_t) (4 + c)], hist[(size_t) (6 + c)], t);
            phase += step;
        }

        if (underrunPending)                                                         // ran dry: go silent and re-buffer
        {
            underruns.fetch_add (1, std::memory_order_relaxed);
            underrunPending = false;
            primed = false;
        }
        publish (wr.load (std::memory_order_acquire) - rd.load (std::memory_order_relaxed), target);
    }

    Status status() const noexcept
    {
        Status s;
        s.primed = pubPrimed.load (std::memory_order_relaxed);
        s.fillFrames = pubFill.load (std::memory_order_relaxed);
        s.targetFrames = pubTarget.load (std::memory_order_relaxed);
        s.ratioPpm = pubPpm.load (std::memory_order_relaxed);
        s.underruns = underruns.load (std::memory_order_relaxed);
        s.overflows = overflows.load (std::memory_order_relaxed);
        s.trims = trims.load (std::memory_order_relaxed);
        return s;
    }

private:
    static float clean (float x) noexcept { return std::isfinite (x) ? x : 0.0f; }

    static float catmullRom (float y0, float y1, float y2, float y3, float t) noexcept
    {
        const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
        const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c = -0.5f * y0 + 0.5f * y2;
        return ((a * t + b) * t + c) * t + y1;
    }

    /** Slide the 4-frame history by one and append the next ring frame (or repeat the last one if the ring is empty). */
    void shiftIn() noexcept
    {
        std::copy (hist.begin() + 2, hist.end(), hist.begin());
        const uint64_t r = rd.load (std::memory_order_relaxed);
        if (r != wr.load (std::memory_order_acquire))
        {
            const float* src = &ring[(size_t) (r & mask) * 2];
            hist[6] = src[0]; hist[7] = src[1];
            rd.store (r + 1, std::memory_order_release);
        }
        else
        {
            hist[6] = hist[4]; hist[7] = hist[5];
            underrunPending = true;
        }
    }

    void publish (uint64_t fill, double target) noexcept
    {
        pubPrimed.store (primed, std::memory_order_relaxed);
        pubFill.store ((double) fill, std::memory_order_relaxed);
        pubTarget.store (target, std::memory_order_relaxed);
        pubPpm.store ((drift.ratio() - 1.0) * 1e6, std::memory_order_relaxed);
    }

    uint64_t cap, mask;
    std::vector<float> ring;                              // stereo interleaved
    alignas (64) std::atomic<uint64_t> wr { 0 };          // capture thread writes
    alignas (64) std::atomic<uint64_t> rd { 0 };          // audio thread writes
    std::atomic<double> srcRate { 0.0 }, targetMs { 50.0 };

    // Audio-thread state.
    double dstRate = 48000.0, phase = 0.0;
    bool primed = false, underrunPending = false;
    std::array<float, 8> hist {};                         // 4 stereo frames; interpolates between frames 1 and 2
    net::DriftController drift { 2400.0, 48000.0, 30.0 };

    std::atomic<uint64_t> underruns { 0 }, overflows { 0 }, trims { 0 };
    std::atomic<bool> pubPrimed { false };
    std::atomic<double> pubFill { 0 }, pubTarget { 0 }, pubPpm { 0 };
};
}
