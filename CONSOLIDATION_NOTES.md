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
| `EchoSafe_ML/uploadLittleFS.ino` | `firmware/uploadLittleFS/uploadLittleFS.ino` | Debug utility (moved into its own sketch folder 2026-09-22 so arduino-cli can compile it in place) |
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
- **(Fixed 2026-09-11)** RevA schematic had no signal-level wiring at all.
  Added all I2S (2× mic pairs + speaker), I2C (mux + 4 haptic drivers),
  motor-output, and relevant power/enable nets via global labels (matching
  the sheet's existing power-rail label convention). Initial mic/motor
  quadrant assignment was a guess (numeric order); confirmed with the user
  same day as clockwise (MIC1/M1=TL, MIC2/M2=TR, MIC3/M3=BR, MIC4/M4=BL)
  and the wiring corrected to match — see CLAUDE.md's hardware section for
  full details of what changed and why.
- **(Fixed 2026-09-11)** Added C6-C9 (0.1µF decoupling caps) for each
  DRV2605's REG pin, per datasheet recommendation.
- **(Decided 2026-09-11, confirmed final)** Speaker architecture: 1×
  MAX98357A driving both 8Ω speakers in parallel (4Ω combined, within
  spec) — no second amp needed.
- **(Added, then replaced, 2026-09-11)** A placeholder USB-C connector was
  wired to the NPM1300 PMIC's native VBUS/CC1/CC2 pins, then removed the
  same day once the real power architecture was confirmed. No power switch
  or button added — confirmed out of scope for this design.
- **(Replaced 2026-09-11) Power architecture overhaul:** NPM1300 PMIC
  removed entirely (it was never actually wired even in the original
  pre-consolidation schematic — confirmed before deleting it) and replaced
  with the user's actual intended parts: J1 = TP4056_Module (HiLetGo,
  w/ protection), LD1 = LD1117V33 (SparkFun breakout), BT1 = 3.7V 350mAh
  LiPo. The three previously-separate rails (3V3_SYS/3V3_AUDIO/1V8_MIC)
  were consolidated into one 3V3_SYS rail from the single LD1117V33 —
  mics moved from a planned 1.8V supply to 3.3V (within ICS-43434 spec).
  Full capacitor mapping (C1-C17, all values and purposes) is in
  CLAUDE.md's "Power Architecture" section, including a correction: the
  DRV2605 REG-pin caps were originally set to 0.1µF (my initial guess);
  TI's datasheet actually specifies 1.0µF, corrected as part of this pass.
- **(Confirmed 2026-09-11) Board partition:** 4 earpiece modules (front-L/
  front-R each with 1 mic + 1 motor + 1 speaker + that mic's own
  decoupling cap; rear-L/rear-R each with just 1 mic + 1 motor + cap)
  plus 1 central pod holding everything else (ESP32, amp, mux, all 4
  haptic drivers, charging/regulation, battery, remaining caps).
- **(Added 2026-09-11) Harness connectors J2-J5**, one per earpiece
  module, documenting exact wire count (9 pins front/with speaker, 7
  pins rear/without) using **star topology** (confirmed with the user):
  a separate cable per module rather than one cable daisy-chaining
  through both mics on a shared I2S bus. The electrical nets are
  identical either way — the choice only affects cable routing and
  pod-side pin count. See CLAUDE.md for the full per-connector pin list.
- **(Done 2026-09-12) Split into 5 separate KiCad projects.** MIC1-4,
  M1-4, LS1-2, and their decoupling caps (C9-C12) moved out of
  `EchoSafe_RevA` (now specifically the central pod) into 4 new small
  projects: `EchoSafe_FrontLeft`, `EchoSafe_FrontRight`,
  `EchoSafe_RearLeft`, `EchoSafe_RearRight`. All 5 share one library
  folder (`EchoSafe_RevA/lib/symbols/`) via relative `sym-lib-table`
  paths — nothing duplicated. Each module connects to the pod's J2-J5
  connectors via a matching local `J1` using the same net names (a
  documentation convention, not an automatic cross-project link — the
  real connection is the physical cable). See CLAUDE.md's "Multi-Board
  Project Structure" section for the full breakdown.
- **(Clarified 2026-09-11)** `EchoSafe_v1_mech_layout.png` is outdated,
  disregard it. `EchoSafe.png` is the actual target look, minus the
  in-ear-bud cable shown in that render — speakers mount in the behind-
  ear housings instead.
- **(Done 2026-09-12) Footprints assigned across all 5 projects**,
  target confirmed as a compact near-final wearable (not an early
  bring-up prototype). Found and fixed two real bugs along the way:
  (1) the project split had silently wiped the mic/motor footprints that
  already existed pre-split — the instance generator zeroed every new
  component's footprint unconditionally, restored from the pre-split
  commit; (2) **footprint library registration (`fp-lib-table`) never
  existed in any version of this project** — same class of gap as the
  `sym-lib-table` issue found earlier — created it for all 5 projects and
  fixed the bare (unprefixed) footprint names that could never have
  resolved without it. Full per-part footprint table and reasoning is in
  CLAUDE.md's "Footprints" section. Footprints across all 5 projects are now fully assigned as of
  2026-09-13: LS1/LS2 = DigiKey 1528-4227-ND = Adafruit #4227 Mini Oval
  Speaker, 8Ω/1W/30x20x5mm, verified via DigiKey/Adafruit listings, not
  from memory. Connects via a Molex PicoBlade 1.25mm 2-pin connector
  (not bare wire) — footprint set to the matching PicoBlade receptacle,
  confirmed against KiCad's own footprint library source.
- **(Verified 2026-09-13)** The JST-SH harness connector footprint
  names from last session were flagged as unverified — checked them
  against KiCad's own footprints repo and found the guess was wrong
  (`BM0xB-SRSS-TB` prefix instead of the real `SM0xB-SRSS-TB`). Fixed
  all 8 occurrences across all 5 projects. Also checked TP4056 module
  pin spacing against multiple independent sources; 2.54mm/0.1"
  through-hole is standard, matching what was already assigned —
  worth a physical check against the actual board once it's in hand.
- **(Critical fix, 2026-09-13) Session-wide Y-axis coordinate bug.**
  Discovered KiCad is actually installed on this machine and ran real
  `kicad-cli sch erc` against every schematic for the first time —
  found that every label/wire/no-connect placed this session with a
  nonzero local Y pin offset used the wrong sign (`placement_y +
  local_y` instead of the correct `placement_y − local_y`, per
  KiCad's Y-up-symbol/Y-down-sheet convention). Fixed via a from-
  scratch single-pass correction across all 5 projects, verified by
  ERC before/after (all 4 modules: 21/21/17/17 → 9/9/8/8 violations,
  all now expected; central pod: down to 0 dangling labels / 0
  genuinely unconnected pins). Also fixed along the way: the ESP32's
  own 3V3/GND/EN pins had never been wired to anything (every
  peripheral got connected, the MCU's own supply pins were missed) —
  added the EN RC delay circuit per Espressif's actual hardware
  design guidelines (fetched, R=10k/C=1uF), and tied #PWR01 (an
  orphaned GND flag left over from the removed NPM1300) to GND. Full
  writeup in CLAUDE.md's "Critical fix" section, including the
  transform formula for anyone hand-editing .kicad_sch again.
- **(Replaced 2026-09-13) Mic footprint (`LGA_CAV_IVS`) didn't match
  the real part.** Found while starting PCB placement: the original
  SnapEDA-imported footprint (pre-dates this session) had GND as a
  tiny 0.127mm corner pad and an off-by-one pad numbering issue
  (a mechanical hole occupying pad "6", pushing the real 6th
  electrical pad to "7"). Checked against InvenSense's actual
  datasheet (DS-000069, fetched this session): GND is really a large
  ring pad around the acoustic port. Authored a replacement footprint
  (`ICS-43434_LGA6`) via KiCad's own `pcbnew` Python API directly from
  the datasheet's dimensions, round-trip verified by reloading it.
  Updated all 4 module projects' `fp-lib-table` and MIC1-4's
  `Footprint` property. One thing not expressible in a footprint file:
  the datasheet also calls for a drilled acoustic port hole through
  the PCB itself at this location — flagged in CLAUDE.md as a manual
  layout step, not something a footprint can encode.
- **(Fixed 2026-09-11)** RevA was missing 3 of 4 DRV2605 haptic drivers and
  the TCA9548A I2C mux the firmware architecture requires. Added U5
  (TCA9548A, verified pinout from TI datasheet SCPS207F) and U6/U7/U8
  (DRV2605LDGS, duplicated from the existing working U3). Placed only —
  not wired to the ESP32 or to each other yet.
- **(Fixed 2026-09-11)** `hardware/EchoSafe_RevA/EchoSafe_RevA/sym-lib-table`
  pointed at files that never existed, even in the original folder pre-
  dating this consolidation, plus 2 of 5 library nicknames didn't match
  the `lib_id` prefixes actually used in the schematic. Corrected to real
  files via `${KIPRJMOD}`-relative paths. Previously any attempt to place
  a *new* instance of these parts in KiCad would have failed; existing
  placements worked only because `.kicad_sch` caches symbols inline.
- The original `EchoSafe_ML/CLAUDE.md` documented `echosafe_inference.ino`
  as having WAV playback / LittleFS / a fuller command set — the actual
  current file has none of that (mic + inference only). Fixed in this
  repo's `CLAUDE.md`.
- `EchoSafe_ML/` had no `.gitignore` despite its own docs claiming one
  excluded `*.npz` etc. This repo has a real one.
