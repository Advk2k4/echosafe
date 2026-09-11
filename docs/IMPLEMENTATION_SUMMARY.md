# EchoSafe Implementation Summary

## What We've Built

A complete end-to-end system for collecting real-world audio features directly from your ESP32 and training a classifier that **perfectly matches** the deployment environment.

## Key Innovation

**Problem**: Training on librosa features ≠ ESP32 features → poor accuracy
**Solution**: Collect features FROM the ESP32 → Train on exact same distribution → Perfect match!

---

## Files Created

### 1. **echosafe_feature_collector.ino** (ESP32 Firmware)
- Captures audio from I2S MEMS microphone
- Extracts MFCC features on-device
- Outputs features via serial in structured format
- **Commands**: 
  - `c` - Capture one sample
  - `r` - Start continuous recording (every 2s)
  - `s` - Stop continuous recording

### 2. **serial_logger.py** (Data Collection)
- Connects to ESP32 via serial
- Captures feature vectors in real-time
- Prompts for labels
- Builds labeled dataset (.npz format)
- **Interactive commands**:
  - `c` - Capture one sample
  - `a` - Auto-capture mode
  - `s` - Save and show stats
  - `q` - Save and quit

### 3. **download_sounds.py** (Dataset Downloader)
- Downloads ESC-50 environmental sound dataset
- Organizes sounds by category
- Creates folders for custom recordings
- Lists recommended categories for safety/wearable devices

### 4. **play_sound.py** (Sound Playback)
- Plays sounds through speakers for mic capture
- Interactive category browser
- Batch playback with configurable pauses
- Can record ambient silence samples

### 5. **train_on_esp32_features.py** (Model Training)
- Trains on ACTUAL ESP32 features
- Automatic train/val/test split
- Feature standardization
- Exports trained model
- **Optional**: Exports model as C code for ESP32

### 6. **README.md** (Complete Guide)
- Full workflow documentation
- Hardware setup instructions
- Troubleshooting guide
- Best practices

### 7. **quick_start.py** (Setup Assistant)
- Checks dependencies
- Guides through first collection
- Interactive setup wizard

---

## Recommended Sound Datasets

### 1. **ESC-50** (Best for General Environmental Sounds)
- **Source**: https://github.com/karolpiczak/ESC-50
- **Size**: ~600MB, 2,000 sounds
- **Categories**: 50 classes including:
  - Animals: dog, cat, rooster, bird
  - Natural: rain, wind, water, thunder
  - Human: crying baby, coughing, sneezing, footsteps
  - Interior/Domestic: door knock, keyboard, clock alarm, vacuum
  - Urban: siren, car horn, engine, helicopter
- **Download**: `python download_sounds.py --dataset esc50`

### 2. **UrbanSound8K** (Urban/City Sounds)
- **Source**: https://urbansounddataset.weebly.com/urbansound8k.html
- **Size**: ~6GB, 8,732 sounds
- **Categories**: Air conditioner, car horn, children playing, dog bark, drilling, engine idling, gun shot, jackhammer, siren, street music
- **Use case**: Safety/alert sounds for wearable
- **Download**: Manual download from website (requires citation agreement)

### 3. **FSD50K** (Freesound Dataset)
- **Source**: https://zenodo.org/record/4060432
- **Size**: ~50GB, 51,197 sounds
- **Categories**: 200 classes from AudioSet ontology
- **Use case**: Most comprehensive for any domain
- **Download**: From Zenodo (large!)

### 4. **AudioSet** (Google's Audio Dataset)
- **Source**: https://research.google.com/audioset/
- **Size**: ~2 million YouTube clips
- **Categories**: 632 classes (very comprehensive)
- **Use case**: When you need very specific sounds
- **Download**: Requires YouTube downloading (complex)

### 5. **Freesound.org** (Individual Sounds)
- **Source**: https://freesound.org/
- **Best for**: Custom, specific sounds
- **Search**: By keyword, browse by category
- **License**: Various (check individual sounds)
- **Use case**: When datasets don't have what you need

---

## Recommended Sound Categories for EchoSafe

Based on safety/wearable use case:

### High Priority (Collect First)
```
✅ Emergency Sounds
   - siren (police, ambulance, fire)
   - car_horn
   - alarm_clock / alarm_bell
   - glass_breaking
   
✅ Human Alerts
   - crying_baby
   - screaming (if available)
   - coughing
   - sneezing
   
✅ Environmental Alerts
   - dog_bark (aggressive vs friendly)
   - door_knock
   - doorbell
   
✅ Background/Calibration
   - silence (ambient room noise)
   - white_noise
   - rain
   - keyboard_typing
```

### Medium Priority
```
🔸 Activity Sounds
   - footsteps
   - clapping
   - vacuum_cleaner
   - washing_machine
   
🔸 Mechanical
   - engine
   - drilling
   - chainsaw
   - hand_saw
```

### Custom/Domain-Specific
```
🔹 Record Your Own
   - Specific alarm sounds in your environment
   - Voice commands ("help", "emergency")
   - Equipment-specific sounds
   - Environmental sounds unique to deployment location
```

---

## Data Collection Strategy

### Phase 1: Download ESC-50
```bash
python download_sounds.py --dataset esc50 --output sounds/
```

### Phase 2: Organize Priority Sounds
```bash
python play_sound.py --interactive sounds/playback_sounds
```

Pick categories from ESC-50:
- dog_bark
- siren
- car_horn
- crying_baby
- alarm_clock
- glass_breaking
- door_knock

### Phase 3: Record Custom Sounds

**Silence (Most Important!)**:
```bash
# Record ambient noise in your deployment environment
python play_sound.py --record-silence --silence-duration 2 --silence-output silence_001.wav

# Record 20-30 different silence samples:
# - Different rooms
# - Different times of day
# - With/without HVAC
# - With/without background conversation
```

**Custom Alarms/Alerts**:
```bash
# Create category for your specific alarm
python download_sounds.py --dataset custom --custom-name "my_alarm"

# Record your actual alarm sound:
# - Play through speaker
# - Record directly to .wav file (16kHz mono recommended)
# - Place in sounds/custom_recordings/my_alarm/
```

### Phase 4: Systematic Collection

**Recommended workflow**:

1. **Start with 6 core classes** (20-30 samples each):
   - silence
   - dog_bark
   - siren
   - car_horn
   - alarm_clock
   - crying_baby

2. **Vary conditions**:
   - Near (0.5m), medium (2m), far (5m) distances
   - Low, medium, high volumes
   - With/without background noise
   - Different angles

3. **Collection tips**:
   - Position speaker at consistent distance from mic
   - Label consistently (use same names!)
   - Take breaks every 10-15 samples
   - Verify features look good in serial output

---

## Expected Performance

With good data collection:

| Metric | Expected Value |
|--------|----------------|
| Training Accuracy | 95-99% |
| Validation Accuracy | 90-95% |
| Test Accuracy | 85-93% |
| Inference Time (ESP32) | 50-150ms |

**If accuracy is lower**:
- Collect more samples (aim for 30-50 per class)
- Ensure high audio quality
- Check label consistency
- Increase model size: `--hidden 256 128 64`

---

## Complete Workflow Timeline

### Day 1: Setup (1-2 hours)
- Upload firmware
- Verify I2S microphone works
- Download ESC-50 dataset
- Test capture with `c` command

### Day 2-3: Data Collection (4-6 hours)
- **Session 1**: Silence class (30 samples, 1 hour)
  - Record ambient noise in different conditions
- **Session 2**: ESC-50 sounds (3-4 classes, 2 hours)
  - Play sounds, capture, label
- **Session 3**: More ESC-50 sounds (3-4 classes, 2 hours)
- **Session 4**: Custom sounds (1-2 classes, 1 hour)

### Day 4: Training (1 hour)
- Train model on collected features
- Evaluate performance
- Identify weak classes → collect more data if needed

### Day 5: Refinement (2-3 hours)
- Collect additional samples for low-accuracy classes
- Retrain
- Export model to C code
- Create inference firmware

### Week 2: Deployment & Testing
- Deploy to ESP32
- Real-world testing
- Fix edge cases
- Final tuning

---

## Key Success Factors

### ✅ DO:
1. **Always collect a "silence" class** with ambient noise
2. **Use consistent label names** (snake_case recommended)
3. **Vary recording conditions** (distance, volume, background)
4. **Collect 20-30 samples minimum** per class
5. **Test on real deployment environment** before finalizing

### ❌ DON'T:
1. Skip the silence class
2. Use only one recording distance
3. Ignore class imbalance (collect equal samples)
4. Change labels mid-collection (dog_bark vs dog-bark vs dogbark)
5. Deploy without thorough testing

---

## Where to Get Help

1. **Arduino ESP32 Issues**:
   - https://github.com/espressif/arduino-esp32
   - Check Serial Monitor for error messages

2. **I2S Microphone Issues**:
   - https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html
   - Verify wiring with pin definitions in code

3. **Dataset Questions**:
   - ESC-50: https://github.com/karolpiczak/ESC-50
   - UrbanSound8K: https://urbansounddataset.weebly.com/
   - Freesound: https://freesound.org/help/

4. **Machine Learning**:
   - TensorFlow docs: https://www.tensorflow.org/
   - Feature engineering: Librosa docs (for reference)

---

## Next Steps After Basic Implementation

1. **Model Optimization**:
   - Quantization (float32 → int8)
   - Pruning
   - Knowledge distillation

2. **Advanced Features**:
   - Add spectral features (spectral centroid, rolloff)
   - Delta and delta-delta MFCCs
   - Temporal modeling (LSTM/GRU)

3. **Production Features**:
   - Confidence thresholds
   - Multi-class detection (detect multiple sounds)
   - Noise robustness testing
   - Power optimization

4. **Continuous Improvement**:
   - Log misclassifications
   - Collect edge cases
   - Periodic retraining
   - A/B testing

---

## Conclusion

This system gives you:
- ✅ **Perfect feature matching** (train = deployment)
- ✅ **Real-world performance** (actual device data)
- ✅ **Easy data collection** (plug-and-play workflow)
- ✅ **Iterative improvement** (collect more → retrain)

The key insight: **Don't try to match librosa to ESP32. Collect from ESP32 and train on that!**

Good luck with EchoSafe! 🎤🔊🤖
