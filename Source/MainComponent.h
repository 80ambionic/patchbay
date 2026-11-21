#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Midi/MidiTopology.h"
#include "Midi/MidiRouter.h"
#include "Midi/VirtualBuses.h"
#include "Midi/NetworkMidiSession.h"
#include "Net/NetworkEngine.h"
#include "UI/MixerPage.h"
#include "UI/NetworkPage.h"
#include "UI/PatchbayCanvas.h"
#include "StateStore.h"

namespace pb
{
class MainComponent : public juce::Component
{
public:
    MainComponent();
    void resized() override;

private:
    // Order matters: router and buses need the topology's CoreMIDI client; the store snapshots the
    // router and canvas, so it is declared last and destroyed first (its destructor flushes a pending save).
    MidiTopology topology;
    MidiRouter   router { topology.getClient() };
    VirtualBuses buses  { topology.getClient() };
    NetworkMidiSession midiNet;            // likewise declared before tabs
    net::NetworkEngine netEngine;          // declared before tabs: the page uses it, so it must outlive the page

    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    PatchbayCanvas* patchbay = nullptr;   // owned by tabs
    NetworkPage*    networkPage = nullptr; // owned by tabs
    MixerPage*      mixerPage = nullptr;   // owned by tabs
    std::unique_ptr<StateStore> store;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
}
