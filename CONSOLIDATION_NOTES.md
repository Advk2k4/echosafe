# Consolidation Notes

This repo was assembled from several scattered folders under `~/Documents/`
on 2026-09-11. Nothing in the original folders was deleted or moved — only
copied. This file records what came from where, and what was deliberately
left out.

## Brought in

| Source | Went to | Notes |
|---|---|---|
| `EchoSafe_ML/echosafe_full_system/` | `firmware/echosafe_full_system/` | Most recently modified firmware, primary target |
| `EchoSafe_ML/echosafe_inference/` | `firmware/echosafe_inference/` | Kept as minimal bring-up reference; `echosafe_full_system.ino` depends on this folder via relative include — keep them siblings |
| `EchoSafe_ML/echosafe_feature_collector/` | `firmware/echosafe_feature_collector/` | Data collection firmware |
| `EchoSafe_ML/mic_oldML.c` | `firmware/mic_oldML.c` | Historical reference, copied verbatim |
| `EchoSafe_ML/uploadLittleFS.ino` | `firmware/uploadLittleFS.ino` | Debug utility |
| `EchoSafe_ML/data/*.wav` | `firmware/data/` | Alert sound files |
| `EchoSafe_ML/retrain.py`, `train_on_esp32_features.py`, `download_sounds.py`, `play_sound.py`, `serial_logger.py`, `quick_start.py` | `ml/` | ML pipeline scripts |
| `EchoSafe_ML/echosafe_dataset.npz` | `ml/echosafe_dataset.npz` | 1059-sample dataset, current |
| `EchoSafe_ML/IMPLEMENTATION_SUMMARY.md` | `docs/IMPLEMENTATION_SUMMARY.md` | Data collection strategy reference |
| `EchoSafe_RevA/` (entire folder) | `hardware/EchoSafe_RevA/` | Copied wholesale to preserve KiCad's relative-path project structure |

## Deliberately left out (and why)

- **`EchoSafe_ML/echosafe_fixed/echosafe_fixed.ino`** and
  **`EchoSafe_ML/final_test/final_test.ino`** — earlier drafts in the same
  lineage as `echosafe_full_system.ino`, superseded by it (confirmed by
  file mtimes and near-total diff overlap). Left in the original
  `EchoSafe_ML/` folder if ever needed for reference.
- **`EchoSafe_ML/trained_models/`** — two training snapshots from
  2026-02-12 using a `barks` class (not `gunshots`) on a ~361–373-sample
  dataset, predating the current 1059-sample/`gunshots` dataset. Stale.
- **`EchoSafe_ML/sounds/`** (~1.6GB, ESC-50 + playback sounds) — not
  copied due to size; regenerate locally via `python ml/download_sounds.py
  --dataset esc50 --output sounds/` if you need it for further data
  collection.
- **`EchoSafe_Audio/`** — an older/parallel ML effort: 16 hand-picked
  features (13 MFCCs + spectral centroid + bandwidth + zero-crossing-rate)
  instead of the current 403-feature pure-MFCC approach, different class
  set (`barks`, `VehicleApproaching` — not in the current 5-class set),
  lower accuracy (88.89% vs. 97.5%). Superseded, not merged in.
- **`EchoSafeTesting/`** — empty except for `.DS_Store`. Not carried
  forward.

## Known issues surfaced during consolidation (not yet fixed)

- **TL-mic workaround applied (2026-09-11):** `echosafe_full_system.ino`'s
  own comments documented a dead top-left (TL) mic (0.0000 RMS on last
  contact). `ML_MIC_CHANNEL` has been switched from `ONLY_LEFT` to
  `I2S_CHANNEL_FMT_ONLY_RIGHT` to match the documented workaround. This is
  a software mitigation, not a hardware fix — verify TL's actual state
  during bring-up. Also newly confirmed: `model_weights.h` was trained on
  the single-mic reference rig's audio (GPIO 5/6/7), not on either array
  channel, so real accuracy recovery requires retraining on array audio,
  not just this channel flip.
- **No PCB has ever been laid out or fabricated** for RevA — the KiCad
  `.kicad_pcb` file is empty. Any physical bring-up needs a breadboard/
  dev-kit setup, not an assembled board.
- **RevA schematic vs. firmware pin mapping has not been cross-checked.**
  They may have been developed somewhat independently — verify before
  trusting either as ground truth for wiring.
- The original `EchoSafe_ML/CLAUDE.md` documented `echosafe_inference.ino`
  as having WAV playback / LittleFS / a fuller command set — the actual
  current file has none of that (mic + inference only). Fixed in this
  repo's `CLAUDE.md`.
- `EchoSafe_ML/` had no `.gitignore` despite its own docs claiming one
  excluded `*.npz` etc. This repo has a real one.
