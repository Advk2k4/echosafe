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
  placed, routed, DRC-clean PCB layouts** (0 violations at full DRC
  severity — including the silkscreen-specific `silk_over_copper`/
  `silk_overlap`/`text_height` checks, not just the default error-level
  set — 0 unconnected pads on every board, verified via `kicad-cli pcb
  drc --severity-all`) — see "PCB Layout", "Central Pod Resize", and
  "Visual/Silkscreen Review" under the `hardware/` section below for how
  the central pod (ESP32-S3-WROOM-1, MAX98357A, TCA9548A, 4× DRV2605L,
  TP4056 + LD1117V33 power chain) was routed, re-placed/re-routed from an
  initially oversized 260×175mm layout down to a genuinely compact
  65×90mm one, and then visually audited for silkscreen legibility and
  footprint-library hygiene. **Fab outputs (gerbers, drill files, CPL,
  BOM) are now generated for all 5 boards** — see "Fab Outputs" below.
  Nothing has been physically fabricated yet.
  **Not yet ready to actually order boards** — still needed: physical
  confirmation of the TP4056 module's real footprint spacing and of the
  mic module's re-derived pad geometry (see "Footprints" below —
  acoustic port holes are done, but both footprints are still
  datasheet-derived, not measured against real parts). Bring-up right
  now still has to happen on a breadboard or dev-kit with jumper wiring,
  following the schematic's part choices and the pin mapping documented
  below.

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

**I2S_NUM_1 mode-switch code review (2026-09-17).** This port is time-
multiplexed between BOT-mic RX (44.1kHz stereo, via `init_dir_mic()`) and
speaker TX (via `init_speaker()`), restored afterward by
`restore_bot_mic()`. Long flagged in this file's own header comment as a
"plausible source of bugs" but never actually reviewed until now — no
hardware needed, this was a pure code-tracing exercise (arduino-cli isn't
installed on this machine, so verified by careful manual trace + brace/
paren balance check, not an actual compile). Traced every call site
(`capture_and_classify()`'s Phase 3, the `'d'`/`'p'`/`'t'` serial
commands) end to end. **The core switching logic is correct** — every
normal path through `play_wav()`/`play_tone_440()` does call
`restore_bot_mic()` before returning, and the state machine is
self-consistent from `setup()` onward. Found and fixed 3 real gaps
along the way, none of them the "leaves it stuck in the wrong mode on
every playback" bug the header comment worried about, but all real:

1. **On `init_speaker()` failure, I2S_NUM_1 was left with no driver
   installed at all**, in neither RX nor TX mode — `init_speaker()`
   unconditionally calls `i2s_driver_uninstall(SPK_PORT)` *before*
   attempting the install, so a failed install still tore down the
   previous (working) RX config, and both `play_wav()` and
   `play_tone_440()` returned early on that failure without calling
   `restore_bot_mic()`. Self-healing (the next cycle's Phase 2 always
   calls `init_dir_mic(MIC_BOT_PORT,...)` again regardless), so the
   impact was at most one broken direction-detection cycle, only on the
   rare path where an I2S driver install actually errors — but a real
   "peripheral left in the wrong state" bug of exactly the kind
   speculated about. Fixed: both functions now call `restore_bot_mic()`
   on that failure path.
2. **`init_speaker()` was missing the settling `delay(50)`** that both
   `init_dir_mic()` and `init_ml_mic()` have after `i2s_set_pin()` — an
   unexplained inconsistency between the three I2S init functions in
   this file, not something the code or a comment justified. Whether it
   caused an audible glitch on the first few samples of an alert can't
   be confirmed without real hardware, but there was no reason for the
   asymmetry. Fixed: added the same `delay(50)`.
3. **`play_wav()` parses the WAV header's `channels`/`bits` fields but
   never validates them** — playback unconditionally assumes 16-bit
   stereo. Checked the real files in `firmware/data/`: all 4 currently
   are 16-bit stereo, so this wasn't live, but it's a silent trap —
   replace any alert WAV with a mono or different-bit-depth file (an
   easy mistake re-exporting from an audio editor) and it would play
   back garbled with no error at all. Fixed: `play_wav()` now rejects
   anything that isn't 16-bit stereo with a clear Serial error instead
   of attempting playback.

None of these 3 fixes have been tested on real hardware (none exists in
this project's bring-up state yet — see "Current Status" above); they
were verified by code-tracing at the time, and since then by a real
compile too — see "Build Verification (arduino-cli)" below. They should
still be exercised via the `p` (speaker test) and `d`/`i` (direction/
inference, which exercise the RX side) serial commands during actual
bring-up — a clean compile confirms the code is well-formed, not that
the logic is correct on real hardware.

### Build Verification (arduino-cli, 2026-09-18)

Nothing in `firmware/` had ever actually been compiled — every review up
to this point (including the I2S fix above) was manual code-tracing
only, since no Arduino toolchain existed on this machine. Installed one
(`brew install arduino-cli`), added the ESP32 board package
(`espressif/arduino-esp32` index), installed the `esp32:esp32` core
(v3.3.12) and the Adafruit DRV2605 Library (already present), then
compiled every real sketch in `firmware/` against the actual target
hardware — **`esp32:esp32:esp32s3` with `FlashSize=16M,PSRAM=opi`**,
matching the ESP32-S3-WROOM-1 **N16R8** module this project specs
(16MB flash, octal/OPI 8MB PSRAM) — not just the board family default.

**`echosafe_full_system.ino`** (the primary target, using its own
`partitions.csv` via `PartitionScheme=custom` — confirmed this is how
the ESP32 Arduino core actually picks up a sketch-local partitions file,
not assumed) — **compiles clean.** 626,711 bytes (3%) flash, 99,340
bytes (30%) RAM. One warning, not introduced by any fix here and not
fixed: `driver/i2s.h`, the API this entire file is built on
(`i2s_driver_install`/`i2s_read`/`i2s_write`/etc., used throughout, not
a small piece to swap out), is now marked deprecated by Espressif in
favor of `driver/i2s_std.h`/`i2s_pdm.h`/`i2s_tdm.h`. Still compiles and
works today — this is a forward-looking maintenance note, not a bug:
a future ESP32 core release could remove the legacy API outright, which
would break the build. Worth a migration pass eventually, not urgent.

**`echosafe_inference.ino`** — compiles clean (552,163 bytes / 3%
flash, 63,556 bytes / 19% RAM), same single pre-existing i2s.h
deprecation warning, same non-issue.

**`echosafe_feature_collector.ino`** — compiles clean (307,499 bytes /
23% flash, 59,076 bytes / 18% RAM) against `FlashSize=16M` with the
*default* partition scheme (no custom `partitions.csv` in this sketch's
folder, and it doesn't need one — no LittleFS/WAV usage, just serial
streaming). Same i2s.h warning.

**`uploadLittleFS.ino`** — not part of the original ask (it's a
"development/debug utility only" tool per its own description above),
checked anyway since the toolchain was already set up, and found 2 real,
independent warnings, both fixed:
- `server.available()` is deprecated in the current ESP32 core in favor
  of `.accept()` (a rename, same behavior) — updated the call.
- A `Serial.printf` format-string mismatch: `%d` used for
  `ESP.getFreeHeap()`, which returns `uint32_t`. Fixed to `%lu` (not
  `%u` — `uint32_t` is `long unsigned int` on this platform, confirmed
  by the compiler still warning on the `%u` attempt before landing on
  `%lu`, not guessed). Undefined behavior in principle, though harmless
  in practice on a platform where both are 4 bytes — worth fixing
  regardless since the compiler flags it for a reason.
**Moved into its own sketch folder (2026-09-22):** this sketch's file
used to sit directly in `firmware/` rather than in
`firmware/uploadLittleFS/`, so arduino-cli (and the Arduino IDE)
couldn't compile it in place — they look for `firmware/firmware.ino`
and fail. At the time this was worked around by compiling a copy from
a correctly-named temp folder rather than fixed for real, since it was
a structural reorganization beyond what that pass's "compile and
check" scope covered. Now actually moved (`git mv
firmware/uploadLittleFS.ino firmware/uploadLittleFS/uploadLittleFS.ino`,
preserving history) and reconfirmed it compiles clean directly from
its new location — no temp-folder workaround needed anymore.

**Net result: all 4 real sketches in `firmware/` compile cleanly** for
the actual target hardware (ESP32-S3-WROOM-1 N16R8), with only the one
pre-existing, non-urgent legacy-API deprecation warning shared across
the 3 that use `driver/i2s.h`. This confirms the code is well-formed
C++ that a real ESP-IDF toolchain accepts — it does not confirm the
firmware actually works correctly on real hardware, which still hasn't
happened (see "Current Status" at the top of this file).

### TDOA / Direction-Detection Math Audit (2026-09-20/21)

`detect_direction()` and `xcorr_lag()` — the GCC-PHAT-style lag +
energy-ratio logic behind Phase 2 (see the file's own "Two-Phase
Operation" comment at the top) — had never been reviewed for
correctness, as opposed to the I2S mode-switching around it. No
hardware exists to test this against real audio, so every claim below
was verified numerically instead: either by direct mathematical proof,
or by porting the exact C logic to Python and running it against
synthetic signals with known ground truth (scripts not checked into
the repo, scratch-only, but the method and results are recorded here
since they're the actual evidence behind each fix).

**First, verified correct (not assumed):** the lag-sign convention.
`xcorr_lag(a, b, n)`'s doc comment claims "positive lag → a leads b →
sound came from a's side." Rederived this from scratch via the
underlying physics (if mic A is closer to the source, sound arrives at
A first, so the *same* acoustic event appears at a *later* buffer index
in B than in A by exactly the propagation-time difference) rather than
trusting the comment, and confirmed the code's `a[i] * b[i+lag]`
correlation does peak at `lag = (b's delay) − (a's delay)`, matching
the claimed convention exactly. Cross-checked against the physical mic
layout too: `xcorr_lag(g_tl, g_tr, ...)` with `left_top = lag_top > 2`
correctly implies "TL received the sound first → source is on the
left," consistent with TL/TR's real positions.

**Fix 1 — dead computation removed.** `xcorr_lag()` computed
`norm = sqrt(ea*eb)` (the two signals' total energy) and divided every
lag candidate's correlation by it before comparing. Since `norm` is the
*same* constant for every candidate in the `argmax` search, dividing
every value in a set by the same positive constant can never change
which one is largest — and the divided value was never used for
anything except that comparison (only `best_lag` is returned). It was
two full 1024-sample energy-sum passes plus a `sqrtf()`, per call, for
zero effect on the output. Removed. Verified three ways: (1) the
argmax-invariance argument itself, (2) a 200-trial simulation
(synthetic signal + a known injected sample-shift per trial, spanning
the full ±24-sample `MAX_LAG` range) confirming the old and new
implementations return identical `best_lag` on every trial, *and* both
recover the injected true shift exactly every time, (3) recompiles
clean, 80 bytes smaller.

**Fix 2 — asymmetric top/bottom threshold, and the deeper bug it was
hiding.** `is_top = tb > 0.0f` vs. `is_bot = tb < -0.15f` — asymmetric,
unlike every other comparison in this function (e.g. the ±2-sample
dead-zone for left/right). Changed to a symmetric `tb > 0.15f` per
direct confirmation this wasn't an intentional design choice. But
simulating the *full* decision cascade before and after that one-number
change produced **identical** quadrant-selection statistics either way
— which is what actually caught the real bug: the cascade had fallback
branches (`else if (agree_left) quad = HAPTIC_TL;` and, further down,
`else if (left_top) quad = HAPTIC_TL;` / `else if (right_top) quad =
HAPTIC_TR;`) that fired whenever left/right was known but top/bottom
was *ambiguous* (neither `is_top` nor `is_bot`), and always defaulted
to a **top** quadrant — with no equivalent "default to bottom" branch
anywhere. Worse: `left_bot`/`right_bot` alone didn't even check
`is_top`/`is_bot` at all, just committed straight to a bottom quadrant
unconditionally. The bias lived entirely in this fallback structure,
independent of wherever `is_top`'s threshold happened to sit — which is
exactly why changing just the threshold number didn't move the
simulated statistics at all.

Restructured the cascade so every branch requires *both* a left/right
read and a confident top/bottom read before committing to a quadrant;
what's left ambiguous (left/right known but top/bottom isn't, or
neither) now falls through to the existing loudest-single-mic
tiebreaker (which weighs all 4 mics evenly) instead of guessing top.
`left_bot`/`right_bot` now get the same `is_top`/`is_bot` treatment
`left_top`/`right_top` already had, instead of skipping it. Verified
with a second simulation, this time of the actual restructured cascade
(not just the threshold), using a scenario designed to have a
genuinely unbiased ground truth (`agree_left` true, `tb` swept
symmetrically around 0, `rms_tl`/`rms_bl` drawn from the identical
distribution so the loudest-mic tiebreaker is a fair coin flip when it
fires): **50.3% TL / 49.7% BL** over 40,000 trials — statistically a
coin flip, vs. **65.9% TL / 34.1% BL** with the original cascade run
through the identical scenario. Recompiles clean.

Neither fix has been exercised on real hardware/real audio (none
exists in this project's bring-up state yet) — both are verified
against the algorithm's own logic and known synthetic ground truth,
not against a real human voice/siren/etc. arriving at a real 4-mic
array. Worth confirming with the `d` (direction test) serial command
once hardware exists, per the file's own testing commands.

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

### `firmware/uploadLittleFS/uploadLittleFS.ino`
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
# Use the venv, not a bare pip install -- see "ml/.venv" note below for why
cd ml && source .venv/bin/activate
# (first time only: python3 -m venv .venv && pip install "numpy<2" tensorflow scikit-learn pyserial matplotlib sounddevice soundfile)

# Retrain (reads echosafe_dataset.npz, writes ../firmware/echosafe_inference/model_weights.h)
python retrain.py

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

**`ml/.venv`, not documented before, added 2026-09-18.** `pip install ...
tensorflow` with no version pin, run today, installs the latest NumPy
(2.x) alongside TensorFlow 2.16.2, a combination that's actually broken —
TF 2.16.2's bundled code references `np.complex_`, an alias NumPy 2.0
removed. `import tensorflow` fails outright with that combo (confirmed on
this machine, not assumed). Rather than downgrade NumPy in the
machine's global Python install — which could break other, unrelated
projects depending on NumPy 2.x — created a venv in `ml/.venv` (already
covered by the repo's `.gitignore`, so it won't get committed) with
`numpy<2` pinned explicitly; pip then resolved `tensorflow==2.21.0`,
which imports cleanly against it. Use this venv for any `ml/` work
rather than the system Python.

### `retrain.py` Code Review (2026-09-18)

Reviewed and fixed 4 real issues, all verified by actually running the
script against the real 1059-sample dataset (in an isolated copy of the
repo's directory structure, not by touching the tracked
`model_weights.h` files — training isn't deterministic-by-default enough
to casually overwrite the checked-in model on every review pass; see the
reproducibility fix below) and then compiling `echosafe_inference.ino`
against the freshly-generated header to confirm real end-to-end
compatibility, not just "the script didn't crash":

1. **`OUTPUT_H_FILE` pointed at a directory that doesn't exist anywhere
   in this repo.** It was `"echosafe_inference/model_weights.h"`, a
   relative path — correct only if resolved from `ml/`, per the
   documented `cd ml && python retrain.py` usage, but there is no
   `ml/echosafe_inference/` anywhere in this project, only
   `firmware/echosafe_inference/`. Confirmed directly (`Path(...).parent.exists()`
   → `False`), not assumed. Practical effect: running `retrain.py`
   exactly as documented would train for the full 150 epochs and then
   **crash on the final line** trying to write to a nonexistent
   directory — never actually updating the real firmware file, directly
   contradicting this file's own "you do not need to manually copy the
   header anywhere" claim above. Fixed: `OUTPUT_H_FILE =
   "../firmware/echosafe_inference/model_weights.h"` — verified by
   actually running the corrected script and confirming the file lands
   in the right place.
2. **No guard against retraining without a `noise` class** — the
   "Important Constraints" section above documents this as a
   hard-learned lesson ("omitting it causes every background sound to
   be misclassified as a target"), but nothing in the script itself
   enforced it; the lesson lived only in documentation, not in the
   tool. Added `assert "noise" in label_map` right after `label_map` is
   loaded, with an error message explaining why.
3. **The exported header's own dataset-composition comment hardcoded a
   stale, wrong class mapping** — `(barks:..., noise:..., bells:...,
   sirens:..., horns:...)` used literal label-id numbers (1, 3, 4, 2, 0)
   matching an old dataset layout that no longer exists (the real,
   current `label_map` is `{horns:0, noise:1, bells:2, gunshots:3,
   sirens:4}` — confirmed by loading the actual `.npz`, not assumed).
   There's no class called "barks" in the current dataset at all. This
   never affected the model's actual behavior (the real training/export
   logic elsewhere in the script correctly used the dynamically-loaded
   `id_to_name` map throughout) — only this one comment line reverted to
   old hardcoded assumptions, but it's exactly the kind of thing someone
   would trust while debugging a model issue. Fixed to build the same
   string dynamically from `id_to_name`, like the rest of the script
   already does. Verified: a fresh run now prints `Classes: horns,
   noise, bells, gunshots, sirens` and `Dataset: 1059 samples
   (horns:201, noise:258, bells:200, gunshots:200, sirens:200)` —
   matching this file's own "Dataset" table above exactly.
4. **Retraining wasn't actually reproducible despite looking like it was
   trying to be.** The train/val/test split already used
   `random_state=42`, but nothing seeded the model's own weight
   initialization or dropout — two runs on identical data produced
   different models (confirmed: ran it twice, diffed the exported
   weights, they differed, and test accuracy varied 96.9% vs. 95.6%
   between runs). First fix attempt (`np.random.seed(42)` +
   `tf.random.set_seed(42)`) **did not actually fix it** — verified by
   running twice again and finding the weights still differed. Root
   cause: this project's Keras 3 keeps its own internal random
   generators for layer init/dropout, separate from raw NumPy/TF global
   state. Fixed properly with `keras.utils.set_random_seed(42)`, which
   seeds Python's `random`, NumPy, TensorFlow, and Keras's generators
   together — verified by running twice more and diffing the two
   exported `model_weights.h` files (excluding the timestamp line):
   byte-identical.

None of this touched the real, tracked `firmware/echosafe_inference/model_weights.h`
or `firmware/echosafe_full_system/model_weights.h` — all verification
ran against an isolated copy. Actually retraining the shipped model is a
deliberate action for the user to take when ready, not a side effect of
a code review.

### `train_on_esp32_features.py` Code Review (2026-09-18)

This is the "parametric training with more control" path from the "Key
commands" table above, run with `--export-c` to produce a firmware-ready
header. Found the single most serious bug in the ML pipeline review so
far, plus 3 more, all verified the same way as `retrain.py` — running
the real script end to end against the real dataset, then compiling
firmware against the result:

1. **`--export-c` produced a header that shares zero identifiers with
   what the firmware actually needs — it would never have compiled.**
   The old `_generate_c_code()` emitted a generic `NUM_LAYERS` /
   `layer_1_weights` / `layer_1_biases` format with an `MODEL_WEIGHTS_H`
   include guard. Checked directly what
   `firmware/echosafe_full_system/echosafe_full_system.ino` actually
   references from this header (`grep` for every identifier, not
   assumed): `NUM_CLASSES`, `INPUT_DIM`, `CLASS_NAMES`, `SCALER_MEAN`,
   `SCALER_SCALE`, `LAYER0_INPUT/OUTPUT/WEIGHTS/BIAS`,
   `LAYER2_INPUT/OUTPUT/WEIGHTS/BIAS`, `LAYER4_INPUT/OUTPUT/WEIGHTS/BIAS`
   — none of which the old export produced. Every one of this project's
   two firmware targets would have failed to compile with "undefined
   identifier" errors if anyone actually used this documented
   `--export-c` flag. Rewrote `export_for_esp32()`/`_generate_c_code()`
   to emit exactly the format `retrain.py` does (same names, same
   include guard, same `CLASS_NAMES`/`SCALER_MEAN`/`SCALER_SCALE`
   derivation from `self.label_map`/`self.scaler`, same 3-layer
   structure) — verified by running `--export-c` for real and compiling
   `echosafe_inference.ino` against the output: clean compile, 3% flash.
2. **Added a matching safety check the old code had no equivalent of:**
   the firmware's `run_inference()` has exactly 3 hardcoded `dense()`
   calls (LAYER0→LAYER2→LAYER4) — it is not generic over layer count.
   `train_on_esp32_features.py` lets you pass `--hidden` with any number
   of layer sizes, so a non-default architecture (e.g. `--hidden 128 64
   32`, 3 hidden + 1 output = 4 Dense layers) would previously have
   exported *something* with no warning that it doesn't match what the
   firmware can actually run. `export_for_esp32()` now asserts exactly 3
   Dense layers and raises a clear error naming the mismatch instead —
   verified by actually running with `--hidden 128 64 32` and confirming
   it fails loudly (`ValueError: Expected exactly 3 Dense layers... got
   4`) rather than silently writing a bad header.
3. **`plt.show()` after `plt.savefig()` hung the script indefinitely
   in this environment, silently, right before the `--export-c` step —
   confirmed twice, not theorized.** The plot save (`savefig`) always
   completed and the resulting PNG existed on disk, but the process
   then sat with static CPU usage indefinitely; killed it and found
   `export_for_esp32()` had never run either time. Root cause: no
   interactive display backend is available here, and `plt.show()`
   blocks waiting for a window-server connection that never arrives.
   This isn't specific to this sandbox — the same thing happens on any
   headless box (SSH, CI, a container) with no `$DISPLAY`, which is a
   completely plausible way to run a training script. Fixed: `show()`
   is now only attempted when both (a) there's no `save_path` to fall
   back on and (b) matplotlib's active backend is a real interactive
   one (checked against an explicit allow-list, e.g. `TkAgg`/`MacOSX`/
   `Qt5Agg`) — otherwise it prints where the plot was saved instead of
   trying to show it. `main()` always passes `save_path`, so in the
   documented CLI usage this never even reaches the backend check.
4. **Same `noise`-class guard and reproducibility fix as `retrain.py`**
   (`assert "noise" in self.label_map`, `keras.utils.set_random_seed(42)`
   in place of nothing) — this script had neither, despite training the
   same way on the same kind of data. Not independently re-verified for
   byte-identical reproducibility the way `retrain.py` was (that check
   was already done once on the same underlying mechanism); ported for
   consistency between the two training paths.

### `serial_logger.py` Code Review (2026-09-18)

This is where `label_map` — the class-name-to-ID mapping every other ML
script and the firmware itself (`WAV_FILES[]`, indexed positionally)
trust without re-checking — actually gets decided, one typed label at a
time (`get_label_id()` assigns the next integer to any name it hasn't
seen before). No real serial hardware exists in this project's current
state to test end to end, so the 2 fixes below were verified with a
standalone harness instead (`/tmp/pcb_build/test_serial_logger.py` —
imports the real module, exercises the new methods directly with
synthetic inputs, no serial port needed) — 6/6 checks passed.

1. **No protection against a captured sample with the wrong shape ever
   existed.** `parse_features()` silently skips (`except ValueError:
   pass`) any MFCC line from the ESP32 that doesn't parse as
   comma-separated floats — a plausible outcome of a dropped/corrupted
   line over a plain, checksum-free serial protocol at 115200 baud. The
   only check before accepting a sample was "is the list non-empty," not
   "does it have the right shape." A capture that lost 2 of its 31
   expected frame-rows in transit would be saved into the dataset as-is,
   silently — the resulting ragged/wrong-shaped sample would only
   surface much later as a confusing `reshape()` error in `retrain.py`
   or `train_on_esp32_features.py`, far from where the actual bad
   capture happened. Added `validate_capture_shape()`, checked against
   `EXPECTED_NUM_FRAMES=31`/`EXPECTED_NUM_MFCC=13` (matching both this
   file's own "ML Pipeline" section above and
   `firmware/echosafe_feature_collector`'s own `NUM_FRAMES`/`NUM_MFCC`
   `#define`s, confirmed by grepping the firmware, not assumed) — wired
   into both `collect_sample()` and `batch_collect()`, rejecting a
   malformed capture with a clear message instead of saving it.
2. **No protection against a typo'd or differently-capitalized label
   silently creating a "ghost" class either**, despite this file's own
   top-level "Important Constraints" already documenting "label names
   must stay consistent (snake_case) across all collection sessions" as
   a hard-learned lesson — same pattern as the missing `noise`-class
   guard found in `retrain.py`: the lesson lived only in docs, not in
   any of the tools. Typing `"Bells"` in a session that already has
   `"bells"` would previously just create a 6th class with 1 sample,
   with nothing surfacing the mistake until the model's behavior looked
   wrong. Added `check_label_typo()` — case/`_`/space-insensitive
   near-miss detection against the existing `label_map`, prompting for
   confirmation before actually creating what might be an accidental
   duplicate class — wired into both `get_label_from_user()` (single-
   capture mode) and `batch_collect()`'s label prompt.

**Cross-cutting risk this surfaced (fixed 2026-09-22).** Because
`get_label_id()` assigned IDs by order of first appearance, the
specific mapping `{horns:0, noise:1, bells:2, gunshots:3, sirens:4}`
this project relies on everywhere (firmware's `WAV_FILES[]`, both
training scripts' `CLASS_NAMES` export) was an accident of
collection-session history, not something enforced anywhere — correct
for the current `ml/echosafe_dataset.npz` only by observation. Nothing
stopped a future from-scratch collection session (a fresh `--output`
path, or a wiped dataset) from producing a differently-ordered map that
trains and exports "successfully" while silently mismatching the
firmware's fixed positional assumptions about which class ID is which
sound.

Confirmed first that this couldn't just be fixed downstream instead —
grepped `retrain.py`/`train_on_esp32_features.py` and found neither
script reconstructs a `label_map`; both just consume whatever's stored
in the `.npz`, so `serial_logger.py`'s `get_label_id()`/
`load_existing_dataset()` really is the only place ID assignment is
decided. Added a module-level `CANONICAL_LABEL_MAP` (the same 5-class
mapping above) and wired it in two places: `get_label_id()` now assigns
a known class name its fixed canonical ID immediately regardless of
what order labels get typed in a session (a genuinely new/6th class
still falls back to the next free ID above whatever's assigned), and
`load_existing_dataset()` validates every loaded class against the
canonical map, raising a loud `ValueError` naming the exact mismatch if
a loaded dataset's `label_map` ever disagrees — rather than silently
trusting it and letting the drift compound. A same-canonical-ID
collision (two different names both trying to claim the same slot) also
raises instead of silently overwriting.

Verified with a standalone harness (no serial hardware needed, same
pattern as the original `serial_logger.py` review) exercising: labels
typed out of canonical order still land on their fixed IDs; a novel 6th
class still gets the next free ID; a matching loaded dataset passes
validation; a deliberately desynced one raises with the mismatched
class named; and the collision guard fires. 7/7 checks passed. Also
loaded the real, tracked `ml/echosafe_dataset.npz` through the updated
code (read-only) and confirmed its `label_map` already matches
`CANONICAL_LABEL_MAP` exactly — this fix changes nothing about the
current dataset, only about what a future from-scratch session could
silently produce.

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
  `TCA_ADDR`). **RESET → `R2` (10kΩ) → `3V3_SYS`, fixed 2026-09-15.**
  Previously a direct tie; TI's own TCA9548A datasheet (SCPS207H,
  fetched and read this session, not recalled) pin description table
  says RESET should "Connect to VCC or V_DPUM through a pull-up
  resistor, if not used" — a direct tie works electrically but doesn't
  match the documented reference practice, and a resistor is what lets
  the line still be pulled low externally (a jumper, test point, or
  future hardware-reset addition) without any risk of contention against
  a hard tie. No specific resistance value is given in the datasheet for
  this pin (unlike SDA/SCL, which have rise-time/capacitance equations —
  RESET has no such bus-timing constraint, it's just a static DC level),
  so 10kΩ was used, matching the same value and the same pattern already
  used for `R1` (the ESP32 EN/CHIP_PU pull-up). Added as `R2` in
  `EchoSafe_RevA.kicad_sch` using the identical placement pattern as R1
  (global-label pins, no drawn wires — consistent with the rest of this
  sheet). Verified via `kicad-cli sch erc`: 211 violations before → 210
  after, same categories throughout (off-grid warnings and pin-to-pin
  type-mismatch warnings on shared rails, both already expected/
  documented above) — no new unconnected pins or dangling labels
  introduced. **PCB re-synced to match (2026-09-15):** R2 (cloned from
  R1's real footprint) placed near U5, the old direct RESET-to-3V3_SYS
  spur removed, and RESET/3V3_SYS rerouted through it on a new
  `TCA_RESET` net. Verified via `kicad-cli pcb drc`: 0 violations / 0
  unconnected pads.
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

### Central Pod Resize (2026-09-15)

**The central pod's board outline was never sized to the design** — the
DRC-driven routing process above (both the hand-written attempt and the
FreeRouting pass) had no board-size constraint, so components and traces
were free to spread across whatever area was convenient for clearance.
The result was technically DRC-clean but **260mm × 175mm**, with real
component/trace extent of 150mm × 171mm — tablet-sized, nowhere near a
"compact wearable pod." This was caught during a pre-fab-order audit,
not during layout itself.

**Fixed: re-placed and re-routed to 65mm × 90mm** (~7.8× smaller by
area — 5,872mm² vs. 45,500mm²), confirmed with the user as "no hard
enclosure constraint, minimize it."

Approach:
1. Computed a floorplan from real component sizes (pad-level extents,
   not bounding boxes inflated by reference-designator silkscreen text
   — several components' `GetBoundingBox()` looked 2-4x their real
   footprint size because of where reference text happened to be
   placed; had to explicitly check pad-only extents to get an accurate
   picture). The ESP32-S3-WROOM-1 module (23×27mm including silkscreen)
   is the single largest constraint.
2. Grouped components logically: front harness connectors (J2/J3) on
   the top edge, rear (J4/J5) on the bottom edge, the TP4056/battery/
   regulator power cluster (J1/BT1/LD1) on one side edge for
   charging-port access (per user request — J1 is a separate physical
   module connected by wires, not soldered flat to the main board, so
   its pads didn't need to sit anywhere specific for mechanical
   reasons, only for cable-management convenience), ESP32 centered,
   haptic-driver cluster (TCA9548A + 4× DRV2605) directly below it,
   audio (MAX98357A) below that.
3. Verified placement had zero **pad-level** overlaps before routing —
   checked this separately from bounding-box overlaps for the same
   silkscreen-inflation reason as above, since a placement pass that
   only avoided bounding-box overlaps would have been far more spread
   out than necessary.
4. Still hit real courtyard overlaps (5 of them, IC-decoupling caps
   sandwiched too close to BT1/LD1's connector bodies) once
   `kicad-cli pcb drc` was run — courtyards extend further than pads,
   and in one direction were asymmetric relative to the footprint's own
   placement anchor (not centered), which wasn't obvious from pad
   position alone. Fixed by widening the board slightly (60mm → 65mm)
   and shifting the power cluster over to open a real gap.
5. Re-ran the same FreeRouting pipeline as the original layout (DSN
   export → `freerouting.jar` → SES import) on the freshly-placed,
   tightly-packed board. **This time it completed in one pass: 0
   unrouted, 0 violations, FreeRouting's own score 999.99/1000** — no
   manual gap-fixing needed at all, unlike the original (much larger,
   more sprawling) layout attempt. Confirmed independently via
   `kicad-cli pcb drc`: 0 violations, 0 unconnected pads.

**Side benefit:** the tighter layout's traces came out at 0.15-0.2mm
width throughout (FreeRouting's own choice, not manually forced) —
comfortably within standard fab capability, and notably avoids the
0.1mm minimum-width trace the original (larger) layout needed for one
QFN escape, which was at the edge of what typical prototype fab
services guarantee.

**Not yet done, follow-up items:** the visual/silkscreen review below is
now done. The acoustic port holes for the 4 mic modules are now done —
see "Footprints" section above for the hole itself plus a significant
mic-footprint pad-position correction that surfaced while adding it.

### Visual/Silkscreen Review (2026-09-16)

Nothing beyond default-severity DRC had ever been visually inspected on
any of the 5 boards. Two tools made this a real review instead of eyeballing
screenshots:

1. **`kicad-cli pcb drc --severity-all`** (not the plain `kicad-cli pcb
   drc` used everywhere earlier in this project) — KiCad has dedicated
   silkscreen DRC rules (`silk_over_copper` = silkscreen clipped by a
   solder mask opening, `silk_overlap` = two silkscreen items
   overlapping, `text_height` = text smaller than the board's minimum)
   that are all **warning-severity and excluded by default**. Every
   earlier "0 DRC violations" claim in this file was true but incomplete
   — it never actually checked silkscreen. Re-running with
   `--severity-all` immediately surfaced real, specific violations with
   exact coordinates, on every board.
2. **`kicad-cli pcb export svg`** (with `--exclude-drawing-sheet` and
   `-l F.SilkS,Edge.Cuts` or `F.Cu,F.SilkS,Edge.Cuts`) rendered to PNG via
   `rsvg-convert` (installed this session — `brew install librsvg`, no
   SVG-to-raster tool existed on this machine before). Doing this
   distinguished **real printed silkscreen** (F.SilkS — reference
   designators, a handful of footprint outlines) from **assembly
   documentation that never gets printed on the physical board**
   (F.Fab — value text, pin-1 triangles, courtyard boxes). An earlier,
   sloppier render that mixed F.Fab and F.SilkS together made the board
   look far more cluttered than it actually is, and had to be redone.

**Findings and fixes, all verified by re-running `kicad-cli pcb drc
--severity-all` after each fix (all 5 boards now show 0 violations at
full severity, not just the default error-level set):**

- **Every placed footprint on all 5 boards had lost its library prefix**
  (`lib_footprint_issues`, ~30 instances on the central pod alone) — each
  `(footprint "...")` declaration in every `.kicad_pcb` had a bare name
  with no `Library:` prefix (e.g. `(footprint "ICS-43434_LGA6"` instead
  of `(footprint "ICS-43434:ICS-43434_LGA6"`), even though the correct
  `fp-lib-table` registrations existed. This is the same class of gap
  documented earlier under "Footprints" (fp-lib-table never existed,
  fixed 2026-09-12) — evidently every subsequent script that reloaded and
  re-saved a footprint via pcbnew's Python API (the central pod resize,
  the mic pad-position fix, the TP4056 footprint swap) dropped the
  library nickname again on save, since `FootprintLoad()`/re-adding a
  footprint to a board doesn't automatically preserve it — `SetFPID()`
  has to be called explicitly with the full `Library:Name` pair. Fixed
  with a script (`/tmp/pcb_build/fix_fpid_prefix.py`) that maps every
  bare footprint name on every board to its correct library nickname
  (cross-checked against each project's `fp-lib-table` and, for standard
  parts, KiCad's own global `fp-lib-table`) and calls `SetFPID()`
  explicitly. **Practical takeaway for any future script that moves a
  footprint onto/off of a board via pcbnew's Python API: always call
  `SetFPID(pcbnew.LIB_ID(lib, name))` explicitly after `FootprintLoad()`
  or after re-adding a footprint — never assume it survives a
  `board.Add()`/`board.Save()` round-trip.**
- **The custom `TP4056_Module` footprint (J1 on the central pod) had two
  real authoring bugs**, found via `silk_over_copper`/`silk_overlap` and
  confirmed visually: (1) all 4 through-hole pads listed `F.SilkS` in
  their own `layers` set, which made each pad's copper literally *be*
  silkscreen too — an automatic, guaranteed self-violation on every pad,
  not a spacing issue. Standard KiCad THT pads should only list
  `*.Cu`/`*.Mask`. (2) The four pin-function labels (`B+`/`B-`/`OUT+`/
  `OUT-`) were center-justified at x=2.2mm against a silkscreen outline
  rectangle ending at x=1.5mm, so — because KiCad's default `fp_text`
  justification is centered, not left — each label's rendered span
  straddled the rectangle edge and, for the top/bottom pins, the pad's
  own solder mask opening. Fixed in the library file
  (`hardware/EchoSafe_RevA/lib/symbols/TP4056_Module.pretty/TP4056_Module.kicad_mod`):
  dropped `F.SilkS` from all 4 pads, moved the reference designator from
  beside the pins to above the part body (`(at 0 -3 0)`), and moved the
  4 pin labels to `x=1.9` with explicit `(justify left)` so they start
  just past the outline instead of straddling it. The placed J1 instance
  on the central pod was re-synced from the fixed library footprint
  (reloaded via `PCB_IO_KICAD_SEXPR.FootprintLoad()`, position/rotation/
  net-per-pad copied across, old instance removed) rather than hand-
  patched — same pattern used for every other footprint fix this
  session.
- **H1 (the acoustic port hole)'s reference designator text sat directly
  on top of MIC1's own pads** on all 4 mic modules — visually confirmed
  (not just DRC) via the puresilk render. The `Acoustic_Port_0.5mm`
  library footprint placed its "H1" reference at local `(2, 0)`, 2mm off
  the hole's center — close enough to overlap the mic's WS/LR pads and
  GND ring at this part's chip-scale pitch. Moved to `(0, 1.6, 0)`,
  clear of the ring (0.55mm outer radius) and every signal pad. Re-synced
  onto all 4 module boards the same way as the TP4056 fix above.
- **M1 (the motor)'s reference designator text was 0.7897mm tall against
  a 0.8mm board minimum** (`text_height`) — a leftover odd value
  (0.789716535433mm, clearly an inch-to-mm conversion artifact) from
  whatever originally produced `XDCR_C0720B001F.kicad_mod`. Bumped to an
  even 1.0mm in the library file, matching the size used elsewhere on
  these boards. Re-synced onto all 4 module boards.
- **Two silkscreen items on the central pod overlapped a neighboring
  component's silkscreen purely from placement density**, not a
  footprint-authoring bug: U2 (MAX98357A, a QFN16)'s reference sat
  exactly on the part's own center-pad — the QFN's exposed thermal pad —
  because the imported footprint places the reference at the part
  origin; moved it to `(30, 70.6)`, just below the package, in open board
  area. C8's reference (a 0603 decoupling cap only ~2mm from its neighbor
  C4) landed on top of C4's silkscreen outline; moved to the opposite
  side of C8 from C4, into open board area. Both are simple text
  repositions — no copper, footprint, or net impact.
- **U1 (ESP32-S3-WROOM-1) and U2 (MAX98357A) each had a literal duplicate
  `*` text object at the exact same F.Silkscreen coordinate** (visible in
  a raw pcbnew dump of each footprint's graphical items, two coincident
  objects with identical text/position) — an authoring artifact of the
  third-party UltraLibrarian-exported footprint files
  (`ESP32-S3-WROOM-1_EXP.kicad_mod`, `21-0136I_T1633-4_MXM.kicad_mod`),
  not something introduced this session. Harmless visually (perfectly
  coincident, so nothing was actually rendered twice) but flagged by
  `silk_overlap` as a literal self-overlap. Removed the duplicate copy on
  each, keeping one.
**Both items originally left as "flagged, not fixed" above were revisited
and addressed (2026-09-16 follow-up):**

- **Pin-1 markers for J1/LS1.** Re-examined the stock footprints first,
  since the original framing ("nothing marks pin 1 on the physically-
  printed board") turned out to be an overstatement: `Connector_JST` and
  `Connector_Molex`'s F.SilkS outlines already have a small asymmetric
  notch cut into the corner nearest pin 1 (visible in the raw
  `.kicad_mod` — the left/pin-1 side of the outline has an extra segment
  the right side doesn't). It's real, but subtle enough at this scale
  (a 0.12mm-wide notch) that it's easy to miss, especially compared to a
  standard bold pin-1 dot/triangle convention. Rather than hand-edit
  KiCad's shared global library files (which would affect every other
  project on this machine and get silently reverted on a KiCad update),
  created a small **project-local override library**,
  `hardware/EchoSafe_RevA/lib/symbols/Connectors_PinMarked.pretty/`,
  holding copies of the 3 affected footprints
  (`JST_SH_SM09B-SRSS-TB_1x09-1MP_P1.00mm_Horizontal`,
  `JST_SH_SM07B-SRSS-TB_1x07-1MP_P1.00mm_Horizontal`,
  `Molex_PicoBlade_53261-0271_1x02-1MP_P1.25mm_Horizontal`) each with one
  added bold filled dot (`fp_circle`, 0.4mm diameter) on F.SilkS next to
  pin 1, placed with margin from both the pad's own solder mask opening
  and the outline/courtyard edges (verified via `kicad-cli pcb drc
  --severity-all` after each addition — still 0 violations). Registered
  in a new `fp-lib-table` entry (`Connectors_PinMarked`) in the central
  pod and all 4 module projects, and resynced J2/J3/J4/J5 on the central
  pod plus J1 (all 4 modules) and LS1 (front modules only) onto the new
  footprints — same reload-and-reposition pattern used for every other
  footprint fix in this review, nets/position/rotation preserved.
- **U1's hidden reference.** Investigated whether it could actually be
  moved outside the ESP32-S3-WROOM-1 module's own footprint outline
  (18.45–41.55mm × 13.92–40.58mm) into genuinely open board area, by
  checking every neighboring footprint's bounding box and every routed
  trace's exact coordinates via pcbnew's Python API rather than
  eyeballing a render. **Conclusion: no such space exists on this board.**
  Every direction outside U1's outline is occupied within 1-7mm by
  another component's silkscreen, body, or dense trace routing — this
  board was deliberately packed to a minimum practical 65×90mm (see
  "Central Pod Resize" above), and U1 (the single largest component) is
  hemmed in on every side by design. Forcing the reference text outside
  the outline would mean either overlapping a neighboring footprint's
  silkscreen (trading one `silk_overlap` for another) or a real re-layout,
  neither of which belongs in a silkscreen-only pass. What *was*
  achievable and genuinely worth doing: the reference was sitting right
  where a diagonal trace crosses the footprint interior, at the edge of a
  small secondary pad cluster — moved it to `(31, 17)`, a fully
  trace-free, pad-free 12mm×6mm-ish pocket in the same footprint's own
  interior (still inside the outline, so still hidden after the module is
  soldered on — same as before), which at least makes it unambiguous and
  fully legible on the bare board before assembly. The "hidden after
  assembly" tradeoff itself is unchanged and, per the original note,
  common practice for a component this large and unambiguous.

### Fab Outputs (2026-09-16)

Generated for all 5 boards via `kicad-cli` (not KiCad's GUI plot dialog —
scripted the same way as every other batch operation this project), into
each project's own `outputs/` folder (`hardware/<Project>/outputs/`,
matching the folder the central pod already had reserved and empty since
the original consolidation):

- **Gerbers** (`outputs/gerbers/*.gtl/.gbl/.gto/.gbo/.gts/.gbs/.gtp/.gbp/.gm1`):
  `kicad-cli pcb export gerbers`, standard 2-layer fab set — F.Cu, B.Cu,
  F.SilkS, B.SilkS, F.Mask, B.Mask, F.Paste, B.Paste, Edge.Cuts. Confirmed
  2-layer (not 4) by checking the `.kicad_pcb` layer table directly rather
  than assuming.
- **Drill files** (`outputs/gerbers/*.drl` + `*_map.pdf`):
  `kicad-cli pcb export drill`, Excellon format, PTH and NPTH generated as
  **separate files** (`--excellon-separate-th`) since the mic modules have
  a real NPTH hole (H1, the acoustic port) alongside ordinary plated
  through-holes — spot-checked `EchoSafe_FrontLeft-NPTH.drl` directly and
  confirmed it contains exactly one 0.5mm hole at H1's actual board
  position. A PDF drill map was generated alongside for a human-readable
  cross-check.
- **CPL / position files** (`outputs/<Project>-CPL.csv`):
  `kicad-cli pcb export pos`, CSV, mm, both sides (front-only in practice
  on every board here, but generated with `--side both` rather than
  assuming).
- **BOM** (`outputs/<Project>-BOM.csv`): `kicad-cli sch export bom` — from
  each project's `.kicad_sch`, not the `.kicad_pcb` (the BOM is a
  schematic-level concept; footprint info in it reflects the schematic
  symbol's `Footprint` field, not whatever's actually placed on the
  board — see the mismatch this caught, below). Grouped by Value+Footprint
  so identical parts (e.g. the central pod's 8× 1µF 0603 caps) collapse to
  one BOM line with a Qty column instead of 8 separate rows.
- **Zipped gerber+drill bundle** (`outputs/<Project>-gerbers.zip`): the
  full contents of `outputs/gerbers/` zipped together, ready to upload
  as-is to a fab (JLCPCB, OSH Park, etc. all accept one combined zip).

**Real bug this caught:** generating the central pod's BOM surfaced that
J2/J3/J4/J5's schematic `Footprint` field still pointed at the *stock*
`Connector_JST:JST_SH_SM0xB-SRSS-TB_...` footprint, not the
`Connectors_PinMarked:...` override created during the Visual/Silkscreen
Review above — because that review only edited footprint instances
already placed on each `.kicad_pcb`, never the originating schematic
symbols' `Footprint` property. This was more than cosmetic: KiCad's
"Update PCB from Schematic" action pushes the schematic's `Footprint`
field onto the board, so left alone, the *next* such sync (by a human, or
a future script) would have silently reverted every connector back to the
un-marked stock footprint, undoing that fix without any error or warning.
Same risk existed for LS1 (front modules) and J1 (all 4 modules). Fixed
by updating the `Footprint` property on the affected symbols in each
`.kicad_sch` to the `Connectors_PinMarked:` library, confirmed via
`kicad-cli sch erc` that violation counts on all 5 schematics still
exactly match the documented baseline (210/9/9/8/8 — see "Critical fix:
Y-axis coordinate bug" above), i.e. this was a metadata-only change with
no electrical effect, then re-generated all 5 BOMs and confirmed the
`Footprint` column now matches what's actually on each board. MIC1/M1
(`ICS-43434`/`C0720B001F` libraries) needed no equivalent fix — those
footprints were corrected in place in their existing library files during
earlier work, so their library *name* never changed, only their
geometry — and H1 (the acoustic port) has no schematic symbol at all
(it's a PCB-only mechanical feature, never modeled electrically), so
there's no `Footprint` field for it to go stale.

All 5 boards reconfirmed at **0 DRC violations at full severity**
(`kicad-cli pcb drc --severity-all`) after the fab-output generation and
the schematic Footprint-field fix.

**Uploaded to JLCPCB's instant-quote tool and checked (2026-09-17).** Ran
all 5 `outputs/<Project>-gerbers.zip` bundles through
`cart.jlcpcb.com/quote` via Claude in Chrome (the built-in browser pane
can't do file uploads — no `file_upload`-equivalent tool — so this
specifically needed the user's real Chrome). Their pipeline parsed every
board cleanly with no errors:

| Board | JLCPCB-detected size | Expected (from layout) |
|---|---|---|
| EchoSafe_RevA | 90×65mm, 2-layer | 65×90mm |
| EchoSafe_FrontLeft | 40×54mm, 2-layer | 54.1×40.1mm |
| EchoSafe_FrontRight | 40×54mm, 2-layer | 54.1×40.1mm |
| EchoSafe_RearLeft | 40×46mm, 2-layer | 46.1×40.1mm |
| EchoSafe_RearRight | 40×46mm, 2-layer | 46.1×40.1mm |

All 5 matched (JLCPCB rounds to whole mm). Each board's rendered
top/bottom copper preview was also visually checked against the expected
layout — reference designators, component footprints, and routing all
matched what's in the actual `.kicad_pcb` files (e.g. the central pod's
preview clearly shows J1's 4-pad TP4056 column, U1's ESP32 footprint, and
J2-J5 in their expected positions). Each board's default PCB
Specifications (thickness, copper weight, min trace/space) were accepted
without any manufacturability warning, and each produced a real
calculated price — the clearest signal their pipeline considers all 5
fab-ready at standard 2-layer spec.

**Deliberately not done: full DFM analysis / their dedicated Gerber
Viewer** — both sit behind a JLCPCB account login, and creating an
account or signing in on the user's behalf is out of bounds. What's
above is everything obtainable without an account; a deeper DFM pass
(if the user wants it) would need them to sign in themselves.

The TP4056/mic-footprint physical-verification caveats from
"Footprints" above still stand — the fab-readiness check above confirms
the *files* are well-formed and manufacturable, not that the footprints
match the real parts. See "Attempted physical verification" above.

### Consolidated Shopping List (2026-09-21)

Built `hardware/EchoSafe_RevA/outputs/echosafe_shopping_list.html`, an
interactive checklist (checkbox state saved per-browser via
`localStorage`, not server-side) covering everything needed to buy for
**one complete unit** (central pod + 4 earpiece modules). Consolidated
by directly reading all 5 boards' generated BOMs
(`hardware/*/outputs/*-BOM.csv`, from "Fab Outputs" above) and summing
quantities per part across boards, e.g. the "1µF 0603 cap" line is 8 on
the central pod (C5-C8 REG pins, C13/C14 mux/amp decoupling, C17 ESP32
bulk) + 1 per earpiece module (C9-C12, mic VDD decoupling) = 12 total,
not just whatever one board's BOM shows in isolation.

Grouped into: ICs/semiconductors, power (battery/TP4056/LD1117V33),
0603 passives, transducers (mics/motors/speakers), and board-mount
connectors (JST-SH receptacles, PicoBlade receptacle) — plus a
separately-flagged **"not in any board BOM" section**, which is the one
real gap this consolidation surfaced: `kicad-cli sch export bom` only
lists parts that solder onto a board, so the 4 point-to-point harness
cables connecting each earpiece module to the central pod (2×9-pin +
2×7-pin, 1.0mm-pitch JST-SH, star topology per "Still needed... item 7"
above) never appear in any of the 5 CSVs — they'd be silently missing
from a shopping list built by just concatenating the BOMs. Flagged
explicitly rather than silently omitted.

Two rows carry the same "footprint not yet physically verified" caveat
already documented above (TP4056 module, ICS-43434 mic) rather than
restating it as new information — sourcing links included where already
established in "Footprints" (Adafruit/DigiKey speaker, LCSC mic,
HiLetGo TP4056 Amazon listing) rather than re-researched.

This is a single-unit list — quantities would need multiplying for more
than one device, which isn't handled by the tool itself.

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

**Pad positions were actually wrong, found and fixed 2026-09-15.** The
"0.9mm pitch" description above doesn't match what the footprint file
actually contained — the 5 signal pads were placed in a simple 2×2-
corner-plus-one arrangement (offsets ±1.3/±1.2mm) that doesn't match the
real chip at all. Found while trying to add the acoustic port hole: the
hole's clearance to a nearby pad exposed that the pad layout's *shape*,
not just its exact spacing, was wrong. Re-derived the correct layout
directly from the datasheet's mechanical package drawing (DS-000069
Rev 1.2, Figure 15 — a precise, corner-referenced drawing, far more
reliable than Figure 3's simplified pin icon), measuring pad positions
via pixel-level connected-component analysis of a 400dpi render rather
than reading dimension lines by eye a second time. Cross-validated
several ways before trusting it: the reconstructed package width from
the pad spacing matched Figure 15's separately-dimensioned 2.65mm body
width exactly; the "PIN #1 REFERENCE CORNER" marker confirmed WS's
position; and an initial version (LR placed at the ring's own
centerline) was caught and corrected by a second pixel-measurement pass
after the first version's LR-to-ring spacing didn't hold up. Real
correct layout (local offsets from the GND ring's center, which stays
at the footprint's own origin): **WS** (0.9, -1.127), **LR** (0.9,
-0.305), **SCK** (-0.9, -0.305), **VDD** (-0.9, -1.127), **SD** (0,
-1.127); pad size 0.6×0.522mm (not 0.6×0.9 — 0.9 was actually the row
pitch, not a pad dimension).

Also found via the same datasheet page: the acoustic port's real sound
port diameter is 0.375mm (InvenSense's own mechanical drawing gives
this number directly), consistent with the datasheet text's "PCB hole
should exceed the sound port diameter, 0.5mm minimum recommended."

**GND ring rebuilt as a true annulus** (was a solid disc, per the
"deliberate simplification" noted above) so the acoustic hole has real
copper-free clearance, and **its outer diameter was shaved from the
datasheet's 1.625mm down to 1.1mm.** At the full 1.625mm OD the ring
genuinely overlaps the SCK/LR pads' nearest corners (confirmed by
direct geometry, held up after the position correction above) — this
wasn't a rounding artifact, it's a real chip-scale-pitch tightness the
datasheet's own land pattern doesn't leave much room for once a
board-level hole is added into the same footprint. 1.1mm was chosen as
the smallest reduction that clears the overlap with a small positive
margin, not shrunk further than necessary.

Even at the corrected geometry, the ring/SCK/LR spacing (~0.07mm) and
the ring/hole spacing are both tighter than this board's generic 0.2mm
netclass clearance and 0.25mm hole clearance rules assume — legitimate
chip-scale pitch, not a mistake (same situation as the SPK_OUTN QFN
escape on the central pod). KiCad's custom-rule condition syntax for
scoping an exception to just this footprint (`A.Reference`,
`Library_Id` matching, and a dedicated `PCB_GROUP` + `memberOf()` were
all tried against real DRC output) didn't reliably match pad-level DRC
items in this KiCad version, so each of the 4 module projects has a
board-wide `<project>.kicad_dru` rule instead (`Chip-scale mic
footprint spacing`, clearance/hole_clearance min 0.01mm/0mm) — reviewed
against the full violation list each time to confirm nothing else on
these small (4-5 component) boards relies on the relaxed threshold.

One more subtlety: the custom ring pad's "anchor" sub-shape (required
by KiCad's custom-pad model, normally centered on the pad) was flagging
a `solder_mask_bridge` violation against the acoustic hole even after
shrinking it to near-zero size, because this board's solder mask margin
is 0 (apertures exactly match copper) and an anchor sitting exactly
inside another net's hole reads as one mask aperture wholly enclosed in
another's, regardless of size. Fixed by moving the anchor 0.35mm off
the ring's true center (into the copper-free gap between the hole and
the ring's inner edge) while keeping the ring polygon itself exactly
where it was — the anchor doesn't need to touch the primitive shape to
count as the same electrical pad.

**Net result:** all 4 module boards re-routed from scratch after the
pad reposition (same FreeRouting pipeline as the central pod) and
re-verified via `kicad-cli pcb drc`: **0 violations, 0 unconnected
pads** on every board. This is a substantial re-derivation with real,
documented cross-checks, but — like the TP4056 footprint — it's still
not a physical measurement. Worth a direct comparison against a real
ICS-43434 part or a verified purchased footprint before fabricating the
mic modules specifically.

**Independently cross-validated against a community footprint (2026-09-17),
short of physical measurement but a real step closer.** Prompted by the
user asking whether a different mic part might come with an
already-verified footprint (avoiding the physical-measurement problem
entirely) — checked first whether the *same* ICS-43434 already used here
has one, rather than assuming a swap was needed. It does: LCSC stocks
this exact part (C5656610) with a community-verified EasyEDA
footprint (`MIC-SMD_6P-L3.5-W2.7-P0.90-BL`), openable and inspectable
without an account. Read every pad's exact coordinates directly from
EasyEDA's property panel (not estimated from a rendered image) and
compared them against this project's own derived values: after
accounting for the EasyEDA footprint being authored at a 90°-rotated
orientation (a coordinate-frame difference, not a design difference),
**every pad position matched exactly** — the 1.8mm column-to-column
pitch (WS/LR vs. SCK/VDD) and the 0.822mm row-to-row pitch (WS/VDD/SD
vs. LR/SCK) both reproduced to the same precision as the values already
in `ICS-43434_LGA6.kicad_mod`. Pad size matched to within rounding
(0.52mm vs. this project's 0.522mm). This is real, independent
corroboration of the datasheet-pixel-measurement approach from an
unrelated source built by someone else — it doesn't replace an actual
physical part in hand, but it substantially de-risks the pad-position
question specifically.

Two things this check did *not* resolve, so the physical-verification
recommendation above still stands: the community footprint bakes its own
acoustic hole directly into the mic footprint at **0.4mm diameter**,
smaller than the 0.5mm this project uses (chosen to match the
datasheet's own text, "0.5mm minimum recommended" — kept as the more
conservative, more clearly-sourced value, but worth knowing the two
footprints disagree here); and the GND ring's outer diameter (1.1mm in
this project, reduced from the datasheet's 1.625mm for clearance — see
above) wasn't re-confirmed against the community footprint's ring within
the time spent on this pass.

**Generated a fit-test template (2026-09-21), same pattern as the
TP4056 one below.** `hardware/EchoSafe_RevA/outputs/ICS43434_fit_test_template.pdf`
— built via the `pcbnew` Python API, same tool used for every other
footprint fix in this project. Places real instances of both
`ICS-43434_LGA6` (MIC1) and `Acoustic_Port_0.5mm` (H1) at the same
relative offset they actually sit at on a real board — confirmed by
reading `hardware/EchoSafe_FrontLeft/EchoSafe_FrontLeft.kicad_pcb`
directly rather than assuming: both footprints are placed at identical
coordinates (32, 8mm) with no rotation. Pin-name callouts (WS/LR/GND/
SCK/VDD/SD) are positioned from the loaded footprint's own real pad
data (`pad.GetPosition()`) and the pad-number→signal mapping read from
the schematic symbol (`Mics.kicad_sym`'s pin table) — not retyped from
this file's own numbers above, to avoid repeating the kind of
by-hand-transcription mistake already caught once for this exact
footprint (see "Pad positions were actually wrong" above).

Same 100mm-calibration-ruler pattern as the TP4056 template, but with
an explicit caveat the TP4056 one didn't need: at this part's scale
(~3.5×2.65mm), ordinary print/scan tolerance can exceed what a visual
alignment check can actually verify, so the instructions point at
calipers against the real part as the real check, print alignment as
only a coarse sanity check. Also notes that since the export is a
vector PDF, on-screen inspection can zoom in losslessly to any
magnification — no separate enlarged-but-not-to-scale copy is needed
the way it might be for a paper printout.

One tooling gap hit and worked around: this KiCad install's `pcbnew`
Python binding doesn't expose `PAGE_INFO` as a typed class
(`board.GetPageSettings()` returns an opaque `SwigPyObject` — calling
`.SetPaperId()` on it fails), so the A4 paper size was forced with a
targeted text-level regex fixup on the saved `.kicad_pcb` file instead
(replacing the `(paper "...")` line) — the same "hand-edit generated
file text when the Python API falls short" pattern already used
elsewhere in this project. Verified this doesn't just silently produce
a mis-scaled PDF: `pdfinfo` on the exported file confirms real A4 page
dimensions (841.9×595.3pt, landscape — same orientation the existing
TP4056 template already exports at, so this is consistent, not a new
inconsistency), and the physical layout was checked by actually
rendering the PDF to a raster image and reading it back, not just
trusting the generator script.

**Also checked, and deliberately not pursued: swapping to a different
mic part.** The user separately asked whether an alternative part might
be worth adopting if it came with a verified footprint. Given the above
— the *current* part already has one, function-compatible variants
(ICS-43432) exist in the same family if ever needed, and every design
decision downstream of the mic (firmware I2S timing, the SEL-pin L/R
channel-select wiring, the acoustic port placement) is already built and
verified around ICS-43434 specifically — there's no finding here that
argues for actually changing parts, only for trusting the existing
footprint somewhat more.

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
| J1 central pod (TP4056 module) | `TP4056_Module:TP4056_Module` (custom, see below) | 4 through-hole pads matching the module's real single-column layout, spacing estimated from research — **still verify against the actual purchased HiLetGo board once in hand** |
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

**TP4056 footprint replaced with a real custom part, not a generic header
(2026-09-15).** The original `PinHeader_1x04_P2.54mm_Vertical` placeholder
(checked 2026-09-13, single row, uniform 2.54mm pitch) turned out to be
the wrong *shape*, not just an unverified pitch number. Found the exact
purchased listing ([Amazon B07PKND8KG](https://www.amazon.com/HiLetgo-Lithium-Charging-Protection-Functions/dp/B07PKND8KG))
and read its product photos directly (zoomed via browser automation,
since the photos are only 336×313px) — confirmed the board's 4 pads
(`B+`, `B-`, `OUT+`, `OUT-`) sit in a single vertical column on one edge,
not a spread-out header. Corroborated by a second, independent source:
[ccadic/TP4056-18650](https://github.com/ccadic/TP4056-18650), a real
measured KiCad footprint for a board from the same reference-design
family (identical chip markings/silkscreen layout), giving actual pad
Y-positions 1.7 / 5.2 / 12.8 / 16.2mm — i.e. gaps of 3.5mm (Out+ to B+),
7.6mm (B+ to B-, where the IC/passives sit), 3.4mm (B- to Out-). A
third source (Addicore's TP4056/TC4056A datasheet PDF, same reference-
design family) confirms the same pin *order* top-to-bottom: OUT+, B+,
B-, OUT-.

Built a custom footprint (`TP4056_Module:TP4056_Module`, in
`hardware/EchoSafe_RevA/lib/symbols/TP4056_Module.pretty/`, authored via
`pcbnew` Python API same as the mic footprint) using this data: 4 round
through-hole pads (1.7mm pad / 1.0mm drill — a reasonable size estimate
for this class of part, not measured), positioned at the researched
relative spacing. Pad numbers assigned to match the schematic symbol's
pin numbers (1=BAT+, 2=BAT-, 3=OUT+, 4=OUT-), independent of the
physical top-to-bottom order, with the real signal names kept on the
silkscreen for a human reading the board. Registered in the central
pod's `fp-lib-table` and assigned to J1.

**This is still a best estimate, not a physical measurement** — board
outline dimensions vary slightly even across this same reference-design
family (26mm vs 28mm length seen across sources), so the pad spacing
could be off by a small amount from the specific unit that arrives.
Confirm against the physical board once available; the footprint is a
straightforward edit if it needs adjusting (same pattern as the mic
footprint fix from 2026-09-13).

**Attempted physical verification (2026-09-17): the user doesn't have the
module in hand yet, so no real measurement was possible this round.**
Also worth recording: a first attempt at getting dimensions came from
asking Gemini directly, which returned a **6-pad, two-row layout**
(adding `IN+`/`IN-`) that contradicted everything already established
here — the schematic only has 4 nets on J1, and the existing 4-pad
single-column layout came from 3 cited sources (the actual Amazon
listing's own photos, a matching reference-design KiCad footprint on
GitHub, and a datasheet PDF). The Gemini answer cited no source, was
internally inconsistent between its own two tables, and turned out (per
Gemini itself, asked directly) to be "generic parametric synthesis... not
grounding itself in the specific hardware part number" — i.e. a
hallucinated "typical breakout" answer, not data. **Not applied.** Real
lesson: a second AI's unsourced answer isn't a substitute for verification
against the actual part, and is worth explicitly distrusting when it
contradicts something already cross-checked against primary sources.

**Generated instead: a 1:1-scale printable fit-test template**
(`hardware/EchoSafe_RevA/outputs/TP4056_fit_test_template.pdf`), built by
placing a real instance of the `TP4056_Module` footprint (not a
redrawn/guessed copy) on a blank A4 sheet alongside a 100mm calibration
ruler and usage instructions, exported via `kicad-cli pcb export pdf`.
The calibration ruler exists because printers/PDF viewers commonly
rescale ("fit to page") silently — the instructions call out to measure
it with a real ruler before trusting any pad alignment. Once the module
arrives, hold the printout against it: if the 4 pad holes don't line up,
measure the real offset and the footprint file
(`hardware/EchoSafe_RevA/lib/symbols/TP4056_Module.pretty/TP4056_Module.kicad_mod`)
is a small, isolated edit to correct — same pattern as every other
footprint fix this project. One authoring gotcha hit while building this:
`PCB_TEXT` defaults to center-justified, not left — a long instructional
string anchored near the page's left margin will silently spill off the
edge of the page unless `SetHorizJustify(GR_TEXT_H_ALIGN_LEFT)` is called
explicitly.

**Evaluated switching the charge IC itself (2026-09-17) — not adopted,
open question for the user.** The user asked whether a different chip
might sidestep the whole physical-verification problem: an IC with a
manufacturer-standard package has a real datasheet mechanical drawing
(or even a distributor-verified EasyEDA/KiCad footprint), unlike a
breakout module from an unspecified clone manufacturer, where the *board
outline itself* varies batch to batch — a genuinely different kind of
uncertainty than what's been fought all session for the TP4056 module
and the mic. Checked each candidate from a user-supplied list (originally
from another AI, so treated with the same skepticism as the earlier
Gemini dimension table) rather than taking it at face value:

| Candidate | Verified real? | Fits this design? |
|---|---|---|
| **IP2312** | Yes — full datasheet fetched directly ([Injoinic, translated](https://make.net.za/wp-content/datasheets/INJOINIC%20IP2312%20Translated.pdf)), real ESOP8 package with complete mechanical dimensions, and LCSC (C605433) has it in stock with a verified EasyEDA footprint | Partially — see below |
| TP5100 | Real chip | No — 2S/series charger; this design is single-cell (BT1 is one 3.7V cell) |
| MCP73871 | Real chip | Wrong topology — power-path management is a different circuit class, not a drop-in |
| IP5306 | Real chip | Wrong topology — it's a power-bank chip (boosts battery voltage *up* to 5V USB output); this design needs the opposite, battery voltage regulated *down* to 3.3V |
| BQ25185 / BQ24075 | Real chips | Same power-path mismatch as MCP73871 |
| CN3791 | Real chip | Irrelevant — solar MPPT input; no solar panel in this design |

**Only IP2312 is a genuine candidate**, and it's a real one: single-cell
Li-ion buck charger, 3A max, 94% efficient (vs. TP4056's linear
dissipation), ESOP8 (a standard 1.27mm-pitch SOP-8-family package —
easier to hand-solder than the MAX98357A QFN16 already in this design),
and a verified, ready-to-use footprint already exists (no re-derivation
needed, unlike the TP4056 module or mic footprint work above). **Not
adopted, because it's a materially bigger change than a footprint
swap:**
- **No integrated battery protection.** The datasheet confirms IP2312 is
  charge-management only — no OVP/OCP/short-circuit protection for the
  cell. The HiLetGo TP4056 module currently spec'd bundles that
  protection on the same board; switching to IP2312 means adding a
  separate protection IC (e.g. DW01A+FS8205A) as new parts, new
  schematic, new footprint.
- **Requires a real application circuit, not a drop-in.** Per the
  datasheet's typical schematic: a 1µH power inductor (a sourced part
  with its own specs, not a passive to pick arbitrarily), 3 capacitors,
  2 resistors (RICHG for charge current, RVSET for charge voltage), and
  optionally an NTC network. This is a small charger-circuit design
  task, not a footprint edit.
- **Introduces a 750kHz switching regulator physically adjacent to the
  audio path.** This design's own power-architecture notes already flag
  switching noise (from the haptic motors) as a risk to watch for on the
  audio rail; adding a second switching source directly in the charge
  path is a new, real EMI/audio-noise variable that TP4056 (linear, no
  switching) doesn't have.
- **Harder to hand-assemble.** The whole point of every module/breakout
  choice so far in this design (TP4056 module, LD1117V33 breakout, JST
  connectors) has been hand-solderable simplicity for what reads as a
  hobbyist/student build. IP2312 itself is easy (SOP-8, 1.27mm pitch),
  but the surrounding inductor + charge-set resistor selection is a
  small design exercise, not a plug-in.

**Left as an open question rather than decided unilaterally** — this is
a real architecture change (new BOM line, new protection circuit, new
EMI consideration), not a footprint correction, and the original
TP4056 + LD1117V33 choice was already a deliberate, considered decision
earlier in this project. If IP2312's smaller size and higher efficiency
are worth the added protection-circuit and inductor-selection work,
that's a call only the user can make — nothing in `TP4056_Module.kicad_mod`
or `EchoSafe_RevA.kicad_sch` has been changed.

**PCB re-synced to match (2026-09-15):** J1's PCB footprint was swapped
and its 4 nets (`VBAT`, `GND`, `VSYS`, `GND`) rerouted to the new pad
positions — pad "2" (BAT-/GND) was kept at its exact old location since
it had the most complex existing local routing, and the other 3 pads'
simpler single-segment traces were redrawn around it. Verified via
`kicad-cli pcb drc`: 0 violations / 0 unconnected pads, same as every
other board in this project.

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
