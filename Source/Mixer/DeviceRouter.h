#pragma once
// Places the mixer's outputs onto the audio device's output channels. Pure C++, unit-tested.
//
//   master L/R  -> device channels (masterFirst, masterFirst + 1)      (1-based)
//   direct[i]   -> device channel directCh[i] (when > 0), summed with whatever else is there
//
// Rules:
//   - Outputs are ADDED into `out` (the caller clears it first), so a master pair and a direct out may share a channel.
//   - If the master's R channel does not exist on the device (e.g. a mono device) but L does, L gets (L + R) / 2.
//   - Channels that do not exist, and null output pointers, are skipped.
//   - Every channel written to is clamped to +-1 afterwards (direct outs can sum past full scale); returns true if any clamped.
#include <algorithm>
#include <cmath>
#include "MixerSettings.h"

namespace pb
{
inline bool routeToDevice (const float* mixL, const float* mixR, int masterFirst,
                           const float* const* direct, const int* directCh, int numDirect,
                           int frames, float* const* out, int numOut, int offset) noexcept
{
    if (frames <= 0 || numOut <= 0 || out == nullptr) return false;
    const int numCh = std::min (numOut, kMaxDeviceOutputs);
    bool touched[kMaxDeviceOutputs] = {};

    auto chan = [&] (int oneBased) -> float*
    {
        if (oneBased < 1 || oneBased > numCh || out[oneBased - 1] == nullptr) return nullptr;
        touched[oneBased - 1] = true;
        return out[oneBased - 1] + offset;
    };

    if (mixL != nullptr && mixR != nullptr)
    {
        float* l = chan (masterFirst);
        float* r = chan (masterFirst + 1);
        if (l != nullptr && r != nullptr)
        {
            for (int i = 0; i < frames; ++i) { l[i] += mixL[i]; r[i] += mixR[i]; }
        }
        else if (l != nullptr)                                      // R is off the end of the device: fold to mono
        {
            for (int i = 0; i < frames; ++i) l[i] += 0.5f * (mixL[i] + mixR[i]);
        }
        else if (r != nullptr)                                      // (only reachable with an absent L channel pointer)
        {
            for (int i = 0; i < frames; ++i) r[i] += mixR[i];
        }
    }

    if (direct != nullptr && directCh != nullptr)
        for (int s = 0; s < numDirect; ++s)
        {
            if (directCh[s] <= 0 || direct[s] == nullptr) continue;
            if (float* d = chan (directCh[s]))
                for (int i = 0; i < frames; ++i) d[i] += direct[s][i];
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
