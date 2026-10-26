# NassauDigital — working on it

The specification is [`docs/DESIGN.md`](docs/DESIGN.md), the schedule is
[`docs/GATES.md`](docs/GATES.md), and [`docs/REVIEW.md`](docs/REVIEW.md) is the
review the design was revised against. Read all three before the first gate.
Where this file and DESIGN.md disagree, DESIGN.md wins.

## Knowledge

The why behind the design (the author's interview, decisions, gotchas) is in
thinktank, notebook **qbx2**, component **`nassau-digital`**: ask
`cru_briefing` / `thinktank_hybrid_search` before reading around, and write
findings back with `cru_log(component="nassau-digital", files=[...])`, a
standing rule with `conventions=[...]`. Not into this file.

## The workspace

Every repository is a sibling checkout in one directory, because the Nassau
build scripts find each other that way (`../nassau-plugin-sdk` and so on):

```
<workspace>/
  nassau-digital/      this repo (track C)
  nassau-dsp/          shared DSP headers (gate X1)
  nassau-analogue/     where the filters come from (gate X1)
  nassau-plugin-sdk/   needed to build nassau-analogue
  qbx/                 Smaragd (tracks Q and X2); read its own CLAUDE.md
```

Clone with plain `https://github.com/tweggen/<repo>.git` URLs and let `gh`
authenticate. **Never put a token into a remote URL** and never print one.

## Branches and worktrees

* **A PR is the only route to `main`.** Open it and stop there; the author
  merges. Never merge, never push to `main`.
* **One PR per gate**, branch `feat/<gate>-<slug>` (e.g. `feat/C1-program-model`),
  `docs/` and `fix/` as usual. A gate's PR also marks the gate DONE in
  GATES.md with the measured numbers (R10).
* **One worktree per branch.** For the Nassau repos, put the worktree beside
  the checkouts, `<workspace>/<repo>-<gate>`, so `../nassau-plugin-sdk` still
  resolves from inside it (or pass `-DNASSAU_SDK_DIR=`). For qbx, follow qbx's
  CLAUDE.md: `.claude/worktrees/<slug>`, started from the main checkout.
* **Stacked PRs** are fine when a gate needs an unmerged one: base the branch
  on the earlier gate's branch and say so in the PR. When the base PR merges,
  **retarget the stacked PR to `main`** at once; a PR merged into a branch
  that already merged lands nowhere.
* **Cross-repo order**: a PR that needs another repo's unmerged PR waits as a
  draft and says which one.

## Gates and checks

* `./ci/gates.sh` before every PR (R1), from C0 on. In qbx, qbx's own
  `./ci/gates.sh` plus the gate's ACs.
* **Watch a new test fail once** before trusting it (break the code, see the
  AC fail, restore). A gate nobody has watched fail is not evidence.
* Plugin builds (nassau-analogue in X1) use `-DIPLUG_DEPLOY_PLUGINS=OFF`, so a
  build never replaces the plugins installed on the machine.
* On Windows a failed `assert` in a Debug build opens a modal dialog; a
  headless run hangs on it. Prefer the tests' soft checks.
* **Hand checks** (Q5.2, Q7.4, and any AC that says "by the author") are not
  yours: when you reach one, stop and ask the author, with what to look at and
  how.
* **Report honestly** (R11): a missed AC is reported with its number, never
  relaxed.

## qbx

* Read qbx's `CLAUDE.md`, `docs/ARCHITECTURE.md` and the `CONTRACT.md` beside
  every module you touch.
* qbx work is planned in a proposal first (`plan/proposed/NN_*.md`, the house
  format of the proposals there), merged as a docs PR, then built gate by gate.
* qbx work carries a YouTrack ticket, project QBX (https://nassau.youtrack.cloud),
  filed with `cru_file`; the key goes into the branch name and the PR.
