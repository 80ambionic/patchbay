#include "NetworkMidiLane.h"

namespace pb
{
namespace
{
const juce::Colour kBg (0xff15171b), kPanel (0xff2a2e35), kText (0xffd8dce2), kDim (0xff8a8f98),
                   kGood (0xff6ccf8e), kBad (0xffe06c6c), kAccent (0xff5b9bd5);

constexpr int kIdNoOne = 1, kIdContacts = 2, kIdAnyone = 3;

int policyToId (MidiNetPolicy p) { return p == MidiNetPolicy::noOne ? kIdNoOne : p == MidiNetPolicy::anyone ? kIdAnyone : kIdContacts; }
MidiNetPolicy idToPolicy (int id) { return id == kIdNoOne ? MidiNetPolicy::noOne : id == kIdAnyone ? MidiNetPolicy::anyone : MidiNetPolicy::contactsOnly; }

juce::String hostText (const MidiNetHost& h)
{
    return juce::String (h.name) + "   (" + juce::String (h.address) + ":" + juce::String (h.port) + ")";
}
}

struct NetworkMidiLane::HostListModel : juce::ListBoxModel
{
    explicit HostListModel (NetworkMidiLane& o) : owner (o) {}
    int getNumRows() override { return (int) owner.settings.hosts.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= getNumRows()) return;
        if (selected) g.fillAll (kAccent.withAlpha (0.35f));
        g.setColour (kText);
        g.setFont (juce::Font (juce::FontOptions (14.0f)));
        g.drawText (hostText (owner.settings.hosts[(size_t) row]), 8, 0, w - 12, h, juce::Justification::centredLeft, true);
    }
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        if (row >= 0 && row < getNumRows()) owner.connectTo (owner.settings.hosts[(size_t) row]);
    }
    void selectedRowsChanged (int) override { owner.repaint(); }
    NetworkMidiLane& owner;
};

struct NetworkMidiLane::ConnectionListModel : juce::ListBoxModel
{
    explicit ConnectionListModel (NetworkMidiLane& o) : owner (o) {}
    int getNumRows() override { return (int) owner.connections.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= getNumRows()) return;
        if (selected) g.fillAll (kAccent.withAlpha (0.35f));
        const auto& c = owner.connections[(size_t) row];
        g.setColour (kGood);
        g.fillEllipse (8.0f, (float) h * 0.5f - 4.0f, 8.0f, 8.0f);
        g.setColour (kText);
        g.setFont (juce::Font (juce::FontOptions (14.0f)));
        g.drawText (juce::String (c.name) + "   (" + juce::String (c.address) + ":" + juce::String (c.port) + ")",
                    24, 0, w - 28, h, juce::Justification::centredLeft, true);
    }
    NetworkMidiLane& owner;
};

NetworkMidiLane::NetworkMidiLane (NetworkMidiSession& s) : session (s)
{
    hostModel = std::make_unique<HostListModel> (*this);
    connModel = std::make_unique<ConnectionListModel> (*this);

    for (auto* l : { &policyLabel, &addressLabel, &portLabel, &nameLabel, &savedLabel, &activeLabel })
    {
        l->setColour (juce::Label::textColourId, kDim);
        addAndMakeVisible (l);
    }

    enableButton.setClickingTogglesState (false);
    enableButton.onClick = [this] { toggleEnabled(); };
    addAndMakeVisible (enableButton);

    policyBox.addItem ("No one", kIdNoOne);
    policyBox.addItem ("Only my contacts", kIdContacts);
    policyBox.addItem ("Anyone on the network", kIdAnyone);
    policyBox.setSelectedId (policyToId (settings.policy), juce::dontSendNotification);
    policyBox.onChange = [this] { applyPolicy(); if (onSettingsChanged) onSettingsChanged(); };
    addAndMakeVisible (policyBox);

    addressEditor.setTextToShowWhenEmpty ("192.168.1.20 or host.local", kDim);
    addressEditor.setInputRestrictions (253);
    nameEditor.setTextToShowWhenEmpty ("optional", kDim);
    nameEditor.setInputRestrictions (64);
    portEditor.setInputRestrictions (5, "0123456789");
    portEditor.setText (juce::String (kDefaultRtpMidiPort), false);
    addressEditor.onReturnKey = [this] { connectFromEditors(); };
    portEditor.onReturnKey = [this] { connectFromEditors(); };
    nameEditor.onReturnKey = [this] { connectFromEditors(); };
    for (auto* e : { &addressEditor, &portEditor, &nameEditor }) addAndMakeVisible (e);

    connectButton.onClick = [this] { connectFromEditors(); };
    connectButton.setColour (juce::TextButton::buttonColourId, kAccent.darker (0.3f));
    addAndMakeVisible (connectButton);

    forgetButton.onClick = [this]
    {
        const int row = savedList.getSelectedRow();
        if (row < 0 || row >= (int) settings.hosts.size()) return;
        settings.hosts.erase (settings.hosts.begin() + row);
        savedList.updateContent();
        savedList.deselectAllRows();
        if (onSettingsChanged) onSettingsChanged();
    };
    addAndMakeVisible (forgetButton);

    disconnectButton.onClick = [this]
    {
        const int row = activeList.getSelectedRow();
        if (row < 0 || row >= (int) connections.size()) return;
        const auto c = connections[(size_t) row];
        session.disconnect (c.address, c.port);
        refresh();
    };
    addAndMakeVisible (disconnectButton);

    for (auto* lb : { &savedList, &activeList })
    {
        lb->setRowHeight (24);
        lb->setColour (juce::ListBox::backgroundColourId, kPanel);
        lb->setColour (juce::ListBox::outlineColourId, kPanel.brighter (0.2f));
        lb->setOutlineThickness (1);
        addAndMakeVisible (lb);
    }
    savedList.setModel (hostModel.get());
    activeList.setModel (connModel.get());

    updateEnabledState();
    startTimerHz (2);
}

NetworkMidiLane::~NetworkMidiLane()
{
    stopTimer();
    savedList.setModel (nullptr);
    activeList.setModel (nullptr);
}

MidiNetSettings NetworkMidiLane::getSettings() const { return settings; }

void NetworkMidiLane::setSettings (const MidiNetSettings& s)
{
    settings = s;
    settings.hosts = normaliseHosts (settings.hosts);
    policyBox.setSelectedId (policyToId (settings.policy), juce::dontSendNotification);
    session.setPolicy (settings.policy);
    savedList.updateContent();
    savedList.repaint();
}

void NetworkMidiLane::toggleEnabled()
{
    errorText.clear();
    session.setEnabled (! session.isEnabled());
    if (session.isEnabled()) session.setPolicy (settings.policy);
    updateEnabledState();
    refresh();
}

void NetworkMidiLane::applyPolicy()
{
    settings.policy = idToPolicy (policyBox.getSelectedId());
    session.setPolicy (settings.policy);
}

void NetworkMidiLane::updateEnabledState()
{
    const bool on = session.isEnabled();
    enableButton.setButtonText (on ? "Disable network MIDI" : "Enable network MIDI");
    enableButton.setColour (juce::TextButton::buttonColourId, on ? kGood.darker (0.5f) : kPanel);
    connectButton.setEnabled (on);
    disconnectButton.setEnabled (on);
    repaint();
}

void NetworkMidiLane::connectFromEditors()
{
    MidiNetHost h;
    h.address = addressEditor.getText().trim().toStdString();
    h.name = sanitizeHostName (nameEditor.getText().toStdString());
    const auto portText = portEditor.getText().trim();
    h.port = portText.isEmpty() ? kDefaultRtpMidiPort : portText.getIntValue();

    if (! isValidHostAddress (h.address)) { errorText = "Enter an IPv4/IPv6 address or a host name."; repaint(); return; }
    if (! isValidPort (h.port))           { errorText = "Port must be 1 to 65535."; repaint(); return; }
    if (h.name.empty()) h.name = h.address;
    connectTo (h);
}

void NetworkMidiLane::connectTo (const MidiNetHost& h)
{
    if (! session.isEnabled()) { errorText = "Enable network MIDI first."; repaint(); return; }

    std::string err;
    if (! session.connect (h, err))
    {
        errorText = err.empty() ? "Could not connect." : juce::String (err);
        repaint();
        return;
    }

    errorText.clear();
    auto hosts = settings.hosts;
    hosts.push_back (h);
    settings.hosts = normaliseHosts (hosts);
    savedList.updateContent();
    if (onSettingsChanged) onSettingsChanged();
    refresh();
}

void NetworkMidiLane::refresh()
{
    auto now = session.getConnections();
    juce::String sig;
    for (const auto& c : now) sig << c.name << '|' << c.address << '|' << c.port << ';';
    if (sig != connectionsSignature)
    {
        connectionsSignature = sig;
        connections = std::move (now);
        activeList.updateContent();
        activeList.repaint();
    }
}

void NetworkMidiLane::timerCallback()
{
    const bool on = session.isEnabled();      // the user can also toggle it from Audio MIDI Setup
    if ((enableButton.getButtonText() == "Disable network MIDI") != on) updateEnabledState();
    refresh();
    repaint (statusArea);
}

void NetworkMidiLane::resized()
{
    auto area = getLocalBounds().reduced (20, 16);
    area.removeFromTop (28);                                    // lane title painted by paint()

    auto row1 = area.removeFromTop (30);
    enableButton.setBounds (row1.removeFromLeft (190));
    row1.removeFromLeft (18);
    policyLabel.setBounds (row1.removeFromLeft (106));
    policyBox.setBounds (row1.removeFromLeft (200));

    area.removeFromTop (10);
    statusArea = area.removeFromTop (52);

    area.removeFromTop (12);
    auto row2 = area.removeFromTop (30);
    addressLabel.setBounds (row2.removeFromLeft (62));
    addressEditor.setBounds (row2.removeFromLeft (220));
    row2.removeFromLeft (12);
    portLabel.setBounds (row2.removeFromLeft (40));
    portEditor.setBounds (row2.removeFromLeft (70));
    row2.removeFromLeft (12);
    nameLabel.setBounds (row2.removeFromLeft (48));
    nameEditor.setBounds (row2.removeFromLeft (150));
    row2.removeFromLeft (12);
    connectButton.setBounds (row2.removeFromLeft (100));

    area.removeFromTop (12);
    auto lists = area;
    auto left = lists.removeFromLeft (lists.getWidth() / 2 - 8);
    lists.removeFromLeft (16);
    auto& right = lists;

    savedLabel.setBounds (left.removeFromTop (22));
    forgetButton.setBounds (left.removeFromBottom (28).removeFromLeft (100));
    left.removeFromBottom (6);
    savedList.setBounds (left);

    activeLabel.setBounds (right.removeFromTop (22));
    disconnectButton.setBounds (right.removeFromBottom (28).removeFromLeft (110));
    right.removeFromBottom (6);
    activeList.setBounds (right);
}

void NetworkMidiLane::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
    g.setColour (kText);
    g.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    g.drawText ("MIDI over network (RTP-MIDI)", 20, 16, getWidth() - 40, 24, juce::Justification::centredLeft);

    g.setColour (kPanel);
    g.fillRoundedRectangle (statusArea.toFloat(), 6.0f);

    const bool on = session.isEnabled();
    auto inner = statusArea.reduced (14, 4);
    g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
    g.setColour (errorText.isNotEmpty() ? kBad : (on ? kGood : kText));
    juce::String head = errorText.isNotEmpty() ? errorText
                      : on ? "Enabled  -  session \"" + juce::String (session.getSessionName()) + "\", UDP port "
                                 + juce::String (session.getListenPort())
                           : "Off  (network MIDI is always off after launch)";
    g.drawText (head, inner.removeFromTop (24), juce::Justification::centredLeft, true);

    g.setColour (kDim);
    g.setFont (juce::Font (juce::FontOptions (12.5f)));
    const auto p = settings.policy;
    g.drawText (p == MidiNetPolicy::noOne ? "Nobody can connect; you can still connect out to a host below."
              : p == MidiNetPolicy::contactsOnly ? "Only hosts you connect to (or add) can open a session. Connecting below adds the peer."
                                                 : "Any device on this network can connect. Use only on networks you trust.",
                inner, juce::Justification::centredLeft, true);
}
}
