# M3: bpf_conformance differential results

The bpf-tv lifter run as a BPF "runtime" against the ecosystem's
executable spec ([bpf_conformance](https://github.com/Alan-Jowett/bpf_conformance),
pinned submodule): bytecode → LLVM BPF MCDisassembler → CFG
reconstruction → `bpf2llvm` lift → host ORC JIT → execute → compare r0.

```
build/bpf_conformance/bin/bpf_conformance_runner \
  --test_file_directory third_party/bpf_conformance/tests \
  --plugin_path build/alive2/bpf-tv/bpf-tv-conformance-plugin \
  --cpu_version v4
```

## Result: 309 / 312 (2026-07-30)

The 3 failures are the bpf2bpf local-call tests (`call_local`,
`call_unwind_fail`, `rfc9669_call_local`) — a documented v0 exclusion.
For comparison, production runtimes report ~all-pass on this corpus
(llvmbpf "313/313" at an earlier corpus version); uBPF/rbpf are the
other measured implementations.

## What the harness found on the way (its purpose: lifter bugs)

1. **store-immediate signed-APInt crash** — `stb [%r10-1], 0xff` hit
   an `isIntN` assertion (immediate built as signed 8-bit). Latent in
   the TV path too: backend-emitted byte stores of values ≥128 would
   have crashed bpf-tv. Fixed (truncate, don't sign-check).
2. **Disassembler immediates are zero-extended** — the tablegen decoder
   zero-extends 16-bit offsets and 32-bit immediates where the asm
   parser sign-extends; without normalization, every negative
   immediate/offset (backward branches included) was wrong in bytes
   mode. Fixed with per-slot sign-extension in `runBytes`.
3. **Coverage gaps closed**: `JSET_{rr,ri}_32`, `BSWAP16/32/64`,
   `JMPL` (gotol), and the full atomics family (`lock` add/and/or/xor,
   fetch variants, `xchg`, `cmpxchg`, 32- and 64-bit) are now lifted —
   atomics as seq_cst `atomicrmw`/`cmpxchg` (execution semantics;
   ordering refinement for TV is future work with the acq/rel
   instructions).

## Notes

- The conformance ABI maps to the lifter naturally: r1 = memory
  pointer, r2 = size, r0 = result — the dummy source function's
  signature carries it.
- Plugin protocol requires clean stdout; lifter debug output now goes
  through the caller-controlled stream.
- Not yet wired into CI (the runner needs boost in the devcontainer
  image); local run documented above.
- Next increments: conformance helpers by number (would cover the
  remaining `call` tests and enable uBPF-parity), bpf2bpf local calls,
  and cross-checking against uBPF/rbpf plugins as additional oracles.
