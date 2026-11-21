#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <functional>
#include "Net/NetworkEngine.h"
#include "StateFile.h"

namespace pb
{
/** Network page, audio lane: receive audio over UDP and play it on an output device. Port, channel count, jitter-buffer depth,
    a Listen toggle, an output-device picker, level meters and live link statistics. */
class NetworkAudioLane : public juce::Component, private juce::Timer
{
public:
    explicit NetworkAudioLane (net::NetworkEngine&);
    ~NetworkAudioLane() override;

    NetworkSettings getSettings() const;
    void setSettings (const NetworkSettings&);
    std::function<void()> onSettingsChanged;     // fired on any user edit, so the owner can persist

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Row { juce::String name, value; bool warn = false; };

    void toggleListening();
    void openDeviceDialog();
    void refreshControls();
    void timerCallback() override;
    std::vector<Row> buildRows (const net::NetworkEngine::Snapshot&) const;
    static juce::String localAddresses();

    net::NetworkEngine& engine;

    juce::Label portLabel { {}, "UDP port" }, channelsLabel { {}, "Channels" }, bufferLabel { {}, "Buffer" };
    juce::TextEditor portEditor;
    juce::ComboBox channelsBox;
    juce::Slider bufferSlider;
    juce::TextButton listenButton { "Listen" }, deviceButton { "Audio device..." };

    juce::String errorText, addresses;
    std::vector<Row> rows;
    juce::String rowsSignature;
    juce::String stateText { "Stopped" };
    bool stateGood = false;
    float levelL = 0.0f, levelR = 0.0f;
    juce::Rectangle<int> statsArea, meterArea;
    juce::Component::SafePointer<juce::DialogWindow> deviceDialog;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkAudioLane)
};
}
