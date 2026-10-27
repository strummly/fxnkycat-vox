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
LabeledControl::LabeledControl (const juce::String& caption, std::unique_ptr<juce::Component> c, bool isCompact)
    : control (std::move (c)), compact (isCompact)
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
        control->setBounds (r.withSizeKeepingCentre (juce::jmin (r.getWidth() - 8, 130), 28));
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
    const float w = r.getWidth() / (float) n;
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    for (int i = 0; i < n; ++i)
    {
        auto seg = juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight());
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
    const int idx = juce::jlimit (0, n - 1, (int) ((float) e.x / (float) getWidth() * (float) n));
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

    // Watermark: the logo, very faint and centred, sitting under the grid and traces
    if (watermark.isValid())
    {
        g.setColour (textCol.withAlpha (0.07f));
        g.drawImage (watermark, outer.reduced (8.0f), juce::RectanglePlacement::centred, true);
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

void ModulePanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (4.0f);
    g.setColour (panelBg);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (enable.getToggleState() ? accent.withAlpha (0.55f) : edge);
    g.drawRoundedRectangle (r, 8.0f, 1.2f);

    // Activity LED: dark when idle, glows brighter the harder the stage works.
    const auto led = ledArea();
    const float a = enable.getToggleState() ? activity : 0.0f;
    const juce::Colour hot { 0xffff7a3d };
    if (a > 0.02f)
    {
        g.setColour (hot.withAlpha (0.35f * a));
        g.fillEllipse (led.expanded (5.0f * a));   // glow
    }
    g.setColour (edge.interpolatedWith (hot, a));
    g.fillEllipse (led);
    g.setColour (bg.withAlpha (0.6f));
    g.drawEllipse (led, 1.0f);

    // Activity meter bar
    const auto m = meterArea();
    g.setColour (bg);
    g.fillRoundedRectangle (m, 1.5f);
    if (a > 0.0f)
    {
        g.setColour (accent.interpolatedWith (hot, a));
        g.fillRoundedRectangle (m.withWidth (m.getWidth() * a), 1.5f);
    }

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
      problemEQ ("3", "Problem EQ",      "Finds and cuts resonances: low, mid, high", 1),
      deEsser   ("4", "De-Esser",        "Finds the strongest S / SH and cuts it", 1),
      pultec    ("5", "Pultec EQ",       "EQP-1A style program equalizer", 2),
      comp2     ("6", "Compressor",      "Fast attack  |  synced auto release", 1),
      tape      ("7", "Tape",            "Analog tape saturation", 1),
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

    analyzer.setWatermark (juce::ImageCache::getFromMemory (BinaryData::logo_png, BinaryData::logo_pngSize));

    // 1. Volume rider
    rider.add (knob ("riderThresh", "Threshold"));
    rider.add (knob ("riderTarget", "Target Loudness"));
    bindEnable (rider, "riderOn");

    // 2. Light compressor
    comp1.add (knob ("comp1Thresh", "Threshold"));
    bindEnable (comp1, "comp1On");

    // 3. Problem EQ
    problemEQ.add (knob ("deqThresh", "Threshold"));
    problemEQ.add (toggle ("deqFemale", "Voice", "MALE", "FEMALE"));
    bindEnable (problemEQ, "deqOn");

    // 4. De-esser
    deEsser.add (knob ("deessSens", "Detection"));
    deEsser.add (knob ("deessReduce", "Reduction"));
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
    bindEnable (pultec, "pultecOn");

    // 6. Compressor
    comp2.add (toggle ("comp2Ratio4", "Ratio", "3:1", "4:1"));
    comp2.add (knob ("comp2Thresh", "Threshold"));
    comp2.add (knob ("comp2Makeup", "Makeup Gain"));
    bindEnable (comp2, "comp2On");

    // 7. Tape saturation
    tape.add (knob ("tapeAmount", "Saturation"));
    bindEnable (tape, "tapeOn");

    // 8. Delay: 1/8 | 1/4 | 1/2 switch, then amount
    if (auto* div = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("delayDiv")))
        delay.add (std::make_unique<ui::LabeledControl> ("Time", std::make_unique<ui::SegmentedSwitch> (*div), true));
    delay.add (knob ("delayAmount", "Amount"));
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

    addAndMakeVisible (analyzer);

    // Resizable from the bottom-right corner; panels, knobs and analyzer scale with the window.
    setResizable (true, true);
    setResizeLimits (1280, 780, 2680, 1600);
    setSize (1400, 912);
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
    }

    if (swapButton.getToggleState() != swapped)
        applySwap (swapButton.getToggleState());

    const auto bpmText = juce::String (proc.hostBpm.load(), 1) + " BPM (project)";
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

    g.setColour (textCol);
    g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    g.drawText ("FXNKYCAT VOX", hdrTitle, juce::Justification::bottomLeft);

    g.setColour (accent);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText ("INPUT HPF", capHpf,    juce::Justification::centredRight);
    g.drawText ("5/6 ORDER", capSwap,   juce::Justification::centredRight);
    g.drawText ("TIMING",    capTiming, juce::Justification::centredRight);

    g.setColour (dimText);
    g.setFont (juce::FontOptions (11.5f));
    const juce::String mid = swapped ? "Compressor > Pultec" : "Pultec > Compressor";
    g.drawFittedText ("HPF > Rider > Comp-Gate > Problem EQ > De-Esser > " + mid + " > Tape > Delay > Filters",
                      hdrChain, juce::Justification::centredLeft, 1, 0.7f);
}

void VocalChainEditor::resized()
{
    auto r = getLocalBounds().reduced (10, 0);
    auto header = r.removeFromTop (64).reduced (8, 0);

    // Right side of the top bar, right to left
    bpmLabel.setBounds   (header.removeFromRight (130).withSizeKeepingCentre (130, 30));
    header.removeFromRight (8);
    timingBox.setBounds  (header.removeFromRight (110).withSizeKeepingCentre (110, 32));
    capTiming = header.removeFromRight (64);
    header.removeFromRight (12);
    swapButton.setBounds (header.removeFromRight (156).withSizeKeepingCentre (156, 28));
    capSwap = header.removeFromRight (84);
    header.removeFromRight (12);
    inHpfSlider.setBounds (header.removeFromRight (180).withSizeKeepingCentre (180, 26));
    capHpf = header.removeFromRight (78);
    header.removeFromRight (12);

    // Left side: name on top with the signal chain underneath
    hdrTitle = header.removeFromTop (36);
    hdrChain = header;

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
