# CLAUDE.md

This file provides guidance to Claude Code when working in this repository.

## Project Overview

EchoSafe is a wearable, around-the-ear sound-alert device for people who are
deaf or hard of hearing, shaped like a Shokz-style bone-conduction headset.
An ESP32-S3 listens via 4 MEMS microphones (one per quadrant around the
head), classifies target sounds on-device with an MLP, and fires directional
haptic motors so the wearer knows WHAT sound occurred and WHICH direction it
came from.

**This repo is a consolidated, cleaned-up successor to several scattered
folders** (`EchoSafe_ML`, `EchoSafe_RevA`, `EchoSafe_Audio`, `EchoSafeTesting`
under `~/Documents/`). See `CONSOLIDATION_NOTES.md` for what was brought in,
what was left behind, and why. The originals were NOT deleted — treat them
as a read-only safety net, not a second source of truth going forward.

**Team repo:** `https://github.com/umass-ece-sdp/sdp26-22` (private). Do not
push here without asking first, every time.

**Current 5 sound classes:** `horns`(0), `noise`(1), `bells`(2), `gunshots`(3), `sirens`(4)

---

## Current Status (as of this consolidation)

- **Firmware:** `firmware/echosafe_full_system/` is the target build (4 mics
  + 2 speakers + 4 haptics) but its confirmed-working status is UNVERIFIED —
  nobody has confirmed it runs end-to-end on real hardware recently.
- **TL-mic workaround applied:** the firmware's own comments documented the
  top-left (TL) mic reading 0.0000 RMS (dead channel) on last contact.
  `ML_MIC_CHANNEL` in `firmware/echosafe_full_system/echosafe_full_system.ino`
  has been switched to `I2S_CHANNEL_FMT_ONLY_RIGHT` (TR mic) to match. This
  is a software workaround, not a hardware fix — confirm during bring-up
  whether TL is actually still dead (serial output prints per-mic RMS during
  the `d` direction-test command); if it's fine now, flipping back to
  `ONLY_LEFT` is one line.
  **Also note:** `model_weights.h` was trained on audio from the single-mic
  reference rig (`echosafe_feature_collector.ino`, GPIO 5/6/7) — a different
  physical mic than either TL or TR on the array. Neither channel is a
  perfect distribution match for the trained model; retraining on audio
  captured from whichever channel is actually used is the real long-term
  fix, this workaround is a stopgap to get the system running.
  **Direction detection is not covered by this workaround** — TDOA reads
  all 4 mics regardless of `ML_MIC_CHANNEL`, so if TL is genuinely dead,
  direction detection will still be degraded (that quadrant will rarely/
  never be selected correctly) until the hardware fault itself is fixed.
- **Hardware:** `hardware/EchoSafe_RevA/` has a complete, detailed KiCad
  *schematic* (ESP32-S3-WROOM-1, 4× ICS-43434 mics, 2× MAX98357A, 4×
  DRV2605L haptics via TCA9548A mux, a power management IC) but the
  **PCB layout was never started** — 0 footprints placed, 0 traces routed,
  nothing fabricated. Any bring-up right now has to happen on a breadboard
  or dev-kit with jumper wiring, following the schematic's part choices and
  the pin mapping documented below.

---

## Firmware

### `firmware/echosafe_full_system/echosafe_full_system.ino` — PRIMARY TARGET
Full system: 4 mics, 2 speakers (parallel, time-shared I2S bus), 4 haptic
motors. Three-phase loop: ML inference (Phase 1) → TDOA direction detection
(Phase 2) → alert playback (Phase 3).

Includes its model via a **relative path** —
`#include "../echosafe_inference/model_weights.h"` — so it depends on
`firmware/echosafe_inference/` staying a sibling folder. Do not move one
without the other. (A duplicate `model_weights.h` also sits directly in this
folder from the original project layout; it's unused dead weight, byte-
identical to the one actually included — harmless to ignore or remove.)

Serial commands: `i` (single inference), `r` (continuous), `s` (stop),
`h` (haptic test — fires all 4 motors in sequence), `d` (direction test —
prints per-mic RMS + detected quadrant), `t` (440Hz tone test),
`p` (speaker test — cycles all WAV alerts).

Requires the **Adafruit DRV2605 Library** (Arduino Library Manager) and the
custom `partitions.csv` in this folder (default Arduino partition scheme is
too small for the WAV alert files on LittleFS).

### `firmware/echosafe_inference/echosafe_inference.ino` — MINIMAL BRING-UP REFERENCE
Deliberately stripped down: single mic (I2S_NUM_0) + MLP inference + Serial
print only. **No speaker, no LittleFS, no WAV playback.** Auto-starts in
continuous classification mode on boot. Commands: `i`/`r`/`s` only.

Use this FIRST when bringing up new/unfamiliar hardware — it's the simplest
possible way to confirm the mic + MLP path works before adding the
complexity of the full system.

### `firmware/echosafe_feature_collector/echosafe_feature_collector.ino`
Data collection firmware. Captures audio at 16kHz, extracts MFCCs on-chip,
streams feature vectors over serial for `ml/serial_logger.py` to capture and
label. Use this to expand the dataset.

### `firmware/mic_oldML.c`
Historical reference implementation of the 4-mic TDOA + DRV2605 haptic
logic that `echosafe_full_system.ino` evolved from. Not a buildable sketch
on its own (no `.ino` extension) — kept for reference. **Do not delete.**

### `firmware/uploadLittleFS.ino`
WiFi-based web file manager for inspecting LittleFS contents over WiFi.
**Set `ST_SSID`/`ST_PASS` before use.** Development/debug utility only — do
not deploy in production.

### `firmware/data/`
WAV alert files to upload to LittleFS (Arduino LittleFS Filesystem Uploader
plugin), used by `echosafe_full_system.ino`:
- `vehiclehorn.wav` → class 0 (horns)
- `bicyclebell.wav` → class 2 (bells)
- `gun_shot.wav` → class 3 (gunshots)
- `emergencysiren.wav` → class 4 (sirens)
- (no WAV for class 1 / `noise` — it's the reject/background class, never alerts)

---

## Hardware Pin Mapping (ESP32-S3-WROOM-1 N16R8)

### Microphones (4× ICS-43434, 2 I2S buses in stereo L/R pairs)
| Mic | Bus | Signal | GPIO |
|---|---|---|---|
| TOP pair | I2S_NUM_0 | LRCLK / WS | 4 |
| TOP pair | I2S_NUM_0 | BCLK | 5 |
| TOP pair | I2S_NUM_0 | DOUT | 16 |
| Top-Left (TL) | — | SEL | GND → LEFT channel |
| Top-Right (TR) | — | SEL | 3V3 → RIGHT channel |
| BOT pair | I2S_NUM_1 | LRCLK / WS | 12 |
| BOT pair | I2S_NUM_1 | BCLK | 21 |
| BOT pair | I2S_NUM_1 | DOUT | 18 |
| Bot-Left (BL) | — | SEL | GND → LEFT channel |
| Bot-Right (BR) | — | SEL | 3V3 → RIGHT channel |

### Speakers (2× MAX98357A, parallel, shares I2S_NUM_1 with BOT mics)
| Signal | GPIO |
|---|---|
| DIN | 47 |
| BCLK | 48 |
| LRC | 45 |
| SD (shutdown) | tie to VIN |

I2S_NUM_1 is time-multiplexed: RX for BOT mics during capture, TX for
speaker during alert playback, then reinitialized back to RX. Verify this
switch doesn't leave the peripheral in the wrong mode — this is a plausible
source of bugs if the full-system firmware turns out not to work.

### Haptics (4× DRV2605L via TCA9548A I2C mux)
| Signal | GPIO |
|---|---|
| SDA | 9 |
| SCL | 11 |
| Mux address | 0x70 (A0–A3 → GND) |
| DRV2605L address (each channel) | 0x5A |

| Mux channel | Motor |
|---|---|
| 0 | Top-Left |
| 1 | Top-Right |
| 2 | Bottom-Left |
| 3 | Bottom-Right |

### Single-mic reference (`echosafe_inference.ino` only — different wiring!)
| Signal | GPIO |
|---|---|
| WS (LRCLK) | 5 |
| SD (DOUT) | 6 |
| SCK (BCLK) | 7 |

Note this uses **different GPIOs** than the TOP mic pair in the full-system
firmware — they are not meant to be wired identically. Don't assume you can
swap firmware without re-wiring.

---

## ML Pipeline

### Feature extraction (identical on-chip and in Python training)
| Parameter | Value |
|---|---|
| Sample rate | 16,000 Hz |
| Frame size | 512 samples |
| Hop (frame step) | 256 samples (50% overlap) |
| Frames per sample | 31 |
| FFT size | 512 |
| Mel filter banks | 26 |
| MFCC coefficients | 13 |
| Pre-emphasis | 0.97 |
| Window | Hamming |

Feature tensor per sample: `(31, 13)` → flattened to `(403,)`. A
`StandardScaler` (zero mean, unit variance) fit on the training split has
its 403-element mean/scale arrays embedded directly in `model_weights.h` so
normalization is bit-identical between training and inference.

### Model
MLP: **403 → 128 → Dropout(0.3) → 64 → Dropout(0.3) → 5 (softmax)**
Test accuracy: **97.5%** (1059-sample dataset, Feb 2026).
Confidence threshold: `0.65` — predictions below this are treated as
uncertain/noise, no alert fires.

### Dataset
`ml/echosafe_dataset.npz` — **1059 samples**, 5 classes, all collected on
real ESP32-S3 hardware (not synthetic/librosa — this eliminates train/
inference domain mismatch):

| Class | Index | Samples |
|---|---|---|
| horns | 0 | 201 |
| noise | 1 | 258 |
| bells | 2 | 200 |
| gunshots | 3 | 200 |
| sirens | 4 | 200 |

Not committed to git (`.gitignore` excludes `*.npz`) — keep the local copy
safe, it's the only one.

### Key commands
```bash
pip install pyserial numpy tensorflow scikit-learn matplotlib sounddevice soundfile

# Retrain (reads echosafe_dataset.npz, writes firmware/echosafe_inference/model_weights.h)
cd ml && python retrain.py

# Parametric training with more control
python train_on_esp32_features.py \
    --dataset echosafe_dataset.npz \
    --output trained_models \
    --epochs 150 --batch-size 32 --hidden 128 64 --dropout 0.3 --export-c

# Data collection (two terminals)
python serial_logger.py --port /dev/cu.usbmodem101 --output echosafe_dataset.npz
python play_sound.py --interactive sounds/playback_sounds   # (re-download ESC-50 first, see below)

# Re-download ESC-50 sound bank (~1.6GB, not kept in this repo)
python download_sounds.py --dataset esc50 --output sounds/
```

**Important:** `retrain.py` writes to `firmware/echosafe_inference/model_weights.h`.
Since `echosafe_full_system.ino` includes that same file via relative path,
retraining updates both firmware targets automatically — you do not need to
manually copy the header anywhere.

---

## Hardware (`hardware/EchoSafe_RevA/`)

KiCad project: `EchoSafe_RevA/EchoSafe_RevA.kicad_sch` /
`.kicad_pcb` / `.kicad_pro`, with a component library in `lib/` (footprints
+ symbols for ESP-32, ICS-43434, MAX98357AETE-T, haptic motors, a power
management IC). **The schematic is real and detailed; the PCB layout was
never started** (empty `.kicad_pcb`, 0 footprints placed). `datasheets/`
and `outputs/` are currently empty.

**Phase 2 cross-check result (2026-09-11): the schematic is not yet wired
at the signal level.** Components are correctly selected and placed (ESP32-
S3-WROOM-1-N16R8, 4× ICS-43434 mics, MAX98357AETE+T, DRV2605LDGS, NPM1300
PMIC, battery), and the power rails are wired (`VBUS_5V`/`VSYS`/`VBAT`/
`1V8_MIC`/`3V3_AUDIO`/`3V3_SYS`/`GND` global labels). But there are only 21
wire segments and **zero signal-net labels** anywhere in the sheet — no I2S
(WS/BCLK/DOUT) or I2C (SDA/SCL) connections exist between the ESP32 and any
peripheral yet, so there is nothing to cross-check against the firmware pin
table above. That work hasn't been started, not just unverified.

Two component-count mismatches vs. what the firmware architecture expects,
also worth resolving before wiring:
- **Only 1× DRV2605LDGS (U3) is placed**, but the firmware drives 4
  independent haptic motors via 4 separate DRV2605 ICs behind a TCA9548A
  I2C mux (`firmware/echosafe_full_system/echosafe_full_system.ino`).
  **No TCA9548A symbol exists in the schematic at all.** 3 more driver ICs
  and the mux need to be added.
- **Only 1× MAX98357AETE+T (U2) is placed**, with 2 `Device:Speaker`
  elements (LS1, LS2) wired to it — the firmware's header comment says
  "2x MAX98357A Speakers (parallel)," which reads as 2 amp ICs. Unclear
  whether the schematic's "1 amp driving 2 parallel speakers" was the
  actual intent (electrically plausible if impedance/current headroom
  allows it) or a second amp IC is still needed — worth confirming before
  wiring, not after.

---

## Important Constraints

- Always include a `noise` class when retraining; omitting it causes every
  background sound to be misclassified as a target (learned the hard way).
- Label names must stay consistent (snake_case) across all collection
  sessions.
- `model_weights.h` is **auto-generated** by `retrain.py` — never hand-edit.
  The C symbol names (`LAYER0_WEIGHTS`, `SCALER_MEAN`, etc.) are hardcoded
  in the firmware; don't rename without updating both sides.
- `ml/echosafe_dataset.npz` is the only copy of 1059 labeled samples —
  never overwrite without a backup.
- Do not push to the team repo without asking first, every time.
- Do not delete the original `EchoSafe_ML` / `EchoSafe_RevA` /
  `EchoSafe_Audio` / `EchoSafeTesting` folders under `~/Documents/` — they're
  the safety net until this consolidated repo is validated as complete. See
  `CONSOLIDATION_NOTES.md`.
