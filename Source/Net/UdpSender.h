#pragma once
// Sends Net Out packets. One thread drains the OutQueue and sendto()s each packet to its stream's destination.
// POSIX sockets (macOS, iOS, Linux), IPv4. Names are resolved on the sender thread when a destination changes, never
// per packet and never on the audio thread.
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include "NetOut.h"

namespace pb::net
{
class UdpSender
{
public:
    struct Counters
    {
        std::atomic<uint64_t> sent { 0 };            // datagrams handed to the OS
        std::atomic<uint64_t> failed { 0 };          // sendto() refused them (no route, buffer full, ...)
        std::atomic<uint64_t> unrouted { 0 };        // packets for a stream with no usable destination
    };

    explicit UdpSender (OutQueue& queue) : q (queue) {}
    ~UdpSender() { stop(); }

    /** Opens the socket and starts the thread. False if the socket could not be created. */
    bool start();
    void stop();
    bool isRunning() const noexcept { return running.load(); }

    /** Any thread. Empty host (or a name that does not resolve) = nothing is sent for that stream. */
    void setDestination (int stream, const std::string& host, int port);

    const Counters& counters() const noexcept { return stats; }

private:
    struct Requested { std::string host; int port = 0; };
    void run();

    OutQueue& q;
    std::thread thread;
    std::atomic<bool> running { false };
    int fd = -1;
    Counters stats;

    std::mutex mutex;                                // guards `requested`; the audio thread never takes it
    std::array<Requested, kNetOutStreams> requested;
    std::atomic<uint32_t> generation { 1 };
};
}
