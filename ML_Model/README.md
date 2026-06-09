# EchoSafe - Real-World Feature Collection & Training

## Overview

This system eliminates train/test distribution mismatch by collecting features **directly from your ESP32** and training on that exact data. No more librosa/ESP32 feature incompatibility!

## Hardware Requirements

- **ESP32-S3-N16R8** (16MB Flash, 8MB PSRAM)
- **I2S MEMS Microphone** (ICS-43434 or INMP441)
- **Speaker** for playing training sounds
- **USB cable** for serial communication

## Software Requirements

```bash
# Python dependencies
pip install pyserial numpy tensorflow scikit-learn matplotlib sounddevice soundfile

# Arduino IDE with ESP32 board support
# https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html
```

## Complete Workflow

### Phase 1: Setup

#### 1.1 Hardware Connections

Connect your I2S microphone to ESP32:
```
Microphone    ESP32-S3
----------    --------
WS (LRCLK) -> GPIO 42
SD (DOUT)  -> GPIO 41
SCK (BCLK) -> GPIO 2
VDD        -> 3.3V
GND        -> GND
```

⚠️ **Adjust pin numbers** in `echosafe_feature_collector.ino` if your board uses different pins.

#### 1.2 Upload Firmware

1. Open `echosafe_feature_collector.ino` in Arduino IDE
2. Select board: **ESP32S3 Dev Module**
3. Configure settings:
   - Flash Size: 16MB
   - PSRAM: OPI PSRAM
   - Upload Speed: 921600
4. Upload to your ESP32

#### 1.3 Verify Setup

Open Serial Monitor (115200 baud). You should see:
```
=== EchoSafe Feature Collector ===
Initializing...
✓ I2S microphone initialized
✓ Mel filters and DCT matrix initialized

Ready for data collection!
```

---

### Phase 2: Download Training Sounds

#### 2.1 List Recommended Categories

```bash
python download_sounds.py --dataset list
```

This shows sound categories ideal for safety/wearable devices:
- Emergency sounds (sirens, alarms, glass breaking)
- Human sounds (crying, coughing, laughing)
- Environmental alerts (dog bark, doorbell, knocking)
- Background noise (silence, rain, typing)

#### 2.2 Download ESC-50 Dataset

```bash
python download_sounds.py --dataset esc50 --output sounds/
```

This downloads and organizes 2,000 environmental sounds into categories.

**Alternative**: Download specific datasets

- **UrbanSound8K**: https://urbansounddataset.weebly.com/urbansound8k.html
- **Freesound.org**: https://freesound.org/ (search and download individual sounds)
- **AudioSet**: https://research.google.com/audioset/ (requires preprocessing)

#### 2.3 Record Custom Sounds (Recommended!)

For best real-world performance, record sounds in your actual environment:

```bash
# Record ambient silence/background noise
python play_sound.py --record-silence --silence-duration 2 --silence-output silence_001.wav

# Organize custom sounds
python download_sounds.py --dataset custom --custom-name "my_alarm"
# Place your .wav files in: sounds/custom_recordings/my_alarm/
```

**Tips**:
- Record 20-30 samples per category
- Vary distance from microphone (near, medium, far)
- Include different volumes
- Add background noise variations
- **Always include a "silence" class** with just ambient room noise

---

### Phase 3: Collect Features from ESP32

#### 3.1 Setup Serial Logger

```bash
# Find your ESP32 port
# Linux: /dev/ttyUSB0 or /dev/ttyACM0
# Mac: /dev/cu.usbserial-*
# Windows: COM3, COM4, etc.

# Start logger
python serial_logger.py --port /dev/ttyUSB0 --output echosafe_dataset.npz
```

#### 3.2 Interactive Collection Workflow

The serial logger provides an interactive interface:

```
🎯 ECHOSAFE INTERACTIVE DATA COLLECTION

Commands:
  'c' - Capture one sample
  'a' - Auto-capture mode (prompts for label after each capture)
  's' - Save dataset and show stats
  'q' - Save and quit
```

**Recommended workflow**:

1. **Test your setup**:
   ```
   📥 Command: c
   ```
   The ESP32 will capture audio and extract features. You'll be prompted to label it.

2. **Start auto-capture mode**:
   ```
   📥 Command: a
   ```

3. **Play a sound** (in another terminal):
   ```bash
   # Play single sound
   python play_sound.py --sound sounds/playback_sounds/dog_bark/1-30226-A-0.wav
   ```

4. **Label the sound**:
   ```
   🏷️  Label this sample:
   Existing classes:
      1. dog_bark (5 samples)
      2. silence (10 samples)
   
   Enter class name: dog_bark
   ```

5. **Continue until you have 20-30 samples per class**

#### 3.3 Efficient Batch Collection

Use the interactive sound player for systematic collection:

```bash
# Terminal 1: Serial logger
python serial_logger.py --port /dev/ttyUSB0

# Terminal 2: Interactive sound player
python play_sound.py --interactive sounds/playback_sounds
```

The player will:
- Show all available categories
- Play all sounds in a category with pauses
- Allow you to set volume, pause duration, and repeat count

**Example session**:
```
📂 Available Categories:
   1. dog_bark (40 files)
   2. siren (40 files)
   3. glass_breaking (40 files)

📥 Select category: 1
   Pause between sounds (seconds): 3
   Volume (0.0-1.0): 0.8
   Repeat count: 1
```

For each sound played, label it in Terminal 1.

#### 3.4 Collect Silence/Background Noise

**Critical**: Always collect a "silence" class!

```bash
# In Terminal 1 (serial logger), press 'a' for auto-capture
# In Terminal 2, record and play silence
python play_sound.py --record-silence --silence-duration 2 --silence-output silence_001.wav
python play_sound.py --sound silence_001.wav

# Label as "silence" in Terminal 1
# Repeat 20-30 times with different ambient conditions
```

#### 3.5 Monitor Progress

Regularly save and check your dataset:

```
📥 Command: s
```

This shows:
```
📊 Dataset Statistics:
   Total samples: 156
   Classes: 6

   Samples per class:
      dog_bark: 28
      siren: 25
      glass_breaking: 30
      silence: 32
      car_horn: 21
      alarm: 20
```

**Target**: 20-30 samples per class minimum. More is better!

---

### Phase 4: Train Model on ESP32 Features

#### 4.1 Train the Model

```bash
python train_on_esp32_features.py \
    --dataset echosafe_dataset.npz \
    --output trained_models \
    --epochs 100 \
    --batch-size 32 \
    --hidden 128 64 \
    --dropout 0.3
```

**Parameters**:
- `--epochs`: Training iterations (100 is usually good)
- `--batch-size`: Samples per update (32 works well)
- `--hidden`: Hidden layer sizes (128, 64 is a good start)
- `--dropout`: Regularization (0.3 prevents overfitting)

#### 4.2 Monitor Training

You'll see:
```
📂 Loading dataset: echosafe_dataset.npz
   ✓ Loaded 156 samples
   Classes: 6
   Feature shape: (31, 13)

🔀 Splitting data...
   ✓ Train: 109 samples
   ✓ Val: 15 samples
   ✓ Test: 32 samples

🏗️  Building model...
   Input dimension: 403
   Hidden layers: [128, 64]
   Output classes: 6

🎯 Training model...
Epoch 1/100
...
Epoch 42/100
...

📊 Evaluating on test set...
   Test Accuracy: 93.75%

   Per-class accuracy:
      dog_bark: 100.0% (6 samples)
      siren: 85.7% (7 samples)
      silence: 100.0% (8 samples)
      ...
```

#### 4.3 Review Results

Training generates:
- **Model file**: `trained_models/echosafe_model_TIMESTAMP.h5`
- **Metadata**: `trained_models/echosafe_model_TIMESTAMP_metadata.json`
- **Scaler**: `trained_models/echosafe_model_TIMESTAMP_scaler.npz`
- **Training plot**: `trained_models/training_history_TIMESTAMP.png`

Check the training plot to ensure:
- ✅ Training and validation accuracy both increase
- ✅ Training and validation loss both decrease
- ✅ No large gap between train/val (indicates overfitting)

**If accuracy is low (<85%)**:
- Collect more samples (aim for 30-50 per class)
- Ensure high-quality audio (mic close to speaker)
- Verify labels are correct
- Add more "silence" samples with varied backgrounds
- Try different hidden layer sizes: `--hidden 256 128 64`

---

### Phase 5: Deploy to ESP32

#### 5.1 Export Model to C Code

```bash
python train_on_esp32_features.py \
    --dataset echosafe_dataset.npz \
    --export-c
```

This generates `model_weights_TIMESTAMP.h` with all weights as C arrays.

#### 5.2 Create Inference Firmware

Create a new Arduino sketch that:
1. Includes the generated weights
2. Loads the scaler parameters
3. Runs inference on new audio

**Pseudocode**:
```cpp
#include "model_weights_20260211_143022.h"

// Load scaler from Python (mean, scale)
float scaler_mean[403] = {...};
float scaler_scale[403] = {...};

void loop() {
    // Capture audio
    capture_audio();
    
    // Extract MFCC features
    extract_mfcc_features();
    
    // Flatten: (31, 13) -> (403,)
    float features_flat[403];
    flatten_features(features_flat);
    
    // Standardize using scaler
    for (int i = 0; i < 403; i++) {
        features_flat[i] = (features_flat[i] - scaler_mean[i]) / scaler_scale[i];
    }
    
    // Run inference
    float output[NUM_CLASSES];
    mlp_forward(features_flat, output);
    
    // Get predicted class
    int predicted_class = argmax(output);
    
    Serial.print("Detected: ");
    Serial.println(class_names[predicted_class]);
}
```

#### 5.3 Test Inference

Upload inference firmware and test with real sounds:

```
Detected: silence
Detected: silence
Detected: dog_bark
Detected: dog_bark
Detected: dog_bark
Detected: silence
```

---

## Troubleshooting

### Issue: Low Training Accuracy

**Solutions**:
- Collect more samples (30-50 per class)
- Ensure microphone is working properly
- Check that sounds are audible and clear
- Add data augmentation (volume variations, background noise)
- Try different model architectures: `--hidden 256 128 64`

### Issue: Serial Logger Not Receiving Data

**Solutions**:
- Check USB connection
- Verify correct port: `ls /dev/tty*` (Linux/Mac)
- Ensure baud rate matches (115200)
- Press reset button on ESP32
- Check Serial Monitor for errors

### Issue: Features Look Wrong

**Symptoms**: All zeros, NaN values, very large numbers

**Solutions**:
- Verify microphone wiring (check pins!)
- Test microphone: press 'c' and speak loudly
- Check I2S initialization in Arduino Serial Monitor
- Reduce speaker volume (clipping causes issues)
- Ensure PSRAM is enabled in board settings

### Issue: ESP32 Runs Out of Memory

**Solutions**:
- Reduce `NUM_FRAMES` (31 → 20)
- Reduce `NUM_MFCC` (13 → 10)
- Enable PSRAM in board settings
- Use smaller model architecture

### Issue: Model Overfits (Train: 99%, Val: 60%)

**Solutions**:
- Increase dropout: `--dropout 0.5`
- Reduce model size: `--hidden 64 32`
- Collect more diverse training data
- Add regularization (L2)

---

## Best Practices

### Data Collection
✅ **Do**:
- Collect in your actual deployment environment
- Include varied distances, volumes, backgrounds
- Record 20-30 samples minimum per class
- Always include a "silence" class
- Label consistently

❌ **Don't**:
- Use only one recording distance
- Skip the "silence" class
- Forget to vary volume/background noise
- Mix up labels

### Model Training
✅ **Do**:
- Start simple (2 hidden layers)
- Use early stopping
- Monitor both train and validation accuracy
- Test on held-out data

❌ **Don't**:
- Overfit to training data
- Use all data for training (save some for testing!)
- Ignore class imbalance

### Deployment
✅ **Do**:
- Test thoroughly before deployment
- Monitor inference latency
- Add confidence thresholds (e.g., only trigger if >90% confident)
- Log misclassifications for future improvement

❌ **Don't**:
- Deploy without testing
- Ignore edge cases
- Skip power optimization

---

## Example End-to-End Session

```bash
# 1. Setup
arduino-cli compile --fqbn esp32:esp32:esp32s3 echosafe_feature_collector.ino
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32s3

# 2. Download sounds
python download_sounds.py --dataset esc50 --output sounds/

# 3. Collect features (Terminal 1)
python serial_logger.py --port /dev/ttyUSB0 --output my_dataset.npz

# 4. Play sounds (Terminal 2)
python play_sound.py --interactive sounds/playback_sounds

# 5. Train model
python train_on_esp32_features.py \
    --dataset my_dataset.npz \
    --epochs 100 \
    --export-c

# 6. Deploy to ESP32
# (Copy generated .h file to Arduino project and create inference sketch)
```

---

## File Reference

| File | Purpose |
|------|---------|
| `echosafe_feature_collector.ino` | ESP32 firmware for feature extraction |
| `serial_logger.py` | Capture and label features from ESP32 |
| `download_sounds.py` | Download and organize training sounds |
| `play_sound.py` | Play sounds for microphone capture |
| `train_on_esp32_features.py` | Train model on collected features |

---

## Next Steps

After completing this workflow:

1. **Improve accuracy**: Collect more samples, especially for confused classes
2. **Add new classes**: Just collect more features and retrain
3. **Optimize for ESP32**: Quantize model, reduce size, optimize inference
4. **Deploy production**: Add confidence thresholds, error handling, power management
5. **Continuous learning**: Periodically collect new samples and retrain

---

## Support

For issues or questions:
- Check the troubleshooting section
- Review Arduino Serial Monitor for ESP32 errors
- Verify Python dependencies: `pip list`
- Ensure correct hardware connections

**Happy feature collecting! 🎤🤖**
