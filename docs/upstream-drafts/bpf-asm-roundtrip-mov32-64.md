# [DRAFT — do not file] BPF asm printer emits `rN = wM` that the BPF asm parser rejects

- **Status**: draft
- **Target**: llvm/llvm-project issue, label `backend:BPF`
- **Confidence**: medium-high — minimized, reproducible, but not yet
  checked against LLVM main HEAD or for an existing duplicate report
- **Found by**: bpf-tv corpus run over `llvm/test/CodeGen/BPF`
  (2026-07-30); 21 of 429 functions failed asm reparse, all this shape

## Summary

The BPF asm printer renders `MOV_32_64` (zero-extending move of a w
register into an r register) as `rN = wM`. The BPF MC asm parser rejects
that spelling, so LLVM's own `-S` output for the BPF target is not always
reassemblable by LLVM.

## Reproduce

Observed at llvm-project `f0ad8ee185bc` (main, 2026-05-13), bpfel:

```
$ printf 'w0 = w1\n' | llvm-mc -triple=bpfel -show-encoding
        w0 = w1        # encoding: [0xbc,0x10,0x00,0x00,0x00,0x00,0x00,0x00]
$ printf 'r0 = w0\n' | llvm-mc -triple=bpfel -show-encoding
<stdin>:1:6: error: invalid operand for instruction
```

End-to-end: any function whose codegen keeps an explicit zext of a
32-bit value, e.g.

```llvm
declare i32 @get()
define i64 @f() {
  %v = call i32 @get()
  %z = zext i32 %v to i64
  ret i64 %z
}
```

`llc -mtriple=bpfel -mcpu=v3` emits `r0 = w0`, which `llvm-mc` cannot
reparse.

## Analysis

`MOV_32_64` (BPFInstrInfo.td) has asm string `"$dst = $src"` with
`$dst : GPR`, `$src : GPR32`. The parser's operand matching appears to
require same-width register pairs for register moves, so the mixed
spelling never matches. Encoding-wise the instruction is identical to the
32-bit move (`BPF_ALU | BPF_MOV | BPF_X`, 0xbc), which zero-extends per
the ISA — so either teaching the parser the mixed spelling or printing
`wN = wM` would restore the round trip.

## Before filing — confidence checklist

- [ ] Reproduce on current LLVM main HEAD (we tested the 2026-05-13 pin)
- [ ] Search existing llvm-project issues/PRs for prior reports
- [ ] Check GAS behavior for comparison (does binutils accept `r0 = w0`?)
- [ ] Decide suggested fix direction (parser accepts vs printer changes —
      printer change would alter every CHECK line downstream; parser
      accept is likely the palatable one)
