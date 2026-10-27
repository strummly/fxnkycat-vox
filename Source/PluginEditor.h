#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include "PluginProcessor.h"

namespace ui
{
    // Spectrum of the plugin's input (before) and output (after), overlaid.
    class SpectrumAnalyzer : public juce::Component, private juce::Timer
    {
    public:
        explicit SpectrumAnalyzer (VocalChainProcessor& p);
        ~SpectrumAnalyzer() override { stopTimer(); }
        void paint (juce::Graphics&) override;
        void setWatermark (juce::Image img, juce::String name)
        {
            watermark = std::move (img); watermarkText = std::move (name); repaint();
        }

    private:
        juce::Image  watermark;       // faint logo behind the traces
        juce::String watermarkText;   // plugin name, drawn beside the logo
        static constexpr int kOrder = 12, kSize = 1 << kOrder, kBins = kSize / 2;
        static constexpr float kMinDb = -96.0f, kMaxDb = 0.0f, kMinHz = 20.0f, kMaxHz = 20000.0f;

        struct Trace
        {
            AnalyzerTap* tap = nullptr;
            std::vector<float> history = std::vector<float> ((size_t) kSize, 0.0f);
            int pos = 0;
            std::vector<float> levelDb = std::vector<float> ((size_t) kBins, kMinDb);
        };

        void timerCallback() override;
        void analyse (Trace&);
        juce::Path makePath (const Trace&, juce::Rectangle<float> area, bool closed) const;
        float xForHz (float hz, juce::Rectangle<float> area) const;
        float yForDb (float db, juce::Rectangle<float> area) const;

        VocalChainProcessor& proc;
        juce::dsp::FFT fft { kOrder };
        juce::dsp::WindowingFunction<float> window { (size_t) kSize, juce::dsp::WindowingFunction<float>::hann, false };
        std::vector<float> fftData = std::vector<float> ((size_t) kSize * 2, 0.0f);
        std::vector<float> scratch = std::vector<float> ((size_t) AnalyzerTap::kSize, 0.0f);
        Trace pre, post;
    };

    // Knob drawing. A slider with the "fillRight" property set draws its gold arc from
    // the pointer to the right-hand end instead of from the left-hand start (thresholds).
    class KnobLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                               float startAngle, float endAngle, juce::Slider&) override;
    };

    // A two-state switch that shows different text for off / on (e.g. MALE / FEMALE).
    class SwitchButton : public juce::Button
    {
    public:
        SwitchButton (juce::String offText, juce::String onText);
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    private:
        juce::String off, on;
    };

    // Any control with a caption above it.
    class LabeledControl : public juce::Component
    {
    public:
        LabeledControl (const juce::String& caption, std::unique_ptr<juce::Component> control, bool compact,
                        int compactMaxWidth = 130);
        void resized() override;
        juce::Component& get() { return *control; }
        void setControlHeight (int h) { controlHeight = h; resized(); }   // compact only
    private:
        juce::Label label;
        std::unique_ptr<juce::Component> control;
        bool compact;
        int  maxWidth;
        int  controlHeight = 28;
    };

    // Multi-position switch (e.g. 1/8 | 1/4 | 1/2) bound to a choice parameter.
    class SegmentedSwitch : public juce::Component
    {
    public:
        explicit SegmentedSwitch (juce::AudioParameterChoice& param);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void setVertical (bool v) { vertical = v; repaint(); }   // options stacked top to bottom
        void setReversed (bool r) { reversed = r; repaint(); }   // show the last option first
    private:
        int slotFor (int option) const { return reversed ? options.size() - 1 - option : option; }
        juce::StringArray options;
        int selected = 0;
        bool vertical = false, reversed = false;
        juce::ParameterAttachment attachment;
    };

    // A titled box for one stage of the chain, with its own on/off switch.
    class ModulePanel : public juce::Component
    {
    public:
        ModulePanel (juce::String number, juce::String title, juce::String subtitle, int rows);
        void add (std::unique_ptr<LabeledControl> c);
        void paint (juce::Graphics&) override;
        void resized() override;
        void setActivity (float v);   // 0-1, drives the LED and meter bar
        // Split the meter into two bars side by side: left = v2 (setActivity2), right = setActivity.
        // ledBoth: the LED follows whichever bar is higher (otherwise only the right one).
        void setSplitMeter (bool s, bool ledBoth = false) { splitMeter = s; ledFollowsBoth = ledBoth; repaint(); }
        void setActivity2 (float v);
        void setNumber (const juce::String& n) { if (n != number) { number = n; repaint(); } }
        // Switches stacked in a column on the left, the last control (a knob) on the right.
        void setSideStack (bool s) { sideStack = s; resized(); }
        juce::ToggleButton enable;
    private:
        bool sideStack = false;
        juce::Rectangle<float> ledArea() const;
        juce::Rectangle<float> meterArea() const;
        float activity = 0.0f, activity2 = 0.0f;
        bool  splitMeter = false, ledFollowsBoth = false;
        juce::String number, title, subtitle;
        int rows;
        std::vector<std::unique_ptr<LabeledControl>> controls;
    };
}

class VocalChainEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit VocalChainEditor (VocalChainProcessor&);
    ~VocalChainEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using APVTS = juce::AudioProcessorValueTreeState;

    std::unique_ptr<ui::LabeledControl> knob (const char* id, const juce::String& caption);
    std::unique_ptr<ui::LabeledControl> toggle (const char* id, const juce::String& caption,
                                                const juce::String& offText, const juce::String& onText);
    std::unique_ptr<ui::LabeledControl> choice (const char* id, const juce::String& caption);
    void bindEnable (ui::ModulePanel& panel, const char* id);
    void timerCallback() override;
    float shownActivity[VocalChainProcessor::kNumStages] {};

    VocalChainProcessor& proc;
    ui::KnobLookAndFeel lnf;
    float shownActivity2[VocalChainProcessor::kNumStages] {};   // left-hand bars of split meters

    // Panels first so they are destroyed after the attachments below.
    ui::ModulePanel rider, comp1, problemEQ, deEsser, pultec, comp2, tape, delay, filters;
    juce::ComboBox timingBox;   // global Timing note value (scroll or click)
    juce::Label    bpmLabel;    // shows the project tempo
    ui::SwitchButton swapButton { "EQ > COMP", "COMP > EQ" };   // order of stages 5 and 6
    bool swapped = false;
    void applySwap (bool s);
    juce::Slider inHpfSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };   // input high-pass

    ui::SwitchButton autoGainButton { "OFF", "ON" };              // output auto gain

    // Presets: 10 user slots
    juce::ComboBox   presetBox;
    juce::TextButton saveButton { "SAVE" };
    void refreshPresetList (int selectSlot = -1);
    void presetChosen();
    void showSaveDialog();

    // Top bar areas, set in resized() and drawn in paint()
    juce::Rectangle<int> capHpf, capSwap, capTiming, capPreset, capAutoGain, hdrDivider;
    ui::SpectrumAnalyzer analyzer;

    std::vector<std::unique_ptr<APVTS::SliderAttachment>>   sliderAtts;
    std::vector<std::unique_ptr<APVTS::ButtonAttachment>>   buttonAtts;
    std::vector<std::unique_ptr<APVTS::ComboBoxAttachment>> comboAtts;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalChainEditor)
};
