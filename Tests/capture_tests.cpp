// CaptureStream: rate conversion, drift tracking, underrun/overflow handling, thread safety. Pure C++.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>
#include "Capture/CaptureStream.h"

using namespace pb;
static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (! (c)) { ++failures; std::printf ("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define NEAR(a, b, tol) do { ++checks; const double va = (a), vb = (b); if (! (std::abs (va - vb) <= (tol))) { ++failures; std::printf ("  FAIL line %d: %s = %g, expected %g\n", __LINE__, #a, va, vb); } } while (0)
#define TEST(name) static void name(); struct Reg_##name { Reg_##name() { tests().push_back ({ #name, &name }); } } reg_##name; static void name()
struct T { const char* n; void (*f)(); };
static std::vector<T>& tests() { static std::vector<T> t; return t; }

constexpr double kPi = 3.14159265358979323846;

/** Deterministic two-clock simulation: the producer delivers `pBlock` frames at srcRate*(1+ppm), the consumer pulls
    `cBlock` frames at dstRate. gen(n) is the n-th source frame (both channels). Returns the consumer's output. */
template <typename Gen>
static std::vector<float> simulate (CaptureStream& cs, double srcRate, double dstRate, double ppm, double seconds,
                                    int pBlock, int cBlock, Gen gen, double producerStart = 0.0)
{
    cs.setSourceRate (srcRate);
    cs.prepare (dstRate);
    const double srcActual = srcRate * (1.0 + ppm * 1e-6);
    double nextP = producerStart, nextC = 0.0;
    uint64_t produced = 0;
    std::vector<float> in ((size_t) pBlock * 2), out, blk ((size_t) cBlock * 2);
    while (nextC < seconds)
    {
        if (nextP <= nextC)
        {
            for (int i = 0; i < pBlock; ++i) { const float v = gen ((double) (produced + (uint64_t) i)); in[(size_t) i * 2] = v; in[(size_t) i * 2 + 1] = -v; }
            cs.write (in.data(), pBlock, 2);
            produced += (uint64_t) pBlock;
            nextP += pBlock / srcActual;
        }
        else
        {
            cs.process (blk.data(), cBlock);
            out.insert (out.end(), blk.begin(), blk.end());
            nextC += cBlock / dstRate;
        }
    }
    return out;
}

static int zeroCrossings (const std::vector<float>& st, size_t from, size_t to)    // left channel, rising
{
    int n = 0;
    for (size_t i = from + 1; i < to; ++i) if (st[(i - 1) * 2] < 0.0f && st[i * 2] >= 0.0f) ++n;
    return n;
}

TEST (converts_44k1_to_48k_without_pitch_error)
{
    CaptureStream cs;
    const double f = 1000.0;
    auto out = simulate (cs, 44100.0, 48000.0, 0.0, 6.0, 512, 256, [&] (double n) { return 0.5f * (float) std::sin (2.0 * kPi * f * n / 44100.0); });
    const size_t from = 48000 * 2, to = 48000 * 6;                          // skip priming + settling
    NEAR ((double) zeroCrossings (out, from, to) / 4.0, f, 2.0);            // 1 kHz out of a 44.1 kHz source
    double rms = 0; for (size_t i = from; i < to; ++i) rms += out[i * 2] * out[i * 2];
    NEAR (std::sqrt (rms / (double) (to - from)), 0.5 / std::sqrt (2.0), 0.01);
    CHECK (cs.status().underruns == 0 && cs.status().overflows == 0);
}

TEST (converts_96k_to_44k1)
{
    CaptureStream cs;
    auto out = simulate (cs, 96000.0, 44100.0, 0.0, 5.0, 1024, 512, [&] (double n) { return 0.4f * (float) std::sin (2.0 * kPi * 500.0 * n / 96000.0); });
    NEAR ((double) zeroCrossings (out, 44100 * 2, 44100 * 5) / 3.0, 500.0, 2.0);
    CHECK (cs.status().underruns == 0);
}

TEST (same_rate_is_a_clean_delay)
{
    // A linear ramp: Catmull-Rom reproduces it exactly, so output[k] = ramp(k - delay) with delay read straight off the value.
    CaptureStream cs;
    auto gen = [] (double n) { return (float) (n * 1e-5); };
    const auto out = simulate (cs, 48000.0, 48000.0, 0.0, 1.0, 256, 256, gen);
    auto delayAt = [&] (size_t k) { return (double) k - (double) out[k * 2] / 1e-5; };
    const double d0 = delayAt (6000), d1 = delayAt (40000);
    CHECK (d0 > 2000 && d0 < 3500);                                           // about the 50 ms target
    NEAR (d1, d0, 12.0);                                                      // latency holds steady (the controller only trims it slowly)
    for (size_t k = 6000; k < 40000; k += 997) NEAR (out[k * 2 + 1], -out[k * 2], 1e-6);   // right channel intact
    for (size_t k = 1; k < 40000; ++k) CHECK (out[k * 2] + 1e-6f >= out[(k - 1) * 2]);        // monotonic: no glitches, no reversal
}

TEST (tracks_clock_drift_in_both_directions)
{
    for (double ppm : { +300.0, -300.0, +450.0 })
    {
        CaptureStream cs;
        simulate (cs, 48000.0, 48000.0, ppm, 360.0, 512, 256, [] (double) { return 0.1f; });
        const auto st = cs.status();
        CHECK (st.underruns == 0);
        CHECK (st.overflows == 0 && st.trims == 0);
        NEAR (st.fillFrames, st.targetFrames, st.targetFrames * 0.35);        // fill stays near target (packet-sized wobble allowed)
        NEAR (st.ratioPpm, ppm, 150.0);                                       // the correction converged on the real drift
    }
}

TEST (waits_for_target_fill_then_plays_and_reprimes_after_underrun)
{
    CaptureStream cs;
    cs.setSourceRate (48000.0); cs.prepare (48000.0);
    std::vector<float> in (256 * 2, 0.5f), out (256 * 2, 9.0f);
    cs.process (out.data(), 256);
    for (float v : out) CHECK (v == 0.0f);                                    // nothing written yet: silence
    cs.write (in.data(), 256, 2);                                             // far below the 2400-frame target
    cs.process (out.data(), 256);
    for (float v : out) CHECK (v == 0.0f);
    CHECK (! cs.status().primed);

    for (int i = 0; i < 12; ++i) cs.write (in.data(), 256, 2);                // reach the target
    cs.process (out.data(), 256);
    CHECK (cs.status().primed);
    NEAR (out[200], 0.5, 0.05);

    for (int i = 0; i < 100; ++i) cs.process (out.data(), 256);               // starve it
    CHECK (cs.status().underruns >= 1 && ! cs.status().primed);
    cs.process (out.data(), 256);
    for (float v : out) CHECK (v == 0.0f);                                    // silent while re-buffering, not stuck noise
    for (int i = 0; i < 14; ++i) cs.write (in.data(), 256, 2);
    cs.process (out.data(), 256);
    CHECK (cs.status().primed);
}

TEST (overflow_and_backlog_trim)
{
    CaptureStream cs (1024);
    cs.setSourceRate (48000.0); cs.prepare (48000.0);
    std::vector<float> in (4096 * 2, 0.25f);
    cs.write (in.data(), 4096, 2);                                            // capacity 1024: the excess is dropped, not corrupting
    CHECK (cs.status().overflows == 1);

    CaptureStream big (1 << 15);
    big.setSourceRate (48000.0); big.prepare (48000.0);
    std::vector<float> chunk (8192 * 2, 0.25f), out (256 * 2);
    for (int i = 0; i < 3; ++i) big.write (chunk.data(), 8192, 2);            // 24576 frames queued vs a 2400 target
    big.process (out.data(), 256);
    CHECK (big.status().trims == 1);
    NEAR (big.status().fillFrames, 2400.0, 400.0);                            // dropped down to about the target
}

TEST (channel_conversion_and_hostile_input)
{
    CaptureStream cs;
    cs.setSourceRate (48000.0); cs.prepare (48000.0);
    std::vector<float> mono (4800, 0.5f);
    cs.write (mono.data(), 1200, 1);                                          // mono is duplicated to both sides
    std::vector<float> six ((size_t) 1200 * 6, 0.25f);
    for (int n = 0; n < 1200; ++n) six[(size_t) n * 6 + 2] = 99.0f;           // channels 3+ are ignored
    cs.write (six.data(), 1200, 6);
    std::vector<float> bad (1200 * 2, std::numeric_limits<float>::quiet_NaN());
    bad[10] = std::numeric_limits<float>::infinity();
    cs.write (bad.data(), 1200, 2);
    cs.write (nullptr, 100, 2); cs.write (mono.data(), 0, 2); cs.write (mono.data(), 100, 0);   // ignored, no crash

    std::vector<float> out (256 * 2);
    bool finite = true, sawMono = false, saw6 = false;
    for (int i = 0; i < 30; ++i)
    {
        cs.process (out.data(), 256);
        for (size_t k = 0; k < out.size(); ++k) { finite = finite && std::isfinite (out[k]) && std::abs (out[k]) < 1.0f; }
        if (std::abs (out[100] - 0.5f) < 0.01f && std::abs (out[101] - 0.5f) < 0.01f) sawMono = true;
        if (std::abs (out[100] - 0.25f) < 0.01f && std::abs (out[101] - 0.25f) < 0.01f) saw6 = true;
    }
    CHECK (finite);
    CHECK (sawMono && saw6);
}

TEST (invalid_rates_are_ignored)
{
    CaptureStream cs;
    cs.setSourceRate (44100.0);
    cs.setSourceRate (std::numeric_limits<double>::quiet_NaN()); cs.setSourceRate (0.0); cs.setSourceRate (-5.0); cs.setSourceRate (1e12); cs.setSourceRate (10.0);
    NEAR (cs.getSourceRate(), 44100.0, 1e-9);
    cs.prepare (std::numeric_limits<double>::infinity());                     // falls back to 48 kHz instead of dividing by garbage
    std::vector<float> out (64 * 2, 1.0f);
    cs.process (out.data(), 64);
    for (float v : out) CHECK (std::isfinite (v));
    CaptureStream none;                                                       // source rate never set: silence, not NaN
    none.prepare (48000.0);
    none.process (out.data(), 64);
    for (float v : out) CHECK (v == 0.0f);
}

TEST (source_rate_change_midstream_recovers)
{
    CaptureStream cs;
    cs.setSourceRate (48000.0); cs.prepare (48000.0);
    std::vector<float> in (512 * 2, 0.3f), out (256 * 2);
    for (int i = 0; i < 30; ++i) cs.write (in.data(), 512, 2);
    for (int i = 0; i < 20; ++i) cs.process (out.data(), 256);
    cs.setSourceRate (44100.0);                                               // tap restarted on a different device
    bool finite = true;
    for (int i = 0; i < 200; ++i)
    {
        cs.write (in.data(), 512, 2);
        cs.process (out.data(), 256); cs.process (out.data(), 256);
        for (float v : out) finite = finite && std::isfinite (v);
    }
    CHECK (finite);
}

TEST (threaded_producer_and_consumer)
{
    CaptureStream cs;
    cs.setSourceRate (48000.0); cs.prepare (48000.0);
    std::atomic<bool> stop { false };
    std::thread prod ([&]
    {
        std::vector<float> in (480 * 2, 0.2f);
        while (! stop.load()) { cs.write (in.data(), 480, 2); std::this_thread::sleep_for (std::chrono::microseconds (500)); }
    });
    std::vector<float> out (128 * 2);
    bool ok = true;
    for (int i = 0; i < 30000; ++i)
    {
        cs.process (out.data(), 128);
        for (float v : out) ok = ok && std::isfinite (v) && std::abs (v) < 1.0f;
        (void) cs.status();
    }
    stop = true; prod.join();
    CHECK (ok);
}

int main()
{
    for (auto& t : tests())
    {
        const int before = failures;
        t.f();
        std::printf ("[%s] %s\n", failures == before ? "  OK  " : "FAILED", t.n);
    }
    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
