#include "UdpSender.h"
#include <chrono>
#include <cstring>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace pb::net
{
bool UdpSender::start()
{
    if (running.load()) return true;

    fd = ::socket (AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    const int flags = ::fcntl (fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl (fd, F_SETFL, flags | O_NONBLOCK);       // a full send buffer must never stall the thread
    int big = 1 << 20;
    ::setsockopt (fd, SOL_SOCKET, SO_SNDBUF, &big, sizeof (big));
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt (fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof (one));
#endif

    while (q.peek() != nullptr) q.release();                         // nothing is consuming now: drop stale packets
    running.store (true);
    thread = std::thread ([this] { run(); });
    return true;
}

void UdpSender::stop()
{
    if (! running.exchange (false)) { if (fd >= 0) { ::close (fd); fd = -1; } return; }
    if (thread.joinable()) thread.join();
    if (fd >= 0) { ::close (fd); fd = -1; }
}

void UdpSender::setDestination (int stream, const std::string& host, int port)
{
    if (stream < 0 || stream >= kNetOutStreams) return;
    {
        std::lock_guard<std::mutex> lock (mutex);
        requested[(size_t) stream] = { host, port };
    }
    generation.fetch_add (1);
}

void UdpSender::run()
{
    struct Resolved { bool ok = false; sockaddr_in addr {}; };
    std::array<Resolved, kNetOutStreams> resolved;
    std::array<Requested, kNetOutStreams> seen;
    uint32_t seenGen = 0;

    while (running.load())
    {
        if (const uint32_t g = generation.load(); g != seenGen)
        {
            std::array<Requested, kNetOutStreams> now;
            { std::lock_guard<std::mutex> lock (mutex); now = requested; }
            for (size_t i = 0; i < now.size(); ++i)
            {
                if (now[i].host == seen[i].host && now[i].port == seen[i].port && seenGen != 0) continue;
                resolved[i] = {};
                if (! now[i].host.empty() && now[i].port >= 1 && now[i].port <= 65535)
                {
                    addrinfo hints {};
                    hints.ai_family = AF_INET;
                    hints.ai_socktype = SOCK_DGRAM;
                    addrinfo* res = nullptr;
                    const std::string portText = std::to_string (now[i].port);
                    if (::getaddrinfo (now[i].host.c_str(), portText.c_str(), &hints, &res) == 0 && res != nullptr)
                    {
                        if (res->ai_addrlen == sizeof (sockaddr_in))
                        {
                            std::memcpy (&resolved[i].addr, res->ai_addr, sizeof (sockaddr_in));
                            resolved[i].ok = true;
                        }
                        ::freeaddrinfo (res);
                    }
                }
            }
            seen = now;
            seenGen = g;
        }

        bool didWork = false;
        for (int batch = 0; batch < 256; ++batch)
        {
            const OutPacket* p = q.peek();
            if (p == nullptr) break;
            didWork = true;
            const auto& d = resolved[p->stream < kNetOutStreams ? p->stream : 0];
            if (! d.ok) stats.unrouted.fetch_add (1, std::memory_order_relaxed);
            else if (::sendto (fd, p->data, p->bytes, 0, reinterpret_cast<const sockaddr*> (&d.addr), sizeof (d.addr)) == (ssize_t) p->bytes)
                stats.sent.fetch_add (1, std::memory_order_relaxed);
            else
                stats.failed.fetch_add (1, std::memory_order_relaxed);
            q.release();
        }
        if (! didWork) std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
}
}
