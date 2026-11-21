#pragma once
#include <juce_core/juce_core.h>
#include <CoreMIDI/CoreMIDI.h>
#include <array>
#include "RouteGraph.h"

namespace pb
{
/** Creates kNumBuses virtual pass-through buses ("PatchBay Bus 1..N").
    Other apps see each as both a MIDI destination and a MIDI source. */
class VirtualBuses
{
public:
    explicit VirtualBuses (MIDIClientRef client);
    ~VirtualBuses();

private:
    struct Bus { MIDIEndpointRef in = 0, out = 0; };
    struct Ctx { VirtualBuses* self = nullptr; int index = 0; };   // per-destination callback context

    static void readProc (const MIDIPacketList*, void* refCon, void* srcRefCon);   // CoreMIDI thread

    std::array<Bus, kNumBuses> buses;
    std::array<Ctx, kNumBuses> ctx;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VirtualBuses)
};
}
