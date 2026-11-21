#pragma once
#include <juce_events/juce_events.h>
#include <functional>
#include "StateFile.h"

namespace pb
{
/** Debounced auto-save around StateFile. Message thread only. */
class StateStore : private juce::Timer
{
public:
    StateStore (juce::File file, std::function<PatchbayState()> snapshotFn)
        : file (std::move (file)), snapshot (std::move (snapshotFn)) {}
    ~StateStore() override { flush(); }

    static juce::File defaultFile()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("PatchBay").getChildFile ("patchbay.json");
    }

    PatchbayState load() const { return loadFile (file); }

    void markDirty()                 // schedule a save in ~400 ms; each call restarts the countdown, so a drag is one write
    {
        dirty = true;
        startTimer (400);
    }

    void flush()                     // save now if anything changed
    {
        stopTimer();
        if (! dirty || ! snapshot) return;
        dirty = false;
        saveAtomically (file, snapshot());
    }

private:
    void timerCallback() override { flush(); }

    juce::File file;
    std::function<PatchbayState()> snapshot;
    bool dirty = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StateStore)
};
}
