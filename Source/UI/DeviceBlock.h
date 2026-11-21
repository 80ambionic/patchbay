#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Midi/MidiTopology.h"
#include <optional>

namespace pb
{
class PatchbayCanvas;

struct PortRef { int32_t uid = 0; bool isOutput = false; };

/** One draggable device box. Inputs on the left, outputs on the right.
    Drag the header to move; drag from a port to start a cable. */
class DeviceBlock : public juce::Component
{
public:
    static constexpr int margin = 12, bodyWidth = 260, headerH = 26, rowH = 24, pad = 6;
    static constexpr int totalWidth = bodyWidth + 2 * margin;
    static int heightFor (const MidiDevice&);

    DeviceBlock (PatchbayCanvas& owner, MidiDevice d);

    const MidiDevice& getDevice() const noexcept { return device; }
    std::optional<juce::Point<float>> portCentreInParent (PortRef) const;
    std::optional<PortRef> portAt (juce::Point<float> local) const;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> body() const { return getLocalBounds().toFloat().reduced ((float) margin, 0.0f); }
    juce::Point<float> portCentreLocal (size_t index, bool isOutput) const;

    PatchbayCanvas& owner;
    MidiDevice device;
    juce::ComponentDragger dragger;
    enum class Mode { none, move, cable } mode = Mode::none;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceBlock)
};
}
