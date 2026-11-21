#include "MixerPage.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include "Capture/AppGrouping.h"
#include "AnalyzerPanel.h"

namespace pb
{
namespace
{
const juce::Colour kBg (0xff15171b), kAppTint (0xff6ccfb8), kInTint (0xffe2b05a), kPanel (0xff23272e), kPanelHi (0xff2a2e35), kText (0xffd8dce2), kDim (0xff8a8f98),
                   kGood (0xff6ccf8e), kWarn (0xfff0a35a), kBad (0xffe06c6c), kAccent (0xff5b9bd5), kNetTint (0xffb48ae0);

constexpr int kStripW = 88, kMasterW = 124, kGap = 6, kMargin = 14, kGroupGap = 28, kHeaderH = 150, kStripH = 470, kTitleH = 22, kAnalyzerH = 196;
constexpr float kMeterFloorDb = -60.0f, kMeterCeilDb = 6.0f;

float toDb (float lin) noexcept { return lin <= 1.0e-6f ? -120.0f : 20.0f * std::log10 (lin); }
float dbToMeterPos (float db) noexcept { return juce::jlimit (0.0f, 1.0f, (db - kMeterFloorDb) / (kMeterCeilDb - kMeterFloorDb)); }

juce::String faderText (double db) { return db <= (double) kMinGainDb + 0.001 ? juce::String ("-inf") : juce::String (db, 1) + " dB"; }

juce::String pairName (const char* prefix, int pair) { return juce::String (prefix) + juce::String (2 * pair + 1) + "-" + juce::String (2 * pair + 2); }

juce::Colour sourceTint (SourceType t)
{
    switch (t) { case SourceType::net: return kAccent; case SourceType::app: return kAppTint; case SourceType::input: return kInTint; default: return kDim; }
}

void styleFader (juce::Slider& fader, int textBoxWidth)
{
    fader.setSliderStyle (juce::Slider::LinearVertical);
    fader.setRange (kMinGainDb, kMaxGainDb, 0.1);
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, textBoxWidth, 22);
    fader.textFromValueFunction = faderText;
    fader.valueFromTextFunction = [] (const juce::String& t)
    {
        const auto s = t.trim().toLowerCase();
        return s.startsWith ("-inf") ? (double) kMinGainDb : (double) clampGainDb (s.getFloatValue());
    };
    fader.setColour (juce::Slider::trackColourId, kPanelHi);
    fader.setColour (juce::Slider::thumbColourId, juce::Colour (0xff4a4f58));
    fader.setColour (juce::Slider::textBoxTextColourId, kText);
    fader.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    fader.setValue (0.0, juce::dontSendNotification);
}

void styleScopeButton (juce::TextButton& b)
{
    b.setClickingTogglesState (false);                       // the page makes the selection exclusive
    b.setColour (juce::TextButton::buttonOnColourId, kAccent.darker (0.1f));
    b.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    b.setTooltip ("Show this track in the analyzer at the top");
}

void styleNameLabel (juce::Label& l, bool editable, float size, bool bold)
{
    l.setJustificationType (juce::Justification::centred);
    l.setColour (juce::Label::textColourId, kText);
    l.setColour (juce::Label::outlineWhenEditingColourId, kAccent);
    l.setFont (juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain)));
    if (editable) l.setEditable (false, true, false);     // double-click to rename
}
}

// ---------------------------------------------------------------------------------------------------------------
class MixerPage::Meter : public juce::Component
{
public:
    explicit Meter (int numBars) : bars ((size_t) numBars) {}

    /** peaks: linear peak per bar since the last call. Instant attack, smooth release (about 24 dB/s). */
    void update (const float* peaks, float dtSeconds)
    {
        const float fall = 24.0f * dtSeconds;
        bool dirty = false;
        for (size_t i = 0; i < bars.size(); ++i)
        {
            const float db = toDb (peaks[i]);
            auto& b = bars[i];
            const float next = db > b.levelDb ? db : std::max (db, b.levelDb - fall);
            if (db > b.holdDb) { b.holdDb = db; b.holdAge = 0.0f; }
            else { b.holdAge += dtSeconds; if (b.holdAge > 1.2f) b.holdDb = std::max (db, b.holdDb - fall); }
            if (std::abs (next - b.levelDb) > 0.05f || std::abs (b.holdDb - b.shownHold) > 0.05f) dirty = true;
            b.levelDb = std::max (next, -120.0f);
        }
        if (dirty) repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (r, 3.0f);
        r = r.reduced (2.0f);

        const float barW = (r.getWidth() - (float) (bars.size() - 1) * 2.0f) / (float) bars.size();
        for (size_t i = 0; i < bars.size(); ++i)
        {
            const float x = r.getX() + (float) i * (barW + 2.0f);
            const float h = r.getHeight() * dbToMeterPos (bars[i].levelDb);
            // Colour by zone: green below -12 dB, amber to -3 dB, red above.
            const float zoneAmber = r.getHeight() * dbToMeterPos (-12.0f), zoneRed = r.getHeight() * dbToMeterPos (-3.0f);
            const float bottom = r.getBottom();
            auto seg = [&] (float from, float to, juce::Colour c)
            {
                const float top = std::min (h, to), bot = from;
                if (top > bot) { g.setColour (c); g.fillRect (x, bottom - top, barW, top - bot); }
            };
            seg (0.0f, zoneAmber, kGood);
            seg (zoneAmber, zoneRed, kWarn);
            seg (zoneRed, r.getHeight(), kBad);

            const float hy = bottom - r.getHeight() * dbToMeterPos (bars[i].holdDb);
            if (bars[i].holdDb > kMeterFloorDb) { g.setColour (kText); g.fillRect (x, hy - 1.0f, barW, 2.0f); }
            bars[i].shownHold = bars[i].holdDb;
        }

        g.setColour (kDim.withAlpha (0.6f));                      // 0 dBFS tick
        const float y0 = r.getBottom() - r.getHeight() * dbToMeterPos (0.0f);
        g.drawHorizontalLine ((int) y0, r.getX(), r.getRight());
    }

private:
    struct Bar { float levelDb = -120.0f, holdDb = -120.0f, holdAge = 0.0f, shownHold = -120.0f; };
    std::vector<Bar> bars;
};

// ---------------------------------------------------------------------------------------------------------------
/** One stereo input strip: [name] [Input menu] [Output menu] [balance] [M][S] [fader + stereo meter]. */
class MixerPage::InputStrip : public juce::Component
{
public:
    InputStrip (int index, std::function<void()> onChange) : idx (index), changedCb (std::move (onChange)), meter (2)
    {
        styleNameLabel (nameLabel, true, 13.0f, false);
        nameLabel.onTextChange = [this]
        {
            auto t = sanitizeStripName (nameLabel.getText().toStdString());
            if (t.empty()) t = defaultName().toStdString();
            nameIsDefault = (t == defaultName().toStdString());
            nameLabel.setText (juce::String::fromUTF8 (t.c_str()), juce::dontSendNotification);
            push();
        };
        addAndMakeVisible (nameLabel);

        sourceBox.onChange = [this]
        {
            const int id = sourceBox.getSelectedId();
            if (id <= 0) return;
            src = idToSource (id);
            if (nameIsDefault) nameLabel.setText (defaultName(), juce::dontSendNotification);
            repaint();
            push();
        };
        sourceBox.setTooltip ("Input: where this strip gets its stereo signal");
        addAndMakeVisible (sourceBox);

        outBox.onChange = [this]
        {
            const int id = outBox.getSelectedId();
            if (id <= 0) return;
            out = id - 1;
            push();
        };
        outBox.setTooltip ("Output: the master or one of the 32 output buses");
        addAndMakeVisible (outBox);
        rebuildOutMenu();
        rebuildSourceMenu();

        balance.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        balance.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f, juce::MathConstants<float>::pi * 2.8f, true);
        balance.setRange (-1.0, 1.0, 0.01);
        balance.setDoubleClickReturnValue (true, 0.0);
        balance.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        balance.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xff3d6fb5));
        balance.setColour (juce::Slider::thumbColourId, kText);
        balance.onValueChange = [this] { balText.setText ("Bal " + balanceText (balance.getValue()), juce::dontSendNotification); push(); };
        addAndMakeVisible (balance);

        balText.setJustificationType (juce::Justification::centred);
        balText.setColour (juce::Label::textColourId, kDim);
        balText.setFont (juce::Font (juce::FontOptions (11.5f)));
        addAndMakeVisible (balText);

        muteButton.setClickingTogglesState (true);
        muteButton.setColour (juce::TextButton::buttonOnColourId, kBad.darker (0.2f));
        muteButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        muteButton.onClick = [this] { push(); };
        addAndMakeVisible (muteButton);

        soloButton.setClickingTogglesState (true);
        soloButton.setColour (juce::TextButton::buttonOnColourId, kWarn.darker (0.2f));
        soloButton.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        soloButton.onClick = [this] { push(); };
        addAndMakeVisible (soloButton);

        styleScopeButton (scopeButton);
        scopeButton.onClick = [this] { if (onScope) onScope(); };
        addAndMakeVisible (scopeButton);

        styleFader (fader, kStripW - 20);
        fader.onValueChange = [this] { push(); };
        addAndMakeVisible (fader);
        addAndMakeVisible (meter);
        balText.setText ("Bal C", juce::dontSendNotification);
        nameLabel.setText (defaultName(), juce::dontSendNotification);
    }

    std::function<void (const StripSettings&)> apply;
    std::function<void()> onScope;                       // the A button: show this strip in the analyzer
    void setScopeOn (bool on) { scopeButton.setToggleState (on, juce::dontSendNotification); }
    juce::String displayName() const { return nameLabel.getText(); }

    StripSettings getStrip() const
    {
        StripSettings s;
        s.src = src;
        s.gainDb = clampGainDb ((float) fader.getValue());
        s.balance = clampBalance ((float) balance.getValue());
        s.mute = muteButton.getToggleState();
        s.solo = soloButton.getToggleState();
        s.out = clampStripOut (out);
        s.name = nameIsDefault ? std::string() : sanitizeStripName (nameLabel.getText().toStdString());
        return s;
    }

    void setStrip (const StripSettings& s)
    {
        src = s.src;
        out = s.out;
        fader.setValue (s.gainDb, juce::dontSendNotification);
        balance.setValue (s.balance, juce::dontSendNotification);
        balText.setText ("Bal " + balanceText (s.balance), juce::dontSendNotification);
        muteButton.setToggleState (s.mute, juce::dontSendNotification);
        soloButton.setToggleState (s.solo, juce::dontSendNotification);
        nameIsDefault = s.name.empty();
        nameLabel.setText (nameIsDefault ? defaultName() : juce::String::fromUTF8 (s.name.c_str()), juce::dontSendNotification);
        rebuildSourceMenu();
        rebuildOutMenu();
        repaint();
    }

    /** App names as chosen in the App panels, shown in the Input menu ("App 1 - Safari"). */
    void setAppNames (const std::array<std::string, kAppSlots>& names)
    {
        if (names == appNames) return;
        appNames = names;
        if (! sourceBox.isPopupActive()) rebuildSourceMenu();
        if (nameIsDefault) nameLabel.setText (defaultName(), juce::dontSendNotification);
    }

    Meter& getMeter() { return meter; }

    void paint (juce::Graphics& g) override
    {
        g.setColour (kPanel);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
        g.setColour (sourceTint (src.type).withAlpha (0.8f));
        g.fillRoundedRectangle (getLocalBounds().toFloat().withHeight (4.0f).reduced (14.0f, 0.0f), 2.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 8);
        nameLabel.setBounds (r.removeFromTop (22));
        r.removeFromTop (4);
        sourceBox.setBounds (r.removeFromTop (24));        // Input above Output
        r.removeFromTop (4);
        outBox.setBounds (r.removeFromTop (24));
        r.removeFromTop (6);
        balance.setBounds (r.removeFromTop (54).withSizeKeepingCentre (54, 54));
        balText.setBounds (r.removeFromTop (16));
        r.removeFromTop (6);

        auto buttons = r.removeFromBottom (28);
        r.removeFromBottom (6);
        const int bw = (buttons.getWidth() - 8) / 3;
        muteButton.setBounds (buttons.removeFromLeft (bw));
        buttons.removeFromLeft (4);
        soloButton.setBounds (buttons.removeFromLeft (bw));
        buttons.removeFromLeft (4);
        scopeButton.setBounds (buttons);

        auto meterCol = r.removeFromRight (24);
        meter.setBounds (meterCol.withTrimmedBottom (22));
        r.removeFromRight (4);
        fader.setBounds (r);
    }

private:
    static juce::String balanceText (double v)
    {
        return std::abs (v) < 0.005 ? juce::String ("C") : (v < 0 ? "L" : "R") + juce::String ((int) std::round (std::abs (v) * 100.0));
    }

    static int sourceToId (Source s)
    {
        switch (s.type) { case SourceType::net: return 100 + s.index; case SourceType::app: return 200 + s.index; case SourceType::input: return 300 + s.index; default: return 1; }
    }
    static Source idToSource (int id)
    {
        Source s;
        if (id >= 300) s = { SourceType::input, id - 300 }; else if (id >= 200) s = { SourceType::app, id - 200 };
        else if (id >= 100) s = { SourceType::net, id - 100 }; else s = { SourceType::off, 0 };
        s.sanitize();
        return s;
    }

    juce::String appLabel (int slot) const
    {
        const auto& n = appNames[(size_t) slot];
        return "App " + juce::String (slot + 1) + (n.empty() ? juce::String() : " - " + juce::String::fromUTF8 (n.c_str()));
    }

    juce::String defaultName() const
    {
        switch (src.type)
        {
            case SourceType::net:   return pairName ("Net ", src.index);
            case SourceType::app:   { const auto& n = appNames[(size_t) src.index]; return n.empty() ? "App " + juce::String (src.index + 1) : juce::String::fromUTF8 (sanitizeStripName (n).c_str()); }
            case SourceType::input: return pairName ("In ", src.index);
            default:                return "Strip " + juce::String (idx + 1);
        }
    }

    void rebuildSourceMenu()
    {
        sourceBox.clear (juce::dontSendNotification);
        sourceBox.addItem ("Off", 1);
        sourceBox.addSeparator();
        sourceBox.addSectionHeading ("Network (Network page)");
        for (int p = 0; p < kNetPairs; ++p) sourceBox.addItem (pairName ("Net ", p), 100 + p);
        sourceBox.addSectionHeading ("Apps (virtual inputs)");
        for (int a = 0; a < kAppSlots; ++a) sourceBox.addItem (appLabel (a), 200 + a);
        sourceBox.addSectionHeading ("Audio input device");
        for (int p = 0; p < kDevicePairs; ++p) sourceBox.addItem (pairName ("In ", p), 300 + p);
        sourceBox.setSelectedId (sourceToId (src), juce::dontSendNotification);
    }

    void rebuildOutMenu()
    {
        outBox.clear (juce::dontSendNotification);
        outBox.addItem ("Master", 1);
        outBox.addSeparator();
        for (int b = 1; b <= kBuses; ++b) outBox.addItem ("Bus " + juce::String (b), 1 + b);
        outBox.setSelectedId (1 + clampStripOut (out), juce::dontSendNotification);
    }

    void push()
    {
        if (apply) apply (getStrip());
        changedCb();
    }

    int idx;
    std::function<void()> changedCb;
    Source src;
    int out = 0;
    bool nameIsDefault = true;
    std::array<std::string, kAppSlots> appNames;
    juce::Label nameLabel, balText;
    juce::ComboBox sourceBox, outBox;
    juce::Slider balance, fader;
    juce::TextButton muteButton { "M" }, soloButton { "S" }, scopeButton { "A" };
    Meter meter;
};

// ---------------------------------------------------------------------------------------------------------------
/** An output bus or the master: [name] [type: Audio Out / Net Out] [device pair, or the network destination] [M] [fader + meter]. */
class MixerPage::OutputStrip : public juce::Component
{
public:
    OutputStrip (bool isMaster, int index, std::function<void()> onChange)
        : master (isMaster), idx (index), changedCb (std::move (onChange)), meter (2)
    {
        styleNameLabel (nameLabel, ! master, master ? 15.0f : 13.0f, master);
        if (! master)
        {
            nameLabel.onTextChange = [this]
            {
                auto t = sanitizeStripName (nameLabel.getText().toStdString());
                if (t.empty()) t = defaultName().toStdString();
                nameLabel.setText (juce::String::fromUTF8 (t.c_str()), juce::dontSendNotification);
                push();
            };
        }
        nameLabel.setText (defaultName(), juce::dontSendNotification);
        addAndMakeVisible (nameLabel);

        typeBox.addItem ("Audio Out", 1);
        typeBox.addItem ("Net Out", 2);
        typeBox.setSelectedId (1, juce::dontSendNotification);
        typeBox.setTooltip ("Where this output goes: sound card outputs, or a PatchBay network stream");
        typeBox.onChange = [this]
        {
            const int id = typeBox.getSelectedId();
            if (id <= 0) return;
            type = id == 2 ? OutType::net : OutType::audio;
            updateDestControls();
            push();
            if (type == OutType::net && dest.host.empty()) editDestination();       // nothing is sent until there is somewhere to send
        };
        addAndMakeVisible (typeBox);

        outBox.onChange = [this]
        {
            const int id = outBox.getSelectedId();
            if (id <= 0) return;
            outFirst = id - 1000;
            push();
        };
        addAndMakeVisible (outBox);

        netButton.onClick = [this] { editDestination(); };
        netButton.setColour (juce::TextButton::buttonColourId, kNetTint.darker (0.6f));
        addChildComponent (netButton);

        muteButton.setClickingTogglesState (true);
        muteButton.setColour (juce::TextButton::buttonOnColourId, kBad.darker (0.2f));
        muteButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        muteButton.onClick = [this] { push(); };
        addAndMakeVisible (muteButton);

        if (! master)
        {
            styleScopeButton (scopeButton);
            scopeButton.onClick = [this] { if (onScope) onScope(); };
            addAndMakeVisible (scopeButton);
        }

        styleFader (fader, (master ? kMasterW : kStripW) - 20);
        fader.onValueChange = [this] { push(); };
        addAndMakeVisible (fader);
        addAndMakeVisible (meter);

        rebuildOutChoices (0);
        updateDestControls();
    }

    std::function<void (const BusSettings&)> apply;
    std::function<void()> onScope;                       // the A button (buses only)
    void setScopeOn (bool on) { scopeButton.setToggleState (on, juce::dontSendNotification); }
    juce::String displayName() const { return nameLabel.getText(); }

    BusSettings getBus() const
    {
        BusSettings b;
        b.gainDb = clampGainDb ((float) fader.getValue());
        b.mute = muteButton.getToggleState();
        b.type = type;
        b.outFirst = clampOutFirst (outFirst);
        b.dest = dest;
        if (! master)
        {
            const auto n = sanitizeStripName (nameLabel.getText().toStdString());
            b.name = n == defaultName().toStdString() ? std::string() : n;
        }
        return b;
    }

    void setBus (const BusSettings& b)
    {
        fader.setValue (b.gainDb, juce::dontSendNotification);
        muteButton.setToggleState (b.mute, juce::dontSendNotification);
        type = b.type;
        outFirst = clampOutFirst (b.outFirst);
        dest = b.dest;
        typeBox.setSelectedId (type == OutType::net ? 2 : 1, juce::dontSendNotification);
        if (! master) nameLabel.setText (b.name.empty() ? defaultName() : juce::String::fromUTF8 (b.name.c_str()), juce::dontSendNotification);
        rebuildOutChoices (lastNumOutputs);
        updateDestControls();
    }

    void setNumOutputs (int n) { rebuildOutChoices (n); }

    Meter& getMeter() { return meter; }

    void paint (juce::Graphics& g) override
    {
        g.setColour (master ? kPanelHi : kPanel);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
        g.setColour ((master ? kText : (type == OutType::net ? kNetTint : kGood)).withAlpha (0.8f));
        g.fillRoundedRectangle (getLocalBounds().toFloat().withHeight (4.0f).reduced (14.0f, 0.0f), 2.0f);
        if (clipShown)
        {
            g.setColour (kBad);
            g.fillRoundedRectangle (clipArea.toFloat(), 3.0f);
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
            g.drawText ("CLIP", clipArea, juce::Justification::centred);
        }
    }

    void setClip (bool on)
    {
        if (on == clipShown) return;
        clipShown = on;
        repaint (clipArea);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 8);
        nameLabel.setBounds (r.removeFromTop (22));
        r.removeFromTop (4);
        typeBox.setBounds (r.removeFromTop (24));
        r.removeFromTop (4);
        auto destRow = r.removeFromTop (24);
        outBox.setBounds (destRow);
        netButton.setBounds (destRow);
        r.removeFromTop (6);
        if (master) { clipArea = r.removeFromTop (18).withSizeKeepingCentre (46, 16); r.removeFromTop (4); }
        else r.removeFromTop (74);                                    // lines the fader up with the input strips' fader

        auto buttons = r.removeFromBottom (28);
        r.removeFromBottom (6);
        if (master) muteButton.setBounds (buttons);
        else
        {
            muteButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2 - 2));
            buttons.removeFromLeft (4);
            scopeButton.setBounds (buttons);
        }

        auto meterCol = r.removeFromRight (master ? 36 : 24);
        meter.setBounds (meterCol.withTrimmedBottom (22));
        r.removeFromRight (4);
        fader.setBounds (r);
    }

private:
    juce::String defaultName() const { return master ? juce::String ("Master") : "Bus " + juce::String (idx + 1); }

    void updateDestControls()
    {
        const bool net = type == OutType::net;
        outBox.setVisible (! net);
        netButton.setVisible (net);
        netButton.setButtonText (dest.host.empty() ? juce::String ("Set destination...")
                                                   : juce::String (dest.host) + ":" + juce::String (dest.port));
        netButton.setTooltip ("Network destination. Click to change.");
        repaint();
    }

    /** Audio Out: the output pairs of the open device. A saved choice the current device cannot provide stays selectable,
        marked "(not on this device)", so opening a different device never loses it. */
    void rebuildOutChoices (int numOutputs)
    {
        lastNumOutputs = numOutputs;
        const int n = std::clamp (numOutputs, 0, kMaxDeviceOutputs);
        outBox.clear (juce::dontSendNotification);
        const int pairs = std::max (1, n / 2);
        for (int p = 0; p < pairs; ++p) outBox.addItem ("Out " + juce::String (2 * p + 1) + "-" + juce::String (2 * p + 2), 1000 + 2 * p + 1);
        const int want = clampOutFirst (outFirst);
        if (outBox.indexOfItemId (1000 + want) < 0)
            outBox.addItem ("Out " + juce::String (want) + "-" + juce::String (want + 1) + " (not on this device)", 1000 + want);
        outBox.setSelectedId (1000 + want, juce::dontSendNotification);
    }

    /** A small dialog for host and port. Nothing is sent until a destination is set. */
    void editDestination()
    {
        auto* w = new juce::AlertWindow ("Net Out destination",
                                         "Send " + (master ? juce::String ("the master") : defaultNameForDialog()) + " as a PatchBay network stream (stereo) to:",
                                         juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor ("host", juce::String (dest.host), "Host name or IP address");
        w->addTextEditor ("port", juce::String (dest.port), "Port (1-65535)");
        w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Clear", 2);
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        juce::Component::SafePointer<OutputStrip> self (this);
        juce::Component::SafePointer<juce::AlertWindow> win (w);
        w->enterModalState (true, juce::ModalCallbackFunction::create ([self, win] (int result)
        {
            if (self == nullptr || win == nullptr || result == 0) return;
            if (result == 2) self->dest = NetDest();
            else
            {
                const auto host = win->getTextEditorContents ("host").trim().toStdString();
                const auto portText = win->getTextEditorContents ("port").trim();
                const int port = portText.getIntValue();
                const bool portOk = portText.containsOnly ("0123456789") && port >= 1 && port <= 65535;
                if (host.empty() || ! isValidHost (host) || ! portOk)
                {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Net Out destination",
                        "Enter a host name or IP address (letters, digits, '.', '-' and '_' only) and a port from 1 to 65535.");
                    return;
                }
                self->dest = { host, port };
            }
            self->updateDestControls();
            self->push();
        }), true);
    }

    juce::String defaultNameForDialog() const { return nameLabel.getText().isEmpty() ? defaultName() : nameLabel.getText(); }

    void push()
    {
        if (apply) apply (getBus());
        changedCb();
    }

    bool master;
    int idx;
    std::function<void()> changedCb;
    OutType type = OutType::audio;
    int outFirst = 1, lastNumOutputs = 0;
    NetDest dest;
    juce::Label nameLabel;
    juce::ComboBox typeBox, outBox;
    juce::TextButton netButton { "Set destination..." }, muteButton { "M" }, scopeButton { "A" };
    juce::Slider fader;
    Meter meter;
    juce::Rectangle<int> clipArea;
    bool clipShown = false;
};

// ---------------------------------------------------------------------------------------------------------------
/** One app-capture slot: pick a running app, switch capture on, choose whether to silence the app's own output.
    A strip whose Input is "App n" plays this slot. */
class MixerPage::AppSlotPanel : public juce::Component
{
public:
    AppSlotPanel (net::NetworkEngine& e, int slotIndex, std::function<void()> onChange)
        : engine (e), slot (slotIndex), changedCb (std::move (onChange))
    {
        title.setText ("App " + juce::String (slot + 1) + "  (stereo input)", juce::dontSendNotification);
        title.setColour (juce::Label::textColourId, kAppTint);
        title.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        addAndMakeVisible (title);

        appBox.setTextWhenNothingSelected ("Choose an app...");
        appBox.onChange = [this] { onAppChosen(); };
        addAndMakeVisible (appBox);

        captureButton.setClickingTogglesState (false);
        captureButton.onClick = [this] { toggleCapture(); };
        addAndMakeVisible (captureButton);

        silenceToggle.setToggleState (target.silenceOriginal, juce::dontSendNotification);
        silenceToggle.setColour (juce::ToggleButton::textColourId, kDim);
        silenceToggle.setColour (juce::ToggleButton::tickColourId, kAppTint);
        silenceToggle.onClick = [this]
        {
            target.silenceOriginal = silenceToggle.getToggleState();
            if (engine.isCaptureOn (slot)) { stopCapture(); startCapture(); }      // the mute behaviour is fixed when the tap is made
            changedCb();
        };
        addAndMakeVisible (silenceToggle);

        status.setColour (juce::Label::textColourId, kDim);
        status.setFont (juce::Font (juce::FontOptions (11.5f)));
        status.setJustificationType (juce::Justification::topLeft);
        status.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (status);

        supported = appCaptureSupported();
        refreshAppList();
        update();
    }

    AppTarget getTarget() const { return target; }

    void setTarget (const AppTarget& t)
    {
        if (t == target) return;                  // loading a preset that keeps this app must not interrupt a running capture
        const bool wasOn = engine.isCaptureOn (slot);
        if (wasOn) stopCapture();
        target = t;
        silenceToggle.setToggleState (t.silenceOriginal, juce::dontSendNotification);
        rebuildItems();                           // capture is never restarted automatically after loading settings
        update();
    }

    /** Re-reads which apps Core Audio knows about. Skipped while the popup is open (items must not change under it). */
    void refreshAppList()
    {
        if (appBox.isPopupActive() || ! supported) return;
        known = listCapturableApps();
        rebuildItems();
    }

    void update()
    {
        const bool on = engine.isCaptureOn (slot);
        captureButton.setButtonText (on ? "Stop capture" : "Capture");
        captureButton.setColour (juce::TextButton::buttonColourId, on ? kBad.darker (0.4f) : kAppTint.darker (0.5f));
        captureButton.setEnabled (supported && ! target.bundleId.empty());
        appBox.setEnabled (supported);
        silenceToggle.setEnabled (supported);

        juce::String text;
        if (! supported) text = "Needs macOS 14.2 or later. Not available on iOS/iPadOS.";
        else if (! errorText.isEmpty()) text = errorText;
        else if (on)
        {
            const auto st = engine.getCaptureStatus (slot);
            text = engine.getCaptureMessage (slot);
            if (engine.getCaptureState (slot) == AppCaptureSession::State::capturing)
                text += st.primed ? "\n" + juce::String (st.fillFrames / std::max (1.0, st.targetFrames) * 50.0, 0) + " ms buffered"   // target is 50 ms
                                  : "\nBuffering...";
        }
        else text = target.bundleId.empty() ? "Pick the browser or music app to use as an input." : "Ready. Press Capture.";
        if (status.getText() != text) status.setText (text, juce::dontSendNotification);
        statusIsError = errorText.isNotEmpty();
        status.setColour (juce::Label::textColourId, statusIsError ? kBad : kDim);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 6);
        title.setBounds (r.removeFromTop (18));
        r.removeFromTop (2);
        appBox.setBounds (r.removeFromTop (24));
        r.removeFromTop (4);
        auto row = r.removeFromTop (24);
        captureButton.setBounds (row.removeFromLeft (84));
        row.removeFromLeft (6);
        silenceToggle.setBounds (row);
        r.removeFromTop (2);
        status.setBounds (r);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (kPanel);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
        g.setColour (kAppTint.withAlpha (0.8f));
        g.fillRoundedRectangle (getLocalBounds().toFloat().withHeight (4.0f).reduced (14.0f, 0.0f), 2.0f);
    }

private:
    void rebuildItems()
    {
        appItems.clear();
        appBox.clear (juce::dontSendNotification);
        appBox.addItem ("(none)", 1);
        int id = 2;
        bool found = target.bundleId.empty();
        for (const auto& a : known)
        {
            appItems.push_back (a.groupId);
            appBox.addItem (juce::String::fromUTF8 (a.name.c_str()) + (a.outputting ? "   (playing)" : ""), id++);
            found = found || a.groupId == target.bundleId;
        }
        if (! found)                                // a saved choice that is not running now stays selectable
        {
            appItems.push_back (target.bundleId);
            const auto nm = target.name.empty() ? fallbackAppName (target.bundleId) : target.name;
            appBox.addItem (juce::String::fromUTF8 (nm.c_str()) + "   (not running)", id++);
        }

        int sel = 1;
        for (size_t i = 0; i < appItems.size(); ++i) if (appItems[i] == target.bundleId) sel = (int) i + 2;
        appBox.setSelectedId (sel, juce::dontSendNotification);
    }

    void onAppChosen()
    {
        const int id = appBox.getSelectedId();
        const bool wasOn = engine.isCaptureOn (slot);
        if (wasOn) stopCapture();
        errorText.clear();

        if (id <= 1 || (size_t) (id - 2) >= appItems.size()) target = AppTarget { {}, {}, target.silenceOriginal };
        else
        {
            const auto& gid = appItems[(size_t) (id - 2)];
            target.bundleId = gid;
            target.name.clear();
            for (const auto& a : known) if (a.groupId == gid) target.name = sanitizeText (a.name, kMaxAppNameLength);
        }
        if (wasOn && ! target.bundleId.empty()) startCapture();       // switching apps while capturing follows the choice
        update();
        changedCb();
    }

    void toggleCapture()
    {
        errorText.clear();
        if (engine.isCaptureOn (slot)) stopCapture(); else startCapture();
        update();
    }

    void startCapture()
    {
        juce::String err;
        if (! engine.startCapture (slot, target.bundleId, target.silenceOriginal, err)) errorText = err;
    }

    void stopCapture() { engine.stopCapture (slot); }

    net::NetworkEngine& engine;
    int slot;
    std::function<void()> changedCb;
    AppTarget target;
    std::vector<AppChoice> known;
    std::vector<std::string> appItems;
    juce::Label title, status;
    juce::ComboBox appBox;
    juce::TextButton captureButton { "Capture" };
    juce::ToggleButton silenceToggle { "Silence app's own output" };
    juce::String errorText;
    bool supported = false, statusIsError = false;
};

// ---------------------------------------------------------------------------------------------------------------
/** Audio input device (a microphone, an interface, or a virtual driver such as BlackHole): its channels are the stereo
    pairs "In 1-2", "In 3-4", ... that a strip can take as its Input.
    Off after every launch: switching it on is what makes the OS ask for microphone access. */
class MixerPage::InputPanel : public juce::Component
{
public:
    explicit InputPanel (net::NetworkEngine& e) : engine (e)
    {
        title.setText ("Audio input device", juce::dontSendNotification);
        title.setColour (juce::Label::textColourId, kInTint);
        title.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        addAndMakeVisible (title);

        useButton.onClick = [this] { toggle(); };
        addAndMakeVisible (useButton);

        deviceButton.onClick = [this] { openDialog(); };
        addAndMakeVisible (deviceButton);

        status.setColour (juce::Label::textColourId, kDim);
        status.setFont (juce::Font (juce::FontOptions (11.5f)));
        status.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (status);
        update();
    }

    ~InputPanel() override { if (dialog != nullptr) dialog.deleteAndZero(); }   // it references the engine's device manager

    void update()
    {
        const bool on = engine.areDeviceInputsEnabled();
        useButton.setButtonText (on ? "Stop using input" : "Use input device");
        useButton.setColour (juce::TextButton::buttonColourId, on ? kBad.darker (0.4f) : kInTint.darker (0.6f));

        juce::String text;
        if (errorText.isNotEmpty()) text = errorText;
        else if (on)
        {
            const int n = engine.getNumInputs();
            auto* d = engine.getDeviceManager().getCurrentAudioDevice();
            text = (d != nullptr ? d->getName() : juce::String ("Audio device")) + "\n"
                   + (n > 0 ? juce::String (n) + " input channel" + (n == 1 ? "" : "s") + " (pairs In 1-2" + (n > 2 ? " ... In " + juce::String ((n - 1) / 2 * 2 + 1) + "-" + juce::String ((n - 1) / 2 * 2 + 2) : juce::String()) + ")"
                            : "No input channels are active. Pick an input in Audio device...")
                   + "\nUse headphones: a microphone feeding speakers will howl.";
        }
        else text = "Off. Turning it on asks for microphone access the first time. Works with microphones, interfaces and virtual drivers such as BlackHole.";
        if (status.getText() != text) status.setText (text, juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, errorText.isNotEmpty() ? kBad : kDim);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 6);
        title.setBounds (r.removeFromTop (18));
        r.removeFromTop (2);
        auto row = r.removeFromTop (26);
        useButton.setBounds (row.removeFromLeft (118));
        row.removeFromLeft (6);
        deviceButton.setBounds (row.removeFromLeft (116));
        r.removeFromTop (4);
        status.setBounds (r);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (kPanel);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
        g.setColour (kInTint.withAlpha (0.8f));
        g.fillRoundedRectangle (getLocalBounds().toFloat().withHeight (4.0f).reduced (14.0f, 0.0f), 2.0f);
    }

private:
    void toggle()
    {
        errorText.clear();
        juce::String err;
        if (! engine.setDeviceInputsEnabled (! engine.areDeviceInputsEnabled(), err)) errorText = err;
        update();
    }

    void openDialog()
    {
        if (dialog != nullptr) { dialog->toFront (true); return; }
        if (auto err = engine.prepareDevice(); err.isNotEmpty()) { errorText = "Audio device: " + err; update(); return; }

        // Inputs are listed here so they can be chosen; merely opening this dialog does not open an input.
        auto* selector = new juce::AudioDeviceSelectorComponent (engine.getDeviceManager(), 0, kMaxDeviceInputs, 2, kMaxDeviceOutputs, false, false, true, false);
        selector->setSize (560, 520);

        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (selector);
        o.dialogTitle = "Audio device";
        o.dialogBackgroundColour = kPanel;
        o.componentToCentreAround = this;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        dialog = o.launchAsync();
    }

    net::NetworkEngine& engine;
    juce::Label title, status;
    juce::TextButton useButton { "Use input device" }, deviceButton { "Audio device..." };
    juce::String errorText;
    juce::Component::SafePointer<juce::DialogWindow> dialog;
};

// ---------------------------------------------------------------------------------------------------------------
MixerPage::MixerPage (net::NetworkEngine& e) : engine (e), core (e.getMixer())
{
    setLookAndFeel (&ssl);
    for (int i = 0; i < kStrips; ++i)
    {
        auto s = std::make_unique<InputStrip> (i, [this] { changed(); });
        s->apply = [this, i] (const StripSettings& st) { applyStrip (i, st); };
        s->onScope = [this, i] { selectScope (i); };
        content.addAndMakeVisible (*s);
        content.strips.push_back (s.get());
        strips.push_back (std::move (s));
    }

    for (int b = 0; b < kBuses; ++b)
    {
        auto s = std::make_unique<OutputStrip> (false, b, [this] { changed(); });
        s->apply = [this, b] (const BusSettings& bs) { applyBus (b, bs); };
        s->onScope = [this, b] { selectScope (kStrips + b); };
        content.addAndMakeVisible (*s);
        content.buses.push_back (s.get());
        busStrips.push_back (std::move (s));
    }

    master = std::make_unique<OutputStrip> (true, 0, [this] { changed(); });
    master->apply = [this] (const BusSettings&) { applyMaster(); };
    addAndMakeVisible (*master);

    for (int s = 0; s < kAppSlots; ++s)
    {
        auto p = std::make_unique<AppSlotPanel> (engine, s, [this] { changed(); });
        content.addAndMakeVisible (*p);
        content.slots.push_back (p.get());
        slotPanels.push_back (std::move (p));
    }
    inputPanel = std::make_unique<InputPanel> (engine);
    content.addAndMakeVisible (*inputPanel);
    content.inputPanel = inputPanel.get();

    analyzer = std::make_unique<AnalyzerPanel> (core, [this] { return engine.getSampleRate(); });
    addAndMakeVisible (*analyzer);

    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, true);
    addAndMakeVisible (viewport);

    // Preset bar
    presetTitle.setText ("Preset", juce::dontSendNotification);
    presetTitle.setColour (juce::Label::textColourId, kDim);
    addAndMakeVisible (presetTitle);
    presetBox.setTextWhenNothingSelected ("(none)");
    presetBox.onChange = [this]
    {
        if (presetLoadInProgress) return;
        const auto name = presetBox.getText();
        if (presetBox.getSelectedId() > 1 && name.isNotEmpty()) loadPreset (name);
    };
    addAndMakeVisible (presetBox);
    saveButton.onClick = [this] { if (loadedPreset.isEmpty()) promptSaveAs(); else savePresetAs (loadedPreset); };
    saveAsButton.onClick = [this] { promptSaveAs(); };
    deleteButton.onClick = [this] { promptDelete(); };
    initButton.setTooltip ("Reset every strip, bus and the master to the factory layout");
    initButton.onClick = [this]
    {
        juce::Component::SafePointer<MixerPage> self (this);
        auto* w = new juce::AlertWindow ("Reset the mixer?", "Every strip, bus and master setting goes back to the defaults. Presets on disk are not touched.",
                                         juce::MessageBoxIconType::QuestionIcon, this);
        w->addButton ("Reset", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([self] (int r)
        {
            if (self == nullptr || r != 1) return;
            self->setSettings (MixerSettings());
            self->loadedPreset.clear();
            self->refreshPresetList ({});
            self->changed();
        }), true);
    };
    for (auto* b : { &saveButton, &saveAsButton, &deleteButton, &initButton }) addAndMakeVisible (*b);
    presetStatus.setColour (juce::Label::textColourId, kDim);
    presetStatus.setFont (juce::Font (juce::FontOptions (12.0f)));
    addAndMakeVisible (presetStatus);
    refreshPresetList ({});

    setSettings (MixerSettings());
    syncOutputChoices();
    selectScope (0);                                      // the analyzer starts on strip 1
    startTimerHz (30);
}

MixerPage::~MixerPage() { stopTimer(); analyzer.reset(); setLookAndFeel (nullptr); }

MixerSettings MixerPage::getSettings() const
{
    MixerSettings s;
    for (int i = 0; i < kStrips; ++i) s.strips[(size_t) i] = strips[(size_t) i]->getStrip();
    for (int b = 0; b < kBuses; ++b) s.buses[(size_t) b] = busStrips[(size_t) b]->getBus();
    const auto m = master->getBus();
    s.masterDb = m.gainDb; s.masterMute = m.mute; s.masterType = m.type; s.masterOut = m.outFirst; s.masterDest = m.dest;
    for (int i = 0; i < kAppSlots; ++i) s.apps[(size_t) i] = slotPanels[(size_t) i]->getTarget();
    s.sanitize();
    return s;
}

void MixerPage::setSettings (const MixerSettings& in)
{
    MixerSettings s = in;
    s.sanitize();
    for (int i = 0; i < kStrips; ++i) strips[(size_t) i]->setStrip (s.strips[(size_t) i]);
    for (int b = 0; b < kBuses; ++b) busStrips[(size_t) b]->setBus (s.buses[(size_t) b]);
    BusSettings m;
    m.gainDb = s.masterDb; m.mute = s.masterMute; m.type = s.masterType; m.outFirst = s.masterOut; m.dest = s.masterDest;
    master->setBus (m);
    for (int i = 0; i < kAppSlots; ++i) slotPanels[(size_t) i]->setTarget (s.apps[(size_t) i]);
    applyAppNames();

    core.setSettings (s);
    engine.setNetOutDestination (0, s.masterType == OutType::net ? s.masterDest.host : std::string(), s.masterDest.port);
    for (int b = 0; b < kBuses; ++b)
    {
        const auto& bs = s.buses[(size_t) b];
        engine.setNetOutDestination (b + 1, bs.type == OutType::net ? bs.dest.host : std::string(), bs.dest.port);
    }
    updatePresetStatus();
}

void MixerPage::applyStrip (int i, const StripSettings& st)
{
    core.setStripSource (i, st.src); core.setStripGainDb (i, st.gainDb); core.setStripBalance (i, st.balance);
    core.setStripMute (i, st.mute); core.setStripSolo (i, st.solo); core.setStripOut (i, st.out);
}

void MixerPage::applyBus (int b, const BusSettings& bs)
{
    core.setBusGainDb (b, bs.gainDb); core.setBusMute (b, bs.mute); core.setBusType (b, bs.type); core.setBusOutFirst (b, bs.outFirst);
    engine.setNetOutDestination (b + 1, bs.type == OutType::net ? bs.dest.host : std::string(), bs.dest.port);
}

void MixerPage::applyMaster()
{
    const auto m = master->getBus();
    core.setMasterDb (m.gainDb); core.setMasterMute (m.mute); core.setMasterType (m.type); core.setMasterOut (m.outFirst);
    engine.setNetOutDestination (0, m.type == OutType::net ? m.dest.host : std::string(), m.dest.port);
}

void MixerPage::applyAppNames()
{
    for (int i = 0; i < kAppSlots; ++i)
    {
        const auto t = slotPanels[(size_t) i]->getTarget();
        appNames[(size_t) i] = t.bundleId.empty() ? std::string() : (t.name.empty() ? fallbackAppName (t.bundleId) : t.name);
    }
    for (auto& s : strips) s->setAppNames (appNames);
}

void MixerPage::selectScope (int target)
{
    for (int i = 0; i < kStrips; ++i) strips[(size_t) i]->setScopeOn (i == target);
    for (int b = 0; b < kBuses; ++b) busStrips[(size_t) b]->setScopeOn (kStrips + b == target);
    analyzer->setTarget (target, scopeName (target));
}

juce::String MixerPage::scopeName (int target) const
{
    if (target >= 0 && target < kStrips) return strips[(size_t) target]->displayName();
    if (target >= kStrips && target < kStrips + kBuses) return busStrips[(size_t) (target - kStrips)]->displayName();
    return {};
}

void MixerPage::changed()
{
    if (analyzer != nullptr) analyzer->setTargetName (analyzer->getTarget(), scopeName (analyzer->getTarget()));
    applyAppNames();
    updatePresetStatus();
    if (onSettingsChanged) onSettingsChanged();
}

void MixerPage::syncOutputChoices()
{
    const int n = engine.getNumOutputs();
    if (n == lastNumOutputs) return;
    lastNumOutputs = n;
    for (auto& b : busStrips) b->setNumOutputs (n);
    master->setNumOutputs (n);
}

// ---- presets -------------------------------------------------------------------------------------------------------
void MixerPage::refreshPresetList (const juce::String& select)
{
    presetLoadInProgress = true;
    presetBox.clear (juce::dontSendNotification);
    presetBox.addItem ("(none)", 1);
    const auto names = presets.list();
    for (int i = 0; i < names.size(); ++i) presetBox.addItem (names[i], i + 2);
    int sel = 1;
    for (int i = 0; i < names.size(); ++i) if (names[i] == select) sel = i + 2;
    presetBox.setSelectedId (sel, juce::dontSendNotification);
    presetLoadInProgress = false;
    deleteButton.setEnabled (loadedPreset.isNotEmpty());
    updatePresetStatus();
}

void MixerPage::updatePresetStatus()
{
    juce::String text;
    if (loadedPreset.isEmpty()) text = "No preset loaded";
    else text = loadedPreset + (getSettings() == loadedSettings ? juce::String() : juce::String ("  (edited - Save to update)"));
    if (presetStatus.getText() != text) presetStatus.setText (text, juce::dontSendNotification);
    deleteButton.setEnabled (loadedPreset.isNotEmpty());
}

void MixerPage::loadPreset (const juce::String& name)
{
    MixerSettings s;
    juce::String err;
    if (! presets.load (name.toStdString(), s, &err))
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Load preset", err);
        refreshPresetList (loadedPreset);
        return;
    }
    // Loading never starts anything: app capture, input and network listening stay as they are, and a preset's Net Out
    // destinations only take effect once audio is running.
    setSettings (s);
    loadedPreset = name;
    loadedSettings = getSettings();
    refreshPresetList (name);
    if (onSettingsChanged) onSettingsChanged();
}

void MixerPage::savePresetAs (const juce::String& rawName)
{
    const auto clean = sanitizePresetName (rawName.toStdString());
    juce::String err;
    const auto current = getSettings();
    if (! presets.save (clean, current, &err))
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save preset", err);
        return;
    }
    loadedPreset = juce::String::fromUTF8 (clean.c_str());
    loadedSettings = current;
    refreshPresetList (loadedPreset);
}

void MixerPage::promptSaveAs()
{
    auto* w = new juce::AlertWindow ("Save mixer preset", "Name for this preset. Saving over an existing name replaces it.",
                                     juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor ("name", loadedPreset, "Preset name");
    w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MixerPage> self (this);
    juce::Component::SafePointer<juce::AlertWindow> win (w);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([self, win] (int r)
    {
        if (self == nullptr || win == nullptr || r != 1) return;
        const auto clean = sanitizePresetName (win->getTextEditorContents ("name").toStdString());
        if (clean.empty())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save preset",
                                                    "Use letters, digits, spaces, '-', '_', '.', '(' or ')' in the name.");
            return;
        }
        self->savePresetAs (juce::String::fromUTF8 (clean.c_str()));
    }), true);
}

void MixerPage::promptDelete()
{
    if (loadedPreset.isEmpty()) return;
    auto* w = new juce::AlertWindow ("Delete preset?", "Delete the preset \"" + loadedPreset + "\" from disk? The mixer itself is not changed.",
                                     juce::MessageBoxIconType::QuestionIcon, this);
    w->addButton ("Delete", 1);
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MixerPage> self (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([self] (int r)
    {
        if (self == nullptr || r != 1) return;
        self->presets.remove (self->loadedPreset.toStdString());
        self->loadedPreset.clear();
        self->refreshPresetList ({});
    }), true);
}

// ---- timer / layout ------------------------------------------------------------------------------------------------
void MixerPage::timerCallback()
{
    const auto p = core.takePeaks();                      // always drain, so a hidden page doesn't keep a stale clip flag
    ++tick;
    if (tick % 30 == 0)                                   // ~1 Hz: follow apps whose audio processes come and go
    {
        engine.refreshCaptures();
        for (auto& sp : slotPanels) sp->update();
        inputPanel->update();
        updatePresetStatus();
    }
    if (tick % 90 == 0) for (auto& sp : slotPanels) sp->refreshAppList();     // ~3 s: apps started or quit
    if (tick % 15 == 0) syncOutputChoices();

    if (! isShowing()) return;

    constexpr float dt = 1.0f / 30.0f;
    for (int i = 0; i < kStrips; ++i)
    {
        const float lr[2] { p.stripL[(size_t) i], p.stripR[(size_t) i] };
        strips[(size_t) i]->getMeter().update (lr, dt);
    }
    for (int b = 0; b < kBuses; ++b)
    {
        const float lr[2] { p.busL[(size_t) b], p.busR[(size_t) b] };
        busStrips[(size_t) b]->getMeter().update (lr, dt);
    }
    const float m[2] { p.masterL, p.masterR };
    master->getMeter().update (m, dt);
    if (p.clipped) { clipHoldTicks = 60; master->setClip (true); }
    else if (clipHoldTicks > 0 && --clipHoldTicks == 0) master->setClip (false);
}

void MixerPage::resized()
{
    auto r = getLocalBounds();
    auto bar = r.removeFromTop (40).reduced (kMargin, 6);
    presetTitle.setBounds (bar.removeFromLeft (52));
    presetBox.setBounds (bar.removeFromLeft (220));
    bar.removeFromLeft (8);
    saveButton.setBounds (bar.removeFromLeft (64));
    bar.removeFromLeft (4);
    saveAsButton.setBounds (bar.removeFromLeft (84));
    bar.removeFromLeft (4);
    deleteButton.setBounds (bar.removeFromLeft (64));
    bar.removeFromLeft (4);
    initButton.setBounds (bar.removeFromLeft (56));
    bar.removeFromLeft (12);
    presetStatus.setBounds (bar);

    analyzer->setBounds (r.removeFromTop (kAnalyzerH).reduced (kMargin - 6, 0));

    // The master stays in view on the right while the strips and buses scroll.
    auto masterArea = r.removeFromRight (kMasterW + 2 * kMargin);
    master->setBounds (masterArea.getX() + kMargin, r.getY() + kMargin + kHeaderH + kGap + kTitleH, kMasterW, kStripH);

    viewport.setBounds (r);
    const int contentW = juce::jmax (content.inputsRight() + kGroupGap + kBuses * (kStripW + kGap) + kMargin, viewport.getMaximumVisibleWidth());
    content.setSize (contentW, juce::jmax (kHeaderH + kStripH + kTitleH + kGap + 2 * kMargin, viewport.getMaximumVisibleHeight()));
}

void MixerPage::paint (juce::Graphics& g) { g.fillAll (kBg); }

int MixerPage::Content::inputsRight() const { return kMargin + kStrips * (kStripW + kGap) - kGap; }

void MixerPage::Content::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
    const int y = kMargin + kHeaderH + kGap;
    g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    g.setColour (kAccent);
    g.drawText ("INPUTS - 32 stereo strips", kMargin + 4, y, 400, kTitleH, juce::Justification::centredLeft);
    g.setColour (kGood);
    g.drawText ("OUTPUT BUSES - 32 stereo", inputsRight() + kGroupGap + 4, y, 400, kTitleH, juce::Justification::centredLeft);
    g.setColour (kDim.withAlpha (0.5f));
    g.drawVerticalLine (inputsRight() + kGroupGap / 2, (float) y, (float) (y + kTitleH + kStripH));
}

void MixerPage::Content::resized()
{
    // Sources row (apps + input device) across the top; the strips and buses sit below it.
    const int top = kMargin + kHeaderH + kGap + kTitleH;
    int x = kMargin;
    for (int i = 0; i < kStrips; ++i)
    {
        strips[(size_t) i]->setBounds (x, top, kStripW, kStripH);
        x += kStripW + (i + 1 < kStrips ? kGap : 0);
    }
    x += kGroupGap;
    for (int b = 0; b < kBuses; ++b)
    {
        buses[(size_t) b]->setBounds (x, top, kStripW, kStripH);
        x += kStripW + kGap;
    }

    int sx = kMargin;
    for (auto* s : slots) { s->setBounds (sx, kMargin, 2 * kStripW + kGap, kHeaderH - kGap); sx += 2 * kStripW + 2 * kGap; }
    if (inputPanel != nullptr) inputPanel->setBounds (sx, kMargin, 3 * kStripW + 2 * kGap, kHeaderH - kGap);
}
}
