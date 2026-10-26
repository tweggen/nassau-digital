# NassauDigital

The go-to sampler of the Nassau plugin family (VST3 + CLAP, plus AU on macOS),
built on the Nassau plugin stack: **IPlug2 + Steinberg VST3-SDK** (not JUCE),
through the shared [nassau-plugin-sdk](https://github.com/tweggen/nassau-plugin-sdk).

As nassau-analogue scratches the Prophet/Juno itch, NassauDigital scratches the
AKAI S3000 / E-mu ESI-32 one: play back one-shot samples and drum loops, layer a
few things, map a drum kit across the keyboard. Speak into the mic and play it
on the keyboard a moment later. It is deliberately **not** a can-do-everything
library engine.

In Smaragd (qbx) any asset you can edit there can be a sample source, kept up
to date as you edit it. Everywhere else the same program plays from embedded
audio.

**Status: design.** Nothing builds yet. The specification is being written in
[`docs/DESIGN.md`](docs/DESIGN.md); a gated schedule (`docs/GATES.md`) follows
once the design is agreed.
