#pragma once
#include <juce_core/juce_core.h>
#include <CoreMIDI/CoreMIDI.h>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>
#include <functional>
#include "RouteGraph.h"

namespace pb
{
/** The patchbay's actual routing: forwards packets from sources to destinations.
    Routes are stored by UID so they survive unplug/replug. Mutating calls: message thread only. */
class MidiRouter
{
public:
    explicit MidiRouter (MIDIClientRef client);
    ~MidiRouter();

    /** Fires after any change to the route list (add/remove/set). Message thread. */
    std::function<void()> onChanged;

    /** Returns false if the route already exists or would close a feedback loop through the buses. */
    bool addRoute (int32_t srcUid, int32_t dstUid);
    void setRoutes (std::vector<Route> newRoutes);   // used when restoring saved state
    void removeRoute (int32_t srcUid, int32_t dstUid);
    bool hasRoute (int32_t srcUid, int32_t dstUid) const;
    bool isConnected (int32_t portUid) const;
    const std::vector<Route>& getRoutes() const noexcept { return routes; }

    /** Re-resolve UIDs to live endpoint refs (call after the topology changes). */
    void refresh() { rebuild(); }

private:
    struct Table { std::unordered_map<MIDIEndpointRef, std::vector<MIDIEndpointRef>> map; };

    void rebuild();
    static void readProc (const MIDIPacketList*, void* refCon, void* srcRefCon);

    MIDIPortRef inPort = 0, outPort = 0;
    std::vector<Route> routes;
    std::set<MIDIEndpointRef> connected;

    juce::SpinLock tableLock;                 // held only to copy a shared_ptr
    std::shared_ptr<const Table> table;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiRouter)
};
}
