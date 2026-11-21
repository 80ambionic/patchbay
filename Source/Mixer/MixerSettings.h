#pragma once
// Mixer settings model: plain C++, no JUCE. Everything the user can set on the mixer page and that gets saved
// (and what a mixer preset contains). All values pass through the clamp helpers, so a hand-edited file or a NaN from a
// UI bug can never reach the DSP.
//
// Layout (v2): 32 stereo input strips, each with its own source, and 32 stereo output buses. Every strip sends to the
// master or to one bus. The master and every bus end at either a pair of device outputs ("Audio Out") or a UDP stream to
// another machine ("Net Out").
#include <array>
#include <cmath>
#include <string>

namespace pb
{
constexpr int   kStrips = 32;               // stereo input strips
constexpr int   kBuses = 32;                // stereo output buses
constexpr int   kAppSlots = 4;              // app-capture slots; each is one stereo source
constexpr int   kNetPairs = 4;              // the network stream has up to 8 channels = 4 stereo pairs
constexpr int   kMaxDeviceInputs = 64;      // device input channels the engine will open (32 stereo pairs)
constexpr int   kMaxDeviceOutputs = 64;     // upper bound on device output channels the router will address
constexpr int   kDevicePairs = kMaxDeviceInputs / 2;
constexpr float kMinGainDb   = -60.0f;      // fader floor: at or below this the strip is fully off (-inf)
constexpr float kMaxGainDb   = 12.0f;
constexpr size_t kMaxStripNameLength = 16;
constexpr size_t kMaxAppNameLength = 48;
constexpr size_t kMaxBundleIdLength = 200;
constexpr size_t kMaxHostLength = 253;
constexpr int   kDefaultNetOutPort = 9000;  // the Network page's default listening port

/** What a strip listens to. `index` is the stereo pair (or app slot) within that source, counted from 0. */
enum class SourceType : int { off = 0, net = 1, app = 2, input = 3 };

/** Where a bus or the master ends up. */
enum class OutType : int { audio = 0, net = 1 };

inline int sourceCount (SourceType t) noexcept
{
    switch (t)
    {
        case SourceType::net:   return kNetPairs;
        case SourceType::app:   return kAppSlots;
        case SourceType::input: return kDevicePairs;
        default:                return 0;
    }
}

inline float clampGainDb (float v) noexcept
{
    if (! std::isfinite (v)) return 0.0f;
    return v < kMinGainDb ? kMinGainDb : (v > kMaxGainDb ? kMaxGainDb : v);
}

inline float clampBalance (float v) noexcept
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

/** First device channel (1-based) of an output pair; the pair is (v, v+1), so v is 1..kMaxDeviceOutputs-1. */
inline int clampOutFirst (int v) noexcept { return v < 1 ? 1 : (v > kMaxDeviceOutputs - 1 ? kMaxDeviceOutputs - 1 : v); }

/** 0 = master, 1..kBuses = bus n. */
inline int clampStripOut (int v) noexcept { return v < 0 ? 0 : (v > kBuses ? kBuses : v); }

inline int clampPort (int v) noexcept { return v < 1 ? kDefaultNetOutPort : (v > 65535 ? kDefaultNetOutPort : v); }

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

/** A hostname or IPv4 address as typed: letters, digits, '.', '-', '_' only (no spaces, no URL parts). Empty is valid
    and means "no destination yet". */
inline bool isValidHost (const std::string& s) noexcept
{
    if (s.size() > kMaxHostLength) return false;
    for (char c : s)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (! ok) return false;
    }
    return true;
}

struct Source
{
    SourceType type = SourceType::off;
    int index = 0;

    void sanitize() noexcept
    {
        const int n = sourceCount (type);
        if (type != SourceType::net && type != SourceType::app && type != SourceType::input) type = SourceType::off;
        index = n <= 0 ? 0 : (index < 0 ? 0 : (index >= n ? n - 1 : index));
    }
    bool operator== (const Source& o) const noexcept { return type == o.type && index == o.index; }
};

/** Where a Net Out stream is sent. Empty host = not set up yet, nothing is sent. */
struct NetDest
{
    std::string host;
    int port = kDefaultNetOutPort;

    void sanitize()
    {
        if (! isValidHost (host)) host.clear();
        port = clampPort (port);
    }
    bool operator== (const NetDest& o) const noexcept { return host == o.host && port == o.port; }
};

struct StripSettings
{
    Source src;
    float gainDb = 0.0f;
    float balance = 0.0f;            // -1 = left only, 0 = as recorded, +1 = right only
    bool mute = false, solo = false;
    int out = 0;                     // 0 = master, n = output bus n
    std::string name;                // empty = the default name for this strip's source

    bool operator== (const StripSettings& o) const noexcept
    { return src == o.src && gainDb == o.gainDb && balance == o.balance && mute == o.mute && solo == o.solo
             && out == o.out && name == o.name; }
};

struct BusSettings
{
    float gainDb = 0.0f;
    bool mute = false;
    OutType type = OutType::audio;
    int outFirst = 1;                // Audio Out: the bus plays on device outputs (outFirst, outFirst + 1)
    NetDest dest;                    // Net Out: where the stream goes
    std::string name;

    bool operator== (const BusSettings& o) const noexcept
    { return gainDb == o.gainDb && mute == o.mute && type == o.type && outFirst == o.outFirst && dest == o.dest && name == o.name; }
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
    std::array<StripSettings, kStrips> strips;
    std::array<BusSettings, kBuses> buses;
    std::array<AppTarget, kAppSlots> apps;
    float masterDb = 0.0f;
    bool masterMute = false;
    OutType masterType = OutType::audio;
    int masterOut = 1;               // Audio Out: master plays on device outputs (masterOut, masterOut + 1)
    NetDest masterDest;              // Net Out

    /** Out of the box: strips 1-4 take network pairs 1-4, strips 5-8 the four app slots, strips 9-32 the audio input
        device's stereo pairs 1-24. Every strip goes to the master; bus n defaults to device outputs (2n-1, 2n). */
    MixerSettings()
    {
        for (int i = 0; i < kStrips; ++i)
        {
            auto& s = strips[(size_t) i];
            if (i < kNetPairs)                       s.src = { SourceType::net, i };
            else if (i < kNetPairs + kAppSlots)      s.src = { SourceType::app, i - kNetPairs };
            else                                     s.src = { SourceType::input, i - kNetPairs - kAppSlots };
        }
        for (int b = 0; b < kBuses; ++b) buses[(size_t) b].outFirst = clampOutFirst (2 * b + 1);
    }

    void sanitize()
    {
        for (auto& s : strips)
        {
            s.src.sanitize();
            s.gainDb = clampGainDb (s.gainDb); s.balance = clampBalance (s.balance);
            s.out = clampStripOut (s.out);
            s.name = sanitizeStripName (s.name);
        }
        for (auto& b : buses)
        {
            b.gainDb = clampGainDb (b.gainDb);
            if (b.type != OutType::audio && b.type != OutType::net) b.type = OutType::audio;
            b.outFirst = clampOutFirst (b.outFirst);
            b.dest.sanitize();
            b.name = sanitizeStripName (b.name);
        }
        for (auto& a : apps)
        {
            if (! isValidBundleId (a.bundleId)) a = AppTarget();
            a.name = sanitizeText (a.name, kMaxAppNameLength);
        }
        masterDb = clampGainDb (masterDb);
        if (masterType != OutType::audio && masterType != OutType::net) masterType = OutType::audio;
        masterOut = clampOutFirst (masterOut);
        masterDest.sanitize();
    }

    bool operator== (const MixerSettings& o) const noexcept
    { return strips == o.strips && buses == o.buses && apps == o.apps && masterDb == o.masterDb && masterMute == o.masterMute
             && masterType == o.masterType && masterOut == o.masterOut && masterDest == o.masterDest; }
};
}
