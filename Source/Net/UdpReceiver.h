#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>
#include "Packet.h"
#include "SpscQueue.h"

namespace pb::net
{
/** One decoded audio datagram, as handed from the network thread to the audio thread. */
struct ReceivedAudio
{
    uint16_t seq = 0;
    uint32_t timestamp = 0;
    uint8_t  channels = 0;
    uint16_t frames = 0;
    float    samples[kMaxFramesPerPacket * kMaxChannels];   // interleaved
};

/** Listens on a UDP port, validates and decodes audio packets, and pushes them into a lock-free queue.
    The audio thread drains `queue()` into a JitterBuffer. The network thread never touches audio-thread
    state and the audio thread never blocks on the network thread.

    One sender per receiver for now (no stream IDs): packets from other senders would interleave. */
class UdpReceiver : private juce::Thread
{
public:
    using Queue = SpscQueue<ReceivedAudio, 128>;

    struct Counters
    {
        std::atomic<uint64_t> audioPackets { 0 };     // valid audio packets queued
        std::atomic<uint64_t> badPackets { 0 };       // failed validation (garbage, truncated, wrong version)
        std::atomic<uint64_t> otherPackets { 0 };     // valid but not audio (midi/ping: not handled here yet)
        std::atomic<uint64_t> queueFullDrops { 0 };   // audio thread isn't draining fast enough
    };

    UdpReceiver() : juce::Thread ("PatchBay UDP receiver") {}
    ~UdpReceiver() override { stop(); }

    /** Pass 0 to let the OS pick a free port (see getPort()). Returns false if the bind failed. */
    bool start (int port);
    void stop();

    bool isRunning() const { return juce::Thread::isThreadRunning(); }
    int  getPort() const noexcept { return boundPort.load(); }
    Queue& queue() noexcept { return q; }
    const Counters& counters() const noexcept { return stats; }

private:
    void run() override;

    std::unique_ptr<juce::DatagramSocket> socket;   // recreated on every start(): shutdown() makes a socket unusable
    std::atomic<int> boundPort { 0 };
    Queue q;
    Counters stats;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UdpReceiver)
};
}
