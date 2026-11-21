// Platform-independent tests: packet codec, jitter buffer, drift controller, SPSC queue, route loop guard.
// Build:  cmake -S . -B build-tests -DPATCHBAY_BUILD_TESTS=ON && cmake --build build-tests && ctest --test-dir build-tests
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <thread>
#include <vector>

#include "Midi/NetMidiTypes.h"
#include "Midi/RouteGraph.h"
#include "Net/DriftController.h"
#include "Net/JitterBuffer.h"
#include "Net/NetworkPlayout.h"
#include "Net/OutputMapper.h"
#include "Net/Packet.h"
#include "Net/SpscQueue.h"

using namespace pb;
using namespace pb::net;

static int failures = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (! (cond)) { ++failures; std::printf ("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define TEST(name) static void name(); struct name##_reg { name##_reg() { tests().push_back ({ #name, name }); } } name##_inst; static void name()
struct T { const char* n; void (*f)(); };
static std::vector<T>& tests() { static std::vector<T> t; return t; }

// A packet of `frames` stereo frames whose every sample encodes (seq, index) so we can tell what played.
static std::vector<float> makePacket (int seq, int frames, int ch = 2)
{
    std::vector<float> v ((size_t) frames * ch);
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < ch; ++c)
            v[(size_t) i * ch + c] = (float) (seq % 1000) + (float) i * 0.001f + (float) c * 0.0001f;
    return v;
}

// ------------------------------------------------------------------------------------ packet codec
TEST (packet_roundtrip_float32_and_int16)
{
    std::vector<float> in (2 * 64);
    for (size_t i = 0; i < in.size(); ++i) in[i] = std::sin ((float) i * 0.1f) * 0.9f;

    for (auto fmt : { SampleFormat::float32, SampleFormat::int16 })
    {
        uint8_t buf[kMaxPacketBytes];
        const auto n = encodeAudio (777, 123456u, in.data(), 64, 2, fmt, buf, sizeof (buf));
        CHECK (n == kHeaderSize + 64 * 2 * bytesPerSample (fmt));

        const auto h = parseHeader (buf, n);
        CHECK (h.has_value());
        if (! h) continue;
        CHECK (h->seq == 777 && h->timestamp == 123456u && h->channels == 2 && h->format == fmt);

        std::vector<float> out (2 * 64);
        int frames = 0;
        CHECK (decodeAudio (*h, buf, out.data(), 64, frames) && frames == 64);
        const float tol = fmt == SampleFormat::float32 ? 0.0f : 1.0f / 16384.0f;
        float worst = 0;
        for (size_t i = 0; i < in.size(); ++i) worst = std::max (worst, std::abs (in[i] - out[i]));
        CHECK (worst <= tol);
    }
}

TEST (packet_header_is_big_endian_on_the_wire)
{
    float x[2] = { 0, 0 };
    uint8_t buf[64];
    encodeAudio (0x0102, 0x0A0B0C0D, x, 1, 2, SampleFormat::float32, buf, sizeof (buf));
    CHECK (buf[0] == 'P' && buf[1] == 'B' && buf[2] == 'N' && buf[3] == 'T');
    CHECK (buf[6] == 0x01 && buf[7] == 0x02);
    CHECK (buf[8] == 0x0A && buf[9] == 0x0B && buf[10] == 0x0C && buf[11] == 0x0D);
}

TEST (packet_rejects_malformed)
{
    float x[4] = { 0, 0, 0, 0 };
    uint8_t good[128];
    const auto n = encodeAudio (1, 0, x, 2, 2, SampleFormat::float32, good, sizeof (good));
    CHECK (parseHeader (good, n).has_value());

    CHECK (! parseHeader (good, kHeaderSize - 1));                 // too short
    CHECK (! parseHeader (good, n - 1));                           // truncated payload
    CHECK (! parseHeader (nullptr, 100));

    auto mutate = [&] (size_t idx, uint8_t v) { uint8_t b[128]; std::memcpy (b, good, n); b[idx] = v; return parseHeader (b, n); };
    CHECK (! mutate (0, 'X'));         // magic
    CHECK (! mutate (4, 9));           // version
    CHECK (! mutate (5, 0));           // type 0
    CHECK (! mutate (5, 4));           // type 4
    CHECK (! mutate (14, 0));          // zero channels
    CHECK (! mutate (14, 9));          // too many channels
    CHECK (! mutate (15, 2));          // unknown sample format
    CHECK (! mutate (14, 3));          // payload no longer a whole number of frames (16 bytes / 12)

    uint8_t big[kMaxPacketBytes + 64] = {};
    writeHeader ({ PacketType::audio, SampleFormat::float32, 1, 0, 0, (uint16_t) ((kMaxFramesPerPacket + 1) * 4) }, big);
    CHECK (! parseHeader (big, sizeof (big)));                     // more frames than we ever accept
}

TEST (packet_parser_survives_random_garbage)
{
    std::mt19937 rng (42);
    uint8_t buf[300];
    int accepted = 0, rejected = 0;

    for (int iter = 0; iter < 300000; ++iter)
    {
        const size_t len = rng() % sizeof (buf);
        for (size_t i = 0; i < len; ++i) buf[i] = (uint8_t) rng();

        if (iter % 4 != 0 && len >= kHeaderSize)          // mostly start from a plausible header, then corrupt a field
        {
            PacketHeader h;
            h.type = (PacketType) (1 + rng() % 4);                         // includes an invalid type 4
            h.format = (SampleFormat) (rng() % 3);                         // includes an invalid format 2
            h.channels = (uint8_t) (rng() % 11);                           // includes 0 and >max
            h.seq = (uint16_t) rng(); h.timestamp = rng();
            h.payloadBytes = (uint16_t) (rng() % (len - kHeaderSize + 12)); // sometimes claims more than is present
            writeHeader (h, buf);
            if (rng() % 16 == 0) buf[4] = (uint8_t) rng();                 // occasionally a bad version
        }

        if (auto h = parseHeader (buf, len))
        {
            ++accepted;
            CHECK ((size_t) h->payloadBytes + kHeaderSize <= len);          // never claims bytes that aren't there
            if (h->type == PacketType::audio)
            {
                float out[kMaxFramesPerPacket * kMaxChannels];
                int frames = 0;
                CHECK (decodeAudio (*h, buf, out, kMaxFramesPerPacket, frames));   // accepted => decodable, in bounds (ASan checks)
                CHECK (frames >= 1);
            }
        }
        else ++rejected;
    }
    std::printf ("  (%d accepted, %d rejected)\n", accepted, rejected);
    CHECK (accepted > 1000 && rejected > 1000);       // the fuzzer must really exercise both paths
}

// ------------------------------------------------------------------------------------- jitter buffer
static JitterBuffer makeJb (int target = 3)
{
    JitterBuffer::Config c; c.targetPackets = target;
    return JitterBuffer (c);
}

TEST (jitter_in_order_is_bit_exact)
{
    auto jb = makeJb();
    const int F = 128;
    for (int s = 0; s < 10; ++s) { auto p = makePacket (s, F); jb.push ((uint16_t) s, p.data(), F); }

    std::vector<float> out (2 * F);
    for (int s = 0; s < 7; ++s)
    {
        CHECK (jb.pull (out.data(), F) == F);
        CHECK (out == makePacket (s, F));
    }
    CHECK (jb.stats().lost == 0 && jb.stats().underruns == 0);
}

TEST (jitter_does_not_play_until_primed)
{
    auto jb = makeJb (3);
    auto p = makePacket (0, 64);
    jb.push (0, p.data(), 64);
    std::vector<float> out (2 * 64, 9.0f);
    CHECK (jb.pull (out.data(), 64) == 0);
    for (float v : out) CHECK (v == 0.0f);       // silence, not stale data
}

TEST (jitter_reorders_and_drops_duplicates)
{
    auto jb = makeJb (3);
    const int F = 64;
    for (int s : { 0, 2, 1, 3, 3, 2 })           // 3 and 2 are duplicated
    {
        auto p = makePacket (s, F);
        jb.push ((uint16_t) s, p.data(), F);
    }
    CHECK (jb.stats().duplicates == 2);

    std::vector<float> out (2 * F);
    for (int s = 0; s < 4; ++s) { jb.pull (out.data(), F); CHECK (out == makePacket (s, F)); }
}

TEST (jitter_conceals_loss_with_silence_of_correct_length_and_stays_aligned)
{
    auto jb = makeJb (3);
    const int F = 100;
    for (int s : { 0, 1, 3, 4, 5 })              // packet 2 never arrives
    {
        auto p = makePacket (s, F);
        jb.push ((uint16_t) s, p.data(), F);
    }
    std::vector<float> out (2 * F);
    jb.pull (out.data(), F); CHECK (out == makePacket (0, F));
    jb.pull (out.data(), F); CHECK (out == makePacket (1, F));
    CHECK (jb.pull (out.data(), F) == 0);                         // concealed
    for (float v : out) CHECK (v == 0.0f);
    jb.pull (out.data(), F); CHECK (out == makePacket (3, F));    // timeline preserved
    CHECK (jb.stats().lost == 1);
}

TEST (jitter_handles_pulls_that_straddle_packets)
{
    auto jb = makeJb (3);
    const int F = 100;
    for (int s = 0; s < 6; ++s) { auto p = makePacket (s, F); jb.push ((uint16_t) s, p.data(), F); }

    std::vector<float> got, expect;
    std::vector<float> out (2 * 37);
    for (int i = 0; i < 10; ++i) { jb.pull (out.data(), 37); got.insert (got.end(), out.begin(), out.end()); }
    for (int s = 0; s < 4; ++s) { auto p = makePacket (s, F); expect.insert (expect.end(), p.begin(), p.end()); }
    CHECK (got.size() == 2 * 370);
    CHECK (std::equal (got.begin(), got.end(), expect.begin()));      // 370 frames, bit-exact across packet boundaries
}

TEST (jitter_late_packets_are_dropped_not_replayed)
{
    auto jb = makeJb (2);
    const int F = 64;
    for (int s = 0; s < 4; ++s) { auto p = makePacket (s, F); jb.push ((uint16_t) s, p.data(), F); }
    std::vector<float> out (2 * F);
    jb.pull (out.data(), F); jb.pull (out.data(), F);    // 0 and 1 played
    auto p = makePacket (0, F);
    jb.push (0, p.data(), F);                            // arrives far too late
    CHECK (jb.stats().late == 1);
    jb.pull (out.data(), F); CHECK (out == makePacket (2, F));
}

TEST (jitter_underrun_outputs_silence_then_reprimes)
{
    auto jb = makeJb (2);
    const int F = 64;
    for (int s = 0; s < 3; ++s) { auto p = makePacket (s, F); jb.push ((uint16_t) s, p.data(), F); }
    std::vector<float> out (2 * F);
    for (int i = 0; i < 3; ++i) jb.pull (out.data(), F);
    CHECK (jb.pull (out.data(), F) == 0);
    CHECK (jb.stats().underruns == 1 && ! jb.isPrimed());

    auto p = makePacket (3, F); jb.push (3, p.data(), F);
    CHECK (jb.pull (out.data(), F) == 0);                // one packet isn't enough to restart (target 2)
    p = makePacket (4, F); jb.push (4, p.data(), F);
    CHECK (jb.pull (out.data(), F) == F && out == makePacket (3, F));
}

TEST (jitter_sequence_wraparound)
{
    auto jb = makeJb (3);
    const int F = 64;
    std::vector<int> seqs = { 65533, 65534, 65535, 0, 1, 2, 3 };
    for (int s : seqs) { auto p = makePacket (s, F); jb.push ((uint16_t) s, p.data(), F); }
    std::vector<float> out (2 * F);
    for (int s : seqs) { jb.pull (out.data(), F); CHECK (out == makePacket (s, F)); }
    CHECK (jb.stats().late == 0 && jb.stats().lost == 0);
}

TEST (jitter_resyncs_when_sender_restarts)
{
    auto jb = makeJb (2);
    const int F = 64;
    for (int s = 40000; s < 40004; ++s) { auto p = makePacket (s, F); jb.push ((uint16_t) s, p.data(), F); }
    std::vector<float> out (2 * F);
    jb.pull (out.data(), F);

    // sender restarts with an unrelated sequence number - must not be dropped as "late" forever
    for (int s = 100; s < 104; ++s) { auto p = makePacket (s, F); jb.push ((uint16_t) s, p.data(), F); }
    CHECK (jb.stats().resyncs >= 1);
    jb.pull (out.data(), F);
    CHECK (out == makePacket (100, F));
}

TEST (jitter_trims_runaway_latency)
{
    JitterBuffer::Config c; c.targetPackets = 2; c.maxLatencyPackets = 8;
    JitterBuffer jb (c);
    const int F = 64;
    for (int s = 0; s < 30; ++s) { auto p = makePacket (s, F); jb.push ((uint16_t) s, p.data(), F); }
    CHECK (jb.bufferedPackets() <= 8);
    CHECK (jb.stats().droppedForLatency > 0);
}

// ----------------------------------------------------------------------------------- drift controller
static void runDrift (double driftPpm)
{
    // Producer delivers 128-frame packets on a clock `driftPpm` faster than the consumer's; the consumer
    // pulls 128 frames per callback scaled by the controller's ratio. Perfectly phase-locked clocks are the
    // worst case for the fill measurement (it only changes when a whole packet slips in or out).
    const double p = 1.0 + driftPpm * 1e-6;
    JitterBuffer::Config c; c.targetPackets = 4; c.maxLatencyPackets = 40;
    JitterBuffer jb (c);
    const int F = 128;
    DriftController drift (4.0 * F, 48000.0);
    drift.reset (4.0 * F);

    double produced = 0, acc = 0, sumRatio = 0, worstFill = 0, lo = 1e9, hi = -1e9;
    int seq = 0, counted = 0;
    std::vector<float> in = makePacket (0, F), out ((size_t) F * 2 * 2);
    const int callbacks = 19200000 / F;            // 400 s of audio at 48 kHz

    for (int cb = 0; cb < callbacks; ++cb)
    {
        produced += F * p;
        while (produced >= F) { produced -= F; jb.push ((uint16_t) seq++, in.data(), F); }

        drift.update (jb.bufferedFrames(), F);
        acc += F * drift.ratio();
        const int want = (int) acc;
        acc -= want;
        jb.pull (out.data(), std::min (want, F * 2));

        if (cb > callbacks * 6 / 10)                // measure only after it has settled
        {
            worstFill = std::max (worstFill, std::abs (drift.smoothedFill() - 4.0 * F) / F);
            sumRatio += drift.ratio(); ++counted;
            lo = std::min (lo, drift.ratio()); hi = std::max (hi, drift.ratio());
        }
    }
    const double meanErrPpm = (sumRatio / counted - p) * 1e6, p2pPpm = (hi - lo) * 1e6;
    std::printf ("  drift %+5.0f ppm: mean ratio error %+.1f ppm, wobble %.0f ppm p-p, worst fill error %.2f packets, underruns %llu, trimmed %llu\n",
                 driftPpm, meanErrPpm, p2pPpm, worstFill,
                 (unsigned long long) jb.stats().underruns, (unsigned long long) jb.stats().droppedForLatency);
    CHECK (jb.stats().underruns == 0);
    CHECK (jb.stats().droppedForLatency == 0);
    CHECK (worstFill < 1.0);                        // never more than a packet from target
    CHECK (std::abs (meanErrPpm) < 5.0);            // the controller found the true clock offset on average
    CHECK (p2pPpm < 400.0);                         // and the instantaneous rate stays well under half a cent
}

TEST (drift_controller_tracks_clock_offsets)
{
    for (double ppm : { 100.0, -100.0, 300.0, -450.0, 0.0 })
        runDrift (ppm);
}

// ----------------------------------------------------------------------------------- network playout
// Producer/consumer simulation helper. The producer's clock runs `p` times faster than the consumer's:
// each consumer callback of F frames, the producer has generated F*p frames and sends whole packets.
struct PlayoutSim
{
    NetworkPlayout pl;
    int F, ch;
    double p, produced = 0;
    long long seqCount = 0;
    std::vector<float> outAll;
    std::function<float (long long frameIndex, int channel)> signal;
    std::function<bool (int seq)> shouldDrop = [] (int) { return false; };

    PlayoutSim (NetworkPlayout::Config c, int framesPerPacket, double producerOverConsumer)
        : pl (c), F (framesPerPacket), ch (c.channels), p (producerOverConsumer) {}

    void step()
    {
        produced += F * p;
        std::vector<float> pkt ((size_t) F * ch);
        while (produced >= F)
        {
            produced -= F;
            for (int i = 0; i < F; ++i)
                for (int c = 0; c < ch; ++c) pkt[(size_t) i * ch + c] = signal (seqCount * F + i, c);
            if (! shouldDrop ((int) seqCount)) pl.push ((uint16_t) seqCount, pkt.data(), F);
            ++seqCount;
        }
        std::vector<float> out ((size_t) F * ch);
        pl.process (out.data(), F);
        outAll.insert (outAll.end(), out.begin(), out.end());
    }
    void run (int callbacks) { for (int i = 0; i < callbacks; ++i) step(); }

    /** Index (in frames) of the first non-silent output frame, or -1. */
    long long firstSound() const
    {
        for (size_t i = 0; i < outAll.size(); ++i)
            if (outAll[i] != 0.0f) return (long long) (i / ch);
        return -1;
    }
};

TEST (playout_at_neutral_ratio_is_bit_exact_for_every_channel)
{
    for (int ch : { 1, 2, 8 })
    {
        NetworkPlayout::Config c; c.channels = ch;
        PlayoutSim sim (c, 128, 1.0);
        sim.signal = [] (long long k, int c2) { return 1.0f + (float) k * 0.001f * (c2 % 2 ? -1.0f : 1.0f) + (float) c2 * 10.0f; };
        sim.run (200);

        const long long k0 = sim.firstSound();
        CHECK (k0 > 0);
        int bad = 0;
        for (long long j = 0; j < 20000; ++j)
            for (int c2 = 0; c2 < ch; ++c2)
                if (sim.outAll[(size_t) ((k0 + j) * ch + c2)] != sim.signal (j, c2)) ++bad;
        CHECK (bad == 0);      // ratio is exactly 1, so interpolation must return input samples untouched
        CHECK (sim.pl.status().jitter.lost == 0 && sim.pl.status().jitter.underruns == 0);
    }
}

static void runGlitchTest (double driftPpm, double seconds)
{
    // Pure 1 kHz sine through the whole chain with the sender's clock off by driftPpm. For any sinusoid of
    // angular frequency w, y[n] - 2cos(w) y[n-1] + y[n-2] = 0. Resampling by 1 +/- 4e-4 shifts w by that
    // fraction, giving a residual of ~1e-5; a dropped/duplicated/discontinuous sample gives ~1e-1.
    const double w = 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0;
    NetworkPlayout::Config c; c.channels = 2; c.sampleRate = 48000.0;
    PlayoutSim sim (c, 128, 1.0 + driftPpm * 1e-6);
    sim.signal = [w] (long long k, int) { return (float) (0.5 * std::sin (w * (double) k)); };
    sim.run ((int) (seconds * 48000.0 / 128.0));

    const long long k0 = sim.firstSound() + 8;
    double worst = 0, peak = 0;
    long long frames = (long long) sim.outAll.size() / 2;
    for (long long n = k0 + 2; n < frames; ++n)
    {
        const double y0 = sim.outAll[(size_t) (n * 2)], y1 = sim.outAll[(size_t) ((n - 1) * 2)], y2 = sim.outAll[(size_t) ((n - 2) * 2)];
        worst = std::max (worst, std::abs (y0 - 2.0 * std::cos (w) * y1 + y2));
        peak = std::max (peak, std::abs (y0));
    }
    const auto st = sim.pl.status();
    std::printf ("  drift %+5.0f ppm over %.0f s: worst residual %.2e (peak %.3f), underruns %llu, lost %llu, trimmed %llu, ratio %+.0f ppm\n",
                 driftPpm, seconds, worst, peak, (unsigned long long) st.jitter.underruns, (unsigned long long) st.jitter.lost,
                 (unsigned long long) st.jitter.droppedForLatency, st.ratioPpm);
    CHECK (worst < 1e-4);                           // glitch-free
    CHECK (peak > 0.45 && peak < 0.55);             // and actually playing the signal at full level
    CHECK (st.jitter.underruns == 0 && st.jitter.lost == 0 && st.jitter.droppedForLatency == 0);
}

TEST (playout_is_glitch_free_while_tracking_clock_drift)
{
    runGlitchTest (0.0, 20);
    runGlitchTest (100.0, 120);
    runGlitchTest (-100.0, 120);
    runGlitchTest (300.0, 120);
    runGlitchTest (-450.0, 120);
}

TEST (playout_keeps_timeline_across_a_lost_packet)
{
    NetworkPlayout::Config c; c.channels = 1;
    PlayoutSim sim (c, 100, 1.0);
    sim.signal = [] (long long k, int) { return 1.0f + (float) k * 0.0001f; };
    sim.shouldDrop = [] (int seq) { return seq == 30; };           // packet 30 = frames 3000..3099
    sim.run (200);

    const long long k0 = sim.firstSound();
    // The resampler holds a tiny fractional-sample offset (1e-5..1e-4 of a sample) accumulated from the controller's
    // reaction to the loss. Away from the gap that is invisible (< 1e-5 = -100 dB, versus 1e-4 per sample for a real
    // one-sample shift). The three samples touching a step edge are interpolated across a ~1.3-high step, so they get a
    // tolerance proportional to the step (offset 1e-4 x 1.3).
    int wrongBefore = 0, wrongAfter = 0, nonZeroInGap = 0, wrongEdges = 0;
    auto get = [&] (long long j) { return sim.outAll[(size_t) (k0 + j)]; };
    auto near = [] (float a, float b, float tol) { return std::abs (a - b) < tol; };
    for (long long j = 0; j < 3000 - 2; ++j)    if (! near (get (j), sim.signal (j, 0), 1e-5f)) ++wrongBefore;
    for (long long j = 3000 + 1; j < 3100 - 2; ++j) if (! near (get (j), 0.0f, 1e-5f)) ++nonZeroInGap;
    for (long long j = 3100 + 2; j < 15000; ++j) if (! near (get (j), sim.signal (j, 0), 1e-5f)) ++wrongAfter;
    for (long long j : { 2998LL, 2999LL, 3000LL, 3098LL, 3099LL, 3100LL, 3101LL })
        if (! near (get (j), (j >= 3000 && j < 3100) ? 0.0f : sim.signal (j, 0), 3e-4f)) ++wrongEdges;
    CHECK (wrongBefore == 0 && nonZeroInGap == 0 && wrongAfter == 0 && wrongEdges == 0);   // silence exactly where the packet was; nothing shifts
    CHECK (sim.pl.status().jitter.lost == 1);
}

TEST (playout_survives_starvation_and_recovers)
{
    NetworkPlayout::Config c; c.channels = 2;
    PlayoutSim sim (c, 128, 1.0);
    sim.signal = [] (long long k, int) { return 0.25f * std::sin ((float) k * 0.05f); };
    sim.run (100);
    const auto before = sim.pl.status().jitter.underruns;

    sim.shouldDrop = [] (int) { return true; };           // network goes away for ~1 s
    sim.run (400);
    CHECK (sim.pl.status().jitter.underruns > before);
    CHECK (! sim.pl.status().primed);

    sim.shouldDrop = [] (int) { return false; };          // and comes back
    sim.run (200);
    CHECK (sim.pl.status().primed);

    bool finite = true; float tailPeak = 0;
    for (float v : sim.outAll) if (! std::isfinite (v)) finite = false;
    for (size_t i = sim.outAll.size() - 128 * 2 * 20; i < sim.outAll.size(); ++i) tailPeak = std::max (tailPeak, std::abs (sim.outAll[i]));
    CHECK (finite);
    CHECK (tailPeak > 0.1f);                              // audio is flowing again
}

TEST (playout_target_change_at_runtime_converges_without_dropouts)
{
    NetworkPlayout::Config c; c.channels = 2; c.targetPackets = 4;
    PlayoutSim sim (c, 128, 1.0 + 50e-6);
    sim.signal = [] (long long k, int) { return 0.3f * std::sin ((float) k * 0.1f); };
    sim.run (20000);                                      // ~53 s settle at 4 packets
    sim.pl.setTargetPackets (9);
    sim.run (60000);                                      // ~160 s to move to 9
    auto st = sim.pl.status();
    std::printf ("  after raising target 4 -> 9: fill %.1f packets (target %.1f), underruns %llu\n",
                 st.fillFrames / 128, st.targetFrames / 128, (unsigned long long) st.jitter.underruns);
    CHECK (std::abs (st.fillFrames - st.targetFrames) < 128.0);
    CHECK (st.jitter.underruns == 0 && st.jitter.droppedForLatency == 0);

    sim.pl.setTargetPackets (3);
    sim.run (60000);
    st = sim.pl.status();
    CHECK (std::abs (st.fillFrames - st.targetFrames) < 128.0);
    CHECK (st.jitter.underruns == 0 && st.jitter.droppedForLatency == 0);
}

// ------------------------------------------------------------------------------------------ output mapping
static std::vector<std::vector<float>> runMapper (int streamCh, int numOut, int frames, int offset, std::vector<float>& stream, float& p0, float& p1, bool nullLast = false)
{
    std::vector<std::vector<float>> bufs ((size_t) numOut, std::vector<float> ((size_t) (frames + offset + 4), -7.0f));   // -7 = "untouched" sentinel
    std::vector<float*> ptrs;
    for (auto& b : bufs) ptrs.push_back (b.data());
    if (nullLast && numOut > 0) ptrs.back() = nullptr;
    stream.assign ((size_t) frames * streamCh, 0.0f);
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < streamCh; ++c) stream[(size_t) i * streamCh + c] = (float) (c + 1) * 0.1f + (float) i * 0.001f;
    p0 = p1 = 0;
    mapToOutputs (stream.data(), streamCh, frames, ptrs.data(), numOut, offset, p0, p1);
    return bufs;
}

TEST (output_mapper_handles_every_channel_combination)
{
    std::vector<float> st; float p0, p1;
    const int N = 16, off = 5;

    // stereo -> stereo: straight through
    auto b = runMapper (2, 2, N, off, st, p0, p1);
    for (int i = 0; i < N; ++i) { CHECK (b[0][(size_t) (off + i)] == st[(size_t) i * 2]); CHECK (b[1][(size_t) (off + i)] == st[(size_t) i * 2 + 1]); }
    CHECK (b[0][0] == -7.0f && b[0][(size_t) (off + N)] == -7.0f);                      // wrote exactly [off, off+N)
    CHECK (std::abs (p0 - (0.1f + 0.015f)) < 1e-6f && std::abs (p1 - (0.2f + 0.015f)) < 1e-6f);

    // mono -> copied to outputs 0 and 1 only
    b = runMapper (1, 4, N, 0, st, p0, p1);
    for (int i = 0; i < N; ++i) { CHECK (b[0][(size_t) i] == st[(size_t) i]); CHECK (b[1][(size_t) i] == st[(size_t) i]); }
    CHECK (b[2][0] == -7.0f && b[3][0] == -7.0f);

    // 2 ch stream -> 8 outputs: extras untouched
    b = runMapper (2, 8, N, 0, st, p0, p1);
    for (int c = 2; c < 8; ++c) CHECK (b[(size_t) c][0] == -7.0f && b[(size_t) c][(size_t) N - 1] == -7.0f);

    // 6 ch stream -> 2 outputs: extra stream channels dropped, first two mapped correctly
    b = runMapper (6, 2, N, 0, st, p0, p1);
    for (int i = 0; i < N; ++i) { CHECK (b[0][(size_t) i] == st[(size_t) i * 6]); CHECK (b[1][(size_t) i] == st[(size_t) i * 6 + 1]); }

    // 8 ch -> 8 outputs: channel c carries stream channel c (no cross-talk)
    b = runMapper (8, 8, N, 0, st, p0, p1);
    for (int c = 0; c < 8; ++c) for (int i = 0; i < N; ++i) CHECK (b[(size_t) c][(size_t) i] == st[(size_t) i * 8 + (size_t) c]);

    // 1 output only, stereo stream: right channel dropped, no write out of bounds (ASan would catch)
    b = runMapper (2, 1, N, 0, st, p0, p1);
    CHECK (b[0][3] == st[6] && p1 == 0.0f);

    // a null output pointer is skipped, others still written
    b = runMapper (2, 2, N, 0, st, p0, p1, true);
    CHECK (b[0][0] == st[0]);
    std::vector<float> st2; (void) runMapper (2, 0, N, 0, st2, p0, p1);                  // zero outputs: no-op
}

// ---------------------------------------------------------------------------------------- RTP-MIDI settings
TEST (midinet_host_address_validation)
{
    for (const char* ok : { "192.168.1.20", "10.0.0.1", "0.0.0.0", "255.255.255.255", "my-mac.local", "host", "a.b.c.d", "Studio-2.example.com",
                            "::1", "fe80::1", "2001:db8::8a2e:370:7334", "1:2:3:4:5:6:7:8" })
        CHECK (isValidHostAddress (ok));

    for (const char* bad : { "", " ", "256.1.1.1", "1.2.3", "1.2.3.4.5", "1.2.3.04", "01.2.3.4", "-bad.local", "bad-.local", "a..b", ".a", "a.",
                             "host name", "host;rm -rf /", "$(id)", "192.168.1.20:5004", "[::1]", ":::1", "::g", "1::2::3", "12345::1", "1:2:3:4:5:6:7:8:9",
                             "host_name", "h\xc3\xa9llo.local", "a\nb", "http://x", "1.2.3.4/24" })
        CHECK (! isValidHostAddress (bad));

    CHECK (! isValidHostAddress (std::string (254, 'a')));
    CHECK (isValidHostAddress (std::string (63, 'a') + ".local"));
    CHECK (! isValidHostAddress (std::string (64, 'a') + ".local"));          // label > 63
}

TEST (midinet_ports_names_and_host_list_normalisation)
{
    CHECK (isValidPort (1) && isValidPort (5004) && isValidPort (65535));
    CHECK (! isValidPort (0) && ! isValidPort (-1) && ! isValidPort (65536) && ! isValidPort (99999));

    CHECK (sanitizeHostName ("  Studio Mac  ") == "Studio Mac");
    CHECK (sanitizeHostName ("a\tb\nc\x01") == "abc");
    CHECK (sanitizeHostName ("   ").empty());
    CHECK (sanitizeHostName (std::string (500, 'x')).size() == kMaxHostNameLength);

    std::vector<MidiNetHost> in = {
        { "Mac", "192.168.1.20", 5004 },
        { "dup of Mac, other name", "192.168.1.20", 5004 },     // same address+port: dropped
        { "Mac port 2", "192.168.1.20", 5006 },                   // same address, other port: kept
        { "", "my-pc.local", 5004 },                              // blank name -> address
        { "bad addr", "no spaces allowed", 5004 },                // dropped
        { "bad port", "10.0.0.5", 70000 },                        // dropped
        { "bad port 2", "10.0.0.6", 0 },                          // dropped
    };
    const auto out = normaliseHosts (in);
    CHECK (out.size() == 3);
    CHECK (out[0].name == "Mac" && out[1].port == 5006 && out[2].name == "my-pc.local");

    std::vector<MidiNetHost> many;
    for (int i = 0; i < 100; ++i) many.push_back ({ "h", "10.0.0." + std::to_string (i), 5004 });
    CHECK ((int) normaliseHosts (many).size() == kMaxMidiNetHosts);

    MidiNetPolicy p = MidiNetPolicy::anyone;
    CHECK (policyFromString ("noOne", p) && p == MidiNetPolicy::noOne);
    CHECK (policyFromString ("contacts", p) && p == MidiNetPolicy::contactsOnly);
    CHECK (policyFromString ("anyone", p) && p == MidiNetPolicy::anyone);
    p = MidiNetPolicy::contactsOnly;
    CHECK (! policyFromString ("ANYONE", p) && ! policyFromString ("", p) && ! policyFromString ("2", p) && p == MidiNetPolicy::contactsOnly);   // unknown: untouched
    CHECK (MidiNetSettings().policy == MidiNetPolicy::contactsOnly);          // the safe default
}

// ------------------------------------------------------------------------------------------ SPSC queue
TEST (spsc_two_threads_preserve_order_and_content)
{
    struct Item { uint32_t id; uint32_t check; };
    static SpscQueue<Item, 64> q;
    constexpr uint32_t total = 2000000;

    std::atomic<bool> bad { false };
    std::thread producer ([&]
    {
        for (uint32_t i = 0; i < total;)
            if (auto* s = q.acquireWrite()) { s->id = i; s->check = i * 2654435761u; q.publish(); ++i; }
    });
    uint32_t expect = 0;
    while (expect < total)
        if (auto* s = q.peek())
        {
            if (s->id != expect || s->check != expect * 2654435761u) bad = true;
            q.release(); ++expect;
        }
    producer.join();
    CHECK (! bad);
}

TEST (spsc_reports_full_and_empty)
{
    SpscQueue<int, 4> q;
    CHECK (q.peek() == nullptr);
    for (int i = 0; i < 4; ++i) { auto* s = q.acquireWrite(); CHECK (s != nullptr); *s = i; q.publish(); }
    CHECK (q.acquireWrite() == nullptr);
    CHECK (*q.peek() == 0); q.release();
    CHECK (q.acquireWrite() != nullptr);
}

// ------------------------------------------------------------------------------------------ route loops
TEST (route_loop_guard)
{
    const int32_t in1 = kBusInBase, out1 = kBusOutBase, in2 = kBusInBase + 1, out2 = kBusOutBase + 1;
    const int32_t hwOut = 1000, hwIn = 2000;

    CHECK (wouldCreateLoop ({}, out1, in1));                                // bus feeding itself
    CHECK (! wouldCreateLoop ({}, out1, in2));
    CHECK (! wouldCreateLoop ({}, hwOut, in1));
    CHECK (! wouldCreateLoop ({}, out1, hwIn));
    CHECK (wouldCreateLoop ({ { out2, in1 } }, out1, in2));                 // out1->in2, out2->in1 closes a ring
    CHECK (! wouldCreateLoop ({ { out2, hwIn } }, out1, in2));              // chain ending at hardware is fine
    CHECK (! wouldCreateLoop ({ { out2, in1 } }, hwOut, in2));              // hw -> bus2 -> bus1 -> (nothing back to hw)
    CHECK (wouldCreateLoop ({ { out2, in1 }, { out1, hwIn } }, out1, in2));  // closes via 2 then 1, even with a side branch

    CHECK (isBusInput (in1) && isBusOutput (out1) && ! isBusUid (hwOut));
    CHECK (busOutputFor (in2) == out2);
}

int main()
{
    for (auto& t : tests())
    {
        const int before = failures;
        std::printf ("[ RUN  ] %s\n", t.n);
        t.f();
        std::printf ("[ %s ] %s\n", failures == before ? " OK " : "FAIL", t.n);
    }
    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
