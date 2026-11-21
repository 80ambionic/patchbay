#include "MixerPage.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include "Capture/AppGrouping.h"

namespace pb
{
namespace
{
const juce::Colour kBg (0xff15171b), kAppTint (0xff6ccfb8), kInTint (0xffe2b05a), kPanel (0xff23272e), kPanelHi (0xff2a2e35), kText (0xffd8dce2), kDim (0xff8a8f98),
                   kGood (0xff6ccf8e), kWarn (0xfff0a35a), kBad (0xffe06c6c), kAccent (0xff5b9bd5);

constexpr int kStripW = 88, kMasterW = 116, kGap = 6, kMargin = 14, kGroupGap = 18, kHeaderH = 150, kStripH = 470;
constexpr float kMeterFloorDb = -60.0f, kMeterCeilDb = 6.0f;

float toDb (float lin) noexcept { return lin <= 1.0e-6f ? -120.0f : 20.0f * std::log10 (lin); }
float dbToMeterPos (float db) noexcept { return juce::jlimit (0.0f, 1.0f, (db - kMeterFloorDb) / (kMeterCeilDb - kMeterFloorDb)); }

juce::String faderText (double db) { return db <= (double) kMinGainDb + 0.001 ? juce::String ("-inf") : juce::String (db, 1) + " dB"; }
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
class MixerPage::Strip : public juce::Component
{
public:
    Strip (bool isMaster, int index, std::function<void()> onChange)
        : master (isMaster), idx (index), changedCb (std::move (onChange)), meter (isMaster ? 2 : 1)
    {
        outBox.onChange = [this]
        {
            const int id = outBox.getSelectedId();
            if (id <= 0) return;
            if (master) masterOutCh = id - 1000; else directOut = id - 1;          // ids: strips 1 = master, 2.. = Out 1..; master pair: 1000 + first channel
            push();
        };
        addAndMakeVisible (outBox);
        rebuildOutChoices (0);

        if (! master)
        {
            nameLabel.setEditable (false, true, false);               // double-click to rename
            nameLabel.setJustificationType (juce::Justification::centred);
            nameLabel.setColour (juce::Label::textColourId, kText);
            nameLabel.setColour (juce::Label::outlineWhenEditingColourId, kAccent);
            nameLabel.onTextChange = [this]
            {
                auto t = sanitizeStripName (nameLabel.getText().toStdString());
                if (t.empty()) t = defaultName().toStdString();
                nameLabel.setText (juce::String::fromUTF8 (t.c_str()), juce::dontSendNotification);
                changedCb();
            };
            nameLabel.setText (defaultName(), juce::dontSendNotification);
            addAndMakeVisible (nameLabel);

            pan.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            pan.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f, juce::MathConstants<float>::pi * 2.8f, true);
            pan.setRange (-1.0, 1.0, 0.01);
            pan.setDoubleClickReturnValue (true, 0.0);
            pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            pan.setColour (juce::Slider::rotarySliderFillColourId, kAccent);
            pan.setColour (juce::Slider::thumbColourId, kText);
            pan.textFromValueFunction = [] (double v) { return std::abs (v) < 0.005 ? juce::String ("C") : (v < 0 ? "L" : "R") + juce::String ((int) std::round (std::abs (v) * 100.0)); };
            pan.onValueChange = [this] { panText.setText ("Pan " + pan.getTextFromValue (pan.getValue()), juce::dontSendNotification); push(); };
            addAndMakeVisible (pan);

            panText.setJustificationType (juce::Justification::centred);
            panText.setColour (juce::Label::textColourId, kDim);
            panText.setFont (juce::Font (juce::FontOptions (11.5f)));
            addAndMakeVisible (panText);

            soloButton.setClickingTogglesState (true);
            soloButton.setColour (juce::TextButton::buttonOnColourId, kWarn.darker (0.2f));
            soloButton.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            soloButton.onClick = [this] { push(); };
            addAndMakeVisible (soloButton);
        }
        else
        {
            nameLabel.setText ("Master", juce::dontSendNotification);
            nameLabel.setJustificationType (juce::Justification::centred);
            nameLabel.setColour (juce::Label::textColourId, kText);
            nameLabel.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
            addAndMakeVisible (nameLabel);
        }

        muteButton.setClickingTogglesState (true);
        muteButton.setColour (juce::TextButton::buttonOnColourId, kBad.darker (0.2f));
        muteButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        muteButton.onClick = [this] { push(); };
        addAndMakeVisible (muteButton);

        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.setRange (kMinGainDb, kMaxGainDb, 0.1);
        fader.setDoubleClickReturnValue (true, 0.0);
        fader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, kStripW - 20, 22);
        fader.textFromValueFunction = faderText;
        fader.valueFromTextFunction = [] (const juce::String& t)
        {
            const auto s = t.trim().toLowerCase();
            return s.startsWith ("-inf") ? (double) kMinGainDb : (double) clampGainDb (s.getFloatValue());
        };
        fader.setColour (juce::Slider::trackColourId, kPanelHi);
        fader.setColour (juce::Slider::thumbColourId, kAccent);
        fader.setColour (juce::Slider::textBoxTextColourId, kText);
        fader.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        fader.setValue (0.0, juce::dontSendNotification);
        fader.onValueChange = [this] { push(); };
        addAndMakeVisible (fader);
        addAndMakeVisible (meter);
    }

    /** Called by the page after construction: where this strip's controls write. */
    std::function<void (const StripSettings&)> apply;      // channel strips
    std::function<void (float, bool, int)> applyMaster;     // master strip: gain dB, mute, first output channel

    StripSettings getStrip() const
    {
        StripSettings s;
        s.gainDb = clampGainDb ((float) fader.getValue());
        s.pan = clampPan ((float) pan.getValue());
        s.mute = muteButton.getToggleState();
        s.solo = soloButton.getToggleState();
        s.directOut = clampDirectOut (directOut);
        const auto n = sanitizeStripName (nameLabel.getText().toStdString());
        s.name = n == defaultName().toStdString() ? std::string() : n;
        return s;
    }

    void setStrip (const StripSettings& s)
    {
        fader.setValue (s.gainDb, juce::dontSendNotification);
        pan.setValue (s.pan, juce::dontSendNotification);
        panText.setText ("Pan " + pan.getTextFromValue (s.pan), juce::dontSendNotification);
        muteButton.setToggleState (s.mute, juce::dontSendNotification);
        soloButton.setToggleState (s.solo, juce::dontSendNotification);
        directOut = clampDirectOut (s.directOut);
        rebuildOutChoices (lastNumOutputs);
        nameLabel.setText (s.name.empty() ? defaultName() : juce::String::fromUTF8 (s.name.c_str()), juce::dontSendNotification);
    }

    void setMaster (float db, bool mute, int outFirst)
    {
        fader.setValue (db, juce::dontSendNotification);
        muteButton.setToggleState (mute, juce::dontSendNotification);
        masterOutCh = clampMasterOut (outFirst);
        rebuildOutChoices (lastNumOutputs);
    }
    int masterOut() const { return clampMasterOut (masterOutCh); }

    /** The device's output channel count changed (or the first query arrived): rebuild the routing menu. */
    void setNumOutputs (int n) { rebuildOutChoices (n); }
    float masterDb() const { return clampGainDb ((float) fader.getValue()); }
    bool masterMute() const { return muteButton.getToggleState(); }

    Meter& getMeter() { return meter; }
    bool isMaster() const noexcept { return master; }

    void paint (juce::Graphics& g) override
    {
        g.setColour (master ? kPanelHi : kPanel);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
        g.setColour ((master ? kText : (idx < kNetworkStrips ? kAccent : (idx < kNetworkStrips + kAppStrips ? kAppTint : kInTint))).withAlpha (0.8f));
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
        nameLabel.setBounds (r.removeFromTop (24));
        if (! master)
        {
            r.removeFromTop (4);
            auto knob = r.removeFromTop (54);
            pan.setBounds (knob.withSizeKeepingCentre (54, 54));
            panText.setBounds (r.removeFromTop (16));
        }
        else
        {
            clipArea = r.removeFromTop (18).withSizeKeepingCentre (46, 16);
            r.removeFromTop (4);
        }
        r.removeFromTop (4);
        outBox.setBounds (r.removeFromTop (24));
        r.removeFromTop (6);

        auto buttons = r.removeFromBottom (28);
        r.removeFromBottom (6);
        if (! master)
        {
            muteButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2 - 2));
            buttons.removeFromLeft (4);
            soloButton.setBounds (buttons);
        }
        else
            muteButton.setBounds (buttons);

        // Fader and meter side by side; the fader's text box sits under the fader only.
        auto meterCol = r.removeFromRight (master ? 36 : 16);
        meter.setBounds (meterCol.withTrimmedBottom (22));
        r.removeFromRight (4);
        fader.setBounds (r);
    }

private:
    juce::String defaultName() const
    {
        if (idx < kNetworkStrips) return "Net " + juce::String (idx + 1);
        if (idx >= kNetworkStrips + kAppStrips) return "In " + juce::String (idx - kNetworkStrips - kAppStrips + 1);
        const int a = idx - kNetworkStrips;
        return "App" + juce::String (a / 2 + 1) + (a % 2 == 0 ? " L" : " R");
    }

    /** Menu: strips get "Master" + "Out 1..N"; the master gets the output pairs. A saved choice the current device
        cannot provide stays selectable, marked "(not on this device)", so opening a different device never loses it. */
    void rebuildOutChoices (int numOutputs)
    {
        lastNumOutputs = numOutputs;
        const int n = std::clamp (numOutputs, 0, kMaxDeviceOutputs);
        outBox.clear (juce::dontSendNotification);
        if (master)
        {
            const int pairs = std::max (1, n / 2);
            for (int p = 0; p < pairs; ++p) outBox.addItem ("Out " + juce::String (2 * p + 1) + "-" + juce::String (2 * p + 2), 1000 + 2 * p + 1);
            const int want = clampMasterOut (masterOutCh);
            if (outBox.indexOfItemId (1000 + want) < 0)
                outBox.addItem ("Out " + juce::String (want) + "-" + juce::String (want + 1) + " (not on this device)", 1000 + want);
            outBox.setSelectedId (1000 + want, juce::dontSendNotification);
        }
        else
        {
            outBox.addItem ("Master", 1);
            for (int c = 1; c <= n; ++c) outBox.addItem ("Out " + juce::String (c), 1 + c);
            const int want = clampDirectOut (directOut);
            if (want > 0 && outBox.indexOfItemId (1 + want) < 0)
                outBox.addItem ("Out " + juce::String (want) + " (not on this device)", 1 + want);
            outBox.setSelectedId (1 + want, juce::dontSendNotification);
        }
    }

    void push()
    {
        if (master) { if (applyMaster) applyMaster (masterDb(), masterMute(), masterOut()); }
        else if (apply) apply (getStrip());
        changedCb();
    }

    bool master;
    int idx;
    std::function<void()> changedCb;
    juce::Label nameLabel, panText;
    juce::ComboBox outBox;
    int directOut = 0, masterOutCh = 1, lastNumOutputs = 0;
    juce::Slider pan, fader;
    juce::TextButton muteButton { "M" }, soloButton { "S" };
    Meter meter;
    juce::Rectangle<int> clipArea;
    bool clipShown = false;
};

// ---------------------------------------------------------------------------------------------------------------
/** One app-capture slot: pick a running app, switch capture on, choose whether to silence the app's own output.
    Feeds the two strips below it (slot L / slot R). */
class MixerPage::AppSlotPanel : public juce::Component
{
public:
    AppSlotPanel (net::NetworkEngine& e, int slotIndex, std::function<void()> onChange)
        : engine (e), slot (slotIndex), changedCb (std::move (onChange))
    {
        title.setText ("App " + juce::String (slot + 1) + "  (virtual input)", juce::dontSendNotification);
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
/** Audio input device (a microphone, an interface, or a virtual driver such as BlackHole) feeding strips 17-24.
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
                   + (n > 0 ? juce::String (n) + " input channel" + (n == 1 ? "" : "s") + " on strips In 1-" + juce::String (n)
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
        auto* selector = new juce::AudioDeviceSelectorComponent (engine.getDeviceManager(), 0, kInputStrips, 2, kMaxDeviceOutputs, false, false, true, false);
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
    for (int i = 0; i < kMixerStrips; ++i)
    {
        auto s = std::make_unique<Strip> (false, i, [this] { changed(); });
        s->apply = [this, i] (const StripSettings& st)
        {
            core.setStripGainDb (i, st.gainDb); core.setStripPan (i, st.pan);
            core.setStripMute (i, st.mute); core.setStripSolo (i, st.solo);
            core.setStripDirectOut (i, st.directOut);
        };
        content.addAndMakeVisible (*s);
        content.strips.push_back (s.get());
        strips.push_back (std::move (s));
    }

    auto m = std::make_unique<Strip> (true, kMixerStrips, [this] { changed(); });
    m->applyMaster = [this] (float db, bool mute, int out) { core.setMasterDb (db); core.setMasterMute (mute); core.setMasterOut (out); };
    content.addAndMakeVisible (*m);
    content.master = m.get();
    strips.push_back (std::move (m));

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

    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, true);
    addAndMakeVisible (viewport);

    setSettings (MixerSettings());
    syncOutputChoices();
    startTimerHz (30);
}

MixerPage::~MixerPage() { stopTimer(); }

MixerSettings MixerPage::getSettings() const
{
    MixerSettings s;
    for (int i = 0; i < kMixerStrips; ++i) s.strips[(size_t) i] = strips[(size_t) i]->getStrip();
    s.masterDb = strips.back()->masterDb();
    s.masterMute = strips.back()->masterMute();
    s.masterOut = strips.back()->masterOut();
    for (int i = 0; i < kAppSlots; ++i) s.apps[(size_t) i] = slotPanels[(size_t) i]->getTarget();
    s.sanitize();
    return s;
}

void MixerPage::setSettings (const MixerSettings& in)
{
    MixerSettings s = in;
    s.sanitize();
    for (int i = 0; i < kMixerStrips; ++i) strips[(size_t) i]->setStrip (s.strips[(size_t) i]);
    strips.back()->setMaster (s.masterDb, s.masterMute, s.masterOut);
    for (int i = 0; i < kAppSlots; ++i) slotPanels[(size_t) i]->setTarget (s.apps[(size_t) i]);
    core.setSettings (s);
}

void MixerPage::syncOutputChoices()
{
    const int n = engine.getNumOutputs();
    if (n == lastNumOutputs) return;
    lastNumOutputs = n;
    for (auto& s : strips) s->setNumOutputs (n);
}

void MixerPage::timerCallback()
{
    const auto p = core.takePeaks();                      // always drain, so a hidden page doesn't keep a stale clip flag
    ++tick;
    if (tick % 30 == 0)                                   // ~1 Hz: follow apps whose audio processes come and go
    {
        engine.refreshCaptures();
        for (auto& sp : slotPanels) sp->update();
        inputPanel->update();
    }
    if (tick % 90 == 0) for (auto& sp : slotPanels) sp->refreshAppList();     // ~3 s: apps started or quit
    if (tick % 15 == 0) syncOutputChoices();

    if (! isShowing()) return;

    constexpr float dt = 1.0f / 30.0f;
    for (int i = 0; i < kMixerStrips; ++i) strips[(size_t) i]->getMeter().update (&p.strip[(size_t) i], dt);

    const float master[2] { p.masterL, p.masterR };
    strips.back()->getMeter().update (master, dt);
    if (p.clipped) { clipHoldTicks = 60; strips.back()->setClip (true); }
    else if (clipHoldTicks > 0 && --clipHoldTicks == 0) strips.back()->setClip (false);
}

void MixerPage::resized()
{
    viewport.setBounds (getLocalBounds());
    const int minW = 2 * kMargin + kMixerStrips * kStripW + (kMixerStrips - 1) * kGap + 3 * kGroupGap + kMasterW;
    content.setSize (juce::jmax (minW, viewport.getMaximumVisibleWidth()),
                     juce::jmax (kHeaderH + kStripH + 2 * kMargin, viewport.getMaximumVisibleHeight()));
}

void MixerPage::paint (juce::Graphics& g) { g.fillAll (kBg); }

void MixerPage::Content::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
    g.setColour (kAccent);
    g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    g.drawText ("NETWORK INPUT", kMargin + 4, kMargin + 6, 8 * kStripW, 20, juce::Justification::centredLeft);
    g.setColour (kDim);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    g.drawText ("Stream channel 1-8 from the Network page", kMargin + 4, kMargin + 28, 8 * kStripW, 20, juce::Justification::centredLeft);
}

void MixerPage::Content::resized()
{
    int x = kMargin;
    const int top = kMargin + kHeaderH;

    for (int i = 0; i < kNetworkStrips; ++i)
    {
        strips[(size_t) i]->setBounds (x, top, kStripW, kStripH);
        x += kStripW + (i + 1 < kNetworkStrips ? kGap : 0);
    }
    x += kGroupGap;

    for (int s = 0; s < kAppSlots; ++s)
    {
        const int sx = x;
        for (int k = 0; k < 2; ++k)
        {
            strips[(size_t) (kNetworkStrips + s * 2 + k)]->setBounds (x, top, kStripW, kStripH);
            x += kStripW + kGap;
        }
        slots[(size_t) s]->setBounds (sx, kMargin, 2 * kStripW + kGap, kHeaderH - kGap);
        if (s + 1 == kAppSlots) x -= kGap;
    }
    x += kGroupGap;

    const int inX = x;
    for (int k = 0; k < kInputStrips; ++k)
    {
        strips[(size_t) (kNetworkStrips + kAppStrips + k)]->setBounds (x, top, kStripW, kStripH);
        x += kStripW + (k + 1 < kInputStrips ? kGap : 0);
    }
    if (inputPanel != nullptr) inputPanel->setBounds (inX, kMargin, 3 * kStripW + 2 * kGap, kHeaderH - kGap);
    x += kGroupGap;
    if (master != nullptr) master->setBounds (x, top, kMasterW, kStripH);
}
}
