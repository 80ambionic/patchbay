#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>
#include <memory>
#include "Mixer/MixerCore.h"
#include "Mixer/MixerSettings.h"
#include "MixerPresets.h"
#include "Net/NetworkEngine.h"
#include "SslLookAndFeel.h"

namespace pb
{
class AnalyzerPanel;
/** Page 3: the mixer console.
      - 32 stereo input strips. Each has an Input menu (Off, a network pair, an app slot, or a stereo pair of the audio input
        device) above an Output menu (Master or one of the 32 buses), a balance knob, mute/solo and a fader.
      - 32 stereo output buses. Each ends at device outputs ("Audio Out") or a UDP stream ("Net Out").
      - A master with the same choice. The master takes every strip whose Output is "Master".
      - A preset bar: save, load and delete the whole mixer by name.
    Controls write straight into the MixerCore (lock-free), so changes are heard immediately; meters poll the core ~30
    times a second. */
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
    class InputStrip;
    class OutputStrip;
    class AppSlotPanel;
    class InputPanel;
    struct Content : juce::Component
    {
        void resized() override;
        void paint (juce::Graphics&) override;
        std::vector<InputStrip*> strips;           // kStrips
        std::vector<OutputStrip*> buses;           // kBuses
        std::vector<AppSlotPanel*> slots;          // kAppSlots
        InputPanel* inputPanel = nullptr;
        int inputsRight() const;
    };

    void timerCallback() override;
    void changed();
    void selectScope (int target);                 // strip 0..31 or bus 32..63 into the analyzer
    juce::String scopeName (int target) const;
    void syncOutputChoices();
    void applyStrip (int i, const StripSettings&);
    void applyBus (int b, const BusSettings&);
    void applyMaster();
    void applyAppNames();

    void refreshPresetList (const juce::String& select);
    void loadPreset (const juce::String& name);
    void savePresetAs (const juce::String& name);
    void promptSaveAs();
    void promptDelete();
    void updatePresetStatus();

    SslLookAndFeel ssl;                            // declared first: must outlive every child that uses it
    net::NetworkEngine& engine;
    MixerCore& core;
    MixerPresets presets { MixerPresets::defaultFolder() };

    std::vector<std::unique_ptr<InputStrip>> strips;
    std::vector<std::unique_ptr<OutputStrip>> busStrips;
    std::unique_ptr<OutputStrip> master;
    std::vector<std::unique_ptr<AppSlotPanel>> slotPanels;
    std::unique_ptr<InputPanel> inputPanel;
    std::unique_ptr<AnalyzerPanel> analyzer;
    Content content;
    juce::Viewport viewport;

    // Preset bar
    juce::Label presetTitle, presetStatus;
    juce::ComboBox presetBox;
    juce::TextButton saveButton { "Save" }, saveAsButton { "Save as..." }, deleteButton { "Delete" }, initButton { "Init" };
    juce::String loadedPreset;                     // name of the preset the mixer was last loaded from / saved to ("" = none)
    MixerSettings loadedSettings;                  // what it contained, to tell whether the mixer has been edited since
    bool presetLoadInProgress = false;

    int clipHoldTicks = 0, tick = 0, lastNumOutputs = -1;
    std::array<std::string, kAppSlots> appNames;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerPage)
};
}
