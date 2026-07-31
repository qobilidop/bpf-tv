# [DRAFT — do not post] llvm/llvm-project#208984 review: likely invalid (or at minimum, not a refinement violation)

- **Status**: draft
- **Target**: possibly a comment on llvm/llvm-project#208984; also an
  internal correction to our own motivation narrative
- **Confidence**: medium-high on the two technical points below; the
  offset-arithmetic claim should be re-derived independently before
  posting anything
- **Context**: we attempted to reproduce #208984 live (it was cited in
  the 2026-07-30 evaluation as the flagship "verifier-accepted, silently
  wrong" example). Three observations came out of it (2026-07-30, LLVM
  pin `f0ad8ee185bc` 2026-05-13).

## 1. The affected paths are source-UB

Every branch that allegedly mislands targets a block terminated by
`unreachable` (the C reproducer's `__builtin_unreachable()` after the
inlined `int_on()` returns). Branching to `unreachable` means that path
may do anything per IR semantics. In both the reporter's disassembly and
ours, the *defined* executions (the `raise_fn` paths) compile correctly.
Consequence for bpf-tv: a refinement checker rightly accepts this code —
if there is a defect here it is a verifier-interaction/QoI matter (a
branch past the function end would make the kernel verifier reject the
program — loud, not silent), which per DESIGN.md belongs to the separate
verifier-acceptability tool, not semantic TV.

## 2. At our pin the "empty block" is not empty

`TrapUnreachable` (default-on in `BPFTargetMachine` unless
`-bpf-disable-trap-unreachable`) lowers `unreachable` to
`call __bpf_trap; exit`. Both the reporter's 12-line `minbug.ll` and our
reconstruction of the C reproducer produce a non-empty final block at
LLVM `f0ad8ee185bc`, with all branch offsets resolving to its first real
instruction. The truly-empty shape (a tail-duplicated block containing
only empty inline asm and no terminator) did not materialize in our
attempts. Older LLVMs without `__bpf_trap` (e.g. Debian 19.1.7) may
genuinely have had empty blocks — the bug may be real-but-fixed there.

## 3. The reporter's offset arithmetic appears off by one

Their disassembly: `0x30: jne32 %r2,0,5` claimed to land at `0x58`.
BPF branch semantics: `target = pc + 8 + off*8 = 0x30 + 8 + 0x28 =
0x60`, which in their own layout is the block they say should be
targeted (`%11`'s code, plausibly a `__bpf_trap` call). Same for
`0x50: jeq32 %r1,0,1` → `0x60`, not `0x58`. If this re-derivation is
right, the "WRONG target" annotations are misreadings and the emitted
code is correct on trunk.

## Implication for bpf-tv's own narrative

The evaluation doc cited #208984 as the live example of
"verifier-accepted but silently wrong". That citation should be treated
as withdrawn until this analysis is refuted: point 1 alone removes it
from the refinement-violation category regardless of points 2–3.
The zext-elimination cluster (M2's 2019 bug, #110618, #208244) remains
the well-grounded motivating record.

## Before doing anything external

- [x] Independently re-derive the offset arithmetic — DONE (2026-07-30),
      mechanically: the issue's exact instruction bytes fed through
      LLVM's BPF MCDisassembler + our CFG builder resolve the flagged
      branches to unit 12 (0x60, the `%11` block) — not 0x58 as
      annotated. The reporter's *first* branch (`+10` → 0x58) follows
      the correct `pc + 8 + off*8` rule; the flagged ones appear to
      miscount the two-unit `lddw` at 0x38. LLVM's own decoder
      contradicts the report's central claim.
      Confidence now: **high** on all three points.
- [ ] Test with an actual Debian LLVM 19.1.7 to confirm the
      real-but-fixed hypothesis for old versions
- [ ] Try harder to synthesize the tail-dup barrier-only-no-terminator
      MIR shape at head (e.g. via `llc -run-pass` on hand-written MIR)
      to check whether *that* variant still miscompiles
