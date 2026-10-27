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
        void setWatermark (juce::Image img) { watermark = std::move (img); repaint(); }

    private:
        juce::Image watermark;   // faint logo behind the traces
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
        LabeledControl (const juce::String& caption, std::unique_ptr<juce::Component> control, bool compact);
        void resized() override;
        juce::Component& get() { return *control; }
    private:
        juce::Label label;
        std::unique_ptr<juce::Component> control;
        bool compact;
    };

    // Multi-position switch (e.g. 1/8 | 1/4 | 1/2) bound to a choice parameter.
    class SegmentedSwitch : public juce::Component
    {
    public:
        explicit SegmentedSwitch (juce::AudioParameterChoice& param);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
    private:
        juce::StringArray options;
        int selected = 0;
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
        void setNumber (const juce::String& n) { if (n != number) { number = n; repaint(); } }
        juce::ToggleButton enable;
    private:
        juce::Rectangle<float> ledArea() const;
        juce::Rectangle<float> meterArea() const;
        float activity = 0.0f;
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
    juce::LookAndFeel_V4 lnf;

    // Panels first so they are destroyed after the attachments below.
    ui::ModulePanel rider, comp1, problemEQ, deEsser, pultec, comp2, tape, delay, filters;
    juce::ComboBox timingBox;   // global Timing note value (scroll or click)
    juce::Label    bpmLabel;    // shows the project tempo
    ui::SwitchButton swapButton { "EQ > COMP", "COMP > EQ" };   // order of stages 5 and 6
    bool swapped = false;
    void applySwap (bool s);
    juce::Slider inHpfSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };   // input high-pass

    // Top bar areas, set in resized() and drawn in paint()
    juce::Rectangle<int> hdrTitle, hdrChain, capHpf, capSwap, capTiming;
    ui::SpectrumAnalyzer analyzer;

    std::vector<std::unique_ptr<APVTS::SliderAttachment>>   sliderAtts;
    std::vector<std::unique_ptr<APVTS::ButtonAttachment>>   buttonAtts;
    std::vector<std::unique_ptr<APVTS::ComboBoxAttachment>> comboAtts;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalChainEditor)
};
