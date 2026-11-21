#include "UdpReceiver.h"

namespace pb::net
{
bool UdpReceiver::start (int port)
{
    stop();

    auto s = std::make_unique<juce::DatagramSocket> (false);
    if (! s->bindToPort (port)) return false;

    boundPort = s->getBoundPort();
    socket = std::move (s);
    startThread (juce::Thread::Priority::high);     // timely draining matters more than UI work
    return true;
}

void UdpReceiver::stop()
{
    signalThreadShouldExit();
    if (socket != nullptr) socket->shutdown();      // aborts a blocking read so the thread can exit promptly
    stopThread (1000);
    socket.reset();
    boundPort = 0;
}

void UdpReceiver::run()
{
    alignas (16) uint8_t buf[kMaxPacketBytes + 64];

    while (! threadShouldExit())
    {
        const int ready = socket->waitUntilReady (true, 100);   // wake periodically to notice stop requests
        if (ready < 0) break;                                   // socket closed or errored
        if (ready == 0) continue;

        juce::String senderIp;
        int senderPort = 0;
        const int n = socket->read (buf, (int) sizeof (buf), false, senderIp, senderPort);
        if (n <= 0) continue;

        const auto header = parseHeader (buf, (size_t) n);
        if (! header) { ++stats.badPackets; continue; }
        if (header->type != PacketType::audio) { ++stats.otherPackets; continue; }

        auto* slot = q.acquireWrite();
        if (slot == nullptr) { ++stats.queueFullDrops; continue; }

        int frames = 0;
        if (! decodeAudio (*header, buf, slot->samples, kMaxFramesPerPacket, frames)) { ++stats.badPackets; continue; }

        slot->seq = header->seq;
        slot->timestamp = header->timestamp;
        slot->channels = header->channels;
        slot->frames = (uint16_t) frames;
        q.publish();
        ++stats.audioPackets;
    }
}
}
