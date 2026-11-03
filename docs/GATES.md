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

**X2.1 DONE 2026-10-27** (qbx proposal 55 M0–M2: qbx#310, #311, #315, QBX-248
and QBX-251). `return_track_split_cmp.qxa`: `tw.test.clap.split`'s bus 1
routed to a return track renders byte-identical to a plain track playing the
same clip (576044 B, on `test_stereo.wav`, whose channels differ). M3, the
engine half of sidechain inputs (qbx#318, QBX-252), is merged too.
**X2.2 DONE 2026-11-01** (qbx#332, QBX-262, proposal 55 M4).
`sidechain_duck_from_return.qxa`: B through `tw.test.clap.duck`, keyed from a
return track fed by A's split aux, measures 0.124997 / 0.062499 against dry
0.499987 / 0.24999 inside the key, 0.25000 × dry on both channels (band ±1 %),
and dry before and after it, with the sidechain cleared, and with the key clip
removed. Soloing B keeps the duck; `post` with the return's fader at −inf stops
it, `pre` does not; moving the key clip moves the duck. **X2 is complete.**

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

*The `<Pads>` table, 2026-11-03 (DESIGN §13, "Settled on 2026-11-03, pads";
qbx QBX-277):* the program grew a program-level table, one `<Pad n key>` row
per pad off its default key (36 + n), written after the zones and only when
it has a row. `Tests/pads_tests` 56/56: parse, write and round-trip (rows keep
file order, unknown attributes and children kept on the table and on a row,
no table written for a program without rows); refusals of n outside 0..95 (six banks, the author's answer of
2026-11-03) or key outside 0..127, a duplicate n, a row without n or key and a second table, each naming
it; a pad's key (row, else 36 + n, else none for n ≥ 92); `setPadKey` (only
the key changes, the default key removes the row, new rows in order of n,
refusals change nothing, the prior key undoes); `padsOnKey` and
`padSelectedByKey` (a shared key keeps the selected pad, else the
lowest-numbered); `zonesCoveringKey` (three pads on one tom zone; two
velocity zones on one key); the zone attribute `pad` read and written with
no meaning; the table kept through an overlay and a flatten. The C1.1
fixture carries a table (`program_tests` 119/119). Ten sabotages, each
biting (writer drops the table; duplicate n accepted; key range unchecked;
the reader and setPadKey taking n up to 127; default key keeps its row;
shared key always the lowest; padKey ignores the rows; equality ignores the
table; coverage by keyLo only). Widening padKey's own n bound to 127 bites
nothing and cannot: every default past n 91 is beyond key 127 anyway, and
no row can carry an n above 95.

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
their own share there. *(2026-11-01, the author: the budget bounds only the
default voice, filter `off` and undriven; filtered and driven voices are
measured and reported, not bounded. C5 has the numbers.)*

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

**Per-voice cost against C2.6 — the budget is missed** *(superseded
2026-11-01: the budget now bounds only the default voice, filter `off` and
undriven, which meets it; these lp24 and driven numbers are reported, not
bounded; see below)* (Release, Xeon E5-1650
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
attenuated (48 kHz: none). Shown by `fx_tests` as a note, not a check.
*(Settled 2026-11-01: C5.3 is defined with lp24 at 20 kHz in the path; with
the filter off the numbers are reported, not bounded; see below.)*

**The author's decisions of 2026-11-01, built** (DESIGN §3, §4.2, §4.3, §13).
`Tests/fx_tests` 47/47, `Tests/program_tests` 103/103; every other suite
prints the same measurements as main (timings aside).
*tube is asymmetric*: a biased triode, (1+b)²(T(x+b) − T(b)) with
T(u) = u/(1+|u|) and b = 1 (one division a branch, no transcendental;
ceilings +2 / −6), replacing the symmetric x/(1+|x|); still without an
antiderivative (it has a logarithm). C5.1's tone (1 kHz, −12 dBFS RMS,
filter off) at drive 0.25 / 0.5 / 1: 2nd harmonic −15.8 / −10.5 / −13.8 dB,
3rd −31.6 / −21.5 / −17.1 dB re the fundamental (soft at drive 1: 2nd
−218.9). (At b = 0.5 the 3rd led at drive 1; at 0 dBFS input the 3rd leads,
as an overdriven triode does.) *DC blocker*: a one-pole high-pass at 10 Hz on
the drive's wet signal (its pole set at note-on for the stage's rate; the dry
share is not filtered), run whenever the drive runs, so an undriven voice
never runs it. The curve makes DC at −16.1 / −11.5 / −8.6 dB re the input;
half a second in, the output's DC is −197.2 / −176.9 dB re the fundamental
and, at drive 1, exactly 0 over the window. The blocker alone on a settled sine:
−0.96 / −0.97 / −0.97 dB at 20 Hz and −0.26 dB at 40 Hz at 1× / 2× / 4× of
48 kHz. *Oversampling is a zone attribute*, `oversample="2"` or `"4"`
(an appended enum, default `"4"`, last in the zone table), latched at
note-on; an undriven voice is not oversampled whatever it says. The C1
writer writes it, defaults included; anything but 2 or 4 is refused; a
relative edit is refused (a choice, not a number); an overlay records it as
`oversample="2" oversample.was="4"`, applies it with nothing drifted and
undoes it. Two driven zones at 2× and 4× in one program run at 2× and 4×
side by side (the tap, per voice), an undriven one at 1×; 16 driven renders
(every curve, lp24 and off, drive 0.3 and 1): the default byte-identical to
`"4"` in all 16, `"2"` different in all 16; with the attribute alone (before
the tube and DC blocker changes) the same 16 renders hashed identically to
the build before it (9983f1a), so 4× is today's sound. The instrument's
`setOversampling()` / `oversampling()` are gone; `oversamplingLatency()` is
the kit bus's delay (4×, 47 frames, as built). At 2×, C5.3's tone at 44.1 kHz
(lp24 at 20 kHz): soft −68.4, tube −52.5, hard −82.5, fold −48.9 dB, the
user's choice. *Drive, cutoff and resonance follow a sounding voice*,
glided over 16 control blocks (512 frames, 10.7 ms at 48 kHz) from the
value it had to the new one, linearly (drive, resonance) or at a constant
ratio per block (cutoff), then ramped within each block as before; an
unchanged value is the zone's own, bit for bit. A 100 Hz sine (−12 dBFS),
lp24, changed mid-note: cutoff 8 kHz → 60 Hz and back, resonance 0 → 0.95
and back (cutoff 100 Hz), drive 0.05 → 1 and back (soft): each line moves
by 15.7–22.2 dB to within 0.0 dB of a note started with the new value;
the high band at the change (largest third difference in the 2048 frames
after it, re the tone) −92.1 / −86.7 / −107.7 / −103.5 / −70.1 / −69.8 dB
against the settled sound's −116.9 / −116.9 / −103.5 / −103.3 / −71.9 /
−71.9 (bound: 6 dB above the settled sound or −80 dB; worst +2.1 dB). Glides
of 8 blocks measured +12.9 dB (drive) and of 1 block +48.3 dB. Curve, filter
type and mix changed mid-note leave the sounding note byte-identical (cmp).
*Moved numbers*, each from the new tube or the blocker, none relaxed:
C5.1 worst 0.003 → 0.005 dB (fold, drive 0.75: the blocker's 10 Hz corner
on the wet share); tube's compensation off the reference level at drive 1,
−18 dBFS +3.86 → +2.82 dB, −6 dBFS −4.59 → −4.37 dB (a new curve). C5.3, lp24
at 20 kHz, 4×: 44.1 kHz tube −84.1 → −84.3 dB; **48 kHz tube −82.8 dB at
24 kHz** where there was no line (an asymmetric curve makes the 12th
harmonic, which folds onto fs/2; soft, hard and fold read −182 … −218, rounding
noise from the blocker where they read exactly 0 before); the worst of the AC
is now −82.8 dB (was −84.1). Filter off, reported: 48 kHz tube −70.0, 44.1
kHz soft −134.0, tube −73.2, hard −103.6, fold −76.0 dB. C5.2, C5.4, C5.5, R8
and R3 unchanged; nassau-dsp's filter_golden untouched. (Every tube number above is the same at 5 and 10 Hz.) Each watched
failing: the voice ignoring its zone's factor (2 checks), the default 2×
(4 fx checks, 7 program checks), cutoff, resonance or drive not reaching the
sounding voice (the follow check; drive also 3 C5.4 checks), mix following
(the byte check), the glide 1 block (+48.3 dB), the tube bias 0 (the even
harmonic check), the DC blocker a pass-through (DC −8.3 dB).
*Cost* (`fx_tests --bench`, Release, Xeon E5-1650 v3, load 6.9–9.6 on 12
cores from other jobs, best of 5, five runs): the default voice 37.6–39.5 ns
one layer (budget 40), **134.1–148.3** four layers (budget 140), against
main's 39.3–60.4 / 140.3–190.6 run alternately in the same session: the
four-layer budget is met only on a quiet box, and this change does not move it (it adds three glide steps per
32-frame block). Reported, not bounded (quietest run): one layer lp24 65.3,
cutoff moving 79.8; drive 0.5 soft filter off 2× 128.0, 4× 208.7; lp24 2×
180.6, 4× 309.5, 4× cutoff moving 368.4; tube 4× lp24 289.6; four layers lp24
163.0, driven 2× 275.6, 4× 406.6. No wall-clock assertion in the suite.
The kit bus stays fixed `soft`, 4×, 47 frames late while on (as built).
*The author's answers on this build, 2026-11-01*: the tube's C5.3 margin at
48 kHz (−82.8 dB against −80) is **accepted**, b = 1 stays; the 512-frame
glide is **accepted**; the DC blocker's corner is **10 Hz** (first built at
5 Hz: DC −179.6 / −167.9 / −166.6 dB, −0.27 dB at 20 Hz, C5.1 worst 0.004).

* **C5.1** Each drive curve's output RMS at moderate input stays within ± 1 dB
  of the input (output compensation).
* **C5.2** The ladder and SVF match nassau-analogue's goldens for the same
  input and settings (`cmp`), because they are the same code.
* **C5.3** Aliasing: a 10 kHz sine through full drive at the chosen
  oversampling factor (the zone default, 4×), **with lp24 at 20 kHz in the
  path** (defined so on 2026-11-01), has no component above 0.5·fs − 2 kHz
  louder than −80 dB relative to the fundamental. With the filter `off` the
  same numbers are printed as a report, not checked.
* **C5.4** A zone with drive 0 at note-on runs without oversampling; automating
  its drive above 0 mid-note changes the sound but not the oversampling of
  that voice (the next note's voice is oversampled, at its zone's factor).
* **C5.5** Two overlapping hits through the kit-bus drive show intermodulation
  products absent when each hit is rendered alone and summed (measured
  spectrum difference).
* *(added 2026-11-01)* **C5.6** `tube` makes even harmonics (2nd above the
  3rd at the reference level) and the DC it makes is blocked on the voice.
* *(added 2026-11-01)* **C5.7** Oversampling is a zone attribute (2× or 4×,
  default 4×): both work, the default renders as 4× byte for byte, and it
  reads, writes, compares and edits (overlay `.was` pairs) like any other.
* *(added 2026-11-01)* **C5.8** Drive, cutoff and resonance changed mid-note
  are heard in the sounding voice, without a click (measured); curve, filter
  type and mix are not.
* *(2026-11-01)* **Budget**: C2.6's 40 ns (one layer) and 140 ns (four
  layers) bound the default voice (filter `off`, undriven), measured with
  `fx_tests --bench` and stated with the box's load; filtered and driven
  voices are measured and reported. No wall-clock check runs in the suite.

### C6 — Outputs

DESIGN §3.1.

**DONE 2026-10-27; C6.3 finished and the routing rule replaced 2026-11-01**
(DESIGN §13, settled 2026-11-01). `Tests/outputs_tests` 41/41, `program_tests`
88/88; API (appends): `Instrument::process(…, outBuses, numBuses, n)` (qbx's
`outBuses[bus][channel]`, bus 0 main, bus k aux k), `setRoutedOutputs(mask)` /
`routedOutputs()` (bit k = aux k+1, as `twSamplerHost`; qbx's
`setRoutedOutBuses` mask >> 1), `kitBusOversampling(output)`, `kNumOutputs` =
9; program attributes `busDrive1`…`busDrive8` and `outputLevel1`…`outputLevel8`
(aux k's; missing = 0 and 0 dB; `busDrive` / `outputLevel` stay main's),
appended to the program table, so the reader, the writer (which writes all
sixteen, defaults included, as it writes every attribute), equality and the
edits take them; `Program::auxBusDrive[8]` / `auxOutputLevel[8]`.
`kRouteFadeFrames` is deprecated and unused (kept so code naming it compiles).
C6.1: a zone on aux 3, routed: aux 3 equals the zone's render on main (cmp,
RMS 0.199933 since filter `off` became the default; 0.199654 before), main's
peak exactly 0, the other aux outputs silent; with a main zone sounding too,
main is exactly that zone alone (cmp) and the aux 3 tone on main −298.1 dB.
C6.2: aux 3 unrouted, main is cmp-equal to the zone set to main in five ways
(mask 0; every aux but 3; aux 3 masked but a null buffer; numBuses 1; the
two-pointer process()), with main's bus drive 0.5, output level −3 dB, the
zone oversampled and notes interleaved A B A B; the zone moves main by up to
0.225. C6.3, at `busDrive` 0.2, `busDrive3` 0.7, `busDrive5` 0.9 (nothing on
aux 5): main is exactly zone A driven alone at 0.2 and aux 3 exactly zone B
driven at 0.7 (B alone on main at `busDrive` 0.7, cmp), 0.0499 away from B at
main's amount; `busDrive` 0.2 → 0.45 moves main by 0.0186 and leaves aux 3
byte-identical, `busDrive3` 0.7 → 0.35 moves aux 3 by 0.0412 and leaves main
byte-identical; intermodulation across outputs −281.3 / −286.2 dB against
−25.3 dB on one shared bus; B unrouted takes main's amount (cmp); `busDrive3`
0 leaves aux 3 undriven while main's bus drives (cmp); buses of unfed aux
outputs at rest (aux 5 at 0.9 included). Levels: `outputLevel` −3,
`outputLevel3` −9: main is A at −3 dB and aux 3 B at −9 dB (cmp, −9.00 dB
RMS); moving `outputLevel3` leaves main byte-identical; a fallback takes main's
level (cmp). Program format: the table order is pinned, each `busDrive<k>` /
`outputLevel<k>` reaches aux k's slot and no other (k = 1…8), out-of-range
values are refused, `busDrive9` is kept as an unknown attribute, absolute and
relative (clamped) edits return the absolute values they replaced and restore
exactly. **Routing changes under a sounding voice**: the voice keeps the output
it started on, and later notes follow (replaces the 256-frame crossfade):
unrouted at frame 12055 under B (bus drives on: main 0.3, aux 3 0.5), aux 3 is
byte-identical to the unchanged render throughout (B keeps its bus too) and
main is silent until C starts at 13055, then exactly C set to main (cmp); the
reverse (routed under B): main is byte-identical to B fallen back to main
throughout, aux 3 exactly C routed (cmp). A voice whose output the host hands
no buffer for (frames 12055–24155, mask unchanged): nothing reaches main,
nothing is written to aux 3, and aux 3 is byte-identical to the unchanged
render before and after the gap (the voice ran on: the gap is 55.46 periods,
so a paused voice would come back out of phase); likewise for a voice kept on
an unrouted aux. R8: byte-identical across reset and blocks 256 / 37 / 1000,
all nine outputs, seven routing changes and aux 8's buffer withdrawn under
its voice, four different bus drives and an aux level. R3: 0 allocations over
3000 blocks, 429 routing changes, five buffer layouts, every bus at its own
amount. Main-only renders, and steady-routing renders at bus drive 0 and
0 dB on all nine outputs, are byte-identical to before this change (a hash
over 200 blocks × nine outputs, main and origin/main 9983f1a builds).
Per-output cost unchanged (Release, Xeon E5-1650 v3, load 4.5 → 2.2, three
runs, base / this): one voice on main 38.5–58.0 / 38.0–39.7 ns, on aux 3
routed 40.0–41.8 / 41.1–41.4, aux 3 falling back 38.4–40.1 / 39.1–61.6 (one
outlier each), main's bus at 0.5 alone 158.0–164.1 / 157.3–164.4, nine buses
1441.1–1480.2 / 1432.2–1489.7; one voice on aux 3 with every bus at 0.5:
371.7–386.4 ns; per voice (fx_tests, lp24) 65.4–68.0 / 65.4–67.9. Each
watched failing: aux buses taking `busDrive` (5 checks), aux outputs taking
`outputLevel` (4), a sounding voice following the routing (7), a voice without
a buffer moved to main (2), ended (2) or paused (1), an aux bus stopping with
its routing under a voice it still carries (1), a voice's output ignoring a
null buffer (3), `busDrive3` misnamed in the table (8), `outputLevel5` written
into aux 4's slot (2). Before 2026-11-01 (the C6 PR): one `busDrive` drove
every output's bus and the routing crossfaded sounding voices over 256 frames
(a step 1.00× the tone's own against 34.7× for a hard switch); both are gone.

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
**Revised 2026-11-01** (DESIGN §13, settled 2026-11-01): `beats` spans the
loop `[loopStart, loopEnd)`, not the played length, and tempo modes ignore the
key. `tempo_tests` 31/31. A layer with a 0.25 s lead-in (2 kHz), the 2-beat
1 s loop and a 0.5 s tail (3 kHz) after `loopEnd`, beats 2, at 100 BPM: loop
period 1200.000 ms, pitch −0.00001 cent off −3.156413 st; the lead-in at
1666.6667 Hz (+0.00006 cent: the same rate) and over by 0.3 s;
`tempo-oneshot` over the same layer sounds for 2100.021 ms (1.75 s × 1.2), tail
at 2500.000 Hz. Keys 60, 67 and 48 render the same bytes (cmp) in
`tempo-loop` and `tempo-oneshot` by resampling and in `tempo-loop` by
stretch (key tracking on); a plain `loop` zone still plays key 67 at
1498.3071 Hz (+7 st). The C7.1 / C7.2 numbers above are unchanged (their
layers have no loop points and are played at the root key); the R8 and R3
cases now play their tempo voices untransposed (same checks, same results:
byte-identical, 0 allocations). Filter key tracking still follows the key in
tempo modes (it is not a transposition). Accepted as built: a sounding
`tempo-oneshot` follows a tempo change; the host passes the tempo with
`setTempo()` per block. Watched failing: beats over `[start, end)` (5
checks), tempo modes tracking the key (3), the lead-in at the sample's own
rate (2).
**Extended 2026-11-02** (DESIGN §13, settled 2026-11-02, for qbx Q4): a
host-supplied `Sample` carries `timeScale()` (stretched ÷ source length,
default 1; a non-finite or non-positive one is refused), and a `stretch` zone
in a tempo mode scales `start`, `end`, `loopStart`, `loopEnd` and the
`velToStart` skip by it at note-on. `tempo_tests` 39/39. A buffer rendered
1.2× (0.3 s lead-in, 1.2 s loop, 0.6 s tail) handed at scale 1.2, positions in
source frames, 100 BPM: loop period 1200.000 ms, carrier 1000.0000 Hz
(−0.00000 cent), lead-in 0.3 s at 2000.0000 Hz; `tempo-oneshot` over source
[0.1 s, 1.5 s) sounds for 1680.000 ms; velocity 64 with velToStart 12700 starts
at frame 7559.999 by stretch and 6300.000 by resample; a `loop` zone renders
scale 1.2 and scale 1 byte-identically. Watched failing: positions unscaled (3
checks), loop points only (2), start only (1), end only (1), skip unscaled (1),
the scale applied to every zone (2).

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

**The author's decisions of 2026-11-01 built, 2026-11-01** (DESIGN §8, §12, §13).
`Tests/overlay_tests` 100/100 (54 new), `Tests/program_tests` 118/118 (15
new). *libraryVersion*: an appended text program attribute, written only when
set (a program without one keeps its bytes); the overlay's existing
`baseVersion` (name kept: it is in written overlays and §12) now means the
base's `libraryVersion`, recorded with the overlay's first edit and cleared by
its undo. On base "3.1" an overlay made on "3" reports `baseDrift`
{libraryVersion, 3, 3.1}, applies every edit and renders byte-identical to
"3" (cmp); flatten clears `libraryVersion`. *Zone lists*: `parseZoneScope`
reads "all", "17", "3 5 7"; `zone="5 9 17"` `tune +2` is recorded as three
set-zones with `.was` 1 / −3 / 0; on v4 exactly #2 is orphaned (id 17) and the
result equals v4 with zones 5 and 9 edited (cmp); a stored list is refused.
*set-program*: `outputLevel -6`, `masterTune +0.5`, `busDrive2 0.3` recorded as
one `set-program` with `.was`; equal to the direct edit (cmp; RMS 0.3191 →
0.1597); on a base with outputLevel −3 reported drifted {outputLevel, 0, −3}
and −6 plays (cmp); `version`/`libraryVersion` refused (recorded and stored);
undo gives the same overlay bytes and render (cmp). *Structural*: `add-zone`
/ `add-layer` take ids from **1,000,000** (zone 1000000, layer 1000001, a
layer 1000002 in base zone 9) and equal the same added by hand (cmp; RMS from
key 42's note-on 0.4343 → 0.5519); a base adding zones after the overlay takes
19, 21 and every overlay edit still applies, all ids distinct; `addZone`
stops at 999999 and the reader refuses `nextId` > 1000000; on its own
flattened program (which keeps ids 1000000+) the adds are refused "already in
the base" and a new overlay hands out 1000003. `hide-zone 9`: what plays lacks
zone 9 and layer 10, the base keeps them, cmp with the zone removed by hand
(RMS key 38 → 40: 0.3636 → 0.2493), flatten drops it, undo puts it back
second. Every verb kind round-trips byte-stable; on v4 the edits naming zone
17 / layer 18 (#0, #1, the add-layer into 17) are orphaned, flatten proceeds
and drops them. Watched failing (21 sabotages, each bit 1–13 checks): version
mismatch not reported (3), baseVersion not recorded (6), stored list not
named for expansion (1), `parseZoneScope` keeping one id (6), set-program
drift ignored (1), set-program of `libraryVersion` allowed (1), overlay ids
from the program's counter (13), `addZone` ignoring the range (3), collision
not refused (1), hide not applied (6), undo of add-zone / add-layer /
hide / set-program / the baseVersion stamp not undone (1 each), equality
ignoring `libraryVersion` (1), empty `libraryVersion` written (2), flatten
keeping it (1), reader letting `nextId` into the range (1) or raising it past
overlay ids (3), overlay `nextId` not written (1), add-layer to a missing
zone refused instead of orphaned (2), an add-zone below the range accepted
(1).
Not built then: hiding a layer and re-basing; both followed the author's
answers on #24 (below).

**The author's answers on #24 built, 2026-11-01** (DESIGN §8, §13).
`Tests/overlay_tests` 121/121 (21 new). Flatten keeping the overlay's ids is
accepted as built. *hide-layer*: `<hide-layer layer="6"/>` leaves zone 5
without layer 6 while the base keeps it; cmp with the layer removed by hand
(RMS from key 36's note-on to key 38's 0.2503 → 0); flatten drops it; an
edit of the hidden layer is refused; orphaned (naming the layer) on a base
without the layer or without its zone (18 on v4); `all`, a list, another
attribute or no layer refused; undo puts it back in its place (first of two
layers). *Rebase*: an overlay made on version 3 (#0 attack, #1 layer tune,
#2 set-program outputLevel, #3 zone 9, #4 attack again, #5 an unknown verb,
#6 a hand-written attack over #4 with a stale `.was`, #7 a `.was` spelled
"0.0") on version 4 (zone 17 attack 0.05, outputLevel −3, zone 9 dropped)
reports 3 → 4, #0 / #2 / #6 drifted, #3 orphaned; `rebaseOverlay` stamps 4 and
rewrites exactly #0 / #2 / #6 (`attack.was="0.05"`, `outputLevel.was="-3"`,
and the chained #6 `attack.was="0.5"`, the value under it with #4 applied),
leaves #1 / #3 / #4 / #5 / #7 byte for byte; afterwards no mismatch, no
drift, #3 still orphaned, and what plays is unchanged (structural and cmp);
the rebased overlay round-trips byte-stable; a second rebase changes
nothing; `undoRebase` restores the overlay's bytes and what played before,
and is refused after a further edit. Watched failing (9 sabotages):
hide-layer not applied (10), its undo not reinserting (2), rebase not
re-stamping (2), not refreshing (4), refreshing against the bare new base
instead of the running program (3), refreshing edits that did not drift (1),
`undoRebase` ignoring later edits (1) or leaving `current` (1), rebase
leaving `current` on the old base (1).

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

**DONE 2026-10-27** (qbx#312, QBX-250; core pinned at 041456a, moved to
5adc61a by qbx#319 with no golden or bound moving). Q0.1: `tw/sampler` builds
the core and registers `tw.native.nassaudigital`; `check_layering.py` clean.
The core is not a submodule: it is found by the locator (one pin per repo,
§9.2), so Q0.1's "submodule" reads as "built from source by qbx". Q0.2:
`twSamplerHost::stats()` reports what `process()` itself last ran on (program
serial, routed mask, sample count, voices), so the lock-free read is observed,
not assumed. Q0.3: `sampler_q0_engine_seam.qxa` silent before frame 1000, RMS
0.49997 / 0.24998 after against the file's 0.49999 / 0.24999, `cmp` against
the golden. Q0.4: the project grows by 0 bytes with a 10 MiB sample installed
(`sampler_slot_test`). Corrections from the build: qbx proposal 56 §11 "Q0".

### Q1 — Model and verbs

DESIGN §9.2, §9.3.

* **Q1.1** `objects/sampler` wraps the core's program; `add-zone`, `set-zone`,
  `set-layer` and `select-zones` are actions addressed by `trackPath` +
  `slotIndex` + zone id, each undone exactly by one undo (asserted on the program).
* **Q1.2** The program is saved in the project and reloads identically
  (structural equality); a program element written by a newer version keeps
  its unknown attributes.

**DONE 2026-10-27** (qbx#317, QBX-253). Q1.1: `sampler_q1_verbs.qxa` captures
the program, runs each of `add-zone`, `remove-zone`, `add-layer`,
`remove-layer`, `set-zone`, `set-layer`, `set-sampler-program` and
`select-zones`, asserts it changed, undoes once and asserts it equal to the
capture by the core's own equality (an undone `add-zone` everywhere but
`nextId`, which DESIGN §2 never reuses: asserted separately at 8). `tune` +95
on all zones from 2/−10/0 gives 96 (clamped)/85/95 and one undo restores
2/−10/0; relative edits on enums and bools are refused. Q1.2:
`sampler_q1_persist.qxa` save → reload → save → reload is structurally equal;
`sampler_q1_newer_program.qxa` keeps a `version="2"` program's unknown
attributes (program, zone, layer) and elements, in order, through load, edit,
save and reload. 14 sabotages, all caught. The program is stored as character
data, not nested elements (QDom sorts attributes), and five assumptions await
the author (selection unsaved and undoable, `=` for a negative absolute value,
ids from `nextId`, one snapshot per edit, the program object made by the first
edit rather than at insert): qbx proposal 56 §11 "Q1". Open: QBX-254
(`remove-plugin` + undo loses the program).

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

**DONE 2026-11-01** (qbx#330, QBX-263). Q2.1 (`sampler_q2_generations.qxa`):
with the job held across the `set-layer` edit, installing the generation
advances the track's content epoch (11 → 12), and the next render equals the
new sample's single-generation golden byte for byte, silent before frame 1000.
Q2.2: two renders with a swap between them each equal their own sample's
golden (17 pages each), also after undo, redo and a 2 → 1 → 2 channel rebuild.
Q2.3 (`sampler_q2_barrier.qxa`): a job held 1500 ms makes the render log
`run barrier: waited 1504 ms for 1 sampler generation(s) on track 0` exactly
once, and its bytes equal a render started after the layer was ready; past the
bound (`SMARAGD_SAMPLER_SETTLE_MS`, default 30 s) the render is refused with
the reason. Not gated: "no install during a render" (no script can make a job
finish mid-render) and live playback. Corrections: qbx proposal 56 §11 "Q2".

### Q3 — Live asset links (decided: from the start)

DESIGN §6, §6.2.

* **Q3.1** A layer linked to an asset: an edit of that asset produces a new
  generation, and the next render hears the edit.
* **Q3.2** An action that does not touch the asset produces no re-render of it
  (render counter unchanged).
* **Q3.3** Linking an asset that contains the sampler's own track is refused.
* **Q3.4** Renaming the asset leaves the link intact, across a save and a
  reload.

**DONE 2026-11-02** (qbx#336, QBX-266). Q3.1 (`sampler_q3_asset_link.qxa`): an
edit inside the linked arrangement is heard by the next render, rms
0.5 → 0.25 → 0.5 in two-sided bands, both after the 150 ms bounce debounce and
through an immediate render whose settle logs the run-barrier line. Q3.2: three
actions that do not touch the asset leave the `renders` counter and the
generation unchanged, after the debounce and after a settle. Q3.3
(`sampler_q3_link_cycle.qxa`): refused for an asset over the master root, for
an arrangement holding the sampler's own track, and through a second sampler's
link; positive controls pass. Q3.4: the rename (new verb `rename-asset`)
starts no bounce, and after save and reload the link names the renamed asset
and an edit is still heard. A bounce superseded by a newer edit is never
installed. qbx suite 551/551; 14 sabotages, each watched failing. Not gated: a
cycle closed after the link exists (the author ruled 2026-11-01 that such an
edit is refused with the same check: QBX-267), a bounce during playback.
Corrections: qbx proposal 56 §11 "Q3".

### Q4 — Tempo modes by stretching

DESIGN §5 (method `stretch`).

* **Q4.1** A 2-beat loop at 120 BPM played at 100 BPM by stretch measures a
  period of 2·60/100 s ± 1 ms with its pitch unchanged ± 5 cents.
* **Q4.2** A tempo change produces a new generation, and so does completion of
  the source's onset analysis; renders after each are byte-identical across two
  runs.

**DONE 2026-11-02** (qbx#339, QBX-268; core part nassau-digital#27, pinned at
a82a698). Q4.1 (`sampler_q4_stretch.qxa`): a 2-beat loop at 120 BPM played at
100 BPM by stretch measures a period of 1200.0000 ms (57600.0000 frames), with
pitch 999.772 Hz (−0.39 cent) and 1000.39 Hz (+0.67 cent) in two windows. The
unstretched control at 120 BPM measures 1000.0000 ms and 999.974 Hz. Loop
points set in source frames measure 900.0000 ms, which holds only because the
core scales the positions. Q4.2 (`sampler_q4_generations.qxa`), from an empty
onset store with the analysis held: the tempo change makes generation 3,
stretched without onsets; completion of the analysis (4 onsets) makes
generation 4. The render after each equals, byte for byte, a reference render
from an earlier run; 3 runs. A render issued straight after `set-tempo` waits
at the run barrier (`waited 1536 ms`). 10 qbx and 6 core sabotages, each
watched failing; qbx suite 559/559. Not gated: a stretch during playback, an
analysis racing a render, the loop seam (stretched with the whole buffer, not
circularly), tune and LFO drift in tempo modes (accepted, DESIGN §13), and
renders on macOS and Windows. Corrections: qbx proposal 56 §11 "Q4".

### Q5 — Live play

DESIGN §9.4.

* **Q5.1** Headless: an armed track, an injected live NoteOn; the onset lies
  within the live lane's block-plus-lead bound, and the lane reports no
  ownership refusal.
* **Q5.2** Hand check by the author on the Windows 11 machine with the USB
  keyboard: the sampler's live onset, measured the same way as
  nassau-analogue's on the same device and buffer size, is within one device
  block of it. Recorded here with both numbers.

**Q5.1 DONE 2026-11-03** (qbx#342, QBX-271), with no product code change: the
sampler takes the live path every instrument takes. "The live lane's
block-plus-lead bound" is 4 ring + 2 lead + 1 device block = 7168 frames at
the headless 1024-frame block (the author, 2026-11-03; the literal one block
plus lead, 3072, is met by no instrument, the test sine included at
4025–5008). `sampler_q5_live_play` (transport stopped): onset 4646–4936
frames over 25 runs; the test sine armed on the same input and panned to the
other channel starts exactly 1 frame later (bound |diff| ≤ 64, so a sampler
one block late fails). `sampler_q5_live_playing` (transport running, where
the ownership half means something): 2324–2774 frames, `liveOwnedRefusals`
and `liveThreadRefusals` 0 in every run, also with a generation installed
while live-owned. `sampler_q5_live_generation`: a swap under a sounding voice
leaves it unchanged and the next note plays the new sample. Six of seven
sabotages bite; removing `retireClosureNodes` bites nothing. Not gated: any
device block but 1024, a tempo-mode zone live, high polyphony, a bounce over
an armed track (qbx QBX-274).

**Q5.2 OPEN**: the author's hand check. Procedure in qbx#342 and qbx proposal
56 §11 "Q5 as built" item 6.

### Q6 — Recording into the instrument

DESIGN §7.

* **Q6.1** `record-start` → stop → `place-sampler-recording`: the next note
  plays the recording at its RMS ± 2 %; one undo removes it.
* **Q6.2** Where it lands: Pad view → the selected pad's key; Key view on an
  empty program → a zone over the whole keyboard; Key view on a program with
  zones → a one-key zone on the last key played. One case each.

**DONE 2026-11-03** (qbx#341, QBX-272). Q6.1 (`sampler_q6_record.qxa`):
record-start, stop, and the take lands as a whole-keyboard zone; the next note
plays it at 0.999967 / 0.999945 of the recording's own RMS (band ±2 %;
absolute 0.49997 / 0.24998). One undo brings back the program as it was,
removes the registered asset and renders silence; redo renders the same bytes.
Q6.2, one case each: Pad view, bank 1 pad 5, lands a one-key zone on key 57,
and a second take replaces that zone's layer 1 (`sampler_q6_land_pad.qxa`);
Key view on an empty program lands a zone over 0–127 rooted on 60, measured
480 / 960 / 240 Hz at the root and an octave either side
(`sampler_q6_land_key_empty.qxa`); Key view on a program with zones lands on
the last of two injected keys (67, not 64), the existing zone untouched, and a
take before any key is refused (`sampler_q6_land_key_zones.qxa`). 14
sabotages, each watched failing; qbx suite 567/567. Not gated or not built:
cycle and punch regions for a take into the instrument, monitoring the
record input, pad reassignment (§13 pads, 2026-11-03: qbx QBX-277 moves the
Pad view onto the `<Pads>` table and the covering-zone landing rule).
Corrections: qbx proposal 56 §11 "Q6".

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

**DONE 2026-11-03** (qbx#340, QBX-273). The sampler reports nine stereo buses
(Main, Aux 1..8) and hands the core the routed mask; a bus routed in the
processor's pre-roll renders into the instrument's own scratch, so a note-on
there keeps its route. Q8.1 (`sampler_q8_outputs.qxa`): the return soloed
equals `goldens/sampler_q8_kick.wav` byte for byte, and with the return muted
the Drums main equals `goldens/sampler_q8_hat.wav`, RMS 0 over [0, 24000)
where only the kick plays. Q8.2 (`sampler_q8_duck.qxa`): inside the kick the
keyed bass measures 0.124997 / 0.062499 against a dry 0.499987 / 0.24999
(out/dry 0.25000, band ±1 %), the Drums track left unmuted; unrouted the bass
is dry again, after undo ducked again. Q8.3: the render right after
`clear-return-source` equals `goldens/sampler_q8_mixed.wav` from frame 0,
again after removing the return; the track's content epoch advances across
each unroute. 7 sabotages, each watched failing; qbx suite 562/562. Found and
fixed on the way: after `clear-return-source` a slot keyed from the former
return kept ducking from cached pages (proposal 55, any plugin). Not gated:
per-output `busDrive<k>` / `outputLevel<k>` heard through qbx (C6.3 covers
the core), the live path, several aux buses routed at once. `set-sampler-
routing` is struck (the author, 2026-11-03): the return tracks are the only
writer of the mask. Corrections: qbx proposal 56 §11 "Q8".

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
