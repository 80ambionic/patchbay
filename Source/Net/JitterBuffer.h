#pragma once
// Sequence-aware playout buffer for network audio. Pure C++, header-only, unit-tested.
//
// Single-threaded: it is owned by the audio thread. The network thread hands packets over through an
// SpscQueue, and the audio callback drains that queue into push() before calling pull().
//
// Handles: reordering (slots are indexed by sequence number), duplicates, late packets, 16-bit
// sequence wrap, lost packets (concealed with silence of the previous packet's length), underruns
// (outputs silence and re-primes), and runaway latency (drops the oldest audio).
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pb::net
{
class JitterBuffer
{
public:
    struct Config
    {
        int channels = 2;
        int capacityPackets = 64;        // must be a power of two (sequence numbers wrap at 65536)
        int targetPackets = 3;           // packets to accumulate before playout starts
        int maxLatencyPackets = 32;      // beyond this the oldest packets are dropped
        int maxFramesPerPacket = 512;
    };

    struct Stats
    {
        uint64_t received = 0, duplicates = 0, late = 0, lost = 0, underruns = 0, droppedForLatency = 0, resyncs = 0;
    };

    explicit JitterBuffer (Config c) : cfg (c), slots ((size_t) c.capacityPackets)
    {
        for (auto& s : slots) s.data.assign ((size_t) c.maxFramesPerPacket * (size_t) c.channels, 0.0f);
    }

    /** Insert one packet of interleaved frames. */
    void push (uint16_t seq, const float* interleaved, int frames)
    {
        if (frames < 1 || frames > cfg.maxFramesPerPacket) return;
        ++stats_.received;

        if (! haveBase)
        {
            haveBase = true;
            readSeq = newestSeq = seq;
            lastFrames = frames;
            store (seq, interleaved, frames);
            return;
        }

        const int d = diff (seq, readSeq);
        if (d < 0 && d >= -cfg.capacityPackets) { ++stats_.late; return; }   // its slot in time has already played
        if (d >= cfg.capacityPackets || d < -cfg.capacityPackets)             // sender jumped or restarted: resync
        {
            ++stats_.resyncs;
            clearAll();
            readSeq = newestSeq = seq;
            readOffset = 0;
            primed = false;
            store (seq, interleaved, frames);
            return;
        }

        auto& s = slot (seq);
        if (s.valid && s.seq == seq) { ++stats_.duplicates; return; }

        store (seq, interleaved, frames);
        if (diff (seq, newestSeq) > 0) newestSeq = seq;

        while (span() > cfg.maxLatencyPackets)                    // trim runaway latency
        {
            slot (readSeq).valid = false;
            ++readSeq;
            readOffset = 0;
            ++stats_.droppedForLatency;
        }

        if (! primed && span() >= cfg.targetPackets) primed = true;
    }

    /** Always writes exactly `frames` interleaved frames. Returns how many were real audio (not concealment). */
    int pull (float* out, int frames)
    {
        const int ch = cfg.channels;
        int produced = 0, real = 0;

        while (produced < frames)
        {
            if (! primed || ! haveBase)
            {
                std::memset (out + (size_t) produced * ch, 0, sizeof (float) * (size_t) (frames - produced) * ch);
                break;
            }

            auto& s = slot (readSeq);
            const bool present = s.valid && s.seq == readSeq;

            if (! present && diff (newestSeq, readSeq) <= 0)       // nothing at or after the cursor: underrun
            {
                ++stats_.underruns;
                primed = false;                                   // wait for the buffer to refill
                continue;                                         // top of loop writes the silence
            }

            const int pktFrames = present ? s.frames : lastFrames;
            if (! present && readOffset == 0) ++stats_.lost;      // newer data exists, so this one isn't coming

            const int n = std::min (pktFrames - readOffset, frames - produced);
            if (present)
            {
                std::memcpy (out + (size_t) produced * ch, s.data.data() + (size_t) readOffset * ch, sizeof (float) * (size_t) n * ch);
                real += n;
            }
            else std::memset (out + (size_t) produced * ch, 0, sizeof (float) * (size_t) n * ch);

            produced += n;
            readOffset += n;
            if (readOffset >= pktFrames)
            {
                if (present) { lastFrames = s.frames; s.valid = false; }
                ++readSeq;
                readOffset = 0;
            }
        }
        return real;
    }

    void reset()
    {
        clearAll();
        haveBase = primed = false;
        readOffset = 0;
        stats_ = {};
    }

    /** Frames queued ahead of the read cursor (counting gaps at the packet size) - input for DriftController. */
    int bufferedFrames() const { return haveBase ? std::max (0, span ()) * lastFrames - readOffset : 0; }
    int bufferedPackets() const { return haveBase ? std::max (0, span()) : 0; }
    bool isPrimed() const noexcept { return primed; }
    int lastPacketFrames() const noexcept { return lastFrames; }     // 0 until the first packet arrives

    /** Change the playout depth at runtime (audio thread). Clamped to leave headroom under maxLatencyPackets. */
    void setTargetPackets (int n) noexcept { cfg.targetPackets = std::clamp (n, 1, std::max (1, cfg.maxLatencyPackets - 4)); }
    int  getTargetPackets() const noexcept { return cfg.targetPackets; }
    const Stats& stats() const noexcept { return stats_; }

private:
    struct Slot { bool valid = false; uint16_t seq = 0; int frames = 0; std::vector<float> data; };

    static int diff (uint16_t a, uint16_t b) noexcept { return (int) (int16_t) (uint16_t) (a - b); }
    int span() const { return diff (newestSeq, readSeq) + 1; }
    Slot& slot (uint16_t seq) { return slots[(size_t) (seq & (uint16_t) (cfg.capacityPackets - 1))]; }

    void store (uint16_t seq, const float* in, int frames)
    {
        auto& s = slot (seq);
        s.valid = true; s.seq = seq; s.frames = frames;
        std::memcpy (s.data.data(), in, sizeof (float) * (size_t) frames * cfg.channels);
    }
    void clearAll() { for (auto& s : slots) s.valid = false; }

    Config cfg;
    std::vector<Slot> slots;
    Stats stats_;
    bool haveBase = false, primed = false;
    uint16_t readSeq = 0, newestSeq = 0;
    int readOffset = 0, lastFrames = 0;
};
}
