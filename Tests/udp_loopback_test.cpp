// End-to-end over real UDP sockets on loopback: sender -> UdpReceiver -> SpscQueue -> JitterBuffer.
// Includes garbage datagrams to prove the receiver rejects them without disturbing the stream.
#include <juce_core/juce_core.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>
#include "Net/JitterBuffer.h"
#include "Net/NetworkPlayout.h"
#include "Net/UdpReceiver.h"

using namespace pb::net;

static int failures = 0;
#define CHECK(c) do { if (! (c)) { ++failures; std::printf ("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static std::vector<float> makePacket (int seq, int frames, int ch)
{
    std::vector<float> v ((size_t) frames * ch);
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < ch; ++c)
            v[(size_t) i * ch + c] = (float) (seq % 1000) + (float) i * 0.001f + (float) c * 0.0001f;
    return v;
}

// Full chain over real sockets: sender -> UDP -> receiver thread -> SPSC queue -> NetworkPlayout -> output samples.
static void playoutPhase (SampleFormat fmt)
{
    const int F = 128, ch = 2, total = 500;
    const double w = 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0;
    auto sine = [&] (long long k, int c) { return (float) (0.5 * std::sin (w * (double) k + c * 0.7)); };

    UdpReceiver rx;
    CHECK (rx.start (0));
    NetworkPlayout::Config cfg; cfg.channels = ch;
    NetworkPlayout pl (cfg);
    juce::DatagramSocket tx (false);

    std::vector<float> pkt ((size_t) F * ch), out ((size_t) F * ch), all;
    uint8_t buf[kMaxPacketBytes];

    for (int s = 0; s < total; ++s)
    {
        for (int i = 0; i < F; ++i)
            for (int c = 0; c < ch; ++c) pkt[(size_t) i * ch + c] = sine ((long long) s * F + i, c);
        const auto len = encodeAudio ((uint16_t) s, (uint32_t) (s * F), pkt.data(), F, ch, fmt, buf, sizeof (buf));
        tx.write ("127.0.0.1", rx.getPort(), buf, (int) len);

        for (int i = 0; i < 500 && rx.counters().audioPackets.load() < (uint64_t) (s + 1); ++i)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));

        while (const auto* p = rx.queue().peek()) { pl.push (p->seq, p->samples, p->frames); rx.queue().release(); }
        pl.process (out.data(), F);                             // one simulated audio callback per packet
        all.insert (all.end(), out.begin(), out.end());
    }
    rx.stop();

    size_t first = 0;
    while (first < all.size() && all[first] == 0.0f) ++first;
    const long long k0 = (long long) first / ch;

    // The stream has no drift here, so the controller stays neutral and output == input delayed by k0 frames.
    // float32 must match exactly; int16 within its quantisation step. Then the recurrence test proves no glitches.
    double worstErr = 0, worstResidual = 0;
    const long long frames = (long long) all.size() / ch;
    for (long long j = 0; j + k0 < frames - 2; ++j)
        for (int c = 0; c < ch; ++c)
            worstErr = std::max (worstErr, (double) std::abs (all[(size_t) ((k0 + j) * ch + c)] - sine (j, c)));
    for (long long n = k0 + 8; n + 1 < frames; ++n)
    {
        if (n < 2) continue;
        const double y0 = all[(size_t) (n * ch)], y1 = all[(size_t) ((n - 1) * ch)], y2 = all[(size_t) ((n - 2) * ch)];
        worstResidual = std::max (worstResidual, std::abs (y0 - 2.0 * std::cos (w) * y1 + y2));
    }
    const auto st = pl.status();
    std::printf ("%s over UDP: first sound at frame %lld, max error vs source %.2e, glitch residual %.2e, lost %llu late %llu underruns %llu\n",
                 fmt == SampleFormat::float32 ? "float32" : "int16  ", k0, worstErr, worstResidual,
                 (unsigned long long) st.jitter.lost, (unsigned long long) st.jitter.late, (unsigned long long) st.jitter.underruns);

    CHECK (k0 > 0 && k0 < 4 * F + 64);                        // starts after priming (target 4 packets) and not much later
    CHECK (worstErr < (fmt == SampleFormat::float32 ? 1e-5 : 1.0 / 16384.0));
    CHECK (worstResidual < (fmt == SampleFormat::float32 ? 1e-5 : 2e-4));   // int16 rounding noise is ~3e-5 per sample
    CHECK (st.jitter.lost == 0 && st.jitter.late == 0 && st.jitter.underruns == 0);
}

int main()
{
    const int F = 128, ch = 2, total = 400;

    UdpReceiver rx;
    CHECK (rx.start (0));
    CHECK (rx.getPort() > 0);
    std::printf ("receiver bound to port %d\n", rx.getPort());

    JitterBuffer::Config cfg; cfg.channels = ch; cfg.targetPackets = 3;
    JitterBuffer jb (cfg);

    auto drain = [&]
    {
        int n = 0;
        while (const auto* p = rx.queue().peek()) { jb.push (p->seq, p->samples, p->frames); rx.queue().release(); ++n; }
        return n;
    };

    juce::DatagramSocket tx (false);
    std::mt19937 rng (7);
    uint8_t buf[kMaxPacketBytes];
    int garbageSent = 0, audioSent = 0, nextExpected = 0, exact = 0, pulled = 0;
    std::vector<float> out ((size_t) F * ch);

    auto waitForAudio = [&] (int count)         // let the datagram cross the loopback before we "run the audio callback"
    {
        for (int i = 0; i < 500 && rx.counters().audioPackets.load() < (uint64_t) count; ++i)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
    };

    for (int s = 0; s < total; ++s)
    {
        auto pkt = makePacket (s, F, ch);
        const auto len = encodeAudio ((uint16_t) s, (uint32_t) (s * F), pkt.data(), F, ch, SampleFormat::float32, buf, sizeof (buf));
        CHECK (tx.write ("127.0.0.1", rx.getPort(), buf, (int) len) == (int) len);
        ++audioSent;

        if (s % 5 == 0)                                       // interleave hostile datagrams
        {
            uint8_t junk[200];
            const int jl = 1 + (int) (rng() % 199);
            for (int i = 0; i < jl; ++i) junk[i] = (uint8_t) rng();
            tx.write ("127.0.0.1", rx.getPort(), junk, jl);
            tx.write ("127.0.0.1", rx.getPort(), buf, (int) len - 7);   // truncated but otherwise valid
            garbageSent += 2;
        }

        // One simulated audio callback per packet sent: drain the queue, then pull one packet's worth.
        waitForAudio (audioSent);
        drain();
        if (jb.isPrimed())
        {
            jb.pull (out.data(), F);
            ++pulled;
            if (out == makePacket (nextExpected, F, ch)) ++exact;
            ++nextExpected;
        }
    }

    for (int i = 0; i < 300 && rx.counters().badPackets.load() < (uint64_t) garbageSent; ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));

    const auto& c = rx.counters();
    std::printf ("audio %llu/%d  bad %llu/%d  queueFull %llu  other %llu\n",
                 (unsigned long long) c.audioPackets.load(), audioSent, (unsigned long long) c.badPackets.load(), garbageSent,
                 (unsigned long long) c.queueFullDrops.load(), (unsigned long long) c.otherPackets.load());

    CHECK (c.audioPackets.load() == (uint64_t) audioSent);    // loopback shouldn't drop at this rate
    CHECK (c.queueFullDrops.load() == 0);
    CHECK (c.badPackets.load() + c.otherPackets.load() == (uint64_t) garbageSent);   // every hostile datagram rejected

    std::printf ("bit-exact packets out of the jitter buffer: %d/%d  (lost %llu, late %llu, dup %llu, underruns %llu)\n", exact, pulled,
                 (unsigned long long) jb.stats().lost, (unsigned long long) jb.stats().late,
                 (unsigned long long) jb.stats().duplicates, (unsigned long long) jb.stats().underruns);
    CHECK (pulled >= total - cfg.targetPackets - 1);
    CHECK (exact == pulled);
    CHECK (jb.stats().underruns == 0);

    // stop() must unblock the socket read promptly and the receiver must be restartable on a new socket
    const auto t0 = std::chrono::steady_clock::now();
    rx.stop();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now() - t0).count();
    std::printf ("stop() took %lld ms\n", (long long) ms);
    CHECK (ms < 500);
    CHECK (! rx.isRunning());
    CHECK (rx.start (0));
    CHECK (rx.isRunning());
    rx.stop();

    playoutPhase (SampleFormat::float32);
    playoutPhase (SampleFormat::int16);

    std::printf (failures == 0 ? "\nALL OK\n" : "\n%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
