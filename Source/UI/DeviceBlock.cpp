#include "DeviceBlock.h"
#include "PatchbayCanvas.h"

namespace pb
{
static constexpr float hitRadius = 14.0f;   // generous for touch

int DeviceBlock::heightFor (const MidiDevice& d)
{
    const auto rows = (int) std::max ({ d.inputs.size(), d.outputs.size(), (size_t) 1 });
    return headerH + 2 * pad + rows * rowH;
}

DeviceBlock::DeviceBlock (PatchbayCanvas& o, MidiDevice d) : owner (o), device (std::move (d))
{
    setSize (totalWidth, heightFor (device));
    setOpaque (false);
}

juce::Point<float> DeviceBlock::portCentreLocal (size_t index, bool isOutput) const
{
    const auto b = body();
    return { isOutput ? b.getRight() : b.getX(),
             (float) (headerH + pad) + (float) index * (float) rowH + (float) rowH * 0.5f };
}

std::optional<PortRef> DeviceBlock::portAt (juce::Point<float> p) const
{
    for (size_t i = 0; i < device.inputs.size(); ++i)
        if (portCentreLocal (i, false).getDistanceFrom (p) <= hitRadius)
            return PortRef { device.inputs[i].uid, false };
    for (size_t i = 0; i < device.outputs.size(); ++i)
        if (portCentreLocal (i, true).getDistanceFrom (p) <= hitRadius)
            return PortRef { device.outputs[i].uid, true };
    return std::nullopt;
}

std::optional<juce::Point<float>> DeviceBlock::portCentreInParent (PortRef r) const
{
    const auto& list = r.isOutput ? device.outputs : device.inputs;
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].uid == r.uid)
            return portCentreLocal (i, r.isOutput) + getPosition().toFloat();
    return std::nullopt;
}

void DeviceBlock::paint (juce::Graphics& g)
{
    auto b = body();
    const bool off = device.offline;

    g.setColour (juce::Colour (0xff2a2e35));
    g.fillRoundedRectangle (b, 6.0f);

    auto head = b.removeFromTop ((float) headerH);
    juce::Path hp;
    hp.addRoundedRectangle (head.getX(), head.getY(), head.getWidth(), head.getHeight(),
                            6.0f, 6.0f, true, true, false, false);
    const auto headCol = device.uid == kBusGroupUid ? juce::Colour (0xff8e6bbf)      // PatchBay's own buses
                                                    : juce::Colour (0xff5b9bd5);
    g.setColour (off ? juce::Colour (0xff5a5f68) : headCol);
    g.fillPath (hp);

    g.setColour (juce::Colours::white);
    g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    g.drawText (device.name, head.reduced (10.0f, 0.0f), juce::Justification::centredLeft, true);

    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    const float labelW = (float) bodyWidth * 0.5f - 18.0f;

    auto drawPort = [&] (const MidiPort& p, size_t i, bool isOut)
    {
        const auto c = portCentreLocal (i, isOut);
        const bool linked = owner.isPortConnected ({ p.uid, isOut });
        const auto col = isOut ? juce::Colour (0xfff0a35a) : juce::Colour (0xff6ccf8e);

        g.setColour (col.withAlpha (linked ? 1.0f : 0.45f));
        g.fillEllipse (c.x - 5.0f, c.y - 5.0f, 10.0f, 10.0f);
        g.setColour (col);
        g.drawEllipse (c.x - 5.0f, c.y - 5.0f, 10.0f, 10.0f, 1.5f);

        g.setColour (off ? juce::Colour (0xff8a8f98) : juce::Colour (0xffd8dce2));
        if (isOut) g.drawText (p.name, juce::Rectangle<float> (c.x - 10.0f - labelW, c.y - rowH * 0.5f, labelW, (float) rowH),
                               juce::Justification::centredRight, true);
        else       g.drawText (p.name, juce::Rectangle<float> (c.x + 10.0f, c.y - rowH * 0.5f, labelW, (float) rowH),
                               juce::Justification::centredLeft, true);
    };

    for (size_t i = 0; i < device.inputs.size();  ++i) drawPort (device.inputs[i],  i, false);
    for (size_t i = 0; i < device.outputs.size(); ++i) drawPort (device.outputs[i], i, true);
}

void DeviceBlock::mouseDown (const juce::MouseEvent& e)
{
    if (auto p = portAt (e.position))
    {
        mode = Mode::cable;
        owner.beginCable (*p, e.getEventRelativeTo (&owner).position);
    }
    else if (e.position.y < (float) headerH)
    {
        mode = Mode::move;
        dragger.startDraggingComponent (this, e);
        toFront (false);
    }
    else mode = Mode::none;
}

void DeviceBlock::mouseDrag (const juce::MouseEvent& e)
{
    if (mode == Mode::cable)      owner.updateCable (e.getEventRelativeTo (&owner).position);
    else if (mode == Mode::move) { dragger.dragComponent (this, e, nullptr); owner.blockMoved (*this); }
}

void DeviceBlock::mouseUp (const juce::MouseEvent& e)
{
    if (mode == Mode::cable) owner.endCable (e.getEventRelativeTo (&owner).position);
    mode = Mode::none;
}
}
