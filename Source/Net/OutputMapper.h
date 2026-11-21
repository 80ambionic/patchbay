#pragma once
// Maps one slice of interleaved stream audio onto the device's planar output buffers. Pure C++, unit-tested.
//   - stream channel i -> output channel i
//   - a mono stream is copied to outputs 0 and 1
//   - output channels beyond the stream are left untouched (the caller has already cleared them)
//   - stream channels beyond the device's outputs are dropped
//   - null output pointers are skipped
// Returns the slice's peak level for output 0 and 1 via peak0/peak1 (max-accumulated, not reset).
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pb::net
{
inline void mapToOutputs (const float* interleaved, int streamChannels, int frames,
                          float* const* out, int numOut, int outOffset, float& peak0, float& peak1) noexcept
{
    for (int c = 0; c < numOut; ++c)
    {
        const int src = streamChannels == 1 ? (c < 2 ? 0 : -1) : (c < streamChannels ? c : -1);
        if (src < 0 || out[c] == nullptr) continue;

        float* dst = out[c] + outOffset;
        float peak = 0.0f;
        for (int i = 0; i < frames; ++i)
        {
            const float v = interleaved[(size_t) i * (size_t) streamChannels + (size_t) src];
            dst[i] = v;
            peak = std::max (peak, std::abs (v));
        }
        if (c == 0) peak0 = std::max (peak0, peak);
        if (c == 1) peak1 = std::max (peak1, peak);
    }
}
}
