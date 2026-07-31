# M2: historical-miscompile reproduction (2019 zext/COPY-physreg bug)

**Claim demonstrated:** bpf-tv detects a real, historical BPF backend
wrong-code bug from the pass with the worst miscompile record
(BPFMIPeephole's zext elimination).

## The bug

Fixed by llvm commit `a0841dfe8594` (2019-11-20, Yonghong Song). The zext
elimination treated a `COPY` from a *physical* register as a safe 32-bit
def. A physical w register at a copy like `%0:gpr32 = COPY $w0` is
typically a function-call return value (or incoming argument) whose upper
32 bits are unknown — so eliminating the subsequent zext leaks the
callee's upper 32 bits into a value the source program zero-extended.

## Reproduction procedure (repeatable)

1. In `third_party/llvm-project`, invert the `Reg.isVirtual()` guard in
   `BPFMIPeephole::isCopyFrom32Def` (see DECISIONS.md "M2 reproduction").
2. `ninja -C build/llvm && ninja -C build/alive2 bpf-tv` (incremental,
   seconds).
3. Run bpf-tv on:

```llvm
declare i32 @get()

define i64 @f() {
  %v = call i32 @get()
  %z = zext i32 %v to i64
  ret i64 %z
}
```

4. Restore the tree (`git checkout -- .` in the submodule), rebuild,
   re-run.

## Results

| compiler | emitted code | bpf-tv (`--optimize-tgt=O0`) | bpf-tv (default `-O3`) |
|---|---|---|---|
| bug reintroduced | `call get; exit` (zext eliminated) | **1 incorrect** ✓ | 1 correct ✗ → **1 incorrect** ✓ after the junk-model fix |
| pristine (fix present) | `call get; w0 = w0; exit` | 1 correct | 1 correct |

*Update, same day: finding (1) below was mitigated — unspecified bits are
now modeled as loads from an external constant global, which -O3 cannot
refine. The planted bug is caught at the default -O3 with corpus numbers
and timing unchanged. See DECISIONS.md.*

The counterexample direction is exactly the historical failure: the
callee's r0 upper 32 bits (modeled as unspecified) reach the return value,
while the source requires them to be zero.

## Two collateral findings

1. **`-O3` on the lifted code creates a false-negative window.** The
   lifter models unspecified upper bits as `freeze poison`; the -O3
   compaction pass may legally *refine* that nondeterminism (it chose 0),
   after which the buggy and correct code become indistinguishable.
   Optimizing the target side can only shrink its behavior set, so it can
   convert non-refinement into refinement. This affects the arm-tv
   reference too (same `enforceSExtZExt` + same default `-O3`). See
   DECISIONS.md for bpf-tv's chosen default.
2. **LLVM BPF asm printer/parser round-trip hole:** the printer emits
   `MOV_32_64` as `rN = wM`, which LLVM's own asm parser rejects
   (minimized with `llvm-mc`). bpf-tv works around it with a
   semantics-identical rewrite (see DECISIONS.md); upstream report
   planned.

Both findings are candidates for upstream engagement (LLVM BPF
maintainers; Regehr's group for the -O3 window).
