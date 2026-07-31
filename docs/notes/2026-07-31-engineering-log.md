# Engineering log — 2026-07-31

Working observations that aren't design decisions (DECISIONS.md),
plan state (PLAN.md), or report numbers (docs/eval/). Newest at top.

## Performance: sweeps are ~97% per-process overhead

Last full sweep: 4316 functions in ~13 min at 8 workers ≈ 1.4 s/fn
average wall, while the median *solver* time on verified functions is
0.03 s. So nearly all sweep time is fixed cost per process:

- full-module `.ll` parse per function (some selftest files are large
  and get re-parsed once per contained function),
- two full-module clones per run (driver CodegenM + generateAsm's
  internal clone),
- LLVM + Z3 init per process,
- the lifter's debug logging (every instruction/global lifted →
  megabytes of captured stdout per sweep).

Candidate fixes, in rough value order: (1) quiet mode for corpus runs
(route lifter `*out` to a null stream), (2) pre-split corpora into
per-function modules once (parse once, emit stripped clones), (3)
batch N functions per process — blocked on the lifter's exit(-1)
error style, which would need to become recoverable first. Process
isolation itself is worth keeping (crash/timeout containment).
llvm-reduce interestingness tests inherit all of this per probe.

## Open threads

- **cpumask_failure `test_global_mask_no_null_check`** (the 1
  remaining selftest INCORRECT): llvm-reduce running with a
  10s-SMT interestingness test. Hand minimizations (kptr-xchg-shaped
  slot global + reload + pass-to-callee) verify, so the trigger is
  something in the fuller shape (bpf_cpumask_create null-check
  diamond, @err stores, two kfunc acquire/release chains?).
  Counterexample has a poison ctx arg and "Mismatch in memory" after
  bpf_rcu_read_lock.
- **ftp bucket @60s SMT**: measurement in flight (315 fns). Early
  indication from spot checks: map-lookup stack-key shapes flip to
  verified at 60s.
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
