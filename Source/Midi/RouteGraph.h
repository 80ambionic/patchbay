#pragma once
// Pure C++ (no CoreMIDI/JUCE) so the routing rules can be unit-tested on any platform.
#include <cstdint>
#include <limits>
#include <set>
#include <vector>

namespace pb
{
struct Route
{
    int32_t srcUid = 0;   // a device OUT (CoreMIDI source)
    int32_t dstUid = 0;   // a device IN  (CoreMIDI destination)
    bool operator== (const Route& o) const noexcept { return srcUid == o.srcUid && dstUid == o.dstUid; }
};

// ---- PatchBay's own virtual buses -------------------------------------------------------------
// Each bus is a pass-through: other apps (or the patchbay) send into the bus INPUT (a CoreMIDI
// destination); whatever arrives is re-emitted from the bus OUTPUT (a CoreMIDI source), which can be
// cabled to any hardware/virtual input and is visible to other apps as a MIDI source.
// UIDs are fixed so saved routes survive relaunches.
constexpr int     kNumBuses      = 4;
constexpr int32_t kBusInBase     = 0x50420100;
constexpr int32_t kBusOutBase    = 0x50420200;
constexpr int32_t kVirtualGroupUid = std::numeric_limits<int32_t>::min();
constexpr int32_t kBusGroupUid     = std::numeric_limits<int32_t>::min() + 1;

constexpr bool    isBusInput  (int32_t uid) noexcept { return uid >= kBusInBase  && uid < kBusInBase  + kNumBuses; }
constexpr bool    isBusOutput (int32_t uid) noexcept { return uid >= kBusOutBase && uid < kBusOutBase + kNumBuses; }
constexpr bool    isBusUid    (int32_t uid) noexcept { return isBusInput (uid) || isBusOutput (uid); }
constexpr int32_t busOutputFor(int32_t inUid) noexcept { return inUid - kBusInBase + kBusOutBase; }

/** Would adding src->dst create a feedback loop through the buses?
    A loop can only close through a bus (input -> output pass-through), so we follow
    dst -> (its bus output) -> routes from that output -> ... and see whether we get back to src.
    Loops that close through external hardware or other apps can't be detected here. */
inline bool wouldCreateLoop (const std::vector<Route>& routes, int32_t src, int32_t dst)
{
    std::set<int32_t> visited;
    std::vector<int32_t> stack { dst };

    while (! stack.empty())
    {
        const auto node = stack.back();
        stack.pop_back();

        if (! isBusInput (node)) continue;      // only buses re-emit what they receive

        const auto out = busOutputFor (node);
        if (out == src) return true;
        if (! visited.insert (out).second) continue;

        for (const auto& r : routes)
            if (r.srcUid == out)
                stack.push_back (r.dstUid);
    }
    return false;
}
}
