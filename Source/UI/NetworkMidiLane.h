#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "Midi/NetworkMidiSession.h"
#include "Midi/NetMidiTypes.h"

namespace pb
{
/** MIDI over the network (RTP-MIDI / AppleMIDI). Enable the session, choose who may connect, connect to a
    peer by address, and see the open sessions. While enabled, the session shows up as CoreMIDI endpoints,
    so it appears on the MIDI Patchbay tab like any other device. The enabled state is never persisted:
    network MIDI is always off after launch until the user switches it on. */
class NetworkMidiLane : public juce::Component, private juce::Timer
{
public:
    explicit NetworkMidiLane (NetworkMidiSession&);
    ~NetworkMidiLane() override;

    MidiNetSettings getSettings() const;
    void setSettings (const MidiNetSettings&);
    std::function<void()> onSettingsChanged;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct HostListModel;
    struct ConnectionListModel;

    void toggleEnabled();
    void applyPolicy();
    void connectTo (const MidiNetHost&);
    void connectFromEditors();
    void refresh();
    void timerCallback() override;
    void updateEnabledState();

    NetworkMidiSession& session;
    MidiNetSettings settings;                       // policy + saved hosts (source of truth for persistence)
    std::vector<NetworkMidiSession::Connection> connections;
    juce::String connectionsSignature;

    juce::Label policyLabel { {}, "Who can connect" }, addressLabel { {}, "Address" }, portLabel { {}, "Port" },
                nameLabel { {}, "Name" }, savedLabel { {}, "Saved hosts" }, activeLabel { {}, "Open sessions" };
    juce::TextButton enableButton { "Enable network MIDI" };
    juce::ComboBox policyBox;
    juce::TextEditor addressEditor, portEditor, nameEditor;
    juce::TextButton connectButton { "Connect" }, forgetButton { "Forget" }, disconnectButton { "Disconnect" };
    juce::ListBox savedList, activeList;
    std::unique_ptr<HostListModel> hostModel;
    std::unique_ptr<ConnectionListModel> connModel;

    juce::String errorText;
    juce::Rectangle<int> statusArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkMidiLane)
};
}
