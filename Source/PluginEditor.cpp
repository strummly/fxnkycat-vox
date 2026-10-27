#include "PluginEditor.h"
#include "BinaryData.h"

namespace
{
    const juce::Colour bg       { 0xff15171c };
    const juce::Colour panelBg  { 0xff20232b };
    const juce::Colour edge     { 0xff343844 };
    const juce::Colour accent   { 0xffe0a64a };
    const juce::Colour textCol  { 0xffe8e6e1 };
    const juce::Colour dimText  { 0xff8f939e };
}

//==============================================================================
namespace ui
{
SwitchButton::SwitchButton (juce::String offText, juce::String onText)
    : juce::Button (offText), off (std::move (offText)), on (std::move (onText))
{
    setClickingTogglesState (true);
}

void SwitchButton::paintButton (juce::Graphics& g, bool highlighted, bool)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool state = getToggleState();

    g.setColour (highlighted ? edge.brighter (0.2f) : edge);
    g.fillRoundedRectangle (r, 5.0f);

    // Two halves; the active one is lit.
    auto left = r.removeFromLeft (r.getWidth() * 0.5f);
    auto right = r;
    g.setColour (accent);
    g.fillRoundedRectangle ((state ? right : left).reduced (2.0f), 4.0f);

    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.setColour (state ? dimText : bg);
    g.drawFittedText (off, left.toNearestInt(), juce::Justification::centred, 1);
    g.setColour (state ? bg : dimText);
    g.drawFittedText (on, right.toNearestInt(), juce::Justification::centred, 1);
}

//------------------------------------------------------------------------------
void KnobLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                        float startAngle, float endAngle, juce::Slider& s)
{
    if (! (bool) s.getProperties().getWithDefault ("fillRight", false))
    {
        LookAndFeel_V4::drawRotarySlider (g, x, y, w, h, pos, startAngle, endAngle, s);
        return;
    }

    // Same look as LookAndFeel_V4, but the value arc runs from the pointer to the end.
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (10.0f);
    const float radius  = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f;
    const float toAngle = startAngle + pos * (endAngle - startAngle);
    const float lineW   = juce::jmin (8.0f, radius * 0.5f);
    const float arcR    = radius - lineW * 0.5f;
    const juce::PathStrokeType stroke (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    juce::Path back;
    back.addCentredArc (bounds.getCentreX(), bounds.getCentreY(), arcR, arcR, 0.0f, startAngle, endAngle, true);
    g.setColour (s.findColour (juce::Slider::rotarySliderOutlineColourId));
    g.strokePath (back, stroke);

    if (s.isEnabled() && pos < 1.0f)
    {
        juce::Path value;
        value.addCentredArc (bounds.getCentreX(), bounds.getCentreY(), arcR, arcR, 0.0f, toAngle, endAngle, true);
        g.setColour (s.findColour (juce::Slider::rotarySliderFillColourId));
        g.strokePath (value, stroke);
    }

    const float thumbW = lineW * 2.0f;
    const juce::Point<float> thumb (bounds.getCentreX() + arcR * std::cos (toAngle - juce::MathConstants<float>::halfPi),
                                    bounds.getCentreY() + arcR * std::sin (toAngle - juce::MathConstants<float>::halfPi));
    g.setColour (s.findColour (juce::Slider::thumbColourId));
    g.fillEllipse (juce::Rectangle<float> (thumbW, thumbW).withCentre (thumb));
}

//------------------------------------------------------------------------------
LabeledControl::LabeledControl (const juce::String& caption, std::unique_ptr<juce::Component> c, bool isCompact,
                                int compactMaxWidth)
    : control (std::move (c)), compact (isCompact), maxWidth (compactMaxWidth)
{
    label.setText (caption, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setFont (juce::FontOptions (12.5f));
    label.setColour (juce::Label::textColourId, dimText);
    addAndMakeVisible (label);
    addAndMakeVisible (*control);
}

void LabeledControl::resized()
{
    auto r = getLocalBounds();
    label.setBounds (r.removeFromTop (18));
    if (compact)
        control->setBounds (r.withSizeKeepingCentre (juce::jmin (r.getWidth() - 8, maxWidth),
                                                     juce::jmin (r.getHeight(), controlHeight)));
    else
        control->setBounds (r.reduced (2));
}

//------------------------------------------------------------------------------
SegmentedSwitch::SegmentedSwitch (juce::AudioParameterChoice& p)
    : options (p.choices),
      attachment (p, [this] (float v) { selected = juce::roundToInt (v); repaint(); })
{
    attachment.sendInitialUpdate();
}

void SegmentedSwitch::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (edge);
    g.fillRoundedRectangle (r, 5.0f);

    const int n = juce::jmax (1, options.size());
    const float w = r.getWidth() / (float) n, h = r.getHeight() / (float) n;
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    for (int i = 0; i < n; ++i)
    {
        const int s = slotFor (i);
        auto seg = vertical ? juce::Rectangle<float> (r.getX(), r.getY() + h * (float) s, r.getWidth(), h)
                            : juce::Rectangle<float> (r.getX() + w * (float) s, r.getY(), w, r.getHeight());
        if (i == selected)
        {
            g.setColour (accent);
            g.fillRoundedRectangle (seg.reduced (2.0f), 4.0f);
        }
        g.setColour (i == selected ? bg : dimText);
        g.drawFittedText (options[i], seg.toNearestInt(), juce::Justification::centred, 1);
    }
}

void SegmentedSwitch::mouseDown (const juce::MouseEvent& e)
{
    const int n = juce::jmax (1, options.size());
    const float pos = vertical ? (float) e.y / (float) getHeight() : (float) e.x / (float) getWidth();
    const int idx = slotFor (juce::jlimit (0, n - 1, (int) (pos * (float) n)));
    attachment.setValueAsCompleteGesture ((float) idx);
}

//------------------------------------------------------------------------------
SpectrumAnalyzer::SpectrumAnalyzer (VocalChainProcessor& p) : proc (p)
{
    pre.tap  = &proc.preTap;
    post.tap = &proc.postTap;
    setOpaque (false);
    startTimerHz (30);
}

void SpectrumAnalyzer::timerCallback()
{
    analyse (pre);
    analyse (post);
    repaint();
}

void SpectrumAnalyzer::analyse (Trace& t)
{
    // Take whatever the audio thread has produced since last time.
    const int got = t.tap->pull (scratch.data(), (int) scratch.size());
    for (int i = 0; i < got; ++i)
    {
        t.history[(size_t) t.pos] = scratch[(size_t) i];
        t.pos = (t.pos + 1) % kSize;
    }

    // FFT of the most recent kSize samples.
    for (int i = 0; i < kSize; ++i)
        fftData[(size_t) i] = t.history[(size_t) ((t.pos + i) % kSize)];
    std::fill (fftData.begin() + kSize, fftData.end(), 0.0f);
    window.multiplyWithWindowingTable (fftData.data(), (size_t) kSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data());

    // A full-scale sine reads 0 dB. Rise instantly, fall smoothly.
    const float norm = 4.0f / (float) kSize;
    for (int b = 0; b < kBins; ++b)
    {
        const float db = juce::jlimit (kMinDb, kMaxDb, juce::Decibels::gainToDecibels (fftData[(size_t) b] * norm, kMinDb));
        float& lvl = t.levelDb[(size_t) b];
        lvl = db > lvl ? db : juce::jmax (db, lvl - 1.2f);
    }
}

float SpectrumAnalyzer::xForHz (float hz, juce::Rectangle<float> a) const
{
    return a.getX() + a.getWidth() * std::log (hz / kMinHz) / std::log (kMaxHz / kMinHz);
}

float SpectrumAnalyzer::yForDb (float db, juce::Rectangle<float> a) const
{
    return juce::jmap (db, kMinDb, kMaxDb, a.getBottom(), a.getY());
}

juce::Path SpectrumAnalyzer::makePath (const Trace& t, juce::Rectangle<float> a, bool closed) const
{
    const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 44100.0;
    juce::Path path;
    bool started = false;

    for (float x = 0.0f; x <= a.getWidth(); x += 1.5f)
    {
        const float hz  = kMinHz * std::pow (kMaxHz / kMinHz, x / a.getWidth());
        const float bin = (float) (hz * kSize / sr);
        const int   b0  = juce::jlimit (0, kBins - 2, (int) bin);
        const float fr  = juce::jlimit (0.0f, 1.0f, bin - (float) b0);
        const float db  = t.levelDb[(size_t) b0] + fr * (t.levelDb[(size_t) b0 + 1] - t.levelDb[(size_t) b0]);
        const float px = a.getX() + x, py = yForDb (db, a);

        if (! started) { path.startNewSubPath (px, py); started = true; }
        else           path.lineTo (px, py);
    }

    if (closed && started)
    {
        path.lineTo (a.getRight(), a.getBottom());
        path.lineTo (a.getX(), a.getBottom());
        path.closeSubPath();
    }
    return path;
}

void SpectrumAnalyzer::paint (juce::Graphics& g)
{
    auto outer = getLocalBounds().toFloat().reduced (4.0f);
    g.setColour (panelBg);
    g.fillRoundedRectangle (outer, 8.0f);
    g.setColour (edge);
    g.drawRoundedRectangle (outer, 8.0f, 1.2f);

    auto a = outer.reduced (36.0f, 22.0f).withTrimmedLeft (-8.0f);

    // Watermark: logo + plugin name side by side, very faint and centred, under the grid and traces.
    // The name is stacked one word per line (FXNKYCAT over VOX).
    {
        const auto wm = outer.reduced (12.0f);
        const float logoSize = watermark.isValid() ? wm.getHeight() : 0.0f;
        const float gap = (logoSize > 0.0f && watermarkText.isNotEmpty()) ? wm.getHeight() * 0.04f : 0.0f;
        const auto lines = juce::StringArray::fromTokens (watermarkText, " ", {});
        const int nLines = juce::jmax (1, lines.size());

        // Letter shapes per unit of font height: where the capitals start and end relative to the baseline.
        juce::Font font (juce::FontOptions (100.0f, juce::Font::bold));
        juce::Rectangle<float> ink;
        {
            juce::GlyphArrangement ga;
            ga.addLineOfText (font, lines.joinIntoString (""), 0.0f, 0.0f);
            juce::Path p; ga.createPath (p);
            ink = p.getBounds() / 100.0f;   // ink.getY() < 0 (above baseline), ink.getBottom() ~ 0
        }
        const float inkTop = -ink.getY(), inkH = juce::jmax (0.01f, ink.getHeight());

        // Outer edges stay where the original layout put them (lines of 0.88 x height / n,
        // font 1.05 x line); only the gap between the words shrinks as the letters grow.
        const float oldLineH = wm.getHeight() * 0.88f / (float) nLines, oldF = oldLineH * 1.05f;
        const float oldBaseline0 = wm.getCentreY() - oldLineH * (float) nLines * 0.5f
                                 + (oldLineH - oldF) * 0.5f + font.getAscent() / 100.0f * oldF;
        const float blockTop    = oldBaseline0 - inkTop * oldF;
        const float blockBottom = oldBaseline0 + oldLineH * (float) (nLines - 1) + (ink.getBottom()) * oldF;

        constexpr float wordGap = 0.08f;   // space between the words, as a fraction of the letter height
        const float capH  = (blockBottom - blockTop) / ((float) nLines + wordGap * (float) (nLines - 1));
        const float fontH = capH / inkH, pitch = capH * (1.0f + wordGap);
        font = font.withHeight (fontH);
        float textW = 0.0f;
        for (auto& l : lines)
            textW = juce::jmax (textW, juce::GlyphArrangement::getStringWidth (font, l));

        // Logo stays centred; the name sits to its left. Shrink everything if the
        // left half of the analyzer is too narrow for half the logo + gap + name.
        float scale = 1.0f;
        const float need = logoSize * 0.5f + gap + textW;
        if (need > wm.getWidth() * 0.5f)
            scale = wm.getWidth() * 0.5f / need;
        const float ls = logoSize * scale, gp = gap * scale, tw = textW * scale;
        font = font.withHeight (font.getHeight() * scale);

        const float logoX = wm.getCentreX() - ls * 0.5f;
        g.setColour (textCol.withAlpha (0.07f));
        if (ls > 0.0f)
            g.drawImage (watermark, { logoX, wm.getCentreY() - ls * 0.5f, ls, ls }, juce::RectanglePlacement::centred, true);
        if (tw > 0.0f)
        {
            // Right-aligned against the logo, so VOX ends under the T of FXNKYCAT.
            const float right = logoX - gp;
            const float top = wm.getCentreY() + (blockTop - wm.getCentreY()) * scale;
            float baseline = top + inkTop * fontH * scale;
            juce::GlyphArrangement ga;
            for (auto& l : lines)
            {
                const float w = juce::GlyphArrangement::getStringWidth (font, l);
                ga.addLineOfText (font, l, right - w, baseline);
                baseline += pitch * scale;
            }
            ga.draw (g);
        }
    }

    // Grid
    g.setFont (juce::FontOptions (10.5f));
    for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = xForHz (hz, a);
        g.setColour (edge.withAlpha (0.7f));
        g.drawVerticalLine ((int) x, a.getY(), a.getBottom());
        g.setColour (dimText);
        g.drawText (hz >= 1000.0f ? juce::String (hz / 1000.0f, 0) + "k" : juce::String ((int) hz),
                    juce::Rectangle<float> (x - 20.0f, a.getBottom() + 2.0f, 40.0f, 14.0f), juce::Justification::centred);
    }
    for (float db = kMaxDb; db >= kMinDb; db -= 24.0f)
    {
        const float y = yForDb (db, a);
        g.setColour (edge.withAlpha (0.7f));
        g.drawHorizontalLine ((int) y, a.getX(), a.getRight());
        g.setColour (dimText);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (a.getX() - 34.0f, y - 7.0f, 30.0f, 14.0f),
                    juce::Justification::centredRight);
    }

    g.saveState();
    g.reduceClipRegion (a.toNearestInt());

    // Before: soft grey fill
    g.setColour (dimText.withAlpha (0.22f));
    g.fillPath (makePath (pre, a, true));
    g.setColour (dimText.withAlpha (0.7f));
    g.strokePath (makePath (pre, a, false), juce::PathStrokeType (1.0f));

    // After: accent line on top
    g.setColour (accent.withAlpha (0.12f));
    g.fillPath (makePath (post, a, true));
    g.setColour (accent);
    g.strokePath (makePath (post, a, false), juce::PathStrokeType (1.6f));

    g.restoreState();

    // Legend
    auto legend = outer.reduced (14.0f, 6.0f).removeFromTop (14.0f).removeFromRight (150.0f);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.setColour (dimText);
    g.fillRect (legend.getX(), legend.getCentreY() - 1.0f, 14.0f, 2.0f);
    g.drawText ("BEFORE", legend.withTrimmedLeft (18.0f), juce::Justification::centredLeft);
    g.setColour (accent);
    g.fillRect (legend.getX() + 78.0f, legend.getCentreY() - 1.0f, 14.0f, 2.0f);
    g.drawText ("AFTER", legend.withTrimmedLeft (96.0f), juce::Justification::centredLeft);
}

//------------------------------------------------------------------------------
ModulePanel::ModulePanel (juce::String num, juce::String t, juce::String sub, int r)
    : number (std::move (num)), title (std::move (t)), subtitle (std::move (sub)), rows (r)
{
    enable.setButtonText ("ON");
    enable.setColour (juce::ToggleButton::tickColourId, accent);
    enable.setColour (juce::ToggleButton::textColourId, dimText);
    addAndMakeVisible (enable);
}

void ModulePanel::add (std::unique_ptr<LabeledControl> c)
{
    addAndMakeVisible (*c);
    controls.push_back (std::move (c));
}

juce::Rectangle<float> ModulePanel::ledArea() const
{
    // Just left of the ON switch.
    const auto b = enable.getBounds().toFloat();
    return { b.getX() - 18.0f, b.getCentreY() - 6.0f, 12.0f, 12.0f };
}

juce::Rectangle<float> ModulePanel::meterArea() const
{
    // Thin bar along the bottom of the header.
    auto r = getLocalBounds().reduced (14, 10).toFloat();
    return { r.getX(), r.getY() + 40.0f, r.getWidth(), 3.0f };
}

void ModulePanel::setActivity (float v)
{
    if (std::abs (v - activity) < 0.004f) return;
    activity = v;
    repaint (ledArea().expanded (8.0f).getSmallestIntegerContainer());
    repaint (meterArea().expanded (2.0f).getSmallestIntegerContainer());
}

void ModulePanel::setActivity2 (float v)
{
    if (std::abs (v - activity2) < 0.004f) return;
    activity2 = v;
    repaint (ledArea().expanded (8.0f).getSmallestIntegerContainer());
    repaint (meterArea().expanded (2.0f).getSmallestIntegerContainer());
}

void ModulePanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (4.0f);
    g.setColour (panelBg);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (enable.getToggleState() ? accent.withAlpha (0.55f) : edge);
    g.drawRoundedRectangle (r, 8.0f, 1.2f);

    // Activity LED: dark when idle, glows brighter the harder the stage works.
    const auto led = ledArea();
    const bool  isOn = enable.getToggleState();
    const float a  = isOn ? activity : 0.0f;
    const float a2 = isOn && splitMeter ? activity2 : 0.0f;
    const juce::Colour hot { 0xffff7a3d };
    const float la = ledFollowsBoth ? juce::jmax (a, a2) : a;
    if (la > 0.02f)
    {
        g.setColour (hot.withAlpha (0.35f * la));
        g.fillEllipse (led.expanded (5.0f * la));   // glow
    }
    g.setColour (edge.interpolatedWith (hot, la));
    g.fillEllipse (led);
    g.setColour (bg.withAlpha (0.6f));
    g.drawEllipse (led, 1.0f);

    // Activity meter bar (or two side by side: left = activity2, right = activity)
    auto drawBar = [&] (juce::Rectangle<float> m, float v)
    {
        g.setColour (bg);
        g.fillRoundedRectangle (m, 1.5f);
        if (v > 0.0f)
        {
            g.setColour (accent.interpolatedWith (hot, v));
            g.fillRoundedRectangle (m.withWidth (m.getWidth() * v), 1.5f);
        }
    };
    if (splitMeter)
    {
        auto m = meterArea();
        constexpr float barGap = 8.0f;
        const float half = (m.getWidth() - barGap) * 0.5f;
        drawBar (m.removeFromLeft (half), a2);
        drawBar (m.removeFromRight (half), a);
    }
    else
        drawBar (meterArea(), a);

    auto header = getLocalBounds().reduced (14, 10).removeFromTop (34).withTrimmedRight (80);  // leave room for LED + ON
    g.setColour (accent);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText (number, header.removeFromLeft (22), juce::Justification::topLeft);

    g.setColour (textCol);
    g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    g.drawText (title, header.removeFromTop (18), juce::Justification::topLeft);
    g.setColour (dimText);
    g.setFont (juce::FontOptions (11.5f));
    g.drawText (subtitle, header, juce::Justification::topLeft);
}

void ModulePanel::resized()
{
    auto r = getLocalBounds().reduced (12, 10);
    auto header = r.removeFromTop (38);
    enable.setBounds (header.removeFromRight (56).removeFromTop (24));
    r.removeFromTop (10);   // room for the activity meter

    const int n = (int) controls.size();
    if (n == 0) return;

    if (sideStack && n > 1)
    {
        // Left column: every control but the last, stacked top to bottom. Right: the last one.
        auto left = r.removeFromLeft (r.getWidth() / 2);
        controls.back()->setBounds (r);
        const int stackH = juce::jmin (left.getHeight() / (n - 1), 110);
        left = left.withSizeKeepingCentre (left.getWidth(), stackH * (n - 1));
        for (int i = 0; i < n - 1; ++i)
            controls[(size_t) i]->setBounds (left.removeFromTop (stackH));
        return;
    }

    const int cols = (n + rows - 1) / rows;
    const int cw = r.getWidth() / cols, rh = r.getHeight() / rows;

    for (int i = 0; i < n; ++i)
    {
        const int row = i / cols, col = i % cols;
        controls[(size_t) i]->setBounds (r.getX() + col * cw, r.getY() + row * rh, cw, rh);
    }
}
} // namespace ui

//==============================================================================
VocalChainEditor::VocalChainEditor (VocalChainProcessor& p)
    : AudioProcessorEditor (p), proc (p),
      rider     ("1", "Volume Rider",    "Rides level to target  |  synced timing", 1),
      comp1     ("2", "Comp-Gate",       "Classic de-ess + gate", 1),
      problemEQ ("3", "Problem EQ",      "Finds and cuts resonances: low, mid, high", 2),
      deEsser   ("4", "De-Esser",        "Finds the strongest S / SH and cuts it", 1),
      pultec    ("5", "Pultec EQ",       "EQP-1A style program equalizer", 2),
      comp2     ("6", "Compressor",      "Fast attack  |  auto release  |  auto makeup", 1),
      tape      ("7", "Tape",            "Analog tape saturation", 2),
      delay     ("8", "Delay",           "Synced to project tempo", 2),
      filters   ("9", "Filters",         "High / low pass, 24 dB/oct", 1),
      analyzer  (p)
{
    lnf.setColour (juce::Slider::rotarySliderFillColourId, accent);
    lnf.setColour (juce::Slider::rotarySliderOutlineColourId, edge);
    lnf.setColour (juce::Slider::thumbColourId, textCol);
    lnf.setColour (juce::Slider::textBoxTextColourId, textCol);
    lnf.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    lnf.setColour (juce::ComboBox::backgroundColourId, bg);
    lnf.setColour (juce::ComboBox::outlineColourId, edge);
    lnf.setColour (juce::ComboBox::textColourId, textCol);
    lnf.setColour (juce::ComboBox::arrowColourId, accent);
    lnf.setColour (juce::PopupMenu::backgroundColourId, panelBg);
    lnf.setColour (juce::PopupMenu::highlightedBackgroundColourId, accent);
    lnf.setColour (juce::PopupMenu::highlightedTextColourId, bg);
    setLookAndFeel (&lnf);

    analyzer.setWatermark (juce::ImageCache::getFromMemory (BinaryData::logo_png, BinaryData::logo_pngSize), "FXNKYCAT VOX");
    // 1. Volume rider
    rider.add (knob ("riderThresh", "Threshold"));
    rider.add (knob ("riderTarget", "Target Loudness"));
    rider.setSplitMeter (true);   // left bar: level over threshold, right bar: how much it rides
    bindEnable (rider, "riderOn");

    // 2. Light compressor
    comp1.add (knob ("comp1Thresh", "Threshold"));
    bindEnable (comp1, "comp1On");

    // 3. Problem EQ
    if (auto* range = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("deqRange")))
    {
        auto sw = std::make_unique<ui::SegmentedSwitch> (*range);
        sw->setVertical (true);
        sw->setReversed (true);   // Mezzo/Soprano on top, Bass/Baritone at the bottom
        auto lc = std::make_unique<ui::LabeledControl> ("Vocal Range", std::move (sw), true, 150);
        lc->setControlHeight (84);
        problemEQ.add (std::move (lc));
    }
    problemEQ.add (knob ("deqThresh", "Threshold"));
    problemEQ.setSideStack (true);
    bindEnable (problemEQ, "deqOn");

    // 4. De-esser
    deEsser.add (knob ("deessSens", "Detection"));
    deEsser.add (knob ("deessReduce", "Reduction"));
    deEsser.setSplitMeter (true);   // left bar: detection, right bar: reduction
    bindEnable (deEsser, "deessOn");

    // 5. Pultec — laid out like the hardware: low section on top, high section below.
    pultec.add (choice ("pLowFreq", "Low Freq (CPS)"));
    pultec.add (knob ("pLowBoost", "Low Boost"));
    pultec.add (knob ("pLowAtten", "Low Atten"));
    pultec.add (choice ("pHighAttFreq", "Atten Sel (KCS)"));
    pultec.add (choice ("pHighFreq", "High Freq (KCS)"));
    pultec.add (knob ("pHighBoost", "High Boost"));
    pultec.add (knob ("pBandwidth", "Bandwidth (Sharp-Broad)"));
    pultec.add (knob ("pHighAtten", "High Atten"));
    pultec.setSplitMeter (true, true);   // left bar: low boost, right bar: high boost
    bindEnable (pultec, "pultecOn");

    // 6. Compressor
    comp2.add (toggle ("comp2Ratio4", "Ratio", "3:1", "4:1"));
    comp2.add (toggle ("comp2SlowRel", "Release", "FAST", "SLOW"));
    comp2.add (knob ("comp2Thresh", "Threshold"));
    comp2.setSideStack (true);
    bindEnable (comp2, "comp2On");

    // 7. Tape saturation
    tape.add (toggle ("tapeOD", "Overdrive", "OFF", "ON"));
    tape.add (knob ("tapeAmount", "Saturation"));
    bindEnable (tape, "tapeOn");

    // 8. Delay: 1/8 | 1/4 | 1/2 switch, then dry/wet mix
    if (auto* div = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("delayDiv")))
        delay.add (std::make_unique<ui::LabeledControl> ("Time", std::make_unique<ui::SegmentedSwitch> (*div), true));
    delay.add (knob ("delayMix", "Mix"));
    bindEnable (delay, "delayOn");

    // 9. High / low pass
    filters.add (knob ("hpFreq", "High Pass"));
    filters.add (knob ("lpFreq", "Low Pass"));
    bindEnable (filters, "filterOn");

    for (auto* panel : { &rider, &comp1, &problemEQ, &deEsser, &pultec, &comp2, &tape, &delay, &filters })
        addAndMakeVisible (panel);

    // Global Timing note value, top right. Scroll the mouse wheel over it or click to pick.
    timingBox.addItemList (VocalChainProcessor::divisionNames(), 1);
    timingBox.setJustificationType (juce::Justification::centred);
    timingBox.setScrollWheelEnabled (true);
    comboAtts.push_back (std::make_unique<APVTS::ComboBoxAttachment> (proc.apvts, "division", timingBox));
    addAndMakeVisible (timingBox);

    // Input high-pass, top bar (all the way left = Off)
    inHpfSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 22);
    inHpfSlider.setColour (juce::Slider::trackColourId, accent);
    inHpfSlider.setColour (juce::Slider::backgroundColourId, edge);
    sliderAtts.push_back (std::make_unique<APVTS::SliderAttachment> (proc.apvts, "inHpf", inHpfSlider));
    addAndMakeVisible (inHpfSlider);

    // Stage 5/6 order switch, top bar
    buttonAtts.push_back (std::make_unique<APVTS::ButtonAttachment> (proc.apvts, "swap56", swapButton));
    addAndMakeVisible (swapButton);
    applySwap (swapButton.getToggleState());

    bpmLabel.setJustificationType (juce::Justification::centredLeft);
    bpmLabel.setColour (juce::Label::textColourId, dimText);
    bpmLabel.setFont (juce::FontOptions (12.5f, juce::Font::bold));
    addAndMakeVisible (bpmLabel);

    // Output auto gain switch, top bar
    buttonAtts.push_back (std::make_unique<APVTS::ButtonAttachment> (proc.apvts, "autoGain", autoGainButton));
    addAndMakeVisible (autoGainButton);

    // Presets: pick a slot to load it; SAVE stores the current settings in a slot.
    presetBox.setTextWhenNothingSelected ("Choose preset...");
    presetBox.setTextWhenNoChoicesAvailable ("No presets");
    presetBox.onChange = [this] { presetChosen(); };
    addAndMakeVisible (presetBox);
    saveButton.setColour (juce::TextButton::buttonColourId, edge);
    saveButton.setColour (juce::TextButton::textColourOffId, accent);
    saveButton.onClick = [this] { showSaveDialog(); };
    addAndMakeVisible (saveButton);
    refreshPresetList();

    addAndMakeVisible (analyzer);

    // Resizable from the bottom-right corner; panels, knobs and analyzer scale with the window.
    setResizable (true, true);
    setResizeLimits (1300, 790, 2680, 1600);
    setSize (1400, 900);
    startTimerHz (30);
}

void VocalChainEditor::timerCallback()
{
    ui::ModulePanel* panels[VocalChainProcessor::kNumStages] =
        { &rider, &comp1, &problemEQ, &deEsser, &pultec, &comp2, &tape, &delay, &filters };

    for (int s = 0; s < VocalChainProcessor::kNumStages; ++s)
    {
        const float v = proc.activity[s].exchange (0.0f);
        // Jump up instantly, fade out over roughly a quarter second.
        shownActivity[s] = juce::jmax (v, shownActivity[s] * 0.82f);
        if (shownActivity[s] < 0.01f) shownActivity[s] = 0.0f;
        panels[s]->setActivity (shownActivity[s]);

        // Left-hand bar of split meters (ignored by panels without one)
        shownActivity2[s] = juce::jmax (proc.activity2[s].exchange (0.0f), shownActivity2[s] * 0.82f);
        if (shownActivity2[s] < 0.01f) shownActivity2[s] = 0.0f;
        panels[s]->setActivity2 (shownActivity2[s]);
    }

    if (swapButton.getToggleState() != swapped)
        applySwap (swapButton.getToggleState());

    const auto bpmText = juce::String (proc.hostBpm.load(), 1) + " BPM";
    if (bpmLabel.getText() != bpmText)
        bpmLabel.setText (bpmText, juce::dontSendNotification);
}

void VocalChainEditor::applySwap (bool s)
{
    swapped = s;
    pultec.setNumber (s ? "6" : "5");
    comp2.setNumber  (s ? "5" : "6");
    if (getWidth() > 0) resized();
    repaint();
}

//------------------------------------------------------------------------------
// Presets
void VocalChainEditor::refreshPresetList (int selectSlot)
{
    presetBox.clear (juce::dontSendNotification);
    for (int s = 0; s < VocalChainProcessor::kNumPresets; ++s)
    {
        const auto name = proc.presetName (s);
        presetBox.addItem (juce::String (s + 1) + "   " + (name.isEmpty() ? "(empty)" : name), s + 1);
        presetBox.setItemEnabled (s + 1, name.isNotEmpty());   // empty slots can't be loaded
    }
    if (selectSlot >= 0)
        presetBox.setSelectedId (selectSlot + 1, juce::dontSendNotification);
}

void VocalChainEditor::presetChosen()
{
    const int slot = presetBox.getSelectedId() - 1;
    if (slot >= 0 && ! proc.loadPreset (slot))
        presetBox.setSelectedId (0, juce::dontSendNotification);
}

void VocalChainEditor::showSaveDialog()
{
    auto* w = new juce::AlertWindow ("Save preset", "Name it and choose one of the 10 slots.",
                                     juce::MessageBoxIconType::NoIcon, this);
    w->setLookAndFeel (&lnf);

    const int current = presetBox.getSelectedId() - 1;
    w->addTextEditor ("name", current >= 0 ? proc.presetName (current) : juce::String(), "Preset name:");

    juce::StringArray slots;
    int firstEmpty = -1;
    for (int s = 0; s < VocalChainProcessor::kNumPresets; ++s)
    {
        const auto name = proc.presetName (s);
        if (name.isEmpty() && firstEmpty < 0) firstEmpty = s;
        slots.add ("Slot " + juce::String (s + 1) + (name.isEmpty() ? "  (empty)" : "  (replace \"" + name + "\")"));
    }
    w->addComboBox ("slot", slots, "Save to:");
    w->getComboBoxComponent ("slot")->setSelectedItemIndex (current >= 0 ? current : juce::jmax (0, firstEmpty));

    w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<VocalChainEditor> safeThis (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, w] (int result)
    {
        if (result == 1 && safeThis != nullptr)
        {
            const int slot = w->getComboBoxComponent ("slot")->getSelectedItemIndex();
            const auto name = w->getTextEditorContents ("name");
            if (safeThis->proc.savePreset (slot, name))
                safeThis->refreshPresetList (slot);
        }
        w->setLookAndFeel (nullptr);
    }), true);
}

VocalChainEditor::~VocalChainEditor()
{
    stopTimer();
    sliderAtts.clear();
    buttonAtts.clear();
    comboAtts.clear();
    setLookAndFeel (nullptr);
}

std::unique_ptr<ui::LabeledControl> VocalChainEditor::knob (const char* id, const juce::String& caption)
{
    auto s = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
    s->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 20);
    if (juce::String (id).endsWith ("Thresh"))
        s->getProperties().set ("fillRight", true);   // gold arc from the pointer to the right end
    sliderAtts.push_back (std::make_unique<APVTS::SliderAttachment> (proc.apvts, id, *s));
    return std::make_unique<ui::LabeledControl> (caption, std::move (s), false);
}

std::unique_ptr<ui::LabeledControl> VocalChainEditor::toggle (const char* id, const juce::String& caption,
                                                              const juce::String& offText, const juce::String& onText)
{
    auto b = std::make_unique<ui::SwitchButton> (offText, onText);
    buttonAtts.push_back (std::make_unique<APVTS::ButtonAttachment> (proc.apvts, id, *b));
    return std::make_unique<ui::LabeledControl> (caption, std::move (b), true);
}

std::unique_ptr<ui::LabeledControl> VocalChainEditor::choice (const char* id, const juce::String& caption)
{
    auto box = std::make_unique<juce::ComboBox>();
    if (auto* param = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (id)))
        box->addItemList (param->choices, 1);
    box->setJustificationType (juce::Justification::centred);
    comboAtts.push_back (std::make_unique<APVTS::ComboBoxAttachment> (proc.apvts, id, *box));
    return std::make_unique<ui::LabeledControl> (caption, std::move (box), true);
}

void VocalChainEditor::bindEnable (ui::ModulePanel& panel, const char* id)
{
    buttonAtts.push_back (std::make_unique<APVTS::ButtonAttachment> (proc.apvts, id, panel.enable));
    panel.enable.onStateChange = [&panel] { panel.repaint(); };
}

void VocalChainEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);

    g.setColour (accent);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText ("INPUT HPF", capHpf,      juce::Justification::centredRight);
    g.drawText ("5/6 ORDER", capSwap,     juce::Justification::centredRight);
    g.drawText ("TIMING",    capTiming,   juce::Justification::centredRight);
    g.drawText ("PRESET",    capPreset,   juce::Justification::centredRight);
    g.drawText ("AUTO GAIN", capAutoGain, juce::Justification::centredRight);

    // Thin divider under the top bar
    g.setColour (edge);
    g.fillRect (hdrDivider);
}

void VocalChainEditor::resized()
{
    auto r = getLocalBounds().reduced (10, 0);
    auto top = r.removeFromTop (52).reduced (8, 4);
    hdrDivider = { r.getX() + 8, r.getY() - 1, r.getWidth() - 16, 1 };

    // One row. Right side (right to left): presets, then auto gain.
    saveButton.setBounds (top.removeFromRight (58).withSizeKeepingCentre (58, 28));
    top.removeFromRight (6);
    presetBox.setBounds (top.removeFromRight (200).withSizeKeepingCentre (200, 30));
    capPreset = top.removeFromRight (58);
    top.removeFromRight (14);
    autoGainButton.setBounds (top.removeFromRight (90).withSizeKeepingCentre (90, 28));
    capAutoGain = top.removeFromRight (80);

    // Left side (left to right): input HPF, 5/6 order, timing + project tempo.
    capHpf = top.removeFromLeft (70);
    inHpfSlider.setBounds (top.removeFromLeft (170).withSizeKeepingCentre (170, 26));
    top.removeFromLeft (12);
    capSwap = top.removeFromLeft (80);
    swapButton.setBounds (top.removeFromLeft (148).withSizeKeepingCentre (148, 28));
    top.removeFromLeft (12);
    capTiming = top.removeFromLeft (56);
    timingBox.setBounds (top.removeFromLeft (100).withSizeKeepingCentre (100, 30));
    top.removeFromLeft (6);
    bpmLabel.setBounds (top.removeFromLeft (juce::jmin (130, top.getWidth())).withSizeKeepingCentre (juce::jmin (130, top.getWidth()), 28));

    r.removeFromBottom (8);

    analyzer.setBounds (r.removeFromTop (r.getHeight() * 30 / 100));

    auto row1 = r.removeFromTop (r.getHeight() / 2 - 20);
    const int w1 = row1.getWidth();
    rider.setBounds     (row1.removeFromLeft (w1 * 26 / 100));
    comp1.setBounds     (row1.removeFromLeft (w1 * 18 / 100));
    problemEQ.setBounds (row1.removeFromLeft (w1 * 24 / 100));
    deEsser.setBounds   (row1);

    auto row2 = r;
    const int w2 = row2.getWidth();
    // Stages 5 and 6 trade places when the order switch is flipped.
    if (swapped)
    {
        comp2.setBounds  (row2.removeFromLeft (w2 * 22 / 100));
        pultec.setBounds (row2.removeFromLeft (w2 * 38 / 100));
    }
    else
    {
        pultec.setBounds (row2.removeFromLeft (w2 * 38 / 100));
        comp2.setBounds  (row2.removeFromLeft (w2 * 22 / 100));
    }
    tape.setBounds    (row2.removeFromLeft (w2 * 11 / 100));
    delay.setBounds   (row2.removeFromLeft (w2 * 11 / 100));
    filters.setBounds (row2);
}
