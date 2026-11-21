#include "PatchbayCanvas.h"
#include <algorithm>
#include <cmath>

namespace pb
{
namespace
{
struct Cable
{
    juce::Point<float> a, c1, c2, b;

    Cable (juce::Point<float> from, juce::Point<float> to) : a (from), b (to)
    {
        const float dx = std::max (60.0f, std::abs (to.x - from.x) * 0.5f);
        c1 = { from.x + dx, from.y };
        c2 = { to.x - dx, to.y };
    }

    juce::Point<float> at (float t) const
    {
        const float u = 1.0f - t;
        return a * (u * u * u) + c1 * (3 * u * u * t) + c2 * (3 * u * t * t) + b * (t * t * t);
    }

    juce::Path toPath() const
    {
        juce::Path p;
        p.startNewSubPath (a);
        p.cubicTo (c1, c2, b);
        return p;
    }
};
}

PatchbayCanvas::PatchbayCanvas (MidiTopology& t, MidiRouter& r) : topology (t), router (r) {}

void PatchbayCanvas::rebuild()
{
    blocks.clear();

    std::vector<juce::Rectangle<int>> taken;       // rectangles already occupied, for placing new devices
    std::vector<DeviceBlock*> unplaced;

    for (const auto& dev : topology.getDevices())
    {
        auto* blk = blocks.add (new DeviceBlock (*this, dev));
        addAndMakeVisible (blk);

        if (auto it = savedPositions.find (dev.uid); it != savedPositions.end())
        {
            blk->setTopLeftPosition (it->second);
            taken.push_back (blk->getBounds());
        }
        else unplaced.push_back (blk);
    }

    // New devices: first free slot in a 3-column grid that doesn't overlap anything already placed.
    for (auto* blk : unplaced)
    {
        const int w = blk->getWidth(), h = blk->getHeight();
        for (int y = 30;; y += 20)
        {
            bool placed = false;
            for (int col = 0; col < 3 && ! placed; ++col)
            {
                const juce::Rectangle<int> r (20 + col * (DeviceBlock::totalWidth + 20), y, w, h);
                const bool free = std::none_of (taken.begin(), taken.end(),
                                                [&] (const auto& t) { return t.expanded (8).intersects (r); });
                if (free)
                {
                    blk->setBounds (r);
                    taken.push_back (r);
                    savedPositions[blk->getDevice().uid] = r.getPosition();
                    placed = true;
                }
            }
            if (placed) break;
        }
    }

    if (onLayoutChanged && ! unplaced.empty()) onLayoutChanged();
    repaint();
}

std::optional<juce::Point<float>> PatchbayCanvas::portCentre (PortRef r) const
{
    for (auto* b : blocks)
        if (auto c = b->portCentreInParent (r)) return c;
    return std::nullopt;
}

void PatchbayCanvas::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff15171b));

    for (const auto& r : router.getRoutes())
    {
        auto a = portCentre ({ r.srcUid, true });
        auto b = portCentre ({ r.dstUid, false });
        if (! a || ! b) continue;     // an endpoint is offline; route is kept

        g.setColour (juce::Colour::fromHSV ((float) ((unsigned) r.srcUid % 360u) / 360.0f, 0.55f, 0.95f, 1.0f));
        g.strokePath (Cable (*a, *b).toPath(), juce::PathStrokeType (3.0f));
    }

    if (drag)
    {
        if (auto start = portCentre (drag->from))
        {
            const auto s = drag->from.isOutput ? *start : drag->pos;
            const auto e = drag->from.isOutput ? drag->pos : *start;
            g.setColour (juce::Colours::white.withAlpha (0.7f));
            g.strokePath (Cable (s, e).toPath(), juce::PathStrokeType (2.0f));
        }
    }
}

void PatchbayCanvas::beginCable (PortRef from, juce::Point<float> pos) { drag = DragState { from, pos }; repaint(); }
void PatchbayCanvas::updateCable (juce::Point<float> pos)              { if (drag) { drag->pos = pos; repaint(); } }

void PatchbayCanvas::endCable (juce::Point<float> pos)
{
    if (! drag) return;
    const auto from = drag->from;
    drag.reset();

    for (auto* b : blocks)
    {
        if (auto target = b->portAt (b->getLocalPoint (this, pos)))
        {
            if (target->isOutput != from.isOutput)     // must connect an OUT to an IN
            {
                const auto src = from.isOutput ? from.uid : target->uid;
                const auto dst = from.isOutput ? target->uid : from.uid;
                router.addRoute (src, dst);
            }
            break;
        }
    }
    repaint();
}

void PatchbayCanvas::blockMoved (DeviceBlock& b)
{
    savedPositions[b.getDevice().uid] = b.getPosition();
    if (onLayoutChanged) onLayoutChanged();
    repaint();
}

std::optional<Route> PatchbayCanvas::cableAt (juce::Point<float> p) const
{
    for (const auto& r : router.getRoutes())
    {
        auto a = portCentre ({ r.srcUid, true });
        auto b = portCentre ({ r.dstUid, false });
        if (! a || ! b) continue;

        const Cable c (*a, *b);
        for (int i = 0; i <= 40; ++i)
            if (c.at ((float) i / 40.0f).getDistanceFrom (p) < 8.0f)
                return r;
    }
    return std::nullopt;
}

void PatchbayCanvas::removeCableAt (juce::Point<float> p)
{
    if (auto r = cableAt (p)) { router.removeRoute (r->srcUid, r->dstUid); repaint(); }
}

void PatchbayCanvas::mouseDown (const juce::MouseEvent& e)        { if (e.mods.isPopupMenu()) removeCableAt (e.position); }
void PatchbayCanvas::mouseDoubleClick (const juce::MouseEvent& e) { removeCableAt (e.position); }
}
