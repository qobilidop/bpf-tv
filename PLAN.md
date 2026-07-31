# bpf-tv working plan

Working document for current execution. Design and rationale live in
[DESIGN.md](DESIGN.md); this file tracks what's being built right now and in
what order. Update as milestones complete.

## Done

- Repo scaffolding: pinned submodules (official alive2 @ arm-tv merge-base,
  llvm-project date-matched, arm-tv as never-built reference), devcontainer
  (pinned Z3 4.15.4) + GHCR publish workflow, `dev.sh` (devcontainer CLI).
- Lifter plumbing vendored from arm-tv (`src/lifter/`, de-ASLP'd, attributed).
- `bpf2llvm` v0: ALU32/64, RFC 9669 edge semantics, movs/movsx, ld_imm64,
  endian ops, all load/store widths, cond jumps + jset, calls, exit.
- Driver + first public use of alive2's `EXTERNAL_PROJECTS` CMake hook.
- e2e suite (`tests/e2e/run.sh`): 9 positive cases + negative control
  (injected miscompile must be reported incorrect). Green natively.
- Solver finding: undef-input quantification diverges Z3 on commuted adds;
  bpf-tv disables undef inputs by default (documented in bpf-tv.cpp).

## In flight

- **M0 — container validation**: full dep build inside the devcontainer, then
  `tests/e2e/run.sh` there. Green-in-container = milestone complete; the
  container is the configuration of record.

## Evaluation milestones (prior art: see "Evaluation" discussion 2026-07-30 —
arm-tv's 4 legs, Alive2's test-suite trick, Jitterbug's retrospective table,
K2's corpus, bpf_conformance as executable spec)

- **M1 — coverage corpus runner** ✅ (2026-07-30): `scripts/corpus_run.py`
  (one process per function via `--fn`, so no driver change was needed),
  report at `docs/eval/2026-07-30-codegen-bpf-corpus.md`. First numbers:
  44.1% verified (189/429 fns, median 0.02s), 25.6% src-IR rejected by
  Alive2 itself, 6 lifter opcode gaps, 21 asm round-trip holes, 9 INCORRECT
  to triage. Follow-ups queued from triage:
  - escaping-stack-pointer false alarms (fi_ri, warn-stack, pr57872 class)
    are shared with reference riscv-tv — investigate arm-tv's ABI handling
    or accept-and-bucket as known-limitation
  - bpf-fastcall-3: model bpf_fastcall's preserved-register ABI in doCall
  - i128: should be rejected by checkTypeSupport but reaches verification
  - triage remaining: loop-exit-cond, remove_truncate_5, rodata_5,
    simplifycfg; 3 crashes; 21 asm-parse-errors; 22 "other"
- **M2 — historical-miscompile regression** ✅ (2026-07-30): reproduced the
  2019 zext/COPY-physreg bug (fix `a0841dfe8594`) by guard-inversion in the
  pinned tree; bpf-tv reports INCORRECT (at `--optimize-tgt=O0`), pristine
  compiler verifies. Report: `docs/eval/2026-07-30-m2-historical-miscompile.md`.
  Collateral findings: (1) -O3-on-lifted-code false-negative window (freeze
  refinement; affects arm-tv reference too) — default stays O3, see
  DECISIONS.md; (2) LLVM BPF asm printer/parser round-trip hole (`rN = wM`)
  worked around by rewrite, corpus asm-parse-errors 21 -> 0, verified
  189 -> 195. Queued: sound unfoldable junk modeling; more "would have
  caught" entries toward the Jitterbug-style table. Upstream reports are
  DRAFTS ONLY in docs/upstream-drafts/ (policy: Bili reviews and signs off
  before anything is filed; each draft carries a confidence checklist).
- **M3 — lifter differential harness** (next session): bpf_conformance corpus
  assembled to bytecode → LLVM BPF disassembler → MCInsts → bpf2llvm →
  execute lifted IR, compare r0; uBPF/rbpf as extra oracles. Both repos
  become pinned submodules.
- **M4 — real-world coverage** (after M3): kernel selftests + Cilium `.o`s
  (K2's corpus); needs a BPF-targeting clang. Headline: inline-asm / CO-RE
  rejection rate on production code.
- **M5 — fuzzing** (later): Csmith integer subset, once the feature filter
  passes most generated programs.

## Execution notes

- Every tricky design decision gets a [DECISIONS.md](DECISIONS.md) entry
  (context / decision / why / revisit-when), in the same commit as the code.

- Iterate natively (fast), validate in container (config of record).
- Corpus runner: 10s SMT timeout, 120s wall cap per function, 8-way parallel.
- CodeGen/BPF tests embed llc RUN lines (various -mcpu, bpfeb); the runner
  ignores RUN lines and uses bpf-tv defaults (bpfel, v3) — files that don't
  fit become taxonomy buckets, which is data, not failure.
- Commit in logical units; checkpoint with Bili between milestones.
