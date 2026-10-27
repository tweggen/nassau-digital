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
* **R2 — The core stays framework-free**: standard headers and the
  header-only `nassau-dsp` (found by its locator, never a submodule: DESIGN
  §9.2) only; no Qt, no IPlug2, no SDK; allocation at prepare time only.
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
shared library, `nassau-dsp` (DESIGN §11); nassau-analogue then consumes them
from there, through the locator and the single nassau-suite pin of DESIGN §9.2
(never a nested submodule).

**DONE 2026-10-27.** nassau-dsp#1 (merged `76e38fb`) and nassau-analogue#16
(merged `08196ac`), pinned once in nassau-suite (#33). X1.1: nassau-analogue
13/13 tests; G5 and G11 byte-identical to the renders from before the move
(sha256 b4e52dd86e01b7a1… / 22afe6268730df0e…), also with `NASSAU_DSP_FLOAT`
and `NASSAU_NO_SIMD` (whose float render differs from the double one, so the
define reaches the moved headers); rechecked at the merged pin from a clean
copy with no sibling checkout (the fetch route). X1.2: nassau-dsp's own CI
green on ubuntu, macOS and Windows with no Qt, IPlug2 or SDK; slopes ladder
−24.27, SVF −12.13, HP +11.95 / +23.90 dB/oct; both low-passes self-oscillate
at 100 %; byte-level golden of 264000 values; the R2 header check watched
failing on a planted `<vector>`; the golden watched failing on a changed
ladder constant and on a 1 % change inside `shapeTriodeK`.

* **X1.1** nassau-analogue's own suite passes unchanged, and its golden renders
  are byte-identical before and after the move.
* **X1.2** nassau-dsp builds and runs its own tests with no Qt, no IPlug2 and
  no SDK, and its headers include nothing outside R2's list.

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
**Re-measured on audio at C5 (2026-10-27)**, now that the filter exists: a
sine through lp24 at resonance 0, whose level gives the cutoff (four bilinear
one-poles), with the amp envelope divided out through a DC channel. Filter
envelope 40.514 / 120.776 / 150.061 ms (set 40/120/150), sustain 0.29998,
each segment starting 0.78 / 0.89 ms after its event (the cutoff is set per
32-frame block and ramped across the next). Velocity → cutoff on audio
1010.8 / 2010.4 / 3997.5 Hz (documented 1011.0 / 2010.9 / 4000), within
0.07 %. Watched failing: the stage handed the zone's static cutoff.

* **C4.1** Amp ADSR + hold and the filter envelope each match their segment
  times ± 1 ms (measured on the envelope applied to DC).
* **C4.2** The LFO at 2 Hz → pitch measures 2.00 ± 0.01 Hz of modulation;
  velocity → level, cutoff and sample start each change the measured quantity
  in the documented direction; mod wheel scales LFO depth.

### C5 — Drive, filter, kit bus *(needs X1)*

DESIGN §3.

**DONE 2026-10-27.** `Tests/fx_tests` 33/33, `Source/Core/nd_drive.h`,
`nd_oversampler.h`, `nd_voice_fx.h`, `nd_kit_bus.h`; nassau-dsp found by
`cmake/NassauDSP.cmake` (verbatim) at pin 76e38fb. C5.1: a −12 dBFS RMS
1 kHz sine, drive 0.25 / 0.5 / 0.75 / 1, every curve within ±0.003 dB of the
input (filter out of the way, hp12 at 20 Hz; with the default lp24 at 20 kHz
at drive 1: −0.06 / −0.06 / −0.08 / −0.54 dB for soft / tube / hard / fold).
C5.2: the voice's stage over nassau-dsp's golden battery (its input, its
cutoff sweep, resonance 0/50/100 %, in uneven chunks) is byte-identical to
`Tests/fixtures/filter_golden.bin` for all 8 filter cases (ladder ×3, SVF ×3,
HP 2 and 4 poles), 24000/24000 samples each, both channels; on audio each
type's slope two octaves out matches the bilinear cascade within 0.01 dB
(lp24 −49.94, lp12 −24.97, hp12 −24.63, hp24 −49.26 dB). C5.3, at the chosen
**4×** with antiderivative antialiasing (soft, hard, fold; tube has none
without a logarithm) and +20 dB input gain at drive 1: loudest line above
fs/2 − 2 kHz at 44.1 kHz soft −145.1, tube −84.1, hard −114.4, fold −85.1 dB;
at 48 kHz none exists (a 10 kHz tone's lines lie on a 2 kHz grid, the only
one above 22 kHz is 24 kHz, which a symmetric curve cannot make: at 48 kHz the
AC passes even without oversampling, so 44.1 kHz is the case that measures).
2× fails at 44.1 kHz (−48.9 dB fold, antiderivative on); +24 dB fails at 4×
(tube −77.7, fold −79.4). C5.4: a drive-0 voice is 1× and stays 1× when drive
is automated to 1 (its 3rd harmonic's alias at 14.1 kHz rises from −172 to
−24 dB), the next note is 4× (loudest alias −84.7 dB); the oversampler's
delay (39 frames at 2×, 47 at 4×) is compensated, a click peaks on the same
frame either way, a burst plays to its end. C5.5: two hits (1 kHz, 1.5 kHz)
through busDrive 0.5 / 1 show 500 Hz at −18.3 / −13.7 dB and 2 kHz at
−18.0 / −11.3 dB re the 1 kHz tone; each alone and summed −156.8 dB or below;
busDrive 0 is an exact bypass. The bus switches its oversampling over 256
frames (step 0.0071 against the tone's own 0.0061). R8: byte-identical after
reset and at blocks 256/37 with drive and bus drive automated mid-render.
R3: 0 allocations in 2 × 1500 blocks over every filter, both factors, drive
automation, the bus on and off, stealing. Each watched failing: no
compensation (+11.4 dB), the g ramp off by one sample (8 golden cases), the
antiderivative off (fold −61.0), factor 2 (−48.9), a plain decimator
(−72.2), oversampling re-decided on a drive change, drive not reaching a
sounding voice, no latency compensation (peak 39/47 frames late), no tail
(burst cut, −10 dB error), the bus not applied, a one-block bus switch (step
0.0138), the bus ramping across the host's block (block sizes differ), a
heap buffer in the stage (425154 allocations).

**Per-voice cost against C2.6 — the budget is missed** (Release, Xeon E5-1650
v3, load 3.6–4.2 on 12 cores from another job, best of 5, three runs):
one layer, drive 0, lp24 static **67–69 ns** (≤ 40), cutoff moving 82–84;
drive 0.5 at 2× 186–191, at 4× **319–326**, 4× with the cutoff moving
377–396; four layers, drive 0 **167–172 ns** (≤ 140), drive 0.5 at 2×
287–297, at 4× **419–429**. The sample reading C2.6 budgeted is unchanged
(the filterless C2 harness measured 36.4 / 131.5 ns before this gate): the
stereo lp24 ladder adds about 30 ns per frame at the base rate, and a 4×
voice spends about 97 ns in the resampler, 60 in the soft curve with its
antiderivative and 120 in the ladder at 4×. C2.6 left drive and filter "their
own share" here; that share is the author's to set.

**Filter `off`, 2026-10-27** (DESIGN §3, §4.2: the author's decision). `off`
is appended to the filter type (number 4, spelled `off`; lp24 / lp12 / hp12 /
hp24 keep 0–3) and is the default: a zone with no `filter` attribute reads as
`off`. The writer writes every attribute, defaults included (C1), so `off` is
written as `filter="off"`; programs saved before this always wrote their
filter and keep it. `off` is a true bypass: no filter code runs, and with no
drive and no oversampling the stage returns at once. Gated
(`Tests/fx_tests` 37/37, `Tests/program_tests` 76/76): a mono noise sample
on its root key, hard left, read by the Hermite (an identity at ratio 1),
renders **byte-identical to the sample** (cmp) with no filter attribute and
with `off` (max |render − sample| 0; lp24 at 20 kHz 1.32 on the same
full-band noise); an oversampled voice (drive 1 at note-on, driveMix 0) with
`off` is byte-identical to the oversampler's round trip down(up(x)). Watched
failing: the default back to lp24 (5 program checks and the default render),
`off` missing from the names (13 program checks), `off` run as a wide-open
lp24 at 20 kHz (the oversampled check; with the 1× shortcut removed too, all
three byte checks). The 1× shortcut alone is cost, not sound: removing it
changes no byte (the round trip through double is exact) and is not gated.
Every C5 number above was measured with lp24 at 20 kHz, then the default: the
C5 fixtures now state `filter="lp24"` and reproduce them unchanged. Tests
that never meant a filter now run without one, which brings back the C2–C4
numbers recorded before C5 (interpolator SNR, C3.5's 0.353504, velocity →
start 12600 / 6300 / 0) and moves C6's recorded aux 3 RMS 0.199654 to
0.199933 and its cross-output intermodulation −286.3 to −286.0 dB (C6's
main-only sha256 is of the old default). Cost, one layer drive 0 `off` 37.4
ns, four layers 136.8 (lp24 68.6 / 164.9 in the same run; C2's filterless
36.4 / 131.5; Release, load 14–19 on 12 cores, best of 3 runs).
**C5.3 with the filter off misses** at 44.1 kHz (the AC was defined and
passed with lp24 at 20 kHz in the path): soft −133.9, tube **−73.0**, hard
−103.6, fold **−76.0** dB, lines at 21.3 / 20.8 kHz that lp24 at 20 kHz had
attenuated (48 kHz: none). Shown by `fx_tests` as a note, not a check. Open
for the author.

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

**DONE 2026-10-27, except C6.3's per-output amount, which waits on the
author** (the program has one `busDrive` and no attribute for an aux output's
own; see the C6 PR). `Tests/outputs_tests` 31/31; API (appends):
`Instrument::process(…, outBuses, numBuses, n)` (qbx's `outBuses[bus][channel]`,
bus 0 main, bus k aux k), `setRoutedOutputs(mask)` / `routedOutputs()` (bit k =
aux k+1, as `twSamplerHost`; qbx's `setRoutedOutBuses` mask >> 1),
`kitBusOversampling(output)`, `kNumOutputs` = 9, `kRouteFadeFrames` = 256.
C6.1: a zone on aux 3, routed: aux 3 equals the zone's render on main (cmp,
RMS 0.199654), main's peak exactly 0, the other aux outputs silent; with a main
zone sounding too, main is exactly that zone alone (cmp) and the aux 3 tone on
main −299.5 dB. C6.2: aux 3 unrouted, main is cmp-equal to the zone set to
main in five ways (mask 0; every aux but 3; aux 3 masked but a null buffer;
numBuses 1; the two-pointer process()), with main's bus drive 0.5, output
level −3 dB, the zone oversampled and notes interleaved A B A B; the zone
moves main by up to 0.225. C6.3, at the one `busDrive` 0.5 every output takes:
main is exactly zone A driven alone and aux 3 exactly zone B driven alone
(cmp); aux 3 equals B driven on main (cmp); intermodulation across outputs
−280.9 / −286.3 dB against −18.0 dB on one shared bus; buses of unfed aux
outputs at rest; an aux output's own amount is not settable. Routing changes
on a sounding 220 Hz tone, on its crest: largest step 1.00× the tone's own on
main and aux 3, at bus drive 0 and 0.5 (a hard switch: 34.7×); main + aux 3 =
the signal within 7.5e-9; a change during a crossfade waits for its end; an
output nothing sounds on switches at once; `reset()` snaps. R8:
byte-identical across reset and blocks 256 / 37 / 1000, all nine outputs, seven
routing changes. R3: 0 allocations over 3000 blocks, 429 routing changes, five
buffer layouts. Main-only renders byte-identical to before C6 (sha256
735f6c05…, blocks 256 and 37). Per-voice cost unchanged against C5 (Release,
Xeon E5-1650 v3, load 16.8 → 9.5 from a qbx build, three runs, base / C6 in
the same session): one layer drive 0 67.1–68.1 / 66.8–67.6 ns, 4× 317.5–324.3
/ 319.4–321.3, four layers 167.7–171.3 / 167.9–168.0, four layers 4×
419.3–431.3 / 420.7–422.9. A bus at `busDrive` 0.5 costs 162.5 ns per frame;
all nine running 1462.6. Each watched failing: routing ignored (14 checks), no
fallback (10), the fallback after main's bus (12), one shared bus (9), aux
buses never run (3), a hard switch (4), a change retargeting a crossfade (2),
the crossfade restarted per call (7, R8 among them), a heap buffer in the
routing (3600 allocations), a silent output crossfading (1).

* **C6.1** A zone on aux 3 with aux 3 routed: its signal is on aux 3 and
  absent from main (below −120 dB).
* **C6.2** The same zone with aux 3 not routed plays through main, and the main
  render equals the render of the program with that zone set to main (`cmp`).
* **C6.3** Each output's own bus drive affects only that output.

### C7 — Tempo modes by resampling

DESIGN §5 (`tempo-loop`, `tempo-oneshot`, method `resample`).

**DONE 2026-10-26.** `Tests/tempo_tests` 22/22. C7.1: a 2-beat 1 s loop
(1 kHz carrier under a once-per-loop swell) at 100 BPM measures a period of
1200.000 ms and a pitch of −3.156413 st (error −0.00001 cent; 44.1 kHz source:
1200.000 ms, +0.000004 cent); at 120 BPM 1000.000 ms and 1000.000 Hz;
`tempo-oneshot` at 100 BPM sounds for 1200.000 ms through a note-off, at
−0.00005 cent; `stretch` and `beats="0"` keep the sample's own rate. C7.2: a
change mid-loop at frame 30208, largest step within 10 ms of it against the
loop's own largest at either tempo: 100→140 BPM 0.0113 vs 0.0540, 140→100
0.0301 vs 0.0540, 90→160 0.0127 vs 0.0614, the new pitch within 0.0000 cent.
Byte-identical across reset and block sizes 256/37 with tempo changes; 0
allocations in 248 000 frames with 100 tempo changes. Watched failing: the
ratio inverted (period 1150 ms at the lag search's edge, +631 cent), and a
read position rescaled at the change (steps 0.163 / 0.163 / 0.192).

* **C7.1** A 2-beat loop at 120 BPM played at 100 BPM measures a period of
  2·60/100 s ± 1 ms, and its pitch falls by 12·log2(120/100) semitones
  ± 1 cent.
* **C7.2** A tempo change mid-loop takes effect without a click: no sample step
  larger than the loop's own largest step.

### C8 — Libraries: base and overlay

DESIGN §8.

**DONE 2026-10-26, previous values added 2026-10-27.** `Tests/overlay_tests`
46/46, `Source/Core/nd_overlay.*` (the §12 overlay: one `set-zone` /
`set-layer` per target, every value beside its `.was`, applied in order, each
edit reported applied / drifted / orphaned / refused / unknown;
`recordZoneEdit` / `recordLayerEdit` / `undoRecordedEdit` make and take back
edits). C8.1: base v3 + an overlay editing zone 17 (attack, tune, level) and
layer 6 equals the direct edits (structural) and renders byte-identical (cmp);
the base alone differs (RMS 0.4322 vs 0.2153 from key 40's note-on); the base
is unchanged. C8.2: on v4 without zone 17, exactly edit #0 is reported
orphaned (id 17), the result equals v4 with only the layer-6 edit (structural
and cmp), the overlay writes the same bytes, and v3 gets the kept edit back; a
dropped layer orphans its `set-layer` alike. C8.3: the flattened program,
written and read back, has no overlay element, no `base`/`baseVersion` and no
library path, equals base + overlay and renders byte-identical (cmp).
Previous values: `tune +2` recorded on zone 17 is stored as `tune="0"
tune.was="-2"`; record → apply → undo gives the same overlay bytes, the same
program and a byte-identical render (cmp). Base v5 (zone 17 attack 0.05)
reports edit #0 drifted, attack was 0 (the default) now 0.05, and plays
v5 with attack 0.25 (structural and cmp); "0.000" against 0 is no drift, one
ulp is. ALL `tune +2` on zones at 1 / −3 / 0 becomes three verbs with `.was`
1 / −3 / 0 and new 3 / −1 / 2, equal to the ALL edit made directly (cmp); a
zone a later base adds keeps its value. A stored `zone="all"` and a stored
`+n` are refused. Watched failing: `set-zone` reaching every zone (8 checks),
an orphan stopping the overlay (3), flatten dropping the overlay (2), `.was`
ignored (2), a drifted edit held back (2), one shared `.was` for ALL (3), undo
not writing `.was` back (2), undo leaving the verbs (4).

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
  `set-layer` and `select-zones` are actions addressed by `trackPath` +
  `slotIndex` + zone id, each undone exactly by one undo (asserted on the program).
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
