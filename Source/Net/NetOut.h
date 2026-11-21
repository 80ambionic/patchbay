#pragma once
// Net Out: turns the mixer's stereo outputs (the master and the buses set to "Net Out") into PatchBay audio datagrams.
// Pure C++, wait-free on the audio thread, unit-tested.
//
//   audio thread:  NetPacketizer::push(stream, L, R, frames) --> OutQueue --> UdpSender thread --> sendto()
//
// Stream 0 is the master; stream n is output bus n. Every stream is stereo float32 in the same wire format the Network
// page receives (Packet.h), cut into kNetOutFrames-frame packets (1040 bytes, so a datagram never fragments on a LAN).
// Each stream has its own sequence number and frame-count timestamp.
#include <array>
#include <cstdint>
#include "../Mixer/MixerSettings.h"
#include "Packet.h"
#include "SpscQueue.h"

namespace pb::net
{
constexpr int kNetOutStreams = 1 + kBuses;
constexpr int kNetOutFrames = 128;

struct OutPacket
{
    uint8_t  stream = 0;
    uint16_t bytes = 0;
    uint8_t  data[kHeaderSize + (size_t) kNetOutFrames * 2 * sizeof (float)];
};

using OutQueue = SpscQueue<OutPacket, 2048>;

class NetPacketizer
{
public:
    explicit NetPacketizer (OutQueue& queue) : q (queue) {}

    /** Audio thread. Appends `frames` stereo frames to a stream; every kNetOutFrames frames a packet goes into the queue.
        Returns how many packets were dropped because the queue was full (the sender is not keeping up). */
    int push (int stream, const float* l, const float* r, int frames) noexcept
    {
        if (stream < 0 || stream >= kNetOutStreams || l == nullptr || r == nullptr || frames <= 0) return 0;
        auto& st = streams[(size_t) stream];
        int dropped = 0;
        for (int i = 0; i < frames; ++i)
        {
            st.buf[(size_t) st.fill * 2]     = l[i];
            st.buf[(size_t) st.fill * 2 + 1] = r[i];
            if (++st.fill == kNetOutFrames)
            {
                if (OutPacket* slot = q.acquireWrite())
                {
                    const size_t n = encodeAudio (st.seq, st.ts, st.buf.data(), kNetOutFrames, 2, SampleFormat::float32,
                                                  slot->data, sizeof (slot->data));
                    slot->stream = (uint8_t) stream;
                    slot->bytes = (uint16_t) n;
                    if (n > 0) q.publish(); else ++dropped;
                }
                else ++dropped;
                ++st.seq;                                   // a dropped packet leaves a gap the receiver counts as lost
                st.ts += (uint32_t) kNetOutFrames;
                st.fill = 0;
            }
        }
        return dropped;
    }

    /** Audio thread: the stream stopped (bus went inactive or changed type); forget the partial packet. */
    void discard (int stream) noexcept
    {
        if (stream >= 0 && stream < kNetOutStreams) streams[(size_t) stream].fill = 0;
    }

    /** Not while push() can run. */
    void reset() noexcept { streams = {}; }

private:
    struct Stream
    {
        std::array<float, (size_t) kNetOutFrames * 2> buf {};
        int fill = 0;
        uint16_t seq = 0;
        uint32_t ts = 0;
    };

    OutQueue& q;
    std::array<Stream, kNetOutStreams> streams {};
};
}
