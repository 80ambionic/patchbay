#pragma once
// Mixer settings model: plain C++, no JUCE. Everything the user can set on the mixer page and that gets saved.
// All values pass through the clamp helpers, so a hand-edited file or a NaN from a UI bug can never reach the DSP.
#include <array>
#include <cmath>
#include <string>

namespace pb
{
constexpr int   kNetworkStrips = 8;         // strips 1-8: network stream channels 1-8
constexpr int   kAppSlots = 4;              // app-capture slots; each is a stereo pair of strips
constexpr int   kAppStrips = kAppSlots * 2; // strips 9-16: slot 1 L/R, slot 2 L/R, ...
constexpr int   kInputStrips = 8;           // strips 17-24: audio input device channels 1-8 (a microphone, an interface, or a
                                            // virtual driver such as BlackHole)
constexpr int   kMixerStrips = kNetworkStrips + kAppStrips + kInputStrips;
constexpr int   kMaxDeviceOutputs = 64;     // upper bound on device output channels the router will address
constexpr float kMinGainDb   = -60.0f;      // fader floor: at or below this the strip is fully off (-inf)
constexpr float kMaxGainDb   = 12.0f;
constexpr size_t kMaxStripNameLength = 16;
constexpr size_t kMaxAppNameLength = 48;
constexpr size_t kMaxBundleIdLength = 200;

inline float clampGainDb (float v) noexcept
{
    if (! std::isfinite (v)) return 0.0f;
    return v < kMinGainDb ? kMinGainDb : (v > kMaxGainDb ? kMaxGainDb : v);
}

inline float clampPan (float v) noexcept
{
    if (! std::isfinite (v)) return 0.0f;
    return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
}

/** dB -> linear; the bottom of the fader range is a true zero. */
inline float gainDbToLinear (float db) noexcept
{
    db = clampGainDb (db);
    return db <= kMinGainDb ? 0.0f : std::pow (10.0f, db / 20.0f);
}

inline int clampDirectOut (int v) noexcept { return v < 0 ? 0 : (v > kMaxDeviceOutputs ? kMaxDeviceOutputs : v); }

/** First device channel (1-based) of the master pair; the pair is (v, v+1), so v is 1..kMaxDeviceOutputs-1. */
inline int clampMasterOut (int v) noexcept { return v < 1 ? 1 : (v > kMaxDeviceOutputs - 1 ? kMaxDeviceOutputs - 1 : v); }

inline std::string sanitizeText (const std::string& in, size_t maxLen)
{
    std::string out;
    for (char c : in)
    {
        if (out.size() >= maxLen) break;
        if ((unsigned char) c >= 0x20 && c != 0x7f) out.push_back (c);    // drops control characters; keeps UTF-8 bytes
    }
    return out;
}

inline std::string sanitizeStripName (const std::string& in) { return sanitizeText (in, kMaxStripNameLength); }

/** Reverse-DNS style identifier, e.g. "com.apple.Safari". Only the characters bundle IDs actually use. */
inline bool isValidBundleId (const std::string& s) noexcept
{
    if (s.empty() || s.size() > kMaxBundleIdLength) return false;
    for (char c : s)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (! ok) return false;
    }
    return true;
}

struct StripSettings
{
    float gainDb = 0.0f;
    float pan = 0.0f;                // -1 = hard left, +1 = hard right
    bool mute = false, solo = false;
    int directOut = 0;               // 0 = through the master bus; n = straight to device output channel n (post-fader, no pan)
    std::string name;                // empty = the default name for this strip

    bool operator== (const StripSettings& o) const noexcept
    { return gainDb == o.gainDb && pan == o.pan && mute == o.mute && solo == o.solo && directOut == o.directOut && name == o.name; }
};

/** Which running app feeds an app-capture slot. The selection is saved, but capture itself is never started
    automatically: the user switches each slot on after launch (it triggers a macOS permission prompt). */
struct AppTarget
{
    std::string bundleId;            // empty = slot unused
    std::string name;                // display name captured when chosen (the app may not be running next launch)
    bool silenceOriginal = true;     // mute the app's normal output so it is heard only through the mixer

    bool operator== (const AppTarget& o) const noexcept
    { return bundleId == o.bundleId && name == o.name && silenceOriginal == o.silenceOriginal; }
};

struct MixerSettings
{
    std::array<StripSettings, kMixerStrips> strips;
    std::array<AppTarget, kAppSlots> apps;
    float masterDb = 0.0f;
    bool masterMute = false;
    int masterOut = 1;               // master bus plays on device outputs (masterOut, masterOut + 1)

    /** Odd channels hard left, even channels hard right: a stereo stream then plays as stereo out of the box. */
    MixerSettings()
    {
        for (int i = 0; i < kMixerStrips; ++i) strips[(size_t) i].pan = (i % 2 == 0) ? -1.0f : 1.0f;
    }

    void sanitize()
    {
        for (auto& s : strips)
        {
            s.gainDb = clampGainDb (s.gainDb); s.pan = clampPan (s.pan);
            s.directOut = clampDirectOut (s.directOut);
            s.name = sanitizeStripName (s.name);
        }
        for (auto& a : apps)
        {
            if (! isValidBundleId (a.bundleId)) a = AppTarget();
            a.name = sanitizeText (a.name, kMaxAppNameLength);
        }
        masterDb = clampGainDb (masterDb);
        masterOut = clampMasterOut (masterOut);
    }

    bool operator== (const MixerSettings& o) const noexcept
    { return strips == o.strips && apps == o.apps && masterDb == o.masterDb && masterMute == o.masterMute && masterOut == o.masterOut; }
};
}
