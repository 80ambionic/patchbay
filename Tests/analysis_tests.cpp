// Analyzer DSP (spectrum, BS.1770 loudness, true peak), the scope ring, and the mixer's analyzer taps. No JUCE.
#include <array>
#include <cmath>
#include <cstdio>
#include <thread>
#include <atomic>
#include <vector>
#include "Dsp/Loudness.h"
#include "Dsp/Spectrum.h"
#include "Mixer/MixerCore.h"
#include "Mixer/ScopeRing.h"

using namespace pb;
using namespace pb::dsp;
static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (! (c)) { ++failures; std::printf ("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define NEAR(a, b, tol) do { ++checks; const double va = (a), vb = (b); if (! (std::abs (va - vb) <= (tol))) { ++failures; std::printf ("  FAIL line %d: %s = %.4f, expected %.4f +- %.4f\n", __LINE__, #a, va, vb, (double) (tol)); } } while (0)
#define TEST(name) static void name(); struct Reg_##name { Reg_##name() { tests().push_back ({ #name, &name }); } } reg_##name; static void name()
struct T { const char* n; void (*f)(); };
static std::vector<T>& tests() { static std::vector<T> t; return t; }

static const double kPi = 3.14159265358979323846;
static std::vector<float> sine (double fs, double hz, double amp, int n, double phase = 0.0)
{
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i) v[(size_t) i] = (float) (amp * std::sin (2.0 * kPi * hz * i / fs + phase));
    return v;
}
static double dbfs (double lin) { return 20.0 * std::log10 (lin); }

/** Runs a stereo signal through the audio-side block maker and the UI-side meter, in 128-frame blocks. */
static void runLoudness (LoudnessMeter& m, double fs, const std::vector<float>& l, const std::vector<float>& r)
{
    LoudnessBlockMaker bm; bm.prepare (fs);
    for (size_t off = 0; off < l.size(); off += 128)
    {
        const int n = (int) std::min<size_t> (128, l.size() - off);
        bm.process (l.data() + off, r.data() + off, n, [&] (const LoudnessHop& h) { m.addHop (h); });
    }
}

// ---------------------------------------------------------------------------------------------------- spectrum
TEST (spectrum_full_scale_scaling_and_peak)
{
    SpectrumAnalyzer sa; sa.setSampleRate (48000.0);
    const double f = 100.0 * 48000.0 / SpectrumAnalyzer::kSize;                  // exactly on bin 100
    const auto x = sine (48000.0, f, 0.5, SpectrumAnalyzer::kSize);
    sa.analyze (x.data());
    double hz = 0; float db = 0;
    CHECK (sa.peak (hz, db));
    NEAR (hz, f, 0.3);
    NEAR (db, -6.02, 0.1);                                                       // 0.5 amplitude = -6.02 dBFS
    const auto full = sine (48000.0, f, 1.0, SpectrumAnalyzer::kSize);
    sa.analyze (full.data());
    sa.peak (hz, db);
    NEAR (db, 0.0, 0.1);
}

TEST (spectrum_peak_between_bins)
{
    SpectrumAnalyzer sa; sa.setSampleRate (48000.0);
    for (double f : { 440.0, 1000.0, 3333.3, 9000.0 })
    {
        const auto x = sine (48000.0, f, 0.7, SpectrumAnalyzer::kSize);
        sa.analyze (x.data());
        double hz = 0; float db = 0;
        CHECK (sa.peak (hz, db));
        NEAR (hz, f, f * 0.004 + 1.0);
        NEAR (db, dbfs (0.7), 0.3);
    }
}

TEST (spectrum_bands_follow_the_tone)
{
    SpectrumAnalyzer sa; sa.setSampleRate (48000.0);
    for (double f : { 100.0, 1000.0, 10000.0 })
    {
        const auto x = sine (48000.0, f, 0.5, SpectrumAnalyzer::kSize);
        sa.analyze (x.data());
        int best = 0;
        for (int b = 1; b < sa.numBands(); ++b) if (sa.bandDb (b) > sa.bandDb (best)) best = b;
        // Resolution is one FFT bin (11.7 Hz at 48 kHz): at 100 Hz that is already 12 % of the tone.
        CHECK (std::abs (sa.bandCentreHz (best) / f - 1.0) < std::max (0.05, 1.5 * (48000.0 / SpectrumAnalyzer::kSize) / f));
        CHECK (sa.bandDb (best) > -8.0f);
    }
}

TEST (spectrum_silence_and_garbage)
{
    SpectrumAnalyzer sa; sa.setSampleRate (44100.0);
    std::vector<float> z ((size_t) SpectrumAnalyzer::kSize, 0.0f);
    sa.analyze (z.data());
    for (int b = 0; b < sa.numBands(); ++b) CHECK (sa.bandDb (b) <= SpectrumAnalyzer::kFloorDb + 0.5f);
    double hz; float db;
    CHECK (! sa.peak (hz, db));
    std::vector<float> bad ((size_t) SpectrumAnalyzer::kSize, std::nanf (""));
    bad[10] = INFINITY;
    sa.analyze (bad.data());                                                     // must not produce NaN
    for (int b = 0; b < sa.numBands(); ++b) CHECK (std::isfinite (sa.bandDb (b)));
}

TEST (spectrum_bands_stay_inside_nyquist)
{
    SpectrumAnalyzer sa; sa.setSampleRate (22050.0);                            // Nyquist 11 kHz, below the 20 kHz default top
    std::vector<float> x ((size_t) SpectrumAnalyzer::kSize, 0.1f);
    sa.analyze (x.data());
    CHECK (sa.bandCentreHz (sa.numBands() - 1) < 11025.0);
}

// ---------------------------------------------------------------------------------------------------- loudness
TEST (k_weighting_matches_the_published_48k_coefficients)
{
    Biquad sh, hp;
    kWeightingCoefficients (48000.0, sh, hp);
    NEAR (sh.b0, 1.53512485958697, 1e-9); NEAR (sh.b1, -2.69169618940638, 1e-9); NEAR (sh.b2, 1.19839281085285, 1e-9);
    NEAR (sh.a1, -1.69065929318241, 1e-9); NEAR (sh.a2, 0.73248077421585, 1e-9);
    NEAR (hp.a1, -1.99004745483398, 1e-9); NEAR (hp.a2, 0.99007225036621, 1e-9);
}

TEST (loudness_reference_tones)
{
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        const int n = (int) (fs * 12);
        // EBU Tech 3341 style: stereo 1 kHz-ish sine at -23 dBFS in both channels reads -23 LUFS.
        const auto t = sine (fs, 997.0, std::pow (10.0, -23.0 / 20.0), n);
        LoudnessMeter m;
        runLoudness (m, fs, t, t);
        NEAR (m.integrated(), -23.0, 0.15);
        NEAR (m.momentary(), -23.0, 0.15);
        NEAR (m.shortTermLufs(), -23.0, 0.15);
        NEAR (m.range(), 0.0, 0.3);

        // BS.1770: a full-scale 997 Hz sine in one channel only reads -3.01 LUFS.
        const auto full = sine (fs, 997.0, 1.0, n);
        std::vector<float> zero ((size_t) n, 0.0f);
        LoudnessMeter m2;
        runLoudness (m2, fs, full, zero);
        NEAR (m2.momentary(), -3.01, 0.15);
    }
}

TEST (loudness_needs_a_full_window_before_reading)
{
    LoudnessMeter m;
    const auto t = sine (48000.0, 997.0, 0.1, 48000 * 3 / 10);                  // 0.3 s: less than a 400 ms window
    runLoudness (m, 48000.0, t, t);
    CHECK (m.momentary() <= LoudnessMeter::kSilence + 0.1);
    CHECK (m.integrated() <= LoudnessMeter::kSilence + 0.1);
}

TEST (integrated_gating_ignores_silence_and_quiet_parts)
{
    const double fs = 48000.0;
    auto tone = [&] (double db, double sec) { return sine (fs, 997.0, std::pow (10.0, db / 20.0), (int) (fs * sec)); };
    auto cat = [] (std::vector<float> a, const std::vector<float>& b) { a.insert (a.end(), b.begin(), b.end()); return a; };

    // 6 s of -23 LUFS then 10 s of digital silence: silence is below the absolute gate.
    auto x = cat (tone (-23.0, 6.0), std::vector<float> ((size_t) (fs * 10), 0.0f));
    LoudnessMeter a; runLoudness (a, fs, x, x);
    NEAR (a.integrated(), -23.0, 0.15);

    // 10 s at -20 then 10 s at -40: the quiet part sits more than 10 LU below the mean, so it is gated out.
    auto y = cat (tone (-20.0, 10.0), tone (-40.0, 10.0));
    LoudnessMeter b; runLoudness (b, fs, y, y);
    NEAR (b.integrated(), -20.0, 0.4);

    // 10 s at -20 then 10 s at -25: both count, so the result is the power mean (about -22.0).
    auto z = cat (tone (-20.0, 10.0), tone (-25.0, 10.0));
    LoudnessMeter c; runLoudness (c, fs, z, z);
    NEAR (c.integrated(), -22.0, 0.4);
}

TEST (loudness_range)
{
    const double fs = 48000.0;
    auto tone = [&] (double db, double sec) { return sine (fs, 997.0, std::pow (10.0, db / 20.0), (int) (fs * sec)); };
    auto x = tone (-20.0, 30.0); auto q = tone (-30.0, 30.0);
    x.insert (x.end(), q.begin(), q.end());
    LoudnessMeter m; runLoudness (m, fs, x, x);
    NEAR (m.range(), 10.0, 1.0);
}

TEST (true_peak_finds_inter_sample_overs)
{
    // A sine at fs/4 sampled 45 degrees off its crest: every sample is +-0.7071, but the waveform reaches 1.0.
    const auto x = sine (48000.0, 12000.0, 1.0, 48000, kPi / 4.0);
    LoudnessMeter m;
    runLoudness (m, 48000.0, x, x);
    NEAR (m.samplePeakDb(), dbfs (0.7071), 0.05);
    CHECK (m.truePeakDb() > -0.4 && m.truePeakDb() < 0.4);

    // A plain low-frequency sine: true peak equals sample peak within a hair.
    const auto y = sine (48000.0, 440.0, 0.5, 48000);
    LoudnessMeter m2; runLoudness (m2, 48000.0, y, y);
    NEAR (m2.truePeakDb(), m2.samplePeakDb(), 0.1);
}

TEST (block_maker_hops_are_exactly_100ms_and_survive_garbage)
{
    LoudnessBlockMaker bm; bm.prepare (44100.0);
    int hops = 0;
    std::vector<float> l (441, 0.2f), r (441, 0.2f);
    l[5] = std::nanf (""); r[7] = INFINITY;
    for (int i = 0; i < 100; ++i) bm.process (l.data(), r.data(), 441, [&] (const LoudnessHop& h) { ++hops; CHECK (std::isfinite (h.meanSqL) && std::isfinite (h.truePeak)); });
    CHECK (hops == 10);                                                         // 44100 frames = 1 s = 10 hops
    // Odd block sizes and a rate with a fractional hop length (32001 Hz: 3200.1 frames) must not drift.
    LoudnessBlockMaker b2; b2.prepare (32001.0);
    hops = 0;
    std::vector<float> s (97, 0.1f);
    for (int i = 0; i < 32001 / 97 * 10; ++i) b2.process (s.data(), s.data(), 97, [&] (const LoudnessHop&) { ++hops; });
    CHECK (hops == (32001 / 97 * 97 * 10) * 10 / 32001);
}

// ---------------------------------------------------------------------------------------------------- scope ring
TEST (scope_ring_returns_the_newest_frames_in_order)
{
    ScopeRing r;
    std::vector<float> l (1000), rr (1000);
    for (int i = 0; i < 1000; ++i) { l[(size_t) i] = (float) i; rr[(size_t) i] = (float) -i; }
    std::vector<float> a (600), b (600);
    CHECK (! r.latest (a.data(), b.data(), 600));                              // fewer than 600 frames written so far
    r.push (l.data(), rr.data(), 1000);
    CHECK (r.latest (a.data(), b.data(), 600));
    CHECK (a[0] == 400.0f && a[599] == 999.0f && b[0] == -400.0f);
    r.push (l.data(), rr.data(), 1000);                                        // wraps the ring several times over time
    for (int k = 0; k < 100; ++k) r.push (l.data(), rr.data(), 1000);
    CHECK (r.latest (a.data(), b.data(), 600));
    CHECK (a[0] == 400.0f && a[599] == 999.0f);
    std::vector<float> big (ScopeRing::kCapacity), big2 (ScopeRing::kCapacity);
    CHECK (! r.latest (big.data(), big2.data(), ScopeRing::kCapacity));         // asking for more than half the ring is refused
}

TEST (scope_ring_concurrent_reader_never_blocks_or_crashes)
{
    ScopeRing r;
    std::atomic<bool> stop { false };
    std::thread prod ([&]
    {
        std::vector<float> x (128, 0.5f);
        while (! stop.load()) r.push (x.data(), x.data(), 128);
    });
    std::vector<float> a (4096), b (4096);
    while (r.written() < 8192) std::this_thread::yield();                      // let the producer get going
    int ok = 0;
    for (int i = 0; i < 2000; ++i) if (r.latest (a.data(), b.data(), 4096)) ++ok;
    stop = true; prod.join();
    CHECK (ok > 0);
    for (float v : a) CHECK (v == 0.5f || v == 0.0f);                          // only values somebody wrote
}

// ---------------------------------------------------------------------------------------------------- mixer taps
namespace
{
struct Rig
{
    MixerCore m;
    std::vector<float> mixL = std::vector<float> (128), mixR = std::vector<float> (128);
    std::array<std::vector<float>, kBuses> busL, busR;
    std::array<float*, kBuses> pl {}, pr {};
    std::vector<float> in = std::vector<float> (128 * 2);
    Rig() { for (int b = 0; b < kBuses; ++b) { busL[(size_t) b].assign (128, 0.0f); busR[(size_t) b].assign (128, 0.0f); pl[(size_t) b] = busL[(size_t) b].data(); pr[(size_t) b] = busR[(size_t) b].data(); } m.prepareAnalysis (48000.0); }
    double phase = 0.0;
    void tone (float amp) { for (int n = 0; n < 128; ++n, phase += 2.0 * 3.14159265358979 * 997.0 / 48000.0) { const float v = amp * (float) std::sin (phase); in[(size_t) n * 2] = v; in[(size_t) n * 2 + 1] = v; } run(); }
    void block (float left, float right)
    {
        for (int n = 0; n < 128; ++n) { in[(size_t) n * 2] = left; in[(size_t) n * 2 + 1] = right; }
        run();
    }
    void run()
    {
        MixerCore::Output o; o.masterL = mixL.data(); o.masterR = mixR.data(); o.busL = pl.data(); o.busR = pr.data();
        MixerCore::Routing rt;
        m.process ({ in.data(), 2 }, {}, {}, 128, o, &rt);
    }
};
}

TEST (mixer_strip_tap_input_and_output)
{
    Rig r;
    r.m.setStripSource (0, { SourceType::net, 0 });
    r.m.setStripGainDb (0, -6.0206f);
    r.m.setStripBalance (0, 0.0f);
    std::vector<float> a (256), b (256);

    r.m.setScope (0, false);                                                    // input: before the fader
    for (int i = 0; i < 4; ++i) r.block (0.5f, -0.25f);
    CHECK (r.m.scopeRing().latest (a.data(), b.data(), 256));
    NEAR (a[255], 0.5, 1e-6); NEAR (b[255], -0.25, 1e-6);

    r.m.setScope (0, true);                                                     // output: after the fader (-6 dB = x0.5)
    for (int i = 0; i < 4; ++i) r.block (0.5f, -0.25f);
    CHECK (r.m.scopeRing().latest (a.data(), b.data(), 256));
    NEAR (a[255], 0.25, 1e-3); NEAR (b[255], -0.125, 1e-3);
}

TEST (mixer_tap_follows_the_selected_strip_only)
{
    Rig r;
    r.m.setStripSource (0, { SourceType::net, 0 });
    r.m.setStripSource (1, { SourceType::net, 0 });
    std::vector<float> a (128), b (128);
    r.m.setScope (1, false);
    const auto w0 = r.m.scopeRing().written();
    r.block (0.3f, 0.3f);
    CHECK (r.m.scopeRing().written() - w0 == 128);                              // exactly one strip's frames, not two
    r.m.setScope (-1, false);
    const auto w1 = r.m.scopeRing().written();
    r.block (0.3f, 0.3f);
    CHECK (r.m.scopeRing().written() == w1);                                    // no target, nothing written
    r.m.setScope (999, true);                                                   // out of range = none
    r.block (0.3f, 0.3f);
    CHECK (r.m.scopeRing().written() == w1);
}

TEST (mixer_tap_on_a_silent_strip_writes_silence)
{
    Rig r;                                                                      // strip 5 has no signal (default source is not net pair 0..)
    r.m.setStripSource (5, { SourceType::off, 0 });
    r.m.setScope (5, false);
    std::vector<float> a (128), b (128);
    r.block (0.5f, 0.5f);
    CHECK (r.m.scopeRing().latest (a.data(), b.data(), 128));
    for (float v : a) CHECK (v == 0.0f);
}

TEST (mixer_bus_tap_pre_and_post)
{
    Rig r;
    r.m.setStripSource (0, { SourceType::net, 0 });
    r.m.setStripOut (0, 1);                                                     // to bus 1
    r.m.setBusGainDb (0, -6.0206f);
    std::vector<float> a (128), b (128);
    r.m.setScope (kStrips + 0, false);
    for (int i = 0; i < 3; ++i) r.block (0.5f, 0.5f);
    CHECK (r.m.scopeRing().latest (a.data(), b.data(), 128));
    NEAR (a[127], 0.5, 1e-3);
    r.m.setScope (kStrips + 0, true);
    for (int i = 0; i < 3; ++i) r.block (0.5f, 0.5f);
    CHECK (r.m.scopeRing().latest (a.data(), b.data(), 128));
    NEAR (a[127], 0.25, 1e-3);
    r.m.setScope (kStrips + 7, false);                                          // an unused bus
    r.block (0.5f, 0.5f);
    CHECK (r.m.scopeRing().latest (a.data(), b.data(), 128));
    for (float v : a) CHECK (v == 0.0f);
}

TEST (mixer_master_ring_and_loudness_hops)
{
    Rig r;
    r.m.setStripSource (0, { SourceType::net, 0 });
    LoudnessMeter lm;
    int blocks = 375;                                                           // 1 s at 48 kHz in 128-frame blocks
    for (int i = 0; i < blocks; ++i)
    {
        r.tone (0.1f);
        LoudnessHop h;
        while (r.m.popLoudnessHop (h)) lm.addHop (h);
    }
    std::vector<float> a (128), b (128);
    CHECK (r.m.masterRing().latest (a.data(), b.data(), 128));
    NEAR (a[127], r.mixL[127], 1e-9);                                           // the master ring holds what the master output carried
    NEAR (lm.momentary(), -20.0, 0.3);                                         // -20 dBFS stereo sine = -20 LUFS
    CHECK (r.m.loudnessHopsDropped() == 0);
    NEAR (lm.samplePeakDb(), dbfs (0.1), 0.05);
}

TEST (mixer_hop_queue_overflow_is_counted_not_blocking)
{
    Rig r;
    r.m.setStripSource (0, { SourceType::net, 0 });
    for (int i = 0; i < 375 * 30; ++i) r.block (0.1f, 0.1f);                    // 30 s with nobody reading: 300 hops > 256 slots
    CHECK (r.m.loudnessHopsDropped() > 0);
    r.m.prepareAnalysis (48000.0);                                              // a device restart empties the queue
    LoudnessHop h;
    CHECK (! r.m.popLoudnessHop (h));
}

int main()
{
    for (const auto& t : tests())
    {
        const int before = failures;
        t.f();
        std::printf ("%-52s %s\n", t.n, failures == before ? "ok" : "FAILED");
    }
    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
