# [DRAFT — do not file] asm-mode call-input matching cannot reconcile pointer bytes with integer bytes

- **Status**: draft
- **Target**: AliveToolkit/alive2 issue (and/or arm-tv authors — same
  audience as the O3 false-negative-window draft; the arm-tv paper's
  "zero false-alarm rate" goal is directly implicated)
- **Confidence**: high that the class is real and purely
  representational (three independent minimized reproducers, each with
  a control that flips the verdict); medium on the located mechanism —
  we characterized it behaviorally from verdicts and counterexamples,
  not by reading the relevant Alive2 code paths. Before filing:
  (1) check the alive2 issue tracker for duplicates (a 2026-07-31 web
  search found none, but that is not authoritative), (2) distill at
  least one reproducer to plain alive2/arm-tv form so the report does
  not require building bpf-tv, (3) confirm the mechanism in
  Alive2 source (escaped-block call-input byte comparison).

## The class

Machine code moves pointer values through integer registers and
byte-wise memory operations; that is what asm is. `config::tgt_is_asm`
plus physical pointers handle this for dereferences, but **call-input
matching on escaped memory appears to compare byte kinds**: a source
byte that is part of a typed pointer value does not match a target
byte that carries the same value as an integer. Every unknown call
whose escaped inputs hold pointer-laden bytes copied or loaded through
integer operations can then fail refinement — as INCORRECT when the
solver finds the counterexample, as failed-to-prove when it times out
first.

The failures are false alarms in the strictest sense we can measure:
**replacing the source's pointer-typed operation with the equivalent
integer operation (+ inttoptr) makes the identical function verify**,
so the refinement itself is provable; only the representation of the
same bytes differs.

## Reproducers (bpf-tv, LLVM pin f0ad8ee, 2026-07-31)

1. **memcpy form** (`llvm/test/CodeGen/BPF/pr57872.ll`): source
   memcpy's 84 bytes from opaque memory into a stack object passed to
   a callee; the lifted target copies the same bytes via 8-byte
   integer loads/stores. Verdict: "source is more defined than
   target" with the counterexample placing a physical pointer among
   the copied bytes. Control: the same function copying from a
   pointer-free constant global verifies.

2. **load form** (kernel selftest `cpumask_failure.c`,
   llvm-reduce-minimized to ~15 lines): `%p = load ptr, ptr %m` passed
   as a call argument, with an integer store to potentially-related
   memory on a sibling path. Control: `load i64` + `inttoptr` in place
   of `load ptr` verifies — in the minimized form AND in the full
   original function.

3. **negative control for the class**: the same load-ptr-to-call
   shape verifies when the loaded memory's contents were last written
   by a matched call or a concrete initializer (so the byte kinds
   correspond across src/tgt); the failures need bytes whose kind can
   diverge.

## Impact

- bpf-tv (BPF backend TV on alive2's asm mode): 18 kernel-selftest
  functions + 1 CodeGen/BPF test currently carry an honest
  "unsupported: pointer bytes" verdict via a static gate (memcpy
  form) and a post-verification downgrade (load form) purely to keep
  the INCORRECT column truthful. Both heuristics — and their
  documented risk of masking a real miscompile in these shapes —
  retire if call-input matching coerces byte kinds.
- arm-tv/riscv-tv: our M1 triage found the pr57872-class false alarms
  reproduce under the reference riscv-tv as well, so this is not
  BPF-specific.

## Suggested direction (phrase as a question, not a prescription)

At call-input (and presumably call-result/return) matching in asm
mode, could a target integer byte refine a source pointer byte when
the integer value equals the pointer's address bytes (the dual of
what physical pointers already permit for dereference)? The
soundness question — whether provenance can be laundered through an
unknown call this way — deserves the authors' judgment; the machine
ABI makes no distinction, so some coercion at this boundary seems
forced for any asm-level target.
