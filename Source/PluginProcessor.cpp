#include "PluginProcessor.h"
#include "PluginEditor.h"

using APVTS = juce::AudioProcessorValueTreeState;

namespace
{
    std::unique_ptr<juce::AudioParameterFloat> fparam (const char* id, const char* name,
                                                       float lo, float hi, float def,
                                                       const char* unit, float step = 0.1f)
    {
        return std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { id, 1 }, name, juce::NormalisableRange<float> (lo, hi, step), def,
            juce::AudioParameterFloatAttributes().withLabel (unit));
    }

    std::unique_ptr<juce::AudioParameterBool> bparam (const char* id, const char* name, bool def)
    {
        return std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, def);
    }

    std::unique_ptr<juce::AudioParameterChoice> cparam (const char* id, const char* name,
                                                        juce::StringArray choices, int def)
    {
        return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, choices, def);
    }
}

// Note lengths offered for the delay, and their length in quarter-note beats.
// "Bar" lengths are in bars of the project's time signature (handled in processBlock).
static const char* const kDivNames[] = { "2 bar", "1 bar", "1/2d", "1/2", "1/2t", "1/4d", "1/4", "1/4t",
                                         "1/8d", "1/8", "1/8t", "1/16d", "1/16", "1/16t" };
static const double kDivBeats[] = { -2.0, -1.0, 3.0, 2.0, 4.0 / 3.0, 1.5, 1.0, 2.0 / 3.0,
                                    0.75, 0.5, 1.0 / 3.0, 0.375, 0.25, 1.0 / 6.0 };   // negative = bars

juce::StringArray VocalChainProcessor::divisionNames()
{
    juce::StringArray a;
    for (auto* n : kDivNames) a.add (n);
    return a;
}

APVTS::ParameterLayout VocalChainProcessor::createLayout()
{
    APVTS::ParameterLayout l;

    // Input high-pass (before stage 1). Lowest setting (20 Hz) = Off.
    {
        juce::NormalisableRange<float> r (20.0f, 500.0f, 1.0f);
        r.setSkewForCentre (100.0f);
        l.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { "inHpf", 1 }, "Input High Pass", r, 20.0f,
            juce::AudioParameterFloatAttributes()
                .withLabel ("Hz")
                .withStringFromValueFunction ([] (float v, int) { return v <= kInHpfOffHz ? juce::String ("Off") : juce::String (juce::roundToInt (v)) + " Hz"; })
                .withValueFromStringFunction ([] (const juce::String& s) { return s.trim().equalsIgnoreCase ("off") ? 20.0f : s.getFloatValue(); })));
    }

    // Global timing note value. Tempo itself comes from the project (host).
    l.add (cparam ("division", "Timing", divisionNames(), 6));   // default 1/4

    // 1. Volume rider
    l.add (bparam ("riderOn", "Rider On", false));
    l.add (fparam ("riderThresh", "Rider Threshold", -60.0f, 0.0f, 0.0f, "dB"));
    l.add (fparam ("riderTarget", "Rider Target", -40.0f, 0.0f, -18.0f, "dB"));

    // 2. Light compressor (2:1, 1 ms attack, 1/4 note release)
    l.add (bparam ("comp1On", "Comp-Gate On", false));
    l.add (fparam ("comp1Thresh", "Comp-Gate Threshold", -60.0f, 0.0f, 0.0f, "dB"));

    // 3. Problem-frequency EQ
    l.add (bparam ("deqOn", "Problem EQ On", false));
    l.add (fparam ("deqThresh", "Problem EQ Threshold", -60.0f, 0.0f, 0.0f, "dB"));
    l.add (bparam ("deqFemale", "Problem EQ Female", false));

    // 4. De-esser
    l.add (bparam ("deessOn", "De-esser On", false));
    l.add (fparam ("deessSens", "De-esser Detection", 0.0f, 100.0f, 50.0f, "%", 1.0f));
    l.add (fparam ("deessReduce", "De-esser Reduction", 0.0f, 20.0f, 6.0f, "dB"));

    // 5. Pultec EQP-1A style
    l.add (bparam ("pultecOn", "Pultec On", false));
    l.add (cparam ("pLowFreq", "Low Frequency", { "20 Hz", "30 Hz", "60 Hz", "100 Hz" }, 2));
    l.add (fparam ("pLowBoost", "Low Boost", 0.0f, 10.0f, 0.0f, "", 0.1f));
    l.add (fparam ("pLowAtten", "Low Atten", 0.0f, 10.0f, 0.0f, "", 0.1f));
    l.add (cparam ("pHighFreq", "High Frequency",
                   { "3 kHz", "4 kHz", "5 kHz", "8 kHz", "10 kHz", "12 kHz", "16 kHz" }, 4));
    l.add (fparam ("pHighBoost", "High Boost", 0.0f, 10.0f, 0.0f, "", 0.1f));
    l.add (fparam ("pBandwidth", "Bandwidth", 0.0f, 10.0f, 5.0f, "", 0.1f));
    l.add (cparam ("pHighAttFreq", "Atten Select", { "5 kHz", "10 kHz", "20 kHz" }, 1));
    l.add (fparam ("pHighAtten", "High Atten", 0.0f, 10.0f, 0.0f, "", 0.1f));

    // Order of stages 5 and 6: false = Pultec then Compressor, true = Compressor then Pultec (default)
    l.add (bparam ("swap56", "Swap EQ/Comp Order", true));

    // 6. Compressor (3:1 / 4:1, fast attack, auto release)
    l.add (bparam ("comp2On", "Compressor On", false));
    l.add (bparam ("comp2Ratio4", "Compressor 4:1", false));
    l.add (fparam ("comp2Thresh", "Compressor Threshold", -40.0f, 0.0f, 0.0f, "dB"));
    l.add (fparam ("comp2Makeup", "Compressor Makeup", 0.0f, 24.0f, 3.0f, "dB"));

    // Tape saturation (between 6 and 7)
    l.add (bparam ("tapeOn", "Tape On", false));
    l.add (fparam ("tapeAmount", "Tape Saturation", 0.0f, 100.0f, 20.0f, "%", 1.0f));

    // 7. Delay (1/8 note)
    l.add (bparam ("delayOn", "Delay On", false));
    l.add (cparam ("delayDiv", "Delay Time", { "1/8", "1/4", "1/2" }, 0));
    l.add (fparam ("delayAmount", "Delay Amount", 0.0f, 100.0f, 20.0f, "%", 1.0f));

    // 9. High / low pass (log-scaled knobs)
    auto freqParam = [] (const char* id, const char* name, float lo, float hi, float def)
    {
        juce::NormalisableRange<float> r (lo, hi, 1.0f);
        r.setSkewForCentre (std::sqrt (lo * hi));
        return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, r, def,
                                                            juce::AudioParameterFloatAttributes().withLabel ("Hz"));
    };
    l.add (bparam ("filterOn", "Filter On", false));
    l.add (freqParam ("hpFreq", "High Pass", 20.0f, 1000.0f, 80.0f));
    l.add (freqParam ("lpFreq", "Low Pass", 2000.0f, 20000.0f, 18000.0f));

    return l;
}

VocalChainProcessor::VocalChainProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    riderOn  = p ("riderOn");  comp1On = p ("comp1On"); deqOn = p ("deqOn"); deessOn = p ("deessOn");
    pultecOn = p ("pultecOn"); comp2On = p ("comp2On"); delayOn = p ("delayOn");

    riderThresh = p ("riderThresh"); riderTarget = p ("riderTarget");
    comp1Thresh = p ("comp1Thresh");
    deqThresh   = p ("deqThresh");   deqFemale   = p ("deqFemale");
    swap56      = p ("swap56");      inHpf       = p ("inHpf");
    deessSens   = p ("deessSens"); deessReduce = p ("deessReduce");

    pLowFreq   = p ("pLowFreq");   pLowBoost  = p ("pLowBoost");  pLowAtten    = p ("pLowAtten");
    pHighFreq  = p ("pHighFreq");  pHighBoost = p ("pHighBoost"); pBandwidth   = p ("pBandwidth");
    pHighAttFreq = p ("pHighAttFreq"); pHighAtten = p ("pHighAtten");

    comp2Ratio4 = p ("comp2Ratio4"); comp2Thresh = p ("comp2Thresh"); comp2Makeup = p ("comp2Makeup");
    division    = p ("division");    delayAmount = p ("delayAmount"); delayDiv = p ("delayDiv");
    tapeOn      = p ("tapeOn");      tapeAmount  = p ("tapeAmount");
    filterOn    = p ("filterOn");    hpFreq      = p ("hpFreq");     lpFreq = p ("lpFreq");
}

bool VocalChainProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

void VocalChainProcessor::prepareToPlay (double sr, int)
{
    rider.prepare (sr);
    inputHpf.prepare (sr);
    compGate.prepare (sr);                   // de-ess + gate, fixed times
    problemEQ.prepare (sr);
    deEsser.prepare (sr);
    pultec.prepare (sr);
    comp2.prepare (sr, 0.5f, 125.0f, 4.0f);  // fast attack; release stages set from tempo each block
    tape.prepare (sr);
    passFilters.prepare (sr);
    delay.prepare (sr);
}

void VocalChainProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numIn = getTotalNumInputChannels();
    for (int c = numIn; c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());

    const int nch = juce::jmin (numIn, vc::kMaxCh);
    const int n   = buffer.getNumSamples();
    if (nch <= 0 || n <= 0) return;

    auto on = [] (std::atomic<float>* a) { return a->load() > 0.5f; };

    // Tempo and time signature from the project; 120 BPM in 4/4 if the host doesn't say.
    double hostTempo = 120.0, beatsPerBar = 4.0;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm(); b.hasValue() && *b > 1.0)
                hostTempo = *b;
            if (auto ts = pos->getTimeSignature(); ts.hasValue() && ts->denominator > 0)
                beatsPerBar = ts->numerator * 4.0 / ts->denominator;
        }
    hostBpm.store ((float) hostTempo);

    const double quarter = 60.0 / hostTempo;   // one 1/4 note in seconds

    // Global Timing note value (default 1/4)
    const int    divIdx   = juce::jlimit (0, (int) std::size (kDivBeats) - 1, (int) division->load());
    const double divBeats = kDivBeats[divIdx] < 0 ? -kDivBeats[divIdx] * beatsPerBar : kDivBeats[divIdx];
    const double unit     = divBeats * quarter;

    // Delay has its own 1/8, 1/4, 1/2 switch
    static constexpr double kDelayBeats[] = { 0.5, 1.0, 2.0 };
    const float delaySec = (float) (kDelayBeats[juce::jlimit (0, 2, (int) delayDiv->load())] * quarter);

    rider.setTiming (unit);
    comp2.setRelease (unit * 0.25);              // fast part of auto release: 1/4 of the Timing note
    comp2.setSlowTimes (unit * 0.5, unit * 2.0); // slow part: builds over 1/2, lets go over 2x the Timing note

    float stageAct[kNumStages] {};

    // Dynamic stages report how many dB they are moving the level; 6 dB = fully lit.
    auto dynAct = [] (float& meter) { const float v = juce::jlimit (0.0f, 1.0f, meter / 6.0f); meter = 0; return v; };

    // Static stages: compare the audio before and after the stage.
    auto runStatic = [&] (int stage, float* const* ch, int nch, int len, auto&& fn)
    {
        for (int c = 0; c < nch; ++c)
            std::copy (ch[c], ch[c] + len, stageIn[(size_t) c].begin());
        fn();
        double eIn = 0, eDiff = 0;
        for (int c = 0; c < nch; ++c)
            for (int i = 0; i < len; ++i)
            {
                const float x = stageIn[(size_t) c][(size_t) i];
                const float d = ch[c][i] - x;
                eIn += x * x; eDiff += d * d;
            }
        if (eIn / (len * nch) < 1.0e-7)   // ignore near-silence (below about -70 dB)
            return;
        const float changeDb = 10.0f * std::log10 ((float) (eDiff / eIn) + 1.0e-12f);
        stageAct[stage] = juce::jmax (stageAct[stage], juce::jlimit (0.0f, 1.0f, juce::jmap (changeDb, -40.0f, -10.0f, 0.0f, 1.0f)));
    };

    // Process in chunks so per-stage measurement needs no allocation.
    for (int start = 0; start < n; start += kChunk)
    {
        const int len = juce::jmin (kChunk, n - start);
        float* ch[vc::kMaxCh] = { buffer.getWritePointer (0) + start,
                                  nch > 1 ? buffer.getWritePointer (1) + start : nullptr };

        preTap.push (ch, nch, len);   // "before" for the analyzer (raw input)

        // Input high-pass, ahead of stage 1
        if (const float hz = inHpf->load(); hz > kInHpfOffHz)
            inputHpf.process (ch, nch, len, hz);

        // 1. Volume rider
        if (on (riderOn))
        {
            rider.process (ch, nch, len, riderThresh->load(), riderTarget->load());
            stageAct[0] = juce::jmax (stageAct[0], dynAct (rider.meter));
        }

        // 2. Light compressor
        if (on (comp1On))
        {
            compGate.process (ch, nch, len, comp1Thresh->load());
            stageAct[1] = juce::jmax (stageAct[1], dynAct (compGate.meter));
        }

        // 3. Problem-frequency EQ
        if (on (deqOn))
        {
            problemEQ.process (ch, nch, len, deqThresh->load(), on (deqFemale));
            float m = problemEQ.takeMeter();
            stageAct[2] = juce::jmax (stageAct[2], dynAct (m));
        }

        // 4. De-esser
        if (on (deessOn))
        {
            deEsser.process (ch, nch, len, deessSens->load(), deessReduce->load());
            float m = deEsser.takeMeter();
            stageAct[3] = juce::jmax (stageAct[3], dynAct (m));
        }

        // 5. Pultec
        auto runPultec = [&]
        {
            if (! on (pultecOn)) return;
            pultec.update ((int) pLowFreq->load(), pLowBoost->load(), pLowAtten->load(),
                           (int) pHighFreq->load(), pHighBoost->load(), pBandwidth->load(),
                           (int) pHighAttFreq->load(), pHighAtten->load());
            runStatic (4, ch, nch, len, [&] { pultec.process (ch, nch, len); });
        };

        // 6. Compressor (meter shows gain reduction, not makeup)
        auto runComp2 = [&]
        {
            if (! on (comp2On)) return;
            comp2.process (ch, nch, len, comp2Thresh->load(), on (comp2Ratio4) ? 4.0f : 3.0f,
                           comp2Makeup->load(), true);
            stageAct[5] = juce::jmax (stageAct[5], dynAct (comp2.meter));
        };

        // Order switch: Comp > EQ (default) or EQ > Comp
        if (on (swap56)) { runComp2(); runPultec(); }
        else             { runPultec(); runComp2(); }

        // 7. Tape saturation
        if (on (tapeOn))
            runStatic (6, ch, nch, len, [&] { tape.process (ch, nch, len, tapeAmount->load()); });

        // 8. Delay
        if (on (delayOn))
            runStatic (7, ch, nch, len, [&] { delay.process (ch, nch, len, delaySec, delayAmount->load() * 0.01f); });

        // 9. High / low pass
        if (on (filterOn))
            runStatic (8, ch, nch, len, [&] { passFilters.process (ch, nch, len, hpFreq->load(), lpFreq->load()); });

        postTap.push (ch, nch, len);  // "after" for the analyzer
    }

    // Publish to the display: keep the peak until the editor reads it.
    for (int s = 0; s < kNumStages; ++s)
    {
        float prev = activity[s].load();
        while (stageAct[s] > prev && ! activity[s].compare_exchange_weak (prev, stageAct[s])) {}
    }
}

juce::AudioProcessorEditor* VocalChainProcessor::createEditor()
{
    return new VocalChainEditor (*this);
}

void VocalChainProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void VocalChainProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VocalChainProcessor();
}
