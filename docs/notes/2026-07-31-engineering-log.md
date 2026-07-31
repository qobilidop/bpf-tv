# Engineering log — 2026-07-31

Working observations that aren't design decisions (DECISIONS.md),
plan state (PLAN.md), or report numbers (docs/eval/). Newest at top.

## Performance: measured (earlier hypothesis below was WRONG)

Measured with per-outcome wall sums from the sweep jsonl: the sweep
budget is NOT per-process overhead. Individual fast functions cost
0.01-0.3s wall total (parse+codegen+lift ~0.02s even on the largest
files; logging ~0.03-0.07MB — both negligible). The real budget of a
~87 worker-minute sweep:

- failed-to-prove: 65.6 min (315 fns x 12.5s avg — they run to the
  10s SMT timeout) = 75% of everything
- stack-escape downgrades: 10.2 min (run to unsound, then rejected)
- ALL 1385 verified functions: 8.6 min total

Fixes landed (scripts/corpus_run.py, m4-sweep.sh):
1. workers default 8 → cores-4 (=12 here): fresh sweep 13 min → 8:19.
2. record-reuse cache keyed on (bpf-tv sha, .ll sha, fn, smt-to,
   flags), `--reuse baseline.jsonl` / `REUSE=1 m4-sweep.sh`: same-
   binary re-sweep 8:19 → 43s.
3. compile stamps (clang sha + .c sha + flags): warm incremental
   sweep → **14s**, byte-identical outcome table.

Not done (and why): reusing failed-to-prove records across BINARY
changes would cut fresh iteration sweeps to ~3 min but hides exactly
the improvements one iterates for — bucket-targeted reruns (the
pattern used all day) cover that case honestly. Cutting smt-to loses
real verified functions (43 verified take >3s, max 23s). Compile
failures (286 files) are deliberately not stamped: the stamp key
can't see header changes, and fast-fail recompiles cost ~10s/sweep.

Determinism note: back-to-back identical sweeps differ by ±1 verified
(solver-timeout jitter at the 10s boundary).

The original (wrong) hypothesis for the record: "~1.4s/fn average ⇒
~97% per-process overhead". The 1.4s average was mean-dragged by the
ftp tail; the median function costs ~0.04s and overhead is a rounding
error. Lesson: sum the budget by bucket before optimizing.

## Open threads

- **cpumask_failure `test_global_mask_no_null_check`** (the 1
  remaining selftest INCORRECT): llvm-reduce running with a
  10s-SMT interestingness test. Hand minimizations (kptr-xchg-shaped
  slot global + reload + pass-to-callee) verify, so the trigger is
  something in the fuller shape (bpf_cpumask_create null-check
  diamond, @err stores, two kfunc acquire/release chains?).
  Counterexample has a poison ctx arg and "Mismatch in memory" after
  bpf_rcu_read_lock.
- **ftp bucket @60s SMT**: measured. Of 315: only 48 verify (15%),
  249 stay failed-to-prove, 16 timeout, 2 stack-escape. The bucket is
  model-hard (quantified memory over helper-written blocks), NOT
  solver budget — keep smt-to=10s default; a 60s record sweep would
  buy +1.1pp coverage for ~6x ftp cost. The 48 are dominated by
  split-stack map-lookup shapes.
- **Downgraded stack-escape residual (80 fns)**: dominated by opaque
  callees *writing* through escaped stack pointers
  (bpf_get_func_arg/dynptr/iters). Next model idea if worth chasing:
  the escaped-block write-back matching, not lifter-side.

## Measurement hygiene lessons (cost us twice today)

- Committed report numbers must be copied from the generated
  corpus-report.md, not re-derived or remembered: "334
  helpers-by-number" (actual 304) and "604 maps" (actual 491) both
  over-counted via grep-on-jsonl matching file *paths*.
- Rebuilding bpf-tv while a corpus rerun is in flight taints the
  measurement (workers spawn the binary per function) — stop reruns
  before rebuilding. Happened with the first ftp rerun; restarted.

## Misc facts worth not rediscovering

- LLVM's asm printer emits `sym:` then `.Lsym$local:` for dso_local
  definitions; data accumulates under the alias label (streamer).
  `demangle` now maps it back.
- `addPassesToEmitFile` unconditionally appends
  FreeMachineFunctionPass; reading MachineFrameInfo after codegen
  requires replicating the pipeline without it (generateAsm).
- The BPF backend lowers fastcc == C (BPFISelLowering falls through);
  `-O2` promotes internal functions to fastcc.
- tryReplaceRoundTrip's single-use guard leaves escaping call args as
  `inttoptr(add(ptrtoint(gep stack, 1024), -K))` whenever r10 is
  ptrtoint'd once and added twice — the per-object rewrite handles
  that shape directly.
- bpf_conformance's 2 remaining failures need bpf2bpf local calls
  (src_reg==1, raw-byte check in runBytes); `callx` is skipped by the
  runner ("unsupported instructions").
