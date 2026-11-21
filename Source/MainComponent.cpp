#include "MainComponent.h"

namespace pb
{
MainComponent::MainComponent()
{
    const auto bg = juce::Colour (0xff15171b);

    patchbay = new PatchbayCanvas (topology, router);
    tabs.addTab ("MIDI Patchbay", bg, patchbay, true);
    networkPage = new NetworkPage (netEngine, midiNet);
    tabs.addTab ("Network", bg, networkPage, true);
    mixerPage = new MixerPage (netEngine);
    tabs.addTab ("Mixer", bg, mixerPage, true);
    addAndMakeVisible (tabs);

    // Restore the saved session before the first layout, then start auto-saving.
    store = std::make_unique<StateStore> (StateStore::defaultFile(), [this]
    {
        PatchbayState st;
        st.routes = router.getRoutes();
        st.positions = patchbay->getPositions();
        st.network = networkPage->audio().getSettings();
        st.midiNet = networkPage->midi().getSettings();
        st.mixer = mixerPage->getSettings();
        return st;
    });

    const auto saved = store->load();
    patchbay->setPositions (saved.positions);
    router.setRoutes (saved.routes);
    networkPage->audio().setSettings (saved.network);
    networkPage->midi().setSettings (saved.midiNet);
    mixerPage->setSettings (saved.mixer);

    router.onChanged = [this] { store->markDirty(); };
    patchbay->onLayoutChanged = [this] { store->markDirty(); };
    networkPage->audio().onSettingsChanged = [this] { store->markDirty(); };
    networkPage->midi().onSettingsChanged  = [this] { store->markDirty(); };
    mixerPage->onSettingsChanged = [this] { store->markDirty(); };

    topology.onChanged = [this]
    {
        router.refresh();       // re-resolve stored routes to the new endpoint refs
        patchbay->rebuild();
    };

    patchbay->rebuild();
    setSize (1100, 700);
}

void MainComponent::resized() { tabs.setBounds (getLocalBounds()); }
}
