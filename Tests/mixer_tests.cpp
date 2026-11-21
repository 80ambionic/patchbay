// Mixer DSP core + settings model. Pure C++ (no JUCE).
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>
#include "Mixer/DeviceRouter.h"
#include "Mixer/MixerCore.h"

using namespace pb;
static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (! (c)) { ++failures; std::printf ("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define NEAR(a, b, tol) do { ++checks; const double va = (a), vb = (b); if (! (std::abs (va - vb) <= (tol))) { ++failures; std::printf ("  FAIL line %d: %s = %g, expected %g\n", __LINE__, #a, va, vb); } } while (0)
#define TEST(name) static void name(); struct Reg_##name { Reg_##name() { tests().push_back ({ #name, &name }); } } reg_##name; static void name()

struct T { const char* n; void (*f)(); };
static std::vector<T>& tests() { static std::vector<T> t; return t; }

// Interleaved constant-level input on `ch` channels, value v on channel c, 0 elsewhere.
static std::vector<float> make (int ch, int frames, int onlyCh, float v)
{
    std::vector<float> d ((size_t) ch * (size_t) frames, 0.0f);
    for (int n = 0; n < frames; ++n) d[(size_t) n * (size_t) ch + (size_t) onlyCh] = v;
    return d;
}

static MixerSettings flat()      // all strips unity, centred, no mute
{
    MixerSettings s;
    for (auto& st : s.strips) st.pan = 0.0f;
    return s;
}

struct Out { std::vector<float> l, r; };
static Out run (MixerCore& m, const std::vector<float>& in, int ch, int frames)
{
    Out o; o.l.assign ((size_t) frames, 0.0f); o.r.assign ((size_t) frames, 0.0f);
    m.process (in.data(), ch, frames, o.l.data(), o.r.data());
    return o;
}

TEST (unity_centre_is_minus_3db)
{
    MixerCore m; m.setSettings (flat());
    const auto o = run (m, make (2, 64, 0, 0.5f), 2, 64);
    const double expect = 0.5 * std::sqrt (0.5);
    for (int n = 0; n < 64; ++n) { NEAR (o.l[(size_t) n], expect, 1e-6); NEAR (o.r[(size_t) n], expect, 1e-6); }
}

TEST (hard_pan_left_right)
{
    auto s = flat(); s.strips[0].pan = -1.0f; s.strips[1].pan = 1.0f;
    MixerCore m; m.setSettings (s);
    auto o = run (m, make (2, 32, 0, 0.8f), 2, 32);
    NEAR (o.l[5], 0.8, 1e-6); NEAR (o.r[5], 0.0, 1e-6);
    m.reset();
    o = run (m, make (2, 32, 1, 0.8f), 2, 32);
    NEAR (o.l[5], 0.0, 1e-6); NEAR (o.r[5], 0.8, 1e-6);
}

TEST (constant_power_across_pan)
{
    for (float p = -1.0f; p <= 1.0f; p += 0.125f)
    {
        auto s = flat(); s.strips[0].pan = p;
        MixerCore m; m.setSettings (s);
        const auto o = run (m, make (1, 16, 0, 0.25f), 1, 16);     // mono: strips 0 and 1 both fed; use strip 0 only
        (void) o;
        MixerCore m2; auto s2 = s; s2.strips[1].mute = true; m2.setSettings (s2);
        const auto o2 = run (m2, make (1, 16, 0, 1.0f), 1, 16);
        NEAR (o2.l[3] * o2.l[3] + o2.r[3] * o2.r[3], 1.0, 1e-5);   // L^2 + R^2 == 1 for every pan position
    }
}

TEST (gain_db_accuracy)
{
    NEAR (gainDbToLinear (0.0f), 1.0, 1e-7);
    NEAR (gainDbToLinear (-6.0206f), 0.5, 1e-4);
    NEAR (gainDbToLinear (20.0f - 8.0f), 3.98107, 1e-4);          // +12 dB
    CHECK (gainDbToLinear (kMinGainDb) == 0.0f);                  // fader floor is true silence
    CHECK (gainDbToLinear (-1000.0f) == 0.0f);
    NEAR (gainDbToLinear (std::numeric_limits<float>::quiet_NaN()), 1.0, 1e-7);   // NaN -> 0 dB, never NaN gain
}

TEST (mute_solo_rules)
{
    auto s = flat(); s.strips[0].pan = -1.0f; s.strips[1].pan = 1.0f;
    {   // mute
        auto t = s; t.strips[0].mute = true;
        MixerCore m; m.setSettings (t);
        const auto o = run (m, make (2, 16, 0, 1.0f), 2, 16);
        NEAR (o.l[4], 0.0, 1e-7);
    }
    {   // solo strip 1: strip 0 goes silent, strip 1 stays
        auto t = s; t.strips[1].solo = true;
        MixerCore m; m.setSettings (t);
        std::vector<float> in (2 * 16, 0.5f);
        const auto o = run (m, in, 2, 16);
        NEAR (o.l[4], 0.0, 1e-7); NEAR (o.r[4], 0.5, 1e-6);
    }
    {   // mute beats solo
        auto t = s; t.strips[1].solo = true; t.strips[1].mute = true;
        MixerCore m; m.setSettings (t);
        std::vector<float> in (2 * 16, 0.5f);
        const auto o = run (m, in, 2, 16);
        NEAR (o.l[4], 0.0, 1e-7); NEAR (o.r[4], 0.0, 1e-7);
    }
    {   // two solos both play
        auto t = s; t.strips[0].solo = true; t.strips[1].solo = true;
        MixerCore m; m.setSettings (t);
        std::vector<float> in (2 * 16, 0.5f);
        const auto o = run (m, in, 2, 16);
        NEAR (o.l[4], 0.5, 1e-6); NEAR (o.r[4], 0.5, 1e-6);
    }
}

TEST (channel_mapping_rules)
{
    auto s = flat();
    {   // mono input feeds strips 1 and 2 only
        MixerCore m; m.setSettings (s);
        const auto o = run (m, make (1, 16, 0, 0.5f), 1, 16);
        NEAR (o.l[2], 2.0 * 0.5 * std::sqrt (0.5), 1e-6);          // two strips, each centred at -3 dB
    }
    {   // 8-channel input with only channel 7 live
        MixerCore m; m.setSettings (s);
        const auto o = run (m, make (8, 16, 7, 0.4f), 8, 16);
        NEAR (o.l[2], 0.4 * std::sqrt (0.5), 1e-6);
    }
    {   // more channels than strips: extra ignored, no out-of-bounds read (ASan checks)
        MixerCore m; m.setSettings (s);
        std::vector<float> in (12 * 16, 0.1f);
        const auto o = run (m, in, 12, 16);
        NEAR (o.l[2], 8 * 0.1 * std::sqrt (0.5), 1e-5);
    }
    {   // stereo input: strips 3..8 silent
        MixerCore m; m.setSettings (s);
        std::vector<float> in (2 * 16, 0.0f);
        const auto o = run (m, in, 2, 16);
        NEAR (o.l[2], 0.0, 1e-9);
    }
}

TEST (master_gain_and_mute)
{
    auto s = flat(); s.strips[0].pan = -1.0f; s.masterDb = -6.0206f;
    MixerCore m; m.setSettings (s);
    auto o = run (m, make (2, 16, 0, 0.8f), 2, 16);
    NEAR (o.l[3], 0.4, 1e-4);
    s.masterMute = true; m.setSettings (s); m.reset();
    o = run (m, make (2, 16, 0, 0.8f), 2, 16);
    NEAR (o.l[3], 0.0, 1e-9);
}

TEST (ramp_is_smooth_and_lands_on_target)
{
    auto s = flat(); s.strips[0].pan = -1.0f;
    MixerCore m; m.setSettings (s);
    const auto in = make (2, 256, 0, 1.0f);                        // only strip 1 carries signal
    run (m, in, 2, 256);                                           // establishes unity (first block snaps)
    m.setStripGainDb (0, -20.0f);                                  // a big jump: 0.1 linear
    const auto o = run (m, in, 2, 256);
    double maxStep = 0.0;
    for (int n = 1; n < 256; ++n) maxStep = std::max (maxStep, (double) std::abs (o.l[(size_t) n] - o.l[(size_t) n - 1]));
    CHECK (maxStep < 0.01);                                        // would be 0.9 without ramping
    CHECK (o.l[0] > 0.9);                                          // starts near the old gain
    NEAR (o.l[255], 0.1, 0.005);                                   // ends at the target
    const auto o2 = run (m, in, 2, 256);                           // next block is exactly steady
    NEAR (o2.l[0], 0.1, 1e-5); NEAR (o2.l[255], 0.1, 1e-5);
}

TEST (first_block_snaps_no_fade_in)
{
    auto s = flat(); s.strips[0].pan = -1.0f;
    MixerCore m; m.setSettings (s);
    const auto o = run (m, make (2, 64, 0, 0.5f), 2, 64);
    NEAR (o.l[0], 0.5, 1e-6);
}

TEST (block_size_invariance_with_static_settings)
{
    auto s = flat(); s.strips[0].gainDb = -3.0f; s.strips[1].pan = 0.5f; s.masterDb = -1.0f;
    std::vector<float> in (2 * 512);
    for (size_t i = 0; i < in.size(); ++i) in[i] = std::sin ((float) i * 0.05f) * 0.5f;

    MixerCore a; a.setSettings (s);
    const auto oa = run (a, in, 2, 512);

    MixerCore b; b.setSettings (s);
    std::vector<float> bl, br;
    for (int off = 0; off < 512; off += 64)
    {
        std::vector<float> chunk (in.begin() + off * 2, in.begin() + (off + 64) * 2);
        const auto o = run (b, chunk, 2, 64);
        bl.insert (bl.end(), o.l.begin(), o.l.end());
        br.insert (br.end(), o.r.begin(), o.r.end());
    }
    for (int n = 0; n < 512; ++n) { NEAR (oa.l[(size_t) n], bl[(size_t) n], 1e-6); NEAR (oa.r[(size_t) n], br[(size_t) n], 1e-6); }
}

TEST (nonfinite_input_is_silenced_and_output_clamped)
{
    MixerCore m; m.setSettings (flat());
    std::vector<float> in (2 * 32, 0.25f);
    in[3] = std::numeric_limits<float>::quiet_NaN();
    in[10] = std::numeric_limits<float>::infinity();
    in[11] = -std::numeric_limits<float>::infinity();
    const auto o = run (m, in, 2, 32);
    for (int n = 0; n < 32; ++n) CHECK (std::isfinite (o.l[(size_t) n]) && std::isfinite (o.r[(size_t) n]));
    CHECK (! m.takePeaks().clipped);

    auto s = flat(); s.strips[0].gainDb = 12.0f; s.strips[0].pan = -1.0f; m.setSettings (s); m.reset();
    const auto c = run (m, make (2, 16, 0, 0.9f), 2, 16);         // 0.9 * 3.98 > 1
    for (int n = 0; n < 16; ++n) CHECK (c.l[(size_t) n] <= 1.0f && c.l[(size_t) n] >= -1.0f);
    NEAR (c.l[4], 1.0, 1e-7);
    const auto p = m.takePeaks();
    CHECK (p.clipped);
    CHECK (! m.takePeaks().clipped);                               // flag resets when read
}

TEST (meters_post_fader_pre_mute_and_reset_on_read)
{
    auto s = flat(); s.strips[0].gainDb = -6.0206f; s.strips[0].mute = true; s.strips[0].pan = -1.0f;
    MixerCore m; m.setSettings (s);
    run (m, make (2, 32, 0, 0.8f), 2, 32);
    auto p = m.takePeaks();
    NEAR (p.strip[0], 0.4, 1e-4);                                  // muted strip still metered, post-fader
    NEAR (p.masterL, 0.0, 1e-9);                                   // ...but silent on the master
    CHECK (p.strip[1] == 0.0f);
    p = m.takePeaks();
    CHECK (p.strip[0] == 0.0f);
}

TEST (control_inputs_are_sanitised)
{
    MixerCore m;
    m.setStripGainDb (-1, 5.0f); m.setStripGainDb (99, 5.0f);      // out-of-range index: ignored, no crash
    m.setStripPan (7, std::numeric_limits<float>::quiet_NaN());
    m.setStripGainDb (0, 1e30f);
    m.setStripPan (0, -50.0f);
    const auto o = run (m, make (2, 8, 0, 0.01f), 2, 8);
    CHECK (std::isfinite (o.l[2]) && std::isfinite (o.r[2]));
    NEAR (o.l[2], 0.01 * gainDbToLinear (kMaxGainDb), 1e-5);       // clamped to +12 dB, hard left
    CHECK (clampGainDb (-1e9f) == kMinGainDb && clampGainDb (1e9f) == kMaxGainDb);
    CHECK (clampPan (std::numeric_limits<float>::infinity()) == 0.0f);

    MixerSettings s; s.strips[0].gainDb = std::numeric_limits<float>::quiet_NaN(); s.masterDb = 500.0f;
    s.strips[1].name = std::string ("a\x01\x02" "b") + std::string (40, 'z');
    s.sanitize();
    CHECK (s.strips[0].gainDb == 0.0f && s.masterDb == kMaxGainDb);
    CHECK (s.strips[1].name.size() == kMaxStripNameLength && s.strips[1].name.substr (0, 2) == "ab");
}

TEST (defaults_play_stereo_out_of_the_box)
{
    MixerCore m;                                                   // default settings: strip1 hard L, strip2 hard R
    std::vector<float> in (2 * 16);
    for (int n = 0; n < 16; ++n) { in[(size_t) n * 2] = 0.3f; in[(size_t) n * 2 + 1] = -0.6f; }
    const auto o = run (m, in, 2, 16);
    NEAR (o.l[5], 0.3, 1e-6); NEAR (o.r[5], -0.6, 1e-6);
}

TEST (concurrent_control_and_process)
{
    MixerCore m;
    std::atomic<bool> stop { false };
    std::thread ui ([&]
    {
        float v = 0.0f;
        while (! stop.load())
        {
            v += 0.37f; if (v > 12.0f) v = -70.0f;
            const int i = (int) std::fmod (v * 100.0f + 1000.0f, 8.0f);
            m.setStripGainDb (i, v); m.setStripPan (i, v / 12.0f); m.setStripMute (i, v > 6.0f); m.setStripSolo (i, v < -65.0f);
            m.setMasterDb (v);
            (void) m.takePeaks();
        }
    });
    std::vector<float> in (2 * 128, 0.5f);
    Out o; o.l.resize (128); o.r.resize (128);
    bool finite = true;
    for (int i = 0; i < 20000; ++i)
    {
        m.process (in.data(), 2, 128, o.l.data(), o.r.data());
        for (int n = 0; n < 128; ++n) finite = finite && std::isfinite (o.l[(size_t) n]) && std::abs (o.l[(size_t) n]) <= 1.0f;
    }
    stop = true; ui.join();
    CHECK (finite);
}


// ---- banks, direct outs, device routing -----------------------------------------------------------------------
using Dir = std::array<std::vector<float>, kMixerStrips>;
struct DirPtrs
{
    Dir buf; std::array<float*, kMixerStrips> p {};
    explicit DirPtrs (int frames) { for (size_t i = 0; i < buf.size(); ++i) { buf[i].assign ((size_t) frames, 123.0f); p[i] = buf[i].data(); } }
};

TEST (app_bank_feeds_strips_9_to_16)
{
    auto s = flat(); s.strips[8].pan = -1.0f; s.strips[9].pan = 1.0f;
    MixerCore m; m.setSettings (s);
    std::vector<float> app (8 * 16, 0.0f);
    for (int n = 0; n < 16; ++n) { app[(size_t) n * 8 + 0] = 0.5f; app[(size_t) n * 8 + 1] = -0.25f; }
    Out o; o.l.assign (16, 0); o.r.assign (16, 0);
    m.process (MixerCore::Input {}, MixerCore::Input { app.data(), 8 }, 16, o.l.data(), o.r.data(), nullptr, nullptr);
    NEAR (o.l[4], 0.5, 1e-6); NEAR (o.r[4], -0.25, 1e-6);
    const auto pk = m.takePeaks();
    NEAR (pk.strip[8], 0.5, 1e-6); NEAR (pk.strip[9], 0.25, 1e-6); CHECK (pk.strip[0] == 0.0f);
}

TEST (app_bank_has_no_mono_duplication_and_banks_do_not_bleed)
{
    MixerCore m; m.setSettings (flat());
    std::vector<float> app (1 * 16, 0.5f), net (2 * 16, 0.0f);
    Out o; o.l.assign (16, 0); o.r.assign (16, 0);
    m.process (MixerCore::Input { net.data(), 2 }, MixerCore::Input { app.data(), 1 }, 16, o.l.data(), o.r.data(), nullptr, nullptr);
    NEAR (o.l[3], 0.5 * std::sqrt (0.5), 1e-6);                   // only app channel 1 (strip 9), not strip 10
    m.takePeaks();
    std::vector<float> net2 (2 * 16, 0.3f);
    m.process (MixerCore::Input { net2.data(), 2 }, MixerCore::Input {}, 16, o.l.data(), o.r.data(), nullptr, nullptr);
    const auto pk = m.takePeaks();
    for (int i = 8; i < kMixerStrips; ++i) CHECK (pk.strip[(size_t) i] == 0.0f);
}

TEST (solo_spans_both_banks)
{
    auto s = flat(); s.strips[8].solo = true; s.strips[8].pan = -1.0f;
    MixerCore m; m.setSettings (s);
    std::vector<float> net (2 * 16, 0.9f), app (8 * 16, 0.2f);
    Out o; o.l.assign (16, 0); o.r.assign (16, 0);
    m.process (MixerCore::Input { net.data(), 2 }, MixerCore::Input { app.data(), 8 }, 16, o.l.data(), o.r.data(), nullptr, nullptr);
    NEAR (o.l[4], 0.2, 1e-6); NEAR (o.r[4], 0.0, 1e-6);           // network strips silenced by the app solo
}

TEST (direct_out_bypasses_pan_and_master)
{
    auto s = flat(); s.strips[0].gainDb = -6.0206f; s.strips[0].directOut = 5; s.strips[0].pan = -1.0f; s.masterDb = -20.0f;
    s.strips[1].mute = true;
    MixerCore m; m.setSettings (s);
    DirPtrs d (16);
    MixerCore::Routing r;
    std::vector<float> in = make (2, 16, 0, 0.8f);
    Out o; o.l.assign (16, 0); o.r.assign (16, 0);
    m.process (MixerCore::Input { in.data(), 2 }, MixerCore::Input {}, 16, o.l.data(), o.r.data(), d.p.data(), &r);
    CHECK (r.directCh[0] == 5);
    for (int i = 1; i < kMixerStrips; ++i) CHECK (r.directCh[(size_t) i] == 0);
    NEAR (d.buf[0][7], 0.4, 1e-4);                                 // post-fader, no pan law, no master gain
    NEAR (o.l[7], 0.0, 1e-9); NEAR (o.r[7], 0.0, 1e-9);            // and absent from the master bus
    CHECK (r.masterFirst == 1);
}

TEST (direct_out_respects_mute_solo_and_is_ignored_without_buffer)
{
    auto s = flat(); s.strips[0].directOut = 3; s.strips[0].pan = -1.0f; s.strips[0].mute = true;
    {
        MixerCore m; m.setSettings (s);
        DirPtrs d (16); MixerCore::Routing r; auto in = make (2, 16, 0, 0.5f);
        Out o; o.l.assign (16, 0); o.r.assign (16, 0);
        m.process (MixerCore::Input { in.data(), 2 }, MixerCore::Input {}, 16, o.l.data(), o.r.data(), d.p.data(), &r);
        CHECK (r.directCh[0] == 3);
        NEAR (d.buf[0][5], 0.0, 1e-9);                             // muted: the buffer is written, with silence (no stale data)
    }
    {
        s.strips[0].mute = false;
        MixerCore m; m.setSettings (s);
        auto in = make (2, 16, 0, 0.5f);
        const auto o = run (m, in, 2, 16);                         // compat process(): no direct buffers
        NEAR (o.l[5], 0.5, 1e-6);                                  // plays through the master instead of vanishing
    }
}

TEST (direct_out_with_no_input_channel_writes_silence)
{
    auto s = flat(); s.strips[5].directOut = 9;
    MixerCore m; m.setSettings (s);
    DirPtrs d (8); MixerCore::Routing r;
    auto in = make (2, 8, 0, 0.5f);
    Out o; o.l.assign (8, 0); o.r.assign (8, 0);
    m.process (MixerCore::Input { in.data(), 2 }, MixerCore::Input {}, 8, o.l.data(), o.r.data(), d.p.data(), &r);
    CHECK (r.directCh[5] == 9);
    for (float v : d.buf[5]) CHECK (v == 0.0f);
}

TEST (switching_route_ramps_without_clicks)
{
    auto s = flat(); s.strips[0].pan = -1.0f;
    MixerCore m; m.setSettings (s);
    DirPtrs d (256); MixerCore::Routing r;
    auto in = make (2, 256, 0, 1.0f);
    Out o; o.l.assign (256, 0); o.r.assign (256, 0);
    m.process (MixerCore::Input { in.data(), 2 }, MixerCore::Input {}, 256, o.l.data(), o.r.data(), d.p.data(), &r);
    m.setStripDirectOut (0, 2);
    m.process (MixerCore::Input { in.data(), 2 }, MixerCore::Input {}, 256, o.l.data(), o.r.data(), d.p.data(), &r);
    double maxStepM = 0, maxStepD = 0;
    for (int n = 1; n < 256; ++n)
    {
        maxStepM = std::max (maxStepM, (double) std::abs (o.l[(size_t) n] - o.l[(size_t) n - 1]));
        maxStepD = std::max (maxStepD, (double) std::abs (d.buf[0][(size_t) n] - d.buf[0][(size_t) n - 1]));
    }
    CHECK (maxStepM < 0.01 && maxStepD < 0.01);                    // master fades out while the direct path fades in
    NEAR (o.l[255], 0.0, 0.01); NEAR (d.buf[0][255], 1.0, 0.01);
}

TEST (master_out_pair_is_reported_and_clamped)
{
    MixerCore m;
    m.setMasterOut (7); m.setStripDirectOut (0, 9999);
    DirPtrs d (4); MixerCore::Routing r; auto in = make (2, 4, 0, 0.1f);
    Out o; o.l.assign (4, 0); o.r.assign (4, 0);
    m.process (MixerCore::Input { in.data(), 2 }, MixerCore::Input {}, 4, o.l.data(), o.r.data(), d.p.data(), &r);
    CHECK (r.masterFirst == 7);
    CHECK (r.directCh[0] == kMaxDeviceOutputs);
    m.setMasterOut (-4); m.process (MixerCore::Input { in.data(), 2 }, MixerCore::Input {}, 4, o.l.data(), o.r.data(), d.p.data(), &r);
    CHECK (r.masterFirst == 1);
    m.setMasterOut (1000); m.process (MixerCore::Input { in.data(), 2 }, MixerCore::Input {}, 4, o.l.data(), o.r.data(), d.p.data(), &r);
    CHECK (r.masterFirst == kMaxDeviceOutputs - 1);                // the pair must still fit: (63, 64)
}

struct Dev
{
    std::vector<std::vector<float>> ch; std::vector<float*> p;
    Dev (int n, int frames, float fill = 0.0f) { ch.assign ((size_t) n, std::vector<float> ((size_t) frames, fill)); for (auto& c : ch) p.push_back (c.data()); }
};

TEST (router_master_pair_placement)
{
    std::vector<float> L (8, 0.5f), R (8, -0.5f);
    Dev dev (8, 8);
    CHECK (! routeToDevice (L.data(), R.data(), 3, nullptr, nullptr, 0, 8, dev.p.data(), 8, 0));
    for (int c = 0; c < 8; ++c)
        for (int n = 0; n < 8; ++n)
            NEAR (dev.ch[(size_t) c][(size_t) n], c == 2 ? 0.5 : (c == 3 ? -0.5 : 0.0), 1e-9);
}

TEST (router_mono_and_edge_devices)
{
    std::vector<float> L (4, 0.6f), R (4, 0.2f);
    {   // mono device: fold to L
        Dev dev (1, 4);
        routeToDevice (L.data(), R.data(), 1, nullptr, nullptr, 0, 4, dev.p.data(), 1, 0);
        NEAR (dev.ch[0][2], 0.4, 1e-6);
    }
    {   // pair starting on the last channel of a stereo device: L plays folded, nothing is written out of bounds
        Dev dev (2, 4);
        routeToDevice (L.data(), R.data(), 2, nullptr, nullptr, 0, 4, dev.p.data(), 2, 0);
        NEAR (dev.ch[0][1], 0.0, 1e-9); NEAR (dev.ch[1][1], 0.4, 1e-6);
    }
    {   // pair entirely past the device: nothing happens
        Dev dev (2, 4);
        CHECK (! routeToDevice (L.data(), R.data(), 5, nullptr, nullptr, 0, 4, dev.p.data(), 2, 0));
        NEAR (dev.ch[0][1], 0.0, 1e-9); NEAR (dev.ch[1][1], 0.0, 1e-9);
    }
    {   // null channel pointer + zero channels + zero frames are all tolerated
        Dev dev (2, 4); dev.p[0] = nullptr;
        routeToDevice (L.data(), R.data(), 1, nullptr, nullptr, 0, 4, dev.p.data(), 2, 0);
        routeToDevice (L.data(), R.data(), 1, nullptr, nullptr, 0, 4, dev.p.data(), 0, 0);
        routeToDevice (L.data(), R.data(), 1, nullptr, nullptr, 0, 0, dev.p.data(), 2, 0);
        routeToDevice (L.data(), R.data(), 1, nullptr, nullptr, 0, 4, nullptr, 2, 0);
        CHECK (true);
    }
}

TEST (router_direct_outs_sum_offset_and_clamp)
{
    std::vector<float> L (8, 0.0f), R (8, 0.0f), a (8, 0.3f), b (8, 0.4f), c (8, 0.9f);
    const float* dp[3] { a.data(), b.data(), c.data() };
    const int dc[3] { 4, 4, 0 };                                   // two strips share channel 4; the third is not routed
    Dev dev (6, 16);                                               // 16-frame device buffers, writing at offset 8
    CHECK (! routeToDevice (L.data(), R.data(), 1, dp, dc, 3, 8, dev.p.data(), 6, 8));
    NEAR (dev.ch[3][8], 0.7, 1e-6); NEAR (dev.ch[3][15], 0.7, 1e-6);
    NEAR (dev.ch[3][0], 0.0, 1e-9);                                // below the offset untouched
    NEAR (dev.ch[4][9], 0.0, 1e-9);

    std::vector<float> hot (8, 0.8f);
    const float* dp2[2] { hot.data(), hot.data() };
    const int dc2[2] { 2, 2 };
    Dev dev2 (4, 8);
    CHECK (routeToDevice (L.data(), R.data(), 1, dp2, dc2, 2, 8, dev2.p.data(), 4, 0));   // 1.6 -> clamped, reported
    NEAR (dev2.ch[1][3], 1.0, 1e-9);

    const int dcBad[2] { 99, -3 };                                 // outside the device or negative: skipped
    Dev dev3 (4, 8);
    routeToDevice (L.data(), R.data(), 1, dp2, dcBad, 2, 8, dev3.p.data(), 4, 0);
    for (auto& ch : dev3.ch) NEAR (ch[2], 0.0, 1e-9);
}

TEST (router_master_and_direct_share_a_channel)
{
    std::vector<float> L (4, 0.5f), R (4, 0.0f), d (4, 0.25f);
    const float* dp[1] { d.data() };
    const int dc[1] { 1 };
    Dev dev (2, 4);
    routeToDevice (L.data(), R.data(), 1, dp, dc, 1, 4, dev.p.data(), 2, 0);
    NEAR (dev.ch[0][1], 0.75, 1e-6);
}

TEST (app_and_direct_settings_sanitise)
{
    MixerSettings s;
    s.strips[0].directOut = -5; s.strips[1].directOut = 5000; s.masterOut = 0;
    s.apps[0] = { "com.apple.Safari", std::string ("Safari") + std::string (100, 'x'), false };
    s.apps[1] = { "bad id; rm -rf", "Evil", true };
    s.apps[2] = { std::string (500, 'a'), "Long", true };
    s.apps[3] = { "com.spotify.client", "Spotify", true };
    s.sanitize();
    CHECK (s.strips[0].directOut == 0 && s.strips[1].directOut == kMaxDeviceOutputs && s.masterOut == 1);
    CHECK (s.apps[0].bundleId == "com.apple.Safari" && s.apps[0].name.size() == kMaxAppNameLength && ! s.apps[0].silenceOriginal);
    CHECK (s.apps[1] == AppTarget() && s.apps[2] == AppTarget());  // invalid bundle IDs: whole slot cleared
    CHECK (s.apps[3].bundleId == "com.spotify.client");
    CHECK (isValidBundleId ("com.google.Chrome.helper") && ! isValidBundleId ("") && ! isValidBundleId ("a b") && ! isValidBundleId ("a/b"));
}

TEST (concurrent_routing_changes)
{
    MixerCore m;
    std::atomic<bool> stop { false };
    std::thread ui ([&]
    {
        int v = 0;
        while (! stop.load()) { ++v; m.setStripDirectOut (v % kMixerStrips, v % 70); m.setMasterOut (v % 70); m.setStripSolo (v % kMixerStrips, v % 3 == 0); }
    });
    DirPtrs d (128); MixerCore::Routing r;
    std::vector<float> net (2 * 128, 0.5f), app (8 * 128, 0.2f);
    Out o; o.l.assign (128, 0); o.r.assign (128, 0);
    Dev dev (8, 128);
    bool ok = true;
    for (int i = 0; i < 20000; ++i)
    {
        m.process (MixerCore::Input { net.data(), 2 }, MixerCore::Input { app.data(), 8 }, 128, o.l.data(), o.r.data(), d.p.data(), &r);
        for (auto& c : dev.ch) std::fill (c.begin(), c.end(), 0.0f);
        routeToDevice (o.l.data(), o.r.data(), r.masterFirst, (const float* const*) d.p.data(), r.directCh.data(), kMixerStrips, 128, dev.p.data(), 8, 0);
        for (auto& c : dev.ch) for (float v : c) ok = ok && std::isfinite (v) && std::abs (v) <= 1.0f;
        ok = ok && r.masterFirst >= 1 && r.masterFirst <= kMaxDeviceOutputs - 1;
    }
    stop = true; ui.join();
    CHECK (ok);
}

TEST (input_bank_feeds_strips_17_to_24)
{
    static_assert (kMixerStrips == 24 && kNetworkStrips + kAppStrips == 16, "bank layout");
    auto s = flat(); s.strips[16].pan = -1.0f; s.strips[17].pan = 1.0f;
    MixerCore m; m.setSettings (s);
    std::vector<float> dev (4 * 16, 0.0f);                                    // a 4-channel input device
    for (int n = 0; n < 16; ++n) { dev[(size_t) n * 4 + 0] = 0.6f; dev[(size_t) n * 4 + 1] = -0.3f; dev[(size_t) n * 4 + 3] = 0.1f; }
    Out o; o.l.assign (16, 0); o.r.assign (16, 0);
    m.process (MixerCore::Input {}, MixerCore::Input {}, MixerCore::Input { dev.data(), 4 }, 16, o.l.data(), o.r.data(), nullptr, nullptr);
    NEAR (o.l[3], 0.6 + 0.1 * std::sqrt (0.5), 1e-6);                         // ch1 hard left, ch4 centred on strip 20; ch3 silent
    NEAR (o.r[3], -0.3 + 0.1 * std::sqrt (0.5), 1e-6);
    const auto pk = m.takePeaks();
    NEAR (pk.strip[16], 0.6, 1e-6); NEAR (pk.strip[17], 0.3, 1e-6); CHECK (pk.strip[18] == 0.0f); NEAR (pk.strip[19], 0.1, 1e-6);
    for (int i = 0; i < 16; ++i) CHECK (pk.strip[(size_t) i] == 0.0f);
}

TEST (input_bank_has_no_mono_duplication_and_solo_spans_three_banks)
{
    MixerCore m; m.setSettings (flat());
    std::vector<float> dev (1 * 16, 0.5f);
    Out o; o.l.assign (16, 0); o.r.assign (16, 0);
    m.process (MixerCore::Input {}, MixerCore::Input {}, MixerCore::Input { dev.data(), 1 }, 16, o.l.data(), o.r.data(), nullptr, nullptr);
    NEAR (o.l[2], 0.5 * std::sqrt (0.5), 1e-6);                               // only strip 17, not also strip 18

    auto s = flat(); s.strips[20].solo = true; s.strips[20].pan = -1.0f;
    MixerCore m2; m2.setSettings (s);
    std::vector<float> net (2 * 16, 0.9f), app (8 * 16, 0.8f), in8 (8 * 16, 0.2f);
    m2.process (MixerCore::Input { net.data(), 2 }, MixerCore::Input { app.data(), 8 }, MixerCore::Input { in8.data(), 8 }, 16, o.l.data(), o.r.data(), nullptr, nullptr);
    NEAR (o.l[4], 0.2, 1e-6); NEAR (o.r[4], 0.0, 1e-6);                       // only the soloed input strip is heard
}

TEST (input_strip_direct_out_and_null_bank_are_safe)
{
    auto s = flat(); s.strips[23].directOut = 4; s.strips[23].pan = -1.0f;
    MixerCore m; m.setSettings (s);
    DirPtrs d (8); MixerCore::Routing r;
    std::vector<float> in8 (8 * 8, 0.0f);
    for (int n = 0; n < 8; ++n) in8[(size_t) n * 8 + 7] = 0.5f;
    Out o; o.l.assign (8, 0); o.r.assign (8, 0);
    m.process (MixerCore::Input {}, MixerCore::Input {}, MixerCore::Input { in8.data(), 8 }, 8, o.l.data(), o.r.data(), d.p.data(), &r);
    CHECK (r.directCh[23] == 4);
    NEAR (d.buf[23][3], 0.5, 1e-6); NEAR (o.l[3], 0.0, 1e-9);
    // a bank that claims channels but has no data pointer must not be dereferenced
    m.process (MixerCore::Input {}, MixerCore::Input { nullptr, 8 }, MixerCore::Input { nullptr, 8 }, 8, o.l.data(), o.r.data(), d.p.data(), &r);
    for (float v : d.buf[23]) CHECK (v == 0.0f);
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
