// Mixer DSP core, device router and settings model. Pure C++ (no JUCE).
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

// Interleaved bank, `ch` channels, channel c holds the constant vals[c] (0 if beyond vals).
static std::vector<float> bank (int ch, int frames, std::vector<float> vals)
{
    std::vector<float> d ((size_t) ch * (size_t) frames, 0.0f);
    for (int n = 0; n < frames; ++n)
        for (int c = 0; c < ch && c < (int) vals.size(); ++c) d[(size_t) n * (size_t) ch + (size_t) c] = vals[(size_t) c];
    return d;
}

// Everything off, every strip centred: tests switch on only what they need.
static MixerSettings blank()
{
    MixerSettings s;
    for (auto& st : s.strips) { st.src = { SourceType::off, 0 }; }
    return s;
}

struct Rig
{
    static constexpr float kSentinel = 7.0f;
    int frames;
    std::vector<float> mL, mR;
    std::array<std::vector<float>, kBuses> bL, bR;
    std::array<float*, kBuses> bLp {}, bRp {};
    MixerCore::Routing routing;

    explicit Rig (int f) : frames (f), mL ((size_t) f), mR ((size_t) f)
    {
        for (int b = 0; b < kBuses; ++b)
        {
            bL[(size_t) b].assign ((size_t) f, kSentinel); bR[(size_t) b].assign ((size_t) f, kSentinel);
            bLp[(size_t) b] = bL[(size_t) b].data(); bRp[(size_t) b] = bR[(size_t) b].data();
        }
    }
    MixerCore::Output out (bool withBuses = true)
    {
        MixerCore::Output o; o.masterL = mL.data(); o.masterR = mR.data();
        if (withBuses) { o.busL = bLp.data(); o.busR = bRp.data(); }
        return o;
    }
    void run (MixerCore& m, MixerCore::Input net, MixerCore::Input app, MixerCore::Input dev, bool withBuses = true)
    {
        m.process (net, app, dev, frames, out (withBuses), &routing);
    }
    bool busUntouched (int b) const { return bL[(size_t) b][0] == kSentinel && bR[(size_t) b][frames - 1] == kSentinel; }
};

TEST (defaults_layout)
{
    MixerSettings s;
    CHECK (kStrips == 32 && kBuses == 32);
    for (int i = 0; i < 4; ++i) CHECK (s.strips[(size_t) i].src == (Source { SourceType::net, i }));
    for (int i = 0; i < 4; ++i) CHECK (s.strips[(size_t) (4 + i)].src == (Source { SourceType::app, i }));
    for (int i = 8; i < 32; ++i) CHECK (s.strips[(size_t) i].src == (Source { SourceType::input, i - 8 }));
    for (const auto& st : s.strips) CHECK (st.out == 0 && st.gainDb == 0.0f && st.balance == 0.0f && ! st.mute && ! st.solo);
    for (int b = 0; b < kBuses; ++b) CHECK (s.buses[(size_t) b].type == OutType::audio && s.buses[(size_t) b].outFirst == 2 * b + 1);
    CHECK (s.masterType == OutType::audio && s.masterOut == 1);
    MixerSettings c = s; c.sanitize();
    CHECK (c == s);                                              // defaults are already valid
}

TEST (stereo_passthrough_unity)
{
    auto s = blank(); s.strips[0].src = { SourceType::net, 0 };
    MixerCore m; m.setSettings (s);
    Rig r (64);
    const auto net = bank (2, 64, { 0.5f, -0.25f });
    r.run (m, { net.data(), 2 }, {}, {});
    for (int n = 0; n < 64; ++n) { NEAR (r.mL[(size_t) n], 0.5, 1e-6); NEAR (r.mR[(size_t) n], -0.25, 1e-6); }   // no -3 dB, no mixing of sides
}

TEST (source_pair_mapping)
{
    // Distinct value per channel so any wrong pair is visible. Strip i -> pair i of each bank, each on its own bus.
    auto s = blank();
    s.strips[0].src = { SourceType::net, 0 };   s.strips[0].out = 1;
    s.strips[1].src = { SourceType::net, 3 };   s.strips[1].out = 2;
    s.strips[2].src = { SourceType::app, 0 };   s.strips[2].out = 3;
    s.strips[3].src = { SourceType::app, 3 };   s.strips[3].out = 4;
    s.strips[4].src = { SourceType::input, 0 }; s.strips[4].out = 5;
    s.strips[5].src = { SourceType::input, 31 };s.strips[5].out = 6;
    MixerCore m; m.setSettings (s);
    Rig r (16);
    std::vector<float> nv, av, dv;
    for (int c = 0; c < 8; ++c) nv.push_back (0.01f * (float) (c + 1));          // 0.01 .. 0.08
    for (int c = 0; c < 8; ++c) av.push_back (0.1f + 0.01f * (float) c);          // 0.10 .. 0.17
    for (int c = 0; c < 64; ++c) dv.push_back (0.002f * (float) (c + 1));         // 0.002 .. 0.128
    const auto net = bank (8, 16, nv), app = bank (8, 16, av), dev = bank (64, 16, dv);
    r.run (m, { net.data(), 8 }, { app.data(), 8 }, { dev.data(), 64 });
    NEAR (r.bL[0][3], 0.01, 1e-6); NEAR (r.bR[0][3], 0.02, 1e-6);
    NEAR (r.bL[1][3], 0.07, 1e-6); NEAR (r.bR[1][3], 0.08, 1e-6);
    NEAR (r.bL[2][3], 0.10, 1e-6); NEAR (r.bR[2][3], 0.11, 1e-6);
    NEAR (r.bL[3][3], 0.16, 1e-6); NEAR (r.bR[3][3], 0.17, 1e-6);
    NEAR (r.bL[4][3], 0.002, 1e-6); NEAR (r.bR[4][3], 0.004, 1e-6);
    NEAR (r.bL[5][3], 0.126, 1e-6); NEAR (r.bR[5][3], 0.128, 1e-6);
    for (int b = 0; b < 6; ++b) CHECK (r.routing.busActive[(size_t) b]);
    for (int b = 6; b < kBuses; ++b) { CHECK (! r.routing.busActive[(size_t) b]); CHECK (r.busUntouched (b)); }
}

TEST (mono_and_odd_channel_counts)
{
    auto s = blank();
    s.strips[0].src = { SourceType::net, 0 }; s.strips[0].out = 1;
    s.strips[1].src = { SourceType::net, 1 }; s.strips[1].out = 2;
    s.strips[2].src = { SourceType::input, 0 }; s.strips[2].out = 3;
    MixerCore m; m.setSettings (s);
    Rig r (8);
    const auto mono = bank (1, 8, { 0.4f });
    r.run (m, { mono.data(), 1 }, {}, { mono.data(), 1 });
    NEAR (r.bL[0][2], 0.4, 1e-6); NEAR (r.bR[0][2], 0.4, 1e-6);                     // mono plays on both sides
    NEAR (r.bL[1][2], 0.0, 1e-6); NEAR (r.bR[1][2], 0.0, 1e-6);                     // there is no pair 2
    NEAR (r.bL[2][2], 0.4, 1e-6); NEAR (r.bR[2][2], 0.4, 1e-6);                     // mono microphone on both sides

    // Three channels: pair 1 is channel 3 alone -> on both sides.
    m.reset();
    const auto three = bank (3, 8, { 0.1f, 0.2f, 0.3f });
    r.run (m, { three.data(), 3 }, {}, {});
    NEAR (r.bL[0][2], 0.1, 1e-6); NEAR (r.bR[0][2], 0.2, 1e-6);
    NEAR (r.bL[1][2], 0.3, 1e-6); NEAR (r.bR[1][2], 0.3, 1e-6);
}

TEST (missing_bank_is_silent)
{
    auto s = blank(); s.strips[0].src = { SourceType::app, 1 }; s.strips[1].src = { SourceType::off, 0 };
    MixerCore m; m.setSettings (s);
    Rig r (16);
    r.run (m, {}, {}, {});
    for (int n = 0; n < 16; ++n) { CHECK (r.mL[(size_t) n] == 0.0f && r.mR[(size_t) n] == 0.0f); }
    const auto app = bank (2, 16, { 0.5f, 0.5f });                                  // app bank too short for slot 1 (pair 1)
    r.run (m, {}, { app.data(), 2 }, {});
    CHECK (r.mL[0] == 0.0f);
}

TEST (balance_law)
{
    auto s = blank(); s.strips[0].src = { SourceType::net, 0 };
    MixerCore m;
    Rig r (16);
    const auto net = bank (2, 16, { 1.0f, 1.0f });
    const float cases[] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f };
    for (float b : cases)
    {
        s.strips[0].balance = b; m.setSettings (s); m.reset();
        r.run (m, { net.data(), 2 }, {}, {});
        const double el = b > 0 ? std::cos (b * M_PI / 2) : 1.0, er = b < 0 ? std::cos (-b * M_PI / 2) : 1.0;
        NEAR (r.mL[8], el, 1e-5); NEAR (r.mR[8], er, 1e-5);
    }
    s.strips[0].balance = 0.5f; m.setSettings (s); m.reset();
    r.run (m, { net.data(), 2 }, {}, {});
    NEAR (20.0 * std::log10 (r.mL[8]), -3.01, 0.02);                               // -3 dB at the quarter point
}

TEST (gain_mute_solo)
{
    auto s = blank();
    for (int i = 0; i < 3; ++i) { s.strips[(size_t) i].src = { SourceType::net, i }; s.strips[(size_t) i].out = i + 1; }
    const auto net = bank (6, 16, { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f });
    Rig r (16);
    MixerCore m;

    s.strips[0].gainDb = -6.0206f; m.setSettings (s); m.reset();
    r.run (m, { net.data(), 6 }, {}, {});
    NEAR (r.bL[0][4], 0.25, 1e-4); NEAR (r.bL[1][4], 0.5, 1e-6);

    s.strips[1].gainDb = kMinGainDb; m.setSettings (s); m.reset();                    // fader at the floor = true silence
    r.run (m, { net.data(), 6 }, {}, {});
    CHECK (r.bL[1][4] == 0.0f);

    s = blank(); for (int i = 0; i < 3; ++i) { s.strips[(size_t) i].src = { SourceType::net, i }; s.strips[(size_t) i].out = i + 1; }
    s.strips[0].mute = true; m.setSettings (s); m.reset();
    r.run (m, { net.data(), 6 }, {}, {});
    CHECK (r.bL[0][4] == 0.0f); NEAR (r.bL[1][4], 0.5, 1e-6);

    s.strips[1].solo = true; m.setSettings (s); m.reset();                            // solo silences the others
    r.run (m, { net.data(), 6 }, {}, {});
    CHECK (r.bL[0][4] == 0.0f); NEAR (r.bL[1][4], 0.5, 1e-6); CHECK (r.bL[2][4] == 0.0f);

    s.strips[0].solo = true; m.setSettings (s); m.reset();                            // mute beats solo
    r.run (m, { net.data(), 6 }, {}, {});
    CHECK (r.bL[0][4] == 0.0f); NEAR (r.bL[1][4], 0.5, 1e-6);
}

TEST (solo_spans_banks)
{
    auto s = blank();
    s.strips[0].src = { SourceType::net, 0 };   s.strips[0].out = 1;
    s.strips[1].src = { SourceType::app, 0 };   s.strips[1].out = 2;
    s.strips[2].src = { SourceType::input, 0 }; s.strips[2].out = 3; s.strips[2].solo = true;
    MixerCore m; m.setSettings (s);
    Rig r (8);
    const auto d = bank (2, 8, { 0.3f, 0.3f });
    r.run (m, { d.data(), 2 }, { d.data(), 2 }, { d.data(), 2 });
    CHECK (r.bL[0][3] == 0.0f && r.bL[1][3] == 0.0f); NEAR (r.bL[2][3], 0.3, 1e-6);
}

TEST (routing_to_master_and_buses)
{
    auto s = blank();
    s.strips[0].src = { SourceType::net, 0 };                      // -> master
    s.strips[1].src = { SourceType::net, 1 }; s.strips[1].out = 5; // -> bus 5
    s.strips[2].src = { SourceType::net, 0 }; s.strips[2].out = 5; // also bus 5: sums
    s.buses[4].type = OutType::net; s.buses[4].outFirst = 9;
    s.masterType = OutType::net; s.masterOut = 3;
    MixerCore m; m.setSettings (s);
    Rig r (8);
    const auto net = bank (4, 8, { 0.1f, 0.1f, 0.2f, 0.2f });
    r.run (m, { net.data(), 4 }, {}, {});
    NEAR (r.mL[2], 0.1, 1e-6);
    NEAR (r.bL[4][2], 0.3, 1e-6); NEAR (r.bR[4][2], 0.3, 1e-6);
    for (int b = 0; b < kBuses; ++b) CHECK (r.routing.busActive[(size_t) b] == (b == 4));
    CHECK (r.routing.busType[4] == OutType::net && r.routing.busFirst[4] == 9);
    CHECK (r.routing.masterType == OutType::net && r.routing.masterFirst == 3);
    CHECK (r.busUntouched (0) && r.busUntouched (31));
}

TEST (no_bus_buffers_falls_back_to_master)
{
    auto s = blank(); s.strips[0].src = { SourceType::net, 0 }; s.strips[0].out = 7;
    MixerCore m; m.setSettings (s);
    Rig r (8);
    const auto net = bank (2, 8, { 0.4f, 0.4f });
    r.run (m, { net.data(), 2 }, {}, {}, false);
    NEAR (r.mL[1], 0.4, 1e-6);
    for (int b = 0; b < kBuses; ++b) CHECK (! r.routing.busActive[(size_t) b]);
}

TEST (bus_and_master_gain_mute)
{
    auto s = blank();
    s.strips[0].src = { SourceType::net, 0 }; s.strips[0].out = 2;
    s.strips[1].src = { SourceType::net, 0 };
    s.buses[1].gainDb = -6.0206f;
    s.masterDb = -6.0206f;
    MixerCore m; m.setSettings (s);
    Rig r (8);
    const auto net = bank (2, 8, { 0.8f, 0.8f });
    r.run (m, { net.data(), 2 }, {}, {});
    NEAR (r.bL[1][3], 0.4, 1e-4);                                  // bus fader
    NEAR (r.mL[3], 0.4, 1e-4);                                     // master fader (bus does not pass through the master)

    s.buses[1].mute = true; s.masterMute = true; m.setSettings (s); m.reset();
    r.run (m, { net.data(), 2 }, {}, {});
    CHECK (r.bL[1][3] == 0.0f && r.mL[3] == 0.0f);
}

TEST (clamp_and_clip_flag)
{
    auto s = blank();
    s.strips[0].src = { SourceType::net, 0 }; s.strips[0].out = 1; s.strips[0].gainDb = 12.0f;
    MixerCore m; m.setSettings (s);
    Rig r (8);
    const auto net = bank (2, 8, { 1.0f, -1.0f });
    r.run (m, { net.data(), 2 }, {}, {});
    for (int n = 0; n < 8; ++n) { CHECK (r.bL[0][(size_t) n] == 1.0f); CHECK (r.bR[0][(size_t) n] == -1.0f); }
    auto p = m.takePeaks();
    CHECK (p.clipped);
    CHECK (! m.takePeaks().clipped);                               // the flag is consumed

    s = blank(); s.strips[0].src = { SourceType::net, 0 }; s.strips[0].gainDb = 12.0f; m.setSettings (s); m.reset();
    r.run (m, { net.data(), 2 }, {}, {});
    CHECK (r.mL[0] == 1.0f && r.mR[0] == -1.0f);
    CHECK (m.takePeaks().clipped);
}

TEST (ramps_and_snap)
{
    auto s = blank(); s.strips[0].src = { SourceType::net, 0 };
    MixerCore m; m.setSettings (s);
    Rig r (100);
    const auto net = bank (2, 100, { 1.0f, 1.0f });
    r.run (m, { net.data(), 2 }, {}, {});                          // first block snaps to unity
    NEAR (r.mL[0], 1.0, 1e-6);
    m.setStripMute (0, true);
    r.run (m, { net.data(), 2 }, {}, {});                          // then fades out linearly over the block
    NEAR (r.mL[0], 0.99, 1e-4); NEAR (r.mL[49], 0.50, 1e-4); NEAR (r.mL[99], 0.0, 1e-6);
    r.run (m, { net.data(), 2 }, {}, {});
    CHECK (r.mL[0] == 0.0f);
    m.setStripMute (0, false);
    r.run (m, { net.data(), 2 }, {}, {});
    NEAR (r.mL[49], 0.5, 1e-4); NEAR (r.mL[99], 1.0, 1e-6);
    m.reset();
    m.setStripMute (0, true);
    r.run (m, { net.data(), 2 }, {}, {});
    CHECK (r.mL[0] == 0.0f);                                       // after reset(), straight to the target
}

TEST (route_change_crossfades)
{
    auto s = blank(); s.strips[0].src = { SourceType::net, 0 };     // master
    MixerCore m; m.setSettings (s);
    Rig r (100);
    const auto net = bank (2, 100, { 1.0f, 1.0f });
    r.run (m, { net.data(), 2 }, {}, {});
    m.setStripOut (0, 3);                                          // master -> bus 3
    r.run (m, { net.data(), 2 }, {}, {});
    for (int n : { 0, 25, 50, 75, 99 }) NEAR (r.mL[(size_t) n] + r.bL[2][(size_t) n], 1.0, 1e-4);   // one block crossfade, no dip, no click
    NEAR (r.bL[2][99], 1.0, 1e-6); NEAR (r.mL[99], 0.0, 1e-6);
    r.run (m, { net.data(), 2 }, {}, {});
    CHECK (r.mL[0] == 0.0f); NEAR (r.bL[2][50], 1.0, 1e-6);

    m.setStripOut (0, 4);                                          // bus 3 -> bus 4: bus 3 stays active for the fade
    r.run (m, { net.data(), 2 }, {}, {});
    CHECK (r.routing.busActive[2] && r.routing.busActive[3]);
    for (int n : { 0, 40, 99 }) NEAR (r.bL[2][(size_t) n] + r.bL[3][(size_t) n], 1.0, 1e-4);
    r.run (m, { net.data(), 2 }, {}, {});
    CHECK (! r.routing.busActive[2] && r.routing.busActive[3]);
}

TEST (hostile_input_samples)
{
    auto s = blank(); s.strips[0].src = { SourceType::net, 0 };
    MixerCore m; m.setSettings (s);
    Rig r (8);
    auto net = bank (2, 8, { 0.5f, 0.5f });
    net[2] = std::numeric_limits<float>::quiet_NaN();
    net[3] = std::numeric_limits<float>::infinity();
    net[4] = -std::numeric_limits<float>::infinity();
    r.run (m, { net.data(), 2 }, {}, {});
    for (int n = 0; n < 8; ++n) { CHECK (std::isfinite (r.mL[(size_t) n]) && std::isfinite (r.mR[(size_t) n])); }
    CHECK (r.mL[1] == 0.0f && r.mR[1] == 0.0f && r.mL[2] == 0.0f);
}

TEST (meters)
{
    auto s = blank();
    s.strips[0].src = { SourceType::net, 0 }; s.strips[0].gainDb = -6.0206f; s.strips[0].mute = true; s.strips[0].balance = 1.0f;
    s.strips[1].src = { SourceType::net, 0 }; s.strips[1].out = 2; s.buses[1].gainDb = -6.0206f;
    MixerCore m; m.setSettings (s);
    Rig r (16);
    const auto net = bank (2, 16, { 0.8f, 0.4f });
    r.run (m, { net.data(), 2 }, {}, {});
    const auto p = m.takePeaks();
    NEAR (p.stripL[0], 0.4, 1e-4); NEAR (p.stripR[0], 0.2, 1e-4);   // post-fader, pre-mute, pre-balance
    NEAR (p.stripL[1], 0.8, 1e-6);
    NEAR (p.busL[1], 0.4, 1e-4); NEAR (p.busR[1], 0.2, 1e-4);        // post-bus fader
    NEAR (p.masterL, 0.0, 1e-6);                                     // strip 0 is muted, strip 1 is on a bus
    const auto q = m.takePeaks();
    CHECK (q.stripL[0] == 0.0f && q.busL[1] == 0.0f);                // reading resets
}

TEST (sanitize_hostile_settings)
{
    MixerSettings s;
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    s.strips[0].gainDb = nan; s.strips[0].balance = inf; s.strips[0].out = 999; s.strips[0].src = { (SourceType) 77, -5 };
    s.strips[1].gainDb = 1e30f; s.strips[1].balance = -1e30f; s.strips[1].out = -4; s.strips[1].src = { SourceType::input, 9999 };
    s.strips[2].src = { SourceType::net, 9 }; s.strips[2].name = std::string ("abc\x01\x02xyz") + std::string (100, 'q');
    s.buses[0].type = (OutType) 9; s.buses[0].outFirst = 0; s.buses[0].gainDb = -inf;
    s.buses[1].outFirst = 5000; s.buses[1].dest = { "bad host/with spaces", 70000 };
    s.buses[2].dest = { std::string (500, 'a'), -1 };
    s.buses[3].dest = { "192.168.1.20", 9100 };
    s.masterOut = -1; s.masterType = (OutType) -3; s.masterDb = nan; s.masterDest = { "host_name-1.local", 0 };
    s.apps[0].bundleId = "bad id!"; s.apps[0].name = std::string (500, 'n');
    s.sanitize();

    CHECK (s.strips[0].gainDb == 0.0f && s.strips[0].balance == 0.0f && s.strips[0].out == kBuses && s.strips[0].src == (Source { SourceType::off, 0 }));
    CHECK (s.strips[1].gainDb == kMaxGainDb && s.strips[1].balance == -1.0f && s.strips[1].out == 0);
    CHECK (s.strips[1].src == (Source { SourceType::input, kDevicePairs - 1 }));
    CHECK (s.strips[2].src == (Source { SourceType::net, kNetPairs - 1 }));
    CHECK (s.strips[2].name.size() == kMaxStripNameLength && s.strips[2].name.substr (0, 3) == "abc" && s.strips[2].name.find ('\x01') == std::string::npos);
    CHECK (s.buses[0].type == OutType::audio && s.buses[0].outFirst == 1 && s.buses[0].gainDb == 0.0f);
    CHECK (s.buses[1].outFirst == kMaxDeviceOutputs - 1 && s.buses[1].dest.host.empty() && s.buses[1].dest.port == kDefaultNetOutPort);
    CHECK (s.buses[2].dest.host.empty() && s.buses[2].dest.port == kDefaultNetOutPort);
    CHECK (s.buses[3].dest.host == "192.168.1.20" && s.buses[3].dest.port == 9100);
    CHECK (s.masterOut == 1 && s.masterType == OutType::audio && s.masterDb == 0.0f);
    CHECK (s.masterDest.host == "host_name-1.local" && s.masterDest.port == kDefaultNetOutPort);
    CHECK (s.apps[0].bundleId.empty() && s.apps[0].name.empty());

    CHECK (isValidHost ("") && isValidHost ("10.0.0.5") && ! isValidHost ("a b") && ! isValidHost ("http://x") && ! isValidHost ("[::1]"));
}

TEST (core_applies_sanitized_settings)
{
    MixerSettings s = blank();
    s.strips[0].src = { SourceType::net, 99 }; s.strips[0].out = 500; s.strips[0].balance = 50.0f;
    MixerCore m; m.setSettings (s);
    Rig r (8);
    const auto net = bank (8, 8, { 0.1f, 0.1f, 0.2f, 0.2f, 0.3f, 0.3f, 0.4f, 0.4f });
    r.run (m, { net.data(), 8 }, {}, {});
    NEAR (r.bL[31][2], 0.0, 1e-6);                                  // balance clamped to +1: left silenced
    NEAR (r.bR[31][2], 0.4, 1e-6);                                  // index clamped to pair 3, out clamped to bus 32
    m.setStripSource (-1, { SourceType::net, 0 }); m.setStripGainDb (999, 0.0f); m.setBusGainDb (-3, 0.0f); m.setBusType (77, OutType::net);   // out-of-range ids are ignored
}

TEST (settings_from_another_thread)
{
    auto s = blank(); for (int i = 0; i < 8; ++i) s.strips[(size_t) i].src = { SourceType::net, i % 4 };
    MixerCore m; m.setSettings (s);
    std::atomic<bool> stop { false };
    std::thread ui ([&]
    {
        int k = 0;
        while (! stop.load())
        {
            ++k;
            m.setStripGainDb (k % 8, (float) (k % 40) - 30.0f); m.setStripBalance (k % 8, ((k % 21) - 10) / 10.0f);
            m.setStripMute (k % 8, k % 3 == 0); m.setStripSolo (k % 8, k % 5 == 0);
            m.setStripOut (k % 8, k % 6); m.setStripSource (k % 8, { (SourceType) (k % 4), k % 40 });
            m.setBusGainDb (k % 32, -(float) (k % 20)); m.setBusMute (k % 32, k % 4 == 0); m.setBusType (k % 32, (OutType) (k % 2)); m.setBusOutFirst (k % 32, k % 70);
            m.setMasterDb ((float) (k % 12)); m.setMasterMute (k % 7 == 0); m.setMasterType ((OutType) (k % 2)); m.setMasterOut (k % 70);
            m.takePeaks();
        }
    });
    Rig r (64);
    const auto net = bank (8, 64, { 0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f });
    const auto dev = bank (64, 64, { 0.2f, 0.2f });
    for (int i = 0; i < 4000; ++i)
    {
        r.run (m, { net.data(), 8 }, { net.data(), 8 }, { dev.data(), 64 });
        for (int n = 0; n < 64; ++n) { if (! (std::abs (r.mL[(size_t) n]) <= 1.0f && std::abs (r.mR[(size_t) n]) <= 1.0f)) { CHECK (false); break; } }
    }
    stop = true; ui.join();
    CHECK (true);
}

// ---- device router -------------------------------------------------------------------------------------------------
struct Dev
{
    std::vector<std::vector<float>> ch; std::vector<float*> p;
    Dev (int n, int frames) : ch ((size_t) n, std::vector<float> ((size_t) frames, 0.0f)) { for (auto& c : ch) p.push_back (c.data()); }
};

TEST (router_places_pairs)
{
    Dev d (6, 8);
    std::vector<float> l (8, 0.1f), r (8, 0.2f), l2 (8, 0.3f), r2 (8, 0.4f);
    StereoOut outs[2] = { { l.data(), r.data(), 1 }, { l2.data(), r2.data(), 5 } };
    CHECK (! routeToDevice (outs, 2, 8, d.p.data(), 6, 0));
    NEAR (d.ch[0][3], 0.1, 1e-6); NEAR (d.ch[1][3], 0.2, 1e-6); NEAR (d.ch[4][3], 0.3, 1e-6); NEAR (d.ch[5][3], 0.4, 1e-6);
    CHECK (d.ch[2][3] == 0.0f && d.ch[3][3] == 0.0f);
}

TEST (router_sums_clamps_and_skips)
{
    Dev d (4, 8);
    std::vector<float> a (8, 0.7f), b (8, 0.6f);
    StereoOut outs[3] = { { a.data(), a.data(), 1 }, { b.data(), b.data(), 1 }, { a.data(), a.data(), 9 } };   // pair 9-10 is off the device
    CHECK (routeToDevice (outs, 3, 8, d.p.data(), 4, 0));            // 1.3 clamps
    CHECK (d.ch[0][0] == 1.0f && d.ch[1][7] == 1.0f && d.ch[2][0] == 0.0f);

    Dev e (4, 8);
    StereoOut nul[2] = { { nullptr, a.data(), 1 }, { a.data(), nullptr, 3 } };
    CHECK (! routeToDevice (nul, 2, 8, e.p.data(), 4, 0));
    CHECK (e.ch[0][0] == 0.0f && e.ch[2][0] == 0.0f);
    CHECK (! routeToDevice (nullptr, 0, 8, e.p.data(), 4, 0) && ! routeToDevice (nul, 2, 0, e.p.data(), 4, 0) && ! routeToDevice (nul, 2, 8, nullptr, 4, 0));
}

TEST (router_mono_fold_offset_and_nan)
{
    Dev d (1, 16);
    std::vector<float> l (8, 0.4f), r (8, 0.2f);
    StereoOut o { l.data(), r.data(), 1 };
    routeToDevice (&o, 1, 8, d.p.data(), 1, 8);                     // R is off the device: L gets the average, written at offset 8
    NEAR (d.ch[0][8], 0.3, 1e-6); CHECK (d.ch[0][0] == 0.0f);

    Dev e (2, 8);
    std::vector<float> bad (8, std::numeric_limits<float>::quiet_NaN());
    StereoOut n { bad.data(), bad.data(), 1 };
    CHECK (routeToDevice (&n, 1, 8, e.p.data(), 2, 0));
    CHECK (e.ch[0][0] == 0.0f && e.ch[1][0] == 0.0f);               // NaN is scrubbed
}

int main()
{
    for (const auto& t : tests())
    {
        const int before = failures;
        t.f();
        std::printf ("%-36s %s\n", t.n, failures == before ? "ok" : "FAILED");
    }
    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
