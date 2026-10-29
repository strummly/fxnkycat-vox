#pragma once

// All of the signal processing for the 7 stages lives here.
// Everything is allocation-free on the audio thread (buffers are sized in prepare()).

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace vc
{
constexpr int    kMaxCh = 2;
constexpr double kPi    = 3.14159265358979323846;

inline float dbToGain (float db)  { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g)   { return 20.0f * std::log10 (std::max (g, 1.0e-9f)); }
inline float timeCoef (double seconds, double fs) { return (float) std::exp (-1.0 / (seconds * fs)); }

// One-pole smoother step: rise with 'att', fall with 'rel'.
inline float follow (float current, float target, float att, float rel)
{
    const float c = target > current ? att : rel;
    return c * current + (1.0f - c) * target;
}

// Soft-knee compressor curve. Returns gain reduction in dB (positive number).
inline float computeGR (float levelDb, float threshDb, float ratio, float kneeDb)
{
    const float over  = levelDb - threshDb;
    const float slope = 1.0f - 1.0f / ratio;

    if (kneeDb > 0.0f && 2.0f * std::abs (over) <= kneeDb)
    {
        const float t = over + kneeDb * 0.5f;
        return slope * t * t / (2.0f * kneeDb);
    }
    return over > 0.0f ? slope * over : 0.0f;
}

//==============================================================================
// RBJ biquad, transposed direct form II.
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1[kMaxCh] {}, z2[kMaxCh] {};

    void reset() { for (int c = 0; c < kMaxCh; ++c) z1[c] = z2[c] = 0.0f; }

    void set (double B0, double B1, double B2, double A0, double A1, double A2)
    {
        b0 = (float) (B0 / A0); b1 = (float) (B1 / A0); b2 = (float) (B2 / A0);
        a1 = (float) (A1 / A0); a2 = (float) (A2 / A0);
    }

    static double clampFreq (double f, double fs) { return std::clamp (f, 10.0, 0.45 * fs); }

    void makePeak (double fs, double f, double q, double db)
    {
        const double A = std::pow (10.0, db / 40.0), w = 2.0 * kPi * clampFreq (f, fs) / fs;
        const double cw = std::cos (w), alpha = std::sin (w) / (2.0 * q);
        set (1 + alpha * A, -2 * cw, 1 - alpha * A, 1 + alpha / A, -2 * cw, 1 - alpha / A);
    }

    void makeLowShelf (double fs, double f, double q, double db)
    {
        const double A = std::pow (10.0, db / 40.0), w = 2.0 * kPi * clampFreq (f, fs) / fs;
        const double cw = std::cos (w), alpha = std::sin (w) / (2.0 * q), s = 2.0 * std::sqrt (A) * alpha;
        set (A * ((A + 1) - (A - 1) * cw + s),
             2 * A * ((A - 1) - (A + 1) * cw),
             A * ((A + 1) - (A - 1) * cw - s),
             (A + 1) + (A - 1) * cw + s,
             -2 * ((A - 1) + (A + 1) * cw),
             (A + 1) + (A - 1) * cw - s);
    }

    void makeHighShelf (double fs, double f, double q, double db)
    {
        const double A = std::pow (10.0, db / 40.0), w = 2.0 * kPi * clampFreq (f, fs) / fs;
        const double cw = std::cos (w), alpha = std::sin (w) / (2.0 * q), s = 2.0 * std::sqrt (A) * alpha;
        set (A * ((A + 1) + (A - 1) * cw + s),
             -2 * A * ((A - 1) + (A + 1) * cw),
             A * ((A + 1) + (A - 1) * cw - s),
             (A + 1) - (A - 1) * cw + s,
             2 * ((A - 1) - (A + 1) * cw),
             (A + 1) - (A - 1) * cw - s);
    }

    void makeHighPass (double fs, double f, double q)
    {
        const double w = 2.0 * kPi * clampFreq (f, fs) / fs;
        const double cw = std::cos (w), alpha = std::sin (w) / (2.0 * q);
        set ((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
    }

    void makeLowPass (double fs, double f, double q)
    {
        const double w = 2.0 * kPi * clampFreq (f, fs) / fs;
        const double cw = std::cos (w), alpha = std::sin (w) / (2.0 * q);
        set ((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
    }

    inline float process (float x, int c)
    {
        const float y = b0 * x + z1[c];
        z1[c] = b1 * x - a1 * y + z2[c];
        z2[c] = b2 * x - a2 * y;
        return y;
    }
};

//==============================================================================
// State-variable filter (Cytomic/Simper). bandpass() has 0 dB gain at the centre,
// which lets us build a dynamic bell as:  y = x + (g - 1) * bandpass(x)
struct SVF
{
    float g = 0, k = 1, a1 = 0, a2 = 0, a3 = 0;
    float ic1[kMaxCh] {}, ic2[kMaxCh] {};

    void setup (double fs, double f, double q)
    {
        g  = (float) std::tan (kPi * std::min (f, 0.45 * fs) / fs);
        k  = (float) (1.0 / q);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    void reset() { for (int c = 0; c < kMaxCh; ++c) ic1[c] = ic2[c] = 0.0f; }

    inline float bandpass (float x, int c)
    {
        const float v3 = x - ic2[c];
        const float v1 = a1 * ic1[c] + a2 * v3;
        const float v2 = ic2[c] + a2 * ic1[c] + a3 * v3;
        ic1[c] = 2.0f * v1 - ic1[c];
        ic2[c] = 2.0f * v2 - ic2[c];
        return k * v1;
    }
};

//==============================================================================
// 1. Rider (vocal rider in the style of NoiseWorks GainAim)
// Two parts, both stereo-linked:
//   - Ride: a loudness average of the singing only (gaps and breaths are left out)
//     moves the gain so the voice sits at the Target. Its speed comes from the
//     speed mode. While the voice is under the noise floor, the ride holds still,
//     so it never jumps up at the start of the next phrase.
//   - Noise floor: a fast (20 ms) level check. Anything under the floor (breaths,
//     room noise, bleed) is pulled down by as much as it is under, up to 12 dB.
// Speed modes:
//   Natural  - follows phrases (Timing note: 1x average, 1/4x down, 1x up), +/-12 dB
//   Balanced - slow, evens out sections (4x average, 1x down, 4x up), +/-12 dB
//   Fast     - follows words (50 ms / 10 ms / 50 ms), +/-12 dB
struct VolumeRider
{
    static constexpr float kFloorDepthDb = 12.0f;

    float ms = 0, fastMs = 0, rideDb = 0, floorDb = 0;
    float meter = 0;       // largest total gain change (dB, either direction) since last read
    float overMeter = 0;   // most the level went over the noise floor (dB) since last read
    float rmsC = 0, downC = 0, upC = 0, maxRangeDb = 12.0f;
    float fastC = 0, floorDownC = 0, floorUpC = 0;
    double fs = 44100, lastBeat = -1;
    int    lastMode = -1;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        lastBeat = -1; lastMode = -1;
        setTiming (0.5, 0);
        fastC      = timeCoef (0.020, fs);   // noise-floor detector: 20 ms
        floorDownC = timeCoef (0.015, fs);   // duck noise over 15 ms
        floorUpC   = timeCoef (0.005, fs);   // let the voice back in over 5 ms
        ms = fastMs = 0; rideDb = floorDb = 0;
    }

    // 'beat' = the plugin's Timing note in seconds (Rider and Leveler follow it).
    void setTiming (double beat, int mode)
    {
        if (beat == lastBeat && mode == lastMode) return;
        lastBeat = beat; lastMode = mode;

        double avg, down, up;
        switch (mode)
        {
            case 1:  avg = beat * 4.0; down = beat;        up = beat * 4.0; maxRangeDb = 12.0f; break;  // Balanced
            case 2:  avg = 0.050;      down = 0.010;       up = 0.050;      maxRangeDb = 12.0f; break;  // Fast
            default: avg = beat;       down = beat * 0.25; up = beat;       maxRangeDb = 12.0f; break;  // Natural
        }
        rmsC  = timeCoef (avg,  fs);
        downC = timeCoef (down, fs);
        upC   = timeCoef (up,   fs);
    }

    void process (float* const* ch, int nch, int n, float floorThreshDb, float targetDb)
    {
        for (int i = 0; i < n; ++i)
        {
            float p = 0;
            for (int c = 0; c < nch; ++c) p += ch[c][i] * ch[c][i];
            p /= (float) nch;

            fastMs = fastC * fastMs + (1.0f - fastC) * p;
            const float fastDb = 10.0f * std::log10 (fastMs + 1.0e-12f);
            const bool  voice  = fastDb > floorThreshDb;

            // Ride: only listens to (and moves during) the singing.
            if (voice)
            {
                overMeter = std::max (overMeter, fastDb - floorThreshDb);
                ms = ms <= 0.0f ? fastMs : rmsC * ms + (1.0f - rmsC) * p;   // start from the first word's level
                const float levelDb = 10.0f * std::log10 (ms + 1.0e-12f);
                const float want = std::clamp (targetDb - levelDb, -maxRangeDb, maxRangeDb);
                const float c = want < rideDb ? downC : upC;
                rideDb = c * rideDb + (1.0f - c) * want;
            }

            // Noise floor: pull down by how far under the floor, up to 12 dB.
            const float floorWant = voice ? 0.0f : std::max (-kFloorDepthDb, fastDb - floorThreshDb);
            const float fc = floorWant < floorDb ? floorDownC : floorUpC;
            floorDb = fc * floorDb + (1.0f - fc) * floorWant;

            const float totalDb = rideDb + floorDb;
            meter = std::max (meter, std::abs (totalDb));
            const float g = dbToGain (totalDb);
            for (int ch_ = 0; ch_ < nch; ++ch_) ch[ch_][i] *= g;
        }
    }
};

//==============================================================================
// 2 & 6. Compressor (stereo-linked, peak detection, soft knee).
// With autoRelease on, a second slow envelope builds up during sustained
// compression, so release is fast after short peaks and slow after long phrases.
struct Compressor
{
    float attC = 0, relC = 0, slowAttC = 0, slowRelC = 0;
    float grFast = 0, grSlow = 0;
    float kneeDb = 6.0f;
    float meter = 0;   // most gain reduction (dB) since last read
    double fs = 44100, lastRelease = -1, lastSlowAtt = -1, lastSlowRel = -1;

    void prepare (double sampleRate, float attackMs, float releaseMs, float knee)
    {
        fs       = sampleRate;
        attC     = timeCoef (attackMs * 0.001, fs);
        slowAttC = timeCoef (0.25, fs);
        slowRelC = timeCoef (1.20, fs);
        kneeDb   = knee;
        grFast = grSlow = 0;
        lastRelease = lastSlowAtt = lastSlowRel = -1;
        setRelease (releaseMs * 0.001);
    }

    // Can be called every block (e.g. tempo-synced); only recalculates on change.
    void setRelease (double seconds)
    {
        if (seconds == lastRelease) return;
        lastRelease = seconds;
        relC = timeCoef (seconds, fs);
    }

    // Slow stage of the auto release, used when autoRelease is on.
    void setSlowTimes (double attackSeconds, double releaseSeconds)
    {
        if (attackSeconds == lastSlowAtt && releaseSeconds == lastSlowRel) return;
        lastSlowAtt = attackSeconds; lastSlowRel = releaseSeconds;
        slowAttC = timeCoef (attackSeconds, fs);
        slowRelC = timeCoef (releaseSeconds, fs);
    }

    void process (float* const* ch, int nch, int n, float threshDb, float ratio,
                  float makeupDb, bool autoRelease)
    {
        const float makeup = dbToGain (makeupDb);

        for (int i = 0; i < n; ++i)
        {
            float peak = 0;
            for (int c = 0; c < nch; ++c) peak = std::max (peak, std::abs (ch[c][i]));

            const float target = computeGR (gainToDb (peak), threshDb, ratio, kneeDb);
            grFast = follow (grFast, target, attC, relC);

            float gr = grFast;
            if (autoRelease)
            {
                grSlow = follow (grSlow, target, slowAttC, slowRelC);
                gr = std::max (grFast, grSlow);
            }

            meter = std::max (meter, gr);
            const float g = dbToGain (-gr) * makeup;
            for (int c = 0; c < nch; ++c) ch[c][i] *= g;
        }
    }
};

//==============================================================================
// One band of dynamic EQ: only cuts its frequency range while that range is
// louder than the threshold. Used by the problem-frequency EQ and the de-esser.
struct DynamicBand
{
    SVF   svf;
    float env = 0, attC = 0, relC = 0;
    float meter = 0;   // deepest cut (dB) since last read

    void prepare (double fs, double freq, double q, float attackMs, float releaseMs)
    {
        svf.setup (fs, freq, q);
        svf.reset();
        attC = timeCoef (attackMs * 0.001, fs);
        relC = timeCoef (releaseMs * 0.001, fs);
        env = 0;
    }

    void setFreq (double fs, double freq, double q) { svf.setup (fs, freq, q); }

    // gate (optional, one value per sample, 0-1): scales the cut, e.g. "how sure are we
    // this is sibilance". Null = always 1.
    void process (float* const* ch, int nch, int n, float threshDb, float ratio, float maxCutDb,
                  const float* gate = nullptr)
    {
        for (int i = 0; i < n; ++i)
        {
            float bp[kMaxCh] {};
            float peak = 0;
            for (int c = 0; c < nch; ++c)
            {
                bp[c] = svf.bandpass (ch[c][i], c);
                peak  = std::max (peak, std::abs (bp[c]));
            }

            env = follow (env, peak, attC, relC);
            const float gt = gate != nullptr ? gate[i] : 1.0f;
            const float cutDb = std::min (maxCutDb, computeGR (gainToDb (env), threshDb, ratio, 4.0f)) * gt;
            meter = std::max (meter, cutDb);
            const float gm1   = dbToGain (-cutDb) - 1.0f;

            for (int c = 0; c < nch; ++c) ch[c][i] += gm1 * bp[c];
        }
    }
};

//==============================================================================
// Sibilance detector, shared by the comp-gate's de-esser (stage 2) and the
// de-esser (stage 4). Says how sure it is (0-1) that the sound right now is an
// S, SH or CH. All three must hold:
//   - the top end (from a little under the S/SH range) outweighs the body of the
//     voice (below 1 kHz): sibilants are mostly hiss, vowels mostly lows/mids,
//   - it is noise-like: a high zero-crossing rate. Pitched sounds (vowels, bright
//     "ee"s, high notes, hums) cross far less often,
//   - its energy sits in the S/SH range, not in the air above it (cymbals).
// It has to hold for a few ms (4 ms on, 30 ms off), so single "t"/"k" clicks
// barely count. The S/SH range follows the vocal range.
struct SibilanceDetector
{
    // S/SH range for each vocal range: bass/baritone, tenor/alto, mezzo/soprano.
    static constexpr double kLoHz[3] = { 2500.0, 3000.0, 3500.0 };
    static constexpr double kHiHz[3] = { 8500.0, 10000.0, 11000.0 };

    Biquad hfA, hfB, lfA, lfB, dc, airA, airB;   // mono, slot 0
    double sr = 44100;
    float hfMs = 0, lfMs = 0, airMs = 0, envC = 0, zcRate = 0, zcC = 0, prevX = 0, sib = 0, sibAtt = 0, sibRel = 0;
    float lastRatioDb = 0, lastAirDb = 0, lastZc = 0;   // latest measurements (testing)

    void prepare (double fs, int range)
    {
        sr = fs;
        range = std::clamp (range, 0, 2);
        const double hpHz = kLoHz[range] * 0.6;   // well under the S/SH range, so all of an "sh" is included
        for (auto* b : { &hfA, &hfB }) { b->makeHighPass (fs, hpHz, 0.707);  b->reset(); }
        for (auto* b : { &lfA, &lfB }) { b->makeLowPass  (fs, 1000.0, 0.707); b->reset(); }
        dc.makeHighPass (fs, 80.0, 0.707); dc.reset();
        const double airHz = std::min (kHiHz[range] * 1.1, fs * 0.45);
        for (auto* b : { &airA, &airB }) { b->makeHighPass (fs, airHz, 0.707); b->reset(); }

        envC   = timeCoef (0.004, fs);   // 4 ms level averages
        zcC    = timeCoef (0.008, fs);   // 8 ms zero-crossing average
        sibAtt = timeCoef (0.004, fs);   // on over 4 ms
        sibRel = timeCoef (0.030, fs);   // off over 30 ms
        hfMs = lfMs = airMs = zcRate = prevX = sib = 0;
    }

    // One mono sample in, sibilance confidence (0-1) out.
    float process (float mono)
    {
        // Top end vs body, and S/SH range vs air
        const float h   = hfB.process (hfA.process (mono, 0), 0);
        const float l   = lfB.process (lfA.process (mono, 0), 0);
        const float air = airB.process (airA.process (mono, 0), 0);
        hfMs  = envC * hfMs  + (1.0f - envC) * h * h;
        lfMs  = envC * lfMs  + (1.0f - envC) * l * l;
        airMs = envC * airMs + (1.0f - envC) * air * air;

        // Noise-likeness: zero crossings per second (rumble removed first)
        const float x = dc.process (mono, 0);
        const float crossed = ((x >= 0.0f) != (prevX >= 0.0f)) ? 1.0f : 0.0f;
        prevX = x;
        zcRate = zcC * zcRate + (1.0f - zcC) * crossed * (float) sr;

        // hfMs includes the air band; take it out to get the S/SH range on its own.
        const float sibBandMs = std::max (0.0f, hfMs - airMs);
        const float ratioDb = 10.0f * std::log10 ((hfMs + 1.0e-12f) / (lfMs + 1.0e-12f));
        const float airDb   = 10.0f * std::log10 ((sibBandMs + 1.0e-12f) / (airMs + 1.0e-12f));
        // Tuned on test sounds: vowels/"ee"/high notes cross <= ~3.5k times/s and sibilants >= ~6k,
        // breaths fail the top-end test and cymbals the S/SH-vs-air test.
        const float a = std::clamp ((ratioDb + 3.0f) / 8.0f, 0.0f, 1.0f);       // top end over body: -3 dB -> 0, +5 dB -> 1
        const float b = std::clamp ((zcRate - 4000.0f) / 3000.0f, 0.0f, 1.0f); // noise-like: 4k -> 0, 7k crossings/s -> 1
        const float d = std::clamp ((airDb - 3.0f) / 6.0f, 0.0f, 1.0f);         // S/SH range over air: +3 -> 0, +9 dB -> 1
        const float audible = hfMs > 1.0e-8f ? 1.0f : 0.0f;                     // ignore below ~-80 dB
        lastRatioDb = ratioDb; lastAirDb = airDb; lastZc = zcRate;
        sib = follow (sib, a * b * d * audible, sibAtt, sibRel);
        return sib;
    }
};

//==============================================================================
// 2. Comp-gate in a classic "de-ess + gate" setup (modelled on how a C1-style
// comp/gate is set up for vocals):
//   - Gate first: wideband, fast open, smooth close to a -30 dB floor, 4 dB hysteresis.
//     It needs no threshold of its own: it opens at stage 1's Noise Floor when the
//     Volume Rider is on, and otherwise sets itself 30 dB under the singing's own
//     long-term level. Hold and close follow the Timing note (1/8 and 0.3 x).
//   - Then split-band de-ess compression: the sidechain listens to a band around
//     5 / 6 / 7 kHz (by vocal range) and only that high band is turned down (4:1, near-hard
//     knee, 0.5 ms / 50 ms) - and only while the shared sibilance detector (same as
//     stage 5) hears an S, SH or CH, so bright vowels and cymbals are left alone.
// One control: Threshold, which sets the de-ess only.
struct CompGate
{
    static constexpr float kAutoGateDb = -30.0f, kGateHystDb = 4.0f, kGateFloorDb = -30.0f;
    float  longMs = 0, longC = 0, learnUpC = 0;   // singing's long-term level, for the automatic gate
    double lastUnit = -1;
    static constexpr float kDeessRatio = 4.0f, kDeessMaxCutDb = 18.0f;

    DynamicBand deess;
    float gateEnv = 0, gateGainDb = 0;
    float envAtt = 0, envRel = 0, openC = 0, closeC = 0;
    int   holdSamples = 0, holdLeft = 0;
    bool  open = false;
    float meter = 0;       // most de-ess cut (dB) since last read
    float gateMeter = 0;   // most gate reduction (dB) since last read

    // Sidechain centre for each vocal range: bass/baritone, tenor/alto, mezzo/soprano.
    static constexpr double kDeessHz[3] = { 5000.0, 6000.0, 7000.0 };
    double sr = 44100;
    int    range = 1;

    // The de-ess only cuts while the shared detector hears an S / SH / CH.
    static constexpr int kBlock = 512;
    SibilanceDetector det;
    std::array<float, kBlock> sibGate {};

    void setRange (int r)
    {
        r = std::clamp (r, 0, 2);
        if (r == range) return;
        range = r;
        deess.setFreq (sr, kDeessHz[range], 0.9);
        det.prepare (sr, range);
    }

    void prepare (double fs)
    {
        sr = fs;
        deess.prepare (fs, kDeessHz[range], 0.9, 0.5f, 50.0f);
        det.prepare (fs, range);
        envAtt = timeCoef (0.0001, fs);   // 0.1 ms detector
        envRel = timeCoef (0.030, fs);
        openC  = timeCoef (0.0005, fs);   // opens in ~0.5 ms
        longC  = timeCoef (3.0, fs);
        learnUpC = timeCoef (0.2, fs);
        lastUnit = -1; setTiming (0.5);
        holdLeft = 0;
        gateEnv = longMs = 0; gateGainDb = kGateFloorDb; open = false;
    }

    // unit = the Timing note in seconds: hold 1/8 of it, close over 0.3 x
    // (50 ms / 150 ms at 120 BPM with the default 1/4 note).
    void setTiming (double unit)
    {
        if (unit == lastUnit) return;
        lastUnit = unit;
        holdSamples = std::max (1, (int) (unit * 0.125 * sr));
        closeC = timeCoef (std::max (0.02, unit * 0.3), sr);
    }

    // deessThreshDb: the stage 2 Threshold (de-ess only).
    // useNoiseFloor: true when the Volume Rider is on; the gate then opens at its
    // Noise Floor (an RMS level, so +3 dB for the gate's peak detector).
    void process (float* const* ch, int nch, int n, float deessThreshDb, bool useNoiseFloor, float noiseFloorDb)
    {
        const float threshDb = deessThreshDb;

        for (int start = 0; start < n; start += kBlock)
        {
            const int len = std::min (kBlock, n - start);
            float* sub[kMaxCh] {};
            for (int c = 0; c < nch; ++c) sub[c] = ch[c] + start;

            // Gate
            for (int i = 0; i < len; ++i)
            {
                float peak = 0;
                for (int c = 0; c < nch; ++c) peak = std::max (peak, std::abs (sub[c][i]));
                gateEnv = follow (gateEnv, peak, envAtt, envRel);
                const float lvl = gainToDb (gateEnv);

                // Where the gate opens: the rider's noise floor, or 30 dB under the singing.
                float openAt;
                if (useNoiseFloor)
                    openAt = noiseFloorDb + 3.0f;
                else
                {
                    const float longDb = 10.0f * std::log10 (longMs + 1.0e-12f);
                    if (lvl > -60.0f && (longMs <= 0.0f || lvl > longDb - 20.0f))   // learn from the singing only
                    {
                        // Catch up quickly when the singing is louder (0.2 s), forget slowly (3 s).
                        const float e2 = gateEnv * gateEnv;
                        longMs = e2 > longMs ? learnUpC * longMs + (1.0f - learnUpC) * e2
                                             : longC * longMs + (1.0f - longC) * e2;
                    }
                    openAt = longMs > 0.0f ? 10.0f * std::log10 (longMs) + kAutoGateDb : -60.0f;
                }
                const float closeAt = openAt - kGateHystDb;

                if (lvl > openAt)            { open = true; holdLeft = holdSamples; }
                else if (lvl < closeAt)      { if (holdLeft > 0) --holdLeft; else open = false; }

                const float target = open ? 0.0f : kGateFloorDb;
                const float c = target > gateGainDb ? openC : closeC;
                gateGainDb = c * gateGainDb + (1.0f - c) * target;

                gateMeter = std::max (gateMeter, -gateGainDb);
                const float g = dbToGain (gateGainDb);
                float mono = 0;
                for (int c2 = 0; c2 < nch; ++c2) { sub[c2][i] *= g; mono += sub[c2][i]; }

                // Is this an S / SH / CH? (on the gated signal the de-ess works on)
                sibGate[(size_t) i] = det.process (mono / (float) nch);
            }

            // Split-band de-ess compression, only while sibilance is detected
            deess.process (sub, nch, len, threshDb, kDeessRatio, kDeessMaxCutDb, sibGate.data());
        }
        meter = std::max (meter, deess.meter);
        deess.meter = 0;
    }
};

//==============================================================================
// Resonance tracker: searches a frequency range for the spot that sticks out
// above its neighbours, glides a narrow cut onto it, and cuts only while that
// spot is both louder than the threshold and actually standing out.
struct ResonanceTracker
{
    static constexpr int kN   = 12;  // search points across the range
    static constexpr int kHop = 32;  // re-analyse every 32 samples

    SVF   det[kN];
    float detEnv[kN] {}, candLogF[kN] {};
    float detC = 0;

    SVF   cut;
    float cutEnv = 0, cutAtt = 0, cutRel = 0;
    float curLogF = 0, targetLogF = 0, glide = 0;
    float prom = 0, promTarget = 0;
    float meter = 0;      // deepest cut (dB) since last read
    float detMeter = 0;   // most the tracked spot went over the threshold (dB) since last read
    double fs = 44100, cutQ = 4.0;
    int hop = 0;
    bool strongestMode = false;   // true: follow the loudest point, not the one that sticks out

    // Optional retune after prepare(): used by the de-esser to chase the loudest
    // sibilant frequency quickly.
    void setStyle (bool strongest, double detSeconds, double glideSeconds, float attackMs, float releaseMs)
    {
        strongestMode = strongest;
        detC   = timeCoef (detSeconds, fs);
        glide  = 1.0f - (float) std::exp (-(double) kHop / (glideSeconds * fs));
        cutAtt = timeCoef (attackMs * 0.001, fs);
        cutRel = timeCoef (releaseMs * 0.001, fs);
    }

    void prepare (double sampleRate, double lo, double hi, double q)
    {
        fs = sampleRate;
        cutQ = q;
        detC   = timeCoef (0.03, fs);                 // 30 ms detector smoothing
        cutAtt = timeCoef (0.005, fs);
        cutRel = timeCoef (0.08, fs);
        glide  = 1.0f - (float) std::exp (-(double) kHop / (0.04 * fs));   // ~40 ms glide
        cut.reset();
        for (auto& d : det) d.reset();
        std::fill (detEnv, detEnv + kN, 0.0f);
        cutEnv = prom = promTarget = 0;
        hop = 0;
        strongestMode = false;
        setRange (lo, hi);
        curLogF = targetLogF = (float) std::log (std::sqrt (lo * hi));
        cut.setup (fs, std::exp ((double) curLogF), cutQ);
    }

    void setRange (double lo, double hi)
    {
        for (int k = 0; k < kN; ++k)
        {
            const double f = lo * std::pow (hi / lo, (double) k / (kN - 1));
            det[k].setup (fs, f, 4.5);
            candLogF[k] = (float) std::log (f);
        }
        targetLogF = (float) std::log (std::sqrt (lo * hi));
    }

    void analyse()
    {
        float lvl[kN];
        for (int k = 0; k < kN; ++k) lvl[k] = 10.0f * std::log10 (detEnv[k] + 1.0e-12f);

        int   best = -1;
        float bestProm = 0.0f;

        if (strongestMode)
        {
            // Loudest point in the range wins; cut is never scaled down by "stand-out".
            // A small upward tilt (~3 dB/oct) keeps vowel presence near the bottom of
            // the range from beating real sibilance.
            best = 0;
            for (int k = 1; k < kN; ++k)
                if (lvl[k] + 0.5f * (float) k > lvl[best] + 0.5f * (float) best) best = k;
            targetLogF = candLogF[best];
            promTarget = 6.0f;
            curLogF += glide * (targetLogF - curLogF);
            prom = promTarget;
            cut.setup (fs, std::exp ((double) curLogF), cutQ);
            return;
        }

        for (int k = 0; k < kN; ++k)
        {
            // Compare with neighbours 2-3 steps away on each side (a local peak test).
            float sum = 0; int cnt = 0;
            for (int d = 2; d <= 3; ++d)
            {
                if (k - d >= 0)  { sum += lvl[k - d]; ++cnt; }
                if (k + d < kN)  { sum += lvl[k + d]; ++cnt; }
            }
            if (cnt == 0) continue;
            const float p = lvl[k] - sum / (float) cnt;
            if (p > bestProm) { bestProm = p; best = k; }
        }

        if (best >= 0) targetLogF = candLogF[best];
        promTarget = bestProm;

        curLogF += glide * (targetLogF - curLogF);
        prom    += glide * (promTarget - prom);
        cut.setup (fs, std::exp ((double) curLogF), cutQ);
    }

    // gate (optional, one value per sample, 0-1): scales the cut, e.g. "how sure are we
    // this is sibilance". Null = always 1.
    void process (float* const* ch, int nch, int n, float threshDb, float ratio, float maxCutDb,
                  const float* gate = nullptr)
    {
        for (int i = 0; i < n; ++i)
        {
            float mono = 0;
            for (int c = 0; c < nch; ++c) mono += ch[c][i];
            mono /= (float) nch;
            const float gt = gate != nullptr ? gate[i] : 1.0f;

            for (int k = 0; k < kN; ++k)
            {
                const float b = det[k].bandpass (mono, 0);
                detEnv[k] = detC * detEnv[k] + (1.0f - detC) * b * b;
            }
            if (++hop >= kHop) { hop = 0; analyse(); }

            float bp[kMaxCh] {};
            float peak = 0;
            for (int c = 0; c < nch; ++c)
            {
                bp[c] = cut.bandpass (ch[c][i], c);
                peak  = std::max (peak, std::abs (bp[c]));
            }
            cutEnv = follow (cutEnv, peak, cutAtt, cutRel);

            // Full cut once the spot stands 6 dB above its neighbours; none if it doesn't stand out.
            const float standOut = std::clamp (prom / 6.0f, 0.0f, 1.0f);
            const float cutDb = std::min (maxCutDb, computeGR (gainToDb (cutEnv), threshDb, ratio, 4.0f)) * standOut * gt;
            meter = std::max (meter, cutDb);
            detMeter = std::max (detMeter, std::max (0.0f, gainToDb (cutEnv) - threshDb) * standOut * gt);
            const float gm1 = dbToGain (-cutDb) - 1.0f;

            for (int c = 0; c < nch; ++c) ch[c][i] += gm1 * bp[c];
        }
    }
};

//==============================================================================
// 3. Problem EQ: three resonance trackers, each searching its own range
// (low mud, mid boxy/nasal ring, upper harshness). The vocal range switch moves
// the search ranges to suit the singer. The threshold slider is shared; the upper
// bands get an offset because vocals naturally carry less energy up there.
struct ProblemEQ
{
    // Search ranges (Hz) per vocal range: { mud lo/hi, mid lo/hi, high lo/hi }
    struct Ranges { double lo[3], hi[3]; };
    static constexpr int kNumRanges = 3;   // bass/baritone, tenor/alto, mezzo/soprano
    static constexpr Ranges kRanges[kNumRanges] = {
        { { 120.0, 350.0, 1800.0 }, { 400.0, 2000.0, 5000.0 } },   // bass / baritone
        { { 170.0, 450.0, 2200.0 }, { 500.0, 2500.0, 6000.0 } },   // tenor / alto
        { { 220.0, 550.0, 2700.0 }, { 650.0, 3000.0, 7000.0 } },   // mezzo / soprano
    };
    static constexpr double kQ[3]        = { 3.0, 4.5, 4.0 };
    static constexpr float  kOffsetDb[3] = { 0.0f, -6.0f, -10.0f };
    static constexpr float  kRatio = 3.0f, kMaxCutDb = 8.0f;

    ResonanceTracker trackers[3];
    int range = -1;

    void prepare (double fs)
    {
        range = 1;   // tenor / alto until told otherwise
        for (int b = 0; b < 3; ++b)
            trackers[b].prepare (fs, kRanges[range].lo[b], kRanges[range].hi[b], kQ[b]);
    }

    void process (float* const* ch, int nch, int n, float threshDb, int vocalRange)
    {
        const int r = std::clamp (vocalRange, 0, kNumRanges - 1);
        if (r != range)
        {
            range = r;
            for (int b = 0; b < 3; ++b)
                trackers[b].setRange (kRanges[r].lo[b], kRanges[r].hi[b]);
        }
        for (int b = 0; b < 3; ++b)
            trackers[b].process (ch, nch, n, threshDb + kOffsetDb[b], kRatio, kMaxCutDb);
    }

    // Deepest cut of any band since last call, then resets.
    float takeMeter()
    {
        float m = 0;
        for (auto& t : trackers) { m = std::max (m, t.meter); t.meter = 0; }
        return m;
    }
};

//==============================================================================
// 4. De-esser that finds the sibilance itself.
// Scans the S/SH range (set by the vocal range; "sh" sits low in it, "s" higher),
// follows whichever frequency is loudest at that moment, and cuts a band there when
// it crosses the detection threshold - but only while the sound is classified as
// sibilance (S, SH, CH). The classifier needs both:
//   - the top end (above the S/SH range's low edge) to outweigh the body of the voice
//     (below 1.5 kHz): sibilants are mostly hiss, vowels are mostly lows/mids, and
//   - a noise-like signal: a high zero-crossing rate (above ~1.8-3.5k crossings/s).
//     Pitched sounds (vowels, bright "ee"s, hums) cross far less often.
// It has to hold for a few ms (3 ms attack), so single clicks like "t"/"k" barely count.
// Solo (listen) plays what is being detected: the S/SH band, only while it is
// classified as sibilance and over the detection threshold.
struct DeEsser
{
    static constexpr int kBlock = 512;

    ResonanceTracker tracker;

    // S/SH search range for each vocal range (shared with the detector).
    static constexpr const double* kLoHz = SibilanceDetector::kLoHz;
    static constexpr const double* kHiHz = SibilanceDetector::kHiHz;
    double sr = 44100;
    int    range = 1;

    SibilanceDetector det;   // is this an S / SH / CH? (shared with stage 2)
    Biquad hfA, hfB;         // S/SH band per channel (24 dB/oct high-pass), for Solo
    std::array<float, kBlock> gate {};
    std::array<std::array<float, kBlock>, kMaxCh> listen {};

    void prepare (double fs)
    {
        sr = fs;
        // Keep the top of the search safely under Nyquist at low sample rates.
        tracker.prepare (fs, kLoHz[range], std::min (kHiHz[range], fs * 0.45), 1.6);
        tracker.setStyle (true, 0.004, 0.005, 0.3f, 60.0f);  // 4 ms detect, 5 ms glide, 0.3/60 ms cut

        det.prepare (fs, range);
        const double hpHz = SibilanceDetector::kLoHz[range] * 0.6;   // same S/SH band the detector listens to
        for (auto* b : { &hfA, &hfB }) { b->makeHighPass (fs, hpHz, 0.707); b->reset(); }
    }

    void setRange (int r)
    {
        r = std::clamp (r, 0, 2);
        if (r == range) return;
        range = r;
        prepare (sr);   // rebuilds the search bands (only when the switch moves)
    }

    // sensitivity: 0-100 %, reductionDb: max cut in dB.
    // listenOnly: replace the output with what is being detected (Solo).
    void process (float* const* ch, int nch, int n, float sensitivity, float reductionDb, bool listenOnly = false)
    {
        const float threshDb = -6.0f - 0.44f * sensitivity;  // 0 % = -6 dB, 100 % = -50 dB

        for (int start = 0; start < n; start += kBlock)
        {
            const int len = std::min (kBlock, n - start);
            float* sub[kMaxCh] {};
            for (int c = 0; c < nch; ++c) sub[c] = ch[c] + start;

            for (int i = 0; i < len; ++i)
            {
                float mono = 0;
                for (int c = 0; c < nch; ++c) mono += sub[c][i];
                mono /= (float) nch;

                const float sib = det.process (mono);
                gate[(size_t) i] = sib;

                if (listenOnly)
                {
                    // What is being detected: the S/SH band, while it is sibilance and over the threshold.
                    const float hfDb = 10.0f * std::log10 (det.hfMs + 1.0e-12f) + 3.0f;   // RMS -> rough peak
                    const float over = std::clamp ((hfDb - threshDb + 3.0f) / 6.0f, 0.0f, 1.0f);
                    for (int c = 0; c < nch; ++c)
                        listen[(size_t) c][(size_t) i] = hfB.process (hfA.process (sub[c][i], c), c) * sib * over;
                }
            }

            tracker.process (sub, nch, len, threshDb, 8.0f, reductionDb, gate.data());

            if (listenOnly)
                for (int c = 0; c < nch; ++c)
                    std::copy (listen[(size_t) c].begin(), listen[(size_t) c].begin() + len, sub[c]);
        }
    }

    float takeMeter() { const float m = tracker.meter; tracker.meter = 0; return m; }
    float takeDetectMeter() { const float m = tracker.detMeter; tracker.detMeter = 0; return m; }
};

//==============================================================================
// 5. Pultec EQP-1A style EQ.
// Knob values are 0-10 like the hardware. The low attenuation shelf sits above the
// boost shelf, so boosting and cutting together gives the classic low-end trick
// (more sub, less mud just above it).
struct PultecEQ
{
    static constexpr float kLowFreqs[]     = { 20.0f, 30.0f, 60.0f, 100.0f };
    static constexpr float kHighFreqs[]    = { 3000.0f, 4000.0f, 5000.0f, 8000.0f, 10000.0f, 12000.0f, 16000.0f };
    static constexpr float kHighAttFreqs[] = { 5000.0f, 10000.0f, 20000.0f };

    Biquad lowBoost, lowAtten, highBoost, highAtten;
    double fs = 44100;
    float last[8] {};
    bool  dirty = true;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        lowBoost.reset(); lowAtten.reset(); highBoost.reset(); highAtten.reset();
        dirty = true;
    }

    void update (int lowFreqIdx, float lowBoostAmt, float lowAttenAmt,
                 int highFreqIdx, float highBoostAmt, float bandwidth,
                 int highAttFreqIdx, float highAttenAmt)
    {
        const float now[8] = { (float) lowFreqIdx, lowBoostAmt, lowAttenAmt, (float) highFreqIdx,
                               highBoostAmt, bandwidth, (float) highAttFreqIdx, highAttenAmt };
        if (! dirty && std::equal (now, now + 8, last)) return;
        std::copy (now, now + 8, last);
        dirty = false;

        const double lf = kLowFreqs[std::clamp (lowFreqIdx, 0, 3)];
        const double hf = kHighFreqs[std::clamp (highFreqIdx, 0, 6)];
        const double af = kHighAttFreqs[std::clamp (highAttFreqIdx, 0, 2)];

        lowBoost.makeLowShelf (fs, lf, 0.8, lowBoostAmt * 1.35);          // up to +13.5 dB
        lowAtten.makeLowShelf (fs, lf * 2.2, 0.5, -lowAttenAmt * 1.0);     // up to -10 dB

        // Bandwidth: 0 = sharp (Q 2.5), 10 = broad (Q 0.4)
        const double q = 2.5 * std::pow (0.4 / 2.5, bandwidth / 10.0);
        highBoost.makePeak (fs, hf, q, highBoostAmt * 1.6);                // up to +16 dB
        highAtten.makeHighShelf (fs, af, 0.6, -highAttenAmt * 1.6);        // up to -16 dB
    }

    // Energy of the input, and of what the low and high boost sections each add,
    // summed since the last takeMeters() (for the stage's two meter bars).
    double eIn = 0, eLowAdd = 0, eHighAdd = 0;

    void process (float* const* ch, int nch, int n)
    {
        for (int c = 0; c < nch; ++c)
        {
            float* d = ch[c];
            for (int i = 0; i < n; ++i)
            {
                float x = d[i];
                eIn += (double) x * x;
                const float lb = lowBoost.process (x, c);
                eLowAdd += (double) (lb - x) * (lb - x);
                x = lowAtten.process (lb, c);
                const float hb = highBoost.process (x, c);
                eHighAdd += (double) (hb - x) * (hb - x);
                x = highAtten.process (hb, c);
                d[i] = x;
            }
        }
    }

    // How much each boost changes the sound, in dB relative to the input
    // (-inf if silent). Resets the sums.
    void takeMeters (float& lowDb, float& highDb, int samples)
    {
        if (eIn / std::max (1, samples) < 1.0e-7) { lowDb = highDb = -200.0f; }   // near-silence
        else
        {
            lowDb  = 10.0f * std::log10 ((float) (eLowAdd  / eIn) + 1.0e-12f);
            highDb = 10.0f * std::log10 ((float) (eHighAdd / eIn) + 1.0e-12f);
        }
        eIn = eLowAdd = eHighAdd = 0;
    }
};

//==============================================================================
// Tape saturation (between the compressor and the delay).
// Drive pushes the signal into an asymmetric soft clipper (tape-style even +
// odd harmonics), with a gentle low "head bump" and high-frequency roll-off that
// both grow with the amount. Output level is compensated so louder != better.
struct TapeSaturation
{
    Biquad headBump, hfLoss;
    double fs = 44100;
    float  lastAmount = -1;
    float  dcX[kMaxCh] {}, dcY[kMaxCh] {}, dcR = 0.995f;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        headBump.reset(); hfLoss.reset();
        lastAmount = -1; lastWet = -1;
        dcR = 1.0f - (float) (2.0 * kPi * 10.0 / fs);   // 10 Hz DC blocker
        for (int c = 0; c < kMaxCh; ++c) dcX[c] = dcY[c] = 0;
    }

    static inline float shape (float x)
    {
        // Asymmetric soft clip: slightly different curve for positive/negative.
        return x >= 0.0f ? std::tanh (x) : std::tanh (x * 0.9f) / 0.9f * 0.95f;
    }

    // amount: 0-100 %.
    // Normal: amount sets how hard the tape is driven.
    // Overdrive: the tape is driven flat out (full saturation + 18 dB more drive, full
    // head bump and roll-off) and amount becomes a dry/wet mix of that driven sound.
    void process (float* const* ch, int nch, int n, float amount, bool overdrive)
    {
        const float knob = std::clamp (amount * 0.01f, 0.0f, 1.0f);
        if (knob <= 0.0f) { lastWet = 0.0f; return; }   // nothing to add (dry only)

        const float a      = overdrive ? 1.0f : knob;      // drive amount
        const float wetMix = overdrive ? knob : 1.0f;
        const float colour = a;                            // tone shaping amount
        const float key = colour;
        if (key != lastAmount)
        {
            lastAmount = key;
            headBump.makePeak (fs, 90.0, 0.9, 2.0 * colour);           // up to +2 dB at 90 Hz
            hfLoss.makeHighShelf (fs, 9000.0, 0.7, -4.0 * colour);     // up to -4 dB above ~9 kHz
        }

        const float driveDb = 18.0f * a + (overdrive ? 18.0f : 0.0f);
        const float drive = dbToGain (driveDb);                      // into the "tape"
        const float comp  = 1.0f / shape (drive * 0.25f) * 0.25f;    // keep level roughly constant

        // Glide the mix across the block so turning the knob doesn't click.
        const float wet0 = lastWet < 0.0f ? wetMix : lastWet;
        lastWet = wetMix;

        for (int c = 0; c < nch; ++c)
        {
            float* d = ch[c];
            for (int i = 0; i < n; ++i)
            {
                const float dry = d[i];
                float x = headBump.process (dry, c);
                x = shape (x * drive) * comp;
                // remove DC created by the asymmetric curve
                const float y = x - dcX[c] + dcR * dcY[c];
                dcX[c] = x; dcY[c] = y;
                x = hfLoss.process (y, c);
                const float w = wet0 + (wetMix - wet0) * (float) (i + 1) / (float) n;
                d[i] = dry + (x - dry) * w;
            }
        }
    }

    float lastWet = -1.0f;
};

//==============================================================================
// Vocal ducker, shared by the delay and the reverb: while the vocal is coming in,
// returns -12 dB by default (depthDb; reached over 20 ms); in the gaps it swells back to 0 dB over
// the release time (set from the Timing note). "Coming in" = within 18 dB of the
// vocal's own long-term level (3 s average) and above -55 dB, so it adapts to any
// input level and needs no threshold.
struct VocalDucker
{
    static constexpr float kDuckDb = 12.0f;
    float depthDb = kDuckDb;   // how far to duck (the delay uses less)

    float env = 0, envAtt = 0, envRel = 0, longMs = 0, longC = 0;
    float duckDb = 0, downC = 0, upC = 0;
    double fs = 44100, lastRel = -1;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        envAtt = timeCoef (0.001, fs); envRel = timeCoef (0.030, fs);
        longC  = timeCoef (3.0, fs);
        downC  = timeCoef (0.020, fs);
        lastRel = -1; setRelease (0.125);
        env = longMs = duckDb = 0;
    }

    void setRelease (double seconds)
    {
        if (seconds == lastRel) return;
        lastRel = seconds;
        upC = timeCoef (std::max (0.05, seconds), fs);
    }

    // One mono sample of the dry vocal in; duck amount in dB (0 or below) out.
    float process (float x)
    {
        env = follow (env, std::abs (x), envAtt, envRel);
        const float lvlDb = gainToDb (env);
        const float p = x * x;
        if (lvlDb > -60.0f) longMs = longMs <= 0.0f ? p : longC * longMs + (1.0f - longC) * p;
        const float refDb = 10.0f * std::log10 (longMs + 1.0e-12f) + 3.0f;   // RMS -> rough peak
        const bool  vocal = lvlDb > -55.0f && lvlDb > refDb - 18.0f;
        duckDb = follow (duckDb, vocal ? -depthDb : 0.0f, upC, downC);
        return duckDb;
    }
};

//==============================================================================
// Tempo delay. Repeats are filtered (150 Hz - 6 kHz) so they sit behind the vocal
// and run at full level; 'mix' crossfades dry -> wet (equal-power, 0 = dry only).
// The echoes duck 3 dB under the vocal (shared ducker design, the reverb ducks 12) and
// swell back in the gaps; they keep repeating underneath, only their level dips.
// The echoes you hear also get a gentle -1 dB mid dip at 1.5 / 2.1 / 3 kHz (by vocal
// range, same centres as the reverb's dip) so they sit behind the voice's presence.
struct TempoDelay
{
    VocalDucker ducker;
    float duckMeter = 0;   // how far the echoes are ducked (dB), largest since last read

    Biquad dip;            // -1 dB mid dip on the echoes you hear (not in the feedback loop)
    static constexpr double kDipHz[3] = { 1500.0, 2100.0, 3000.0 };   // bass/baritone, tenor/alto, mezzo/soprano
    int dipRange = -1;

    void setVocalRange (int r)
    {
        r = std::clamp (r, 0, 2);
        if (r == dipRange) return;
        dipRange = r;
        dip.makePeak (fs, kDipHz[r], 1.0, -1.0);   // broad (Q 1), like the reverb's dip
    }
    static constexpr float  kFeedback   = 0.35f;
    static constexpr double kMaxSeconds = 12.0;   // 2 bars of 4/4 at 40 BPM

    std::vector<float> buf[kMaxCh];
    int    size = 0, writePos = 0;
    double fs = 44100;
    float  curDelay = -1, smoothC = 0, lpA = 0, hpA = 0;
    float  lp[kMaxCh] {}, hp[kMaxCh] {};

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        size = (int) (fs * kMaxSeconds) + 8;
        for (auto& b : buf) b.assign ((size_t) size, 0.0f);
        writePos = 0;
        curDelay = -1;
        smoothC  = timeCoef (0.05, fs);
        lpA = 1.0f - (float) std::exp (-2.0 * kPi * 6000.0 / fs);
        hpA = 1.0f - (float) std::exp (-2.0 * kPi * 150.0 / fs);
        for (int c = 0; c < kMaxCh; ++c) lp[c] = hp[c] = 0;
        ducker.prepare (fs);
        ducker.depthDb = 3.0f;   // much lighter than the reverb, so the echoes stay clearly audible
        dip.reset(); dipRange = -1; setVocalRange (1);
    }

    void process (float* const* ch, int nch, int n, float delaySeconds, float mix)
    {
        const float target = std::clamp ((float) (delaySeconds * fs), 1.0f, (float) size - 4.0f);
        if (curDelay < 0) curDelay = target;

        const float m      = std::clamp (mix, 0.0f, 1.0f);
        const float dryG   = (float) std::cos (m * kPi * 0.5);
        const float wetG   = (float) std::sin (m * kPi * 0.5);

        for (int i = 0; i < n; ++i)
        {
            curDelay = smoothC * curDelay + (1.0f - smoothC) * target;

            float rp = (float) writePos - curDelay;
            if (rp < 0) rp += (float) size;
            const int   i0 = (int) rp;
            const int   i1 = (i0 + 1) % size;
            const float fr = rp - (float) i0;

            float mono = 0;
            for (int c = 0; c < nch; ++c) mono += ch[c][i];
            const float duckDb = ducker.process (mono / (float) nch);
            duckMeter = std::max (duckMeter, -duckDb);
            const float duckG = wetG * dbToGain (duckDb);

            for (int c = 0; c < nch; ++c)
            {
                auto& b = buf[c];
                const float d = b[(size_t) i0] + fr * (b[(size_t) i1] - b[(size_t) i0]);

                lp[c] += lpA * (d - lp[c]);
                hp[c] += hpA * (lp[c] - hp[c]);
                const float wet = lp[c] - hp[c];

                const float x = ch[c][i];
                b[(size_t) writePos] = x + wet * kFeedback;   // repeats keep going at full level...
                ch[c][i] = x * dryG + dip.process (wet, c) * duckG;   // ...only what you hear is ducked and dipped
            }
            writePos = (writePos + 1) % size;
        }
    }
};

//==============================================================================
// 9. High-pass + low-pass filter, 24 dB/octave each (two cascaded Butterworth stages).
struct PassFilters
{
    static constexpr double kQ1 = 0.5411961, kQ2 = 1.3065630;  // 4th-order Butterworth

    Biquad hp1, hp2, lp1, lp2;
    double fs = 44100;
    float lastHp = -1, lastLp = -1;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        hp1.reset(); hp2.reset(); lp1.reset(); lp2.reset();
        lastHp = lastLp = -1;
    }

    void process (float* const* ch, int nch, int n, float hpHz, float lpHz)
    {
        if (hpHz != lastHp) { lastHp = hpHz; hp1.makeHighPass (fs, hpHz, kQ1); hp2.makeHighPass (fs, hpHz, kQ2); }
        if (lpHz != lastLp) { lastLp = lpHz; lp1.makeLowPass (fs, lpHz, kQ1);  lp2.makeLowPass (fs, lpHz, kQ2); }

        for (int c = 0; c < nch; ++c)
        {
            float* d = ch[c];
            for (int i = 0; i < n; ++i)
                d[i] = lp2.process (lp1.process (hp2.process (hp1.process (d[i], c), c), c), c);
        }
    }
};

//==============================================================================
// Input high-pass (top bar), 24 dB/octave, before stage 1. Bypassed at its lowest setting.
struct InputHighPass
{
    Biquad s1, s2;
    double fs = 44100;
    float last = -1;

    void prepare (double sampleRate) { fs = sampleRate; s1.reset(); s2.reset(); last = -1; }

    void process (float* const* ch, int nch, int n, float hz)
    {
        if (hz != last)
        {
            last = hz;
            s1.makeHighPass (fs, hz, PassFilters::kQ1);
            s2.makeHighPass (fs, hz, PassFilters::kQ2);
        }
        for (int c = 0; c < nch; ++c)
            for (int i = 0; i < n; ++i)
                ch[c][i] = s2.process (s1.process (ch[c][i], c), c);
    }
};

//==============================================================================
// Light compressor (background, between stages 3 and 4). No controls.
// 1.5:1, 30 ms attack, release = the top-bar Timing note, 6 dB soft knee, RMS
// detection (10 ms), stereo-linked.
// Threshold sets itself: it follows the singing's own long-term loudness (a ~3 s
// average that ignores silence and gaps), so only the parts louder than the
// singer's average get gently pulled in.
// Output is matched to the input (~1 s averages, 0 to +6 dB), so it never changes
// the overall level - only evens it out.
struct LightComp
{
    static constexpr float kRatio = 1.5f, kKneeDb = 6.0f, kMaxMakeupDb = 6.0f;

    double fs = 44100, lastRel = -1;
    float rmsMs = 0, rmsC = 0, longMs = 0, longC = 0;
    float grDb = 0, attC = 0, relC = 0;
    float inAvg = 0, outAvg = 0, avgC = 0, makeupDb = 0, mkC = 0;
    float meter = 0;   // most gain reduction (dB) since last read

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        rmsC = timeCoef (0.010, fs);
        longC = timeCoef (3.0, fs);
        attC = timeCoef (0.030, fs);
        avgC = timeCoef (1.0, fs);
        mkC  = timeCoef (0.5, fs);
        lastRel = -1;
        setRelease (0.5);
        rmsMs = longMs = grDb = inAvg = outAvg = makeupDb = 0;
    }

    void setRelease (double seconds)
    {
        if (seconds == lastRel) return;
        lastRel = seconds;
        relC = timeCoef (std::max (0.02, seconds), fs);
    }

    void process (float* const* ch, int nch, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            float p = 0;
            for (int c = 0; c < nch; ++c) p += ch[c][i] * ch[c][i];
            p /= (float) nch;

            rmsMs = rmsC * rmsMs + (1.0f - rmsC) * p;
            const float lvl = 10.0f * std::log10 (rmsMs + 1.0e-12f);

            // Auto threshold: long-term loudness of the singing only.
            float thrDb = 10.0f * std::log10 (longMs + 1.0e-12f);
            const bool singing = lvl > -60.0f && (longMs <= 0.0f || lvl > thrDb - 20.0f);
            if (singing)
            {
                longMs = longMs <= 0.0f ? rmsMs : longC * longMs + (1.0f - longC) * rmsMs;
                thrDb = 10.0f * std::log10 (longMs + 1.0e-12f);
            }

            const float target = longMs > 0.0f ? computeGR (lvl, thrDb, kRatio, kKneeDb) : 0.0f;
            grDb = target > grDb ? attC * grDb + (1.0f - attC) * target
                                 : relC * grDb + (1.0f - relC) * target;
            meter = std::max (meter, grDb);

            const float g = dbToGain (-grDb);

            // Match output to input (held during silence).
            if (singing)
            {
                inAvg  = avgC * inAvg  + (1.0f - avgC) * p;
                outAvg = avgC * outAvg + (1.0f - avgC) * p * g * g;
            }
            const float mkTarget = (inAvg > 1.0e-9f && outAvg > 1.0e-9f)
                                     ? std::clamp (10.0f * std::log10 (inAvg / outAvg), 0.0f, kMaxMakeupDb) : 0.0f;
            makeupDb = mkC * makeupDb + (1.0f - mkC) * mkTarget;

            const float total = g * dbToGain (makeupDb);
            for (int c = 0; c < nch; ++c) ch[c][i] *= total;
        }
    }
};

//==============================================================================
// Warmth & Air (stage 4, after the background compressor). Two knobs, 0-100 %.
// Warmth - vintage tube-style saturation: a biased (asymmetric) tanh stage, so it
//   adds mostly even harmonics like a triode, driven up to +18 dB. The curve is
//   loudness-matched (~150 ms averages), so turning it up adds colour and rounds
//   peaks without changing the level. A small low-mid lift (180 / 250 / 320 Hz by
//   vocal range, up to
//   +2 dB) and a soft top (7 kHz shelf, up to -2 dB) give the "vintage" tone.
// Air - harmonic exciter: takes the top of the voice (above 3 / 3.5 / 4.2 kHz by
//   vocal range), normalises it
//   to a steady level so it works the same on quiet and loud lines, bends it
//   through a soft asymmetric curve to create new upper harmonics, keeps only what
//   lands above 5 / 6 / 7 kHz, and blends that in. Adds sheen and openness without the
//   harshness of a plain treble boost.
struct WarmthAir
{
    static constexpr float kBias = 0.15f;      // tube bias -> even harmonics
    static constexpr float kAirLevel = 2.0f;   // exciter blend at 100 %

    Biquad lowMid, softTop;          // warmth tone
    Biquad airHp1, airHp2;           // exciter input band (3 / 3.5 / 4.2 kHz by vocal range)
    Biquad genHp1, genHp2;           // keep only new harmonics above 5 / 6 / 7 kHz
    float  dcX[kMaxCh] {}, dcY[kMaxCh] {}, dcR = 0.995f;
    float  airEnv[kMaxCh] {}, envAtt = 0, envRel = 0;
    float  lastWarm = -1, lastWarmAmt = 0, lastAirAmt = 0;
    float  wIn[kMaxCh] {}, wOut[kMaxCh] {}, wGain[kMaxCh] { 1.0f, 1.0f }, matchC = 0;   // warmth loudness match
    double fs = 44100;

    // Follow the top-bar vocal range: bass/baritone, tenor/alto, mezzo/soprano.
    static constexpr double kLiftHz[3]   = { 180.0, 250.0, 320.0 };    // Warmth body lift
    static constexpr double kAirInHz[3]  = { 3000.0, 3500.0, 4200.0 }; // Air: top of the voice it excites
    static constexpr double kAirOutHz[3] = { 5000.0, 6000.0, 7000.0 }; // Air: new harmonics kept above
    int range = -1;

    void setVocalRange (int r)
    {
        r = std::clamp (r, 0, 2);
        if (r == range) return;
        range = r;
        airHp1.makeHighPass (fs, kAirInHz[r], 0.707);  airHp2.makeHighPass (fs, kAirInHz[r], 0.707);
        genHp1.makeHighPass (fs, kAirOutHz[r], 0.707); genHp2.makeHighPass (fs, kAirOutHz[r], 0.707);
        lastWarm = -1;   // re-tune the warmth lift on the next block
    }

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        for (auto* b : { &lowMid, &softTop, &airHp1, &airHp2, &genHp1, &genHp2 }) b->reset();
        range = -1; setVocalRange (1);
        dcR = 1.0f - (float) (2.0 * kPi * 10.0 / fs);
        envAtt = timeCoef (0.001, fs); envRel = timeCoef (0.030, fs);
        for (int c = 0; c < kMaxCh; ++c) dcX[c] = dcY[c] = airEnv[c] = 0;
        lastWarm = -1; lastWarmAmt = lastAirAmt = 0;
        matchC = timeCoef (0.150, fs);
        for (int c = 0; c < kMaxCh; ++c) { wIn[c] = wOut[c] = 0; wGain[c] = 1.0f; }
    }

    static inline float tube (float x, float drive)
    {
        // Biased tanh, minus its resting point, divided by its slope at rest -> unity gain for small signals.
        const float t0 = std::tanh (kBias);
        const float slope = drive * (1.0f - t0 * t0);
        return (std::tanh (drive * x + kBias) - t0) / slope;
    }

    // warmth, air: 0-100 %
    void process (float* const* ch, int nch, int n, float warmth, float air)
    {
        const float w = std::clamp (warmth * 0.01f, 0.0f, 1.0f);
        const float a = std::clamp (air * 0.01f, 0.0f, 1.0f);
        if (w <= 0.0f && a <= 0.0f && lastWarmAmt <= 0.0f && lastAirAmt <= 0.0f) return;

        if (w != lastWarm)
        {
            lastWarm = w;
            lowMid.makePeak (fs, kLiftHz[std::max (0, range)], 0.7, 2.0 * w);
            softTop.makeHighShelf (fs, 7000.0, 0.7, -2.0 * w);
        }
        const float drive = dbToGain (18.0f * w);
        const float w0 = lastWarmAmt, a0 = lastAirAmt;
        lastWarmAmt = w; lastAirAmt = a;

        for (int c = 0; c < nch; ++c)
        {
            float* d = ch[c];
            for (int i = 0; i < n; ++i)
            {
                const float t  = (float) (i + 1) / (float) n;
                const float wm = w0 + (w - w0) * t;   // smoothed amounts (no zipper noise)
                const float am = a0 + (a - a0) * t;
                float x = d[i];

                // --- Warmth ---
                if (wm > 0.0f)
                {
                    float s = tube (lowMid.process (x, c), drive);
                    const float y = s - dcX[c] + dcR * dcY[c];   // remove the DC the bias creates
                    dcX[c] = s; dcY[c] = y;
                    s = softTop.process (y, c);
                    // Keep the warmth path as loud as what went in (held during silence).
                    wIn[c]  = matchC * wIn[c]  + (1.0f - matchC) * x * x;
                    wOut[c] = matchC * wOut[c] + (1.0f - matchC) * s * s;
                    if (wIn[c] > 1.0e-9f && wOut[c] > 1.0e-9f)
                    {
                        const float target = std::clamp (std::sqrt (wIn[c] / wOut[c]), 0.25f, 4.0f);
                        wGain[c] = matchC * wGain[c] + (1.0f - matchC) * target;
                    }
                    s *= wGain[c];
                    x += (s - x) * std::min (1.0f, wm * 4.0f);  // fades in over the first 25 %
                }

                // --- Air ---
                if (am > 0.0f)
                {
                    const float band = airHp2.process (airHp1.process (x, c), c);
                    airEnv[c] = follow (airEnv[c], std::abs (band), envAtt, envRel);
                    const float u  = band / (airEnv[c] + 1.0e-6f);              // steady level, ~ +/-1
                    const float th = std::tanh (3.0f * u);
                    const float gen = (th + 0.3f * th * th) * airEnv[c];       // asymmetric -> odd + even harmonics
                    const float top = genHp2.process (genHp1.process (gen, c), c);
                    x += top * am * kAirLevel;
                }
                d[i] = x;
            }
        }
    }
};

//==============================================================================
// Reverb (stage 10) with automatic ducking.
// An 8-line feedback delay network (Hadamard mixing, damping in each loop) fed
// through pre-delay and 4 diffusion all-passes. The wet signal always goes through
// a fixed 250 Hz high-pass and 6 kHz low-pass (12 dB/oct each), so the reverb
// stays out of the low end and the sibilance, plus a gentle -1.5 dB dip at 1.5 /
// 2.1 / 3 kHz (by vocal range) so it sits behind the voice's presence. The dry
// vocal is not filtered.
// Types (in switch order):
// Pre-delay and length follow the song tempo (see kTypes), e.g. at 120 BPM:
//   Room    - small, darker, ~8 ms pre-delay (1/256 note), ~0.85 s (1.7 beats)
//   Hall    - big, ~31 ms (1/64 note), ~2.55 s (5.1 beats)
//   Plate   - dense and bright, ~16 ms (1/128 note), ~2.1 s (4.25 beats)
//   Chamber - large and smooth, ~23 ms (3/256 note), ~1.7 s (3.4 beats)
//   Shimmer - long hall, ~31 ms (1/64 note), ~3.4 s (6.8 beats), with an octave-up
//             copy of the tail fed back in, so it blooms upward
// Ducking (VocalDucker, same as the delay): -12 dB while the vocal is coming in,
// swelling back in the gaps, so the tail fills them and the vocal stays clear.
// Mix: 0-100 % reverb added on top of the untouched dry signal.
struct DuckingReverb
{
    static constexpr int kLines = 8, kDiff = 4;
    // preNote / lenBeats follow the song tempo: pre-delay as a fraction of a 1/4 note,
    // length (time to fade 60 dB) in beats. baseRt60 bounds the length to 0.5x-2x so
    // very slow or very fast songs stay sensible.
    struct Type { float preNote, size, lenBeats, baseRt60, dampHz, diffusion, width, shimmer; };
    static constexpr int kNumTypes = 5;
    static constexpr Type kTypes[kNumTypes] = {
        { 1.0f / 64.0f, 0.40f, 1.7f, 0.94f, 5000.0f, 0.60f, 0.8f, 0.0f  },   // room    - 1/256 note, 1.7 beats
        { 1.0f / 16.0f, 1.00f, 5.1f, 2.72f, 7000.0f, 0.72f, 1.0f, 0.0f  },   // hall    - 1/64 note,  5.1 beats
        { 1.0f / 32.0f, 0.65f, 4.25f, 2.38f, 9000.0f, 0.75f, 1.0f, 0.0f  },   // plate   - 1/128 note, 4.25 beats
        { 3.0f / 64.0f, 0.90f, 3.4f, 1.87f, 6500.0f, 0.70f, 1.0f, 0.0f  },   // chamber - 3/256 note, 3.4 beats
        { 1.0f / 16.0f, 1.00f, 6.8f, 3.4f, 8000.0f, 0.75f, 1.0f, 0.35f },   // shimmer - 1/64 note,  6.8 beats
    };
    static constexpr float kLineMs[kLines] = { 29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.7f, 73.1f };
    static constexpr float kDiffMs[kDiff]  = { 4.7f, 3.6f, 12.7f, 9.3f };
    static constexpr float kDuckDb = VocalDucker::kDuckDb;
    static constexpr float kWetGain = 3.5f;   // ~+11 dB (makes up for the 250 Hz / 6 kHz wet filters), so full Mix is a big, obvious reverb

    std::vector<float> line[kLines], diff[kDiff], pre;
    int   len[kLines] {}, pos[kLines] {}, diffLen[kDiff] {}, diffPos[kDiff] {}, preLen = 1, prePos = 0;
    float fb[kLines] {}, lpZ[kLines] {}, lpA = 0, diffG = 0.7f, width = 1.0f, shimmer = 0.0f;
    Biquad wetHp, wetLp;   // fixed 250 Hz high-pass and 6 kHz low-pass on the wet signal
    Biquad wetDip;         // -1.5 dB mid dip, centre set by the vocal range
    static constexpr double kDipHz[3] = { 1500.0, 2100.0, 3000.0 };   // bass/baritone, tenor/alto, mezzo/soprano
    int dipRange = -1;

    void setVocalRange (int r)
    {
        r = std::clamp (r, 0, 2);
        if (r == dipRange) return;
        dipRange = r;
        wetDip.makePeak (fs, kDipHz[r], 1.0, -1.5);   // broad (Q 1), so it shapes the whole 1.5-3 kHz area gently
    }

    // Shimmer: octave-up pitch shifter (two crossfaded taps reading at double speed)
    std::vector<float> psBuf;
    int   psPos = 0, psW = 1;
    float psPhase = 0, prevOut = 0;

    float pitchUp (float in)
    {
        const int size = (int) psBuf.size();
        psBuf[(size_t) psPos] = in;
        float out = 0;
        for (int tap = 0; tap < 2; ++tap)
        {
            float ph = psPhase + 0.5f * (float) tap;
            if (ph >= 1.0f) ph -= 1.0f;
            const float d   = 1.0f + (float) psW * (1.0f - ph);   // delay shrinks 1 sample per sample = 2x speed
            float rp = (float) psPos - d;
            while (rp < 0.0f) rp += (float) size;
            const int   i0 = (int) rp;
            const float fr = rp - (float) i0;
            const float s  = psBuf[(size_t) i0] + fr * (psBuf[(size_t) ((i0 + 1) % size)] - psBuf[(size_t) i0]);
            const float w  = std::sin ((float) kPi * ph);
            out += s * w * w;   // the two windows add up to 1
        }
        psPhase += 1.0f / (float) psW;
        if (psPhase >= 1.0f) psPhase -= 1.0f;
        psPos = (psPos + 1) % size;
        return out;
    }
    double fs = 44100;
    int    type = -1;

    VocalDucker ducker;   // shared design with the delay
    float  lastMix = 0;
    double quarterSec = 0.5, appliedQuarter = -1;   // song tempo (one 1/4 note, seconds)
    float meter = 0;   // how far the reverb is ducked right now (dB), largest since last read

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        for (int k = 0; k < kLines; ++k) line[k].assign ((size_t) (kLineMs[k] * 0.001 * fs) + 4, 0.0f);
        for (int k = 0; k < kDiff; ++k)  diff[k].assign ((size_t) (kDiffMs[k] * 0.001 * fs) + 4, 0.0f);
        pre.assign ((size_t) (0.120 * fs) + 4, 0.0f);   // room for 1/64 note at 40 BPM
        std::fill (lpZ, lpZ + kLines, 0.0f);
        std::fill (pos, pos + kLines, 0); std::fill (diffPos, diffPos + kDiff, 0); prePos = 0;
        wetHp.makeHighPass (fs, 250.0, 0.707);  wetHp.reset();
        wetLp.makeLowPass  (fs, 6000.0, 0.707); wetLp.reset();
        wetDip.reset(); dipRange = -1; setVocalRange (1);
        psW = std::max (16, (int) (0.050 * fs));   // 50 ms grains
        psBuf.assign ((size_t) psW + 8, 0.0f);
        psPos = 0; psPhase = 0; prevOut = 0;
        ducker.prepare (fs);
        lastMix = 0;
        type = -1; appliedQuarter = -1; setType (2);
    }

    void setRelease (double seconds) { ducker.setRelease (seconds); }

    // Song tempo (one 1/4 note in seconds): re-tunes pre-delay and length when it moves.
    void setTempo (double quarterSeconds)
    {
        quarterSec = std::clamp (quarterSeconds, 0.1, 2.0);
        if (type >= 0 && std::abs (quarterSec - appliedQuarter) > 0.005 * appliedQuarter)
            applyType();
    }

    void setType (int t)
    {
        t = std::clamp (t, 0, kNumTypes - 1);
        if (t == type) return;
        type = t;
        applyType();
    }

    void applyType()
    {
        appliedQuarter = quarterSec;
        const auto& p = kTypes[type];
        const float rt60 = std::clamp (p.lenBeats * (float) quarterSec, 0.5f * p.baseRt60, 2.0f * p.baseRt60);
        for (int k = 0; k < kLines; ++k)
        {
            len[k] = std::clamp ((int) (kLineMs[k] * p.size * 0.001 * fs), 1, (int) line[k].size() - 1);
            pos[k] %= len[k];
            fb[k]  = std::pow (10.0f, -3.0f * (float) len[k] / (rt60 * (float) fs));   // RT60 decay per loop
        }
        for (int k = 0; k < kDiff; ++k)
        {
            diffLen[k] = std::clamp ((int) (kDiffMs[k] * (0.5f + 0.5f * p.size) * 0.001 * fs), 1, (int) diff[k].size() - 1);
            diffPos[k] %= diffLen[k];
        }
        preLen = std::clamp ((int) (p.preNote * quarterSec * fs), 1, (int) pre.size() - 1);
        lpA    = (float) std::exp (-2.0 * kPi * p.dampHz / fs);
        diffG  = p.diffusion;
        width  = p.width;
        shimmer = p.shimmer;
    }

    // mix: 0-1
    void process (float* const* ch, int nch, int n, float mix)
    {
        if (mix <= 0.0f && lastMix <= 0.0f) { lastMix = 0.0f; return; }
        const float mix0 = lastMix;
        lastMix = mix;
        const int preSize = (int) pre.size();

        for (int i = 0; i < n; ++i)
        {
            float x = 0;
            for (int c = 0; c < nch; ++c) x += ch[c][i];
            x /= (float) nch;

            // --- Ducking: is the vocal coming in right now? ---
            const float duckDb = ducker.process (x);
            meter = std::max (meter, -duckDb);

            // --- Pre-delay ---
            pre[(size_t) prePos] = x;
            float y = pre[(size_t) ((prePos - preLen + preSize) % preSize)];
            prePos = (prePos + 1) % preSize;

            // --- Input diffusion (Schroeder all-passes) ---
            for (int k = 0; k < kDiff; ++k)
            {
                float& slot = diff[k][(size_t) diffPos[k]];
                const float v = slot;
                const float out = v - diffG * y;
                slot = y + diffG * out;
                y = out;
                diffPos[k] = (diffPos[k] + 1) % diffLen[k];
            }

            // --- Shimmer: feed an octave-up copy of the tail back in (soft-limited) ---
            if (shimmer > 0.0f)
                y += std::tanh (pitchUp (prevOut) * shimmer);

            // --- FDN ---
            float o[kLines];
            for (int k = 0; k < kLines; ++k)
            {
                o[k] = line[k][(size_t) pos[k]];
                lpZ[k] = o[k] + lpA * (lpZ[k] - o[k]);   // damping
            }
            float h[kLines];
            std::copy (lpZ, lpZ + kLines, h);
            for (int s = 1; s < kLines; s <<= 1)          // fast Hadamard
                for (int a = 0; a < kLines; a += s << 1)
                    for (int b = a; b < a + s; ++b)
                    {
                        const float u = h[b], v = h[b + s];
                        h[b] = u + v; h[b + s] = u - v;
                    }
            constexpr float norm = 0.35355339f;           // 1/sqrt(8)
            for (int k = 0; k < kLines; ++k)
            {
                line[k][(size_t) pos[k]] = h[k] * norm * fb[k] + y * ((k & 1) ? -0.5f : 0.5f);
                pos[k] = (pos[k] + 1) % len[k];
            }

            float wl = (o[0] - o[2] + o[4] - o[6]) * 0.5f;
            float wr = (o[1] - o[3] + o[5] - o[7]) * 0.5f;
            const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * width;
            wl = mid + side; wr = mid - side;
            prevOut = mid;

            const float m = mix0 + (mix - mix0) * (float) (i + 1) / (float) n;
            const float g = m * kWetGain * dbToGain (duckDb);
            auto filt = [&] (float v, int c) { return wetDip.process (wetLp.process (wetHp.process (v, c), c), c); };
            if (nch > 1)
            {
                ch[0][i] += filt (wl, 0) * g;
                ch[1][i] += filt (wr, 1) * g;
            }
            else
                ch[0][i] += filt (mid, 0) * g;
        }
    }
};

} // namespace vc
