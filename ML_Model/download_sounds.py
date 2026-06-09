#!/usr/bin/env python3
"""
EchoSafe Dataset Downloader
Downloads and prepares environmental sound datasets for feature collection

Supports:
- ESC-50 (Environmental Sound Classification)
- UrbanSound8K
- Custom recordings

Usage:
    python download_sounds.py --dataset esc50 --output sounds/
"""

import argparse
import urllib.request
import zipfile
import tarfile
import os
from pathlib import Path
import json
import shutil

class SoundDatasetDownloader:
    def __init__(self, output_dir='sounds'):
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)
    
    def download_esc50(self):
        """Download ESC-50 dataset"""
        print("\n📦 Downloading ESC-50 Dataset...")
        print("   Source: https://github.com/karolpiczak/ESC-50")
        
        url = "https://github.com/karolpiczak/ESC-50/archive/master.zip"
        zip_path = self.output_dir / "esc50.zip"
        extract_path = self.output_dir / "ESC-50"
        
        # Download
        print(f"   Downloading from {url}...")
        urllib.request.urlretrieve(url, zip_path)
        print("   ✓ Downloaded")
        
        # Extract
        print("   Extracting...")
        with zipfile.ZipFile(zip_path, 'r') as zip_ref:
            zip_ref.extractall(self.output_dir)
        
        # Organize
        temp_dir = self.output_dir / "ESC-50-master"
        if temp_dir.exists():
            if extract_path.exists():
                shutil.rmtree(extract_path)
            temp_dir.rename(extract_path)
        
        # Clean up
        zip_path.unlink()
        
        print(f"   ✓ Extracted to {extract_path}")
        
        # Print categories
        audio_dir = extract_path / "audio"
        if audio_dir.exists():
            categories = self._get_esc50_categories(extract_path)
            print(f"\n   📂 ESC-50 contains {len(categories)} categories:")
            for i, cat in enumerate(sorted(categories), 1):
                print(f"      {i:2d}. {cat}")
        
        return extract_path
    
    def _get_esc50_categories(self, esc50_path):
        """Get ESC-50 category names"""
        categories = set()
        meta_file = esc50_path / "meta" / "esc50.csv"
        
        if meta_file.exists():
            with open(meta_file, 'r') as f:
                lines = f.readlines()[1:]  # Skip header
                for line in lines:
                    parts = line.strip().split(',')
                    if len(parts) >= 4:
                        categories.add(parts[3])  # Category column
        
        return categories
    
    def organize_for_playback(self, dataset_path, categories=None):
        """Organize sounds into categories for easy playback"""
        print("\n📁 Organizing sounds for playback...")
        
        playback_dir = self.output_dir / "playback_sounds"
        playback_dir.mkdir(exist_ok=True)
        
        if "ESC-50" in str(dataset_path):
            self._organize_esc50(dataset_path, playback_dir, categories)
        
        print(f"   ✓ Organized to {playback_dir}")
        return playback_dir
    
    def _organize_esc50(self, esc50_path, playback_dir, categories):
        """Organize ESC-50 by category"""
        meta_file = esc50_path / "meta" / "esc50.csv"
        audio_dir = esc50_path / "audio"
        
        if not meta_file.exists() or not audio_dir.exists():
            print("   ✗ ESC-50 metadata or audio files not found")
            return
        
        # Parse metadata
        file_to_category = {}
        with open(meta_file, 'r') as f:
            lines = f.readlines()[1:]  # Skip header
            for line in lines:
                parts = line.strip().split(',')
                if len(parts) >= 4:
                    filename = parts[0]
                    category = parts[3]
                    
                    # Filter by requested categories
                    if categories is None or category in categories:
                        file_to_category[filename] = category
        
        # Copy files to category folders
        for filename, category in file_to_category.items():
            src = audio_dir / filename
            if src.exists():
                # Create category folder
                cat_dir = playback_dir / category
                cat_dir.mkdir(exist_ok=True)
                
                # Copy file
                dst = cat_dir / filename
                shutil.copy2(src, dst)
        
        print(f"   Organized {len(file_to_category)} files into categories")
    
    def create_custom_category(self, category_name):
        """Create folder for custom recordings"""
        custom_dir = self.output_dir / "custom_recordings" / category_name
        custom_dir.mkdir(parents=True, exist_ok=True)
        
        print(f"\n📁 Created custom category: {custom_dir}")
        print(f"   Place your .wav files here for '{category_name}'")
        
        return custom_dir
    
    def list_recommended_categories(self):
        """List recommended sound categories for safety/wearable device"""
        print("\n🎯 RECOMMENDED CATEGORIES FOR ECHOSAFE (Safety/Wearable)")
        print("="*70)
        
        categories = {
            "Emergency Sounds": [
                "siren",
                "car_horn", 
                "fireworks",
                "glass_breaking",
                "gunshot" # if legal and safe
            ],
            "Human Sounds": [
                "crying_baby",
                "coughing",
                "sneezing",
                "laughing",
                "breathing"
            ],
            "Environmental Alerts": [
                "dog_bark",
                "cat_meow",
                "door_wood_knock",
                "doorbell",
                "alarm_clock"
            ],
            "Background/Noise": [
                "silence",  # Important!
                "white_noise",
                "rain",
                "wind",
                "keyboard_typing"
            ],
            "Activity Sounds": [
                "footsteps",
                "clapping",
                "hand_saw",
                "vacuum_cleaner",
                "washing_machine"
            ]
        }
        
        for group, items in categories.items():
            print(f"\n{group}:")
            for item in items:
                print(f"   • {item}")
        
        print("\n" + "="*70)
        print("COLLECTION STRATEGY:")
        print("   1. Start with 20-30 samples per category")
        print("   2. Include 'silence' category (just ambient room noise)")
        print("   3. Record in your actual deployment environment")
        print("   4. Vary distance, volume, and background noise")
        print("="*70)

def main():
    parser = argparse.ArgumentParser(description='Download sound datasets for EchoSafe')
    parser.add_argument('--dataset', choices=['esc50', 'custom', 'list'], 
                       default='list', help='Dataset to download or action')
    parser.add_argument('--output', default='sounds', help='Output directory')
    parser.add_argument('--categories', nargs='+', help='Specific categories to extract')
    parser.add_argument('--custom-name', help='Name for custom category folder')
    
    args = parser.parse_args()
    
    downloader = SoundDatasetDownloader(args.output)
    
    if args.dataset == 'list':
        downloader.list_recommended_categories()
        
        print("\n\n💡 NEXT STEPS:")
        print("="*70)
        print("1. Download ESC-50:")
        print("   python download_sounds.py --dataset esc50")
        print("\n2. Or create custom category for your own recordings:")
        print("   python download_sounds.py --dataset custom --custom-name 'my_alarm'")
        print("\n3. Then use playback script to play sounds while collecting features")
        print("="*70)
    
    elif args.dataset == 'esc50':
        dataset_path = downloader.download_esc50()
        downloader.organize_for_playback(dataset_path, args.categories)
        
        print("\n✅ ESC-50 Downloaded and organized!")
        print("\nNext: Use the playback script to play sounds while collecting features:")
        print("   python play_sound.py --sound sounds/playback_sounds/dog_bark/1-30226-A-0.wav")
    
    elif args.dataset == 'custom':
        if not args.custom_name:
            print("Error: --custom-name required for custom dataset")
            return
        
        downloader.create_custom_category(args.custom_name)

if __name__ == '__main__':
    main()
