#!/usr/bin/env python3
"""
EchoSafe Sound Playback Script
Plays sounds through speakers for microphone to capture during feature collection

Usage:
    # Play single sound
    python play_sound.py --sound path/to/sound.wav
    
    # Play all sounds in category with pauses
    python play_sound.py --category sounds/playback_sounds/dog_bark --pause 3
    
    # Interactive mode
    python play_sound.py --interactive sounds/playback_sounds
"""

import argparse
import sys
from pathlib import Path
import time

try:
    import sounddevice as sd
    import soundfile as sf
    import numpy as np
except ImportError:
    print("Error: Required audio libraries not installed")
    print("Install with: pip install sounddevice soundfile numpy")
    sys.exit(1)

class SoundPlayer:
    def __init__(self, device=None):
        self.device = device
        self.list_devices()
    
    def list_devices(self):
        """List available audio devices"""
        print("\n🔊 Available Audio Devices:")
        print(sd.query_devices())
        print()
    
    def play_sound(self, filepath, volume=1.0):
        """Play a single sound file"""
        filepath = Path(filepath)
        
        if not filepath.exists():
            print(f"✗ File not found: {filepath}")
            return False
        
        try:
            # Load audio file
            data, samplerate = sf.read(str(filepath))
            
            # Adjust volume
            data = data * volume
            
            # Ensure within valid range
            data = np.clip(data, -1.0, 1.0)
            
            print(f"▶️  Playing: {filepath.name}")
            print(f"   Duration: {len(data) / samplerate:.2f}s, Sample rate: {samplerate}Hz")
            
            # Play
            sd.play(data, samplerate, device=self.device)
            sd.wait()  # Wait for playback to finish
            
            print(f"   ✓ Finished")
            return True
            
        except Exception as e:
            print(f"✗ Error playing {filepath}: {e}")
            return False
    
    def play_category(self, category_dir, pause=3, volume=1.0, repeat=1):
        """Play all sounds in a category folder"""
        category_dir = Path(category_dir)
        
        if not category_dir.exists():
            print(f"✗ Category directory not found: {category_dir}")
            return
        
        # Get all audio files
        audio_files = []
        for ext in ['*.wav', '*.mp3', '*.flac', '*.ogg']:
            audio_files.extend(category_dir.glob(ext))
        
        audio_files = sorted(audio_files)
        
        if not audio_files:
            print(f"✗ No audio files found in {category_dir}")
            return
        
        print(f"\n📁 Category: {category_dir.name}")
        print(f"   Found {len(audio_files)} audio files")
        print(f"   Pause between files: {pause}s")
        print(f"   Repeat count: {repeat}")
        print()
        
        for rep in range(repeat):
            if repeat > 1:
                print(f"\n🔁 Repetition {rep + 1}/{repeat}")
            
            for i, filepath in enumerate(audio_files, 1):
                print(f"\n[{i}/{len(audio_files)}]")
                self.play_sound(filepath, volume)
                
                # Pause between sounds (except after last one)
                if i < len(audio_files) or rep < repeat - 1:
                    print(f"   ⏸  Pausing {pause} seconds...")
                    time.sleep(pause)
    
    def interactive_mode(self, sounds_dir):
        """Interactive sound selection and playback"""
        sounds_dir = Path(sounds_dir)
        
        if not sounds_dir.exists():
            print(f"✗ Directory not found: {sounds_dir}")
            return
        
        # Get all categories
        categories = [d for d in sounds_dir.iterdir() if d.is_dir()]
        categories = sorted(categories)
        
        if not categories:
            print(f"✗ No category folders found in {sounds_dir}")
            return
        
        print("\n" + "="*70)
        print("🎵 ECHOSAFE INTERACTIVE SOUND PLAYER")
        print("="*70)
        
        while True:
            print("\n📂 Available Categories:")
            for i, cat in enumerate(categories, 1):
                # Count audio files
                audio_count = sum(1 for _ in cat.glob('*.wav'))
                audio_count += sum(1 for _ in cat.glob('*.mp3'))
                print(f"   {i:2d}. {cat.name} ({audio_count} files)")
            
            print("\n   0. Quit")
            
            try:
                choice = input("\n📥 Select category (number): ").strip()
                
                if choice == '0':
                    print("\n👋 Goodbye!")
                    break
                
                idx = int(choice) - 1
                if 0 <= idx < len(categories):
                    selected_cat = categories[idx]
                    
                    # Get playback options
                    print(f"\n🎯 Selected: {selected_cat.name}")
                    
                    try:
                        pause = float(input("   Pause between sounds (seconds, default 3): ").strip() or "3")
                        volume = float(input("   Volume (0.0-1.0, default 0.8): ").strip() or "0.8")
                        repeat = int(input("   Repeat count (default 1): ").strip() or "1")
                    except ValueError:
                        print("   Using defaults: pause=3s, volume=0.8, repeat=1")
                        pause, volume, repeat = 3, 0.8, 1
                    
                    # Play the category
                    self.play_category(selected_cat, pause, volume, repeat)
                    
                    # Ask if user wants to continue
                    cont = input("\n📥 Play another category? (y/n): ").strip().lower()
                    if cont != 'y':
                        print("\n👋 Goodbye!")
                        break
                else:
                    print("   ✗ Invalid selection")
            
            except ValueError:
                print("   ✗ Please enter a number")
            except KeyboardInterrupt:
                print("\n\n👋 Interrupted. Goodbye!")
                break
    
    def record_silence(self, duration=2.0, output_file='silence.wav'):
        """Record ambient noise/silence for the 'silence' class"""
        print(f"\n🎙️  Recording ambient noise/silence for {duration} seconds...")
        print("   Keep the environment quiet (normal background noise is OK)")
        print("   Starting in 3...")
        time.sleep(1)
        print("   2...")
        time.sleep(1)
        print("   1...")
        time.sleep(1)
        print("   🔴 RECORDING...")
        
        samplerate = 16000  # Match ESP32 sample rate
        
        # Record
        recording = sd.rec(int(duration * samplerate), 
                          samplerate=samplerate, 
                          channels=1, 
                          dtype='float32',
                          device=self.device)
        sd.wait()
        
        # Save
        sf.write(output_file, recording, samplerate)
        
        print(f"   ✓ Saved to {output_file}")
        print(f"   Use this for 'silence' or 'background_noise' class")

def main():
    parser = argparse.ArgumentParser(description='EchoSafe Sound Player')
    parser.add_argument('--sound', help='Path to single sound file to play')
    parser.add_argument('--category', help='Path to category folder (plays all sounds)')
    parser.add_argument('--interactive', help='Path to sounds directory for interactive mode')
    parser.add_argument('--pause', type=float, default=3, help='Pause between sounds (seconds)')
    parser.add_argument('--volume', type=float, default=0.8, help='Playback volume (0.0-1.0)')
    parser.add_argument('--repeat', type=int, default=1, help='Number of times to repeat')
    parser.add_argument('--device', type=int, help='Audio device ID')
    parser.add_argument('--record-silence', action='store_true', help='Record ambient silence')
    parser.add_argument('--silence-duration', type=float, default=2.0, help='Silence recording duration')
    parser.add_argument('--silence-output', default='silence.wav', help='Silence output filename')
    
    args = parser.parse_args()
    
    player = SoundPlayer(device=args.device)
    
    if args.record_silence:
        player.record_silence(args.silence_duration, args.silence_output)
    
    elif args.sound:
        player.play_sound(args.sound, args.volume)
    
    elif args.category:
        player.play_category(args.category, args.pause, args.volume, args.repeat)
    
    elif args.interactive:
        player.interactive_mode(args.interactive)
    
    else:
        print("\n💡 Usage Examples:")
        print("="*70)
        print("# Play single sound")
        print("python play_sound.py --sound path/to/dog_bark.wav")
        print("\n# Play all sounds in a category")
        print("python play_sound.py --category sounds/playback_sounds/dog_bark")
        print("\n# Interactive mode")
        print("python play_sound.py --interactive sounds/playback_sounds")
        print("\n# Record ambient silence")
        print("python play_sound.py --record-silence")
        print("="*70)

if __name__ == '__main__':
    main()
