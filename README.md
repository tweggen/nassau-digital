# NassauDigital

The go-to sampler of the Nassau plugin family (VST3 + CLAP, plus AU on macOS),
built on the Nassau plugin stack: **IPlug2 + Steinberg VST3-SDK** (not JUCE),
through the shared [nassau-plugin-sdk](https://github.com/tweggen/nassau-plugin-sdk).

As nassau-analogue scratches the Prophet/Juno itch, NassauDigital scratches the
AKAI S3000 / E-mu ESI-32 one: play back one-shot samples and drum loops, layer a
few things, map a drum kit across the keyboard. Speak into the mic and play it
on the keyboard a moment later. It is deliberately **not** a can-do-everything
library engine.

This repository holds the **core**: the program model and the voice DSP, with
no Qt and no IPlug2. Its first home is **Smaragd** (qbx), as a native
instrument, where any asset you can edit can be a sample source, kept up to
date as you edit it. A plugin that plays exported programs in other DAWs
follows later on the same core.

**Status: design.** Nothing builds yet. The specification is being written in
[`docs/DESIGN.md`](docs/DESIGN.md); a gated schedule (`docs/GATES.md`) follows
once the design is agreed.
