#include "NetworkPage.h"

namespace pb
{
NetworkPage::NetworkPage (net::NetworkEngine& e, NetworkMidiSession& m) : audioLane (e), midiLane (m)
{
    content.audio = &audioLane;
    content.midi = &midiLane;
    content.addAndMakeVisible (audioLane);
    content.addAndMakeVisible (midiLane);
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);
}

void NetworkPage::resized()
{
    viewport.setBounds (getLocalBounds());
    const int w = juce::jmax (Content::kMinWidth, viewport.getMaximumVisibleWidth());
    content.setSize (w, Content::kAudioHeight + Content::kMidiHeight);
}

void NetworkPage::Content::resized()
{
    auto r = getLocalBounds();
    if (audio != nullptr) audio->setBounds (r.removeFromTop (kAudioHeight));
    if (midi != nullptr)  midi->setBounds (r);
}
}
