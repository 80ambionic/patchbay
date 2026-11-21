#pragma once
// Mixer DSP core. Pure C++ (no JUCE), allocation-free and lock-free in process(), unit-tested.
//
//   input channel i -> strip i: fader gain -> mute/solo -> constant-power pan -> stereo sum -> master gain -> clamp
//                                                      \-> or, if the strip has a direct out: post-fader mono -> device channel
//
// Inputs come in two banks of interleaved audio:
//   network bank -> strips 1-8   (a 1-channel stream feeds strips 1 and 2 so a mono source still plays on both sides)
//   app bank     -> strips 9-16  (strip 9 + k takes app channel k; no mono duplication - app captures are always stereo)
//   input bank   -> strips 17-24 (strip 17 + k takes audio-input-device channel k)
//
// Rules:
//   - Strips without an input channel are silent. Input channels beyond a bank's strips are ignored.
//   - If any strip is soloed, strips that are not soloed are silent. Mute always wins over solo.
//   - Pan law: constant power (-3 dB at centre).
//   - A strip with a direct out bypasses pan and master and is written (post-fader, mute/solo applied) to that strip's
//     buffer in `direct`; the chosen device channel is reported through `routing`. Direct outs are ignored when the caller
//     passes no `direct` buffer for that strip, and then the strip plays through the master as usual.
//   - Gain changes ramp linearly across the block, so moving a fader or switching a route never clicks. The first block
//     after reset() jumps straight to the targets.
//   - Non-finite input samples are replaced by 0; the master output is clamped to +-1.
//   - Strip meters are post-fader, pre-mute, pre-pan.
//
// Threading: set*() may be called from any thread while process() runs on the audio thread (atomics only).
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include "MixerSettings.h"

namespace pb
{
class MixerCore
{
public:
    struct Peaks
    {
        std::array<float, kMixerStrips> strip {};
        float masterL = 0.0f, masterR = 0.0f;
        bool clipped = false;
    };

    struct Input { const float* data = nullptr; int channels = 0; };   // interleaved

    /** What process() decided for this block; the device router needs exactly this, not a fresh read of the atomics. */
    struct Routing
    {
        int masterFirst = 1;                                   // 1-based device channel of master L (R is the next one)
        std::array<int, kMixerStrips> directCh {};             // 0 = strip not written to `direct` this block
    };

    MixerCore() { setSettings (MixerSettings()); }

    // ---- control side (any thread) -----------------------------------------------------------------------------
    void setSettings (const MixerSettings& in)
    {
        MixerSettings s = in;
        s.sanitize();
        for (int i = 0; i < kMixerStrips; ++i)
        {
            const auto& st = s.strips[(size_t) i];
            setStripGainDb (i, st.gainDb); setStripPan (i, st.pan); setStripMute (i, st.mute);
            setStripSolo (i, st.solo); setStripDirectOut (i, st.directOut);
        }
        setMasterDb (s.masterDb);
        setMasterMute (s.masterMute);
        setMasterOut (s.masterOut);
    }

    void setStripGainDb (int i, float db) noexcept { if (valid (i)) strip[(size_t) i].gainDb.store (clampGainDb (db), std::memory_order_relaxed); }
    void setStripPan    (int i, float p)  noexcept { if (valid (i)) strip[(size_t) i].pan.store (clampPan (p), std::memory_order_relaxed); }
    void setStripMute   (int i, bool m)   noexcept { if (valid (i)) strip[(size_t) i].mute.store (m, std::memory_order_relaxed); }
    void setStripSolo   (int i, bool s)   noexcept { if (valid (i)) strip[(size_t) i].solo.store (s, std::memory_order_relaxed); }
    void setStripDirectOut (int i, int ch) noexcept { if (valid (i)) strip[(size_t) i].directOut.store (clampDirectOut (ch), std::memory_order_relaxed); }
    void setMasterDb (float db) noexcept { masterDb.store (clampGainDb (db), std::memory_order_relaxed); }
    void setMasterMute (bool m) noexcept { masterMute.store (m, std::memory_order_relaxed); }
    void setMasterOut (int firstCh) noexcept { masterOut.store (clampMasterOut (firstCh), std::memory_order_relaxed); }

    /** Peaks since the previous call (the call resets them). Call from the UI thread. */
    Peaks takePeaks() noexcept
    {
        Peaks p;
        for (int i = 0; i < kMixerStrips; ++i) p.strip[(size_t) i] = stripPeak[(size_t) i].exchange (0.0f, std::memory_order_relaxed);
        p.masterL = peakL.exchange (0.0f, std::memory_order_relaxed);
        p.masterR = peakR.exchange (0.0f, std::memory_order_relaxed);
        p.clipped = clipped.exchange (false, std::memory_order_relaxed);
        return p;
    }

    // ---- audio side (one thread) -------------------------------------------------------------------------------
    /** Next block jumps to the current targets instead of ramping (call when the device restarts). */
    void reset() noexcept { snapped = false; }

    /** Network bank only (strips 1-8); everything plays through the master. */
    void process (const float* in, int inChannels, int frames, float* outL, float* outR) noexcept
    {
        process (Input { in, inChannels }, Input {}, frames, outL, outR, nullptr, nullptr);
    }

    /** outL/outR are overwritten with `frames` samples. `direct` (optional) is kMixerStrips buffers of `frames` samples;
        only the ones reported in routing->directCh are written. */
    void process (Input net, Input app, int frames, float* outL, float* outR,
                  float* const* direct, Routing* routing) noexcept
    {
        process (net, app, Input {}, frames, outL, outR, direct, routing);
    }

    /** All three banks. */
    void process (Input net, Input app, Input dev, int frames, float* outL, float* outR,
                  float* const* direct, Routing* routing) noexcept
    {
        if (frames <= 0) return;
        std::fill (outL, outL + frames, 0.0f);
        std::fill (outR, outR + frames, 0.0f);

        // ---- targets for this block
        bool anySolo = false;
        for (auto& s : strip) anySolo = anySolo || s.solo.load (std::memory_order_relaxed);

        std::array<float, kMixerStrips> faderLin {}, tgtL {}, tgtR {}, tgtD {};
        std::array<int, kMixerStrips> dch {};
        for (int i = 0; i < kMixerStrips; ++i)
        {
            auto& s = strip[(size_t) i];
            const size_t k = (size_t) i;
            const float g = gainDbToLinear (s.gainDb.load (std::memory_order_relaxed));
            const bool audible = ! s.mute.load (std::memory_order_relaxed)
                                 && (! anySolo || s.solo.load (std::memory_order_relaxed));
            const int want = s.directOut.load (std::memory_order_relaxed);
            const bool useDirect = want > 0 && direct != nullptr && direct[i] != nullptr;
            const float angle = (s.pan.load (std::memory_order_relaxed) + 1.0f) * (kPi * 0.25f);   // 0 .. pi/2

            faderLin[k] = g;
            dch[k] = useDirect ? want : 0;
            tgtD[k] = useDirect && audible ? g : 0.0f;
            tgtL[k] = ! useDirect && audible ? g * std::cos (angle) : 0.0f;
            tgtR[k] = ! useDirect && audible ? g * std::sin (angle) : 0.0f;
        }
        const float tgtMaster = masterMute.load (std::memory_order_relaxed)
                                    ? 0.0f : gainDbToLinear (masterDb.load (std::memory_order_relaxed));

        if (routing != nullptr)
        {
            routing->masterFirst = masterOut.load (std::memory_order_relaxed);
            routing->directCh = dch;
        }

        if (! snapped)
        {
            curL = tgtL; curR = tgtR; curD = tgtD; curMaster = tgtMaster; snapped = true;
        }

        // ---- mix
        const float invFrames = 1.0f / (float) frames;
        for (int i = 0; i < kMixerStrips; ++i)
        {
            const size_t k = (size_t) i;
            const bool isNet = i < kNetworkStrips;
            const Input& bank = isNet ? net : (i < kNetworkStrips + kAppStrips ? app : dev);
            const int local = isNet ? i : (i < kNetworkStrips + kAppStrips ? i - kNetworkStrips : i - kNetworkStrips - kAppStrips);
            const int srcCh = (isNet && bank.channels == 1) ? (local < 2 ? 0 : -1) : (local < bank.channels ? local : -1);

            float* dbuf = dch[k] > 0 ? direct[i] : nullptr;
            const float dL = (tgtL[k] - curL[k]) * invFrames, dR = (tgtR[k] - curR[k]) * invFrames, dD = (tgtD[k] - curD[k]) * invFrames;
            float gl = curL[k], gr = curR[k], gd = curD[k];

            if (srcCh >= 0 && bank.data != nullptr)
            {
                float peak = 0.0f;
                for (int n = 0; n < frames; ++n)
                {
                    const float x = clean (bank.data[(size_t) n * (size_t) bank.channels + (size_t) srcCh]);
                    peak = std::max (peak, std::abs (x));
                    gl += dL; gr += dR; gd += dD;
                    outL[n] += x * gl;
                    outR[n] += x * gr;
                    if (dbuf != nullptr) dbuf[n] = x * gd;
                }
                raise (stripPeak[k], peak * faderLin[k]);
            }
            else if (dbuf != nullptr)
                std::fill (dbuf, dbuf + frames, 0.0f);

            curL[k] = tgtL[k]; curR[k] = tgtR[k]; curD[k] = tgtD[k];   // land exactly on the target
        }

        // ---- master
        const float dM = (tgtMaster - curMaster) * invFrames;
        float gm = curMaster;
        float pl = 0.0f, pr = 0.0f;
        bool clip = false;
        for (int n = 0; n < frames; ++n)
        {
            gm += dM;
            float l = outL[n] * gm, r = outR[n] * gm;
            if (l > 1.0f || l < -1.0f) { clip = true; l = l > 0.0f ? 1.0f : -1.0f; }
            if (r > 1.0f || r < -1.0f) { clip = true; r = r > 0.0f ? 1.0f : -1.0f; }
            outL[n] = l; outR[n] = r;
            pl = std::max (pl, std::abs (l)); pr = std::max (pr, std::abs (r));
        }
        curMaster = tgtMaster;
        raise (peakL, pl);
        raise (peakR, pr);
        if (clip) clipped.store (true, std::memory_order_relaxed);
    }

    /** Lets the engine report a clamp that happened in the device router (direct outs summing past full scale). */
    void noteClip() noexcept { clipped.store (true, std::memory_order_relaxed); }

private:
    static constexpr float kPi = 3.14159265358979323846f;

    static bool valid (int i) noexcept { return i >= 0 && i < kMixerStrips; }
    static float clean (float x) noexcept { return std::isfinite (x) ? x : 0.0f; }
    static void raise (std::atomic<float>& a, float v) noexcept
    {
        float cur = a.load (std::memory_order_relaxed);
        while (v > cur && ! a.compare_exchange_weak (cur, v, std::memory_order_relaxed)) {}
    }

    struct Strip
    {
        std::atomic<float> gainDb { 0.0f }, pan { 0.0f };
        std::atomic<bool> mute { false }, solo { false };
        std::atomic<int> directOut { 0 };
    };
    static_assert (std::atomic<float>::is_always_lock_free && std::atomic<bool>::is_always_lock_free
                   && std::atomic<int>::is_always_lock_free, "audio-thread atomics must be lock-free");

    std::array<Strip, kMixerStrips> strip;
    std::atomic<float> masterDb { 0.0f };
    std::atomic<bool> masterMute { false };
    std::atomic<int> masterOut { 1 };

    std::array<std::atomic<float>, kMixerStrips> stripPeak {};
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };
    std::atomic<bool> clipped { false };

    // Audio-thread state.
    std::array<float, kMixerStrips> curL {}, curR {}, curD {};
    float curMaster = 1.0f;
    bool snapped = false;
};
}
