#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <iterator>
#include "DSP.h"

// Lock-free hand-off of mono audio from the audio thread to the analyzer display.
struct AnalyzerTap
{
    static constexpr int kSize = 32768;

    juce::AbstractFifo fifo { kSize };
    std::vector<float> buf = std::vector<float> ((size_t) kSize, 0.0f);

    void push (const float* const* ch, int nch, int n)
    {
        int i = 0;
        auto w = fifo.write (n);   // drops samples if the display isn't keeping up
        w.forEach ([&] (int idx)
        {
            float s = 0;
            for (int c = 0; c < nch; ++c) s += ch[c][i];
            buf[(size_t) idx] = s / (float) nch;
            ++i;
        });
    }

    int pull (float* dest, int maxN)
    {
        int i = 0;
        auto r = fifo.read (juce::jmin (maxN, fifo.getNumReady()));
        r.forEach ([&] (int idx) { dest[i++] = buf[(size_t) idx]; });
        return i;
    }
};

class VocalChainProcessor : public juce::AudioProcessor
{
public:
    AnalyzerTap preTap, postTap;   // input and output of the whole chain

    // How hard each stage is working right now, 0-1 (peak since the editor last read it).
    // Order: rider, light comp, problem EQ, de-esser, pultec, compressor, tape, delay, filters.
    static constexpr int kNumStages = 9;
    std::atomic<float> activity[kNumStages] {};
    // Second (left-hand) meter for stages with split meters, 0-1:
    // rider = level over threshold, de-esser = detection, Pultec = low boost.
    std::atomic<float> activity2[kNumStages] {};

    VocalChainProcessor();
    ~VocalChainProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "fxnkycat vox"; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static juce::StringArray divisionNames();

    // User presets: 10 slots saved as XML files shared by every project/DAW.
    static constexpr int kNumPresets = 10;
    static juce::File presetFolder();
    static juce::File presetFile (int slot);          // slot 0-9
    juce::String presetName (int slot) const;         // empty if the slot is unused
    bool savePreset (int slot, const juce::String& name);
    bool loadPreset (int slot);

    std::atomic<float> hostBpm { 120.0f };   // project tempo, for display
    static constexpr float kInHpfOffHz = 20.5f;  // input high-pass at/below this = Off
    juce::AudioProcessorValueTreeState apvts;

private:
    std::atomic<float>* p (const char* id) { return apvts.getRawParameterValue (id); }

    // Stage on/off
    std::atomic<float> *riderOn, *comp1On, *deqOn, *deessOn, *pultecOn, *comp2On, *delayOn;
    // 1 Rider
    std::atomic<float> *riderThresh, *riderTarget;
    // 2 Light compressor
    std::atomic<float> *comp1Thresh;
    // 3 Problem EQ
    std::atomic<float> *deqThresh, *deqRange;
    // 4 De-esser
    std::atomic<float> *deessSens, *deessReduce;
    std::atomic<float> *swap56, *inHpf, *autoGain;
    // 5 Pultec
    std::atomic<float> *pLowFreq, *pLowBoost, *pLowAtten, *pHighFreq, *pHighBoost,
                       *pBandwidth, *pHighAttFreq, *pHighAtten;
    // 6 Compressor
    std::atomic<float> *comp2Ratio4, *comp2Thresh, *comp2SlowRel;
    // Global tempo + 7 Delay
    std::atomic<float> *division, *delayMix, *delayDiv;
    // Tape
    std::atomic<float> *tapeOn, *tapeAmount, *tapeOD;
    // 9 High / low pass
    std::atomic<float> *filterOn, *hpFreq, *lpFreq;

    // Auto gain state (audio thread only)
    double currentRate = 44100.0;
    float  agIn = 0.0f, agOut = 0.0f, agGainDb = 0.0f;

    static constexpr int kChunk = 512;
    std::array<std::array<float, kChunk>, vc::kMaxCh> stageIn {};

    vc::VolumeRider rider;
    vc::InputHighPass inputHpf;
    vc::CompGate    compGate;
    vc::Compressor  comp2;
    vc::ProblemEQ   problemEQ;
    vc::DeEsser     deEsser;
    vc::PultecEQ    pultec;
    vc::TapeSaturation tape;
    vc::TempoDelay  delay;
    vc::PassFilters passFilters;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalChainProcessor)
};
