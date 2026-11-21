#pragma once
// ITU-R BS.1770-4 / EBU R128 loudness. Split in two so the audio thread stays cheap and wait-free:
//   LoudnessBlockMaker  (audio thread)  K-weights the stereo master, tracks sample / true peak, and emits one small
//                                       summary per 100 ms hop. No allocation, no locks.
//   LoudnessMeter       (UI thread)     turns those hops into momentary (400 ms), short-term (3 s), integrated (gated),
//                                       loudness range, and peak readouts.
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace pb::dsp
{
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void clear() noexcept { z1 = z2 = 0.0; }
};

/** The two K-weighting stages for any sample rate (BS.1770-4: shelving pre-filter, then the RLB high-pass). */
inline void kWeightingCoefficients (double fs, Biquad& shelf, Biquad& highpass) noexcept
{
    const double pi = 3.14159265358979323846;
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan (pi * f0 / fs), Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
        shelf.b1 = 2.0 * (K * K - Vh) / a0;
        shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
        shelf.a1 = 2.0 * (K * K - 1.0) / a0;
        shelf.a2 = (1.0 - K / Q + K * K) / a0;
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan (pi * f0 / fs);
        const double a0 = 1.0 + K / Q + K * K;
        highpass.b0 = 1.0; highpass.b1 = -2.0; highpass.b2 = 1.0;
        highpass.a1 = 2.0 * (K * K - 1.0) / a0;
        highpass.a2 = (1.0 - K / Q + K * K) / a0;
    }
}

/** One 100 ms hop of the master. Mean squares are of the K-weighted signal. */
struct LoudnessHop
{
    float meanSqL = 0.0f, meanSqR = 0.0f;     // K-weighted mean square per channel over the hop
    float samplePeak = 0.0f;                  // linear
    float truePeak = 0.0f;                    // linear, 4x oversampled
};

/** 4x oversampling true-peak detector (BS.1770 Annex 2 style: 48-tap windowed sinc, 12 taps per phase). */
class TruePeakDetector
{
public:
    TruePeakDetector()
    {
        const double pi = 3.14159265358979323846;
        for (int p = 0; p < 4; ++p)
        {
            double sum = 0.0;
            for (int k = 0; k < kTaps; ++k)
            {
                const int n = 4 * k + p;
                const double t = (n - 23.5) / 4.0;
                const double sinc = std::abs (t) < 1e-12 ? 1.0 : std::sin (pi * t) / (pi * t);
                const double w = 0.5 - 0.5 * std::cos (2.0 * pi * (n + 0.5) / 48.0);
                h[(size_t) p][(size_t) k] = sinc * w;
                sum += sinc * w;
            }
            for (int k = 0; k < kTaps; ++k) h[(size_t) p][(size_t) k] /= sum;          // unity DC gain per phase
        }
        reset();
    }
    void reset() noexcept { hist.fill (0.0f); pos = 0; }

    /** Returns the largest |interpolated sample| around this input sample. */
    float process (float x) noexcept
    {
        hist[(size_t) pos] = x;
        float peak = std::abs (x);
        for (int p = 0; p < 4; ++p)
        {
            double acc = 0.0;
            for (int k = 0; k < kTaps; ++k) acc += h[(size_t) p][(size_t) k] * hist[(size_t) ((pos - k + kTaps) % kTaps)];
            peak = std::max (peak, (float) std::abs (acc));
        }
        pos = (pos + 1) % kTaps;
        return peak;
    }

private:
    static constexpr int kTaps = 12;
    std::array<std::array<double, kTaps>, 4> h {};
    std::array<float, kTaps> hist {};
    int pos = 0;
};

/** Audio thread side. Call `prepare` before the stream starts (not from the audio thread). */
class LoudnessBlockMaker
{
public:
    void prepare (double sampleRate) noexcept
    {
        fs = sampleRate > 1000.0 ? sampleRate : 48000.0;
        for (int c = 0; c < 2; ++c) { kWeightingCoefficients (fs, shelf[(size_t) c], hp[(size_t) c]); shelf[(size_t) c].clear(); hp[(size_t) c].clear(); tp[(size_t) c].reset(); }
        hops = 0; count = 0; sumL = sumR = 0.0; sp = tpk = 0.0f;
        nextEdge = (long long) std::llround (fs * 0.1);
    }

    /** Feed stereo samples; `emit(const LoudnessHop&)` is called for every completed 100 ms hop. */
    template <typename Emit>
    void process (const float* l, const float* r, int n, Emit&& emit) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const float xl = std::isfinite (l[i]) ? l[i] : 0.0f, xr = std::isfinite (r[i]) ? r[i] : 0.0f;
            const double yl = hp[0].process (shelf[0].process (xl)), yr = hp[1].process (shelf[1].process (xr));
            sumL += yl * yl; sumR += yr * yr;
            sp = std::max (sp, std::max (std::abs (xl), std::abs (xr)));
            tpk = std::max (tpk, std::max (tp[0].process (xl), tp[1].process (xr)));
            ++count; ++total;
            if (total >= nextEdge)
            {
                LoudnessHop h;
                h.meanSqL = (float) (sumL / (double) count); h.meanSqR = (float) (sumR / (double) count);
                h.samplePeak = sp; h.truePeak = tpk;
                emit (h);
                sumL = sumR = 0.0; sp = tpk = 0.0f; count = 0;
                ++hops;
                nextEdge = (long long) std::llround (fs * 0.1 * (double) (hops + 1));
            }
        }
    }

private:
    double fs = 48000.0, sumL = 0.0, sumR = 0.0;
    std::array<Biquad, 2> shelf, hp;
    std::array<TruePeakDetector, 2> tp;
    float sp = 0.0f, tpk = 0.0f;
    long long count = 0, total = 0, nextEdge = 4800, hops = 0;
};

/** UI thread side. Feed hops in order with `addHop`; read the results at any time. */
class LoudnessMeter
{
public:
    static constexpr double kSilence = -120.0;

    void reset()
    {
        window.clear(); gateBlocks.clear(); shortTerm.clear();
        momentary_ = shortTerm_ = integrated_ = lra_ = kSilence;
        truePeakDb_ = samplePeakDb_ = kSilence; maxMomentary_ = maxShort_ = kSilence;
        hopCount = 0;
    }
    LoudnessMeter() { reset(); }

    void addHop (const LoudnessHop& h)
    {
        window.push_back ((double) h.meanSqL + (double) h.meanSqR);       // channel weights are 1.0 for L and R
        if (window.size() > 30) window.erase (window.begin());

        samplePeakDb_ = std::max (samplePeakDb_, toDb (h.samplePeak));
        truePeakDb_ = std::max (truePeakDb_, toDb (h.truePeak));

        if (window.size() >= 4)
        {
            const double z400 = mean (4);
            momentary_ = lufs (z400);
            maxMomentary_ = std::max (maxMomentary_, momentary_);
            if (momentary_ > -70.0)                                        // absolute gate (400 ms blocks, 75 % overlap)
            {
                if (gateBlocks.size() < kMaxHistory) { gateBlocks.push_back (z400); }
                dirty = true;
            }
        }
        else momentary_ = kSilence;

        if (window.size() >= 30)
        {
            const double z3 = mean (30);
            shortTerm_ = lufs (z3);
            maxShort_ = std::max (maxShort_, shortTerm_);
            if (shortTerm_ > -70.0 && shortTerm.size() < kMaxHistory) { shortTerm.push_back (shortTerm_); dirty = true; }
        }
        ++hopCount;
        // Recomputing walks the history; once it is long, do it about once a second instead of every hop.
        if (dirty && (gateBlocks.size() < 600 || hopCount % 10 == 0)) recompute();
    }

    double momentary() const noexcept { return momentary_; }              // LUFS
    double shortTermLufs() const noexcept { return shortTerm_; }
    double integrated() const noexcept { return integrated_; }
    double range() const noexcept { return lra_; }                        // LU
    double truePeakDb() const noexcept { return truePeakDb_; }            // dBTP
    double samplePeakDb() const noexcept { return samplePeakDb_; }
    double maxMomentary() const noexcept { return maxMomentary_; }
    double maxShortTerm() const noexcept { return maxShort_; }

    static double lufs (double meanSquareSum) noexcept { return meanSquareSum > 1.0e-12 ? -0.691 + 10.0 * std::log10 (meanSquareSum) : kSilence; }

private:
    static constexpr size_t kMaxHistory = 24u * 3600u * 10u;             // 24 hours of 100 ms hops
    static double toDb (float lin) noexcept { return lin > 1.0e-6f ? 20.0 * std::log10 ((double) lin) : kSilence; }
    double mean (size_t n) const
    {
        double s = 0.0;
        for (size_t i = window.size() - n; i < window.size(); ++i) s += window[i];
        return s / (double) n;
    }

    void recompute()
    {
        dirty = false;
        // Integrated: gate at -70 LUFS (already applied), then at 10 LU below the mean of what remains.
        if (! gateBlocks.empty())
        {
            double s = 0.0;
            for (double z : gateBlocks) s += z;
            const double rel = lufs (s / (double) gateBlocks.size()) - 10.0;
            double s2 = 0.0; size_t n2 = 0;
            for (double z : gateBlocks) if (lufs (z) > rel) { s2 += z; ++n2; }
            integrated_ = n2 > 0 ? lufs (s2 / (double) n2) : kSilence;
        }
        // Range: short-term values above -70, then above (their power mean - 20 LU); 95th minus 10th percentile.
        if (shortTerm.size() >= 2)
        {
            double s = 0.0;
            for (double l : shortTerm) s += std::pow (10.0, (l + 0.691) / 10.0);
            const double rel = lufs (s / (double) shortTerm.size()) - 20.0;
            std::vector<double> v;
            for (double l : shortTerm) if (l > rel) v.push_back (l);
            if (v.size() >= 2)
            {
                std::sort (v.begin(), v.end());
                auto pct = [&] (double p) { return v[(size_t) std::llround (p * (double) (v.size() - 1))]; };
                lra_ = std::max (0.0, pct (0.95) - pct (0.10));
            }
            else lra_ = 0.0;
        }
    }

    std::vector<double> window;               // last 3 s of hop energies (L + R), newest last
    std::vector<double> gateBlocks;           // 400 ms block energies above the absolute gate
    std::vector<double> shortTerm;            // short-term loudness values above -70
    double momentary_, shortTerm_, integrated_, lra_, truePeakDb_, samplePeakDb_, maxMomentary_, maxShort_;
    size_t hopCount = 0;
    bool dirty = false;
};
}
