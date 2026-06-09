# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

EchoSafe is a wearable real-time sound classification device for individuals with hearing impairments. An ESP32-S3 listens via a MEMS microphone, classifies sounds with an on-device MLP neural network, and plays an alert WAV through a speaker (MAX98357A) when a target sound is detected. The full system will add 3 additional directional microphones (TDOA-based direction detection) and haptic motors (DRV2605 × 4 via TCA9548A I2C mux) — but the **ML classification pipeline is the current focus**.

The core design principle: MFCC features are extracted **on the ESP32** (not in Python) so training data and inference data share the exact same feature distribution — zero domain mismatch.

**Team repo:** `https://github.com/umass-ece-sdp/sdp26-22`  
Local working directory (`EchoSafe_ML/`) maps to `software/ML/` in the repo.

**Current 5 sound classes:** `horns` (0), `noise` (1), `bells` (2), `gunshots` (3), `sirens` (4)

---

## Install Dependencies

```bash
pip install pyserial numpy tensorflow scikit-learn matplotlib sounddevice soundfile
```

---

## Key Commands

### Data Collection (two terminals required)

```bash
# Terminal 1: Collect and label features from ESP32 via serial
python serial_logger.py --port /dev/cu.usbmodem101 --output echosafe_dataset.npz

# Terminal 2: Play sounds through speaker for ESP32 to capture
python play_sound.py --interactive sounds/playback_sounds
```

### Training

```bash
# Quick retrain — reads echosafe_dataset.npz, writes echosafe_inference/model_weights.h directly
python retrain.py

# Parametric training — more control, saves .h5 model + scaler + metadata to trained_models/
python train_on_esp32_features.py \
    --dataset echosafe_dataset.npz \
    --output trained_models \
    --epochs 150 \
    --batch-size 32 \
    --hidden 128 64 \
    --dropout 0.3 \
    --export-c
```

### Sound Setup

```bash
# Download ESC-50 dataset (~2000 sounds) organized into sounds/playback_sounds/
python download_sounds.py --dataset esc50 --output sounds/
```

### First-Time Setup Verification

```bash
# Check all dependencies and walk through initial collection interactively
python quick_start.py
```

---

## Architecture

### Data Pipeline

1. `echosafe_feature_collector/echosafe_feature_collector.ino` — runs on ESP32-S3; captures audio via I2S MEMS mic (GPIO 5/6/7), extracts MFCC features on-device (31 frames × 13 coefficients = 403 values), streams over serial at 115200 baud. Serial commands: `c` (capture one), `r` (continuous), `s` (stop).
2. `serial_logger.py` — reads the serial stream, prompts for a label, accumulates samples into `echosafe_dataset.npz` (keys: `features`, `labels`, `label_map`, `metadata`).
3. `retrain.py` / `train_on_esp32_features.py` — load the `.npz`, flatten features to `(N, 403)`, apply `StandardScaler`, train a `Sequential` MLP (403 → 128 → 64 → N_classes), emit C header arrays for firmware.

### Model & Feature Format

MFCC extraction parameters (identical on-chip and in Python training):

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

- Feature tensor per sample: `(31, 13)` → flattened to `(403,)` before the MLP.
- `StandardScaler` (zero mean, unit variance) fit on train split; its 403-element mean/scale arrays are embedded in `model_weights.h` so normalisation is bit-identical at inference time.

### Dataset

Current `echosafe_dataset.npz` — **1059 samples**, 5 classes (all collected on ESP32):

| Class | Index | Samples |
|---|---|---|
| horns | 0 | 201 |
| noise | 1 | 258 |
| bells | 2 | 200 |
| gunshots | 3 | 200 |
| sirens | 4 | 200 |

Note: `echosafe_dataset.npz` is **not committed to git** (large binary). Keep it safe locally.

### Model

MLP: **403 → 128 → Dropout(0.3) → 64 → Dropout(0.3) → 5 (softmax)**  
Training config: Adam (lr=1e-3), categorical cross-entropy, max 150 epochs, early stopping patience=20, ReduceLROnPlateau (factor=0.5, patience=7), `compute_class_weight('balanced')`.

### Current Model Performance

Overall test accuracy: **97.5%** (trained on 1059-sample dataset, Feb 2026)  
Classes: horns / noise / bells / gunshots / sirens  
Weights live in: `echosafe_inference/model_weights.h` (auto-generated, do not edit manually).

---

## Firmware

### `echosafe_feature_collector/echosafe_feature_collector.ino`
Data collection firmware. Captures audio at 16kHz, extracts MFCCs on-chip, streams feature vectors over serial. Use this to build the dataset.

### `echosafe_inference/echosafe_inference.ino`
**Production inference firmware** — single microphone + speaker.  
- Reads `model_weights.h`, runs the MLP, plays back a WAV alert via MAX98357A.  
- LittleFS stores WAV files; uses chunk-scanning WAV parser (`parse_wav_chunks`) for non-standard metadata chunks.
- Mono WAV files are duplicated L+R for stereo I2S output.
- Serial commands: `i` (single inference), `r` (continuous), `s` (stop), `p` (test all speaker sounds), `t` (440Hz tone test).
- Requires custom partition table (Tools → Partition Scheme → Custom) with `partitions.csv`.

### `echosafe_inference/partitions.csv`
Custom 16MB partition table. Required because the default `huge_app` scheme only gives 896KB for LittleFS (too small for WAV files).
```
nvs      0x9000   20KB
otadata  0xe000    8KB
app0     0x10000   3MB   ← enough for model_weights.h + firmware
coredump 0x310000 64KB
spiffs   0x320000 12.8MB ← LittleFS: all WAV files fit easily
```
**Arduino IDE settings:** ESP32S3 Dev Module, Flash 16MB (128Mb), PSRAM OPI, Partition Scheme: Custom, Upload 921600.

### `echosafe_inference/data/`
WAV alert files to upload to LittleFS (use Arduino LittleFS Filesystem Uploader plugin):
- `vehiclehorn.wav` → class 0 (horns)
- `bicyclebell.wav` → class 2 (bells)
- `gun_shot.wav` → class 3 (gunshots)
- `emergencysiren.wav` → class 4 (sirens)

### `echosafe_full_system/echosafe_full_system.ino`
**Production full-system firmware** — 4 mics + 2 speakers + 4 haptic motors.
- Three-phase operation: Phase 1 ML inference (I2S_NUM_0 @ 16kHz, TL mic), Phase 2 TDOA direction detection (both I2S buses @ 44.1kHz), Phase 3 alert playback (I2S_NUM_1 TX, shares bus with BOT mics).
- Requires `Adafruit DRV2605 Library` (Library Manager).
- Shares `model_weights.h` with `echosafe_inference` via relative `#include`.
- Serial commands: `i` (inference), `r` (continuous), `s` (stop), `h` (haptic test), `d` (direction test), `t` (tone test), `p` (speaker test).
- Uses the same `partitions.csv` scheme as `echosafe_inference`.

### `uploadLittleFS.ino`
WiFi-based web file manager for LittleFS — serves a browser UI to inspect LittleFS contents over WiFi. **Credentials must be set before use** (replace `ST_SSID`/`ST_PASS` with your network). Development/debug utility only; do not deploy in production.

### `mic_oldML.c`
Reference file for the full-project hardware — contains 4-mic TDOA direction detection (I2S ports, pin mapping) and DRV2605 haptic driver logic (TCA9548A I2C mux). **Do not delete.** This will be the basis for integrating direction + haptics into the unified firmware.

---

## Hardware Pin Mapping (ESP32-S3-N16R8)

### Current (single-mic inference):
| Peripheral | Signal | GPIO |
|---|---|---|
| Microphone (I2S_NUM_0 RX) | WS | 5 |
| Microphone | SD | 6 |
| Microphone | SCK | 7 |
| Speaker MAX98357A (I2S_NUM_1 TX) | DIN | 47 |
| Speaker | BCLK | 48 |
| Speaker | LRC | 45 |

### Full system (`echosafe_full_system.ino` + `mic_oldML.c`):
| Peripheral | Signal | GPIO |
|---|---|---|
| MIC_TOP (I2S_NUM_0 RX) | WS | 4 |
| MIC_TOP | BCLK | 5 |
| MIC_TOP | DOUT | 16 |
| MIC_BOT (I2S_NUM_1 RX/TX shared) | WS | 12 |
| MIC_BOT | BCLK | 21 |
| MIC_BOT | DOUT | 18 |
| Speaker 1+2 (I2S_NUM_1 TX) | DIN | 47 |
| Speaker | BCLK | 48 |
| Speaker | LRC | 45 |
| I2C (TCA9548A + DRV2605 × 4) | SDA | 9 |
| I2C | SCL | 11 |

---

## Important Constraints

- Always include a `noise` class; omitting it causes every background sound to be misclassified as a target.
- Label names must be consistent (snake_case) across all collection sessions.
- The C symbol names in `model_weights.h` (`LAYER0_WEIGHTS`, `LAYER2_WEIGHTS`, `LAYER4_WEIGHTS`, `SCALER_MEAN`, `SCALER_SCALE`, etc.) are hardcoded in `echosafe_inference.ino`. Do not rename without updating both files.
- Confidence threshold: `CONFIDENCE_DEFAULT 0.65f` — predictions below this → treated as noise, no alert.
- `model_weights.h` is auto-generated by `retrain.py`. Never edit manually.
- `echosafe_dataset.npz` is not in git (too large). It must be present locally to retrain.

---

## Git / Repo Notes

- Team repo: `https://github.com/umass-ece-sdp/sdp26-22`
- Local `EchoSafe_ML/` corresponds to `software/ML/` in the repo.
- To push local changes to the repo: copy updated files into a clone of sdp26-22 at `software/ML/`, then commit and push from there.
- Files excluded from git (`.gitignore`): `*.npz`, `trained_models/*.h5`, `*.pyc`, `.DS_Store`, Arduino build artifacts.
