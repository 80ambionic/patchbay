#pragma once
#include <juce_core/juce_core.h>
#include <map>
#include <vector>
#include "Midi/NetMidiTypes.h"
#include "Midi/RouteGraph.h"
#include "Mixer/MixerSettings.h"

namespace pb
{
struct BlockPos
{
    int x = 0, y = 0;
    bool operator== (const BlockPos& o) const noexcept { return x == o.x && y == o.y; }
};

/** Network page settings. "Listening" is deliberately NOT persisted: the app never opens a network port
    on launch without the user pressing Listen. */
struct NetworkSettings
{
    static constexpr int kMinPort = 1024, kMaxPort = 65535, kMaxChannels = 8, kMinBuffer = 2, kMaxBuffer = 24;

    int port = 9000;
    int channels = 2;
    int bufferPackets = 4;

    bool operator== (const NetworkSettings& o) const noexcept
    { return port == o.port && channels == o.channels && bufferPackets == o.bufferPackets; }
};

struct PatchbayState
{
    std::vector<Route> routes;
    std::map<int32_t, BlockPos> positions;   // device uid -> block top-left on the canvas
    NetworkSettings network;
    MidiNetSettings midiNet;
    MixerSettings mixer;
};

/** The mixer section on its own (used by the state file and by mixer presets). mixerFromVar fills `out` with defaults
    first, then whatever valid fields the value holds; it returns false (leaving defaults) if the value is not a layout-2
    mixer object, e.g. a file written by the 24-strip mixer of an earlier version. */
juce::var mixerToVar (const MixerSettings&);
bool mixerFromVar (const juce::var&, MixerSettings& out);

/** Versioned JSON for PatchbayState. Parsing is defensive: a hand-edited, truncated or foreign file yields
    whatever valid entries it contains (or an empty state), never garbage entries or a crash. */
juce::String toJson (const PatchbayState&);
PatchbayState fromJson (const juce::String&);

/** Writes via a temp file + rename, so a crash mid-write leaves the previous file intact. */
bool saveAtomically (const juce::File&, const PatchbayState&);
PatchbayState loadFile (const juce::File&);    // empty state if missing/unreadable
}
