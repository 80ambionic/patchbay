#pragma once
// Mixer DSP core. Pure C++ (no JUCE), allocation-free and lock-free in process(), unit-tested.
//
//   source -> strip (stereo): fader -> mute/solo -> balance -> master bus  or  output bus n
//   output bus n: fader -> mute -> clamp -> its destination (device output pair or network stream)
//   master:       fader -> mute -> clamp -> its destination
//
// Sources come in three banks of interleaved audio. A strip picks one stereo pair from one bank:
//   net   bank: the network stream, 1-8 channels  -> pairs 0-3 (channels 1-2, 3-4, ...)
//   app   bank: the four app-capture slots, always stereo -> pair k = slot k
//   input bank: the audio input device, up to 64 channels -> pairs 0-31
// A pair whose left channel does not exist is silent. A pair with only a left channel (a mono stream, a mono microphone,
// an odd channel count) plays that channel on both sides.
//
// Rules:
//   - If any strip is soloed, strips that are not soloed are silent. Mute always wins over solo. Solo spans all banks.
//   - Balance law: unity at the centre; turning it away from a side attenuates that side with a cos law
//     (-3 dB at the quarter point, silent at the end). The other side is untouched.
//   - A strip whose output is a bus plays on that bus; if the caller gives no buffers for that bus the strip plays on the
//     master. A bus is "active" for a block when at least one strip is routed to it; inactive buses are not written.
//   - Gain changes ramp linearly across the block, so moving a fader or switching a route never clicks. The first block
//     after reset() jumps straight to the targets. When a strip's output changes, it fades out of the old destination and
//     into the new one over one block.
//   - Non-finite input samples are replaced by 0; master and bus outputs are clamped to +-1 (the clip flag is raised).
//   - Strip meters are post-fader, pre-mute, pre-balance; bus and master meters are post-everything.
//
// Threading: set*() may be called from any thread while process() runs on the audio thread (atomics only).
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include "MixerSettings.h"
#include "Dsp/Loudness.h"
#include "Net/SpscQueue.h"
#include "ScopeRing.h"

namespace pb
{
class MixerCore
{
public:
    struct Peaks
    {
        std::array<float, kStrips> stripL {}, stripR {};
        std::array<float, kBuses> busL {}, busR {};
        float masterL = 0.0f, masterR = 0.0f;
        bool clipped = false;
    };

    struct Input { const float* data = nullptr; int channels = 0; };   // interleaved

    /** Where process() writes. busL/busR (optional) are kBuses buffers each; only active buses are written. */
    struct Output
    {
        float* masterL = nullptr;
        float* masterR = nullptr;
        float* const* busL = nullptr;
        float* const* busR = nullptr;
    };

    /** What process() decided for this block; the engine needs exactly this, not a fresh read of the atomics. */
    struct Routing
    {
        OutType masterType = OutType::audio;
        int masterFirst = 1;                                   // Audio Out: 1-based device channel of master L
        std::array<bool, kBuses> busActive {};
        std::array<OutType, kBuses> busType {};
        std::array<int, kBuses> busFirst {};
    };

    MixerCore() { setSettings (MixerSettings()); }

    // ---- control side (any thread) -----------------------------------------------------------------------------
    void setSettings (const MixerSettings& in)
    {
        MixerSettings s = in;
        s.sanitize();
        for (int i = 0; i < kStrips; ++i)
        {
            const auto& st = s.strips[(size_t) i];
            setStripSource (i, st.src); setStripGainDb (i, st.gainDb); setStripBalance (i, st.balance);
            setStripMute (i, st.mute); setStripSolo (i, st.solo); setStripOut (i, st.out);
        }
        for (int b = 0; b < kBuses; ++b)
        {
            const auto& bs = s.buses[(size_t) b];
            setBusGainDb (b, bs.gainDb); setBusMute (b, bs.mute); setBusType (b, bs.type); setBusOutFirst (b, bs.outFirst);
        }
        setMasterDb (s.masterDb);
        setMasterMute (s.masterMute);
        setMasterType (s.masterType);
        setMasterOut (s.masterOut);
    }

    void setStripSource (int i, Source src) noexcept
    {
        if (! validStrip (i)) return;
        src.sanitize();
        auto& s = strip[(size_t) i];
        s.srcType.store ((int) src.type, std::memory_order_relaxed);
        s.srcIndex.store (src.index, std::memory_order_relaxed);
    }
    void setStripGainDb (int i, float db) noexcept { if (validStrip (i)) strip[(size_t) i].gainDb.store (clampGainDb (db), std::memory_order_relaxed); }
    void setStripBalance (int i, float b) noexcept { if (validStrip (i)) strip[(size_t) i].balance.store (clampBalance (b), std::memory_order_relaxed); }
    void setStripMute   (int i, bool m)   noexcept { if (validStrip (i)) strip[(size_t) i].mute.store (m, std::memory_order_relaxed); }
    void setStripSolo   (int i, bool s)   noexcept { if (validStrip (i)) strip[(size_t) i].solo.store (s, std::memory_order_relaxed); }
    void setStripOut    (int i, int out)  noexcept { if (validStrip (i)) strip[(size_t) i].out.store (clampStripOut (out), std::memory_order_relaxed); }

    void setBusGainDb  (int b, float db) noexcept { if (validBus (b)) bus[(size_t) b].gainDb.store (clampGainDb (db), std::memory_order_relaxed); }
    void setBusMute    (int b, bool m)   noexcept { if (validBus (b)) bus[(size_t) b].mute.store (m, std::memory_order_relaxed); }
    void setBusType    (int b, OutType t) noexcept { if (validBus (b)) bus[(size_t) b].type.store (t == OutType::net ? 1 : 0, std::memory_order_relaxed); }
    void setBusOutFirst (int b, int first) noexcept { if (validBus (b)) bus[(size_t) b].outFirst.store (clampOutFirst (first), std::memory_order_relaxed); }

    void setMasterDb (float db) noexcept { masterDb.store (clampGainDb (db), std::memory_order_relaxed); }
    void setMasterMute (bool m) noexcept { masterMute.store (m, std::memory_order_relaxed); }
    void setMasterType (OutType t) noexcept { masterType.store (t == OutType::net ? 1 : 0, std::memory_order_relaxed); }
    void setMasterOut (int firstCh) noexcept { masterOut.store (clampOutFirst (firstCh), std::memory_order_relaxed); }

    // ---- analyzer taps (UI side) ------------------------------------------------------------------------------
    /** Which signal feeds the analyzer's scope ring: a strip (0..31) or a bus (32..63), or -1 for none. `post` = after the
        fader (strip: after fader, balance and mute; bus: after the bus fader), otherwise the signal going in. */
    void setScope (int target, bool post) noexcept
    {
        scopePost.store (post, std::memory_order_relaxed);
        scopeTarget.store (target >= 0 && target < kStrips + kBuses ? target : -1, std::memory_order_relaxed);
    }
    const ScopeRing& scopeRing() const noexcept { return scope; }
    const ScopeRing& masterRing() const noexcept { return masterScope; }

    /** Master loudness hops (100 ms each), oldest first. Returns false when none are waiting. UI thread only. */
    bool popLoudnessHop (dsp::LoudnessHop& h) noexcept
    {
        const auto* p = hopQueue.peek();
        if (p == nullptr) return false;
        h = *p; hopQueue.release();
        return true;
    }
    int loudnessHopsDropped() const noexcept { return hopDrops.load (std::memory_order_relaxed); }

    /** Call while the audio thread is not running (device start). Sets the rate for the loudness filters. */
    void prepareAnalysis (double sampleRate) noexcept
    {
        blockMaker.prepare (sampleRate);
        dsp::LoudnessHop discard;
        while (popLoudnessHop (discard)) {}
    }

    /** Peaks since the previous call (the call resets them). Call from the UI thread. */
    Peaks takePeaks() noexcept
    {
        Peaks p;
        for (int i = 0; i < kStrips; ++i)
        {
            p.stripL[(size_t) i] = stripPeakL[(size_t) i].exchange (0.0f, std::memory_order_relaxed);
            p.stripR[(size_t) i] = stripPeakR[(size_t) i].exchange (0.0f, std::memory_order_relaxed);
        }
        for (int b = 0; b < kBuses; ++b)
        {
            p.busL[(size_t) b] = busPeakL[(size_t) b].exchange (0.0f, std::memory_order_relaxed);
            p.busR[(size_t) b] = busPeakR[(size_t) b].exchange (0.0f, std::memory_order_relaxed);
        }
        p.masterL = peakL.exchange (0.0f, std::memory_order_relaxed);
        p.masterR = peakR.exchange (0.0f, std::memory_order_relaxed);
        p.clipped = clipped.exchange (false, std::memory_order_relaxed);
        return p;
    }

    // ---- audio side (one thread) -------------------------------------------------------------------------------
    /** Next block jumps to the current targets instead of ramping (call when the device restarts). */
    void reset() noexcept { snapped = false; }

    /** masterL/masterR are overwritten with `frames` samples. Active buses are too (bus buffers of inactive buses are
        left untouched). */
    void process (Input net, Input app, Input dev, int frames, const Output& out, Routing* routing) noexcept
    {
        if (frames <= 0 || out.masterL == nullptr || out.masterR == nullptr) return;
        std::fill (out.masterL, out.masterL + frames, 0.0f);
        std::fill (out.masterR, out.masterR + frames, 0.0f);

        const bool haveBuses = out.busL != nullptr && out.busR != nullptr;
        auto busBuffers = [&] (int b) { return haveBuses && out.busL[b] != nullptr && out.busR[b] != nullptr; };

        // ---- targets for this block
        bool anySolo = false;
        for (auto& s : strip) anySolo = anySolo || s.solo.load (std::memory_order_relaxed);

        std::array<float, kStrips> faderLin {}, tgtL {}, tgtR {};
        std::array<int, kStrips> dest {};                       // 0 = master, n = bus n
        std::array<SourceType, kStrips> stype {};
        std::array<int, kStrips> sindex {};
        std::array<bool, kBuses> active {};
        for (int i = 0; i < kStrips; ++i)
        {
            const auto k = (size_t) i;
            auto& s = strip[k];
            const float g = gainDbToLinear (s.gainDb.load (std::memory_order_relaxed));
            const bool audible = ! s.mute.load (std::memory_order_relaxed)
                                 && (! anySolo || s.solo.load (std::memory_order_relaxed));
            const float bal = s.balance.load (std::memory_order_relaxed);

            int d = s.out.load (std::memory_order_relaxed);
            if (d > 0 && ! busBuffers (d - 1)) d = 0;           // no buffers for that bus: play on the master
            dest[k] = d;
            if (d > 0) active[(size_t) (d - 1)] = true;

            Source src { (SourceType) s.srcType.load (std::memory_order_relaxed), s.srcIndex.load (std::memory_order_relaxed) };
            src.sanitize();
            stype[k] = src.type; sindex[k] = src.index;

            faderLin[k] = g;
            tgtL[k] = audible ? g * (bal > 0.0f ? std::cos (bal * kHalfPi) : 1.0f) : 0.0f;
            tgtR[k] = audible ? g * (bal < 0.0f ? std::cos (-bal * kHalfPi) : 1.0f) : 0.0f;
        }

        std::array<float, kBuses> tgtBus {};
        for (int b = 0; b < kBuses; ++b)
        {
            auto& bs = bus[(size_t) b];
            tgtBus[(size_t) b] = bs.mute.load (std::memory_order_relaxed) ? 0.0f : gainDbToLinear (bs.gainDb.load (std::memory_order_relaxed));
        }
        const float tgtMaster = masterMute.load (std::memory_order_relaxed)
                                    ? 0.0f : gainDbToLinear (masterDb.load (std::memory_order_relaxed));

        if (! snapped)
        {
            curL = tgtL; curR = tgtR; curBus = tgtBus; curMaster = tgtMaster; prevDest = dest; snapped = true;
        }

        // A bus a strip just left stays active for this block, so the strip can fade out of it.
        std::array<bool, kStrips> fadeOut {};
        for (int i = 0; i < kStrips; ++i)
        {
            const auto k = (size_t) i;
            const int pd = prevDest[k];
            if (pd != dest[k] && pd > 0 && busBuffers (pd - 1)) { fadeOut[k] = true; active[(size_t) (pd - 1)] = true; }
        }

        if (routing != nullptr)
        {
            routing->masterType = masterType.load (std::memory_order_relaxed) == 1 ? OutType::net : OutType::audio;
            routing->masterFirst = masterOut.load (std::memory_order_relaxed);
            for (int b = 0; b < kBuses; ++b)
            {
                const auto k = (size_t) b;
                routing->busActive[k] = active[k];
                routing->busType[k] = bus[k].type.load (std::memory_order_relaxed) == 1 ? OutType::net : OutType::audio;
                routing->busFirst[k] = bus[k].outFirst.load (std::memory_order_relaxed);
            }
        }

        for (int b = 0; b < kBuses; ++b)                         // an inactive bus has no ramp to continue
            if (! active[(size_t) b]) curBus[(size_t) b] = tgtBus[(size_t) b];

        for (int b = 0; b < kBuses; ++b)
            if (active[(size_t) b]) { std::fill (out.busL[b], out.busL[b] + frames, 0.0f); std::fill (out.busR[b], out.busR[b] + frames, 0.0f); }

        // ---- strips
        const float invFrames = 1.0f / (float) frames;
        const int tapTarget = scopeTarget.load (std::memory_order_relaxed);
        const bool tapPost = scopePost.load (std::memory_order_relaxed);
        for (int i = 0; i < kStrips; ++i)
        {
            const auto k = (size_t) i;
            const Input& bank = stype[k] == SourceType::net ? net : (stype[k] == SourceType::app ? app : dev);
            int cl = -1, cr = -1;
            if (stype[k] != SourceType::off && bank.data != nullptr)
            {
                const int l = 2 * sindex[k];
                if (l < bank.channels) { cl = l; cr = l + 1 < bank.channels ? l + 1 : l; }
            }

            if (cl >= 0)
            {
                auto dst = [&] (int d, float*& dl, float*& dr)
                {
                    dl = d == 0 ? out.masterL : out.busL[d - 1];
                    dr = d == 0 ? out.masterR : out.busR[d - 1];
                };
                float* dl; float* dr;
                const bool moved = prevDest[k] != dest[k];
                const int pd = prevDest[k];
                const bool prevOk = moved && (pd == 0 || fadeOut[k]);

                float peakL = 0.0f, peakR = 0.0f;
                // Fade in (or ramp) on the current destination.
                dst (dest[k], dl, dr);
                const float startL = moved ? 0.0f : curL[k], startR = moved ? 0.0f : curR[k];
                const float dL = (tgtL[k] - startL) * invFrames, dR = (tgtR[k] - startR) * invFrames;
                float gl = startL, gr = startR;
                for (int n = 0; n < frames; ++n)
                {
                    const float xl = clean (bank.data[(size_t) n * (size_t) bank.channels + (size_t) cl]);
                    const float xr = clean (bank.data[(size_t) n * (size_t) bank.channels + (size_t) cr]);
                    peakL = std::max (peakL, std::abs (xl)); peakR = std::max (peakR, std::abs (xr));
                    gl += dL; gr += dR;
                    dl[n] += xl * gl;
                    dr[n] += xr * gr;
                    if (tapTarget == i) scope.pushOne (tapPost ? xl * gl : xl, tapPost ? xr * gr : xr);
                }
                raise (stripPeakL[k], peakL * faderLin[k]);
                raise (stripPeakR[k], peakR * faderLin[k]);

                // Fade out of the previous destination, so a route change does not click.
                if (prevOk)
                {
                    float *pl, *pr;
                    dst (pd, pl, pr);
                    const float eL = -curL[k] * invFrames, eR = -curR[k] * invFrames;
                    float hl = curL[k], hr = curR[k];
                    for (int n = 0; n < frames; ++n)
                    {
                        const float xl = clean (bank.data[(size_t) n * (size_t) bank.channels + (size_t) cl]);
                        const float xr = clean (bank.data[(size_t) n * (size_t) bank.channels + (size_t) cr]);
                        hl += eL; hr += eR;
                        pl[n] += xl * hl;
                        pr[n] += xr * hr;
                    }
                }
            }
            else if (tapTarget == i) pushSilence (frames);                    // a strip with no signal shows an empty spectrum
            curL[k] = tgtL[k]; curR[k] = tgtR[k]; prevDest[k] = dest[k];      // land exactly on the target
        }

        bool clip = false;

        // ---- buses
        for (int b = 0; b < kBuses; ++b)
        {
            const auto k = (size_t) b;
            if (! active[k]) { if (tapTarget == kStrips + b) pushSilence (frames); continue; }
            const float dB = (tgtBus[k] - curBus[k]) * invFrames;
            float g = curBus[k];
            float pl = 0.0f, pr = 0.0f;
            float* l = out.busL[b];
            float* r = out.busR[b];
            for (int n = 0; n < frames; ++n)
            {
                g += dB;
                if (tapTarget == kStrips + b && ! tapPost) scope.pushOne (l[n], r[n]);
                float xl = l[n] * g, xr = r[n] * g;
                if (xl > 1.0f || xl < -1.0f) { clip = true; xl = xl > 0.0f ? 1.0f : -1.0f; }
                if (xr > 1.0f || xr < -1.0f) { clip = true; xr = xr > 0.0f ? 1.0f : -1.0f; }
                l[n] = xl; r[n] = xr;
                if (tapTarget == kStrips + b && tapPost) scope.pushOne (xl, xr);
                pl = std::max (pl, std::abs (xl)); pr = std::max (pr, std::abs (xr));
            }
            curBus[k] = tgtBus[k];
            raise (busPeakL[k], pl);
            raise (busPeakR[k], pr);
        }

        // ---- master
        const float dM = (tgtMaster - curMaster) * invFrames;
        float gm = curMaster;
        float pl = 0.0f, pr = 0.0f;
        for (int n = 0; n < frames; ++n)
        {
            gm += dM;
            float l = out.masterL[n] * gm, r = out.masterR[n] * gm;
            if (l > 1.0f || l < -1.0f) { clip = true; l = l > 0.0f ? 1.0f : -1.0f; }
            if (r > 1.0f || r < -1.0f) { clip = true; r = r > 0.0f ? 1.0f : -1.0f; }
            out.masterL[n] = l; out.masterR[n] = r;
            pl = std::max (pl, std::abs (l)); pr = std::max (pr, std::abs (r));
        }
        curMaster = tgtMaster;
        masterScope.push (out.masterL, out.masterR, frames);
        blockMaker.process (out.masterL, out.masterR, frames, [this] (const dsp::LoudnessHop& h)
        {
            if (auto* slot = hopQueue.acquireWrite()) { *slot = h; hopQueue.publish(); }
            else hopDrops.fetch_add (1, std::memory_order_relaxed);
        });
        raise (peakL, pl);
        raise (peakR, pr);
        if (clip) clipped.store (true, std::memory_order_relaxed);
    }

    /** Lets the engine report a clamp that happened in the device router (outputs summing past full scale). */
    void noteClip() noexcept { clipped.store (true, std::memory_order_relaxed); }

private:
    static constexpr float kHalfPi = 1.57079632679489661923f;

    void pushSilence (int frames) noexcept
    {
        static constexpr float zeros[256] = {};
        for (int done = 0; done < frames; done += 256) { const int n = std::min (256, frames - done); scope.push (zeros, zeros, n); }
    }

    static bool validStrip (int i) noexcept { return i >= 0 && i < kStrips; }
    static bool validBus (int b) noexcept { return b >= 0 && b < kBuses; }
    static float clean (float x) noexcept { return std::isfinite (x) ? x : 0.0f; }
    static void raise (std::atomic<float>& a, float v) noexcept
    {
        float cur = a.load (std::memory_order_relaxed);
        while (v > cur && ! a.compare_exchange_weak (cur, v, std::memory_order_relaxed)) {}
    }

    struct Strip
    {
        std::atomic<int> srcType { 0 }, srcIndex { 0 }, out { 0 };
        std::atomic<float> gainDb { 0.0f }, balance { 0.0f };
        std::atomic<bool> mute { false }, solo { false };
    };
    struct Bus
    {
        std::atomic<float> gainDb { 0.0f };
        std::atomic<bool> mute { false };
        std::atomic<int> type { 0 }, outFirst { 1 };
    };
    static_assert (std::atomic<float>::is_always_lock_free && std::atomic<bool>::is_always_lock_free
                   && std::atomic<int>::is_always_lock_free, "audio-thread atomics must be lock-free");

    std::array<Strip, kStrips> strip;
    std::array<Bus, kBuses> bus;
    std::atomic<float> masterDb { 0.0f };
    std::atomic<bool> masterMute { false };
    std::atomic<int> masterType { 0 }, masterOut { 1 };

    std::array<std::atomic<float>, kStrips> stripPeakL {}, stripPeakR {};
    std::array<std::atomic<float>, kBuses> busPeakL {}, busPeakR {};
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };
    std::atomic<bool> clipped { false };

    // Analyzer taps.
    ScopeRing scope, masterScope;
    std::atomic<int> scopeTarget { -1 };
    std::atomic<bool> scopePost { false };
    net::SpscQueue<dsp::LoudnessHop, 256> hopQueue;
    std::atomic<int> hopDrops { 0 };
    dsp::LoudnessBlockMaker blockMaker;

    // Audio-thread state.
    std::array<float, kStrips> curL {}, curR {};
    std::array<float, kBuses> curBus {};
    std::array<int, kStrips> prevDest {};
    float curMaster = 1.0f;
    bool snapped = false;
};
}
