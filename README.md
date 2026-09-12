# EchoSafe

Wearable, around-the-ear sound-alert device for people who are deaf or hard
of hearing. ESP32-S3, 4 directional MEMS mics, on-device ML sound
classification, 4 directional haptic motors.

See [`CLAUDE.md`](CLAUDE.md) for architecture, pin mapping, ML pipeline, and
current status. See [`CONSOLIDATION_NOTES.md`](CONSOLIDATION_NOTES.md) for
where this repo's contents came from.

```
firmware/   ESP32-S3 Arduino sketches (full system, single-mic reference, data collector)
ml/         Training pipeline + dataset
hardware/   5 KiCad projects: central pod + 4 earpiece modules (PCB layout not yet started)
docs/       Supplementary reference docs
```
