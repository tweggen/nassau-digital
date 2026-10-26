# NassauDigital — Implementation Gates

Each gate is a **self-contained work package**, done only when every one of its
acceptance criteria (AC) is objectively demonstrated. Nearly all of them are
test assertions, so "done" is a command, not an opinion.

Read [`DESIGN.md`](DESIGN.md) first. It is the specification; this file is the
schedule. Where the two disagree, `DESIGN.md` wins and you fix this file. The
order started from the independent review's (REVIEW.md, "Suggested gate
order") and was rearranged for the author's decisions: asset links and both
tempo methods are in from the start, and multi-output waits for qbx's return
tracks.

There are three tracks:

* **C — the core**, in this repository. Headless, no qbx, no Qt.
* **Q — the native instrument in qbx.** Each Q gate is a PR to qbx, specified
  first in a qbx proposal (`plan/proposed/`), and held to qbx's own gates
  (`./ci/gates.sh`) besides the ACs here.
* **X — prerequisites in other repositories**, which a C or Q gate waits for.

---

## Rules that bind every gate

The house rules of the Nassau DSP cores (nassau-zermatt's R1–R11, as restated
in nassau-bloom `docs/GATES.md`) apply unchanged. In short:

* **R1 — No gate regresses an earlier one.** The exit command is the full
  suite: `./ci/gates.sh` here, and qbx's `./ci/gates.sh` for a Q gate.
* **R2 — The core stays framework-free**: standard headers only, no Qt, no
  IPlug2, no SDK; allocation at prepare time only.
* **R3 — No allocation, no locks, no I/O in `process()`.** A sample buffer is
  released on a worker, never on the audio thread (DESIGN §6).
* **R4 — Append-only parameters and program attributes**; unknown attributes
  survive a load and a save (DESIGN §12).
* **R5 — No transcendental per sample** on the audio path.
* **R6 — Acceptance is measured**: real audio through the public API, measured
  (RMS, pitch, period, spectrum), not private state.
* **R7 — No `-ffast-math`, no `-march=native`.**
* **R8 — Determinism.** No `rand()`, no `time()`, no thread timing in what a
  render hears. Reset, the same notes, the same bytes (DESIGN §6.1, §2.2).
* **R9 — The instrument never writes its own parameters.**
* **R10 — Finish a gate by updating this file**: mark it DONE with a one-line
  summary of what was measured (numbers, not adjectives). One commit per gate.
* **R11 — Report honestly.** If an AC cannot be met, say so with the measured
  number and stop. Do not relax an AC on your own.

---

## Track X — prerequisites

### X1 — Shared DSP: the filters and shaper leave nassau-analogue

The 24 dB ladder, the 12 dB SVF, the HP cascade and `shapeTriodeK`, with
`synth_dsp.h`, `nassau_real` and the 32-sample control block, move into a
shared library (DESIGN §11); nassau-analogue then consumes them from there.

* **X1.1** nassau-analogue's own suite passes unchanged, and its golden renders
  are byte-identical before and after the move.
* **X1.2** The shared library builds and tests with no Qt, no IPlug2 and no SDK.

Needed by C5.

### X2 — qbx: return tracks and sidechain inputs (decided: first)

A qbx proposal of its own, built before Q8 (DESIGN §3.1): a return track whose
content is a tap on `(track, slot, output)` (proposal 37 §5.4), and a plugin
input that reads another track as its sidechain. Its ACs are the proposal's;
the two this schedule depends on:

* **X2.1** A test plugin with a second output bus: a `.qxa` case routes bus 1 to
  a return track, and the return track's render equals the bus's signal
  (`cmp`).
* **X2.2** A compressor on track B keyed from a return track ducks B by the
  expected gain while the key sounds (measured RMS), and not without it.

---

## Track C — the core

### C0 — Scaffold

**DONE 2026-10-26.** `./ci/gates.sh` green on ubuntu, macOS and Windows CI
(run 40289766611: 8/8 ctest on each, jobs 8 s / 14 s / 29 s). The R2 header
check (`cmake/CheckCoreHeaders.cmake`) runs as a build step and as ctest
cases on five violation fixtures; watched failing: `#include <QString>` in
`Source/Core/nd_version.cpp` stopped the build.

CMake library and test harness on the house pattern, `ci/` with the four verbs
(`build`, `clean`, `install`, `uninstall`; install is a no-op for a library)
and `ci/gates.sh`.

* **C0.1** `./ci/gates.sh` builds and runs an empty suite on Windows, macOS and
  Linux CI.
* **C0.2** A header check fails the build if anything under `Source/Core/`
  includes a header outside R2's list. Watched failing once.

### C1 — Program model and XML

DESIGN §2, §2.1, §12.

**DONE 2026-10-26.** `Tests/program_tests` 66/66. C1.1: the fixture
(`Tests/fixtures/program_full.xml`) sets all 40 zone, 14 layer and 5 program
attributes off their defaults (checked by the test, plus every enum value but
the outputs), and parse → write → parse is equal and write is byte-stable.
C1.2: unknown attributes and elements survive at program, zone and layer
level, also through an edit. C1.3: after deleting the highest-id zone the new
id equals the old `nextId` (5; 13 through the fixture and a save). C1.4:
tune +2 on ALL gives −95/0/95.5 → −93/2/96 (clamped); relative edits on 6
enum/bool parameters refused, all-or-nothing. C1.5: 7/7 kinds of edit restored
exactly from what they returned. Each AC watched failing under a sabotage.

* **C1.1** Parse → write → parse yields the identical structure, for a
  fixture covering every element and attribute.
* **C1.2** Unknown attributes and elements survive the round trip.
* **C1.3** Ids are never reused: delete the zone with the highest id, add one,
  and the new id equals the old `nextId`.
* **C1.4** ALL scope: an absolute edit reaches every zone; a relative numeric
  edit moves each zone from its own value and clamps per zone; a relative edit
  on an enum is refused.
* **C1.5** Every edit returns the previous absolute values it replaced, and
  applying them restores the program exactly (structural equality).

### C2 — One voice

DESIGN §2, §3 (without drive and filter), §5 (`oneshot`, `gated`, `loop`).

**DONE 2026-10-26.** `Tests/voice_tests` 23/23, `Tests/interp_tests`. C2.1:
period 109.0909 vs 54.5456 samples, ratio 1.999997. C2.2: 1000.00002 Hz,
+0.00003 cent (44.1 kHz sample, 48 kHz render). C2.3: oneshot level after
note-off / before = 1.000; gated mid-release RMS ratio 0.5008, silent from
note-off + release; loop period 2000.000 samples for a 2000-frame loop
(1000.000 an octave up). C2.4: centre −3.0103 dB per side, hard left: right
exactly 0. C2.5: byte-identical after reset, across instances and across
block sizes 256/37. R3: 0 allocations in 288 000 frames with stealing. Each
AC watched failing under a sabotage.

**Interpolator (measured choice): windowed sinc**, 16 taps, Kaiser β 7,
cutoff 0.45·fs, 512 phases. Error of a 44.1 kHz sine played at 48 kHz (SNR,
fit over residual): Hermite 89.5 / 46.5 / 26.4 / 13.8 / 7.6 dB at 1 / 5 / 10
/ 15 / 18 kHz, sinc 79.0 / 86.0 / 80.7 / 83.5 / 78.2 dB. Cost, ns per output
frame, one layer: Hermite 15.0–17.4, sinc 29.2–35.9. The sinc costs about
twice as much and is 54 dB cleaner at 10 kHz.

**C2.6, the CPU budget per voice** (DESIGN §13), Release, Intel Xeon E5-1650
v3 at 3.5 GHz: **one layer ≤ 40 ns per output frame** (measured 29.2 mono at
the root key, 35.6 stereo pitched, so 0.17 % of a core at 48 kHz), **four
layers ≤ 140 ns** (measured 129.4). Drive and filter (C5) come on top and get
their own share there.

* **C2.1** A sine sample at root key 60, played on 72, measures 2.000 ± 0.001 ×
  the frequency on 60 (autocorrelation).
* **C2.2** A 44.1 kHz sample in a 48 kHz render plays at its recorded pitch
  ± 0.1 cent: the rate ratio is folded into the pitch ratio, one interpolator
  pass.
* **C2.3** `oneshot` ignores note-off (the sample plays to its end); `gated`
  releases (RMS falls by the envelope's release); `loop` sustains with a
  measured period equal to the loop length ± 1 sample.
* **C2.4** A mono layer panned centre is −3.0 ± 0.1 dB per side; hard left
  gives silence on the right below −120 dB.
* **C2.5** Reset, the same notes, the same bytes, two runs (`cmp`).
* **C2.6** Per-voice cost at the chosen interpolator is measured and written
  here: the CPU budget per voice (DESIGN §13) is set at this gate.

### C3 — Layers, velocity, round-robin, voices

DESIGN §2, §2.2, §4.5.

**DONE 2026-10-26.** `Tests/layers_tests` 20/20. C3.1: summed RMS across a
10-step equal-power fade within −0.002 … 0.000 dB (velocities 50–80); velFade
0 switches between 64 and 65 (other layer below −100 dB). C3.2: a gap velocity
renders exact zeros. C3.3: 64 hits at three spacings play the 3 layers
20/23/21, 21/18/25 and 21/21/22 times, never the same layer twice in a row in
a page, byte-identical across runs. C3.4: page alone = page after its
predecessor (cmp), with the predecessor ending on the layer the page starts
with. C3.5: group 1 gone 240 samples (the 5 ms choke fade) after the choking
note, group 2's amplitude unchanged (0.353504 before and after). C3.6: the
(P+1)th note steals the oldest at P = 8, 16, 32, 64, 128, 256; 0 allocations
in process(). Each AC watched failing under a sabotage.

* **C3.1** Two layers overlapping by `velFade` crossfade with equal power: the
  summed RMS across the fade stays within ± 0.5 dB; `velFade = 0` switches.
* **C3.2** A velocity in the zone but in no layer's window is silent below
  −120 dB.
* **C3.3** Round-robin: the same note at the same position plays the same layer
  in two runs; over 64 hits on one key at distinct positions, each of 3 layers
  plays between 15 and 28 times; no two consecutive hits of a key in one page
  play the same layer.
* **C3.4** A page rendered alone equals the same page rendered after its
  predecessor (`cmp`), with round-robin in use.
* **C3.5** Choke: a note in group 1 silences sounding group-1 voices within the
  choke fade, and leaves group 2 untouched.
* **C3.6** Polyphony at each of 8…256: the 9th note on an 8-voice pool steals
  the oldest; no allocation after prepare (an allocation counter, as in
  nassau-analogue's AllocTests).

### C4 — Envelopes and modulation

DESIGN §4.1, §4.4.

**DONE 2026-10-26.** `Tests/mod_tests` 26/26. C4.1: amp envelope on DC
measures attack 50.000, hold 30.000, decay 100.000, release 200.000 ms (set
50/30/100/200; also 200.000 ms from mid-attack); filter envelope 40.000 /
120.000 / 150.000 ms (set 40/120/150), measured at the drive → filter slot
through the public modulation tap, since the filter is C5's. C4.2: LFO 2 Hz →
pitch measures 2.000013 Hz (±57.8 Hz around 1 kHz); mod wheel 0 / 64 / 127 →
depth 0.00 / 29.11 / 57.79 Hz; LFO → pan 2.000000 Hz; velocity 64/127 → level
×0.5039; velocity 1/64/127 → start frame 12600/6300/0 (later when softer);
velocity → cutoff 1011/2011/4000 Hz with +24 st (at the slot, until C5).
Voice cost unchanged against C2.6 (35.8 ns stereo one layer, 130.1 ns four
layers). Each AC watched failing under a sabotage.

* **C4.1** Amp ADSR + hold and the filter envelope each match their segment
  times ± 1 ms (measured on the envelope applied to DC).
* **C4.2** The LFO at 2 Hz → pitch measures 2.00 ± 0.01 Hz of modulation;
  velocity → level, cutoff and sample start each change the measured quantity
  in the documented direction; mod wheel scales LFO depth.

### C5 — Drive, filter, kit bus *(needs X1)*

DESIGN §3.

* **C5.1** Each drive curve's output RMS at moderate input stays within ± 1 dB
  of the input (output compensation).
* **C5.2** The ladder and SVF match nassau-analogue's goldens for the same
  input and settings (`cmp`), because they are the same code.
* **C5.3** Aliasing: a 10 kHz sine through full drive at the chosen
  oversampling factor has no component above 0.5·fs − 2 kHz louder than
  −80 dB relative to the fundamental.
* **C5.4** A zone with drive 0 at note-on runs without oversampling; automating
  its drive above 0 mid-note changes the sound but not the oversampling of
  that voice (the next note's voice is oversampled).
* **C5.5** Two overlapping hits through the kit-bus drive show intermodulation
  products absent when each hit is rendered alone and summed (measured
  spectrum difference).

### C6 — Outputs

DESIGN §3.1.

* **C6.1** A zone on aux 3 with aux 3 routed: its signal is on aux 3 and
  absent from main (below −120 dB).
* **C6.2** The same zone with aux 3 not routed plays through main, and the main
  render equals the render of the program with that zone set to main (`cmp`).
* **C6.3** Each output's own bus drive affects only that output.

### C7 — Tempo modes by resampling

DESIGN §5 (`tempo-loop`, `tempo-oneshot`, method `resample`).

* **C7.1** A 2-beat loop at 120 BPM played at 100 BPM measures a period of
  2·60/100 s ± 1 ms, and its pitch falls by 12·log2(120/100) semitones
  ± 1 cent.
* **C7.2** A tempo change mid-loop takes effect without a click: no sample step
  larger than the loop's own largest step.

### C8 — Libraries: base and overlay

DESIGN §8.

* **C8.1** Base v3 + an overlay that edits zone 17: what plays equals a
  program with the edit made directly (`cmp`).
* **C8.2** Base v4 drops zone 17: the edit is kept, reported as orphaned, and
  nothing else changes.
* **C8.3** Flatten yields a program that renders byte-identical to base +
  overlay and refers to no base.

---

## Track Q — the native instrument in qbx

Each Q gate is a qbx PR under a qbx proposal for the sampler, written before
Q0. Every Q gate also passes qbx's `./ci/gates.sh`, including
`check_layering.py`.

### Q0 — Engine module and seam *(needs C2)*

DESIGN §9.1, §9.2.

* **Q0.1** The core is a submodule; a new engine module `tw/sampler` builds it
  and registers the instrument (`tw.native.nassaudigital`) like the built-in
  303; `check_layering.py` is clean.
* **Q0.2** `twSamplerHost` is reached by capability: `setProgram`, `setSample`
  and `setRoutedOutputs` installed from a test, read lock-free in `process()`.
* **Q0.3** A `.qxa` case inserts the instrument with
  `insert-plugin format="tw" uid="tw.native.nassaudigital"`, plays a note at
  frame 1000, and the render is silent before it and carries the sample's RMS
  after, matching a golden (`cmp`).
* **Q0.4** The slot's saved state holds the program reference only: the
  project file grows by less than 1 KB when a 10 MB sample is added.

### Q1 — Model and verbs

DESIGN §9.2, §9.3.

* **Q1.1** `objects/sampler` wraps the core's program; `add-zone`, `set-zone`,
  `set-layer` and `select-zones` are actions addressed by `trackPath` + `slot`
  + zone id, each undone exactly by one undo (asserted on the program).
* **Q1.2** The program is saved in the project and reloads identically
  (structural equality); a program element written by a newer version keeps
  its unknown attributes.

### Q2 — Generations and determinism

DESIGN §6.1.

* **Q2.1** Installing a generation stales the track's pages: a render after
  swapping a layer's sample hears the new sample from the first frame.
* **Q2.2** Two renders of one session, with a swap between them, are each
  internally made of one generation (no page of the old sample after a page of
  the new).
* **Q2.3** An offline render started while a layer's render is outstanding
  waits at the run barrier (its log line is asserted), and its bytes equal a
  render started after the layer was ready (`cmp`).

### Q3 — Live asset links (decided: from the start)

DESIGN §6, §6.2.

* **Q3.1** A layer linked to an asset: an edit of that asset produces a new
  generation, and the next render hears the edit.
* **Q3.2** An action that does not touch the asset produces no re-render of it
  (render counter unchanged).
* **Q3.3** Linking an asset that contains the sampler's own track is refused.
* **Q3.4** Renaming the asset leaves the link intact, across a save and a
  reload.

### Q4 — Tempo modes by stretching

DESIGN §5 (method `stretch`).

* **Q4.1** A 2-beat loop at 120 BPM played at 100 BPM by stretch measures a
  period of 2·60/100 s ± 1 ms with its pitch unchanged ± 5 cents.
* **Q4.2** A tempo change produces a new generation, and so does completion of
  the source's onset analysis; renders after each are byte-identical across two
  runs.

### Q5 — Live play

DESIGN §9.4.

* **Q5.1** Headless: an armed track, an injected live NoteOn; the onset lies
  within the live lane's block-plus-lead bound, and the lane reports no
  ownership refusal.
* **Q5.2** Hand check by the author on the Windows 11 machine with the USB
  keyboard: the sampler's live onset, measured the same way as
  nassau-analogue's on the same device and buffer size, is within one device
  block of it. Recorded here with both numbers.

### Q6 — Recording into the instrument

DESIGN §7.

* **Q6.1** `record-start` → stop → `place-sampler-recording`: the next note
  plays the recording at its RMS ± 2 %; one undo removes it.
* **Q6.2** Where it lands: Pad view → the selected pad's key; Key view on an
  empty program → a zone over the whole keyboard; Key view on a program with
  zones → a one-key zone on the last key played. One case each.

### Q7 — Editor

DESIGN §7, §9.3. Qt, following Smaragd's look.

* **Q7.1** Key, Pad, Map and Zone views open at their fixed sizes; the view of
  an instance survives a save and reload; a new instance starts in the
  per-user default view.
* **Q7.2** Holding a key selects its zone, holding two selects both, through a
  tap of the device input (not the engine's event feed); asserted with
  injected MIDI.
* **Q7.3** The XML command pane applies a verb as one undoable action and
  completes verb and attribute names.
* **Q7.4** Hand check by the author: every label and value readable in every
  view; a velocity gap shows in the Map view.

### Q8 — Multiple outputs in qbx *(needs X2)*

DESIGN §3.1.

* **Q8.1** A kick zone on aux 1, aux 1 routed to a return track: the return
  track carries the kick, main does not (`cmp` against a golden of each).
* **Q8.2** A compressor on a bass track keyed from that return track ducks the
  bass while the kick sounds (measured RMS), the 909-into-a-ducked-bass case.
* **Q8.3** Unrouting aux 1 sends the kick back to main, and the change is a
  state change: the next render hears it from the first frame.

---

## Later

* **P — the plugin player** (DESIGN §10): its own gates once Q8 is done,
  starting from export (a program file plus `audio/`, every asset link baked)
  and a VST3/CLAP build on nassau-plugin-sdk that plays it.
* **Resonant band-pass and high-pass filters** (DESIGN §3): a C gate of their
  own after C5.
* **A strict round-robin cycle**, only if position-based round-robin does not
  satisfy in use (DESIGN §2.2): needs qbx to hand the instrument the count of
  earlier notes.
