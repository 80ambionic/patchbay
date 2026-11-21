#pragma once
// Wire format for PatchBay's network audio/MIDI. Pure C++ (no JUCE), header-only, unit-tested.
//
//  offset size field
//   0     4    magic 'PBNT' (big-endian)
//   4     1    version
//   5     1    type        (1 audio, 2 midi, 3 ping)
//   6     2    seq         (big-endian, wraps)
//   8     4    timestamp   (big-endian; sample frames for audio, microseconds for midi)
//  12     2    payloadBytes(big-endian)
//  14     1    channels    (audio only)
//  15     1    format      (audio only: 0 float32-LE, 1 int16-LE)
//  16..        payload
//
// Keep header + payload under ~1400 bytes so a datagram never fragments on a LAN (2ch x 128 frames of
// float32 is 1024 bytes).
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace pb::net
{
static_assert (std::endian::native == std::endian::little, "payload codec assumes a little-endian host (true for macOS/iOS)");

constexpr uint32_t kMagic = 0x50424E54;   // 'PBNT'
constexpr uint8_t  kVersion = 1;
constexpr size_t   kHeaderSize = 16;
constexpr int      kMaxChannels = 8;
constexpr int      kMaxFramesPerPacket = 512;
constexpr size_t   kMaxPacketBytes = kHeaderSize + (size_t) kMaxFramesPerPacket * kMaxChannels * sizeof (float);

enum class PacketType : uint8_t { audio = 1, midi = 2, ping = 3 };
enum class SampleFormat : uint8_t { float32 = 0, int16 = 1 };

struct PacketHeader
{
    PacketType   type = PacketType::audio;
    SampleFormat format = SampleFormat::float32;
    uint8_t      channels = 2;
    uint16_t     seq = 0;
    uint32_t     timestamp = 0;
    uint16_t     payloadBytes = 0;
};

constexpr size_t bytesPerSample (SampleFormat f) noexcept { return f == SampleFormat::int16 ? 2 : 4; }

namespace detail
{
inline void put16 (uint8_t* p, uint16_t v) { p[0] = (uint8_t) (v >> 8); p[1] = (uint8_t) v; }
inline void put32 (uint8_t* p, uint32_t v) { p[0] = (uint8_t) (v >> 24); p[1] = (uint8_t) (v >> 16); p[2] = (uint8_t) (v >> 8); p[3] = (uint8_t) v; }
inline uint16_t get16 (const uint8_t* p) { return (uint16_t) ((p[0] << 8) | p[1]); }
inline uint32_t get32 (const uint8_t* p) { return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3]; }
}

inline void writeHeader (const PacketHeader& h, uint8_t* dst)
{
    detail::put32 (dst, kMagic);
    dst[4] = kVersion;
    dst[5] = (uint8_t) h.type;
    detail::put16 (dst + 6, h.seq);
    detail::put32 (dst + 8, h.timestamp);
    detail::put16 (dst + 12, h.payloadBytes);
    dst[14] = h.channels;
    dst[15] = (uint8_t) h.format;
}

/** Validates everything a hostile or corrupt datagram could get wrong. Returns nullopt if rejected. */
inline std::optional<PacketHeader> parseHeader (const uint8_t* data, size_t len)
{
    if (data == nullptr || len < kHeaderSize) return std::nullopt;
    if (detail::get32 (data) != kMagic || data[4] != kVersion) return std::nullopt;

    PacketHeader h;
    if (data[5] < 1 || data[5] > 3) return std::nullopt;
    h.type = (PacketType) data[5];
    h.seq = detail::get16 (data + 6);
    h.timestamp = detail::get32 (data + 8);
    h.payloadBytes = detail::get16 (data + 12);
    h.channels = data[14];
    if (data[15] > 1) return std::nullopt;
    h.format = (SampleFormat) data[15];

    if ((size_t) h.payloadBytes + kHeaderSize > len) return std::nullopt;     // truncated

    if (h.type == PacketType::audio)
    {
        if (h.channels < 1 || h.channels > kMaxChannels) return std::nullopt;
        const size_t frameBytes = (size_t) h.channels * bytesPerSample (h.format);
        if (h.payloadBytes == 0 || h.payloadBytes % frameBytes != 0) return std::nullopt;
        if (h.payloadBytes / frameBytes > (size_t) kMaxFramesPerPacket) return std::nullopt;
    }
    return h;
}

/** Interleaved float frames -> datagram. Returns bytes written, or 0 if it doesn't fit / bad args. */
inline size_t encodeAudio (uint16_t seq, uint32_t timestamp, const float* interleaved, int frames, int channels,
                           SampleFormat fmt, uint8_t* dst, size_t capacity)
{
    if (frames < 1 || frames > kMaxFramesPerPacket || channels < 1 || channels > kMaxChannels) return 0;
    const size_t payload = (size_t) frames * (size_t) channels * bytesPerSample (fmt);
    if (payload + kHeaderSize > capacity || payload > 0xFFFF) return 0;

    PacketHeader h;
    h.type = PacketType::audio; h.format = fmt; h.channels = (uint8_t) channels;
    h.seq = seq; h.timestamp = timestamp; h.payloadBytes = (uint16_t) payload;
    writeHeader (h, dst);

    uint8_t* out = dst + kHeaderSize;
    const size_t n = (size_t) frames * (size_t) channels;
    if (fmt == SampleFormat::float32)
        std::memcpy (out, interleaved, n * sizeof (float));
    else
        for (size_t i = 0; i < n; ++i)
        {
            const float c = interleaved[i] > 1.0f ? 1.0f : (interleaved[i] < -1.0f ? -1.0f : interleaved[i]);
            const auto s = (int16_t) (c * 32767.0f + (c >= 0 ? 0.5f : -0.5f));
            std::memcpy (out + i * 2, &s, 2);
        }
    return kHeaderSize + payload;
}

/** Datagram payload -> interleaved float frames. `header` must come from parseHeader on the same buffer. */
inline bool decodeAudio (const PacketHeader& h, const uint8_t* datagram, float* out, int maxFrames, int& framesOut)
{
    framesOut = 0;
    if (h.type != PacketType::audio) return false;
    const size_t frameBytes = (size_t) h.channels * bytesPerSample (h.format);
    const int frames = (int) (h.payloadBytes / frameBytes);
    if (frames > maxFrames) return false;

    const uint8_t* in = datagram + kHeaderSize;
    const size_t n = (size_t) frames * h.channels;
    if (h.format == SampleFormat::float32)
        std::memcpy (out, in, n * sizeof (float));
    else
        for (size_t i = 0; i < n; ++i)
        {
            int16_t s; std::memcpy (&s, in + i * 2, 2);
            out[i] = (float) s / 32768.0f;
        }
    framesOut = frames;
    return true;
}
}
