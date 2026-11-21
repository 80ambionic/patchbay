#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <vector>
#include "Dsp/Loudness.h"
#include "Dsp/Spectrum.h"
#include "Mixer/MixerCore.h"

namespace pb
{
/** A log-frequency spectrum display: gridlines, a smoothed curve with a slower peak-hold trace and a peak readout. */
class SpectrumView : public juce::Component
{
public:
    SpectrumView();
    void setSampleRate (double sr);
    /** `l`/`r` hold SpectrumAnalyzer::kSize frames. Mono sum -> FFT -> smoothing. `dt` is the time since the last call. */
    void feed (const float* l, const float* r, float dt);
    /** No new audio (device stopped or nothing selected): let the curve fall to the floor. */
    void decay (float dt);
    void setAccent (juce::Colour c) { accent = c; repaint(); }
    void paint (juce::Graphics&) override;

private:
    void updateFromBands (float dt, bool haveAudio);

    dsp::SpectrumAnalyzer analyzer;
    std::vector<float> shown, hold;
    std::vector<float> mono;
    juce::Colour accent { 0xff5b9bd5 };
    bool havePeak = false;
    double peakHz = 0.0;
    float peakDb = -120.0f;
};

/** The two analyzer windows across the top of the mixer:
      - left: the selected track (a strip or a bus) with an Input / Output toggle,
      - right: the master (Output) with Hz peak readout and loudness (momentary, short-term, integrated, range, true peak). */
class AnalyzerPanel : public juce::Component, private juce::Timer
{
public:
    AnalyzerPanel (MixerCore&, std::function<double()> sampleRate);
    ~AnalyzerPanel() override;

    /** target: strip 0..31, bus 32..63, or -1 for none. `name` is shown in the header. */
    void setTarget (int target, const juce::String& name);
    void setTargetName (int target, const juce::String& name);    // a rename of the selected track
    int getTarget() const noexcept { return target; }
    bool isPost() const noexcept { return post; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Readouts;
    void timerCallback() override;
    void applyScope();

    MixerCore& core;
    std::function<double()> rate;
    int target = -1;
    bool post = false;
    juce::String targetName;

    SpectrumView trackView, masterView;
    juce::TextButton inputButton { "Input" }, outputButton { "Output" }, masterOutButton { "Output" }, resetButton { "Reset" };
    std::unique_ptr<Readouts> readouts;
    dsp::LoudnessMeter loudness;

    std::vector<float> bl, br;
    uint64_t lastTrackWritten = 0, lastMasterWritten = 0;
    juce::Rectangle<int> trackBox, masterBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalyzerPanel)
};
}
