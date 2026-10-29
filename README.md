# fxnkycat vox (VST3 / AU)

A 11-stage vocal processing chain (plus a background compressor) in one plugin, built with JUCE 8.

| # | Stage | Controls | Fixed |
|---|-------|----------|-------|
| – | Timing (top bar) | Scrollable note value: 2 bar … 1/16t (default 1/4) | Tempo comes from the project automatically (120 BPM if the host doesn't report one). Sets: the rider's timing (Natural/Balanced), the background compressor's release, the compressor's auto release, the comp-gate's hold (1/8 note) and close (0.3 note), and how fast the delay and reverb swell back after ducking (1/4 note) |
| 1 | Rider (GainAim-style) | Speed (Natural, Balanced, Fast), Noise floor, Target loudness | Stereo-linked. Rides the singing to the target, ignoring gaps and breaths and holding still while under the noise floor. Natural follows phrases (Timing note), Balanced is 4x slower, Fast follows words (all ±12 dB). Anything under the noise floor (20 ms detector) is pulled down by as much as it is under, up to 12 dB |
| 2 | Gate/Pre-Esser (gate + de-ess) | Threshold (sets the de-ess only) | Gate: opens at stage 1's Noise Floor when the Volume Rider is on, otherwise sets itself 30 dB under the singing's own level (learns it in ~0.2 s). 4 dB hysteresis, closes to -30 dB; hold = 1/8 and close = 0.3 of the Timing note. LED shows the gate. De-ess: sidechain around 5 / 6 / 7 kHz (by vocal range), only that band is cut, 4:1, 0.5 ms / 50 ms, up to 18 dB, and only on S, SH and CH (same detector as stage 5). Meter bar shows the de-ess |
| 3 | Problem EQ (resonance search) | Threshold (vocal range is in the top bar) | Three trackers search low / mid / high ranges set by the vocal range (Bass/Baritone 120–400 Hz, 350 Hz–2 kHz, 1.8–5 kHz; Tenor/Alto 170–500 Hz, 450 Hz–2.5 kHz, 2.2–6 kHz; Mezzo/Soprano 220–650 Hz, 550 Hz–3 kHz, 2.7–7 kHz) for the frequency that sticks out, then cut it up to 8 dB |
| (3½) | Light compressor (background, no panel) | – | Always on between stages 3 and 4 (Problem EQ and Warmth / Air). 1.5:1, 30 ms attack, release = Timing note, 6 dB soft knee, RMS detection. Threshold sets itself to the singing's own long-term loudness (3 s average, ignores gaps), output is matched to the input (0 to +6 dB), so it only evens out the louder parts. Can be switched off as the "Light Comp On" parameter in the DAW |
| 4 | Harmonics | Exciter (top), Warmth (bottom), both start at 0 | Warmth: vintage tube-style saturation (biased tanh = mostly even harmonics, up to +18 dB drive, level-matched at low levels) with a small low-mid lift and soft 7 kHz top. Exciter: harmonic exciter - the top of the voice is level-normalised, bent through a soft asymmetric curve to make new harmonics, and only the new top is blended in. Both follow the vocal range: lift 180 / 250 / 320 Hz, Exciter from 3 / 3.5 / 4.2 kHz, new harmonics above 5 / 6 / 7 kHz |
| 5 | De-esser (sibilance search) | Detection, Reduction (both start at 0), Solo | Scans 2.5–8.5 kHz (Bass/Baritone), 3–10 kHz (Tenor/Alto) or 3.5–11 kHz (Mezzo/Soprano), follows the loudest S/SH frequency (retargets in ~5 ms) and cuts a band there; 0.3 ms / 60 ms. Only reacts to S, SH and CH: the top end must outweigh the body of the voice (below 1.5 kHz) and the sound must be noise-like (high zero-crossing rate), so vowels, "ee"s and single clicks are left alone. Solo plays what is being detected (and skips stages 6–11 and auto gain while on) |
| 6 | Vintage EQ (Pultec EQP-1A style) | Low Freq (20/30/60/100), Low Boost, Low Atten, High Freq (3/4/5/8/10/12/16k), High Boost, Bandwidth, Atten Select (5/10/20k), High Atten | Knobs are 0–10 like the hardware |
| 7 | Compressor | 3:1 / 4:1, Fast / Slow release, Threshold | 0.5 ms attack, program-dependent auto release (Slow = 4× longer), automatic makeup gain (half the gain reduction a full-scale signal would get) |
| 8 | Tape saturation | Overdrive on/off, Saturation (starts at 0) | Asymmetric tape-style soft clipping (up to +18 dB drive, level-compensated), small 90 Hz head bump and top-end roll-off. With Overdrive on, the tape runs flat out (+36 dB drive) and the Saturation knob becomes a dry/wet mix |
| 9 | Delay | Time switch (1/8, 1/4, 1/2), Mix (starts at 0) | Echoes at full level; Mix is an equal-power dry/wet blend. Echoes duck 3 dB while the vocal is coming in (much lighter than the reverb's 12 dB) and swell back in the gaps (same ducker as the reverb; the repeats keep going underneath). Synced to the project tempo, 35% feedback, repeats filtered 150 Hz–6 kHz |
| 10 | Reverb | Type (Room, Hall, Plate [default], Chamber, Shimmer), Mix (starts at 0) | 8-line feedback delay network with pre-delay and diffusion. The reverb (not the dry vocal) always goes through a fixed 250 Hz high-pass and 6 kHz low-pass, plus a -1.5 dB mid dip at 1.5 kHz (Bass/Baritone), 2.1 kHz (Tenor/Alto) or 3 kHz (Mezzo/Soprano). Pre-delay and length follow the song tempo (at 120 BPM: Room ~8 ms / ~1 s, darker; Hall ~31 ms / ~3 s; Plate ~16 ms / ~2.5 s, bright; Chamber ~23 ms / ~2 s; Shimmer ~31 ms / ~4 s with an octave-up copy of the tail fed back in). Full Mix is a big, obvious reverb. Ducks 12 dB while the vocal is coming in (adapts to the input level) and swells back over a quarter of the Timing note in the gaps. Mix adds reverb on top of the untouched dry signal |
| 11 | Filters | High pass (20 Hz–1 kHz), Low pass (2–20 kHz) | 24 dB/octave Butterworth each; default fully open (20 Hz / 20 kHz) |

Click a stage's name or its LED to turn it on (gold) or off (grey). Clicking, dragging or scrolling any control in a stage that is off turns it on. All stages start OFF and all thresholds start at 0 dB.

**Top bar**
- **PRESET** menu + **SAVE**: "-init-" at the top resets every setting to its default (bypass untouched), then 10 user slots, shared by every project and DAW. Pick a slot to load it; SAVE asks for a name and a slot. Files live in `%APPDATA%\fxnkycat vox\Presets` (Windows) or `~/Library/Application Support/fxnkycat vox/Presets` (Mac).
- **Colour arrow** (right of SAVE): accent colour - Gold, Red, Blue, Green or Purple. Shared by every copy of the plugin and remembered (saved next to the presets).
- **BYPASS**: turns the whole plugin off (input passes straight through, with a short crossfade so it never clicks). Also works as the DAW's own bypass for the plugin. Presets don't change it.
- **AUTO GAIN**: trims the output so it stays about as loud as the input (~1 s average, ±18 dB, holds during silence).
- **INPUT HPF**: high-passes the signal before stage 1 (Off, or 21–500 Hz, 24 dB/octave). The analyzer's BEFORE trace is taken ahead of it.
- **VOCAL RANGE**: Bass/Baritone, Tenor/Alto (default), Mezzo/Soprano. Sets the detection ranges of stages 2, 3 and 4.
- **COMP/EQ SWAP** light: off (default) = Pultec EQ is stage 6, then the Compressor (7); on = Compressor first, then the EQ. The two panels trade places to match.
- **TIMING** and the project tempo readout.

A spectrum analyzer across the top overlays the plugin's input (BEFORE, grey) and output (AFTER, gold), 20 Hz–20 kHz, -96 to 0 dB.
The window is resizable from the bottom-right corner.

## Build (Windows)

Needs: Visual Studio 2022 (or Build Tools) with "Desktop development with C++", and CMake 3.22+.
JUCE is downloaded automatically on the first configure.

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build\VocalChain_artefacts\Release\VST3\fxnkycat vox.vst3`.
Copy it into `C:\Program Files\Common Files\VST3\` and rescan plugins.
If the project folder path is very long, build from a short path (e.g. `subst V: <folder>`), or MSBuild fails with FTK1011.

## Build (Mac) — VST3 + AU, universal (Apple Silicon + Intel)

### Option A: GitHub Actions (no Mac needed)

1. Put this folder in a GitHub repository (the `.github/workflows/build-mac.yml` file must be at the repo root).
2. On GitHub, open **Actions → Build Mac → Run workflow** (it also runs on every push to `main`).
3. When it finishes (~10–15 min), download **fxnkycat-vox-mac** from the run's **Artifacts** section.
   Unzip it once to get `fxnkycat-vox-mac.zip` — share that file.

### Option B: on a Mac

Needs Xcode (from the App Store) and CMake (`brew install cmake`).

```
cmake -B build -G Xcode
cmake --build build --config Release
```

Output:
- `build/VocalChain_artefacts/Release/VST3/fxnkycat vox.vst3`
- `build/VocalChain_artefacts/Release/AU/fxnkycat vox.component`

### Gatekeeper

The Mac build is ad-hoc signed, not notarized, so users must run the `xattr` commands in
`installer/mac/HOW TO INSTALL.txt` once. Removing that step needs an Apple Developer account ($99/yr)
to sign with a Developer ID certificate and notarize.

## Files

- `Source/DSP.h` — all the audio processing
- `Source/PluginProcessor.*` — parameters and the chain order
- `Source/PluginEditor.*` — the plugin window
- `Resources/logo.png` — analyzer watermark
- `installer/windows`, `installer/mac` — install notes shipped in the zips
- `.github/workflows/build-mac.yml` — Mac build on GitHub
