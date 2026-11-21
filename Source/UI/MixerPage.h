#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>
#include <memory>
#include "Mixer/MixerCore.h"
#include "Mixer/MixerSettings.h"
#include "Net/NetworkEngine.h"

namespace pb
{
/** Page 3: the mixer console. 24 input strips - 8 network channels, 4 app-capture slots (stereo, so 2 strips each) and
    8 audio-input-device channels - plus a master strip. Every strip can feed the master bus or go straight to a chosen device output; the master pair is
    chosen too. Controls write straight into the MixerCore (lock-free), so changes are heard immediately; meters poll
    the core ~30 times a second. */
class MixerPage : public juce::Component, private juce::Timer
{
public:
    explicit MixerPage (net::NetworkEngine&);
    ~MixerPage() override;

    MixerSettings getSettings() const;
    void setSettings (const MixerSettings&);
    std::function<void()> onSettingsChanged;       // fired on any user edit, so the owner can persist

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class Meter;
    class Strip;
    class AppSlotPanel;
    class InputPanel;
    struct Content : juce::Component
    {
        void resized() override;
        void paint (juce::Graphics&) override;
        std::vector<Strip*> strips;                // kMixerStrips channel strips
        std::vector<AppSlotPanel*> slots;          // kAppSlots
        InputPanel* inputPanel = nullptr;
        Strip* master = nullptr;
    };

    void timerCallback() override;
    void changed() { if (onSettingsChanged) onSettingsChanged(); }
    void syncOutputChoices();

    net::NetworkEngine& engine;
    MixerCore& core;
    std::vector<std::unique_ptr<Strip>> strips;      // kMixerStrips channel strips, then the master strip
    std::vector<std::unique_ptr<AppSlotPanel>> slotPanels;
    std::unique_ptr<InputPanel> inputPanel;
    Content content;
    juce::Viewport viewport;
    int clipHoldTicks = 0, tick = 0, lastNumOutputs = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerPage)
};
}
