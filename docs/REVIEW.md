# Review of DESIGN.md, 2026-10-26

An independent review of the first draft of [`DESIGN.md`](DESIGN.md), asked for
by the author. The reviewer (Claude Fable 5.1, run as a sub-agent with the
draft and read access to qbx, nassau-analogue and nassau-plugin-sdk, and none
of the conversation that produced the draft) was told to verify the draft's
claims about qbx against the code and not to trust them.

The review is kept **verbatim** below. The section after it,
[Response](#response), says what the design did with each finding. Line
numbers refer to the qbx and nassau-analogue trees of 2026-10-26.

---

## Summary verdict

The architecture choice (dependency-free core, first front end native in qbx,
player later) is sound and the qbx claims are mostly right: a `tw`-format
instrument, the vocoder, assets, recording and `.qxa` all exist. Three things
are under-specified enough to stall the build: (1) **the twPlugin ABI has no
channel for PCM buffers or a typed program**, and the engine factory cannot
reach the app model, so §9's "model pushes immutable snapshots down" needs a
new seam that the doc has not named; (2) the §5/§6 "next note uses it" rule is
a live-lane rule written as if it applied to renders, where it would break
determinism; (3) the model is missing a root key, a sample rate, a channel
width and a loop-length-in-beats, and several listed features contradict each
other. Fixable, but fix them in the document before `GATES.md`.

## Findings

**1. [major] "Instrument is freeze-path only" is not the obstacle to live play;
the live lane is, and it has conditions.** Invariant 42
(`plugins/CONTRACT.md:611-619`) only says the *legacy positionless pull*
answers silence. Live play exists via live ownership (`:921-939`, `:941-969`)
and the `LiveGraphPump` (`playback/CONTRACT.md:90-109`). Consequences the doc
omits: a lane exists only while the track is **armed or monitoring**
(`shell/CONTRACT.md:812-813`); the live lane **refuses when device rate !=
project rate** (`playback/CONTRACT.md:245-253`), so a 44.1 k project on a 48 k
WASAPI device has no live sampler at all; latency = device block (the pump's
block is the device's `bufferFrames`, fallback 1024, `slivemonitor.cpp:705-706`)
+ two blocks of lead (`playback/CONTRACT.md:192-202`) + device buffer. At 1024
frames that is roughly 60-80 ms, which is not "play it on the keyboard"
territory; at ASIO 256 it is. **Fix:** state the live-play precondition
(armed/monitor, equal rates, ASIO or small WASAPI buffer) and a latency budget
in §9; make a measured round-trip a gate.

**2. [blocker] The ABI cannot carry PCM or a program; the doc assumes it
can.** `twPlugin` is `prepare/process/reset/params/saveState/loadState`
(`twplugin.h:44-70`), and the native instrument is minted by uid with no
handle to anything (`twpluginregistry.cc:707-708`). Today the 303's state is
params-only (`twnativeinstrument.cc:222-238`) and the slot stores an opaque
base64 blob in the `.qxp` (`spluginslot.h:283`, `spluginslot.cpp:308-312,387-388`).
Pushing megabytes of PCM through `loadState()` would serialize them into the
project file and copy on every edit. **Fix:** a capability-queried extension
interface (the pattern `twplugin.h:38-39` already names), e.g.
`twSamplerHost`: `setProgram(shared_ptr<const Program>)`,
`setBuffer(bufferId, shared_ptr<const PcmSnapshot>)`, swapped under the
processor mutex and read lock-free in `process()`; the slot's `saveState()`
then stores only the program *reference*, and `SPluginSlot` grows a typed
child object. The app may reach `tw/plugins` from `objects/track`
(`check_layering.py:383`), so the dynamic_cast is layering-legal there.

**3. [major] Where the core links is undecided, and qbx has no precedent for
an external source dependency.** `tw_plugins` may depend only on
core/graph/events (`check_layering.py:48`); `tw303a/CMakeLists.txt` has no
`FetchContent`/`add_subdirectory` of a sibling repo (grep: only vcpkg
packages). The doc says the program model lives both "in this repository"
(§0) and "in the project model" (§9) — two models. **Fix:** vendor the core as
a submodule under a new engine module `tw/sampler` (deps: core, events, plus
the core) that owns the `twPlugin` subclass; app side a slice
`objects/sampler` at the rank of `objects/midi` (`check_layering.py:142`:
track must not depend on it), whose `SObject`s *wrap* the core's program (one
model, Qt-free inside, undo outside). Say which.

**4. [major] Recording "into a key" has no seam today.** `place-recording` is
a thin verb over lane planners that land on a track lane
(`recplan/CONTRACT.md:1-7,49-59`); the growing clip is replaced by a
WAV-backed cut at stop (`wave/CONTRACT.md:105-113`), and the growing source is
`isReproducible()==false` (`sources/CONTRACT.md:159-163`), so the core cannot
hold it as an immutable buffer. "A moment later" therefore means: stop →
`toCapturingSource()` (one copy, `:169-173`) → asset → snapshot → next note.
The threshold start does not exist (shell grep finds only the log threshold;
**unverified** beyond that). **Fix:** a `PassVerbs` implementation for the
sampler (`place-sampler-recording`) and a §7 note that the sample is playable
at *stop*, not during capture; threshold is new recorder work.

**5. [blocker] §5/§6's availability rule is incoherent for renders.** Every
instrument page is a pure function of position and feed
(`FREEZE_PROTOCOL.md:116-119`), and a state-chunk change must bump the param
epoch and stale pages (`plugins/CONTRACT.md:217-241`). If buffer generation N+1
lands mid-render, page P (rendered with N) and P+1 (with N+1) disagree, and a
re-render of P *does* switch a sounding voice — the opposite of §6. **Precise
rule:** (a) a buffer *generation* is an integer per layer, part of the
snapshot the slot pushes; (b) installing a generation is a state change:
`bumpParamEpoch` + `invalidateRenderPath` on the track, so every page is
re-rendered against one generation; (c) the run barrier
(`SApplication::beginRun`, `FREEZE_PROTOCOL.md:129-140`) *pins* the generation
set for the run, and an offline render **waits at the barrier** until no layer
has an outstanding render — never inside a freeze; (d) "the next note after
availability" applies to the **live lane only**, where a voice reads the
generation current at its note-on. Determinism follows: render bytes depend on
(program, generations, feed), never on wall time.

**6. [major] The vocoder is for pre-rendering, not for the voice, and tempo
loops need data the model lacks.** Streaming `twGrainSource` renders
65536-frame blocks on worker threads and is never RT
(`sources/CONTRACT.md:23-31,81-84`); the pump never renders a page
(`playback/CONTRACT.md:183-191`). So §5 is right to pre-render, but a tempo
loop needs its native length in beats (or its tempo) and the tempo arrives per
chunk (`twpluginslotproc.cc:954-962`). The model has no `beats`/`loopTempo`
field, and the vocoder's transient keyframes take an onsets array
(`twpagedvocoder.h:110-112`) that comes from a sidecar aspect keyed on content
hash — a fresh recording has none until analysis runs. **Fix:** add `beats`
per layer; state that a tempo loop re-renders on tempo change and on
onset-analysis completion (two generations).

**7. [major] §3 overstates what nassau-analogue has.** `synth_filter.h` holds a
24 dB ZDF **low-pass** ladder with a saturator in the feedback (`:99-119`), a
12 dB SVF whose output is **lp only** (`:440-449`), and a **non-resonant** HP
cascade (`:457-458`). Resonant BP and HP "whose resonance saturates" do not
exist there. The header also depends on `synth_dsp.h`, `nassau_real` and a
32-sample control-block convention (`:188-216`). **Fix:** either scope v1 to
LP 12/24 + non-resonant HP, or budget a gate for a resonant SVF with BP/HP
outputs; name the shared library and the control-block contract.

**8. [major] Model gaps.** (a) No **root key** anywhere (§2, §12) although §7
depends on it; (b) no **sample rate** on a buffer: the core reads "immutable
PCM", so either the snapshot carries a rate the interpolator folds into the
pitch ratio, or the app pre-converts through `viewAtRate`
(`sources/CONTRACT.md:18-19`, linear, "not mastering-grade", `:231-232`); (c)
**width**: the 303 is mono-out (`twnativeinstrument.cc:116`); a sampler with
pan and a kit bus must declare 2 outputs (then `GenFold` on a mono project,
`WideGen` on 6; `plugins/CONTRACT.md:255-262`), and a mono layer beside a
stereo one in one zone needs a stated pan law; (d) **one-shot + loop**: a drum
loop played once at tempo is a fifth mode, not "one-shot" (plays to end) nor
"loop" (no end) — name `tempo-oneshot` or drop it; (e) **layer velocity
windows** inside a zone's velocity range that do not cover it give silent
notes — define the gap rule; (f) **ALL-scope relative edits** on enums
(`filter`, `driveCurve`, `mode`) are meaningless, and relative edits that
clamp are non-invertible unless the inverse stores per-zone absolute values;
(g) **choke vs stealing**: "choke before stealing" is fine, but choke across
*overlapping* zones that share a key would cut a layer the player is holding —
say choke is by group only; (h) **Key view record** "across the whole
keyboard" on a program that already has a drum map overlaps every drum (zones
may overlap, §2), layering the voice onto each hit; (i) a 4×4 pad view cannot
show GM channel 10 (35-81); pads need a key assignment/bank.

**9. [minor] Hand-selection needs a second MIDI reader.** The live event
source is drained by the pump and has one consumer
(`twliveeventsource.h:72-78`; `plugins/CONTRACT.md:978-979`). "Holding a key
selects the zone" needs a UI tap on the device side, not on the collect.

**10. [minor] Live asset links are expensive by construction.**
`arrangementChanged` fires on **every action** (`sproject.h:280-285`), so a
linked asset re-renders on unrelated edits; a sampler on a track inside its
own linked sub-arrangement is a freeze cycle answered with silence
(`FREEZE_PROTOCOL.md:36-39`). State the self-reference refusal and a debounce.

**11. [minor] Oversampling "only while drive > 0"** switches filter state
mid-voice when drive is automated through zero; define a ramp or make the
switch per note.

## Challenged decisions

- **"Loop, tempo" via the vocoder (decided).** Keep it, but the cost is
  finding 6: pre-render, two generations, O(buffer) memory per tempo. A
  cheaper v1 is resample-to-tempo (pitch moves) with vocoder as G-later.
- **Live asset link (decided).** Keep it; cost is finding 10 plus the
  generation rule (5). Recommend file + recording first, asset link as its own
  gate.
- **Native in Smaragd first (decided).** Agree; but it makes the
  `twSamplerHost` seam and `objects/sampler` slice qbx work that lands before
  any sound.
- **Programs as model data, not a blob (decided).** Agree; cost: a second
  serializer path. Have the core own one XML grammar and feed qbx's
  `QDomElement` subtree to it as text.

## Suggested gate order

- **G0 core voice, headless, no qbx:** load WAV, one zone, render 1 s at 48 k;
  reset→same notes→`cmp` byte-identical; pitch of key 72 vs 60 measured at
  2.000 ± 0.001 by autocorrelation.
- **G1 program model + XML round-trip:** parse → serialize → parse yields
  identical structure; unknown attributes survive; ids never reused (`nextId`
  in file).
- **G2 `tw/sampler` module + `twSamplerHost`:** `check_layering.py` clean;
  `plugins_test`-style pump: NoteOn at 1000 → exact silence before, RMS of the
  sample after; `insert-plugin format="tw" uid="tw.native.nassaudigital"`
  renders through a `.qxa` case with `cmp` against a golden.
- **G3 generations and determinism:** a `.qxa` that swaps a layer's file
  mid-session, renders twice, `cmp` equal; a render started with an
  outstanding generation waits at the barrier (assert the log line and the
  final bytes).
- **G4 live play:** armed track, `CaptureBackend`, injected live NoteOn; onset
  within `blockFrames*3 + deviceBuffer` frames; `liveOwnedRefusals == 0`.
- **G5 record into pad:** `record-start` → stop → `place-sampler-recording`;
  the next NoteOn renders the recorded RMS ± 2 %; undo removes the zone.
- **G6 drive/filter/kit bus:** golden renders for each curve; aliasing gate: a
  10 kHz sine through drive at 4× has no component above 0.5·fs − 2 kHz over
  −80 dB.
- **G7 tempo loop:** 2 bars at 120 → set-tempo 100 → the loop's measured
  period = 2·60/100 s ± 1 ms; render byte-identical across two runs.
- **G8 overlay:** base v3 + overlay; drop a zone in v4; the edit is kept and
  reported; flatten equals base+overlay byte for byte.

## Open questions for the author

1. Which output width does the instrument declare, and what happens on a mono
   project?
2. Does `viewAtRate` (linear) or the core's interpolator do the file-rate →
   project-rate conversion?
3. Is the program addressed by `.qxa` verbs as `trackPath`+`slotIndex` like
   `set-plugin-param`, or by a program name?
4. Is live play acceptable only on ASIO / small buffers, given finding 1?
5. Is the plugin player's reader the *same* code as the qbx loader
   (core-owned XML), or will the qbx side use `QDom`?
6. Round-robin (Q4): if "out", say so in §1 so the per-layer-state door stays
   shut.

Unverified: the exact live latency numbers (no device gate exists,
`playback/CONTRACT.md:342-347`), the absence of a threshold trigger beyond a
grep, and whether `twSamplerHost` can be reached from `objects/sampler`
without a new `APP_ENG` entry.

---

## Response

What the design did with each finding. Three of the reviewer's claims were
re-checked by hand before acting on them (finding 1's rate refusal, finding
7's filters, finding 10's `arrangementChanged`): all three hold.

| # | Verdict | In DESIGN.md |
|---|---|---|
| 1 | accepted in part | §9.4. The preconditions hold (armed or monitoring, device rate = project rate). The latency estimate does not: 60–80 ms is the 1024-frame fallback, used only when a backend reports no block size. The live lane otherwise runs in the device's own block, and nassau-analogue played live through it on Windows 11 with a USB keyboard at a latency the author found low. The gate measures the sampler's live onset against nassau-analogue's on the same device. |
| 2 | accepted | §9.1: the `twSamplerHost` extension; the slot's state holds the program reference, not PCM |
| 3 | accepted | §9.2: the core is a submodule under a new engine module `tw/sampler`; the app slice `objects/sampler` wraps the core's program, one model |
| 4 | accepted | §7: playable at stop, not during capture; `place-sampler-recording`; threshold start is new recorder work |
| 5 | accepted, the reviewer's rule verbatim | §6.1: buffer generations; installing one is a state change; renders pin generations at the run barrier and wait there; "next note" is the live lane's rule only |
| 6 | accepted | §2 `beats` per layer; §5: a tempo loop re-renders on tempo change and when onset analysis completes |
| 7 | accepted, v1 scoped down | §3: v1 is low-pass 12/24 from nassau-analogue plus a non-resonant high-pass; a resonant BP/HP filter is its own later gate |
| 8 a–i | accepted, each | §2 (root key, rate, channels, velocity gap rule, choke by group only), §2.1 (ALL scope: relative edits on numbers only, undo stores absolute values), §5 (`tempo-oneshot`), §7 (recording in Key view, pad banks) |
| 9 | accepted | §7: hand selection reads a UI tap of the device input, not the engine's event feed |
| 10 | accepted | §6.2: debounce, re-render only on a change in the asset's own content epoch, self-reference refused |
| 11 | accepted | §3: oversampling is decided per note, at note-on |

**Challenged decisions.** Two of them change what gets built first, so they
are the author's to settle, and §13 asks them: whether v1's tempo loop
resamples (pitch moves) with the vocoder later, and whether the live asset link
comes after files and recordings. The other two challenges agree with the
decision and only name its cost; the design takes both (one XML grammar,
owned by the core).

**Gate order.** Taken as the starting point for `GATES.md`, which is written
once §13 is answered.
