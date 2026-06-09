#!/usr/bin/env python3
"""
EchoSafe Quick Start
Verifies setup and guides through first feature collection
"""

import sys
import subprocess
from pathlib import Path

def check_dependencies():
    """Check if required Python packages are installed"""
    print("\n🔍 Checking dependencies...")
    
    required = {
        'serial': 'pyserial',
        'numpy': 'numpy',
        'tensorflow': 'tensorflow',
        'sklearn': 'scikit-learn',
        'sounddevice': 'sounddevice',
        'soundfile': 'soundfile',
        'matplotlib': 'matplotlib'
    }
    
    missing = []
    
    for module, package in required.items():
        try:
            __import__(module)
            print(f"   ✓ {package}")
        except ImportError:
            print(f"   ✗ {package} (missing)")
            missing.append(package)
    
    if missing:
        print(f"\n⚠️  Missing packages: {', '.join(missing)}")
        print(f"\n📦 Install with:")
        print(f"   pip install {' '.join(missing)}")
        return False
    
    print("\n   ✅ All dependencies installed!")
    return True

def check_files():
    """Check if required files exist"""
    print("\n📁 Checking files...")
    
    required_files = [
        'echosafe_feature_collector.ino',
        'serial_logger.py',
        'download_sounds.py',
        'play_sound.py',
        'train_on_esp32_features.py'
    ]
    
    missing = []
    
    for filename in required_files:
        if Path(filename).exists():
            print(f"   ✓ {filename}")
        else:
            print(f"   ✗ {filename} (missing)")
            missing.append(filename)
    
    if missing:
        print(f"\n⚠️  Missing files: {', '.join(missing)}")
        return False
    
    print("\n   ✅ All files present!")
    return True

def guide_user():
    """Interactive guide for first-time setup"""
    print("\n" + "="*70)
    print("🎯 ECHOSAFE QUICK START GUIDE")
    print("="*70)
    
    print("\n📝 PREREQUISITES:")
    print("   ✓ ESP32-S3-N16R8 board")
    print("   ✓ I2S MEMS microphone (ICS-43434 or INMP441)")
    print("   ✓ USB cable")
    print("   ✓ Speaker (for playing training sounds)")
    
    input("\n   Press Enter when ready to continue...")
    
    # Step 1: Upload firmware
    print("\n" + "-"*70)
    print("STEP 1: Upload Firmware to ESP32")
    print("-"*70)
    print("\n1. Connect ESP32 to your computer via USB")
    print("2. Open Arduino IDE")
    print("3. Open: echosafe_feature_collector.ino")
    print("4. Select board: ESP32S3 Dev Module")
    print("5. Select port: (your ESP32 port)")
    print("6. Click Upload")
    print("\n7. Open Serial Monitor (115200 baud)")
    print("   You should see: 'Ready for data collection!'")
    
    upload_ok = input("\n   Firmware uploaded successfully? (y/n): ").lower() == 'y'
    
    if not upload_ok:
        print("\n⚠️  Please upload firmware before continuing")
        print("   Refer to README.md for detailed instructions")
        return
    
    # Step 2: Find serial port
    print("\n" + "-"*70)
    print("STEP 2: Find ESP32 Serial Port")
    print("-"*70)
    print("\nCommon ports:")
    print("   Linux: /dev/ttyUSB0, /dev/ttyACM0")
    print("   macOS: /dev/cu.usbserial-*")
    print("   Windows: COM3, COM4, COM5")
    
    port = input("\n   Enter your ESP32 port: ").strip()
    
    if not port:
        print("\n⚠️  No port entered")
        return
    
    # Step 3: Download sounds
    print("\n" + "-"*70)
    print("STEP 3: Download Training Sounds (Optional)")
    print("-"*70)
    print("\nYou can either:")
    print("   A. Download ESC-50 dataset (~600MB, 2000 sounds)")
    print("   B. Record your own sounds")
    print("   C. Skip for now and use live sounds")
    
    download = input("\n   Download ESC-50? (y/n): ").lower() == 'y'
    
    if download:
        print("\n📦 Downloading ESC-50...")
        try:
            subprocess.run([
                sys.executable, 'download_sounds.py',
                '--dataset', 'esc50',
                '--output', 'sounds'
            ])
        except Exception as e:
            print(f"   ⚠️  Error: {e}")
    
    # Step 4: Start collection
    print("\n" + "-"*70)
    print("STEP 4: Start Feature Collection")
    print("-"*70)
    print("\nI'll start the serial logger for you.")
    print("In the logger, type 'c' to capture a sample.")
    print("\nTips:")
    print("   • Start by collecting 'silence' (ambient noise)")
    print("   • Collect 20-30 samples per sound category")
    print("   • Use consistent labels (e.g., 'dog_bark', not 'dog barking')")
    
    start = input("\n   Start serial logger? (y/n): ").lower() == 'y'
    
    if start:
        print(f"\n🚀 Starting serial logger on {port}...")
        print("   (Press Ctrl+C to exit)")
        
        try:
            subprocess.run([
                sys.executable, 'serial_logger.py',
                '--port', port,
                '--output', 'echosafe_dataset.npz'
            ])
        except KeyboardInterrupt:
            print("\n\n👋 Stopped by user")
        except Exception as e:
            print(f"\n⚠️  Error: {e}")
    
    # Next steps
    print("\n" + "="*70)
    print("📚 NEXT STEPS")
    print("="*70)
    print("\n1. Collect features:")
    print("   python serial_logger.py --port", port)
    print("\n2. Play sounds while collecting (in another terminal):")
    print("   python play_sound.py --interactive sounds/playback_sounds")
    print("\n3. Train model when you have 20+ samples per class:")
    print("   python train_on_esp32_features.py --dataset echosafe_dataset.npz")
    print("\n4. Read the full guide:")
    print("   cat README.md")
    print("\n" + "="*70)

def main():
    print("""
╔═══════════════════════════════════════════════════════════════════╗
║                                                                   ║
║   ███████╗ ██████╗██╗  ██╗ ██████╗ ███████╗ █████╗ ███████╗███████╗
║   ██╔════╝██╔════╝██║  ██║██╔═══██╗██╔════╝██╔══██╗██╔════╝██╔════╝
║   █████╗  ██║     ███████║██║   ██║███████╗███████║█████╗  █████╗  
║   ██╔══╝  ██║     ██╔══██║██║   ██║╚════██║██╔══██║██╔══╝  ██╔══╝  
║   ███████╗╚██████╗██║  ██║╚██████╔╝███████║██║  ██║██║     ███████╗
║   ╚══════╝ ╚═════╝╚═╝  ╚═╝ ╚═════╝ ╚══════╝╚═╝  ╚═╝╚═╝     ╚══════╝
║                                                                   ║
║              Real-World Feature Collection & Training             ║
║                                                                   ║
╚═══════════════════════════════════════════════════════════════════╝
    """)
    
    # Check dependencies
    if not check_dependencies():
        sys.exit(1)
    
    # Check files
    if not check_files():
        print("\n⚠️  Some files are missing. Please ensure all scripts are in the same directory.")
        sys.exit(1)
    
    # Guide user
    guide_user()

if __name__ == '__main__':
    main()
