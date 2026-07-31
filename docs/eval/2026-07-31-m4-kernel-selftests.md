# M4: kernel selftests coverage (first measurement)

Corpus: `tools/testing/selftests/bpf/progs` at linux `8ba098e6b6ff`
(2026-07-31), 1009 C files → 690 compiled (pinned clang 23 @ `f0ad8ee`,
`-target bpf -O2`, macOS header shim) → **4316 functions** through
bpf-tv (`-cpu v3` default, 10s SMT timeout).

Run: `./scripts/m4-sweep.sh native` (container mode for the
clean-Linux-headers cross-check).

## Outcomes

| outcome | count | % | reading |
|---|---|---|---|
| unsupported:inline-asm | 1735 | 40.2% | the measured cap (see below) |
| unsupported:global-lookup | 604 | 14.0% | maps (`.maps` globals) — v0 exclusion |
| **verified** | **601** | **13.9%** | median 0.03s, p90 0.05s, max 11.8s |
| other | 571 | 13.2% | 304 helper-calls-by-number (feature landed, see below), 109 calling-conv, rest misc |
| unsupported:src-ir | 322 | 7.5% | Alive2-side rejections (atomics etc.) |
| compile-error | 286 | 6.6% | mostly macOS-shim artifacts; container run pending |
| failed-to-prove | 46 | 1.1% | |
| INCORRECT | 5 | 0.1% | after burndown (was 20); all one diagnosed class, below |
| everything else | ~130 | ~3% | varargs, byval, arg-type, ... |

## Key findings

1. **Inline asm is the #1 coverage cap on real-world code — now
   quantified at 40.2%** of selftest functions (the evaluation doc
   flagged exactly this risk as "measure early"). Composition,
   measured exactly after the barrier_var passthrough landed
   (2026-07-31): of the 1721 bucketed functions, **1653 are `__naked`**
   — entire programs as asm blobs, which are *asm inputs*, not
   compiler output, and thus legitimately out of scope for backend TV
   — **64 carry genuinely non-identity asm** (bpf_throw guards,
   `.8byte`, `may_goto`), and 4 recovered (1 verified, 2 maps-blocked,
   1 varargs). `barrier_var` itself is only 51 call sites in 26 files;
   the passthrough's main value is that those sites no longer block
   functions whose other blockers (maps, volatile) fall later.
2. **Maps are the #2 cap (14%)** — `LD_imm64` of `.maps`-section
   globals fails lifter global lookup. The K2-style map modeling from
   the design doc is the fix; until then this bucket is the honest
   price of the v0 cut.
3. **Helper calls by number (304; the earlier "334" over-counted by
   matching file paths)** — same gap as the conformance harness's
   `call 5` test. **Feature landed 2026-07-31** (per-ID uninterpreted
   `@__bpf_helper_N`, see DECISIONS.md). Re-running exactly those 304
   functions: **110 verified**, 148 move to the maps bucket
   (`unsupported:global-lookup` — they also use `.maps` globals), 22
   varargs, 17 failed-to-prove, 1 timeout, and 6 INCORRECT. All 6
   (5 × `iters_testmod.ll` `iter_next_*`, 1 ×
   `verifier_iterating_callbacks.ll` `unsafe_on_2nd_iter`) are the
   already-documented escaping-stack-object class below — the
   counterexamples show a callee-returned pointer modeled as
   stack-derived (or a stack pointer smuggled through a loop-context
   struct), not a defect in the helper modeling. Known-class INCORRECT
   total: 5 → 11.
4. **kfunc-call INCORRECT cluster: diagnosed and burned down 20 → 5.**
   Root cause is not kfuncs at all: **two distinct stack objects
   escaping to callees** cannot be matched by Alive2 against the
   lifted single-block stack (minimized: two allocas + calls fails,
   one alloca + any number of calls verifies). Now rejected honestly
   as `unsupported:stack-escape`. The same fix retired the
   long-standing `simplifycfg.ll` residual — the LLVM CodeGen corpus
   is now at **0 INCORRECT**. The 5 that remain remain reach the same
   state through a path no static check can see (a kfunc *returns* a
   stack-derived pointer that then reaches another call); documented,
   not chased.
5. **Verification cost stays trivial** (p90 = 0.05s) — solver time is
   not the bottleneck at v0 scope; feature coverage is.

## Fixes that fell out of round one

- Metadata kinds `srcloc`/`errno.tbaa`/`tbaa.struct`/`inline_history`
  are now stripped by the driver (1683 functions were blocked on
  "Unsupported metadata: 51").
- `libarena/include` added to the include path (269 compile errors).

## Container cross-check (real Linux headers)

`./scripts/m4-sweep.sh container` (devcontainer, glibc headers, no
shim): 559 files / 3451 functions, 13.0% verified, inline-asm 41.8%,
maps 11.0%. **More** compile errors than the native shim run (417 vs
286) — glibc headers do not cross-compile cleanly for `-target bpf`
without `-nostdinc`, so the "clean" environment is actually the worse
compiler front end here. The two runs agree on structure (inline-asm
~41%, verified ~13%, maps 11–14%), which is the point of the
cross-check; the native shim numbers stand as the record. A proper
container run would use the shim flags too (kernel selftests build
with `-nostdinc` in-tree as well).

## Caveats

- macOS header shim compiles 68% of files; the devcontainer run with
  real Linux headers is the number of record (pending).
- `-cpu v3`: signed-div functions land in backend-error; a v4 sweep
  would convert them.
- Single-function-at-a-time validation; bpf2bpf callees not composed.
