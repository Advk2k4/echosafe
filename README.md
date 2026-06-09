# EchoSafe

A wearable real-time sound classification device for individuals with hearing impairments.

An ESP32-S3 continuously listens via MEMS microphones, classifies environmental sounds using an on-device MLP neural network, and alerts the wearer through a speaker (MAX98357A) and haptic motors (DRV2605 × 4) when a target sound is detected. Direction of the sound is estimated via TDOA (Time Difference of Arrival) across four microphones.

## How It Works

The key design principle is **zero domain mismatch**: MFCC features are extracted on the ESP32 itself (not in Python), so training data and inference data share the exact same feature distribution. This avoids the accuracy degradation typical of training on librosa/Python features and deploying on embedded hardware.

**Pipeline:**
1. ESP32 captures audio at 16 kHz via I2S MEMS mic
2. On-device MFCC extraction: 31 frames × 13 coefficients = 403 features per sample
3. Python collects labeled feature vectors over serial → `echosafe_dataset.npz`
4. MLP trained in Python (403 → 128 → 64 → N classes), weights exported as a C header
5. Inference firmware loads the header, runs the MLP, and plays a WAV alert when confidence > 65%

**Current model:** 97.5% test accuracy, 5 classes (horns, noise, bells, gunshots, sirens), 1059-sample dataset collected entirely on-device.

## Repository Structure

```
ML_Model/                          ← ML pipeline and firmware
├── echosafe_feature_collector/    ← Arduino: data collection firmware (serial MFCC streaming)
├── echosafe_inference/            ← Arduino: single-mic inference + speaker alert
├── echosafe_full_system/          ← Arduino: 4-mic + haptics production firmware
├── serial_logger.py               ← Collect and label MFCC samples from ESP32
├── retrain.py                     ← Quick retrain → writes model_weights.h directly
├── train_on_esp32_features.py     ← Parametric training with full output artifacts
├── play_sound.py                  ← Play training sounds for mic capture
├── download_sounds.py             ← Download ESC-50 dataset
├── quick_start.py                 ← First-time setup verification and guided walkthrough
├── mic_oldML.c                    ← Reference: 4-mic TDOA + DRV2605 haptic driver logic
└── uploadLittleFS.ino             ← WiFi web UI for LittleFS inspection (dev utility)
```

## Hardware

**ESP32-S3-N16R8** (16MB Flash, 8MB OPI PSRAM)

| Peripheral | Signal | GPIO |
|---|---|---|
| Mic (I2S_NUM_0) | WS/SD/SCK | 5 / 6 / 7 |
| Speaker MAX98357A | DIN/BCLK/LRC | 47 / 48 / 45 |
| I2C (TCA9548A + DRV2605 × 4) | SDA/SCL | 9 / 11 |

Full 4-mic wiring and pin mapping documented in [ML_Model/CLAUDE.md](ML_Model/CLAUDE.md).

## Quickstart

```bash
# Install Python dependencies
pip install pyserial numpy tensorflow scikit-learn matplotlib sounddevice soundfile

# Verify setup and guided first collection
python ML_Model/quick_start.py

# Download training sounds (ESC-50 dataset)
python ML_Model/download_sounds.py --dataset esc50 --output ML_Model/sounds/

# Collect features from ESP32 (Terminal 1)
python ML_Model/serial_logger.py --port /dev/cu.usbmodem101 --output echosafe_dataset.npz

# Play sounds for capture (Terminal 2)
python ML_Model/play_sound.py --interactive ML_Model/sounds/playback_sounds

# Train and deploy
python ML_Model/retrain.py
# Flash echosafe_inference/ to ESP32 via Arduino IDE
```

See [ML_Model/README.md](ML_Model/README.md) for the complete step-by-step workflow.

## Arduino IDE Settings

- Board: **ESP32S3 Dev Module**
- Flash Size: **16MB (128Mb)**
- PSRAM: **OPI PSRAM**
- Partition Scheme: **Custom** (use `partitions.csv` from the firmware folder)
- Upload Speed: **921600**
