#!/usr/bin/env python3
"""
EchoSafe Retraining Script
- Loads echosafe_dataset.npz (434 samples)
- Handles class imbalance via sklearn compute_class_weight
- Trains MLP (403->128->64->5)
- Exports model_weights.h in the exact format expected by echosafe_inference.ino
"""

import numpy as np
from datetime import datetime
from pathlib import Path
from sklearn.model_selection import train_test_split
from sklearn.preprocessing import StandardScaler
from sklearn.utils.class_weight import compute_class_weight

import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers

DATASET_PATH  = "echosafe_dataset.npz"
OUTPUT_H_FILE = "echosafe_inference/model_weights.h"

# ── Load dataset ─────────────────────────────────────────────────────────────
data = np.load(DATASET_PATH, allow_pickle=True)
features  = data['features']   # (434, 31, 13)
labels    = data['labels']     # (434,)
label_map = data['label_map'].item()   # {'horns':0,'barks':1,'sirens':2,'noise':3,'bells':4}

num_classes = len(label_map)
id_to_name  = {v: k for k, v in label_map.items()}

print("=" * 60)
print("  EchoSafe Retraining")
print("=" * 60)
print(f"\nDataset: {DATASET_PATH}")
print(f"Samples: {len(features)}")
print(f"Classes: {num_classes}")
print("\nClass distribution:")
for lid in range(num_classes):
    c = int(np.sum(labels == lid))
    print(f"  {id_to_name[lid]:10s} ({lid}): {c}")

# ── Flatten features ──────────────────────────────────────────────────────────
X = features.reshape(len(features), -1)   # (434, 403)
y = labels

# ── Train/val/test split (stratified) ────────────────────────────────────────
X_tmp, X_test, y_tmp, y_test = train_test_split(
    X, y, test_size=0.15, random_state=42, stratify=y)
X_train, X_val, y_train, y_val = train_test_split(
    X_tmp, y_tmp, test_size=0.12, random_state=42, stratify=y_tmp)

print(f"\nSplit  →  train: {len(X_train)}, val: {len(X_val)}, test: {len(X_test)}")

# ── Standardise ───────────────────────────────────────────────────────────────
scaler  = StandardScaler()
X_train = scaler.fit_transform(X_train)
X_val   = scaler.transform(X_val)
X_test  = scaler.transform(X_test)

# ── Class weights (counter imbalance) ────────────────────────────────────────
cw_values = compute_class_weight('balanced',
                                 classes=np.arange(num_classes),
                                 y=y_train)
class_weight_dict = {i: w for i, w in enumerate(cw_values)}
print("\nClass weights (balanced):")
for lid, w in class_weight_dict.items():
    print(f"  {id_to_name[lid]:10s}: {w:.3f}")

# ── One-hot labels ────────────────────────────────────────────────────────────
y_train_oh = keras.utils.to_categorical(y_train, num_classes)
y_val_oh   = keras.utils.to_categorical(y_val,   num_classes)
y_test_oh  = keras.utils.to_categorical(y_test,  num_classes)

# ── Build MLP ─────────────────────────────────────────────────────────────────
INPUT_DIM = X_train.shape[1]   # 403

model = keras.Sequential([
    layers.Input(shape=(INPUT_DIM,)),
    layers.Dense(128, activation='relu',    name='hidden_1'),
    layers.Dropout(0.3,                     name='dropout_1'),
    layers.Dense(64,  activation='relu',    name='hidden_2'),
    layers.Dropout(0.3,                     name='dropout_2'),
    layers.Dense(num_classes, activation='softmax', name='output'),
], name='echosafe_mlp')

model.compile(
    optimizer=keras.optimizers.Adam(learning_rate=1e-3),
    loss='categorical_crossentropy',
    metrics=['accuracy']
)
model.summary()

# ── Train ─────────────────────────────────────────────────────────────────────
callbacks = [
    keras.callbacks.EarlyStopping(
        monitor='val_loss', patience=20, restore_best_weights=True),
    keras.callbacks.ReduceLROnPlateau(
        monitor='val_loss', factor=0.5, patience=7, min_lr=1e-6),
]

print("\nTraining…")
history = model.fit(
    X_train, y_train_oh,
    validation_data=(X_val, y_val_oh),
    epochs=150,
    batch_size=32,
    class_weight=class_weight_dict,
    callbacks=callbacks,
    verbose=1,
)

# ── Evaluate ──────────────────────────────────────────────────────────────────
test_loss, test_acc = model.evaluate(X_test, y_test_oh, verbose=0)

y_pred_prob   = model.predict(X_test, verbose=0)
y_pred_class  = np.argmax(y_pred_prob, axis=1)

print("\n" + "=" * 60)
print(f"  Test Loss:     {test_loss:.4f}")
print(f"  Test Accuracy: {test_acc*100:.2f}%")
print("\n  Per-class accuracy on test set:")
for lid in range(num_classes):
    mask  = y_test == lid
    total = int(np.sum(mask))
    if total == 0:
        continue
    correct = int(np.sum(y_pred_class[mask] == lid))
    pct     = correct / total * 100
    print(f"    {id_to_name[lid]:10s}: {pct:5.1f}%  ({correct}/{total})")
print("=" * 60)

# ── Export model_weights.h ────────────────────────────────────────────────────
# The firmware uses these exact names:
#   LAYER0_WEIGHTS / LAYER0_BIAS  (Dense 403→128)
#   LAYER2_WEIGHTS / LAYER2_BIAS  (Dense 128→64)
#   LAYER4_WEIGHTS / LAYER4_BIAS  (Dense  64→5)
# (numbering follows Keras layer index: 0=dense, 1=dropout, 2=dense, 3=dropout, 4=dense)

dense_layers = [l for l in model.layers if isinstance(l, layers.Dense)]
assert len(dense_layers) == 3, f"Expected 3 Dense layers, got {len(dense_layers)}"

W0, b0 = dense_layers[0].get_weights()   # (403, 128), (128,)
W2, b2 = dense_layers[1].get_weights()   # (128,  64), ( 64,)
W4, b4 = dense_layers[2].get_weights()   # ( 64,   5), (  5,)

def fmt_array(name, arr, per_line=10):
    flat = arr.flatten()
    lines = [f"const float {name}[] = {{"]
    for i in range(0, len(flat), per_line):
        chunk = flat[i:i+per_line]
        lines.append("  " + ", ".join(f"{v:.8f}f" for v in chunk) + ",")
    lines.append("};\n")
    return "\n".join(lines)

class_names_str = ", ".join(f'"{id_to_name[i]}"' for i in range(num_classes))
scaler_mean_str = "\n".join(
    "  " + ", ".join(f"{v:.8f}f" for v in scaler.mean_[i:i+10]) + ","
    for i in range(0, len(scaler.mean_), 10)
)
scaler_scale_str = "\n".join(
    "  " + ", ".join(f"{v:.8f}f" for v in scaler.scale_[i:i+10]) + ","
    for i in range(0, len(scaler.scale_), 10)
)

h = f"""// EchoSafe Model Weights - Auto-generated
// Generated: {datetime.now().strftime('%Y%m%d_%H%M%S')}
// Test Accuracy: {test_acc*100:.1f}%
// Classes: {', '.join(id_to_name[i] for i in range(num_classes))}
// Dataset: {len(features)} samples  (barks:{int(np.sum(labels==1))}, noise:{int(np.sum(labels==3))}, bells:{int(np.sum(labels==4))}, sirens:{int(np.sum(labels==2))}, horns:{int(np.sum(labels==0))})
// Class-weight balanced training

#ifndef ECHOSAFE_MODEL_WEIGHTS_H
#define ECHOSAFE_MODEL_WEIGHTS_H

#define NUM_CLASSES {num_classes}
#define INPUT_DIM   {INPUT_DIM}

const char* CLASS_NAMES[] = {{{class_names_str}}};

const int SCALER_SIZE = {INPUT_DIM};
const float SCALER_MEAN[] = {{
{scaler_mean_str}
}};

const float SCALER_SCALE[] = {{
{scaler_scale_str}
}};

// ── Layer 0: Dense {W0.shape[0]} → {W0.shape[1]} ────────────────────────────
const int LAYER0_INPUT  = {W0.shape[0]};
const int LAYER0_OUTPUT = {W0.shape[1]};
{fmt_array('LAYER0_WEIGHTS', W0)}
{fmt_array('LAYER0_BIAS',    b0)}
// ── Layer 2: Dense {W2.shape[0]} → {W2.shape[1]} ────────────────────────────
const int LAYER2_INPUT  = {W2.shape[0]};
const int LAYER2_OUTPUT = {W2.shape[1]};
{fmt_array('LAYER2_WEIGHTS', W2)}
{fmt_array('LAYER2_BIAS',    b2)}
// ── Layer 4: Dense {W4.shape[0]} → {W4.shape[1]} ──────────────────────────────
const int LAYER4_INPUT  = {W4.shape[0]};
const int LAYER4_OUTPUT = {W4.shape[1]};
{fmt_array('LAYER4_WEIGHTS', W4)}
{fmt_array('LAYER4_BIAS',    b4)}
#endif // ECHOSAFE_MODEL_WEIGHTS_H
"""

Path(OUTPUT_H_FILE).write_text(h)
print(f"\nExported → {OUTPUT_H_FILE}")
print(f"File size: {Path(OUTPUT_H_FILE).stat().st_size // 1024} KB")
print("\nDone. Flash echosafe_inference.ino to the ESP32.")
