#!/usr/bin/env python3
"""
EchoSafe Serial Feature Logger
Captures features from ESP32 and builds labeled dataset

Usage:
    python serial_logger.py --port /dev/ttyUSB0 --output dataset.npz
    
    On Windows: --port COM3
    On macOS: --port /dev/cu.usbserial-*
"""

import serial
import numpy as np
import argparse
import json
import time
from datetime import datetime
from pathlib import Path
import sys

class FeatureLogger:
    def __init__(self, port, baudrate=115200, output_file='echosafe_dataset.npz'):
        self.port = port
        self.baudrate = baudrate
        self.output_file = output_file
        self.serial = None
        
        # Dataset storage
        self.features = []
        self.labels = []
        self.metadata = []
        
        # Label categories
        self.label_map = {}
        self.next_label_id = 0
        
        # Load existing dataset if available
        self.load_existing_dataset()
    
    def load_existing_dataset(self):
        """Load existing dataset to continue collection"""
        if Path(self.output_file).exists():
            print(f"\n📂 Loading existing dataset: {self.output_file}")
            data = np.load(self.output_file, allow_pickle=True)
            
            self.features = list(data['features'])
            self.labels = list(data['labels'])
            self.metadata = list(data['metadata'])
            
            # Reconstruct label map
            if 'label_map' in data:
                self.label_map = data['label_map'].item()
                self.next_label_id = max(self.label_map.values()) + 1
            
            print(f"   Loaded {len(self.features)} existing samples")
            print(f"   Classes: {list(self.label_map.keys())}")
    
    def save_dataset(self):
        """Save dataset to file"""
        print(f"\n💾 Saving dataset to {self.output_file}...")
        
        np.savez(
            self.output_file,
            features=np.array(self.features),
            labels=np.array(self.labels),
            metadata=np.array(self.metadata),
            label_map=self.label_map
        )
        
        print(f"   ✓ Saved {len(self.features)} samples")
        self.print_dataset_stats()
    
    def print_dataset_stats(self):
        """Print current dataset statistics"""
        print("\n📊 Dataset Statistics:")
        print(f"   Total samples: {len(self.features)}")
        print(f"   Classes: {len(self.label_map)}")
        
        # Count samples per class
        from collections import Counter
        label_counts = Counter(self.labels)
        
        # Convert label IDs back to names
        id_to_name = {v: k for k, v in self.label_map.items()}
        
        print("\n   Samples per class:")
        for label_id, count in sorted(label_counts.items()):
            class_name = id_to_name.get(label_id, f"Unknown({label_id})")
            print(f"      {class_name}: {count}")
    
    def get_label_id(self, label_name):
        """Get or create label ID for a label name"""
        if label_name not in self.label_map:
            self.label_map[label_name] = self.next_label_id
            self.next_label_id += 1
        return self.label_map[label_name]
    
    def connect(self):
        """Connect to ESP32"""
        print(f"\n🔌 Connecting to ESP32 on {self.port}...")
        try:
            self.serial = serial.Serial(self.port, self.baudrate, timeout=1)
            time.sleep(2)  # Wait for ESP32 to reset
            print("   ✓ Connected!")
            return True
        except serial.SerialException as e:
            print(f"   ✗ Error: {e}")
            return False
    
    def disconnect(self):
        """Disconnect from ESP32"""
        if self.serial and self.serial.is_open:
            self.serial.close()
            print("\n🔌 Disconnected from ESP32")
    
    def send_command(self, command):
        """Send command to ESP32"""
        if self.serial and self.serial.is_open:
            self.serial.write(command.encode())
            self.serial.flush()
    
    def parse_features(self):
        """Parse features from serial stream"""
        feature_data = {
            'timestamp': None,
            'sample_rate': None,
            'num_frames': None,
            'num_mfcc': None,
            'capture_time_ms': None,
            'mfcc': []
        }
        
        in_features = False
        in_mfcc_data = False
        
        print("\n📡 Waiting for features from ESP32...")
        
        while True:
            if not self.serial or not self.serial.is_open:
                return None
            
            try:
                line = self.serial.readline().decode('utf-8', errors='ignore').strip()
                
                if not line:
                    continue
                
                # Print raw output for debugging
                if line and not line.startswith('MFCC_DATA'):
                    print(f"   {line}")
                
                # Parse feature markers
                if line == "FEATURES_START":
                    in_features = True
                    feature_data['mfcc'] = []
                    continue
                
                if line == "FEATURES_END":
                    in_features = False
                    in_mfcc_data = False
                    continue
                
                if line == "READY_FOR_LABEL":
                    return feature_data
                
                # Parse metadata
                if in_features and ':' in line and not in_mfcc_data:
                    key, value = line.split(':', 1)
                    
                    if key == "TIMESTAMP":
                        feature_data['timestamp'] = int(value)
                    elif key == "SAMPLE_RATE":
                        feature_data['sample_rate'] = int(value)
                    elif key == "NUM_FRAMES":
                        feature_data['num_frames'] = int(value)
                    elif key == "NUM_MFCC":
                        feature_data['num_mfcc'] = int(value)
                    elif key == "CAPTURE_TIME_MS":
                        feature_data['capture_time_ms'] = int(value)
                    elif key == "MFCC_DATA":
                        in_mfcc_data = True
                
                # Parse MFCC data
                elif in_mfcc_data and in_features:
                    # Parse comma-separated floats
                    try:
                        mfcc_row = [float(x) for x in line.split(',')]
                        feature_data['mfcc'].append(mfcc_row)
                    except ValueError:
                        pass  # Skip non-numeric lines
                
            except Exception as e:
                print(f"   ⚠ Parse error: {e}")
                continue
    
    def get_label_from_user(self):
        """Prompt user for label"""
        print("\n🏷️  Label this sample:")
        
        # Show available classes
        if self.label_map:
            print("   Existing classes:")
            for i, (name, id_) in enumerate(sorted(self.label_map.items(), key=lambda x: x[1]), 1):
                print(f"      {i}. {name} ({sum(1 for l in self.labels if l == id_)} samples)")
        
        print("\n   Enter class name (or press Enter to skip this sample):")
        label = input("   > ").strip()
        
        if not label:
            print("   ⏭  Skipping sample...")
            return None
        
        return label
    
    def collect_sample(self):
        """Trigger ESP32 to capture and collect one sample"""
        print("\n" + "="*60)
        print("🎤 CAPTURING NEW SAMPLE")
        print("="*60)
        
        # Send capture command
        self.send_command('c')
        
        # Parse features
        feature_data = self.parse_features()
        
        if not feature_data or not feature_data['mfcc']:
            print("   ✗ Failed to capture features")
            return False
        
        # Convert to numpy array
        mfcc_array = np.array(feature_data['mfcc'], dtype=np.float32)
        
        print(f"\n✓ Captured features: shape {mfcc_array.shape}")
        print(f"   Frames: {feature_data['num_frames']}")
        print(f"   MFCCs: {feature_data['num_mfcc']}")
        print(f"   Capture time: {feature_data['capture_time_ms']} ms")
        
        # Get label from user
        label_name = self.get_label_from_user()
        
        if label_name is None:
            return False
        
        # Add to dataset
        label_id = self.get_label_id(label_name)
        self.features.append(mfcc_array)
        self.labels.append(label_id)
        
        # Store metadata
        metadata = {
            'timestamp': datetime.now().isoformat(),
            'esp_timestamp': feature_data['timestamp'],
            'sample_rate': feature_data['sample_rate'],
            'capture_time_ms': feature_data['capture_time_ms'],
            'label_name': label_name
        }
        self.metadata.append(metadata)
        
        print(f"\n   ✓ Added to dataset as class '{label_name}' (ID: {label_id})")
        print(f"   Total samples: {len(self.features)}")
        
        return True
    
    def batch_collect(self):
        """Continuous capture with a single label applied to all samples.
        Label is set once at the start; press Ctrl+C to stop."""
        print("\n" + "="*60)
        print("🔄 BATCH COLLECTION MODE")
        print("="*60)

        # Show existing classes so the user can reuse a name
        if self.label_map:
            print("\n   Existing classes:")
            id_to_name = {v: k for k, v in self.label_map.items()}
            for lid in sorted(id_to_name):
                count = sum(1 for l in self.labels if l == lid)
                print(f"      {id_to_name[lid]} ({count} samples)")

        label_name = input("\n   Enter label for this batch: ").strip()
        if not label_name:
            print("   No label entered — cancelling.")
            return

        label_id = self.get_label_id(label_name)
        print(f"\n   Label set to '{label_name}' (ID: {label_id})")
        print("   Starting continuous capture — press Ctrl+C to stop.\n")

        # Start continuous mode on the ESP32
        self.send_command('r')

        batch_count = 0
        try:
            while True:
                feature_data = self.parse_features()

                if not feature_data or not feature_data['mfcc']:
                    print("   ⚠ Empty capture — skipping.")
                    continue

                mfcc_array = np.array(feature_data['mfcc'], dtype=np.float32)

                self.features.append(mfcc_array)
                self.labels.append(label_id)
                self.metadata.append({
                    'timestamp': datetime.now().isoformat(),
                    'esp_timestamp': feature_data['timestamp'],
                    'sample_rate': feature_data['sample_rate'],
                    'capture_time_ms': feature_data['capture_time_ms'],
                    'label_name': label_name,
                })

                batch_count += 1
                total = sum(1 for l in self.labels if l == label_id)
                print(f"   ✓ Sample {batch_count} captured  |  '{label_name}' total: {total}")

        except KeyboardInterrupt:
            print(f"\n\n   Stopped. {batch_count} samples added in this batch.")

        # Stop continuous mode on the ESP32
        self.send_command('s')
        self.save_dataset()

    def interactive_collection(self):
        """Interactive data collection loop"""
        print("\n" + "="*60)
        print("🎯 ECHOSAFE INTERACTIVE DATA COLLECTION")
        print("="*60)
        print("\nCommands:")
        print("  'c' - Capture one sample (prompts for label each time)")
        print("  'r' - Batch mode (label once, captures continuously)")
        print("  's' - Save dataset and show stats")
        print("  'q' - Save and quit")
        print()

        while True:
            try:
                command = input("\n📥 Command (c/r/s/q): ").strip().lower()

                if command == 'c':
                    self.collect_sample()

                elif command == 'r':
                    self.batch_collect()

                elif command == 's':
                    self.save_dataset()

                elif command == 'q':
                    print("\n👋 Saving and quitting...")
                    self.save_dataset()
                    break

                else:
                    print("   Unknown command")

            except KeyboardInterrupt:
                print("\n\n⚠️  Interrupted! Saving dataset...")
                self.save_dataset()
                break
            except Exception as e:
                print(f"\n⚠️  Error: {e}")
                import traceback
                traceback.print_exc()

def main():
    parser = argparse.ArgumentParser(description='EchoSafe Feature Logger')
    parser.add_argument('--port', required=True, help='Serial port (e.g., /dev/ttyUSB0, COM3)')
    parser.add_argument('--baudrate', type=int, default=115200, help='Baud rate (default: 115200)')
    parser.add_argument('--output', default='echosafe_dataset.npz', help='Output dataset file')
    
    args = parser.parse_args()
    
    # Create logger
    logger = FeatureLogger(args.port, args.baudrate, args.output)
    
    # Connect to ESP32
    if not logger.connect():
        print("Failed to connect to ESP32. Exiting.")
        sys.exit(1)
    
    try:
        # Start interactive collection
        logger.interactive_collection()
    finally:
        logger.disconnect()

if __name__ == '__main__':
    main()
