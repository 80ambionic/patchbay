#pragma once
// Spectrum analysis for the mixer's analyzer view. Pure C++ (no JUCE), UI-thread only.
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace pb::dsp
{
/** In-place radix-2 complex FFT of a fixed power-of-two size. */
class Fft
{
public:
    explicit Fft (int sizeLog2) : n (1 << sizeLog2), rev ((size_t) n), tw ((size_t) n / 2)
    {
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < sizeLog2; ++b) if (i & (1 << b)) r |= 1 << (sizeLog2 - 1 - b);
            rev[(size_t) i] = r;
        }
        const double pi = 3.14159265358979323846;
        for (int i = 0; i < n / 2; ++i) tw[(size_t) i] = std::polar (1.0, -2.0 * pi * i / n);
    }

    int size() const noexcept { return n; }

    void transform (std::vector<std::complex<double>>& x) const
    {
        for (int i = 0; i < n; ++i) if (i < rev[(size_t) i]) std::swap (x[(size_t) i], x[(size_t) rev[(size_t) i]]);
        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len / 2, step = n / len;
            for (int i = 0; i < n; i += len)
                for (int k = 0; k < half; ++k)
                {
                    const auto t = x[(size_t) (i + k + half)] * tw[(size_t) (k * step)];
                    const auto u = x[(size_t) (i + k)];
                    x[(size_t) (i + k)] = u + t;
                    x[(size_t) (i + k + half)] = u - t;
                }
        }
    }

private:
    int n;
    std::vector<int> rev;
    std::vector<std::complex<double>> tw;
};

/** Hann-windowed spectrum, scaled so a full-scale sine reads 0 dBFS, then folded into log-spaced bands for drawing. */
class SpectrumAnalyzer
{
public:
    static constexpr int kSizeLog2 = 12;                  // 4096-point FFT
    static constexpr int kSize = 1 << kSizeLog2;
    static constexpr float kFloorDb = -120.0f;

    explicit SpectrumAnalyzer (int numBands = 160, double lowHz = 20.0, double highHz = 20000.0)
        : fft (kSizeLog2), win ((size_t) kSize), buf ((size_t) kSize), binDb ((size_t) kSize / 2 + 1, kFloorDb),
          nBands (numBands), lo (lowHz), hi (highHz), bands ((size_t) numBands, kFloorDb)
    {
        const double pi = 3.14159265358979323846;
        double sum = 0.0;
        for (int i = 0; i < kSize; ++i) { win[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * pi * i / kSize); sum += win[(size_t) i]; }
        norm = 2.0 / sum;                                  // a full-scale sine has peak bin magnitude sum/2
    }

    void setSampleRate (double sr) { sampleRate = sr > 1000.0 ? sr : 48000.0; }
    double getSampleRate() const noexcept { return sampleRate; }

    /** `mono` holds kSize samples. Updates the bin and band magnitudes (dBFS). */
    void analyze (const float* mono)
    {
        for (int i = 0; i < kSize; ++i)
        {
            const float v = std::isfinite (mono[i]) ? mono[i] : 0.0f;
            buf[(size_t) i] = { (double) v * win[(size_t) i], 0.0 };
        }
        fft.transform (buf);
        for (int k = 0; k <= kSize / 2; ++k)
        {
            const double mag = std::abs (buf[(size_t) k]) * norm;
            binDb[(size_t) k] = mag > 1.0e-6 ? (float) (20.0 * std::log10 (mag)) : kFloorDb;
        }

        const double nyq = sampleRate * 0.5, top = std::min (hi, nyq * 0.999);
        const double ratio = top / lo;
        for (int b = 0; b < nBands; ++b)
        {
            const double f0 = lo * std::pow (ratio, (double) b / nBands), f1 = lo * std::pow (ratio, (double) (b + 1) / nBands);
            const double fc = std::sqrt (f0 * f1), binHz = sampleRate / kSize;
            // Narrow band (below one bin wide): interpolate the curve at its centre. Wide band: strongest bin inside.
            const double pos = fc / binHz;
            const int k0 = (int) pos;
            const float frac = (float) (pos - k0);
            float v = binDb[(size_t) k0] * (1.0f - frac) + binDb[(size_t) std::min (k0 + 1, kSize / 2)] * frac;
            const int a = (int) std::ceil (f0 / binHz), z = (int) std::floor (f1 / binHz);
            for (int k = std::max (a, 1); k <= std::min (z, kSize / 2); ++k) v = std::max (v, binDb[(size_t) k]);
            bands[(size_t) b] = v;
        }
    }

    int numBands() const noexcept { return nBands; }
    float bandDb (int b) const noexcept { return bands[(size_t) b]; }
    double bandCentreHz (int b) const noexcept
    {
        const double top = std::min (hi, sampleRate * 0.5 * 0.999), ratio = top / lo;
        return lo * std::pow (ratio, (b + 0.5) / nBands);
    }
    /** Fraction 0..1 along the log axis for a frequency (for gridlines). */
    double hzToPos (double hz) const noexcept
    {
        const double top = std::min (hi, sampleRate * 0.5 * 0.999);
        return std::clamp (std::log (hz / lo) / std::log (top / lo), 0.0, 1.0);
    }

    /** Strongest peak between lowHz and highHz after `analyze`: interpolated frequency (Hz) and level (dBFS). */
    bool peak (double& hz, float& db) const noexcept
    {
        const double binHz = sampleRate / kSize;
        const int a = std::max (2, (int) std::ceil (lo / binHz)), z = std::min (kSize / 2 - 2, (int) std::floor (std::min (hi, sampleRate * 0.5) / binHz));
        int best = -1; float bv = kFloorDb + 20.0f;
        for (int k = a; k <= z; ++k) if (binDb[(size_t) k] > bv) { bv = binDb[(size_t) k]; best = k; }
        if (best < 0) return false;
        // Parabolic interpolation on the dB values around the maximum.
        const double y0 = binDb[(size_t) best - 1], y1 = binDb[(size_t) best], y2 = binDb[(size_t) best + 1];
        const double den = y0 - 2.0 * y1 + y2;
        const double d = std::abs (den) < 1.0e-9 ? 0.0 : std::clamp (0.5 * (y0 - y2) / den, -0.5, 0.5);
        hz = (best + d) * binHz;
        db = (float) (y1 - 0.25 * (y0 - y2) * d);
        return true;
    }

private:
    Fft fft;
    std::vector<double> win;
    std::vector<std::complex<double>> buf;
    std::vector<float> binDb;
    int nBands;
    double lo, hi, norm = 1.0, sampleRate = 48000.0;
    std::vector<float> bands;
};
}
