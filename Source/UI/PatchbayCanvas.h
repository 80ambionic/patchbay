#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <map>
#include <optional>
#include "DeviceBlock.h"
#include "Midi/MidiTopology.h"
#include "Midi/MidiRouter.h"
#include "StateFile.h"

namespace pb
{
/** MIDI Studio-style canvas: device blocks + cables. Click-drag port to port to connect;
    right-click or double-click a cable to remove it. */
class PatchbayCanvas : public juce::Component
{
public:
    PatchbayCanvas (MidiTopology&, MidiRouter&);

    void rebuild();   // after any topology change

    /** Saved block positions (device uid -> top-left). Set before the first rebuild() to restore a layout. */
    std::map<int32_t, BlockPos> getPositions() const
    {
        std::map<int32_t, BlockPos> out;
        for (const auto& [uid, p] : savedPositions) out[uid] = { p.x, p.y };
        return out;
    }
    void setPositions (const std::map<int32_t, BlockPos>& in)
    {
        savedPositions.clear();
        for (const auto& [uid, p] : in) savedPositions[uid] = { p.x, p.y };
    }
    std::function<void()> onLayoutChanged;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    // Called by DeviceBlock
    void beginCable (PortRef from, juce::Point<float> pos);
    void updateCable (juce::Point<float> pos);
    void endCable (juce::Point<float> pos);
    void blockMoved (DeviceBlock&);
    bool isPortConnected (PortRef r) const { return router.isConnected (r.uid); }

private:
    std::optional<juce::Point<float>> portCentre (PortRef) const;
    std::optional<Route> cableAt (juce::Point<float>) const;
    void removeCableAt (juce::Point<float>);

    MidiTopology& topology;
    MidiRouter& router;
    juce::OwnedArray<DeviceBlock> blocks;
    std::map<int32_t, juce::Point<int>> savedPositions;   // device uid -> position, survives rebuilds

    struct DragState { PortRef from; juce::Point<float> pos; };
    std::optional<DragState> drag;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PatchbayCanvas)
};
}
