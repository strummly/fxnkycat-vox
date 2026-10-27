#pragma once

// All of the signal processing for the 7 stages lives here.
// Everything is allocation-free on the audio thread (buffers are sized in prepare()).

#include <algorithm>
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
// 1. Volume rider
// Measures a slow RMS level. When the level is above the threshold, it moves the
// gain so the output sits at the target loudness. Below the threshold it eases
// back to 0 dB (no change), so breaths/noise are not pushed up.
struct VolumeRider
{
    static constexpr float kMaxRangeDb = 12.0f;

    float ms = 0, gainDb = 0;
    float meter = 0;   // largest ride (dB, either direction) since last read
    float rmsC = 0, downC = 0, upC = 0, idleC = 0;
    double fs = 44100, lastBeat = -1;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        lastBeat = -1;
        setTiming (0.5);
        ms = 0; gainDb = 0;
    }

    // Timing follows the song. With 'beat' = the plugin's Timing note (1/4 by default):
    // loudness window = 1 x, turn-down = 1/4 x, turn-up = 1 x, return to unity = 2 x.
    void setTiming (double beat)
    {
        if (beat == lastBeat) return;
        lastBeat = beat;
        rmsC  = timeCoef (beat,         fs);
        downC = timeCoef (beat * 0.25,  fs);
        upC   = timeCoef (beat,         fs);
        idleC = timeCoef (beat * 2.0,   fs);
    }

    void process (float* const* ch, int nch, int n, float threshDb, float targetDb)
    {
        for (int i = 0; i < n; ++i)
        {
            float p = 0;
            for (int c = 0; c < nch; ++c) p += ch[c][i] * ch[c][i];
            p /= (float) nch;

            ms = rmsC * ms + (1.0f - rmsC) * p;
            const float levelDb = 10.0f * std::log10 (ms + 1.0e-12f);

            float c;
            float want = 0.0f;
            if (levelDb > threshDb)
            {
                want = std::clamp (targetDb - levelDb, -kMaxRangeDb, kMaxRangeDb);
                c = want < gainDb ? downC : upC;
            }
            else
            {
                c = idleC;
            }

            gainDb = c * gainDb + (1.0f - c) * want;
            meter = std::max (meter, std::abs (gainDb));
            const float g = dbToGain (gainDb);
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

    void process (float* const* ch, int nch, int n, float threshDb, float ratio, float maxCutDb)
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
            const float cutDb = std::min (maxCutDb, computeGR (gainToDb (env), threshDb, ratio, 4.0f));
            meter = std::max (meter, cutDb);
            const float gm1   = dbToGain (-cutDb) - 1.0f;

            for (int c = 0; c < nch; ++c) ch[c][i] += gm1 * bp[c];
        }
    }
};

//==============================================================================
// 2. Comp-gate in a classic "de-ess + gate" setup (modelled on how a C1-style
// comp/gate is set up for vocals):
//   - Gate first: wideband, fast open, short hold, smooth close to a -30 dB floor.
//     Its threshold sits 40 dB under the main threshold, with 4 dB hysteresis.
//   - Then split-band de-ess compression: the sidechain listens to a band around
//     6 kHz and only that high band is turned down (4:1, near-hard knee, 0.5 ms / 50 ms).
// One control: threshold.
struct CompGate
{
    static constexpr float kGateOffsetDb = -40.0f, kGateHystDb = 4.0f, kGateFloorDb = -30.0f;
    static constexpr float kDeessRatio = 4.0f, kDeessMaxCutDb = 18.0f;

    DynamicBand deess;
    float gateEnv = 0, gateGainDb = 0;
    float envAtt = 0, envRel = 0, openC = 0, closeC = 0;
    int   holdSamples = 0, holdLeft = 0;
    bool  open = false;
    float meter = 0;   // most reduction (gate or de-ess) since last read

    void prepare (double fs)
    {
        deess.prepare (fs, 6000.0, 0.9, 0.5f, 50.0f);
        envAtt = timeCoef (0.0001, fs);   // 0.1 ms detector
        envRel = timeCoef (0.030, fs);
        openC  = timeCoef (0.0005, fs);   // opens in ~0.5 ms
        closeC = timeCoef (0.150, fs);    // closes over ~150 ms
        holdSamples = (int) (0.050 * fs); // 50 ms hold
        holdLeft = 0;
        gateEnv = 0; gateGainDb = kGateFloorDb; open = false;
    }

    void process (float* const* ch, int nch, int n, float threshDb)
    {
        const float openAt  = threshDb + kGateOffsetDb;
        const float closeAt = openAt - kGateHystDb;

        // Gate
        for (int i = 0; i < n; ++i)
        {
            float peak = 0;
            for (int c = 0; c < nch; ++c) peak = std::max (peak, std::abs (ch[c][i]));
            gateEnv = follow (gateEnv, peak, envAtt, envRel);
            const float lvl = gainToDb (gateEnv);

            if (lvl > openAt)            { open = true; holdLeft = holdSamples; }
            else if (lvl < closeAt)      { if (holdLeft > 0) --holdLeft; else open = false; }

            const float target = open ? 0.0f : kGateFloorDb;
            const float c = target > gateGainDb ? openC : closeC;
            gateGainDb = c * gateGainDb + (1.0f - c) * target;

            meter = std::max (meter, -gateGainDb);
            const float g = dbToGain (gateGainDb);
            for (int c2 = 0; c2 < nch; ++c2) ch[c2][i] *= g;
        }

        // Split-band de-ess compression
        deess.process (ch, nch, n, threshDb, kDeessRatio, kDeessMaxCutDb);
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
    float meter = 0;   // deepest cut (dB) since last read
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

    void process (float* const* ch, int nch, int n, float threshDb, float ratio, float maxCutDb)
    {
        for (int i = 0; i < n; ++i)
        {
            float mono = 0;
            for (int c = 0; c < nch; ++c) mono += ch[c][i];
            mono /= (float) nch;

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
            const float cutDb = std::min (maxCutDb, computeGR (gainToDb (cutEnv), threshDb, ratio, 4.0f)) * standOut;
            meter = std::max (meter, cutDb);
            const float gm1 = dbToGain (-cutDb) - 1.0f;

            for (int c = 0; c < nch; ++c) ch[c][i] += gm1 * bp[c];
        }
    }
};

//==============================================================================
// 3. Problem EQ: three resonance trackers, each searching its own range
// (low mud, mid boxy/nasal ring, upper harshness). The male/female switch moves
// the search ranges. The threshold slider is shared; the upper bands get an offset
// because vocals naturally carry less energy up there.
struct ProblemEQ
{
    struct Region { double maleLo, maleHi, femLo, femHi, q; float threshOffset; };
    static constexpr Region kRegions[3] = {
        { 150.0,  400.0,  200.0,  500.0,  3.0,   0.0f },  // mud
        { 400.0,  2000.0, 500.0,  2500.0, 4.5,  -6.0f },  // boxy / nasal resonance
        { 2000.0, 5000.0, 2500.0, 6500.0, 4.0, -10.0f },  // harshness
    };
    static constexpr float kRatio = 3.0f, kMaxCutDb = 8.0f;

    ResonanceTracker trackers[3];
    int voice = -1;  // 0 male, 1 female

    void prepare (double fs)
    {
        for (int b = 0; b < 3; ++b)
            trackers[b].prepare (fs, kRegions[b].maleLo, kRegions[b].maleHi, kRegions[b].q);
        voice = 0;
    }

    void process (float* const* ch, int nch, int n, float threshDb, bool female)
    {
        const int v = female ? 1 : 0;
        if (v != voice)
        {
            voice = v;
            for (int b = 0; b < 3; ++b)
                trackers[b].setRange (female ? kRegions[b].femLo : kRegions[b].maleLo,
                                      female ? kRegions[b].femHi : kRegions[b].maleHi);
        }
        for (int b = 0; b < 3; ++b)
            trackers[b].process (ch, nch, n, threshDb + kRegions[b].threshOffset, kRatio, kMaxCutDb);
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
// Scans 3-10 kHz ("sh" sits low in that range, "s" higher), follows whichever
// frequency is loudest at that moment, and cuts a band there when it crosses the
// detection threshold. Retargets within a few ms, so it can jump from an "sh" to an "s".
struct DeEsser
{
    ResonanceTracker tracker;

    void prepare (double fs)
    {
        tracker.prepare (fs, 3000.0, 10000.0, 1.6);
        tracker.setStyle (true, 0.004, 0.005, 0.3f, 60.0f);  // 4 ms detect, 5 ms glide, 0.3/60 ms cut
    }

    // sensitivity: 0-100 %, reductionDb: max cut in dB
    void process (float* const* ch, int nch, int n, float sensitivity, float reductionDb)
    {
        const float threshDb = -6.0f - 0.44f * sensitivity;  // 0 % = -6 dB, 100 % = -50 dB
        tracker.process (ch, nch, n, threshDb, 8.0f, reductionDb);
    }

    float takeMeter() { const float m = tracker.meter; tracker.meter = 0; return m; }
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

    void process (float* const* ch, int nch, int n)
    {
        for (int c = 0; c < nch; ++c)
        {
            float* d = ch[c];
            for (int i = 0; i < n; ++i)
            {
                float x = d[i];
                x = lowBoost.process (x, c);
                x = lowAtten.process (x, c);
                x = highBoost.process (x, c);
                x = highAtten.process (x, c);
                d[i] = x;
            }
        }
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
        lastAmount = -1;
        dcR = 1.0f - (float) (2.0 * kPi * 10.0 / fs);   // 10 Hz DC blocker
        for (int c = 0; c < kMaxCh; ++c) dcX[c] = dcY[c] = 0;
    }

    static inline float shape (float x)
    {
        // Asymmetric soft clip: slightly different curve for positive/negative.
        return x >= 0.0f ? std::tanh (x) : std::tanh (x * 0.9f) / 0.9f * 0.95f;
    }

    // amount: 0-100 %
    void process (float* const* ch, int nch, int n, float amount)
    {
        const float a = std::clamp (amount * 0.01f, 0.0f, 1.0f);
        if (a <= 0.0f) return;

        if (a != lastAmount)
        {
            lastAmount = a;
            headBump.makePeak (fs, 90.0, 0.9, 2.0 * a);           // up to +2 dB at 90 Hz
            hfLoss.makeHighShelf (fs, 9000.0, 0.7, -4.0 * a);     // up to -4 dB above ~9 kHz
        }

        const float drive = dbToGain (18.0f * a);                // up to +18 dB into the "tape"
        const float comp  = 1.0f / shape (drive * 0.25f) * 0.25f; // keep level roughly constant

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
                d[i] = x;
            }
        }
    }
};

//==============================================================================
// 7. Tempo delay, fixed 1/8 note. Repeats are filtered (150 Hz - 6 kHz) so they
// sit behind the vocal. 'amount' is the level of the repeats added to the dry signal.
struct TempoDelay
{
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
    }

    void process (float* const* ch, int nch, int n, float delaySeconds, float amount)
    {
        const float target = std::clamp ((float) (delaySeconds * fs), 1.0f, (float) size - 4.0f);
        if (curDelay < 0) curDelay = target;

        for (int i = 0; i < n; ++i)
        {
            curDelay = smoothC * curDelay + (1.0f - smoothC) * target;

            float rp = (float) writePos - curDelay;
            if (rp < 0) rp += (float) size;
            const int   i0 = (int) rp;
            const int   i1 = (i0 + 1) % size;
            const float fr = rp - (float) i0;

            for (int c = 0; c < nch; ++c)
            {
                auto& b = buf[c];
                const float d = b[(size_t) i0] + fr * (b[(size_t) i1] - b[(size_t) i0]);

                lp[c] += lpA * (d - lp[c]);
                hp[c] += hpA * (lp[c] - hp[c]);
                const float wet = lp[c] - hp[c];

                const float x = ch[c][i];
                b[(size_t) writePos] = x + wet * kFeedback;
                ch[c][i] = x + wet * amount;
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

} // namespace vc
