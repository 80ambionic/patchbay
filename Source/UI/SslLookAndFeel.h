#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>

namespace pb
{
/** Console look in the spirit of a classic large-format analogue desk (SSL 4000 era): charcoal panels, coloured
    skirted knob caps with a white pointer and a tick scale, long-throw faders with a dB scale and a rectangular cap
    with a white centre line, and square illuminated switches. Purely cosmetic: it draws the same juce::Slider and
    juce::Button objects the mixer already uses.

    Colours come from the components, so one knob can be blue and another red:
      Slider::rotarySliderFillColourId   -> knob cap colour
      Slider::thumbColourId              -> fader cap colour
      TextButton::buttonOnColourId       -> lit colour of a switch
    The class never owns or touches audio state. */
class SslLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SslLookAndFeel()
    {
        setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff1b1e23));
        setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff3a3f48));
        setColour (juce::ComboBox::textColourId, juce::Colour (0xffd8dce2));
        setColour (juce::ComboBox::arrowColourId, juce::Colour (0xffb7bcc5));
        setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff1b1e23));
        setColour (juce::PopupMenu::textColourId, juce::Colour (0xffd8dce2));
        setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (0xff3b6ea5));
        setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2b2f36));
        setColour (juce::TextButton::textColourOffId, juce::Colour (0xffc4c8d0));
    }

    //==========================================================================================================
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos, float startAngle, float endAngle,
                           juce::Slider& s) override
    {
        const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
        const float size = std::min (bounds.getWidth(), bounds.getHeight());
        const auto c = bounds.getCentre();
        const float outer = size * 0.5f;                          // outer edge of the tick scale
        const float skirt = outer * 0.80f;                        // knob skirt
        const float cap = outer * 0.62f;                          // coloured cap
        const float angle = startAngle + pos * (endAngle - startAngle);
        const bool enabled = s.isEnabled();
        const float alpha = enabled ? 1.0f : 0.4f;

        // Tick scale: 11 ticks across the sweep, the centre one longer (the detent for a balance control).
        g.setColour (juce::Colour (0xff9aa0aa).withAlpha (alpha));
        for (int i = 0; i <= 10; ++i)
        {
            const float a = startAngle + (float) i / 10.0f * (endAngle - startAngle);
            const float len = (i == 5) ? 0.20f : 0.11f;
            const float r0 = outer * (1.0f - len), r1 = outer;
            g.drawLine (c.x + r0 * std::sin (a), c.y - r0 * std::cos (a), c.x + r1 * std::sin (a), c.y - r1 * std::cos (a),
                        (i == 5) ? 1.6f : 1.0f);
        }

        // Skirt: dark, with a soft top-left highlight.
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a3e46), c.x - skirt * 0.6f, c.y - skirt * 0.8f,
                                                 juce::Colour (0xff121417), c.x + skirt * 0.6f, c.y + skirt * 0.9f, false));
        g.fillEllipse (c.x - skirt, c.y - skirt, skirt * 2.0f, skirt * 2.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawEllipse (c.x - skirt, c.y - skirt, skirt * 2.0f, skirt * 2.0f, 1.0f);

        // Cap in the component's colour.
        const auto capColour = s.findColour (juce::Slider::rotarySliderFillColourId).withAlpha (alpha);
        g.setGradientFill (juce::ColourGradient (capColour.brighter (0.35f), c.x - cap * 0.5f, c.y - cap * 0.7f,
                                                 capColour.darker (0.55f), c.x + cap * 0.5f, c.y + cap * 0.8f, false));
        g.fillEllipse (c.x - cap, c.y - cap, cap * 2.0f, cap * 2.0f);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawEllipse (c.x - cap, c.y - cap, cap * 2.0f, cap * 2.0f, 1.0f);

        // White pointer line from the cap edge to near the centre.
        const float p0 = cap * 0.18f, p1 = cap * 0.96f;
        g.setColour (juce::Colours::white.withAlpha (alpha));
        g.drawLine (c.x + p0 * std::sin (angle), c.y - p0 * std::cos (angle), c.x + p1 * std::sin (angle), c.y - p1 * std::cos (angle), 2.2f);
    }

    //==========================================================================================================
    /** dB marks for a vertical fader, top to bottom. */
    static constexpr int kFaderMarks[] = { 12, 6, 0, -5, -10, -20, -30, -40, -60 };

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        if (style != juce::Slider::LinearVertical)
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, s);
            return;
        }

        const float alpha = s.isEnabled() ? 1.0f : 0.4f;
        const bool labels = width >= 40;
        const float scaleW = labels ? 17.0f : 0.0f;
        const float capW = std::min ((float) width - scaleW - 4.0f, 26.0f);
        const float capH = 34.0f;
        const float cx = (float) x + scaleW + ((float) width - scaleW) * 0.5f;

        // Where the cap centre travels: JUCE gives minSliderPos (bottom) .. maxSliderPos (top) already inset for the thumb.
        const float yBottom = minSliderPos, yTop = maxSliderPos;
        auto yForDb = [&] (double db) { return yBottom + (float) s.valueToProportionOfLength (db) * (yTop - yBottom); };

        // Slot.
        const float slotW = 5.0f;
        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.fillRoundedRectangle (cx - slotW * 0.5f, yTop - 6.0f, slotW, (yBottom - yTop) + 12.0f, 2.0f);

        // Scale: long ticks right and left of the slot, labels on the left, 0 dB emphasised.
        g.setFont (juce::Font (juce::FontOptions (9.0f)));
        for (int db : kFaderMarks)
        {
            if ((double) db < s.getMinimum() || (double) db > s.getMaximum()) continue;
            const float ty = yForDb ((double) db);
            const bool zero = (db == 0);
            g.setColour ((zero ? juce::Colour (0xffe8eaee) : juce::Colour (0xff8a8f98)).withAlpha (alpha));
            g.drawHorizontalLine ((int) std::round (ty), cx - 12.0f, cx - 4.0f);
            g.drawHorizontalLine ((int) std::round (ty), cx + 4.0f, cx + 12.0f);
            if (labels)
            {
                const juce::String text = db == -60 ? juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x9e")) : (db > 0 ? "+" + juce::String (db) : juce::String (db));
                g.drawText (text, x, (int) std::round (ty) - 6, (int) scaleW - 1, 12, juce::Justification::centredRight);
            }
        }

        // Cap: dark rectangular block, light bevel, white centre line (the classic SSL fader).
        const juce::Rectangle<float> cap (cx - capW * 0.5f, sliderPos - capH * 0.5f, capW, capH);
        const auto base = s.findColour (juce::Slider::thumbColourId).withAlpha (alpha);
        g.setGradientFill (juce::ColourGradient (base.brighter (0.25f), cap.getX(), cap.getY(), base.darker (0.5f), cap.getX(), cap.getBottom(), false));
        g.fillRoundedRectangle (cap, 2.5f);
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawRoundedRectangle (cap, 2.5f, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.18f * alpha));
        g.drawHorizontalLine ((int) cap.getY() + 1, cap.getX() + 2.0f, cap.getRight() - 2.0f);
        g.setColour (juce::Colours::white.withAlpha (alpha));
        g.fillRect (cap.getX() + 2.0f, cap.getCentreY() - 1.0f, cap.getWidth() - 4.0f, 2.0f);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        for (float gy = cap.getY() + 5.0f; gy < cap.getBottom() - 4.0f; gy += 4.0f)           // grip ridges
            if (std::abs (gy - cap.getCentreY()) > 4.0f) g.drawHorizontalLine ((int) gy, cap.getX() + 4.0f, cap.getRight() - 4.0f);
    }

    int getSliderThumbRadius (juce::Slider& s) override
    {
        return s.getSliderStyle() == juce::Slider::LinearVertical ? 17 : LookAndFeel_V4::getSliderThumbRadius (s);
    }

    //==========================================================================================================
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& /*bg*/, bool highlighted, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        const bool on = b.getToggleState();
        const auto lit = b.findColour (juce::TextButton::buttonOnColourId);
        auto base = b.findColour (juce::TextButton::buttonColourId);
        if (highlighted) base = base.brighter (0.12f);
        if (down) base = base.darker (0.2f);

        g.setGradientFill (juce::ColourGradient (base.brighter (0.15f), r.getX(), r.getY(), base.darker (0.3f), r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, 3.0f);
        if (on)                                              // illuminated switch
        {
            g.setColour (lit.withAlpha (0.92f));
            g.fillRoundedRectangle (r.reduced (2.0f), 2.0f);
            g.setColour (lit.brighter (0.6f).withAlpha (0.5f));
            g.drawHorizontalLine ((int) r.getY() + 3, r.getX() + 4.0f, r.getRight() - 4.0f);
        }
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
    }
};
}
