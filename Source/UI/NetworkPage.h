#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "NetworkAudioLane.h"
#include "NetworkMidiLane.h"

namespace pb
{
/** Page 2: the network receiver. Two lanes stacked vertically - audio over UDP and MIDI over RTP-MIDI -
    inside a scrolling viewport so the page stays usable in small windows and on iPad split view. */
class NetworkPage : public juce::Component
{
public:
    NetworkPage (net::NetworkEngine&, NetworkMidiSession&);

    NetworkAudioLane& audio() { return audioLane; }
    NetworkMidiLane&  midi()  { return midiLane; }

    void resized() override;

private:
    struct Content : juce::Component
    {
        void resized() override;
        NetworkAudioLane* audio = nullptr;
        NetworkMidiLane*  midi = nullptr;
        static constexpr int kAudioHeight = 400, kMidiHeight = 380, kMinWidth = 700;
    };

    NetworkAudioLane audioLane;
    NetworkMidiLane  midiLane;
    Content content;
    juce::Viewport viewport;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkPage)
};
}
