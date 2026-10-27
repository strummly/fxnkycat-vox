# fxnkycat vox (VST3 / AU)

A 9-stage vocal processing chain in one plugin, built with JUCE 8.

| # | Stage | Controls | Fixed |
|---|-------|----------|-------|
| – | Timing (top bar) | Scrollable note value: 2 bar … 1/16t (default 1/4) | Tempo comes from the project automatically (120 BPM if the host doesn't report one). Scales the rider's timing and the compressor's auto release |
| 1 | Volume rider | Threshold, Target loudness | 1/4 note loudness window, ±12 dB max ride; below threshold = 0 dB |
| 2 | Comp-gate (classic de-ess + gate) | Threshold | Gate first (opens 40 dB under threshold, 4 dB hysteresis, 50 ms hold, closes over 150 ms to -30 dB). Then split-band de-ess: sidechain around 6 kHz, only that band is cut, 4:1, 0.5 ms / 50 ms, up to 18 dB |
| 3 | Problem EQ (resonance search) | Vocal range (Bass/Baritone, Tenor/Alto [default], Mezzo/Soprano), Threshold | Three trackers search low / mid / high ranges set by the vocal range (Bass/Baritone 120–400 Hz, 350 Hz–2 kHz, 1.8–5 kHz; Tenor/Alto 170–500 Hz, 450 Hz–2.5 kHz, 2.2–6 kHz; Mezzo/Soprano 220–650 Hz, 550 Hz–3 kHz, 2.7–7 kHz) for the frequency that sticks out, then cut it up to 8 dB |
| 4 | De-esser (sibilance search) | Detection, Reduction (both start at 0) | Scans 3–10 kHz, follows the loudest S/SH frequency (retargets in ~5 ms) and cuts a band there; 0.3 ms / 60 ms |
| 5 | Compressor | 3:1 / 4:1, Fast / Slow release, Threshold | 0.5 ms attack, program-dependent auto release (Slow = 4× longer), automatic makeup gain (half the gain reduction a full-scale signal would get) |
| 6 | Pultec EQP-1A style | Low Freq (20/30/60/100), Low Boost, Low Atten, High Freq (3/4/5/8/10/12/16k), High Boost, Bandwidth, Atten Select (5/10/20k), High Atten | Knobs are 0–10 like the hardware |
| 7 | Tape saturation | Overdrive on/off, Saturation (starts at 0) | Asymmetric tape-style soft clipping (up to +18 dB drive, level-compensated), small 90 Hz head bump and top-end roll-off. With Overdrive on, the tape runs flat out (+36 dB drive) and the Saturation knob becomes a dry/wet mix |
| 8 | Delay | Time switch (1/8, 1/4, 1/2), Mix (starts at 0) | Echoes at full level; Mix is an equal-power dry/wet blend. Synced to the project tempo, 35% feedback, repeats filtered 150 Hz–6 kHz |
| 9 | Filters | High pass (20 Hz–1 kHz), Low pass (2–20 kHz) | 24 dB/octave Butterworth each; default fully open (20 Hz / 20 kHz) |

Every stage has an ON switch. All stages start OFF and all thresholds start at 0 dB.

**Top bar**
- **PRESET** menu + **SAVE**: 10 user slots, shared by every project and DAW. Pick a slot to load it; SAVE asks for a name and a slot. Files live in `%APPDATA%\fxnkycat vox\Presets` (Windows) or `~/Library/Application Support/fxnkycat vox/Presets` (Mac).
- **AUTO GAIN**: trims the output so it stays about as loud as the input (~1 s average, ±18 dB, holds during silence).
- **INPUT HPF**: high-passes the signal before stage 1 (Off, or 21–500 Hz, 24 dB/octave). The analyzer's BEFORE trace is taken ahead of it.
- **5/6 ORDER**: COMP > EQ (default, Compressor is stage 5) or EQ > COMP. The two panels trade places to match.
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
