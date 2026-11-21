#include "VirtualBuses.h"

namespace pb
{
VirtualBuses::VirtualBuses (MIDIClientRef client)
{
    for (int i = 0; i < kNumBuses; ++i)
    {
        const auto name = juce::String ("PatchBay Bus ") + juce::String (i + 1);
        auto cfName = name.toCFString();

        // Legacy MIDI 1.0 creators (deprecated in recent SDKs, still supported).
        // refCon is per-destination, so each bus gets its own context saying which output to re-emit on.
        ctx[(size_t) i] = { this, i };
        MIDIDestinationCreate (client, cfName, &VirtualBuses::readProc, &ctx[(size_t) i], &buses[(size_t) i].in);
        MIDISourceCreate      (client, cfName, &buses[(size_t) i].out);
        CFRelease (cfName);

        // Fixed UIDs so saved routes survive relaunch.
        const auto okIn  = MIDIObjectSetIntegerProperty (buses[(size_t) i].in,  kMIDIPropertyUniqueID, (SInt32) (kBusInBase  + i));
        const auto okOut = MIDIObjectSetIntegerProperty (buses[(size_t) i].out, kMIDIPropertyUniqueID, (SInt32) (kBusOutBase + i));
        jassert (okIn == noErr && okOut == noErr);
        juce::ignoreUnused (okIn, okOut);
    }
}

VirtualBuses::~VirtualBuses()
{
    // Dispose destinations first so no callback can fire while sources are going away.
    for (auto& b : buses) if (b.in  != 0) MIDIEndpointDispose (b.in);
    for (auto& b : buses) if (b.out != 0) MIDIEndpointDispose (b.out);
}

// Runs on a CoreMIDI thread. Re-emit everything received on bus N from bus N's source endpoint;
// CoreMIDI then delivers it to every client (including our router) connected to that source.
void VirtualBuses::readProc (const MIDIPacketList* list, void* refCon, void*)
{
    const auto* c = static_cast<const Ctx*> (refCon);
    MIDIReceived (c->self->buses[(size_t) c->index].out, list);
}
}
