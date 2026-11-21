#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <CoreMIDI/CoreMIDI.h>
#include <functional>
#include "RouteGraph.h"
#include <vector>

namespace pb
{
// CoreMIDI naming: a "source" emits MIDI (a device's OUT), a "destination" receives it (a device's IN).
struct MidiPort
{
    int32_t         uid = 0;      // kMIDIPropertyUniqueID - stable across replug, use this for saved routes
    MIDIEndpointRef ref = 0;      // volatile handle - re-resolved on every topology change
    juce::String    name;
    bool            isOutput = false;
};

struct MidiDevice
{
    int32_t      uid = 0;
    juce::String name;
    bool         offline = false;
    std::vector<MidiPort> inputs;    // CoreMIDI destinations
    std::vector<MidiPort> outputs;   // CoreMIDI sources
};

/** Reads the CoreMIDI setup (devices > entities > endpoints, plus standalone/virtual endpoints)
    and rescans when hardware or virtual ports appear/disappear. Message-thread only. */
class MidiTopology : private juce::AsyncUpdater
{
public:
    MidiTopology();
    ~MidiTopology() override;

    std::function<void()> onChanged;

    void rescan();
    const std::vector<MidiDevice>& getDevices() const noexcept { return devices; }
    MIDIClientRef getClient() const noexcept { return client; }

private:
    void handleAsyncUpdate() override;
    static void notifyProc (const MIDINotification*, void* ctx);

    MIDIClientRef client = 0;
    std::vector<MidiDevice> devices;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiTopology)
};
}
