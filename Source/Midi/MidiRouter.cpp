#include "MidiRouter.h"
#include <algorithm>

namespace pb
{
namespace
{
MIDIEndpointRef resolve (int32_t uid)
{
    MIDIObjectRef obj = 0;
    MIDIObjectType type {};
    if (MIDIObjectFindByUniqueID ((MIDIUniqueID) uid, &obj, &type) != noErr || obj == 0)
        return 0;

    switch (type)
    {
        case kMIDIObjectType_Source:
        case kMIDIObjectType_Destination:
        case kMIDIObjectType_ExternalSource:
        case kMIDIObjectType_ExternalDestination: return (MIDIEndpointRef) obj;
        default: return 0;
    }
}
}

// NOTE: MIDIInputPortCreate / MIDISend are the legacy (MIDI 1.0 packet list) APIs. They are
// deprecated in newer SDKs (warnings only). Migrate to MIDIInputPortCreateWithProtocol / MIDISendEventList
// when you want MIDI 2.0 / UMP.
MidiRouter::MidiRouter (MIDIClientRef client)
{
    MIDIInputPortCreate (client, CFSTR ("PatchBay In"), &MidiRouter::readProc, this, &inPort);
    MIDIOutputPortCreate (client, CFSTR ("PatchBay Out"), &outPort);
}

MidiRouter::~MidiRouter()
{
    if (inPort  != 0) MIDIPortDispose (inPort);    // stops callbacks before we go away
    if (outPort != 0) MIDIPortDispose (outPort);
}

bool MidiRouter::hasRoute (int32_t s, int32_t d) const
{
    return std::find (routes.begin(), routes.end(), Route { s, d }) != routes.end();
}

bool MidiRouter::isConnected (int32_t uid) const
{
    return std::any_of (routes.begin(), routes.end(),
                        [uid] (const Route& r) { return r.srcUid == uid || r.dstUid == uid; });
}

bool MidiRouter::addRoute (int32_t s, int32_t d)
{
    if (hasRoute (s, d) || wouldCreateLoop (routes, s, d)) return false;
    routes.push_back ({ s, d });
    rebuild();
    if (onChanged) onChanged();
    return true;
}

void MidiRouter::removeRoute (int32_t s, int32_t d)
{
    routes.erase (std::remove (routes.begin(), routes.end(), Route { s, d }), routes.end());
    rebuild();
    if (onChanged) onChanged();
}

void MidiRouter::setRoutes (std::vector<Route> newRoutes)
{
    routes.clear();
    for (const auto& r : newRoutes)                 // re-validate: a hand-edited file must not create loops
        if (! hasRoute (r.srcUid, r.dstUid) && ! wouldCreateLoop (routes, r.srcUid, r.dstUid))
            routes.push_back (r);
    rebuild();
}

void MidiRouter::rebuild()
{
    auto t = std::make_shared<Table>();
    std::set<MIDIEndpointRef> wanted;

    for (const auto& r : routes)
    {
        const auto s = resolve (r.srcUid), d = resolve (r.dstUid);
        if (s == 0 || d == 0) continue;           // offline right now; the route is kept and revives on replug
        t->map[s].push_back (d);
        wanted.insert (s);
    }

    { juce::SpinLock::ScopedLockType sl (tableLock); table = t; }

    for (auto it = connected.begin(); it != connected.end();)
    {
        if (wanted.count (*it) == 0) { MIDIPortDisconnectSource (inPort, *it); it = connected.erase (it); }
        else ++it;
    }
    for (auto s : wanted)
        if (connected.count (s) == 0
            && MIDIPortConnectSource (inPort, s, (void*) (uintptr_t) s) == noErr)
            connected.insert (s);
}

// Runs on a CoreMIDI thread.
void MidiRouter::readProc (const MIDIPacketList* list, void* refCon, void* srcRefCon)
{
    auto* self = static_cast<MidiRouter*> (refCon);

    std::shared_ptr<const Table> t;
    { juce::SpinLock::ScopedLockType sl (self->tableLock); t = self->table; }
    if (t == nullptr) return;

    const auto it = t->map.find ((MIDIEndpointRef) (uintptr_t) srcRefCon);
    if (it == t->map.end()) return;

    for (auto dst : it->second)
        MIDISend (self->outPort, dst, list);
}
}
