# CLAUDE.md

This file provides guidance to Claude Code when working in this repository.

## Project Overview

EchoSafe is a wearable, around-the-ear sound-alert device for people who are
deaf or hard of hearing, shaped like a Shokz-style bone-conduction headset.
An ESP32-S3 listens via 4 MEMS microphones (one per quadrant around the
head), classifies target sounds on-device with an MLP, and fires directional
haptic motors so the wearer knows WHAT sound occurred and WHICH direction it
came from.

**This repo is a consolidated, cleaned-up successor to several scattered
folders** (`EchoSafe_ML`, `EchoSafe_RevA`, `EchoSafe_Audio`, `EchoSafeTesting`
under `~/Documents/`). See `CONSOLIDATION_NOTES.md` for what was brought in,
what was left behind, and why. The originals were NOT deleted — treat them
as a read-only safety net, not a second source of truth going forward.

**Team repo:** `https://github.com/umass-ece-sdp/sdp26-22` (private). Do not
push here without asking first, every time.

**Current 5 sound classes:** `horns`(0), `noise`(1), `bells`(2), `gunshots`(3), `sirens`(4)

---

## Current Status (as of this consolidation)

- **Firmware:** `firmware/echosafe_full_system/` is the target build (4 mics
  + 2 speakers + 4 haptics) but its confirmed-working status is UNVERIFIED —
  nobody has confirmed it runs end-to-end on real hardware recently.
- **TL-mic workaround applied:** the firmware's own comments documented the
  top-left (TL) mic reading 0.0000 RMS (dead channel) on last contact.
  `ML_MIC_CHANNEL` in `firmware/echosafe_full_system/echosafe_full_system.ino`
  has been switched to `I2S_CHANNEL_FMT_ONLY_RIGHT` (TR mic) to match. This
  is a software workaround, not a hardware fix — confirm during bring-up
  whether TL is actually still dead (serial output prints per-mic RMS during
  the `d` direction-test command); if it's fine now, flipping back to
  `ONLY_LEFT` is one line.
  **Also note:** `model_weights.h` was trained on audio from the single-mic
  reference rig (`echosafe_feature_collector.ino`, GPIO 5/6/7) — a different
  physical mic than either TL or TR on the array. Neither channel is a
  perfect distribution match for the trained model; retraining on audio
  captured from whichever channel is actually used is the real long-term
  fix, this workaround is a stopgap to get the system running.
  **Direction detection is not covered by this workaround** — TDOA reads
  all 4 mics regardless of `ML_MIC_CHANNEL`, so if TL is genuinely dead,
  direction detection will still be degraded (that quadrant will rarely/
  never be selected correctly) until the hardware fault itself is fixed.
- **Hardware:** `hardware/` holds 5 KiCad projects (4 small earpiece
  modules + 1 central pod), all with complete schematics **and now fully
  placed, routed, DRC-clean PCB layouts** (0 violations, 0 unconnected
  pads on every board, verified via `kicad-cli pcb drc`) — see "PCB
  Layout" under the `hardware/` section below for how the central pod
  (ESP32-S3-WROOM-1, MAX98357A, TCA9548A, 4× DRV2605L, TP4056 + LD1117V33
  power chain) was routed. Nothing has been fabricated yet. Bring-up
  right now still has to happen on a breadboard or dev-kit with jumper
  wiring, following the schematic's part choices and the pin mapping
  documented below — the PCB files are ready for fab but haven't been
  ordered/assembled.

---

## Firmware

### `firmware/echosafe_full_system/echosafe_full_system.ino` — PRIMARY TARGET
Full system: 4 mics, 2 speakers (parallel, time-shared I2S bus), 4 haptic
motors. Three-phase loop: ML inference (Phase 1) → TDOA direction detection
(Phase 2) → alert playback (Phase 3).

Includes its model via a **relative path** —
`#include "../echosafe_inference/model_weights.h"` — so it depends on
`firmware/echosafe_inference/` staying a sibling folder. Do not move one
without the other. (A duplicate `model_weights.h` also sits directly in this
folder from the original project layout; it's unused dead weight, byte-
identical to the one actually included — harmless to ignore or remove.)

Serial commands: `i` (single inference), `r` (continuous), `s` (stop),
`h` (haptic test — fires all 4 motors in sequence), `d` (direction test —
prints per-mic RMS + detected quadrant), `t` (440Hz tone test),
`p` (speaker test — cycles all WAV alerts).

Requires the **Adafruit DRV2605 Library** (Arduino Library Manager) and the
custom `partitions.csv` in this folder (default Arduino partition scheme is
too small for the WAV alert files on LittleFS).

### `firmware/echosafe_inference/echosafe_inference.ino` — MINIMAL BRING-UP REFERENCE
Deliberately stripped down: single mic (I2S_NUM_0) + MLP inference + Serial
print only. **No speaker, no LittleFS, no WAV playback.** Auto-starts in
continuous classification mode on boot. Commands: `i`/`r`/`s` only.

Use this FIRST when bringing up new/unfamiliar hardware — it's the simplest
possible way to confirm the mic + MLP path works before adding the
complexity of the full system.

### `firmware/echosafe_feature_collector/echosafe_feature_collector.ino`
Data collection firmware. Captures audio at 16kHz, extracts MFCCs on-chip,
streams feature vectors over serial for `ml/serial_logger.py` to capture and
label. Use this to expand the dataset.

### `firmware/mic_oldML.c`
Historical reference implementation of the 4-mic TDOA + DRV2605 haptic
logic that `echosafe_full_system.ino` evolved from. Not a buildable sketch
on its own (no `.ino` extension) — kept for reference. **Do not delete.**

### `firmware/uploadLittleFS.ino`
WiFi-based web file manager for inspecting LittleFS contents over WiFi.
**Set `ST_SSID`/`ST_PASS` before use.** Development/debug utility only — do
not deploy in production.

### `firmware/data/`
WAV alert files to upload to LittleFS (Arduino LittleFS Filesystem Uploader
plugin), used by `echosafe_full_system.ino`:
- `vehiclehorn.wav` → class 0 (horns)
- `bicyclebell.wav` → class 2 (bells)
- `gun_shot.wav` → class 3 (gunshots)
- `emergencysiren.wav` → class 4 (sirens)
- (no WAV for class 1 / `noise` — it's the reject/background class, never alerts)

---

## Hardware Pin Mapping (ESP32-S3-WROOM-1 N16R8)

### Microphones (4× ICS-43434, 2 I2S buses in stereo L/R pairs, run on 3.3V)
| Mic | Bus | Signal | GPIO |
|---|---|---|---|
| TOP pair | I2S_NUM_0 | LRCLK / WS | 4 |
| TOP pair | I2S_NUM_0 | BCLK | 5 |
| TOP pair | I2S_NUM_0 | DOUT | 16 |
| Top-Left (TL) — MIC1 | — | SEL | GND → LEFT channel |
| Top-Right (TR) — MIC2 | — | SEL | `3V3_SYS` → RIGHT channel |
| BOT pair | I2S_NUM_1 | LRCLK / WS | 12 |
| BOT pair | I2S_NUM_1 | BCLK | 21 |
| BOT pair | I2S_NUM_1 | DOUT | 18 |
| Bottom-Right (BR) — MIC3 | — | SEL | `3V3_SYS` → RIGHT channel |
| Bottom-Left (BL) — MIC4 | — | SEL | GND → LEFT channel |

All 4 mic VDD pins → `3V3_SYS` (mics originally spec'd for a separate 1.8V
rail; moved to the single 3.3V system rail when the PMIC was replaced —
ICS-43434 supports up to 3.63V, so this is within spec).

### Speakers (2× MAX98357A, parallel, shares I2S_NUM_1 with BOT mics)
| Signal | GPIO |
|---|---|
| DIN | 47 |
| BCLK | 48 |
| LRC | 45 |
| SD (shutdown) | tie to VIN |

I2S_NUM_1 is time-multiplexed: RX for BOT mics during capture, TX for
speaker during alert playback, then reinitialized back to RX. Verify this
switch doesn't leave the peripheral in the wrong mode — this is a plausible
source of bugs if the full-system firmware turns out not to work.

### Haptics (4× DRV2605L via TCA9548A I2C mux)
| Signal | GPIO |
|---|---|
| SDA | 9 |
| SCL | 11 |
| Mux address | 0x70 (A0–A3 → GND) |
| DRV2605L address (each channel) | 0x5A |

| Mux channel | Motor |
|---|---|
| 0 | Top-Left |
| 1 | Top-Right |
| 2 | Bottom-Left |
| 3 | Bottom-Right |

### Single-mic reference (`echosafe_inference.ino` only — different wiring!)
| Signal | GPIO |
|---|---|
| WS (LRCLK) | 5 |
| SD (DOUT) | 6 |
| SCK (BCLK) | 7 |

Note this uses **different GPIOs** than the TOP mic pair in the full-system
firmware — they are not meant to be wired identically. Don't assume you can
swap firmware without re-wiring.

---

## ML Pipeline

### Feature extraction (identical on-chip and in Python training)
| Parameter | Value |
|---|---|
| Sample rate | 16,000 Hz |
| Frame size | 512 samples |
| Hop (frame step) | 256 samples (50% overlap) |
| Frames per sample | 31 |
| FFT size | 512 |
| Mel filter banks | 26 |
| MFCC coefficients | 13 |
| Pre-emphasis | 0.97 |
| Window | Hamming |

Feature tensor per sample: `(31, 13)` → flattened to `(403,)`. A
`StandardScaler` (zero mean, unit variance) fit on the training split has
its 403-element mean/scale arrays embedded directly in `model_weights.h` so
normalization is bit-identical between training and inference.

### Model
MLP: **403 → 128 → Dropout(0.3) → 64 → Dropout(0.3) → 5 (softmax)**
Test accuracy: **97.5%** (1059-sample dataset, Feb 2026).
Confidence threshold: `0.65` — predictions below this are treated as
uncertain/noise, no alert fires.

### Dataset
`ml/echosafe_dataset.npz` — **1059 samples**, 5 classes, all collected on
real ESP32-S3 hardware (not synthetic/librosa — this eliminates train/
inference domain mismatch):

| Class | Index | Samples |
|---|---|---|
| horns | 0 | 201 |
| noise | 1 | 258 |
| bells | 2 | 200 |
| gunshots | 3 | 200 |
| sirens | 4 | 200 |

Not committed to git (`.gitignore` excludes `*.npz`) — keep the local copy
safe, it's the only one.

### Key commands
```bash
pip install pyserial numpy tensorflow scikit-learn matplotlib sounddevice soundfile

# Retrain (reads echosafe_dataset.npz, writes firmware/echosafe_inference/model_weights.h)
cd ml && python retrain.py

# Parametric training with more control
python train_on_esp32_features.py \
    --dataset echosafe_dataset.npz \
    --output trained_models \
    --epochs 150 --batch-size 32 --hidden 128 64 --dropout 0.3 --export-c

# Data collection (two terminals)
python serial_logger.py --port /dev/cu.usbmodem101 --output echosafe_dataset.npz
python play_sound.py --interactive sounds/playback_sounds   # (re-download ESC-50 first, see below)

# Re-download ESC-50 sound bank (~1.6GB, not kept in this repo)
python download_sounds.py --dataset esc50 --output sounds/
```

**Important:** `retrain.py` writes to `firmware/echosafe_inference/model_weights.h`.
Since `echosafe_full_system.ino` includes that same file via relative path,
retraining updates both firmware targets automatically — you do not need to
manually copy the header anywhere.

---

## Hardware (`hardware/`)

**As of 2026-09-12 this is 5 separate KiCad projects, not 1** — see
"Multi-Board Project Structure" below for the full layout. The original
`EchoSafe_RevA` project is now specifically the **central pod** (ESP32,
amp, mux, haptic drivers, charging/regulation, battery); 4 new small
projects (`EchoSafe_FrontLeft/Right`, `EchoSafe_RearLeft/Right`) hold one
mic + motor (+ speaker, front only) each. All 5 share one library folder
at `EchoSafe_RevA/lib/symbols/`.

`EchoSafe_RevA/EchoSafe_RevA.kicad_sch` / `.kicad_pcb` / `.kicad_pro` is
the central pod's project — its `.kicad_pcb` exists but is empty (0
footprints placed). **The 4 new module projects don't have a `.kicad_pcb`
file at all yet** — schematic capture only; KiCad creates one
automatically the first time someone opens a board view for that project.
`datasheets/` and `outputs/` are currently empty.

### Critical fix: Y-axis coordinate bug affecting the entire session (2026-09-13)

**KiCad is actually installed on this machine** (`/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli`,
version 8.0.0) — discovered while starting PCB layout work, and immediately
used to run real ERC (Electrical Rules Check) against every schematic from
this session, instead of continuing to rely on the text-based checks
(paren balance, UUID uniqueness, net endpoint counts) used until now. That
was the right call: **every label, wire, and no-connect marker placed
this session with a nonzero local Y pin offset was silently in the wrong
position.**

Root cause: KiCad symbol-library graphics are authored Y-up, but the
schematic sheet is Y-down. For an unrotated, unmirrored placement, the
correct transform from a symbol's local pin offset to its absolute sheet
position is `abs_y = placement_y − local_y` — every script this session
used `abs_y = placement_y + local_y`. The two only agree when local_y=0
(which is why a few connections — like the very first mic pin checked
early on — happened to look fine). Confirmed with real evidence, not
guessed: `kicad-cli sch erc` flagged pins as unconnected exactly where
the sign error would predict, and the labels intended for them were
found sitting at the mirrored position.

Fixed via a from-scratch, single-pass correction (not sequential in-place
edits — an earlier attempt at that approach introduced a second bug,
caught before committing: correcting one label's coordinate could make it
collide with a not-yet-corrected label's original position, causing a
later step to move the wrong element). The final approach computed every
correction from an unmodified snapshot of each file, detected any
old-position collisions between distinct pins up front, and applied all
corrections in one pass.

**Result, verified by real ERC, not by the tooling that produced the
bug:**
- All 4 module projects: 21/21/17/17 raw ERC violations → 9/9/8/8, all
  of them expected (spare unused GPIO pins, the already-documented
  placeholder-connector-symbol warning, and "power pin not driven"
  errors that are the correct consequence of the multi-project split —
  the driving component lives in a different project's file, which ERC
  can't see into).
- Central pod: 172 → ultimately 0 dangling labels and 0 genuinely
  unconnected pins. Remaining ~200 ERC items are exclusively: pairwise
  "pin_to_pin" type-mismatch warnings from many different IC pin types
  legitimately sharing the `3V3_SYS`/`GND` rails (expected on any real
  board with this many ICs on shared power), cosmetic "off-grid"
  warnings, the placeholder-connector-symbol warning (×7, one per
  `Conn_Harness_*` instance across all 5 projects), and spare/unused
  pins (ESP32 GPIOs never wired to anything, TCA9548A mux channels 4-7
  since only 4 of 8 channels are used).

**A second, genuine gap surfaced by this same ERC pass, unrelated to the
Y-axis bug:** the ESP32 module's own power pins — `3V3`, all 7 `GND`
pins, and `EN`/CHIP_PU — had never actually been wired to anything. Every
peripheral this session got carefully connected; the MCU's own supply
pins were overlooked. Fixed:
- `3V3` → `3V3_SYS`, all 7 `GND` pins → `GND`.
- `EN` → a proper RC delay circuit (R1 10kΩ to `3V3_SYS`, C18 1µF to
  `GND`), **R=10kΩ/C=1µF confirmed against Espressif's own ESP32-S3
  Hardware Design Guidelines** (fetched, not recalled from memory) —
  Espressif's guidance is explicit that CHIP_PU/EN must never be left
  floating.
- The `Device:R` symbol needed for R1 was pulled directly from KiCad's
  own installed `Device.kicad_sym` (same file the running `kicad-cli`
  uses), not reconstructed by analogy to `Device:C`.
- Also found and fixed: `#PWR01`, a leftover `power:GND` flag symbol
  from the original pre-consolidation schematic, was sitting completely
  unconnected (an artifact of the removed NPM1300 section) — tied to
  `GND`.

**Practical takeaway if you extend this schematic by hand-editing
`.kicad_sch` text again (rather than through KiCad's GUI):** for any
placement at angle 0 with no mirror, remember `abs_y = placement_y −
local_y`, not `+`. And prefer running `kicad-cli sch erc` on anything
non-trivial over trusting invariant checks alone — it catches a category
of bug (wrong absolute position, still syntactically valid, still
internally self-consistent) that paren-balance and UUID-uniqueness
checks structurally cannot.

### Power Architecture (current, as of 2026-09-11)

```
USB-C (on the TP4056 module itself, not separately represented)
        │
        ▼
   J1 — TP4056_Module (HiLetGo, w/ battery protection)
   BAT+/BAT- ──── VBAT ────  BT1 (3.7V 350mAh LiPo)
   OUT+/OUT- ──── VSYS ────► LD1 — LD1117V33 (SparkFun breakout)
                                    VOUT ──── 3V3_SYS ────► everything else
```

- **BT1**: 3.7V 350mAh Li-ion polymer battery.
- **J1 (TP4056_Module)**: complete self-contained charger board (own USB
  port, charge IC, status LEDs, battery protection) — represented in this
  schematic only by its 4 external connections (`BAT+`, `BAT-`, `OUT+`,
  `OUT-`). The module's internal USB port, caps, and LEDs are not modeled;
  there is nothing to wire for them.
- **LD1 (LD1117V33)**: fixed 3.3V linear regulator, takes the TP4056's
  `VSYS` output and produces the single `3V3_SYS` rail everything else
  runs on. Pin numbering follows the bare LD1117 TO-220/SOT-223 datasheet
  convention (1=GND, 2=VOUT, 3=VIN) even though the actual part is a
  breakout board.
- **This replaced an NPM1300 PMIC (Nordic nPM1300)** that was in the
  original pre-consolidation schematic, along with a placeholder USB-C
  connector added earlier in this session. Both were removed entirely
  (lib_symbol defs, instances, and their wiring) once TP4056 + LD1117V33
  was confirmed as the actual intended design — simpler, and correctly
  matches parts the user already has (HiLetGo TP4056, SparkFun LD1117V33).
- **Two previously-separate 3.3V rails were merged.** The original design
  had `3V3_SYS` (digital) and `3V3_AUDIO` (amp) as isolated rails, plus a
  `1V8_MIC` rail for the mics. All three are now one `3V3_SYS` rail from
  the single LD1117V33 — simpler, appropriate for a small battery-powered
  wearable where the amp isn't driving continuous high power. If audio
  noise from digital switching turns out to be audible in practice, that's
  the first place to reconsider (add a second regulator or an LC filter
  for the amp specifically).

### Capacitors (C1-C17) — full mapping

| Ref | Value | Purpose |
|---|---|---|
| C1 | 10µF | LD1117V33 VIN bulk |
| C2 | 10µF | LD1117V33 VOUT bulk |
| C3 | 100nF | LD1117V33 VIN local bypass |
| C4 | 100nF | LD1117V33 VOUT local bypass |
| C5 | 1µF | DRV2605 U3 REG pin |
| C6 | 1µF | DRV2605 U6 REG pin |
| C7 | 1µF | DRV2605 U7 REG pin |
| C8 | 1µF | DRV2605 U8 REG pin |
| C9 | 1µF | MIC1 VDD decoupling |
| C10 | 1µF | MIC2 VDD decoupling |
| C11 | 1µF | MIC3 VDD decoupling |
| C12 | 1µF | MIC4 VDD decoupling |
| C13 | 1µF | TCA9548A VCC decoupling |
| C14 | 1µF | MAX98357A VDD decoupling |
| C15 | 100nF | ESP32-S3 3V3 pin local bypass |
| C16 | 10µF | Bulk cap on `VSYS` (TP4056 output → LD1117V33 input), absorbs haptic-motor current transients |
| C17 | 1µF | ESP32-S3 3V3 pin bulk cap, paired with C15 — Espressif's WROOM-1 reference design recommends a bulk + local-bypass pair on the module's 3.3V pin, not a single cap |

Note the REG-pin value was corrected from an earlier 0.1µF guess to 1.0µF
— TI's DRV2605L datasheet actually specifies 1.0µF there. All 17 caps use
global labels (not direct wires) to reach their rail — consistent with
the rest of this schematic, and electrically identical for schematic-
capture purposes; physical proximity on the real board is a PCB-layout
concern, not a schematic one. All still have empty `Footprint` fields —
package/value are set, physical part selection is still open.

**Phase 2 cross-check result (2026-09-11): the schematic was not wired
at the signal level.** Components were correctly selected and placed (ESP32-
S3-WROOM-1-N16R8, 4× ICS-43434 mics, MAX98357AETE+T, DRV2605LDGS, NPM1300
PMIC, battery), and the power rails were wired (`VBUS_5V`/`VSYS`/`VBAT`/
`1V8_MIC`/`3V3_AUDIO`/`3V3_SYS`/`GND` global labels). But there were only 21
wire segments and **zero signal-net labels** anywhere in the sheet — no I2S
(WS/BCLK/DOUT) or I2C (SDA/SCL) connections between the ESP32 and any
peripheral, so there was nothing to cross-check against the firmware pin
table above. **This is still true** — see "still needed" below.

**Component-count mismatch fixed (2026-09-11):** the schematic had only
1× DRV2605LDGS (U3) and no TCA9548A mux, against the firmware's 4-driver-
behind-a-mux architecture. Added 3 more DRV2605LDGS instances (U6, U7, U8)
and 1 TCA9548A (U5, 24-pin TSSOP, pinout taken directly from TI datasheet
SCPS207F — not from memory) to `hardware/EchoSafe_RevA/EchoSafe_RevA/EchoSafe_RevA.kicad_sch`.
**These are placed but not yet wired** — same as every other signal net in
this schematic, connecting them (ESP32 SDA/SCL → mux upstream, mux SD0-3/
SC0-3 → each DRV2605's SDA/SCL, address pins to GND per the firmware's
mux-channel-0-3 mapping) is still open work.

Also fixed while in here: `sym-lib-table` had **always** pointed to
nonexistent files (verified against the original `~/Documents/EchoSafe_RevA/`
too — this predates the consolidation, not something the copy caused) and
two of its library nicknames didn't match the `lib_id` prefixes actually
used by placed symbols. Corrected to point at the real files with
`${KIPRJMOD}`-relative paths. Practical effect: previously, opening this
project in KiCad and trying to place a *new* instance of any of these 5
parts from the Symbol Library browser would have failed with a missing-
library error — already-placed symbols still displayed fine because
`.kicad_sch` caches placed symbol definitions inline, which is the only
reason this project has looked functional at all.

Still no standalone library file exists for `Driver_Haptic` (DRV2605LDGS,
now TCA9548A) — both live only as embedded/cached definitions in the
`.kicad_sch`, same pattern the original DRV2605LDGS already used before
this session. Works fine for what's placed; a real `Driver_Haptic.kicad_sym`
would be needed to place further instances from the Symbol Library browser
in KiCad itself.

**Signal wiring added (2026-09-11)**, using global labels (matching this
sheet's existing power-rail convention) rather than point-to-point wires,
since components are spread across the sheet. Every net below is a set of
same-named `global_label`s placed directly at each pin's connection point —
no drawn wire segments needed for correctness, this is standard KiCad
practice. Verified after insertion: whole-file parenthesis balance intact,
381 unique UUIDs (no duplicates), no two different net names sharing a
coordinate.

**Judgment calls made that you should verify against actual physical
intent** — none of this was encoded anywhere in the existing schematic, so
these were assumptions, not verified facts, until confirmed:

- **MIC1=Top-Left, MIC2=Top-Right, MIC3=Bottom-Right, MIC4=Bottom-Left**
  (clockwise, confirmed 2026-09-11). The schematic gave no positional/
  naming hint about which placed mic instance is physically which
  quadrant — this ordering was confirmed against actual intended physical
  layout. MIC3's SEL pin → `1V8_MIC` (right channel, it's Bottom-Right)
  and MIC4's SEL pin → `GND` (left channel, it's Bottom-Left) — the
  reverse of the original guess, corrected once the real layout was known.
- **M1=Top-Left, M2=Top-Right, M3=Bottom-Right, M4=Bottom-Left** motors,
  same clockwise convention applied for consistency with the mics.
- Driver-to-channel mapping follows the firmware's own fixed TCA9548A
  channel table (channel 0=Top-Left, 1=Top-Right, 2=Bottom-Left, 3=
  Bottom-Right) — U3 stays on channel 0 (TL), U6 on channel 1 (TR), but
  since M3/M4 turned out to be swapped from the initial guess, **U7
  (channel 2, "Bottom-Left") now drives M4** and **U8 (channel 3,
  "Bottom-Right") now drives M3** — the driver-to-channel assignment
  didn't change, only which physical motor each driver's OUTP/OUTN
  connects to.
- **Rail assignments**: DRV2605 (×4), TCA9548A, MAX98357A, and mic power/
  enable pins all → `3V3_SYS`. Originally split across three separate
  rails (`3V3_SYS`/`3V3_AUDIO`/`1V8_MIC`) inferred from the pre-existing
  NPM1300-era rail names; consolidated to one rail once the power
  architecture was replaced with TP4056 + a single LD1117V33 — see
  "Power Architecture" above.
- **TCA9548A address pins A0/A1/A2 → GND** (address 0x70, per firmware's
  `TCA_ADDR`). **RESET → `3V3_SYS` directly** — datasheet recommends a
  pull-up resistor rather than a direct tie; direct tie was used here as a
  simplification since firmware never asserts a hardware reset on this
  line. Add a resistor if you want to match the datasheet exactly.
- **DRV2605 IN/TRIG → GND** (firmware uses `DRV2605_MODE_INTTRIG` via I2C,
  so this pin's physical state doesn't matter functionally, but leaving a
  digital input floating is bad practice).
- **DRV2605 EN → `3V3_SYS`** (always-enabled; firmware has no hardware
  enable/disable control for haptics).

**Speaker architecture (1 amp vs. 2, decided 2026-09-11):** keeping the
current design — 1× MAX98357A driving both speakers (LS1, LS2) in
parallel off the same `SPK_OUTP`/`SPK_OUTN` net — rather than adding a
second amp IC. Reasoning: both speakers only ever need to play the same
mono alert content (no independent/stereo audio requirement), so a second
amp buys nothing functionally; it would cost board area, BOM cost, and
idle quiescent current, all of which matter more for a battery-powered
wearable than they would for a mains-powered product. The real constraint
is electrical: MAX98357A is only rated for ≥ ~4Ω BTL load, and two
speakers in parallel add their impedances in parallel — **this only stays
safe if each speaker is 8Ω** (giving 4Ω combined). If the speakers already
chosen/available are 4Ω each, parallel wiring drops to ~2Ω, which is
out of the amp's safe operating range (risk of thermal shutdown, reduced
headroom, or long-term amp stress under sustained loud output) — in that
case, either swap to two 8Ω speakers or fall back to a second MAX98357A
(each driving one 4Ω speaker independently; a second amp can still listen
to the same shared `SPK_DIN`/`SPK_BCLK`/`SPK_LRCLK` I2S lines from the
ESP32, since I2S is a broadcast bus — no second I2S peripheral needed).
**Action item: confirm actual speaker impedance before finalizing.**

**Speaker impedance confirmed (2026-09-11): speakers are 8Ω.** With
2× 8Ω in parallel = 4Ω, the single-MAX98357A design is within its rated
load — no change needed, decision above stands as final, not conditional.

**USB-C charging (2026-09-11, superseded same day):** a placeholder bare
USB-C connector wired directly to the NPM1300's CC1/CC2/VBUS pins was
added first, then removed once TP4056 + LD1117V33 was confirmed as the
actual design — see "Power Architecture" above. The TP4056 module has its
own onboard USB port; no separate connector symbol is needed.

No physical power switch or user-facing button were added — confirmed
these aren't part of this design (only the mechanical concept image
suggested them, and that image was explicitly disregarded as outdated).

**Note on the two reference images checked during this session:**
`EchoSafe_v1_mech_layout.png` is outdated/disregarded per your feedback.
`EchoSafe.png` (Shokz-style render) is close to the intended final look,
**except this design has no separate in-ear buds** — the two speakers
(LS1/LS2) mount inside the behind-ear housings themselves, not on a
separate earbud cable.

**Still needed before this can go to PCB layout:**
1. ~~Wire the actual I2S/I2C signal nets~~ — done.
2. ~~REG decoupling caps~~ — done (C5-C8, corrected to 1µF).
3. ~~Resolve 1-amp-vs-2-amp~~ — done, confirmed final with 8Ω speakers.
4. ~~Verify MIC/motor quadrant assignments~~ — done.
5. ~~Settle charging/power architecture~~ — done: TP4056 + LD1117V33,
   single 3V3_SYS rail (see "Power Architecture" above).
6. ~~Pick real footprints for every part~~ — done (2026-09-12), including
   LS1/LS2 as of 2026-09-13 (Adafruit #4227 / DigiKey 1528-4227-ND) —
   see "Footprints" section below for the full breakdown and what's still
   worth double-checking (JST-SH exact library name, TP4056's real hole
   spacing).
7. ~~Define board partition~~ — confirmed (2026-09-11): **4 small earpiece
   modules + 1 central pod**, not one monolithic board:
   - Front-Left module: MIC1, M1, LS1 (speaker), + MIC1's own decoupling
     cap (C9) — travels with the mic, not centralized (decoupling only
     works if it's physically close to the pin it protects)
   - Front-Right module: MIC2, M2, LS2 (speaker), + C10
   - Rear-Right module: MIC3, M3 (no speaker), + C11
   - Rear-Left module: MIC4, M4 (no speaker), + C12
   - Central pod (everything else): U1 (ESP32), U2 (MAX98357A), U5
     (TCA9548A), U3/U6/U7/U8 (DRV2605 ×4), J1 (TP4056), LD1 (LD1117V33),
     BT1 (battery), remaining 13 capacitors (C1-C8, C13-C17)
   ~~Harness connectors added (2026-09-11)~~ — J2 (Front-Left, 9 pins),
   J3 (Front-Right, 9 pins), J4 (Rear-Left, 7 pins), J5 (Rear-Right,
   7 pins). Schematic symbol is still a generic placeholder
   (`Connector:Conn_Harness_09`/`_07`), but each now has a real
   **footprint** assigned (JST-SH, 1.0mm pitch — see "Footprints"
   section below). Each carries its own full copy of the shared mic I2S bus (**star topology, confirmed
   2026-09-11** — separate cable per module rather than one cable
   daisy-chaining through both front, or both rear, modules) plus that
   module's motor drive and, for the front pair, speaker drive:
   - J2/J3 (front, 9 pins each): WS, BCLK, DOUT, 3V3_SYS, GND, motor
     OUTP, motor OUTN, speaker OUTP, speaker OUTN
   - J4/J5 (rear, 7 pins each): WS, BCLK, DOUT, 3V3_SYS, GND, motor
     OUTP, motor OUTN
   Each mic's SEL pin does **not** need its own harness wire — it's a
   static GND/3V3 tie, wired locally on the earpiece module itself using
   the 3V3_SYS/GND already carried there for the mic's own power.
   Star topology means the *net* is identical either way (both mics on a
   pair always tied to the same ESP32 pins); the choice only affects
   physical cable routing and how many pins the central pod's side of
   the harness needs (double, since each shared signal gets its own
   dedicated cable run instead of one continuous daisy-chained wire).
   This schematic is still captured as one flat sheet — the connectors
   document the board boundary and exact wire count, but there aren't
   yet separate KiCad projects/sheets per physical board (see below).
8. ~~Split into actual separate KiCad projects per physical board~~ —
   done (2026-09-12). See "Multi-Board Project Structure" below.
9. ~~Verify all 5 schematics with real ERC~~ — done (2026-09-13). Caught
   and fixed a session-wide Y-axis coordinate bug plus an unpowered
   ESP32 — see "Critical fix" section above. All 5 projects now verified
   at 0 dangling labels / 0 genuinely-unconnected pins.
10. ~~PCB layout itself~~ — done (2026-09-15). All 5 projects now have
    footprints placed, fully routed, and verified at **0 DRC violations
    / 0 unconnected pads** via `kicad-cli pcb drc`. See "PCB Layout"
    below for how the central pod (by far the largest/densest board) was
    routed.

### PCB Layout (2026-09-15)

All 5 boards are placed, routed, and DRC-clean:

- **The 4 module boards** (`EchoSafe_FrontLeft/Right`,
  `EchoSafe_RearLeft/Right`) — small (4-5 component) boards, routed
  directly and verified clean early in this phase.
- **The central pod** (`EchoSafe_RevA`) — ~22 components, ~150 net
  connections, much denser. Hand-written/scripted routing (via pcbnew's
  Python API) oscillated between 300-600 DRC violations without
  converging, so this board was routed instead with **FreeRouting**
  (open-source autorouter, v2.4.1): exported the board to Specctra
  `.dsn`, ran FreeRouting headless (`java -jar freerouting.jar -de
  reva.dsn -do reva_routed.ses -mp 30 -mt 1`), then imported the
  resulting `.ses` back via `pcbnew.ImportSpecctraSES()`. This got the
  board from 300-600 violations down to 3 violations + 3 unconnected
  pads (4 nets FreeRouting couldn't complete) — the remaining gaps were
  fixed by hand, one at a time, by querying the exact pad/track geometry
  around each gap via pcbnew's Python API (never trusting remembered
  coordinates — the board changes after every fix) and routing around
  obstacles with explicit clearance math.
  **Key lesson from the last, hardest gap (SPK_OUTN on U2, a MAX98357A
  QFN16 at 0.5mm pin pitch):** don't double-count pad half-thickness —
  the required clearance from a trace centerline to a pad is
  `trace_half_width + netclass_clearance` measured from the pad's own
  *edge*, not from its center with pad half-width added a second time.
  Getting this wrong made an achievable route look impossible. The
  actual blocker turned out to be a neighboring net's *via* (larger than
  a pad, needs `via_radius + trace_half + clearance`), solved by
  necking the trace down to 0.1mm width for just the ~1.5mm stretch
  needed to clear it, then widening back to the normal 0.3mm afterward —
  standard practice for tight QFN escapes, and something FreeRouting's
  own successful traces in this exact area were already doing (0.15-
  0.2mm, not 0.3mm) as a clue this session initially missed.
  Java (`brew install openjdk`, keg-only at `/opt/homebrew/opt/openjdk/`)
  and FreeRouting itself were installed to make this possible — see git
  history for the full sequence of fix scripts if reconstructing this
  process is ever needed again.

### Multi-Board Project Structure (2026-09-12)

`hardware/` now holds **5 independent KiCad projects**, not one:

```
hardware/
  EchoSafe_RevA/            <- CENTRAL POD (original project, kept as-is)
    lib/symbols/            <- shared library, used by ALL 5 projects
    EchoSafe_RevA/
      EchoSafe_RevA.kicad_pro / .kicad_sch / sym-lib-table
  EchoSafe_FrontLeft/       <- NEW: MIC1, M1, LS1, C9
  EchoSafe_FrontRight/      <- NEW: MIC2, M2, LS2, C10
  EchoSafe_RearLeft/        <- NEW: MIC4, M4, C12
  EchoSafe_RearRight/       <- NEW: MIC3, M3, C11
```

The 4 new module projects are deliberately simple: each is one small
`.kicad_pro`/`.kicad_sch` pair at the top level of its folder (not the
central pod's double-nested `EchoSafe_RevA/EchoSafe_RevA/` layout —
that nesting wasn't worth replicating for a 4-5-component board). Each
has its own `sym-lib-table` pointing back at the **shared** library under
`EchoSafe_RevA/lib/symbols/` via `${KIPRJMOD}/../EchoSafe_RevA/lib/symbols/...`
— nothing was duplicated, all 5 projects read the same symbol files.

**What moved out of the central pod:** MIC1-4, M1-4, LS1-2, and each
mic's own decoupling cap (C9-C12) were removed from
`EchoSafe_RevA.kicad_sch` entirely (instances *and* their pin-level
labels) and re-created fresh in their respective module project, with
new UUIDs (each project has its own independent UUID namespace — there's
no cross-project uniqueness requirement, unlike within one file). The
central pod's copy of every shared net (`MIC_TOP_WS`, `MOTOR_TL_OUTP`,
`SPK_OUTP`, etc.) lost exactly the endpoints that moved out and kept the
ones that stayed (verified net-by-net, not just paren-balance).

**Each module schematic is self-contained and small** — its mic, motor,
(front only) speaker, decoupling cap, and one `J1` connector (9-pin front
with speaker pins, 7-pin rear without), wired with the **same net names**
as the corresponding pod-side connector (J2/J3/J4/J5). Matching names
across independent projects is a human/documentation convention here —
KiCad doesn't automatically link separate projects' netlists. The actual
electrical connection between a module and the pod is the physical cable;
these matching connectors + net names are what tell you how to build it.

### Footprints (2026-09-12, mic footprint replaced 2026-09-13)

Target: **compact near-final wearable** (confirmed with the user — not an
early bring-up prototype), so choices below favor small/soldered over
socketed/header where there was a real choice to make.

**Mic footprint (`LGA_CAV_IVS`) replaced entirely — it didn't match the
real part.** Discovered while starting PCB placement: the original
SnapEDA-imported `LGA_CAV_IVS.kicad_mod` (pre-dates this session) has pad
3 (GND) as a tiny 0.127×0.127mm corner pad, and a non-electrical
mechanical hole occupying pad number "6" — pushing the real 6th
electrical pad to number "7", one off from the schematic symbol's pin 6
(SD). Checked against InvenSense's actual datasheet (DS-000069 Rev 1.0,
fetched and read this session, not recalled): GND is really a large ring
pad (Ø1.025mm inner / Ø1.625mm outer) surrounding the microphone's
acoustic port, nothing like what the footprint had.

Authored a new footprint (`ICS-43434_LGA6`, in a new library
`Mics_Corrected.pretty` alongside the original `lib/symbols/` folder)
using KiCad's own `pcbnew` Python API (bundled with this KiCad install,
not hand-written S-expressions) directly from the datasheet's Figure 3
(pin positions) and Figure 13 (land pattern dimensions): 5 roundrect
signal pads (WS/LR/SCK/VDD/SD) at the datasheet's stated 0.6×0.9mm size
and 0.9mm pitch, plus GND as a solid circular pad at the ring's outer
diameter (1.625mm) — a deliberate simplification from the true annular
ring, which is electrically equivalent but **does not include the
board-level acoustic port hole** the datasheet also specifies (a drilled/
routed opening through the PCB itself, ≥0.5mm diameter, centered under
the GND pad) — that's a PCB-outline-level feature, not something a
footprint file can express, and must be added by hand once real layout
starts. Round-trip verified by reloading the saved file through
`pcbnew.FootprintLoad()` and checking pad count/positions/shapes match.

Updated: `fp-lib-table` in all 4 module projects to register the new
library, and the `Footprint` property on MIC1-4 to
`ICS-43434:ICS-43434_LGA6`. Re-ran ERC on all 4 modules afterward —
unchanged (9/9/8/8), confirming this was purely a physical/PCB-level fix
with no effect on schematic connectivity, as expected.

**Fixed a regression first:** splitting into 5 projects had wiped the
mic (`LGA_CAV_IVS`) and motor (`C0720B001F:XDCR_C0720B001F`) footprints
that already existed pre-split — my instance generator set every new
component to an empty footprint unconditionally. Restored both from the
pre-split commit.

**Also fixed: footprint library registration never existed, in any
version of this project.** Same class of bug as the `sym-lib-table` gap
found earlier — `LGA_CAV_IVS`, `ESP32-S3-WROOM-1_EXP`, and
`21-0136I_T1633-4_MXM` were all bare names with no `Library:` prefix, and
there was no `fp-lib-table` file anywhere to register a library even if
there had been. Created `fp-lib-table` in the central pod (registering
`ESP32-S3-WROOM-1` and `MAX98357AETE-T`) and in all 4 module projects
(registering `ICS-43434` and `C0720B001F`), then added the missing
`Library:` prefixes so the references actually resolve. Verified every
path resolves to a real `.kicad_mod` file on disk.

**Already-correct, unchanged:** DRV2605LDGS ×4 and TCA9548A already had
standard-library footprints from earlier this session
(`Package_SO:VSSOP-10_3x3mm_P0.5mm`, `Package_SO:TSSOP-24_4.4x7.8mm_P0.65mm`).

**Newly assigned:**
| Part | Footprint | Basis |
|---|---|---|
| All 17 capacitors | `Capacitor_SMD:C_0603_1608Metric` | 0603 — small enough for a wearable, still hand-solderable |
| BT1 (battery) | `Connector_JST:JST_PH_S2B_PH_K_1x02_P2.00mm_Horizontal` | Confirmed with user: this battery ships with a pre-attached JST-PH connector |
| LD1 (LD1117V33 breakout) | `Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical` | Represents the breakout's actual 0.1" hole pattern — used regardless of whether pins or direct wire are soldered in |
| J1 central pod (TP4056 module) | `Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical` | Placeholder for BAT+/BAT-/OUT+/OUT- pad pattern — **verify against the actual purchased HiLetGo board**, hole spacing varies between board revisions sold under that listing |
| J2/J3 (pod, 9-pin) + FrontLeft/FrontRight module J1 | `Connector_JST:JST_SH_BM09B-SRSS-TB_1x09-1MP_P1.00mm_Horizontal` | JST-SH (1.0mm pitch) chosen for the compact-wearable target — 9 conductors at 2.54mm would be too bulky for a headband |
| J4/J5 (pod, 7-pin) + RearLeft/RearRight module J1 | `Connector_JST:JST_SH_BM07B-SRSS-TB_1x07-1MP_P1.00mm_Horizontal` | Same reasoning, 7-pin variant |

**JST-SH names verified (2026-09-13).** Fetched the actual files from
KiCad's own footprints repo instead of relying on the earlier unverified
guess — and the guess was wrong: it used a `BM0xB-SRSS-TB` prefix: the
real files use `SM0xB-SRSS-TB`. Confirmed both exist with the right pad
count (`JST_SH_SM09B-SRSS-TB_1x09-1MP_P1.00mm_Horizontal` = 9 signal pads
+ 2 mounting pads, `JST_SH_SM07B-SRSS-TB_1x07-1MP_P1.00mm_Horizontal` =
7 signal pads + 2 mounting pads) and fixed all 8 occurrences across all
5 projects (J2/J3/J4/J5 in the central pod, one `J1` per module).

**TP4056 pin spacing checked (2026-09-13):** couldn't pin down exact
dimensions for the specific HiLetGo listing, but multiple independent
sources agree 2.54mm/0.1" through-hole pitch is standard across common
TP4056 module variants (some SMD-pad variants exist too) — the
`PinHeader_1x04_P2.54mm_Vertical` placeholder already assigned matches
the common case. Still worth a 30-second check against the physical
board once it's in hand, since "TP4056 HiLetGo" covers slightly
different board revisions.

**LS1/LS2 (speakers) assigned (2026-09-13):** [DigiKey 1528-4227-ND](https://www.digikey.com/en/products/detail/adafruit-industries-llc/4227/10245140)
= Adafruit #4227 "Mini Oval Speaker," 8Ω, 1W, 30×20×5mm, verified via
DigiKey/Adafruit's own listings (not from memory). Notably, **this part
connects via a Molex PicoBlade 1.25mm-pitch 2-pin connector on a ~10mm
cable, not bare wire leads or solder pads** — changes the footprint from
what a generic "speaker" assumption would have used. Footprint set to
`Connector_Molex:Molex_PicoBlade_53261-0271_1x02-1MP_P1.25mm_Horizontal`
(the mating receptacle for that connector), confirmed against KiCad's
own official footprint library source, not guessed like the JST-SH name
above. Also worth noting: Adafruit's own product page recommends this
exact speaker for use with MAX98357A — which is what this design already
uses, good independent confirmation the part choice fits.

`#PWR01` (a GND power-flag symbol) correctly has no footprint — power
symbols never do, that's not a gap.

---

## Important Constraints

- Always include a `noise` class when retraining; omitting it causes every
  background sound to be misclassified as a target (learned the hard way).
- Label names must stay consistent (snake_case) across all collection
  sessions.
- `model_weights.h` is **auto-generated** by `retrain.py` — never hand-edit.
  The C symbol names (`LAYER0_WEIGHTS`, `SCALER_MEAN`, etc.) are hardcoded
  in the firmware; don't rename without updating both sides.
- `ml/echosafe_dataset.npz` is the only copy of 1059 labeled samples —
  never overwrite without a backup.
- Do not push to the team repo without asking first, every time.
- Do not delete the original `EchoSafe_ML` / `EchoSafe_RevA` /
  `EchoSafe_Audio` / `EchoSafeTesting` folders under `~/Documents/` — they're
  the safety net until this consolidated repo is validated as complete. See
  `CONSOLIDATION_NOTES.md`.
