#!/usr/bin/env python3
"""
EchoSafe Model Trainer - Train on Real ESP32 Features
Trains classifier using features collected directly from ESP32

Usage:
    python train_on_esp32_features.py --dataset echosafe_dataset.npz --output model.h5
"""

import numpy as np
import argparse
from pathlib import Path
import json
from sklearn.model_selection import train_test_split
from sklearn.preprocessing import StandardScaler
import matplotlib.pyplot as plt
from datetime import datetime

try:
    import tensorflow as tf
    from tensorflow import keras
    from tensorflow.keras import layers
except ImportError:
    print("Error: TensorFlow not installed")
    print("Install with: pip install tensorflow")
    exit(1)

# Matches the fix in retrain.py: np.random.seed()/tf.random.set_seed() alone
# don't seed Keras 3's own internal generators for layer init/dropout, so
# two runs on identical data would still produce different models despite
# the data split below already being seeded via random_state=42.
keras.utils.set_random_seed(42)

class ESP32ModelTrainer:
    def __init__(self, dataset_path, output_dir='trained_models'):
        self.dataset_path = Path(dataset_path)
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)
        
        # Data
        self.X_train = None
        self.X_val = None
        self.X_test = None
        self.y_train = None
        self.y_val = None
        self.y_test = None
        self.scaler = None
        self.label_map = None
        self.num_classes = None
        
        # Model
        self.model = None
        self.history = None
        self.test_acc = None
    
    def load_dataset(self):
        """Load collected ESP32 features"""
        print(f"\n📂 Loading dataset: {self.dataset_path}")
        
        data = np.load(self.dataset_path, allow_pickle=True)
        
        features = data['features']  # Shape: (num_samples, num_frames, num_mfcc)
        labels = data['labels']
        self.label_map = data['label_map'].item()
        
        self.num_classes = len(self.label_map)

        # Hard-learned lesson (see CLAUDE.md "Important Constraints"): omitting
        # the noise/reject class causes every background sound to be
        # misclassified as a target class. Same guard as retrain.py.
        assert "noise" in self.label_map, (
            f"'noise' class missing from label_map ({sorted(self.label_map)}) -- "
            "training without it causes background sounds to be misclassified "
            "as a target class. Add noise samples to the dataset before training."
        )

        print(f"   ✓ Loaded {len(features)} samples")
        print(f"   Classes: {self.num_classes}")
        print(f"   Feature shape: {features[0].shape}")
        
        # Print class distribution
        print("\n   Class distribution:")
        id_to_name = {v: k for k, v in self.label_map.items()}
        for label_id in range(self.num_classes):
            count = np.sum(labels == label_id)
            class_name = id_to_name[label_id]
            print(f"      {class_name}: {count} samples")
        
        return features, labels
    
    def prepare_data(self, features, labels, test_size=0.2, val_size=0.1):
        """Prepare train/val/test splits"""
        print(f"\n🔀 Splitting data...")
        print(f"   Test size: {test_size*100:.0f}%")
        print(f"   Validation size: {val_size*100:.0f}%")
        
        # Flatten features: (samples, frames, mfcc) -> (samples, frames * mfcc)
        num_samples = features.shape[0]
        num_frames = features.shape[1]
        num_mfcc = features.shape[2]
        
        X_flat = features.reshape(num_samples, num_frames * num_mfcc)
        
        # Split into train+val and test
        X_temp, self.X_test, y_temp, self.y_test = train_test_split(
            X_flat, labels, test_size=test_size, random_state=42, stratify=labels
        )
        
        # Split train+val into train and val
        val_size_adjusted = val_size / (1 - test_size)
        self.X_train, self.X_val, self.y_train, self.y_val = train_test_split(
            X_temp, y_temp, test_size=val_size_adjusted, random_state=42, stratify=y_temp
        )
        
        print(f"   ✓ Train: {len(self.X_train)} samples")
        print(f"   ✓ Val: {len(self.X_val)} samples")
        print(f"   ✓ Test: {len(self.X_test)} samples")
        
        # Standardize features
        print("\n📊 Standardizing features...")
        self.scaler = StandardScaler()
        self.X_train = self.scaler.fit_transform(self.X_train)
        self.X_val = self.scaler.transform(self.X_val)
        self.X_test = self.scaler.transform(self.X_test)
        
        print(f"   Mean: {self.scaler.mean_[:5]}...")
        print(f"   Std: {self.scaler.scale_[:5]}...")
        
        # One-hot encode labels
        self.y_train = keras.utils.to_categorical(self.y_train, self.num_classes)
        self.y_val = keras.utils.to_categorical(self.y_val, self.num_classes)
        self.y_test = keras.utils.to_categorical(self.y_test, self.num_classes)
    
    def build_model(self, hidden_layers=[128, 64], dropout_rate=0.3):
        """Build MLP classifier"""
        print(f"\n🏗️  Building model...")
        
        input_dim = self.X_train.shape[1]
        
        model = keras.Sequential()
        model.add(layers.Input(shape=(input_dim,)))
        
        # Hidden layers
        for i, units in enumerate(hidden_layers):
            model.add(layers.Dense(units, activation='relu', name=f'hidden_{i+1}'))
            model.add(layers.Dropout(dropout_rate, name=f'dropout_{i+1}'))
        
        # Output layer
        model.add(layers.Dense(self.num_classes, activation='softmax', name='output'))
        
        model.compile(
            optimizer='adam',
            loss='categorical_crossentropy',
            metrics=['accuracy']
        )
        
        print(f"   Input dimension: {input_dim}")
        print(f"   Hidden layers: {hidden_layers}")
        print(f"   Dropout: {dropout_rate}")
        print(f"   Output classes: {self.num_classes}")
        
        model.summary()
        
        self.model = model
        return model
    
    def train(self, epochs=100, batch_size=32, patience=15):
        """Train the model"""
        print(f"\n🎯 Training model...")
        print(f"   Epochs: {epochs}")
        print(f"   Batch size: {batch_size}")
        print(f"   Early stopping patience: {patience}")
        
        callbacks = [
            keras.callbacks.EarlyStopping(
                monitor='val_loss',
                patience=patience,
                restore_best_weights=True
            ),
            keras.callbacks.ReduceLROnPlateau(
                monitor='val_loss',
                factor=0.5,
                patience=5,
                min_lr=1e-6
            )
        ]
        
        self.history = self.model.fit(
            self.X_train, self.y_train,
            validation_data=(self.X_val, self.y_val),
            epochs=epochs,
            batch_size=batch_size,
            callbacks=callbacks,
            verbose=1
        )
        
        print("\n   ✓ Training complete!")
    
    def evaluate(self):
        """Evaluate model on test set"""
        print(f"\n📊 Evaluating on test set...")
        
        test_loss, test_acc = self.model.evaluate(self.X_test, self.y_test, verbose=0)
        self.test_acc = test_acc  # stashed for export_for_esp32()'s header comment

        print(f"   Test Loss: {test_loss:.4f}")
        print(f"   Test Accuracy: {test_acc*100:.2f}%")
        
        # Per-class accuracy
        y_pred = self.model.predict(self.X_test, verbose=0)
        y_pred_classes = np.argmax(y_pred, axis=1)
        y_true_classes = np.argmax(self.y_test, axis=1)
        
        id_to_name = {v: k for k, v in self.label_map.items()}
        
        print("\n   Per-class accuracy:")
        for label_id in range(self.num_classes):
            mask = y_true_classes == label_id
            if np.sum(mask) > 0:
                class_acc = np.mean(y_pred_classes[mask] == label_id)
                class_name = id_to_name[label_id]
                print(f"      {class_name}: {class_acc*100:.1f}% ({np.sum(mask)} samples)")
        
        return test_loss, test_acc
    
    def save_model(self, model_name=None):
        """Save model and metadata"""
        if model_name is None:
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            model_name = f"echosafe_model_{timestamp}"
        
        model_path = self.output_dir / f"{model_name}.h5"
        metadata_path = self.output_dir / f"{model_name}_metadata.json"
        scaler_path = self.output_dir / f"{model_name}_scaler.npz"
        
        print(f"\n💾 Saving model...")
        
        # Save Keras model
        self.model.save(model_path)
        print(f"   ✓ Model: {model_path}")
        
        # Save scaler
        np.savez(scaler_path, mean=self.scaler.mean_, scale=self.scaler.scale_)
        print(f"   ✓ Scaler: {scaler_path}")
        
        # Save metadata
        metadata = {
            'label_map': self.label_map,
            'num_classes': self.num_classes,
            'input_shape': int(self.X_train.shape[1]),
            'num_frames': 31,  # From ESP32
            'num_mfcc': 13,    # From ESP32
            'sample_rate': 16000,
            'training_date': datetime.now().isoformat(),
            'train_samples': len(self.X_train),
            'val_samples': len(self.X_val),
            'test_samples': len(self.X_test),
        }
        
        with open(metadata_path, 'w') as f:
            json.dump(metadata, f, indent=2)
        
        print(f"   ✓ Metadata: {metadata_path}")
        
        return model_path, metadata_path, scaler_path
    
    def plot_training_history(self, save_path=None):
        """Plot training history"""
        if self.history is None:
            print("No training history available")
            return
        
        fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 4))
        
        # Accuracy
        ax1.plot(self.history.history['accuracy'], label='Train')
        ax1.plot(self.history.history['val_accuracy'], label='Validation')
        ax1.set_xlabel('Epoch')
        ax1.set_ylabel('Accuracy')
        ax1.set_title('Model Accuracy')
        ax1.legend()
        ax1.grid(True)
        
        # Loss
        ax2.plot(self.history.history['loss'], label='Train')
        ax2.plot(self.history.history['val_loss'], label='Validation')
        ax2.set_xlabel('Epoch')
        ax2.set_ylabel('Loss')
        ax2.set_title('Model Loss')
        ax2.legend()
        ax2.grid(True)
        
        plt.tight_layout()
        
        if save_path:
            plt.savefig(save_path, dpi=150, bbox_inches='tight')
            print(f"   ✓ Training plot: {save_path}")

        # plt.show() here used to run unconditionally after savefig. On a
        # machine with no interactive display backend available (headless/
        # SSH/CI, or -- confirmed directly -- this environment), it blocks
        # forever waiting for a window server connection that never comes,
        # silently hanging the whole script right before the --export-c
        # step that matters most (confirmed twice: killed two stuck runs,
        # both had produced this PNG via savefig() but never reached
        # export_for_esp32()). The plot is already saved to save_path above
        # in every real invocation (main() always passes one); only try an
        # interactive window when there's no save_path to fall back on, and
        # even then behind a backend check so a non-interactive backend
        # (e.g. Agg) can't hang the script the same way.
        if not save_path:
            interactive_backends = {
                "qtagg", "qt5agg", "gtk3agg", "gtk4agg", "tkagg",
                "wxagg", "macosx", "nbagg", "webagg",
            }
            if plt.get_backend().lower() in interactive_backends:
                plt.show()
            else:
                print("   (no interactive display backend available -- "
                      "pass save_path to save the plot instead of showing it)")
        plt.close(fig)
    
    def export_for_esp32(self, model_path, output_c_file):
        """Convert model to C arrays for ESP32.

        Output format must match exactly what echosafe_full_system.ino /
        echosafe_inference.ino #include and reference by name (NUM_CLASSES,
        INPUT_DIM, CLASS_NAMES, SCALER_MEAN/SCALE, LAYER0/2/4_*) -- this
        used to emit a generic NUM_LAYERS/layer_N_weights format that shared
        none of those identifiers with what the firmware actually expects,
        so a file produced by this function would fail to compile if used
        as model_weights.h. Fixed to match retrain.py's format, which is
        the format the firmware is actually written against.
        """
        print(f"\n🔧 Exporting model for ESP32...")

        # Load model
        model = keras.models.load_model(model_path)

        # Extract weights
        dense_layers = [l for l in model.layers if isinstance(l, layers.Dense)]

        # The firmware's inference code is not generic over layer count --
        # run_inference() in echosafe_full_system.ino has exactly 3 hardcoded
        # dense() calls (LAYER0->LAYER2->LAYER4). A model built with anything
        # other than the default --hidden 128 64 (2 hidden + 1 output = 3
        # Dense layers) would silently produce a header the firmware can't
        # actually use correctly. Fail loudly instead.
        if len(dense_layers) != 3:
            raise ValueError(
                f"Expected exactly 3 Dense layers (2 hidden + output) to match "
                f"the firmware's hardcoded LAYER0/LAYER2/LAYER4 inference code, "
                f"got {len(dense_layers)}. Re-run with --hidden set to exactly "
                f"2 values, or update run_inference() in "
                f"echosafe_full_system.ino to match this architecture."
            )

        W0, b0 = dense_layers[0].get_weights()
        W2, b2 = dense_layers[1].get_weights()
        W4, b4 = dense_layers[2].get_weights()

        c_code = self._generate_c_code(W0, b0, W2, b2, W4, b4)

        output_path = Path(output_c_file)
        with open(output_path, 'w') as f:
            f.write(c_code)

        print(f"   ✓ C code: {output_path}")
        print(f"   Include this file in your ESP32 project")

    def _generate_c_code(self, W0, b0, W2, b2, W4, b4):
        """Generate model_weights.h content -- same format as retrain.py."""
        id_to_name = {v: k for k, v in self.label_map.items()}
        class_names_str = ", ".join(f'"{id_to_name[i]}"' for i in range(self.num_classes))

        def fmt_array(name, arr, per_line=10):
            flat = arr.flatten()
            lines = [f"const float {name}[] = {{"]
            for i in range(0, len(flat), per_line):
                chunk = flat[i:i + per_line]
                lines.append("  " + ", ".join(f"{v:.8f}f" for v in chunk) + ",")
            lines.append("};\n")
            return "\n".join(lines)

        scaler_mean_str = "\n".join(
            "  " + ", ".join(f"{v:.8f}f" for v in self.scaler.mean_[i:i + 10]) + ","
            for i in range(0, len(self.scaler.mean_), 10)
        )
        scaler_scale_str = "\n".join(
            "  " + ", ".join(f"{v:.8f}f" for v in self.scaler.scale_[i:i + 10]) + ","
            for i in range(0, len(self.scaler.scale_), 10)
        )
        acc_str = f"{self.test_acc * 100:.1f}%" if self.test_acc is not None else "unknown"
        input_dim = W0.shape[0]

        return f"""// EchoSafe Model Weights - Auto-generated (train_on_esp32_features.py)
// Generated: {datetime.now().strftime('%Y%m%d_%H%M%S')}
// Test Accuracy: {acc_str}
// Classes: {', '.join(id_to_name[i] for i in range(self.num_classes))}

#ifndef ECHOSAFE_MODEL_WEIGHTS_H
#define ECHOSAFE_MODEL_WEIGHTS_H

#define NUM_CLASSES {self.num_classes}
#define INPUT_DIM   {input_dim}

const char* CLASS_NAMES[] = {{{class_names_str}}};

const int SCALER_SIZE = {input_dim};
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

def main():
    parser = argparse.ArgumentParser(description='Train EchoSafe model on ESP32 features')
    parser.add_argument('--dataset', required=True, help='Path to .npz dataset file')
    parser.add_argument('--output', default='trained_models', help='Output directory')
    parser.add_argument('--epochs', type=int, default=100, help='Training epochs')
    parser.add_argument('--batch-size', type=int, default=32, help='Batch size')
    parser.add_argument('--hidden', nargs='+', type=int, default=[128, 64], help='Hidden layer sizes')
    parser.add_argument('--dropout', type=float, default=0.3, help='Dropout rate')
    parser.add_argument('--export-c', action='store_true', help='Export model as C code for ESP32')
    
    args = parser.parse_args()
    
    # Create trainer
    trainer = ESP32ModelTrainer(args.dataset, args.output)
    
    # Load and prepare data
    features, labels = trainer.load_dataset()
    trainer.prepare_data(features, labels)
    
    # Build and train model
    trainer.build_model(hidden_layers=args.hidden, dropout_rate=args.dropout)
    trainer.train(epochs=args.epochs, batch_size=args.batch_size)
    
    # Evaluate
    trainer.evaluate()
    
    # Save model
    model_path, metadata_path, scaler_path = trainer.save_model()
    
    # Plot training history
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    plot_path = trainer.output_dir / f"training_history_{timestamp}.png"
    trainer.plot_training_history(save_path=plot_path)
    
    # Export for ESP32 if requested
    if args.export_c:
        c_file = trainer.output_dir / f"model_weights_{timestamp}.h"
        trainer.export_for_esp32(model_path, c_file)
    
    print("\n" + "="*70)
    print("✅ TRAINING COMPLETE!")
    print("="*70)
    print(f"Model: {model_path}")
    print(f"Metadata: {metadata_path}")
    print(f"Scaler: {scaler_path}")
    print("\nNext steps:")
    print("1. Test the model with new ESP32 features")
    print("2. Export to C code and upload to ESP32")
    print("3. Collect more data if accuracy is low")
    print("="*70)

if __name__ == '__main__':
    main()
