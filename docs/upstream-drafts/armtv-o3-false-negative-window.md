# [DRAFT — do not send] -O3 on lifted code can refine freeze-poison nondeterminism into a false-negative window

- **Status**: draft
- **Target**: arm-tv authors (Regehr group) — methodological note /
  discussion, not a bug report; possibly an alive2 GitHub issue later
- **Confidence**: medium — demonstrated end-to-end in bpf-tv, and the
  same code pattern exists in the arm-tv branch, but NOT yet reproduced
  against arm-tv/riscv-tv themselves with a planted bug
- **Found by**: bpf-tv M2 experiment (2026-07-30), reproducing the 2019
  BPF zext-elimination bug (llvm `a0841dfe8594`)

## Summary

backend-tv-style pipelines model unspecified machine state (upper bits of
a call's return register, junk bits of sub-64-bit arguments) as
`freeze poison`, then run `-O3` over the lifted code before the Alive2
refinement check (`optimize_module`, default O3). The optimizer may
legally *refine* `freeze poison` to a concrete value — we observed
InstCombine choosing 0 — which deletes exactly the nondeterminism the
machine model introduced.

Since optimizing the target side can only shrink its behavior set,
`tgt' ⊑ tgt` always holds, and `tgt' ⊑ src` can hold where `tgt ⊑ src`
does not: a structural false-negative window. It cannot cause false
positives, only missed miscompiles — specifically miscompiles whose
wrongness lives in the modeled-unspecified bits, which for BPF is
precisely the zext-elimination bug class (5 historical wrong-code fixes).

## Demonstration (bpf-tv, but the pattern is shared)

1. Reintroduce the 2019 bug fixed by `a0841dfe8594` (zext elimination
   trusts a COPY from a physical register): compiled `zext i32 (call ...)`
   loses its `w0 = w0` zext.
2. `bpf-tv --optimize-tgt=O0` → **1 incorrect transformations** (caught).
3. `bpf-tv` (default `-O3`, also at `-O1`) → 1 correct transformations
   (missed). The optimized lifted IR shows the junk fold:
   `or (shl (zext (freeze poison)), 32), (zext %v)` became `zext %v`,
   annotated `range(i64 0, 4294967296)`.

The relevant helper (`enforceSExtZExt`, freeze-poison junk padding) and
the `-O3` default are shared with `backend_tv/` on the arm-tv branch
(`riscv2llvm.cpp` verbatim; `arm2llvm.cpp` same helper), so arm-tv and
riscv-tv likely share the window.

## Possible mitigations (for discussion)

- Model unspecified bits with nondeterminism the optimizer must preserve
  (e.g. volatile loads from a dedicated scratch object) — closes the
  window at full compaction speed, at some memory-model cost.
- Run refinement twice (O3 for throughput, O0 on "correct" results
  sampled) — statistical, not sound.
- An Alive2/opt mode that pins freeze-poison values as opaque.

## Second finding (2026-07-30, later the same day): `tail`-marker false ALARMS from the same -O3 step

The -O3-on-lifted-code step also causes spurious *failures* (the
opposite direction from the finding above): it marks lifted calls
`tail` while call arguments' stack provenance is hidden behind
`ptrtoint/add/inttoptr`; after `run-replace-ptrtoint` collapses the
round trip into a GEP, a `tail` call with an alloca-derived argument
makes callee accesses UB, and refinement fails with "source is more
defined than target". Reproduced on the reference riscv-tv (7 corpus
functions from llvm/test/CodeGen/BPF fail identically there).
Minimized: an exact src/tgt pair that flips verdict when the single
`tail` token is removed. Fix (validated in bpf-tv): clear tail-call
kinds on all lifted calls after optimization — strictly
behavior-enlarging, so sound; 7 of our 9 false alarms became genuine
verifications. arm-tv/riscv-tv would likely see the same coverage gain.

## Third finding (2026-07-30, code study): call-input pointer refinement asymmetry

`Pointer::fninputRefined` (ir/pointer.cpp) keeps an asm-mode-only
`isLogical() == other.isLogical()` conjunct that `Pointer::refined`
(return values) already dropped in the #1133/#1153 fixes. Since `-O3`
never folds `ptrtoint→add→inttoptr` chains and the branch's
`-run-replace-ptrtoint` mitigation is off by default, every SP-relative
pointer argument to a call reaches refinement as a *physical* pointer,
fails the conjunct against the source's logical alloca pointer, and
produces "source is more defined than target / function did not
return". The `toLogicalLocal` + `at_least_same_offseting` machinery the
conjunct guards already handles the big-frame-vs-small-alloca case, so
the conjunct appears to be a leftover; removing it (mirroring
#1133/#1153) is a one-line experiment with clear precedent. Related
documented gaps worth raising in the same conversation: callee writes
to escaped local blocks are unmodeled (memory.cpp TODOs, issue #969) —
a bug-missing window for helper-writes-to-stack miscompiles in all
backend-tv-family tools.

## Before sending — confidence checklist

- [ ] Reproduce against riscv-tv itself: plant an equivalent bug in the
      RISC-V backend (or an older LLVM with a known zext bug) and show
      backend-tv misses it at O3, catches at O0
- [ ] Check whether the arm-tv paper/repo already acknowledges this
      (their `disable_undef_input` comments show they thought about
      adjacent issues; search for freeze-refinement discussion)
- [x] Prototype unfoldable junk modeling in bpf-tv — DONE (2026-07-30):
      loads from an external *constant* global with no initializer
      (unknown-but-fixed contents; one distinct offset per junk site).
      Result: the planted 2019 bug is caught at default -O3; corpus
      numbers and verification times unchanged (195 verified, median
      0.02s). Two implementation constraints worth relaying: Alive2
      rejects non-constant globals introduced only in the target, and
      declarations unused by the source function are not registered by
      llvm2alive — hence constant-global, not mutable-global (cost: no
      per-loop-iteration or cross-call junk freshness; per-site
      independence preserved). Volatile loads were not needed.
