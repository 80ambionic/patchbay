#include "AnalyzerPanel.h"

namespace pb
{
namespace
{
const juce::Colour kBg (0xff0f1114), kPanel (0xff1b1e23), kGrid (0xff2b3038), kText (0xffd8dce2), kDim (0xff8a8f98),
                   kGood (0xff6ccf8e), kWarn (0xfff0a35a), kBad (0xffe06c6c), kAccent (0xff5b9bd5), kMasterTint (0xff6ccf8e);
constexpr float kFloorDb = -90.0f, kCeilDb = 0.0f;
constexpr int kBands = 160;

juce::String hzText (double hz)
{
    return hz < 1000.0 ? juce::String ((int) std::round (hz)) + " Hz" : juce::String (hz / 1000.0, hz < 10000.0 ? 2 : 1) + " kHz";
}
juce::String dbText (double v, int dp = 1) { return v <= -119.0 ? juce::String ("-inf") : juce::String (v, dp); }
}

// ---------------------------------------------------------------------------------------------------------------
SpectrumView::SpectrumView()
    : analyzer (kBands), shown ((size_t) kBands, dsp::SpectrumAnalyzer::kFloorDb), hold ((size_t) kBands, dsp::SpectrumAnalyzer::kFloorDb),
      mono ((size_t) dsp::SpectrumAnalyzer::kSize)
{
}

void SpectrumView::setSampleRate (double sr)
{
    if (sr > 1000.0 && sr != analyzer.getSampleRate()) analyzer.setSampleRate (sr);
}

void SpectrumView::feed (const float* l, const float* r, float dt)
{
    for (int i = 0; i < dsp::SpectrumAnalyzer::kSize; ++i) mono[(size_t) i] = 0.5f * (l[i] + r[i]);
    analyzer.analyze (mono.data());
    havePeak = analyzer.peak (peakHz, peakDb);
    updateFromBands (dt, true);
}

void SpectrumView::decay (float dt)
{
    havePeak = false;
    updateFromBands (dt, false);
}

void SpectrumView::updateFromBands (float dt, bool haveAudio)
{
    const float fall = 45.0f * dt, holdFall = 8.0f * dt;
    bool changed = false;
    for (int b = 0; b < kBands; ++b)
    {
        const float target = haveAudio ? analyzer.bandDb (b) : dsp::SpectrumAnalyzer::kFloorDb;
        auto& s = shown[(size_t) b];
        const float next = target > s ? target : std::max (target, s - fall);
        if (std::abs (next - s) > 0.02f) changed = true;
        s = next;
        hold[(size_t) b] = std::max (s, hold[(size_t) b] - holdFall);
    }
    if (changed || haveAudio) repaint();
}

void SpectrumView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (kBg);
    g.fillRoundedRectangle (r, 4.0f);
    auto plot = r.reduced (4.0f);
    const float left = plot.getX() + 26.0f, bottom = plot.getBottom() - 14.0f;
    const juce::Rectangle<float> area (left, plot.getY() + 2.0f, plot.getRight() - left - 2.0f, bottom - plot.getY() - 2.0f);
    auto yFor = [&] (float db) { return area.getBottom() - area.getHeight() * juce::jlimit (0.0f, 1.0f, (db - kFloorDb) / (kCeilDb - kFloorDb)); };
    auto xFor = [&] (double hz) { return area.getX() + area.getWidth() * (float) analyzer.hzToPos (hz); };

    g.setFont (juce::Font (juce::FontOptions (9.5f)));
    for (int db = (int) kFloorDb; db <= (int) kCeilDb; db += 10)
    {
        const float y = yFor ((float) db);
        g.setColour (db % 30 == 0 ? kGrid.brighter (0.15f) : kGrid);
        g.drawHorizontalLine ((int) y, area.getX(), area.getRight());
        if (db % 20 == 0 || db == 0) { g.setColour (kDim); g.drawText (juce::String (db), (int) plot.getX(), (int) y - 6, 24, 12, juce::Justification::centredRight); }
    }
    const double marks[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    for (double hz : marks)
    {
        if (hz >= analyzer.getSampleRate() * 0.5) continue;
        const float x = xFor (hz);
        g.setColour (kGrid);
        g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
        g.setColour (kDim);
        const auto t = hz >= 1000 ? juce::String ((int) (hz / 1000)) + "k" : juce::String ((int) hz);
        g.drawText (t, (int) x - 18, (int) area.getBottom() + 1, 36, 12, juce::Justification::centred);
    }

    // Curve.
    juce::Path fill, line, holdLine;
    for (int b = 0; b < kBands; ++b)
    {
        const float x = xFor (analyzer.bandCentreHz (b));
        const float y = yFor (shown[(size_t) b]), yh = yFor (hold[(size_t) b]);
        if (b == 0) { fill.startNewSubPath (x, area.getBottom()); fill.lineTo (x, y); line.startNewSubPath (x, y); holdLine.startNewSubPath (x, yh); }
        else { fill.lineTo (x, y); line.lineTo (x, y); holdLine.lineTo (x, yh); }
        if (b == kBands - 1) { fill.lineTo (x, area.getBottom()); fill.closeSubPath(); }
    }
    g.saveState();
    g.reduceClipRegion (area.toNearestInt());
    g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.55f), 0.0f, area.getY(), accent.withAlpha (0.05f), 0.0f, area.getBottom(), false));
    g.fillPath (fill);
    g.setColour (kText.withAlpha (0.35f));
    g.strokePath (holdLine, juce::PathStrokeType (1.0f));
    g.setColour (accent.brighter (0.4f));
    g.strokePath (line, juce::PathStrokeType (1.6f));
    g.restoreState();

    g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    g.setColour (havePeak ? kText : kDim);
    g.drawText (havePeak ? "PEAK  " + hzText (peakHz) + "   " + juce::String (peakDb, 1) + " dB" : juce::String ("PEAK  --"),
                area.toNearestInt().removeFromTop (16).reduced (6, 0), juce::Justification::centredRight);
}

// ---------------------------------------------------------------------------------------------------------------
/** Loudness numbers for the master. */
class AnalyzerPanel::Readouts : public juce::Component
{
public:
    explicit Readouts (const dsp::LoudnessMeter& m) : meter (m) {}

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        g.setColour (kBg);
        g.fillRoundedRectangle (r.toFloat(), 4.0f);
        struct Row { const char* label; double value; const char* unit; int kind; };       // kind 0 = loudness, 1 = range, 2 = true peak
        const Row rows[] = { { "MOMENTARY", meter.momentary(), "LUFS", 0 }, { "SHORT-TERM", meter.shortTermLufs(), "LUFS", 0 },
                             { "INTEGRATED", meter.integrated(), "LUFS", 0 }, { "RANGE", meter.range(), "LU", 1 },
                             { "TRUE PEAK", meter.truePeakDb(), "dBTP", 2 } };
        const int rowH = r.getHeight() / 5;
        for (int i = 0; i < 5; ++i)
        {
            auto row = r.removeFromTop (rowH).reduced (8, 1);
            const auto& v = rows[i];
            juce::Colour c = kText;
            if (v.kind == 2 && v.value > -1.0) c = kBad;
            else if (v.kind == 2 && v.value > -3.0) c = kWarn;
            else if (v.value <= -119.0 && v.kind != 1) c = kDim;
            g.setColour (kDim);
            g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
            g.drawText (v.label, row.removeFromTop (11), juce::Justification::centredLeft);
            g.setColour (c);
            g.setFont (juce::Font (juce::FontOptions (rowH > 30 ? 20.0f : 16.0f, juce::Font::bold)));
            const auto txt = v.kind == 1 ? (v.value <= -119.0 ? juce::String ("--") : juce::String (v.value, 1)) : dbText (v.value);
            g.drawText (txt, row.removeFromLeft (row.getWidth() - 34), juce::Justification::centredRight);
            g.setColour (kDim);
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            g.drawText (v.unit, row, juce::Justification::bottomRight);
        }
    }

private:
    const dsp::LoudnessMeter& meter;
};

// ---------------------------------------------------------------------------------------------------------------
AnalyzerPanel::AnalyzerPanel (MixerCore& c, std::function<double()> sampleRate)
    : core (c), rate (std::move (sampleRate)), bl ((size_t) dsp::SpectrumAnalyzer::kSize), br ((size_t) dsp::SpectrumAnalyzer::kSize)
{
    readouts = std::make_unique<Readouts> (loudness);
    trackView.setAccent (kAccent);
    masterView.setAccent (kMasterTint);
    addAndMakeVisible (trackView);
    addAndMakeVisible (masterView);
    addAndMakeVisible (*readouts);

    for (auto* b : { &inputButton, &outputButton })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (4711);
        b->setColour (juce::TextButton::buttonOnColourId, kAccent.darker (0.2f));
        b->setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        addAndMakeVisible (*b);
    }
    inputButton.setToggleState (true, juce::dontSendNotification);
    inputButton.setTooltip ("Analyze the signal going into the track (before its fader)");
    outputButton.setTooltip ("Analyze the signal leaving the track (after its fader)");
    inputButton.onClick = [this] { post = false; applyScope(); };
    outputButton.onClick = [this] { post = true; applyScope(); };

    masterOutButton.setClickingTogglesState (false);
    masterOutButton.setToggleState (true, juce::dontSendNotification);      // the master analyzer always shows the output
    masterOutButton.setColour (juce::TextButton::buttonOnColourId, kMasterTint.darker (0.4f));
    masterOutButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    masterOutButton.setTooltip ("The master analyzer shows the master output");
    masterOutButton.setEnabled (false);
    addAndMakeVisible (masterOutButton);

    resetButton.setTooltip ("Restart the integrated loudness, range and peak measurements");
    resetButton.onClick = [this] { loudness.reset(); readouts->repaint(); };
    addAndMakeVisible (resetButton);

    startTimerHz (30);
}

AnalyzerPanel::~AnalyzerPanel() { stopTimer(); core.setScope (-1, false); }

void AnalyzerPanel::setTarget (int t, const juce::String& name)
{
    target = t;
    targetName = name;
    lastTrackWritten = 0;
    applyScope();
    repaint();
}

void AnalyzerPanel::setTargetName (int t, const juce::String& name)
{
    if (t == target && name != targetName) { targetName = name; repaint (trackBox); }
}

void AnalyzerPanel::applyScope() { core.setScope (target, post); }

void AnalyzerPanel::resized()
{
    auto r = getLocalBounds().reduced (6, 4);
    const int masterW = juce::jlimit (300, 520, r.getWidth() * 2 / 5);
    masterBox = r.removeFromRight (masterW);
    r.removeFromRight (8);
    trackBox = r;

    auto t = trackBox;
    auto head = t.removeFromTop (26);
    outputButton.setBounds (head.removeFromRight (64).reduced (0, 2));
    head.removeFromRight (2);
    inputButton.setBounds (head.removeFromRight (64).reduced (0, 2));
    trackView.setBounds (t);

    auto m = masterBox;
    auto mh = m.removeFromTop (26);
    resetButton.setBounds (mh.removeFromRight (56).reduced (0, 2));
    mh.removeFromRight (4);
    masterOutButton.setBounds (mh.removeFromRight (64).reduced (0, 2));
    readouts->setBounds (m.removeFromRight (std::min (150, m.getWidth() / 2)));
    m.removeFromRight (6);
    masterView.setBounds (m);
}

void AnalyzerPanel::paint (juce::Graphics& g)
{
    g.setColour (kPanel);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f), 8.0f);
    g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    g.setColour (kAccent);
    const auto title = target < 0 ? juce::String ("TRACK ANALYZER - nothing selected (press A on a strip)")
                                  : "TRACK ANALYZER - " + juce::String (target < kStrips ? "Strip " + juce::String (target + 1) : "Bus " + juce::String (target - kStrips + 1))
                                        + (targetName.isNotEmpty() ? "  (" + targetName + ")" : juce::String());
    g.drawText (title, trackBox.withHeight (26).withTrimmedRight (140), juce::Justification::centredLeft);
    g.setColour (kMasterTint);
    g.drawText ("MASTER ANALYZER", masterBox.withHeight (26).withTrimmedRight (130), juce::Justification::centredLeft);
}

void AnalyzerPanel::timerCallback()
{
    const float dt = 1.0f / 30.0f;
    const double sr = rate ? rate() : 0.0;
    if (sr > 1000.0) { trackView.setSampleRate (sr); masterView.setSampleRate (sr); }

    constexpr uint32_t n = (uint32_t) dsp::SpectrumAnalyzer::kSize;
    auto service = [&] (const ScopeRing& ring, SpectrumView& view, uint64_t& last, bool wanted)
    {
        const auto w = ring.written();
        if (wanted && w != last && ring.latest (bl.data(), br.data(), n)) view.feed (bl.data(), br.data(), dt);
        else view.decay (dt);                                    // no new audio: the device is stopped or nothing is selected
        last = w;
    };
    service (core.scopeRing(), trackView, lastTrackWritten, target >= 0);
    service (core.masterRing(), masterView, lastMasterWritten, true);

    dsp::LoudnessHop h;
    bool any = false;
    while (core.popLoudnessHop (h)) { loudness.addHop (h); any = true; }
    if (any) readouts->repaint();
}
}
