#pragma once
// Places stereo mixer outputs (the master, and every output bus set to "Audio Out") onto the audio device's output
// channels. Pure C++, unit-tested.
//
//   each entry: L -> device channel `first`, R -> device channel `first + 1`      (1-based)
//
// Rules:
//   - Outputs are ADDED into `out` (the caller clears it first), so two entries may share channels and simply sum.
//   - If an entry's R channel does not exist on the device (e.g. a mono device) but L does, L gets (L + R) / 2.
//   - Channels that do not exist, and null output pointers, are skipped.
//   - Every channel written to is clamped to +-1 afterwards (entries can sum past full scale); returns true if any clamped.
#include <algorithm>
#include <cmath>
#include "MixerSettings.h"

namespace pb
{
struct StereoOut
{
    const float* l = nullptr;
    const float* r = nullptr;
    int first = 1;
};

inline bool routeToDevice (const StereoOut* outs, int numOuts, int frames, float* const* out, int numOut, int offset) noexcept
{
    if (frames <= 0 || numOut <= 0 || out == nullptr || outs == nullptr) return false;
    const int numCh = std::min (numOut, kMaxDeviceOutputs);
    bool touched[kMaxDeviceOutputs] = {};

    auto chan = [&] (int oneBased) -> float*
    {
        if (oneBased < 1 || oneBased > numCh || out[oneBased - 1] == nullptr) return nullptr;
        touched[oneBased - 1] = true;
        return out[oneBased - 1] + offset;
    };

    for (int e = 0; e < numOuts; ++e)
    {
        const auto& o = outs[e];
        if (o.l == nullptr || o.r == nullptr) continue;
        float* l = chan (o.first);
        float* r = chan (o.first + 1);
        if (l != nullptr && r != nullptr)
        {
            for (int i = 0; i < frames; ++i) { l[i] += o.l[i]; r[i] += o.r[i]; }
        }
        else if (l != nullptr)                                      // R is off the end of the device: fold to mono
        {
            for (int i = 0; i < frames; ++i) l[i] += 0.5f * (o.l[i] + o.r[i]);
        }
        else if (r != nullptr)                                      // (only reachable with an absent L channel pointer)
        {
            for (int i = 0; i < frames; ++i) r[i] += o.r[i];
        }
    }

    bool clipped = false;
    for (int c = 0; c < numCh; ++c)
    {
        if (! touched[c]) continue;
        float* p = out[c] + offset;
        for (int i = 0; i < frames; ++i)
        {
            const float v = p[i];
            if (! (v <= 1.0f && v >= -1.0f)) { clipped = true; p[i] = std::isnan (v) ? 0.0f : (v > 0.0f ? 1.0f : -1.0f); }
        }
    }
    return clipped;
}
}
