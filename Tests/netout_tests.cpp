// Net Out: packetizer (pure) and the UDP sender, exercised over real loopback sockets. No JUCE.
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "Net/NetOut.h"
#include "Net/NetworkPlayout.h"
#include "Net/UdpSender.h"

using namespace pb;
using namespace pb::net;
static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (! (c)) { ++failures; std::printf ("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define TEST(name) static void name(); struct Reg_##name { Reg_##name() { tests().push_back ({ #name, &name }); } } reg_##name; static void name()
struct T { const char* n; void (*f)(); };
static std::vector<T>& tests() { static std::vector<T> t; return t; }

static std::vector<float> ramp (int n, float start) { std::vector<float> v ((size_t) n); for (int i = 0; i < n; ++i) v[(size_t) i] = start + 0.0001f * (float) i; return v; }

static std::vector<OutPacket> drain (OutQueue& q)
{
    std::vector<OutPacket> out;
    while (const OutPacket* p = q.peek()) { out.push_back (*p); q.release(); }
    return out;
}

TEST (packetizer_cuts_packets_and_numbers_them)
{
    OutQueue q; NetPacketizer pk (q);
    const auto l = ramp (300, 0.0f), r = ramp (300, 0.5f);
    CHECK (pk.push (3, l.data(), r.data(), 300) == 0);              // 300 frames = 2 packets + 44 left over
    auto got = drain (q);
    CHECK (got.size() == 2);
    for (size_t i = 0; i < got.size(); ++i)
    {
        const auto& p = got[i];
        CHECK (p.stream == 3 && p.bytes == kHeaderSize + kNetOutFrames * 2 * 4);
        const auto h = parseHeader (p.data, p.bytes);
        CHECK (h.has_value());
        if (! h) continue;
        CHECK (h->type == PacketType::audio && h->channels == 2 && h->format == SampleFormat::float32);
        CHECK (h->seq == i && h->timestamp == i * kNetOutFrames);
        std::array<float, kNetOutFrames * 2> dec {}; int frames = 0;
        CHECK (decodeAudio (*h, p.data, dec.data(), kNetOutFrames, frames) && frames == kNetOutFrames);
        for (int n = 0; n < kNetOutFrames; ++n)
        {
            CHECK (dec[(size_t) n * 2] == l[i * kNetOutFrames + (size_t) n]);
            CHECK (dec[(size_t) n * 2 + 1] == r[i * kNetOutFrames + (size_t) n]);
        }
    }
    CHECK (pk.push (3, l.data(), r.data(), 84) == 0);               // 44 + 84 = 128: the leftover completes a packet
    got = drain (q);
    CHECK (got.size() == 1);
    if (! got.empty())
    {
        const auto h = parseHeader (got[0].data, got[0].bytes);
        CHECK (h && h->seq == 2 && h->timestamp == 256);
        std::array<float, kNetOutFrames * 2> dec {}; int frames = 0;
        decodeAudio (*h, got[0].data, dec.data(), kNetOutFrames, frames);
        CHECK (dec[0] == l[256]);                                   // continues where the first call stopped
        CHECK (dec[44 * 2] == l[0]);                                // and then the second call's first frame
    }
}

TEST (packetizer_streams_are_independent)
{
    OutQueue q; NetPacketizer pk (q);
    std::vector<float> a (128, 0.1f), b (128, 0.2f);
    pk.push (0, a.data(), a.data(), 128);
    pk.push (32, b.data(), b.data(), 128);
    pk.push (0, a.data(), a.data(), 128);
    const auto got = drain (q);
    CHECK (got.size() == 3 && got[0].stream == 0 && got[1].stream == 32 && got[2].stream == 0);
    CHECK (parseHeader (got[2].data, got[2].bytes)->seq == 1);
    CHECK (parseHeader (got[1].data, got[1].bytes)->seq == 0);
    // invalid arguments are ignored
    CHECK (pk.push (-1, a.data(), a.data(), 128) == 0 && pk.push (33, a.data(), a.data(), 128) == 0);
    CHECK (pk.push (1, nullptr, a.data(), 128) == 0 && pk.push (1, a.data(), a.data(), 0) == 0);
    CHECK (drain (q).empty());
}

TEST (packetizer_discard_and_reset)
{
    OutQueue q; NetPacketizer pk (q);
    std::vector<float> a (100, 0.3f);
    pk.push (1, a.data(), a.data(), 100);
    pk.discard (1);
    pk.push (1, a.data(), a.data(), 100);                           // 100 + 100 would have made a packet without the discard
    CHECK (drain (q).empty());
    pk.push (1, a.data(), a.data(), 28);
    CHECK (drain (q).size() == 1);
    pk.discard (-5); pk.discard (99);                               // harmless
    pk.reset();
    pk.push (1, a.data(), a.data(), 100); pk.push (1, a.data(), a.data(), 28);
    const auto got = drain (q);
    CHECK (got.size() == 1 && parseHeader (got[0].data, got[0].bytes)->seq == 0);
}

TEST (packetizer_counts_drops_when_queue_full)
{
    OutQueue q; NetPacketizer pk (q);
    std::vector<float> a (128, 0.1f);
    int dropped = 0;
    for (int i = 0; i < 2048 + 10; ++i) dropped += pk.push (0, a.data(), a.data(), 128);
    CHECK (dropped == 10);
    const auto got = drain (q);
    CHECK (got.size() == 2048);
    CHECK (pk.push (0, a.data(), a.data(), 128) == 0);              // after draining, flow resumes
    const auto more = drain (q);
    CHECK (more.size() == 1);
    CHECK (parseHeader (more[0].data, more[0].bytes)->seq == (uint16_t) (2048 + 10));   // the gap is visible to the receiver
}

// ---- real sockets --------------------------------------------------------------------------------------------------
struct Listener
{
    int fd = -1; int port = 0;
    Listener()
    {
        fd = ::socket (AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a {}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl (INADDR_LOOPBACK); a.sin_port = 0;
        ::bind (fd, (sockaddr*) &a, sizeof (a));
        socklen_t len = sizeof (a); ::getsockname (fd, (sockaddr*) &a, &len); port = ntohs (a.sin_port);
        timeval tv { 0, 300000 }; ::setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv));
    }
    ~Listener() { if (fd >= 0) ::close (fd); }
    /** Receives up to `want` datagrams (or until a 300 ms silence). */
    std::vector<std::vector<uint8_t>> receive (size_t want)
    {
        std::vector<std::vector<uint8_t>> got;
        uint8_t buf[2048];
        while (got.size() < want)
        {
            const auto n = ::recv (fd, buf, sizeof (buf), 0);
            if (n <= 0) break;
            got.emplace_back (buf, buf + n);
        }
        return got;
    }
};

TEST (sender_delivers_to_the_right_destinations)
{
    OutQueue q; NetPacketizer pk (q); UdpSender tx (q);
    Listener masterRx, busRx;
    CHECK (tx.start());
    tx.setDestination (0, "127.0.0.1", masterRx.port);
    tx.setDestination (7, "localhost", busRx.port);                  // a name, resolved once on the sender thread
    std::this_thread::sleep_for (std::chrono::milliseconds (20));

    const auto l = ramp (512, 0.0f), r = ramp (512, -0.5f);
    pk.push (0, l.data(), r.data(), 512);
    pk.push (7, r.data(), l.data(), 256);
    pk.push (9, l.data(), r.data(), 128);                            // no destination: counted, not sent anywhere

    const auto m = masterRx.receive (4), b = busRx.receive (2);
    CHECK (m.size() == 4 && b.size() == 2);
    for (size_t i = 0; i < m.size(); ++i)
    {
        const auto h = parseHeader (m[i].data(), m[i].size());
        CHECK (h && h->seq == i && h->channels == 2);
        if (! h) continue;
        std::array<float, kNetOutFrames * 2> dec {}; int frames = 0;
        decodeAudio (*h, m[i].data(), dec.data(), kNetOutFrames, frames);
        CHECK (dec[0] == l[i * kNetOutFrames] && dec[1] == r[i * kNetOutFrames]);
    }
    for (size_t i = 0; i < b.size(); ++i)
    {
        const auto h = parseHeader (b[i].data(), b[i].size());
        CHECK (h.has_value());
        if (! h) continue;
        std::array<float, kNetOutFrames * 2> dec {}; int frames = 0;
        decodeAudio (*h, b[i].data(), dec.data(), kNetOutFrames, frames);
        CHECK (dec[0] == r[i * kNetOutFrames] && dec[1] == l[i * kNetOutFrames]);   // L/R as pushed to that stream
    }
    for (int i = 0; i < 100 && tx.counters().unrouted.load() < 1; ++i) std::this_thread::sleep_for (std::chrono::milliseconds (5));
    CHECK (tx.counters().sent.load() == 6 && tx.counters().unrouted.load() == 1 && tx.counters().failed.load() == 0);
    tx.stop();
}

TEST (sender_follows_destination_changes)
{
    OutQueue q; NetPacketizer pk (q); UdpSender tx (q);
    Listener a, b;
    CHECK (tx.start());
    tx.setDestination (2, "127.0.0.1", a.port);
    std::this_thread::sleep_for (std::chrono::milliseconds (20));
    std::vector<float> s (128, 0.25f);
    pk.push (2, s.data(), s.data(), 128);
    CHECK (a.receive (1).size() == 1);

    tx.setDestination (2, "127.0.0.1", b.port);                     // moved at run time
    std::this_thread::sleep_for (std::chrono::milliseconds (20));
    pk.push (2, s.data(), s.data(), 128);
    CHECK (b.receive (1).size() == 1);
    CHECK (a.receive (1).empty());

    tx.setDestination (2, "", 0);                                    // cleared: nothing leaves
    std::this_thread::sleep_for (std::chrono::milliseconds (20));
    pk.push (2, s.data(), s.data(), 128);
    CHECK (b.receive (1).empty());
    tx.stop();
}

TEST (sender_survives_bad_destinations_and_restarts)
{
    OutQueue q; NetPacketizer pk (q); UdpSender tx (q);
    CHECK (tx.start() && tx.start());                                // starting twice is fine
    tx.setDestination (1, "this-name-does-not-exist.invalid", 9000);
    tx.setDestination (-1, "127.0.0.1", 9000); tx.setDestination (999, "127.0.0.1", 9000);   // ignored
    tx.setDestination (4, "127.0.0.1", 0); tx.setDestination (5, "127.0.0.1", 70000);
    std::vector<float> s (128, 0.1f);
    for (int st : { 1, 4, 5 }) pk.push (st, s.data(), s.data(), 128);
    for (int i = 0; i < 200 && tx.counters().unrouted.load() < 3; ++i) std::this_thread::sleep_for (std::chrono::milliseconds (5));
    CHECK (tx.counters().unrouted.load() == 3 && tx.counters().sent.load() == 0);
    tx.stop(); tx.stop();                                            // stopping twice is fine
    CHECK (! tx.isRunning());
    pk.push (1, s.data(), s.data(), 128);                            // stale packet sits in the queue...
    Listener rx;
    CHECK (tx.start());                                              // ...and is discarded on restart
    tx.setDestination (1, "127.0.0.1", rx.port);
    std::this_thread::sleep_for (std::chrono::milliseconds (20));
    CHECK (rx.receive (1).empty());
    pk.push (1, s.data(), s.data(), 128);
    CHECK (rx.receive (1).size() == 1);
}

TEST (sender_throughput_many_streams)
{
    OutQueue q; NetPacketizer pk (q); UdpSender tx (q);
    std::vector<std::unique_ptr<Listener>> rx;
    CHECK (tx.start());
    for (int st = 0; st < 8; ++st) { rx.push_back (std::make_unique<Listener>()); tx.setDestination (st, "127.0.0.1", rx.back()->port); }
    std::this_thread::sleep_for (std::chrono::milliseconds (20));
    std::vector<float> s (128, 0.1f);
    for (int round = 0; round < 40; ++round)
    {
        for (int st = 0; st < 8; ++st) pk.push (st, s.data(), s.data(), 128);
        std::this_thread::sleep_for (std::chrono::microseconds (500));     // roughly real-time pacing
    }
    size_t total = 0;
    for (auto& l : rx) total += l->receive (40).size();
    CHECK (total >= 8 * 40 * 95 / 100);                                      // loopback should not lose packets; allow 5% slack
    tx.stop();
}

TEST (output_plays_back_through_the_receiver_path)
{
    // What this machine's own receiver does with a Net Out stream: decode -> NetworkPlayout. No loss, right content.
    OutQueue q; NetPacketizer pk (q);
    NetworkPlayout::Config c; c.channels = 2; c.sampleRate = 48000.0; c.targetPackets = 4;
    NetworkPlayout play (c);

    const int total = 128 * 600;
    std::vector<float> l ((size_t) total), r ((size_t) total);
    for (int n = 0; n < total; ++n) { l[(size_t) n] = 0.5f * std::sin (2.0 * 3.14159265358979 * 440.0 * n / 48000.0); r[(size_t) n] = -l[(size_t) n]; }

    std::vector<float> outBuf (256 * 2), outL, outR;
    int fed = 0;
    for (int block = 0; block < total / 256; ++block)
    {
        pk.push (5, l.data() + fed, r.data() + fed, 256);          // the engine pushes one callback block at a time
        fed += 256;
        while (const OutPacket* p = q.peek())
        {
            const auto h = parseHeader (p->data, p->bytes);
            std::array<float, kNetOutFrames * 2> dec {}; int frames = 0;
            CHECK (h && decodeAudio (*h, p->data, dec.data(), kNetOutFrames, frames));
            if (h) play.push (h->seq, dec.data(), frames);
            q.release();
        }
        play.process (outBuf.data(), 256);
        if (block >= 100) for (int n = 0; n < 256; ++n) { outL.push_back (outBuf[(size_t) n * 2]); outR.push_back (outBuf[(size_t) n * 2 + 1]); }
    }
    double el = 0, er = 0, cross = 0;
    for (size_t n = 0; n < outL.size(); ++n) { el += outL[n] * outL[n]; er += outR[n] * outR[n]; cross += outL[n] * outR[n]; }
    el = std::sqrt (el / (double) outL.size()); er = std::sqrt (er / (double) outR.size());
    CHECK (std::abs (el - 0.5 / std::sqrt (2.0)) < 0.01 && std::abs (er - 0.5 / std::sqrt (2.0)) < 0.01);   // full level on both sides
    CHECK (cross < -0.9 * (double) outL.size() * 0.125);                                                       // R is the inverse of L: channels not swapped or mixed
    const auto st = play.status();
    CHECK (st.primed && st.jitter.lost == 0 && st.jitter.underruns == 0 && st.jitter.late == 0 && st.jitter.duplicates == 0);
}

int main()
{
    for (const auto& t : tests())
    {
        const int before = failures;
        t.f();
        std::printf ("%-44s %s\n", t.n, failures == before ? "ok" : "FAILED");
    }
    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
