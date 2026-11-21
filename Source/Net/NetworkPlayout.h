#pragma once
// Everything between "decoded packets arrive" and "samples leave for the sound card":
//
//   push(seq, frames) --> JitterBuffer --(pull 64-frame chunks)--> Catmull-Rom resampler --> process(out)
//                              |                                          ^
//                              +-- bufferedFrames() --> DriftController --+ (playback ratio)
//
// The ratio is ~1 +/- a few hundred ppm: it absorbs the difference between the sender's and this device's
// audio clocks so the buffer neither drains nor grows. Pure C++, no JUCE, real-time safe after construction
// (no allocation, locks or syscalls in push/process). One thread (the audio thread) calls everything.
//
// Assumes the sender's sample rate equals this device's rate; the wire format carries no rate field yet,
// so a 44.1 kHz stream into a 48 kHz device will play at the wrong pitch. (Wire format v2 should add it.)
#include <algorithm>
#include <cstring>
#include <vector>
#include "DriftController.h"
#include "JitterBuffer.h"
#include "Packet.h"

namespace pb::net
{
class NetworkPlayout
{
public:
    struct Config
    {
        int channels = 2;                 // 1..kMaxChannels; must match the stream
        double sampleRate = 48000.0;
        int targetPackets = 4;
        double driftSettleSeconds = 30.0;
    };

    struct Status
    {
        bool primed = false;              // false while buffering or after an underrun
        int packetFrames = 0;
        double fillFrames = 0;            // jitter-buffer content (what the drift controller regulates)
        double targetFrames = 0;
        double ratioPpm = 0;              // current drift correction
        JitterBuffer::Stats jitter;
    };

    explicit NetworkPlayout (Config c)
        : cfg (c),
          jb (makeJbConfig (c)),
          drift (c.targetPackets * 128.0, c.sampleRate, c.driftSettleSeconds),
          chunk ((size_t) kChunkFrames * (size_t) c.channels, 0.0f),
          hist ((size_t) 4 * (size_t) c.channels, 0.0f)
    {
        jb.setTargetPackets (c.targetPackets);
    }

    void push (uint16_t seq, const float* interleaved, int frames) { jb.push (seq, interleaved, frames); }

    void setTargetPackets (int n) { jb.setTargetPackets (n); }

    /** Writes exactly `frames` interleaved frames (silence while buffering, underrunning, or concealing loss). */
    void process (float* out, int frames)
    {
        updateDrift (frames);
        const int ch = cfg.channels;
        const double ratio = drift.ratio();

        for (int i = 0; i < frames; ++i)
        {
            while (phase >= 1.0) { advanceInput(); phase -= 1.0; }

            const float t = (float) phase;
            for (int c = 0; c < ch; ++c)
                out[(size_t) i * ch + c] = catmullRom (hist[c], hist[ch + c], hist[2 * ch + c], hist[3 * ch + c], t);

            phase += ratio;
        }
    }

    Status status() const
    {
        Status s;
        s.primed = jb.isPrimed();
        s.packetFrames = jb.lastPacketFrames();
        s.fillFrames = measuredFill();
        s.targetFrames = (double) jb.getTargetPackets() * s.packetFrames;
        s.ratioPpm = (drift.ratio() - 1.0) * 1e6;
        s.jitter = jb.stats();
        return s;
    }

    int channels() const noexcept { return cfg.channels; }

private:
    static constexpr int kChunkFrames = 64;

    static JitterBuffer::Config makeJbConfig (const Config& c)
    {
        JitterBuffer::Config j;
        j.channels = c.channels;
        j.targetPackets = c.targetPackets;
        return j;
    }

    static float catmullRom (float y0, float y1, float y2, float y3, float t) noexcept
    {
        const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
        const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c = -0.5f * y0 + 0.5f * y2;
        return ((a * t + b) * t + c) * t + y1;        // exactly y1 at t = 0
    }

    void updateDrift (int callbackFrames)
    {
        if (! jb.isPrimed())
        {
            wasPrimed = false;
            return;                                   // ratio stays at its last value; nothing to regulate while buffering
        }
        const int pf = jb.lastPacketFrames();
        const double target = (double) jb.getTargetPackets() * pf;
        drift.setTarget (target);
        if (! wasPrimed)                              // (re)starting playout
        {
            chunkPos = kChunkFrames;                  // drop read-ahead: it is silence pulled while buffering, not audio
            drift.reset (measuredFill());             // forget the transient, start neutral
            wasPrimed = true;
        }
        const double fill = measuredFill();
        drift.update (fill, callbackFrames);
    }

    /** Frames of audio queued for playout: the jitter buffer plus what the resampler has already pulled but
        not yet consumed. Leaving the read-ahead out would add up to +/-32 frames of sawtooth noise. */
    double measuredFill() const noexcept { return (double) jb.bufferedFrames() + (double) (kChunkFrames - chunkPos); }

    /** Shift the 4-sample history by one input frame, pulling a fresh chunk from the jitter buffer when needed. */
    void advanceInput()
    {
        const int ch = cfg.channels;
        if (chunkPos >= kChunkFrames)
        {
            jb.pull (chunk.data(), kChunkFrames);     // always returns exactly kChunkFrames (silence if starved)
            chunkPos = 0;
        }
        std::memmove (hist.data(), hist.data() + ch, sizeof (float) * (size_t) (3 * ch));
        std::memcpy (hist.data() + 3 * ch, chunk.data() + (size_t) chunkPos * ch, sizeof (float) * (size_t) ch);
        ++chunkPos;
    }

    Config cfg;
    JitterBuffer jb;
    DriftController drift;
    std::vector<float> chunk, hist;       // hist: [h0 h1 h2 h3] x channels; interpolates between h1 and h2
    int chunkPos = kChunkFrames;          // start "exhausted" so the first advance pulls
    double phase = 0.0;
    bool wasPrimed = false;
};
}
