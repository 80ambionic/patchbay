#include "NetworkAudioLane.h"

namespace pb
{
namespace
{
const juce::Colour kBg (0xff15171b), kPanel (0xff2a2e35), kText (0xffd8dce2), kDim (0xff8a8f98),
                   kGood (0xff6ccf8e), kWarn (0xfff0a35a), kBad (0xffe06c6c), kAccent (0xff5b9bd5);

juce::String num (uint64_t v) { return juce::String ((juce::int64) v); }
}

NetworkAudioLane::NetworkAudioLane (net::NetworkEngine& e) : engine (e)
{
    for (auto* l : { &portLabel, &channelsLabel, &bufferLabel })
    {
        l->setColour (juce::Label::textColourId, kDim);
        addAndMakeVisible (l);
    }

    portEditor.setInputRestrictions (5, "0123456789");
    portEditor.setText (juce::String (NetworkSettings().port), false);
    portEditor.setSelectAllWhenFocused (true);
    auto commitPort = [this]
    {
        portEditor.setText (juce::String (getSettings().port), false);     // normalise (clamps to the valid range)
        if (onSettingsChanged) onSettingsChanged();
    };
    portEditor.onReturnKey = commitPort;
    portEditor.onFocusLost = commitPort;
    addAndMakeVisible (portEditor);

    for (int c = 1; c <= NetworkSettings::kMaxChannels; ++c)
        channelsBox.addItem (c == 1 ? "1 (mono)" : c == 2 ? "2 (stereo)" : juce::String (c), c);
    channelsBox.setSelectedId (NetworkSettings().channels, juce::dontSendNotification);
    channelsBox.onChange = [this] { if (onSettingsChanged) onSettingsChanged(); };
    addAndMakeVisible (channelsBox);

    bufferSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    bufferSlider.setRange (NetworkSettings::kMinBuffer, NetworkSettings::kMaxBuffer, 1);
    bufferSlider.setValue (NetworkSettings().bufferPackets, juce::dontSendNotification);
    bufferSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 24);
    bufferSlider.setTextValueSuffix (" pkts");
    bufferSlider.onValueChange = [this]
    {
        engine.setBufferPackets ((int) bufferSlider.getValue());       // takes effect live
        if (onSettingsChanged) onSettingsChanged();
    };
    addAndMakeVisible (bufferSlider);

    listenButton.onClick = [this] { toggleListening(); };
    listenButton.setColour (juce::TextButton::buttonColourId, kAccent.darker (0.3f));
    addAndMakeVisible (listenButton);

    deviceButton.onClick = [this] { openDeviceDialog(); };
    addAndMakeVisible (deviceButton);

    addresses = localAddresses();
    refreshControls();
    startTimerHz (10);
}

NetworkAudioLane::~NetworkAudioLane()
{
    stopTimer();
    if (deviceDialog != nullptr) deviceDialog.deleteAndZero();    // it references the engine's device manager
}

NetworkSettings NetworkAudioLane::getSettings() const
{
    NetworkSettings s;
    s.port = std::clamp (portEditor.getText().getIntValue(), NetworkSettings::kMinPort, NetworkSettings::kMaxPort);
    if (portEditor.getText().isEmpty()) s.port = NetworkSettings().port;
    s.channels = std::clamp (channelsBox.getSelectedId(), 1, NetworkSettings::kMaxChannels);
    s.bufferPackets = (int) bufferSlider.getValue();
    return s;
}

void NetworkAudioLane::setSettings (const NetworkSettings& s)
{
    portEditor.setText (juce::String (s.port), false);
    channelsBox.setSelectedId (s.channels, juce::dontSendNotification);
    bufferSlider.setValue (s.bufferPackets, juce::dontSendNotification);
    engine.setBufferPackets (s.bufferPackets);
}

juce::String NetworkAudioLane::localAddresses()
{
    juce::StringArray out;
    for (const auto& a : juce::IPAddress::getAllAddresses (false))
        if (a != juce::IPAddress::local()) out.add (a.toString());
    return out.isEmpty() ? juce::String ("(no network address found)") : out.joinIntoString ("  /  ");
}

void NetworkAudioLane::toggleListening()
{
    if (engine.isListening())
    {
        engine.stop();
        errorText.clear();
    }
    else
    {
        juce::String err;
        addresses = localAddresses();                   // the network may have changed since launch
        if (! engine.start (getSettings(), err)) errorText = err;
        else errorText.clear();
    }
    refreshControls();
    timerCallback();
}

void NetworkAudioLane::openDeviceDialog()
{
    if (deviceDialog != nullptr) { deviceDialog->toFront (true); return; }

    if (auto err = engine.prepareDevice(); err.isNotEmpty()) { errorText = "Audio output: " + err; repaint(); return; }

    auto* selector = new juce::AudioDeviceSelectorComponent (engine.getDeviceManager(), 0, 0, 2, 64, false, false, true, false);
    selector->setSize (540, 420);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (selector);
    o.dialogTitle = "Audio output";
    o.dialogBackgroundColour = kPanel;
    o.componentToCentreAround = this;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    deviceDialog = o.launchAsync();
}

void NetworkAudioLane::refreshControls()
{
    const bool on = engine.isListening();
    listenButton.setButtonText (on ? "Stop" : "Listen");
    listenButton.setColour (juce::TextButton::buttonColourId, on ? kBad.darker (0.4f) : kAccent.darker (0.3f));
    portEditor.setEnabled (! on);          // these need a restart to take effect; the buffer slider is live
    channelsBox.setEnabled (! on);
}

std::vector<NetworkAudioLane::Row> NetworkAudioLane::buildRows (const net::NetworkEngine::Snapshot& s) const
{
    std::vector<Row> r;
    auto ms = [] (double v) { return juce::String (v, 1) + " ms"; };

    r.push_back ({ "Output", engine.describeDevice(), false });
    r.push_back ({ "Packets received", num (s.packets), false });
    if (s.packetFrames > 0 && s.sampleRate > 0)
        r.push_back ({ "Packet size", juce::String (s.packetFrames) + " frames  (" + ms (s.packetFrames / s.sampleRate * 1000.0) + ")", false });
    r.push_back ({ "Buffer", s.playing ? ms (s.fillMs) + "  (target " + ms (s.targetMs) + ")" : juce::String ("-"), false });
    r.push_back ({ "Clock correction", s.playing ? (s.ratioPpm >= 0 ? "+" : "") + juce::String (s.ratioPpm, 0) + " ppm" : juce::String ("-"), false });
    r.push_back ({ "Lost (concealed)", num (s.lost), s.lost > 0 });
    r.push_back ({ "Late (dropped)", num (s.late), s.late > 0 });
    r.push_back ({ "Duplicates", num (s.duplicates), false });
    r.push_back ({ "Underruns", num (s.underruns), s.underruns > 0 });
    r.push_back ({ "Dropped for latency", num (s.trimmed), s.trimmed > 0 });
    r.push_back ({ "Stream restarts", num (s.resyncs), false });
    r.push_back ({ "Rejected datagrams", num (s.rejected), s.rejected > 0 });
    r.push_back ({ "Receive overflows", num (s.queueOverflows), s.queueOverflows > 0 });
    if (s.channelMismatches > 0)
        r.push_back ({ "Wrong channel count", num (s.channelMismatches) + "  (sender is sending " + juce::String (s.lastMismatchChannels)
                                              + ", page is set to " + juce::String (getSettings().channels) + ")", true });
    return r;
}

void NetworkAudioLane::timerCallback()
{
    const auto s = engine.snapshot();

    // Meters: instant attack, smooth release.
    levelL = std::max (s.peakL, levelL * 0.82f);
    levelR = std::max (s.peakR, levelR * 0.82f);

    juce::String newState; bool good = false;
    if (! s.listening)               newState = "Stopped";
    else if (s.packets == 0)         newState = "Listening - waiting for a sender";
    else if (s.playing)              { newState = "Playing"; good = true; }
    else                             newState = s.underruns > 0 ? "Underrun - rebuffering" : "Buffering...";

    rows = buildRows (s);
    juce::String sig = newState + (errorText) + juce::String (levelL, 2) + juce::String (levelR, 2);
    for (const auto& r : rows) sig << r.name << r.value;

    stateText = newState; stateGood = good;
    if (sig != rowsSignature) { rowsSignature = sig; repaint(); }
}

void NetworkAudioLane::resized()
{
    auto area = getLocalBounds().reduced (20, 16);
    area.removeFromTop (28);                      // lane title painted by paint()

    auto row1 = area.removeFromTop (30);
    portLabel.setBounds (row1.removeFromLeft (62));
    portEditor.setBounds (row1.removeFromLeft (80));
    row1.removeFromLeft (18);
    channelsLabel.setBounds (row1.removeFromLeft (70));
    channelsBox.setBounds (row1.removeFromLeft (110));
    row1.removeFromLeft (18);
    listenButton.setBounds (row1.removeFromLeft (110));
    row1.removeFromLeft (10);
    deviceButton.setBounds (row1.removeFromLeft (150));

    area.removeFromTop (10);
    auto row2 = area.removeFromTop (30);
    bufferLabel.setBounds (row2.removeFromLeft (62));
    bufferSlider.setBounds (row2.removeFromLeft (juce::jmin (420, row2.getWidth())));

    area.removeFromTop (14);
    area.removeFromTop (58);                      // status text block painted by paint()
    meterArea = area.removeFromRight (juce::jmin (260, area.getWidth() / 3)).withTrimmedLeft (20);
    statsArea = area;
}

void NetworkAudioLane::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
    g.setColour (kText);
    g.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    g.drawText ("Audio over network (UDP)", 20, 16, getWidth() - 40, 24, juce::Justification::centredLeft);

    // Status block under the controls
    const auto top = statsArea.getY() - 58;
    juce::Rectangle<int> status (20, top, getWidth() - 40, 52);
    g.setColour (kPanel);
    g.fillRoundedRectangle (status.toFloat(), 6.0f);

    g.setColour (errorText.isNotEmpty() ? kBad : (stateGood ? kGood : kText));
    g.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    g.drawText (errorText.isNotEmpty() ? errorText : stateText, status.reduced (14, 4).removeFromTop (24), juce::Justification::centredLeft, true);

    g.setColour (kDim);
    g.setFont (juce::Font (juce::FontOptions (12.5f)));
    g.drawText ("Send UDP audio to  " + addresses + "  port " + juce::String (getSettings().port)
                    + "   -   wire format: see Source/Net/Packet.h (float32 or int16, up to 8 channels)",
                status.reduced (14, 4).withTrimmedTop (26), juce::Justification::centredLeft, true);

    // Statistics table
    g.setFont (juce::Font (juce::FontOptions (13.5f)));
    int y = statsArea.getY() + 6;
    for (const auto& r : rows)
    {
        if (y + 22 > statsArea.getBottom()) break;
        g.setColour (kDim);
        g.drawText (r.name, statsArea.getX(), y, 170, 22, juce::Justification::centredLeft);
        g.setColour (r.warn ? kWarn : kText);
        g.drawText (r.value, statsArea.getX() + 176, y, statsArea.getWidth() - 176, 22, juce::Justification::centredLeft, true);
        y += 24;
    }

    // Level meters
    auto m = meterArea.reduced (0, 6);
    g.setColour (kDim);
    g.setFont (juce::Font (juce::FontOptions (12.5f)));
    g.drawText ("Output level", m.removeFromTop (20), juce::Justification::centredLeft);

    auto drawMeter = [&] (const char* name, float level)
    {
        auto line = m.removeFromTop (26);
        g.setColour (kDim);
        g.drawText (name, line.removeFromLeft (18), juce::Justification::centredLeft);
        auto bar = line.reduced (0, 6).toFloat();
        g.setColour (kPanel);
        g.fillRoundedRectangle (bar, 3.0f);

        const float db = juce::Decibels::gainToDecibels (level, -60.0f);
        const float frac = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        g.setColour (db > -1.0f ? kBad : (db > -12.0f ? kWarn : kGood));
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * frac), 3.0f);
    };
    drawMeter ("L", levelL);
    drawMeter ("R", levelR);
}
}
