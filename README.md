# fxnkycat vox (VST3 / AU)

A 9-stage vocal processing chain in one plugin, built with JUCE 8.

| # | Stage | Controls | Fixed |
|---|-------|----------|-------|
| – | Timing (top bar) | Scrollable note value: 2 bar … 1/16t (default 1/4) | Tempo comes from the project automatically (120 BPM if the host doesn't report one). Scales the rider's timing and the compressor's auto release |
| 1 | Volume rider | Threshold, Target loudness | 1/4 note loudness window, ±12 dB max ride; below threshold = 0 dB |
| 2 | Comp-gate (classic de-ess + gate) | Threshold | Gate first (opens 40 dB under threshold, 4 dB hysteresis, 50 ms hold, closes over 150 ms to -30 dB). Then split-band de-ess: sidechain around 6 kHz, only that band is cut, 4:1, 0.5 ms / 50 ms, up to 18 dB |
| 3 | Problem EQ (resonance search) | Threshold, Male/Female | Three trackers search low (150–400 / 200–500 Hz), mid (400–2k / 500–2.5k) and high (2–5k / 2.5–6.5k) ranges for the frequency that sticks out, then cut it up to 8 dB |
| 4 | De-esser (sibilance search) | Detection, Reduction | Scans 3–10 kHz, follows the loudest S/SH frequency (retargets in ~5 ms) and cuts a band there; 0.3 ms / 60 ms |
| 5 | Pultec EQP-1A style | Low Freq (20/30/60/100), Low Boost, Low Atten, High Freq (3/4/5/8/10/12/16k), High Boost, Bandwidth, Atten Select (5/10/20k), High Atten | Knobs are 0–10 like the hardware |
| 6 | Compressor | 3:1 / 4:1, Threshold, Makeup | 0.5 ms attack, auto release: 1/16 note after short peaks, up to 1/2 note after long phrases |
| 7 | Tape saturation | Saturation | Asymmetric tape-style soft clipping (up to +18 dB drive, level-compensated), small 90 Hz head bump and top-end roll-off that grow with the amount |
| 8 | Delay | Time switch (1/8, 1/4, 1/2), Amount | Synced to the project tempo, 35% feedback, repeats filtered 150 Hz–6 kHz |
| 9 | Filters | High pass (20 Hz–1 kHz), Low pass (2–20 kHz) | 24 dB/octave Butterworth each; defaults 80 Hz / 18 kHz |

Every stage has an ON switch. All stages start OFF and all thresholds start at 0 dB.

An "INPUT HPF" slider in the top bar high-passes the signal before stage 1 (Off, or 21–500 Hz, 24 dB/octave). The analyzer's BEFORE trace is taken ahead of it.

A "5/6 ORDER" switch in the top bar sets the order of the Compressor and Pultec: COMP > EQ (default, Compressor is stage 5) or EQ > COMP. The two panels trade places to match.

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
