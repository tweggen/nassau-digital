# NassauDigital — Design

**NassauDigital** is the go-to sampler of the Nassau family. It scratches the
AKAI S3000 / E-mu ESI-32 itch: one-shot samples and drum loops, a few layers, a
drum kit mapped across the keyboard (General MIDI channel 10 and your own), and
"speak into the mic, play it on the keyboard" a moment later. It is
deliberately **not** a library engine that can do everything; §1 says where it
stops, and that limit is part of the contract, not a first-version shortcut.

**Where it lives** (decided 2026-10-26). This repository holds a
**dependency-free core**: the program model, its XML format and the voice DSP,
with no Qt, no IPlug2 and no SDK, like every Nassau DSP core. Its **first front
end is native in Smaragd** (qbx), as an instrument of qbx's own `tw` format,
because there everything around a sampler already exists: assets, recording,
the stretch vocoder, undoable actions, `.qxa` scripts and headless tests (§9).
A **plugin player** (VST3/CLAP/AU on nassau-plugin-sdk) that plays exported
programs from embedded audio follows later on the same core (§10). Programs
leaving Smaragd is postponed, not dropped.

This file is the specification; the schedule is [`GATES.md`](GATES.md). Every decision below that came from the author's interview of
2026-10-26 is marked **(decided)**; everything else is a proposal. The first
draft was reviewed independently; the review and what was done with each
finding are in [`REVIEW.md`](REVIEW.md).

---

## 1. Scope, and where it stops

In:

* one-shot and gated playback, forward loops, tempo-following loops (§5);
* a program of **zones** (key × velocity rectangles), each up to **4 layers**
  **(decided)**;
* per-zone drive, filter and amp envelope; an ALL scope that edits every zone
  at once **(decided)**;
* a kit bus with its own drive after all voices **(decided)**;
* choke groups, so an open hi-hat is cut by the closed one;
* **multiple outputs**: a zone can go to its own output, so a kick can key a
  ducking compressor or a gate on another track (§3.1);
* recording straight into a key or a pad (§7).

Out, permanently. This is the limit that keeps it from becoming Kontakt, whose
complexity comes from the model having no limit, not from its views:

* no scripting language inside the instrument;
* no modulation matrix: modulation is a fixed set of routes (§4.4);
* no per-layer effects chains; the kit bus has one drive, nothing pluggable;
* no disk streaming of huge multisampled libraries: samples are held in memory;
* no slicing a loop to fit the tempo inside the sampler. If that ever comes, it
  is a qbx feature for every sample, not a sampler feature **(decided)**.

A feature that would need any of these is answered "no" rather than designed.

## 2. The model

```
Program        name, polyphony, kit-bus drive, output level, master tune, nextId
 └─ Zone × n   id, keyLo..keyHi, velLo..velHi, rootKey, play mode (§5),
     │         drive, filter, amp envelope, modulation routes,
     │         pan, level, tune, choke group, pad (§7)
     └─ Layer × 1..4
               id, sample, start/end, loop points, beats (tempo modes),
               velLo..velHi, velFade, rrGroup, tune, level, pan, reverse
Sample         immutable PCM: channels (1 or 2), sample rate, frames,
               a generation (§6.1)
```

* **The zone is the editing unit**, as the keygroup was on the S3000. A layer
  carries only offsets; everything with a shape (filter, envelope) is the
  zone's.
* **Root key.** A zone plays its samples at their recorded pitch on `rootKey`;
  other keys transpose by their distance from it, unless the zone's key
  tracking is off (drums usually play one key).
* **Sample rate.** A sample keeps its own rate. The voice folds the ratio of
  the sample's rate to the output rate into its pitch ratio, so a 44.1 kHz
  sample in a 48 kHz project is one interpolator pass, not a conversion
  followed by a second one.
* **Channels.** A sample is mono or stereo; the instrument always renders
  **stereo** (§9.3). A mono layer is panned with a constant-power law; a
  stereo layer's pan is a balance.
* **Zones may overlap.** Overlapping zones are how layering across zones and
  crossfades are done; the layers inside a zone are for velocity layers and
  stacks.
* **Velocity windows are crossfaded** **(decided)**. A layer's window is
  clamped into its zone's. Each layer has a fade width (`velFade`, in velocity
  steps) at its window's edges, over which its level ramps with an
  equal-power curve, so two adjacent layers whose windows overlap by that width
  crossfade instead of switching. A width of 0 is a hard switch. A velocity
  inside the zone that no layer covers plays nothing, and the Map view marks
  the gap; the editor never invents a fill.
* **Round-robin is in** **(decided)**: layers of a zone that share an `rrGroup`
  number and overlapping velocity windows take turns. The rule that picks the
  next one is §2.2.
* **Choke groups** are by group number only: a note in group *g* cuts the
  voices of group *g*, and nothing else. Zones sharing a key do not choke each
  other unless they share a group.
* **Stable ids.** Every zone and layer has an id that never changes for its
  life and is never reused; the program keeps `nextId`. A library overlay (§8)
  and an undo step name zones by it.
* A **one-sample instrument is a program with one zone** covering the whole
  keyboard. There is no separate "simple" mode with its own data.

### 2.1 The ALL scope

Every zone parameter can be set for one zone, for the selected zones, or for
ALL.

* **Absolute** edits apply to every kind of parameter.
* **Relative** edits (`tune +2` on ALL raises every zone by two semitones from
  where each one is) apply to **numeric** parameters only. On an enum (filter
  type, drive curve, play mode) a relative edit is refused.
* A relative edit clamps per zone, so it cannot be inverted by the opposite
  edit. Its undo therefore restores each zone's **previous absolute value**.

### 2.2 Which round-robin layer plays

The author asked for round-robin with its per-layer state in the voice
**(decided)**. The voice keeps which layer it plays for its whole life. Choosing
that layer is the hard part, because qbx renders a track in independent pages,
in any order, and may re-render one alone (§6.1): a counter that advances with
every note would give a page rendered alone a different layer than the same
page rendered after its predecessor, and renders would stop being
byte-identical.

**Proposal:** the choice is a function of the note, not of the notes before it.
The layer is picked by a hash of the note's **position on the timeline** (in
project frames), its key and the zone id, modulo the group's size, with the
rule that it never repeats the layer of the same key's previous note when that
note is in the same page. **Decided: try this first**; a strict cycle only if
this does not satisfy in use. The same note at the same place always plays the
same layer, live and rendered, and neighbouring hits differ. It is not a strict
cycle (A B C A B C): a strict cycle needs the count of every earlier note of
the zone, which a page does not have. If a strict cycle matters, the count can
come from the track's MIDI ahead of the page (qbx's chase already reads back
for controllers); that is a qbx change and a later gate.

## 3. Signal path

Per voice, one voice per sounding zone:

```
 layer 1..4 ──► Σ ──► Drive ──► Filter ──► Amp ──► Pan ──┐
 (sample read,         (pre-filter     (resonance        │
  pitch, level)         waveshaper)     saturates)       │
                                                         ▼
                              all voices ──► Σ ──► Kit-bus Drive ──► Output (stereo)
```

* **Drive** before the filter **(decided)**: a waveshaper with input gain and a
  few curves (soft, tube, hard, fold) and output compensation, so turning it up
  adds grit, not level. The shouted vocal. **`tube` is asymmetric** (a biased
  triode: even harmonics), and a driven voice's wet signal passes a **DC
  blocker** (10 Hz) before it meets the dry **(decided 2026-11-01)**.
* **Filter** with distortion inside it **(decided)**. **v1:** the 24 dB/oct
  ZDF low-pass ladder whose resonance feedback saturates and the 12 dB/oct
  low-pass SVF, both from nassau-analogue (`Source/DSP/synth_filter.h`), plus
  its non-resonant 12/24 dB/oct high-pass. Resonant band-pass and high-pass
  modes do not exist there and are a **later gate** of their own. The
  analogue code assumes its `synth_dsp.h`, `nassau_real` and a 32-sample
  control block; sharing it means moving those with it (§11).
  **`off` is a filter type of its own and the default (decided
  2026-10-27):** a true bypass that costs nothing, so a zone plays its
  sample as it is, up to the interpolator, until a filter is chosen. It is an
  appended enum value, so older programs read unchanged; a program that names
  `lp24` keeps it. (C5 had made `lp24` at 20 kHz the default, which coloured
  every voice: −2.6 dB at 15 kHz, about 30 ns per voice.)
* **Kit-bus drive** after the sum **(decided)**: overlapping hits distort
  together, the 909 driven into a hot desk channel.
* **Oversampling.** Distortion aliases. A voice's drive and filter run
  oversampled if its zone's drive is above zero **at note-on**; the decision
  holds for the life of the voice, so automating drive through zero never
  switches filter state inside a note. The kit bus oversamples while its drive
  is above zero, ramped over one block. The factor is **a zone setting**,
  `oversample` = 2 or 4, **default 4×** **(decided 2026-11-01)**; the kit bus
  runs at 4×.
* **What follows a sounding voice (decided 2026-11-01):** drive, cutoff and
  resonance, glided to a new value over 16 control blocks (512 frames) so a
  change is heard without a click; curve, filter type, mix, the oversampling
  factor and every other zone parameter stay as at note-on.
* **Sample reading** uses one interpolator for every pitch (a windowed sinc or
  a 4-point Hermite: a measured choice). An S950/SP-1200-style bit and rate
  reduction is a candidate for the Drive's curve list, not a separate stage.

### 3.1 Outputs

The S3000 had a stereo pair and eight individual outputs, and a drum kit is
mixed through them: a kick on its own channel can duck the bass or open a gate,
and a snare can get its own reverb. NassauDigital has the same:

* **Main**, a stereo pair, plus **eight aux outputs** **(decided: 8)**, each a
  stereo pair, so a stereo sample or a panned mono one keeps its image on an
  aux output as on main. Every zone has an `output` field: main or aux 1–8.
* **Each output is a bus of its own** with its own sum, its own kit-bus
  drive and its own level (program attributes `busDrive1` … `busDrive8` and
  `outputLevel1` … `outputLevel8` for aux 1–8; `busDrive` and `outputLevel`
  are main's). A zone sent to an aux output does not also reach main, so the
  kick that keys the ducking is not doubled in the mix.
* **An aux output nobody listens to falls back to main.** qbx tells the
  instrument which outputs are routed (§9.1); a zone whose output is not routed
  plays through main, so a kit never goes silent because a return track is
  missing. Which outputs are routed is state, so renders stay deterministic.
* **A routing change reaches new notes only**: a sounding voice keeps the
  output it started on until it ends (settled 2026-11-01, §13).

**In qbx this needs new work.** qbx already reports a plugin's extra output
buses but routes only the first and drops the rest. Its multi-output design
exists (proposal 37 §5.4): a **return track** whose content is a tap on
`(track, slot, output)`, with no new engine types. What is missing is that
return track in the model, its cache sizing and its UI, planned as proposal 37
P9 and not built. Ducking and gating need one more thing: a plugin on another
track that reads the return track as its **sidechain** input. The sampler is
the first instrument to need both. **They come first, as a qbx proposal of
their own** **(decided)**, built before the sampler's multi-output gate: a qbx
feature every multi-output plugin and every ducking compressor uses, not a
sampler feature.

## 4. Voice parameters

### 4.1 Amp envelope

ADSR plus hold, per zone. One-shot zones ignore note-off (§5).

### 4.2 Filter

Type (`off` by default, decided 2026-10-27; §3), cutoff, resonance, key
tracking, envelope amount. Cutoff and resonance follow a sounding voice; the
type is latched at note-on (§3).

### 4.3 Drive

Amount, curve, mix, and the oversampling factor of a driven voice (2× or 4×,
default 4×; §3). The amount follows a sounding voice; curve, mix and factor are
latched at note-on.

### 4.4 Modulation, fixed routes

Not a matrix. **One LFO**, and no routes beyond this fixed set are foreseen
**(decided)**:

* velocity → level, cutoff, sample start;
* a filter envelope (ADSR) → cutoff;
* the LFO → pitch, cutoff, pan;
* mod wheel → LFO depth, cutoff.

### 4.5 Program

Polyphony **8, 16, 32, 64, 128 or 256 voices** **(decided)**, a fixed voice
pool allocated when the instrument is prepared, so changing it is a state
change, not something that happens on the audio path. The default is 32
**(decided)**. Choke groups are applied first, then the oldest voice is stolen. Also:
output level, master tune, kit-bus drive per output (§3.1).

## 5. Play modes and loops

Per zone:

| Mode | Note-off | Plays | Tempo |
|---|---|---|---|
| `oneshot` | ignored | start to end, once | no |
| `gated` | releases the envelope | start to end, once | no |
| `loop` | releases the envelope | loops between the loop points | no; pitch follows the key |
| `tempo-loop` | releases the envelope | loops, at the song tempo | yes |
| `tempo-oneshot` | ignored | once, at the song tempo | yes |

A tempo mode follows the tempo by one of two methods, chosen per zone, and
**both are in from the start** **(decided)**:

* **stretch**: the **same stretching qbx uses everywhere** **(decided)**, qbx's
  in-house phase vocoder (`twPagedVocoder`, `tw303a/sources`). The pitch stays.
* **resample**: the loop is played faster or slower, so its pitch moves with
  the tempo, as a turntable or an S3000 without time-stretch would. No render
  ahead is needed: it is a pitch ratio in the voice, available at once.

Both need the layer's length in **beats** (`beats`), which the editor proposes
from the file and the user confirms. `beats` spans the layer's **loop**
`[loopStart, loopEnd)` (without loop points, that is `[start, end)`), not the
played length; a lead-in before the loop plays at the same rate. A tempo mode
**ignores the key**: a tempo-synced layer plays at the tempo's rate whichever
key triggers it, so key tracking does not transpose it (both settled
2026-11-01, §13). A sounding `tempo-oneshot` follows a tempo change like a
`tempo-loop`, and the host passes the tempo per block (`setTempo()`), as
built. What follows is about **stretch** only.

The stretched buffer is longer or shorter than the source the layer's
positions were set on, so the host hands it with a **time scale**, its length
over the source's (1 by default), and the core multiplies a `stretch` zone's
`start`, `end`, `loopStart`, `loopEnd` and velocity-to-start skip by it at
note-on; the program keeps source frames (settled 2026-11-02, §13).

The vocoder renders large blocks on worker threads and never runs on the audio
path, so a tempo layer is **rendered ahead** into a new generation of its
sample (§6.1). It is rendered again when the tempo changes and once more when
qbx's onset analysis of the source finishes (the vocoder places its transient
keyframes from those onsets; a fresh recording has none until then).

## 6. Sample sources

A layer's sample is one of:

1. **a file** (WAV, AIFF, FLAC; whatever qbx reads);
2. **a recording** made in the instrument (§7), stored as a project asset;
3. **a qbx asset**: anything you can edit in Smaragd (a clip, a comped take, a
   cut, a sub-arrangement), linked live **(decided)**, and **from the start**,
   not as a later gate **(decided)**.

**The core never sees where audio came from.** It is handed immutable sample
buffers and is told when to use a new one. A buffer is released on a worker,
never on the audio thread.

### 6.1 Generations, and what a render hears

New audio for a layer arrives while you play: a changed asset, a tempo change,
finished onset analysis. **(decided)** that nothing waits on that computation
and that a sounding voice is never switched. Renders must also stay
byte-identical. Both hold under these rules:

1. Every layer's sample has a **generation**, an integer that grows with each
   new buffer. The generation is part of the snapshot qbx hands the engine.
2. **Installing a generation is a state change** of the instrument: it bumps
   the instrument's parameter epoch and invalidates the track's render path, so
   every page is rendered again against one generation. No page is ever made of
   two.
3. **A render pins the generations** at qbx's run barrier. An offline render
   (a bounce) **waits at the barrier** until no layer has a render outstanding,
   never inside a page.
4. **"The next note after the data is available" is the live lane's rule.**
   When you play live, a voice reads the generation current at its note-on
   and keeps it to the end.

The bytes of a render then depend on the program, the generations and the
notes, never on the clock.

### 6.2 Live asset links

* qbx signals a change after **every** action (`arrangementChanged`), so the
  sampler re-renders a linked asset only when **that asset's own content
  epoch** moved, debounced, never on unrelated edits.
* A sampler cannot link an asset that contains the sampler's own track: that
  is a render cycle, and the link is refused when it is made.
* While a render is outstanding the layer shows a "rendering" mark; the
  previous generation keeps playing.

## 7. Views and recording

Fixed-size views, one model:

* **Key** (the default): a keyboard with the zones drawn on it.
* **Pad**: a 4×4 pad grid. Pads are **banks** of 16 consecutive keys starting
  at key 36 (the GM kick), so the GM drum range 35–81 is four banks; a pad can
  be reassigned to any key.
* **Map**: a key × velocity grid with every zone as a rectangle, for editing
  drum maps and splits; velocity gaps (§2) are marked.
* **Zone**: the selected zone's knob rows (drive, filter, envelope, layers).

**Choosing the view** **(decided)**. A new instance starts in the view a
per-user setting names (Key by default). Switching an instance to Pad is saved
with that instance in the project, and it changes where recordings land. The view, the selected pad and the record input are the host's instance
state, saved beside the program and never in it; the last key played is live
and not saved (§13, settled 2026-11-02).

**Recording** **(decided)** goes through qbx's recording, so input monitoring
and latency compensation are qbx's. A new verb, `place-sampler-recording`,
lands the take in the instrument instead of on a track lane. **The sample is
playable when recording stops**, not while it is being captured: the growing
take is not a fixed buffer until then. Where it lands:

* **Pad view**: on the selected pad's key, as that pad's zone (replacing
  layer 1 if the zone exists).
* **Key view**, empty program: a zone across the whole keyboard, played
  chromatically around its root key, 60 (§13, 2026-11-03).
* **Key view**, program with zones: a new one-key zone on the **last key
  played**, so a recording never lands on top of an existing drum map.
* The Map and Zone views land a take as the Key view does; the pad's zone is
  the first one-key zone on the pad's key (§13, 2026-11-03).

A **threshold start** (recording begins at the first sound) is new work in
qbx's recorder, not something that exists.

**Your hands choose what you edit**, as on the S3000 and the MPC. Holding a key
or hitting a pad selects that zone; holding several selects several; setting a
range is done by playing its lowest and highest key. This needs the editor to
see the MIDI input itself: qbx's live event feed has one reader, the engine, so
the editor gets its own tap of the device input.

## 8. Libraries: a read-only base with your edits on top

There is no import format to support yet **(decided)**, so NassauDigital defines
its own program format (§12). The design still has to allow libraries you must
not change, because every sampler meets them:

* A program can be based on a **read-only base program**. Your edits are kept
  as a separate **overlay**: the list of changes, each naming a zone or layer
  by its stable id.
* What you hear is the base with the overlay applied. The base file is never
  written.
* If a new version of the base drops a zone you had edited, the edit is
  **kept and shown greyed out**, never silently dropped.
* **Flatten** turns base + overlay into a program of your own.
* **Every edit stores the value it replaces** **(decided 2026-10-27)**: beside
  each new value, the expected previous one (`attack.was`), for checking the
  edit against the base and for undo. Undo of an edit removes it and writes
  those previous values back.
* **A base that changed under an edit** **(decided 2026-10-27)**: when the
  base's value is no longer the edit's `.was`, the edit is still applied (the
  overlay's value plays) and reported as **drifted**, with what it expected
  and what the base has now. The values are compared as they are written in
  the format, so float noise does not count as a change.
* **ALL is expanded when the edit is made** **(decided 2026-10-27)**: an edit
  with ALL scope becomes one edit per zone that exists then, each with its own
  previous value. A zone a later base adds is not covered by it.
* **Relative input is resolved when the edit is made** **(decided
  2026-10-27)**: `tune +2` is stored as its absolute new and previous values;
  the typed relative form is never stored.
* **Library versions** **(decided 2026-11-01)**: a library names its version
  in the append-only program attribute `libraryVersion` (text; written only
  when set). The overlay's `baseVersion` is the base's `libraryVersion` when
  the overlay was made, recorded with its first edit. Applied to a base of
  another version, the overlay still applies and the mismatch is **reported**,
  in the shape drift uses (`libraryVersion`, was, now); an overlay with no
  `baseVersion` is not compared. A flattened program is no longer that
  library, so it carries no `libraryVersion`.
* **Zone lists are expanded when the edit is made** **(decided 2026-11-01)**:
  `zone="3 5 7"` becomes one edit per zone, each with its own previous value,
  like ALL; a base that lacks one of the zones orphans only that zone's edit.
  A stored list (or `all`) is refused, not guessed.
* **Program attributes are edited with `set-program`** **(the author's
  decision of 2026-11-01)**: `attr`/`attr.was` pairs like `set-zone`, drift
  reported the same way, never orphaned. Every program attribute but
  `version` (the format's) and `libraryVersion` (the base's own; an overlay
  that rewrote it would claim to be a version it is not).
* **Structural edits** **(decided 2026-11-01)**: zones and layers an overlay
  **adds** take ids from a **reserved range, 1,000,000 up**, which a program
  never hands out (adding a zone that would need one is refused, and a file's
  `nextId` may not enter it), so a newer base that adds zones never collides
  with an overlay's. The overlay keeps its own `nextId` (never reused, as a
  program's). Removing a **base zone** is a stored **hide**: the zone stays in
  the base, the overlay's program does not have it, undo brings it back where
  it was, and flatten drops it. A flattened program keeps its overlay's ids
  (the round-robin choice hashes the zone id, so renumbering would change what
  it plays); a base that carries one anyway has the colliding add refused and
  reported.
* **Hiding a layer** **(the author, 2026-11-01, on #24)**: `hide-layer`, the
  same mechanics as hiding a zone: one layer of a base zone is left out of
  what plays (its zone stays), undo puts it back where it was, flatten drops
  it, and a base without the layer or its zone orphans it.
* **Rebase** **(the author, 2026-11-01, on #24)**: after a drift or version
  report the user can accept the new base. The overlay's `baseVersion` becomes
  the new base's `libraryVersion` and every edit that drifted gets as its
  previous value what the new base has under it (earlier edits applied, in
  order), so both reports clear; what plays does not change. Orphans stay
  orphans, and an edit that applied without drift is left as written. A
  rebase is undoable like an edit (refused once the overlay has changed
  since).
* **Flatten keeps the overlay's ids** **(the author, 2026-11-01, on #24:
  accepted as built)**.

This is what Kontakt, Logic's Sampler and SFZ do implicitly (edits live in the
instance; the library is untouched; "save as" writes a new file that refers to
the old samples), made explicit and minimal.

## 9. In Smaragd: the native instrument

The qbx side is specified in a qbx proposal (`plan/proposed/`); this section
records the shape that proposal starts from.

### 9.1 The engine seam

qbx's plugin ABI (`twPlugin`: prepare, process, reset, parameters, state
blobs) cannot carry a program or sample buffers, and the registry creates a
native instrument from its id with no handle to anything. Pushing audio through
the state blob would put megabytes into the project file and copy them on every
edit. So:

* The instrument offers an **extension interface**, queried by capability:
  `twSamplerHost`, with `setProgram(shared_ptr<const Program>)`,
  `setSample(sampleId, shared_ptr<const Sample>)` and
  `setRoutedOutputs(mask)` (§3.1). Snapshots are swapped under the slot
  processor's mutex and read lock-free in `process()`.
* Its aux outputs are ordinary extra output buses, reported as every plugin's
  are, so the return-track work in qbx serves every multi-output plugin.
* The slot's saved state holds only a reference to the program, which lives in
  the model (§9.2).

### 9.2 Where the code lives

* The core is built inside qbx by a new engine module **`tw/sampler`**
  (depending on core and events), which owns the `twPlugin` subclass and is
  registered like the built-in 303 (`twNativeInstrument`). qbx has no
  sibling-repo dependency yet; this is the first.
* **One pin per repository** **(decided 2026-10-26, replacing "the core is a
  git submodule of qbx")**. The core is **not** a submodule of qbx, and
  nassau-dsp is not a submodule of this repo or of nassau-analogue. A Nassau
  repo that other Nassau repos consume is pinned **once**, as a top-level
  submodule of nassau-suite, the way nassau-plugin-sdk already is: the suite
  passes its path to every consumer (`-DNASSAU_DIGITAL_DIR`,
  `-DNASSAU_DSP_DIR`), and a consumer finds it through a locator copied
  verbatim from the providing repo's `cmake/bootstrap/`, in one order:
  explicit directory, sibling checkout (`../nassau-digital`, `../nassau-dsp`),
  then a fetch of a pinned commit. A nested pin would be a second answer to
  "which core shipped?", and nassau-suite's release script, which reads only
  its own `.gitmodules`, would not tag it. Sibling checkouts are also what an
  IDE opened on the workspace works with.
* The app side is a new slice **`objects/sampler`**, at the rank of
  `objects/midi` **(decided 2026-10-27, proposal 56 Q-6)**: it does **not**
  depend on `objects/track`, although the slot it configures lives there; the
  shell installs the slot's instance initializer as a hook. Its model objects **wrap the core's program**: there is one
  model, Qt-free inside, with undo, links and signals outside. A layer's asset
  source is an ordinary link to the asset, so renaming an asset changes
  nothing.
* **One XML grammar, owned by the core.** qbx hands the core the program's
  element subtree as text; the plugin player reads the same files with the
  same code.

### 9.3 Behaviour inside qbx

* **Deterministic**, like the 303: reset, the same notes, the same bytes.
* **Stereo out.** It declares two outputs; qbx folds them on a mono project
  and spreads them on a wider one, as for every generator.
* **Commands are `.qxa` verbs** **(decided: same syntax)**: `add-zone`,
  `set-zone`, `set-layer`, `select-zones`, `place-sampler-recording`, ...
  Every edit is an action, so it is undoable, scriptable, and tested in the
  headless runner. The command field takes the XML itself, with no shorthand
  **(decided)**; it is an editor pane with completion of verbs and attributes,
  not a one-line field.
* **Addressing** **(decided)**: a verb names the instrument the way the plugin
  verbs already do, by `trackPath` and `slotIndex` (the attribute
  `set-plugin-param` uses; **decided 2026-10-27**, proposal 56 Q-5), and a
  zone or layer by its stable id:

  ```xml
  <set-zone trackPath="0" slotIndex="0" zone="17" tune="+2"/>
  <set-zone trackPath="0" slotIndex="0" zone="all" attack="0.01"/>
  ```

  A program name is not unique (two instances can load the same program) and
  can change, so it is not an address. Inside the instrument's own editor the
  `trackPath`/`slotIndex` pair is implied and may be left out.
* **Drag and drop**: assets and files come in with qbx's existing
  `application/x-smaragd-resource` payload (`asset:`, `file:`, `media:`).
* **UI** is Qt, following Smaragd's look; the Nassau plugin style
  (nassau-plugin-sdk `docs/UI_STYLE.md`) governs the later plugin player.

### 9.4 Playing live

qbx has a dedicated live path, the **live lane**, and every instrument played
from a keyboard goes through it: nassau-analogue does today, with a USB MIDI
keyboard on Windows 11, at a latency the author found low. The sampler takes
the same path, so it gets the same latency and the same conditions:

* the track is **armed or monitoring**;
* the **audio device runs at the project's rate**: the live lane refuses
  otherwise, for every instrument, until that limitation in qbx is lifted;
* the live lane works in the **device's own block size**. Only when a backend
  reports none does it fall back to 1024 frames, the case the review's
  60–80 ms estimate assumed (REVIEW.md, finding 1).

What the sampler adds is its own cost per block, which must stay small enough
not to raise that latency. A gate measures the onset of a live note against
nassau-analogue's on the same device.

## 10. Later: the plugin player

On nassau-plugin-sdk, VST3/CLAP/AU, the same core. It plays an **exported
program**: the program file plus its audio, every live asset link baked to
audio at export. It may be read-only at first. It reads the program with the
core's own reader (§9.2). Tempo modes there need the vocoder extracted from
qbx (§11).

## 11. Shared code

| What | Today | Moves to |
|---|---|---|
| 24 dB ladder, 12 dB SVF (low-pass), HP cascade, `shapeTriodeK`, with `synth_dsp.h`, `nassau_real` and the 32-sample control block | nassau-analogue `Source/DSP` | **`nassau-dsp`** (decided), a header-only repo: the core must not need the SDK, which would pull iPlug2 and the VST3 SDK into qbx for a few headers. Consumed by locator, pinned once in nassau-suite (§9.2) |
| phase vocoder | qbx `tw303a/sources` | stays in qbx for the native instrument; extracted only when the plugin player needs tempo modes |
| program format reader and writer | new | this repo's core (no Qt) |

## 12. Program format

XML in the style of `.qxa` scripts, an element per object with attributes, so
the file, the command line and test scripts read alike. Unknown attributes are
kept on load and written back. Proposal:

```xml
<NassauDigitalProgram version="1" name="Kit 1" polyphony="32" busDrive="0.2" nextId="3">
  <zone id="1" keyLo="36" keyHi="36" velLo="0" velHi="127" rootKey="36"
        mode="oneshot" choke="0" output="aux1" drive="0.4" driveCurve="tube"
        filter="lp24" cutoff="8000">
    <layer id="2" src="audio/kick.wav" velLo="0" velHi="127" velFade="0"
           rrGroup="0" tune="0"/>
  </zone>
</NassauDigitalProgram>
```

An overlay (§8) is a list of the same verbs, applied to the base in order.
Each changed parameter is written with the value it replaces, as one paired
attribute `<name>.was` **(decided 2026-10-27)**; each verb names one zone or
layer, since ALL is expanded when the edit is made:

```xml
<NassauDigitalOverlay version="1" base="Library/Strings.ndp" baseVersion="3">
  <set-zone zone="17" attack="0.25" attack.was="0.1"/>
  <set-layer layer="18" tune="-1" tune.was="0"/>
</NassauDigitalOverlay>
```

The verbs of 2026-11-01 (§8) are `set-program` (no address, `attr`/`attr.was`
pairs), `add-zone` and `add-layer` (every attribute written, as in a program
file; a zone the overlay adds is one `add-zone` followed by one `add-layer`
per layer), `hide-zone` and `hide-layer` (`<hide-layer layer="ID"/>`); the overlay's own `nextId` is a root attribute,
written once it has added something:

```xml
<NassauDigitalOverlay version="1" base="Library/Strings.ndp" baseVersion="3" nextId="1000003">
  <set-program outputLevel="-6" outputLevel.was="0"/>
  <add-zone zone="1000000" keyLo="42" keyHi="42" rootKey="42" ... />
  <add-layer layer="1000001" zone="1000000" src="audio/d.wav" ... />
  <add-layer layer="1000002" zone="9" src="audio/e.wav" ... />
  <hide-zone zone="5"/>
</NassauDigitalOverlay>
```

In Smaragd the program is stored in the project; export writes a program file
plus an `audio/` folder.

## 13. Open questions

For the author. Answered inline on 2026-10-26 (the answers are the author's
own words); each is now applied in the section named after it. What is still
open is collected under **Still open** below.

1. → §5. **Tempo modes in v1** (challenged by the review): stretch with the vocoder
   from the start as decided, or a first version that resamples to the tempo
   (the pitch moves) with the vocoder after it?
   Timo: Please both from the getgo.
2. → §6. **Order of sources** (challenged by the review): files and recordings
   first and the live asset link as its own later gate, or the link from the
   start?
   Timo: Assets from the start, please.
3. → §3.1. **Outputs**: how many aux pairs (the S3000's eight individual outputs
   would be four stereo pairs, or eight mono outputs)? And are return tracks
   plus sidechain inputs a qbx proposal of their own, ahead of the sampler, or
   part of the sampler's (§3.1)?
   Timo: 8 outputs is fine.
4. → §4.4. **Modulation routes**: is §4.4's fixed set right, and does one LFO suffice?
   Timo: Undecided. Let's start with one LFO.
5. → §2. **Velocity layers**: hard switches only, or crossfaded windows?
   Timo: Crossfaded
6. → §2, §2.2. **Round-robin**: in or out? If out, it goes on §1's list, because it is the
   one feature that forces per-layer state into the voice.
   Timo: In, please. Keep per-layer state in voice.
7. → §9.3. **Command shorthand**: type the XML, or a one-line form that maps to it
   mechanically (`set-zone tune=+2`)?
   Timo: XML would be fine. This cries for IDE or dedicated editor anyway.
8. → §9.3 (a proposal). **Addressing**: does a verb name the instrument by track and slot, like
   `set-plugin-param`, or by a program name?
   Timo: Undecided. Please suggest.
9. → §4.5. **Polyphony**: a default (32?) and a maximum.
   Timo: 8 to 256 in 2 by the power of n steps.

**Settled on 2026-10-26**, the questions this list left open:

* **Return tracks and sidechain inputs come first**, as a qbx proposal of their
  own, built before the sampler's multi-output gate (§3.1).
* **Round-robin by position** (§2.2): tried first; a strict cycle only if it
  does not satisfy in use.
* **Addressing** by `trackPath` + `slotIndex` + stable zone id (§9.3), agreed
  (the attribute's name settled on 2026-10-27: `slotIndex`, as the plugin
  verbs write it).
* **Polyphony** defaults to 32 voices (§4.5), agreed.
* **Modulation** (§4.4): one LFO and no further routes foreseen; the fixed set
  listed there is the whole of it.

To measure in the gates: the interpolator, the oversampling factor, and the
CPU budget per voice, which should be named first.

**Settled on 2026-11-01** (the author, in an interview; each is applied in the
gate named, by the change that builds it — until then the code still does what
GATES.md records as built):

* **C5 drive voicing.** `tube` becomes **asymmetric** (even harmonics), with a
  DC blocker on driven voices; it replaces the symmetric triode, which is not
  kept as a curve of its own. soft, hard and fold, the +20 dB input gain, the
  level compensation and the dry→wet fade over drive 0 … 0.05 stand.
  Bit and rate reduction stays a candidate.
* **C5 oversampling is a zone setting**: a new append-only zone attribute
  choosing 2× or 4×, **default 4×**. Undriven voices are still not oversampled.
* **C5 what follows a sounding voice**: **drive, cutoff and resonance** follow
  (smoothed per control block); curve, filter type and mix stay as at note-on.
* **C5.3 is defined with lp24 at 20 kHz in the path.** With the filter `off`,
  aliasing at 44.1 kHz is the user's choice (measured: tube −73.0, fold −76.0
  dB at 4×); it is reported, not bounded.
* **C5 per-voice budget**: 40 ns (one layer) and 140 ns (four layers) bound the
  **default voice** (filter `off`, undriven). Filtered and driven voices are
  measured and reported, not bounded.
* **C5 kit-bus curve**: fixed **soft**, no attribute.
* **C5 kit-bus latency** (47 frames while a bus drive is on): as built, and
  revisited when qbx gets plugin delay compensation.
* **C6.3 per-output bus drive**: flat program attributes **`busDrive1` …
  `busDrive8`** beside `busDrive` (main); a missing one is 0 (that output's bus
  off).
* **C6 per-output level**: **`outputLevel1` … `outputLevel8`** beside
  `outputLevel` (main); a missing one is 0 dB.
* **C6 a routing change under a sounding voice**: the voice **keeps the route
  it started on** until it ends; only new notes follow the change (replaces the
  256-frame crossfade). An unrouted aux may then keep sounding into a buffer the
  host no longer reads; that is accepted.
* **C6 accepted as built**: an output the host hands no buffer for counts as
  unrouted; changing a zone's `output` while it sounds affects only new notes.
* **C7 `beats` spans the loop** `[loopStart, loopEnd)`, not the played length;
  a lead-in plays at the same rate.
* **C7 tempo modes ignore the key**: a tempo-synced layer plays at the tempo's
  rate whichever key triggers it.
* **C7 accepted as built**: a sounding tempo-oneshot follows a tempo change;
  the host passes the tempo with `setTempo()` per block.
* **C8 library versions**: a new append-only program attribute
  **`libraryVersion`**; applying an overlay to a base of another version still
  applies and reports the mismatch.
* **C8 zone lists** (`zone="3 5 7"`) are **expanded into one edit per zone**
  when the edit is made, like `all`.
* **C8 structural edits in an overlay**: zones and layers an overlay adds take
  ids from a **reserved high range** a base never uses; removing a base zone is
  a stored "hide" edit.
* **C8 orphans**: as built (reported per edit; flatten proceeds and drops them).
* **C8 `set-program`**: program-level attributes are edited in an overlay by a
  `set-program` verb with `attr`/`attr.was` pairs, like `set-zone`.

* **C8, the author's answers on #24**: flatten keeps the overlay's ids
  (accepted as built); add `hide-layer` (as `hide-zone`, for one layer); add a
  **rebase** that accepts a new base after the drift report, re-stamps
  `baseVersion`, refreshes the stored `.was` so the reports clear, is
  undoable, and leaves orphans orphaned.

The C8 items above are built (§8, §12; GATES.md C8, 2026-11-01).

**Settled on 2026-11-02** (the author, answering QBX-268's question; applied
in §5 and built with it):

* **Positions in a stretched buffer: option A.** A host-supplied sample
  carries a **time scale**, the stretched length over the source length,
  default 1 (`Sample::timeScale()`). For a `stretch` zone in a tempo mode the
  core multiplies `start`, `end`, `loopStart`, `loopEnd` and the `velToStart`
  skip by it at note-on (rounded to the nearest frame); every other zone
  ignores it. The program keeps source frames, so a host never rewrites a
  program attribute. (Option B, the host publishing rescaled positions, was
  not taken.)
* tune, master tune and LFO pitch still apply to the read rate in tempo
  modes, accepted as is for now (a zone with tune ≠ 0 drifts off tempo).

**Settled on 2026-11-02** (the author, answering qbx QBX-272's questions on
§7; applied in §7 and built with qbx's Q6):

* **The view, the selected pad and its bank are instance state of the host,
  not of the program.** In Smaragd they are model state of the instance's
  program object, saved in qbx's own element beside the program and **never**
  in the program text, so an exported program never carries them.
* **They are set by a verb from Q6 on**, not only by the editor:
  `set-sampler-view` (`view`, `padBank`, `pad`) is a real, undoable verb,
  brought forward from Q7; the editor (Q7) drives the same verb.
* **The last key played** is the last note seen by the instance's **device
  MIDI input tap**, the same tap Q7.2's hand selection uses, not the engine's
  event feed. It is live state and is **not saved**; a headless case sets it by
  injecting MIDI into that input.
* **The record input** (Q-1's `audio:<device>:<mask>`) sits beside the view
  state on the host's instance object, **not** in the program. A headless case
  feeds it through qbx's existing file input device.

**Settled on 2026-11-03** (the author, accepting the readings qbx QBX-272
built for points §7 does not state; applied in §7):

* **Map and Zone views land a take as the Key view does.**
* **The pad's zone** is the first one-key zone on the pad's key; a new take
  replaces its layer 1.
* **The whole-keyboard zone** (Key view, empty program) is rooted on key 60.
* **A Key-view take onto a key that already has a one-key zone** adds a second
  zone there rather than replacing it.

Still open, not yet asked: what a zone's `pad` attribute means, and how a pad
reassigned to another key is stored.

