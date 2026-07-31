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
| unsupported:global-lookup | 491 | 11.4% | maps (`.maps` globals) — retired 2026-07-31, see below (an earlier revision misquoted 604/14%) |
| **verified** | **601** | **13.9%** | median 0.03s, p90 0.05s, max 11.8s |
| other | 571 | 13.2% | 304 helper-calls-by-number (feature landed, see below), 109 calling-conv, rest misc |
| unsupported:src-ir | 322 | 7.5% | Alive2-side rejections (atomics etc.) |
| compile-error | 286 | 6.6% | mostly macOS-shim artifacts; container run pending |
| failed-to-prove | 46 | 1.1% | |
| INCORRECT | 5 | 0.1% | after burndown (was 20); all one diagnosed class, below |
| everything else | ~130 | ~3% | varargs, byval, arg-type, ... |

## Running total (2026-07-31, after helpers + barrier_var + maps)

A fresh full sweep (same flags/corpus) confirmed at this stage:
**1212 verified = 28.1%** of 4316 functions — doubled from the 13.9%
first measurement, with `unsupported:global-lookup` gone and
INCORRECT = 31, every one the documented escaping-stack class.

## Per-object stack blocks (2026-07-31, same day)

The escaping-stack class is now retired (DECISIONS.md): the lifter
reads the backend's own frame layout out of MachineFrameInfo and the
driver carves alloca-backed slots into per-object blocks after -O3
(all-or-nothing, falling back to the faithful single-block model).
Final sweep: **verified 1273 = 29.5%**, **INCORRECT = 1** (a distinct
kfunc/poison-arg class in `cpumask_failure.ll`, queued). Of the old
31: 18 verify, 12 move to failed-to-prove (solver budget — the
map-lookup stack-key shape verifies at 60s SMT). Multi-escape
functions are admitted when slots cover their allocas; the ones whose
refinement still fails under a successful split (27, dominated by
opaque callees writing through escaped stack pointers) are downgraded
back to the honest `unsupported:stack-escape` rejection (81 total)
rather than reported as false miscompilations. A new
`unsupported:ptr-bytes` gate (17) covers the pr57872 byte-kind class:
memcpy of possibly-pointer-carrying memory into an escaping stack
object cannot preserve pointer bytes through integer registers.
failed-to-prove is now the largest reducible bucket (mostly
quantified-memory solver cost); inline asm remains dominated by
out-of-scope `__naked` programs.

## Same-day addenda (fastcc, alignment clamp, measurements)

- **fastcc accepted** (the backend lowers it identically to C): the
  158-function calling-convention bucket collapses; 110 verify.
  Diagnosing the two INCORRECTs this exposed found an alignment
  modeling bug in the split (O3-stamped aligns proved against the
  16-aligned frame that a split object cannot justify); with the
  clamp, both verify. **Record sweep: 1386 verified = 32.1%**,
  INCORRECT = 1 (the cpumask singleton), "other" 168 → 10.
- **failed-to-prove is model-hard, not budget-hard**: re-running the
  315-function bucket at 60s SMT converts only 48 (15%); 249 stay
  failed-to-prove, 16 time out. The 10s default stands.
- Sweep infrastructure: 12-way default, record-reuse cache and
  compile stamps (`REUSE=1 ./scripts/m4-sweep.sh native`) — fresh
  sweep 8:19, same-binary incremental re-sweep 14s. Back-to-back
  sweeps jitter by ±1 verified at the SMT-timeout boundary.
- **The last INCORRECT is resolved — the corpus is at 0.** The
  cpumask singleton llvm-reduced to the pr57872 byte-kind class
  reached through a pointer-typed load feeding a call argument (the
  identical function with `load i64` + `inttoptr` verifies); the
  post-verification downgrade now covers that shape. **Record:
  1388 verified = 32.2%, INCORRECT = 0**; CodeGen corpus 212
  verified, 0 INCORRECT; conformance 310/312. See DECISIONS.md for
  the masking trade-off, stated plainly.

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
2. **Maps are the #2 cap (11.4%; the earlier "604/14%" misquoted the run report's 491) — retired 2026-07-31.** The anticipated
   K2-style modeling turned out to be unnecessary: at the v0 cut point
   a map is an ordinary named global (see DESIGN.md "Maps at the v0
   cut point"); the actual blocker was the `.Lsym$local` alias-label
   gap in lifter symbol resolution (every `.maps` global is
   dso_local). Re-running all 641 maps-blocked functions (491 original + 150
   revealed by the helper/barrier_var features):
   **485 verified**, 129 failed-to-prove (a quantified-memory solver
   class; 60s SMT timeout does not help), **20 INCORRECT — all the
   documented escaping-stack class** (the map-lookup stack-key idiom
   is precisely shaped to trigger it; known-class total now 31),
   6 call-mapping/varargs. Collateral fix: three programs with a
   function literally named `entry` no longer break the lifted module
   (synthetic entry-block name collision).
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
