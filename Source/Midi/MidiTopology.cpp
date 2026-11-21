#include "MidiTopology.h"

namespace pb
{
namespace
{
juce::String getString (MIDIObjectRef obj, CFStringRef prop)
{
    CFStringRef s = nullptr;
    if (MIDIObjectGetStringProperty (obj, prop, &s) == noErr && s != nullptr)
    {
        auto result = juce::String::fromCFString (s);
        CFRelease (s);
        return result;
    }
    return {};
}

int32_t getInt (MIDIObjectRef obj, CFStringRef prop, int32_t fallback = 0)
{
    SInt32 v = fallback;
    MIDIObjectGetIntegerProperty (obj, prop, &v);
    return (int32_t) v;
}

MidiPort makePort (MIDIEndpointRef ep, bool isOutput)
{
    MidiPort p;
    p.ref = ep;
    p.uid = getInt (ep, kMIDIPropertyUniqueID);
    p.name = getString (ep, kMIDIPropertyName);
    if (p.name.isEmpty()) p.name = getString (ep, kMIDIPropertyDisplayName);
    p.isOutput = isOutput;
    return p;
}
}

MidiTopology::MidiTopology()
{
    MIDIClientCreate (CFSTR ("PatchBay"), &MidiTopology::notifyProc, this, &client);
    rescan();
}

MidiTopology::~MidiTopology()
{
    cancelPendingUpdate();
    if (client != 0) MIDIClientDispose (client);
}

void MidiTopology::notifyProc (const MIDINotification* m, void* ctx)
{
    switch (m->messageID)
    {
        case kMIDIMsgSetupChanged:
        case kMIDIMsgObjectAdded:
        case kMIDIMsgObjectRemoved:
            static_cast<MidiTopology*> (ctx)->triggerAsyncUpdate();   // coalesces bursts, hops to message thread
            break;
        default: break;
    }
}

void MidiTopology::handleAsyncUpdate()
{
    rescan();
    if (onChanged) onChanged();
}

void MidiTopology::rescan()
{
    std::vector<MidiDevice> found;

    // 1) Real/external devices: device > entity > endpoints
    const auto numDevices = MIDIGetNumberOfDevices();
    for (ItemCount i = 0; i < numDevices; ++i)
    {
        MIDIDeviceRef dev = MIDIGetDevice (i);
        if (dev == 0) continue;

        MidiDevice d;
        d.uid = getInt (dev, kMIDIPropertyUniqueID);
        d.name = getString (dev, kMIDIPropertyName);
        d.offline = getInt (dev, kMIDIPropertyOffline) != 0;

        const auto numEntities = MIDIDeviceGetNumberOfEntities (dev);
        for (ItemCount e = 0; e < numEntities; ++e)
        {
            MIDIEntityRef ent = MIDIDeviceGetEntity (dev, e);
            for (ItemCount s = 0, n = MIDIEntityGetNumberOfSources (ent); s < n; ++s)
                d.outputs.push_back (makePort (MIDIEntityGetSource (ent, s), true));
            for (ItemCount s = 0, n = MIDIEntityGetNumberOfDestinations (ent); s < n; ++s)
                d.inputs.push_back (makePort (MIDIEntityGetDestination (ent, s), false));
        }

        if (! d.inputs.empty() || ! d.outputs.empty())
            found.push_back (std::move (d));
    }

    // 2) Standalone endpoints with no owning entity = virtual ports (IAC-style, other apps' ports)
    MidiDevice virt;
    virt.uid = kVirtualGroupUid;
    virt.name = "Virtual Ports";

    MidiDevice buses;                       // PatchBay's own pass-through buses get their own block
    buses.uid = kBusGroupUid;
    buses.name = "PatchBay Buses";

    for (ItemCount i = 0, n = MIDIGetNumberOfSources(); i < n; ++i)
    {
        auto src = MIDIGetSource (i);
        MIDIEntityRef ent = 0;
        if (MIDIEndpointGetEntity (src, &ent) != noErr || ent == 0)
        {
            auto port = makePort (src, true);
            (isBusUid (port.uid) ? buses : virt).outputs.push_back (std::move (port));
        }
    }
    for (ItemCount i = 0, n = MIDIGetNumberOfDestinations(); i < n; ++i)
    {
        auto dst = MIDIGetDestination (i);
        MIDIEntityRef ent = 0;
        if (MIDIEndpointGetEntity (dst, &ent) != noErr || ent == 0)
        {
            auto port = makePort (dst, false);
            (isBusUid (port.uid) ? buses : virt).inputs.push_back (std::move (port));
        }
    }

    if (! virt.inputs.empty() || ! virt.outputs.empty())
        found.push_back (std::move (virt));

    if (! buses.inputs.empty() || ! buses.outputs.empty())
        found.insert (found.begin(), std::move (buses));    // first, so its default slot is stable

    devices = std::move (found);
}
}
