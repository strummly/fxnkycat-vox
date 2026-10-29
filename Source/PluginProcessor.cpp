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

    // Output auto gain: keeps the output about as loud as the input
    l.add (bparam ("autoGain", "Auto Gain", false));
    l.add (bparam ("bypass", "Bypass", false));   // whole plugin; also the DAW's own bypass

    // Global timing note value. Tempo itself comes from the project (host).
    l.add (cparam ("division", "Timing", divisionNames(), 6));   // default 1/4

    // 1. Volume rider
    l.add (bparam ("riderOn", "Rider On", false));
    l.add (fparam ("riderThresh", "Rider Noise Floor", -60.0f, 0.0f, 0.0f, "dB"));
    l.add (fparam ("riderTarget", "Rider Target", -40.0f, 0.0f, -18.0f, "dB"));
    l.add (cparam ("riderMode", "Rider Speed", { "Natural", "Balanced", "Fast" }, 0));

    // 2. Light compressor (2:1, 1 ms attack, 1/4 note release)
    l.add (bparam ("comp1On", "Comp-Gate On", false));
    l.add (fparam ("comp1Thresh", "Comp-Gate Threshold", -60.0f, 0.0f, 0.0f, "dB"));

    // 3. Problem-frequency EQ
    l.add (bparam ("deqOn", "Problem EQ On", false));
    l.add (fparam ("deqThresh", "Problem EQ Threshold", -60.0f, 0.0f, 0.0f, "dB"));
    // Vocal range (top bar): sets the detection ranges of stages 2, 3 and 4.
    // ID kept as "deqRange" so older sessions and presets still load.
    l.add (cparam ("deqRange", "Vocal Range",
                   { "Bass/Baritone", "Tenor/Alto", "Mezzo/Soprano" }, 1));   // default Tenor/Alto

    // 4. De-esser
    l.add (bparam ("deessOn", "De-esser On", false));
    l.add (fparam ("deessSens", "De-esser Detection", 0.0f, 100.0f, 0.0f, "%", 1.0f));
    l.add (fparam ("deessReduce", "De-esser Reduction", 0.0f, 20.0f, 0.0f, "dB"));
    l.add (bparam ("deessSolo", "De-esser Solo", false));   // hear only what is being detected

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
    l.add (bparam ("swap56", "Compressor Before EQ", false));   // default: Pultec EQ (6) then Compressor (7)

    // 6. Compressor (3:1 / 4:1, fast attack, auto release)
    l.add (bparam ("comp2On", "Compressor On", false));
    l.add (bparam ("comp2Ratio4", "Compressor 4:1", false));
    l.add (fparam ("comp2Thresh", "Compressor Threshold", -40.0f, 0.0f, 0.0f, "dB"));
    l.add (bparam ("comp2SlowRel", "Compressor Slow Release", false));   // makeup gain is automatic

    // 7. Tape saturation
    l.add (bparam ("tapeOn", "Tape On", false));
    l.add (fparam ("tapeAmount", "Tape Saturation", 0.0f, 100.0f, 0.0f, "%", 1.0f));
    l.add (bparam ("tapeOD", "Tape Overdrive", false));

    // 8. Delay: echoes at full level, knob is the dry/wet mix
    l.add (bparam ("delayOn", "Delay On", false));
    l.add (cparam ("delayDiv", "Delay Time", { "1/8", "1/4", "1/2" }, 0));
    l.add (fparam ("delayMix", "Delay Mix", 0.0f, 100.0f, 0.0f, "%", 1.0f));

    // 9. High / low pass (log-scaled knobs)
    auto freqParam = [] (const char* id, const char* name, float lo, float hi, float def)
    {
        juce::NormalisableRange<float> r (lo, hi, 1.0f);
        r.setSkewForCentre (std::sqrt (lo * hi));
        return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, r, def,
                                                            juce::AudioParameterFloatAttributes().withLabel ("Hz"));
    };
    // Reverb (stage 9)
    l.add (bparam ("reverbOn", "Reverb On", false));
    l.add (cparam ("revType", "Reverb Type", { "Room", "Hall", "Plate", "Chamber", "Shimmer" }, 2));   // default Plate
    l.add (fparam ("revMix", "Reverb Mix", 0.0f, 100.0f, 0.0f, "%", 1.0f));
    // Light compressor between stages 3 and 4: runs in the background (no panel).
    l.add (bparam ("lightCompOn", "Light Comp On", true));
    // Warmth & Air (stage 4)
    l.add (bparam ("colorOn", "Harmonics On", false));
    l.add (fparam ("warmth", "Warmth", 0.0f, 100.0f, 0.0f, "%", 1.0f));
    l.add (fparam ("air", "Exciter", 0.0f, 100.0f, 0.0f, "%", 1.0f));

    l.add (bparam ("filterOn", "Filter On", false));
    l.add (freqParam ("hpFreq", "High Pass", 20.0f, 1000.0f, 20.0f));
    l.add (freqParam ("lpFreq", "Low Pass", 2000.0f, 20000.0f, 20000.0f));

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

    riderThresh = p ("riderThresh"); riderTarget = p ("riderTarget"); riderMode = p ("riderMode");
    deessSolo   = p ("deessSolo");
    comp1Thresh = p ("comp1Thresh");
    deqThresh   = p ("deqThresh");   deqRange    = p ("deqRange");   autoGain = p ("autoGain");
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
    swap56      = p ("swap56");      inHpf       = p ("inHpf");
    deessSens   = p ("deessSens"); deessReduce = p ("deessReduce");

    pLowFreq   = p ("pLowFreq");   pLowBoost  = p ("pLowBoost");  pLowAtten    = p ("pLowAtten");
    pHighFreq  = p ("pHighFreq");  pHighBoost = p ("pHighBoost"); pBandwidth   = p ("pBandwidth");
    pHighAttFreq = p ("pHighAttFreq"); pHighAtten = p ("pHighAtten");

    comp2Ratio4 = p ("comp2Ratio4"); comp2Thresh = p ("comp2Thresh"); comp2SlowRel = p ("comp2SlowRel");
    division    = p ("division");    delayMix    = p ("delayMix");    delayDiv = p ("delayDiv");
    tapeOn      = p ("tapeOn");      tapeAmount  = p ("tapeAmount");  tapeOD   = p ("tapeOD");
    filterOn    = p ("filterOn");    hpFreq      = p ("hpFreq");     lpFreq = p ("lpFreq");
    reverbOn    = p ("reverbOn");    revType     = p ("revType");    revMix = p ("revMix");
    lightCompOn = p ("lightCompOn");
    colorOn     = p ("colorOn");     warmth      = p ("warmth");      air = p ("air");
}

bool VocalChainProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

void VocalChainProcessor::prepareToPlay (double sr, int samplesPerBlock)
{
    rider.prepare (sr);
    inputHpf.prepare (sr);
    currentRate = sr;
    dryBuf.setSize (vc::kMaxCh, juce::jmax (samplesPerBlock, 512));   // bypass crossfade (no allocation later)
    wasBypassed = bypassParam != nullptr && bypassParam->get();
    agIn = agOut = 0.0f; agGainDb = 0.0f;
    compGate.prepare (sr);                   // de-ess + gate, fixed times
    problemEQ.prepare (sr);
    deEsser.prepare (sr);
    pultec.prepare (sr);
    comp2.prepare (sr, 0.5f, 125.0f, 4.0f);  // fast attack; release stages set from tempo each block
    tape.prepare (sr);
    passFilters.prepare (sr);
    delay.prepare (sr);
    reverb.prepare (sr);
    lightComp.prepare (sr);
    warmthAir.prepare (sr);
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

    // Bypass: pass the input straight through (the analyzer shows it as both before and after).
    // Switching on or off crossfades over one block so it never clicks.
    const bool bypassNow = bypassParam != nullptr && bypassParam->get();
    if (bypassNow && wasBypassed)
    {
        const float* const* rd = buffer.getArrayOfReadPointers();
        preTap.push (rd, nch, n);
        postTap.push (rd, nch, n);
        return;
    }
    const bool bypassFade = bypassNow != wasBypassed;
    if (bypassFade)
    {
        dryBuf.setSize (nch, n, false, false, true);
        for (int c = 0; c < nch; ++c) dryBuf.copyFrom (c, 0, buffer, c, 0, n);
    }

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

    rider.setTiming (unit, juce::jlimit (0, 2, (int) riderMode->load()));   // Natural, Balanced, Fast
    lightComp.setRelease (unit);   // background compressor: release = Timing note
    reverb.setRelease (unit * 0.25);  // reverb swells back up over a quarter of the Timing note
    reverb.setTempo (quarter);        // pre-delay and length follow the song tempo
    delay.ducker.setRelease (unit * 0.25);   // delay echoes swell back the same way
    reverb.setType ((int) revType->load());

    // Compressor release: Fast (default) or Slow (4x longer), both program-dependent.
    const double relScale = on (comp2SlowRel) ? 4.0 : 1.0;
    comp2.setRelease (unit * 0.25 * relScale);              // fast part of the auto release
    comp2.setSlowTimes (unit * 0.5, unit * 2.0 * relScale); // slow part: builds over 1/2, lets go over 2x

    // Compressor auto makeup: half the gain reduction a full-scale signal would get.
    const float c2Thresh = comp2Thresh->load();
    const float c2Ratio  = on (comp2Ratio4) ? 4.0f : 3.0f;
    const float c2Makeup = 0.5f * -c2Thresh * (1.0f - 1.0f / c2Ratio);

    float stageAct[kNumStages] {}, stageAct2[kNumStages] {};

    // Change in dB (relative to the input) -> 0-1, same scale as the static stages.
    auto changeAct = [] (float changeDb) { return juce::jlimit (0.0f, 1.0f, juce::jmap (changeDb, -40.0f, -10.0f, 0.0f, 1.0f)); };

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
    const int vocalRange = juce::jlimit (0, 2, (int) deqRange->load());   // top-bar Vocal Range

    for (int start = 0; start < n; start += kChunk)
    {
        const int len = juce::jmin (kChunk, n - start);
        float* ch[vc::kMaxCh] = { buffer.getWritePointer (0) + start,
                                  nch > 1 ? buffer.getWritePointer (1) + start : nullptr };

        preTap.push (ch, nch, len);   // "before" for the analyzer (raw input)

        auto meanSquare = [&]
        {
            double e = 0;
            for (int c = 0; c < nch; ++c)
                for (int i = 0; i < len; ++i) e += ch[c][i] * ch[c][i];
            return (float) (e / (double) (len * nch));
        };
        const float inMs = meanSquare();   // for auto gain

        // Input high-pass, ahead of stage 1
        if (const float hz = inHpf->load(); hz > kInHpfOffHz)
            inputHpf.process (ch, nch, len, hz);

        // 1. Volume rider
        if (on (riderOn))
        {
            rider.process (ch, nch, len, riderThresh->load(), riderTarget->load());
            stageAct[0] = juce::jmax (stageAct[0], dynAct (rider.meter));
            // Left bar: how far the level goes over the noise floor, full at 12 dB
            stageAct2[0] = juce::jmax (stageAct2[0], juce::jlimit (0.0f, 1.0f, rider.overMeter / 12.0f));
            rider.overMeter = 0;
        }

        // 2. Light compressor
        if (on (comp1On))
        {
            compGate.setRange (vocalRange);
            compGate.setTiming (unit);   // gate hold / close follow the Timing note
            // Threshold = de-ess only. Gate opens at the rider's Noise Floor when the rider is on, else automatic.
            compGate.process (ch, nch, len, comp1Thresh->load(), on (riderOn), riderThresh->load());
            stageAct[1]  = juce::jmax (stageAct[1],  dynAct (compGate.meter));       // meter bar: de-esser
            stageAct2[1] = juce::jmax (stageAct2[1], dynAct (compGate.gateMeter));   // LED: gate
        }

        // 3. Problem-frequency EQ
        if (on (deqOn))
        {
            problemEQ.process (ch, nch, len, deqThresh->load(), vocalRange);
            float m = problemEQ.takeMeter();
            stageAct[2] = juce::jmax (stageAct[2], dynAct (m));
        }

        // Background light compressor (1.5:1, auto threshold, output matched). No panel.
        if (on (lightCompOn))
        {
            lightComp.process (ch, nch, len);
            lightComp.meter = 0;
        }

        // 4. Warmth & Air (tube saturation + harmonic exciter)
        if (on (colorOn))
        {
            warmthAir.setVocalRange (vocalRange);   // lift and air bands follow the top-bar vocal range
            runStatic (3, ch, nch, len, [&] { warmthAir.process (ch, nch, len, warmth->load(), air->load()); });
        }

        // 5. De-esser. Solo: output only what is being detected (the S/SH band while it is sibilance), and
        // skip everything after it so the removed part is heard as-is.
        const bool soloing = on (deessOn) && on (deessSolo);
        if (on (deessOn))
        {
            deEsser.setRange (vocalRange);
            deEsser.process (ch, nch, len, deessSens->load(), deessReduce->load(), soloing);
            float m = deEsser.takeMeter();
            stageAct[4] = juce::jmax (stageAct[4], dynAct (m));
            // Detection: how far the s/sh goes over the threshold, full scale at 12 dB.
            stageAct2[4] = juce::jmax (stageAct2[4], juce::jlimit (0.0f, 1.0f, deEsser.takeDetectMeter() / 12.0f));
        }

        if (! soloing)
        {
        // 6/7. Pultec
        auto runPultec = [&]
        {
            if (! on (pultecOn)) return;
            pultec.update ((int) pLowFreq->load(), pLowBoost->load(), pLowAtten->load(),
                           (int) pHighFreq->load(), pHighBoost->load(), pBandwidth->load(),
                           (int) pHighAttFreq->load(), pHighAtten->load());
            pultec.process (ch, nch, len);
            // Two bars: left = low boost, right = high boost
            float lowDb, highDb;
            pultec.takeMeters (lowDb, highDb, len * nch);
            stageAct2[5] = juce::jmax (stageAct2[5], changeAct (lowDb));
            stageAct[5]  = juce::jmax (stageAct[5],  changeAct (highDb));
        };

        // 6/7. Compressor (meter shows gain reduction, not makeup)
        auto runComp2 = [&]
        {
            if (! on (comp2On)) return;
            comp2.process (ch, nch, len, c2Thresh, c2Ratio, c2Makeup, true);
            stageAct[6] = juce::jmax (stageAct[6], dynAct (comp2.meter));
        };

        // Order: Pultec EQ then Compressor (default), or Compressor first when swapped
        if (on (swap56)) { runComp2(); runPultec(); }
        else             { runPultec(); runComp2(); }

        // 8. Tape saturation
        if (on (tapeOn))
            runStatic (7, ch, nch, len, [&] { tape.process (ch, nch, len, tapeAmount->load(), on (tapeOD)); });

        // 9. Delay
        if (on (delayOn))
        {
            delay.setVocalRange (vocalRange);   // echo mid dip follows the top-bar vocal range
            runStatic (8, ch, nch, len, [&] { delay.process (ch, nch, len, delaySec, delayMix->load() * 0.01f); });
        }

        // 10. Reverb (ducks under the vocal, swells in the gaps).
        // Meter: left bar = how much reverb is being added, right bar = how far it is ducked.
        if (on (reverbOn))
        {
            const float duckSoFar = stageAct[9];
            stageAct[9] = 0.0f;
            reverb.setVocalRange (vocalRange);   // mid dip follows the top-bar vocal range
            runStatic (9, ch, nch, len, [&] { reverb.process (ch, nch, len, revMix->load() * 0.01f); });
            stageAct2[9] = juce::jmax (stageAct2[9], stageAct[9]);   // reverb being added
            stageAct[9]  = juce::jmax (duckSoFar, juce::jlimit (0.0f, 1.0f, reverb.meter / vc::DuckingReverb::kDuckDb));
            reverb.meter = 0;
        }

        // 11. High / low pass
        if (on (filterOn))
            runStatic (10,ch, nch, len, [&] { passFilters.process (ch, nch, len, hpFreq->load(), lpFreq->load()); });

        // Auto gain: follow input and output loudness (~1 s average) and trim the
        // output so it matches the input, within +/-18 dB. Holds during silence.
        {
            const float outMs = meanSquare();
            const float c = (float) std::exp (-(double) len / (1.0 * currentRate));
            agIn  = c * agIn  + (1.0f - c) * inMs;
            agOut = c * agOut + (1.0f - c) * outMs;

            float target = agGainDb;
            if (! on (autoGain))
                target = 0.0f;
            else if (agIn > 1.0e-7f && agOut > 1.0e-9f)   // ignore near-silence
                target = juce::jlimit (-18.0f, 18.0f, 10.0f * std::log10 (agIn / agOut));

            const float gc = (float) std::exp (-(double) len / (0.3 * currentRate));
            const float newDb = gc * agGainDb + (1.0f - gc) * target;
            const float g0 = juce::Decibels::decibelsToGain (agGainDb), g1 = juce::Decibels::decibelsToGain (newDb);
            agGainDb = newDb;
            if (std::abs (g1 - 1.0f) > 1.0e-5f || std::abs (g0 - 1.0f) > 1.0e-5f)
                for (int cc = 0; cc < nch; ++cc)
                    for (int i = 0; i < len; ++i)
                        ch[cc][i] *= g0 + (g1 - g0) * (float) (i + 1) / (float) len;
        }
        } // ! soloing

        postTap.push (ch, nch, len);  // "after" for the analyzer
    }

    // Bypass just switched: crossfade processed <-> dry across this block.
    if (bypassFade)
    {
        for (int c = 0; c < nch; ++c)
        {
            float* out = buffer.getWritePointer (c);
            const float* dry = dryBuf.getReadPointer (c);
            for (int i = 0; i < n; ++i)
            {
                const float t = (float) (i + 1) / (float) n;
                const float wDry = bypassNow ? t : 1.0f - t;
                out[i] = out[i] * (1.0f - wDry) + dry[i] * wDry;
            }
        }
    }
    wasBypassed = bypassNow;

    // Publish to the display: keep the peak until the editor reads it.
    for (int s = 0; s < kNumStages; ++s)
    {
        float prev = activity[s].load();
        while (stageAct[s] > prev && ! activity[s].compare_exchange_weak (prev, stageAct[s])) {}
        float prev2 = activity2[s].load();
        while (stageAct2[s] > prev2 && ! activity2[s].compare_exchange_weak (prev2, stageAct2[s])) {}
    }
}

//==============================================================================
// Presets
juce::File VocalChainProcessor::presetFolder()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    dir = dir.getChildFile ("Application Support");
   #endif
    return dir.getChildFile ("fxnkycat vox").getChildFile ("Presets");
}

juce::File VocalChainProcessor::presetFile (int slot)
{
    return presetFolder().getChildFile ("Preset " + juce::String (slot + 1).paddedLeft ('0', 2) + ".xml");
}

juce::String VocalChainProcessor::presetName (int slot) const
{
    const auto f = presetFile (slot);
    if (! f.existsAsFile()) return {};
    if (auto xml = juce::XmlDocument::parse (f))
        return xml->getStringAttribute ("presetName", "Preset " + juce::String (slot + 1));
    return {};
}

bool VocalChainProcessor::savePreset (int slot, const juce::String& name)
{
    if (slot < 0 || slot >= kNumPresets) return false;
    auto xml = apvts.copyState().createXml();
    if (xml == nullptr) return false;
    xml->setAttribute ("presetName", name.trim().isEmpty() ? "Preset " + juce::String (slot + 1) : name.trim());
    presetFolder().createDirectory();
    return xml->writeTo (presetFile (slot));
}

bool VocalChainProcessor::loadPreset (int slot)
{
    if (slot < 0 || slot >= kNumPresets) return false;
    auto xml = juce::XmlDocument::parse (presetFile (slot));
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return false;
    xml->removeAttribute ("presetName");

    // Presets never change the bypass button: keep whatever it is set to now.
    const bool keepBypass = bypassParam != nullptr && bypassParam->get();
    apvts.replaceState (juce::ValueTree::fromXml (*xml));
    if (bypassParam != nullptr && bypassParam->get() != keepBypass)
        bypassParam->setValueNotifyingHost (keepBypass ? 1.0f : 0.0f);
    return true;
}

void VocalChainProcessor::resetToDefaults()
{
    for (auto* prm : getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
            if (rp != bypassParam)   // presets never touch bypass
            {
                rp->beginChangeGesture();
                rp->setValueNotifyingHost (rp->getDefaultValue());
                rp->endChangeGesture();
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
