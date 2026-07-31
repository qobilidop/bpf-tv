# bpf-tv design

Translation validation for the LLVM BPF backend: prove (per compilation, via
SMT) that the BPF machine code the backend emits refines the LLVM IR it was
given. The kernel verifier checks *safety*; nothing above the bytecode checks
*correctness* — a miscompiled program can be verifier-accepted and silently
wrong (e.g. llvm-project#208984). bpf-tv fills that gap.

Tool-first, not paper-first. Full motivation and landscape:
`garden/scratchpad/2026/2026-07-30/bpf-tv-evaluation.md`.

## Architecture

The arm-tv recipe (Lopes/Regehr et al., OOPSLA 2025), applied out-of-tree:

```
            LLVM BPF backend                bpf2llvm lifter
LLVM IR ─────────────────────▶ BPF asm ─────────────────────▶ lifted LLVM IR
   │                                                               │
   │ llvm2alive                              -O3 compaction, then llvm2alive
   ▼                                                               ▼
 Alive2 IR (src) ◀────────── refinement check ──────────── Alive2 IR (tgt)
                        (Alive2 "assembly mode": tgt_is_asm,
                         physical pointers, freeze-poison init)
```

- **Substrate**: official AliveToolkit/alive2, pinned as a git submodule at
  `third_party/alive2`. Measured 2026-07-30: the assembly-mode core
  (`config::tgt_is_asm`, physical-pointer memory model in `ir/memory.cpp` /
  `ir/state.cpp`) is upstream, and the arm-tv branch carries **zero**
  semantic changes to core Alive2 beyond its `backend_tv/` directory (its
  only core delta is a cosmetic `std::flush`). So the official repo is a
  sufficient and better-maintained foundation.
- **Lifter framework**: we own a BPF-specialized descendant of arm-tv's
  target-agnostic plumbing (~3 kloc: `mc2llvm` builder/ABI core,
  `streamerwrapper` asm→CFG reconstruction, `generateAsm`), vendored into
  `src/` with attribution (MIT), stripped of FP/vector/ASLP machinery BPF
  never uses. The arm-tv branch stays pinned at
  `third_party/alive2-arm-tv` as a never-built reference; its `backend_tv/`
  moves slowly (4 commits in the first half of 2026), so manually porting
  fixes is cheap.
- **Own driver** (`src/bpf-tv.cpp`, modeled on arm-tv's
  `tools/backend-tv.cpp`): constructs the BPF lifter directly. No fork, no
  patches: bpf-tv consumes official alive2's static libs Minotaur-style.

## The BPF machine model (lifter semantics)

- Registers r0–r10 as 64-bit slots (`createRegStorage`), w0–w10 as the low
  32-bit subregisters of the same storage. **Writes to w regs zero the upper
  32 bits** — this is the single semantic dimension behind the backend's worst
  bug cluster (BPFMIPeephole zext elimination, ~5 wrong-code fixes in 7
  years), so it must be modeled exactly.
- ABI: r1–r5 arguments, r0 return, r6–r9 callee-saved, r10 read-only frame
  pointer = top of a 512-byte stack region (offsets negative). No stack
  arguments, no varargs, no FP, no vectors.
- ISA edge cases per RFC 9669 runtime semantics: div-by-zero → dst = 0,
  mod-by-zero → dst unchanged (lift via `createCheckedUDiv`/`SDiv` etc. —
  LLVM IR udiv-by-0 is UB, the machine instruction is not); shift amounts
  masked by 0x3F/0x1F (`createMaskedShl` etc.).
- Helpers and kfuncs: uninterpreted external calls with matched call sequences
  (K2's treatment; sound because the backend doesn't reorder calls). `call` →
  marshall r1–r5, clobber r0–r5, result in r0.
- Signed div/mod (v4): sdiv/smod with INT_MIN/-1 → INT_MIN (kernel-defined),
  div/mod-by-zero as above.

## Cut point: the `.o` as a template

The compiler's `.o` is not final code — libbpf rewrites imm/off fields at load
time (CO-RE), and the verifier patches again. bpf-tv validates the compiler's
step only:

- **v0**: concrete relocations (programs without CO-RE, or already-relocated
  objects). Validates what the backend emits, matching the arm-tv precedent.
- **v1 (the novel increment)**: treat CO-RE-relocatable imm/off fields as
  symbolic constants constrained by `.BTF.ext` records — proving correctness
  for *all* kernels at once, and turning BPFMISimplifyPatchable /
  BPFAbstractMemberAccess from trusted into validated code.
- Post-libbpf and post-verifier-patching cut points are out of scope
  (different obligations; verifier territory).

## v0 scope

In: sequential integer code, loop-free or bounded loops (Alive2's bounded
unrolling), single subprogram, loads/stores through pointer args and stack,
helper calls, all of ALU32/ALU64/JMP/JMP32, byte-swaps, `-mcpu=v3` baseline.

Out (reject with a clear message): arena address spaces (`addr_space_cast`;
Alive2 lacks multi-address-space support), inline asm (pervasive via
`barrier_var`/`may_goto` macros — measure the coverage cost early), bpf2bpf
multi-subprogram composition, atomics (add after the core works; note open
miscompile #210280 as a target), gotox/jump tables, 128-bit values.

## Pipeline mechanics (inherited from backend-tv)

1. `addDebugInfo` tags each IR instruction with a line number so lifted
   instructions map back (`lineMap`).
2. `generateAsm` runs the real backend (SelectionDAG path) to textual asm.
   Risk to validate early: the BPF MC asm parser must round-trip what the BPF
   asm printer emits (pseudo-C syntax). If round-trip holes appear, switch to
   lifting from the object file's MCInsts instead of textual asm.
3. `mc2llvm::run()` parses asm into MCFunction/MCBasicBlocks (via
   `MCStreamerWrapper`), calls `platformInit`, then `lift()` per MCInst.
4. Lifted module is compacted with `-O3` (`optimize_module`), then both sides
   go through `llvm2alive` and the refinement check
   (`Verifier::compareFunctions`), with `config::tgt_is_asm = true`.

## Target facts (driver constants)

- Triple `bpfel`, DataLayout `e-m:e-p:64:64-i64:64-i128:128-n32:64-S128`
  (from `BPFTargetMachine.cpp`), CPU `v3` (clang ≥20 default; v4 later),
  features "".
- `LLVMInitializeBPF{TargetInfo,Target,TargetMC,AsmParser,AsmPrinter}`.
- Opcode/register enums from the LLVM build tree:
  `Target/BPF/BPFGenInstrInfo.inc`, `BPFGenRegisterInfo.inc` (`BPF::R0..R10`,
  `BPF::W0..W10`).

## Testing

- **Lifter correctness** (the TCB risk — hand lifters are empirically
  bug-prone): differential-test the lifter against executable oracles.
  bpf_conformance's 313-test corpus (asm + memory in, r0 out) as the harness;
  uBPF/rbpf as oracles; `alive-exec` can execute the lifted IR.
- **End-to-end**: lit-style tests — small .ll files through the full
  pipeline, checked for "0 incorrect transformations"; known-miscompile
  regression cases from the LLVM issue record as true-positive tests.
- **Campaign** (post-v0): kernel selftests + Cilium `.o`s for coverage
  measurement; YARPgen/IRFuzzer per arm-tv's fuzzing recipe.

## Build model

- Reproducibility policy: every dependency is a submodule pinned at an exact
  commit, and the canonical build environment is the devcontainer
  (`.devcontainer/`, Ubuntu 24.04 — same family as alive2's CI). This will
  extend to test-harness deps (bpf_conformance, uBPF/rbpf) when they arrive.
- `third_party/llvm-project`: submodule pinned to LLVM main `f0ad8ee185bc`
  (2026-05-13). Built from source — RTTI + EH + assertions, targets
  `AArch64;RISCV;BPF` — because the lifter includes build-tree tablegen
  output (`Target/BPF/BPFGen*.inc`); an installed LLVM lacks those.
- `third_party/alive2`: official repo, submodule pinned at `cb9fbd54`
  (2026-04-27) — the arm-tv branch's merge-base, so it is exactly the core
  the reference code was developed against, and its compatibility with our
  LLVM pin is proven (the arm-tv head built green against it natively).
  Built with `BUILD_LLVM_UTILS=ON` only — no ANTLR/ASLP anywhere in our
  dependency chain.
- `third_party/alive2-arm-tv`: reference-only submodule (arm-tv branch head
  `ab2fce5a`), never built by our scripts.
- Z3: pinned source build (`z3-4.15.4`, Minotaur's recommended practice) in
  the devcontainer at `/opt/z3`. Note: brew's Z3 4.16 showed a pathological
  backend-tv refinement-query slowdown on macOS (2026-07-30, unresolved);
  treat container Z3 as the configuration of record.
- All builds are out-of-tree under `$BUILD_ROOT` (default `build/`;
  `build-linux/` in the devcontainer) so submodules stay clean.
- `scripts/build-deps.sh` builds both; top-level CMake then builds `bpf-tv`
  against them (Minotaur-style out-of-tree consumer).

## Staging

1. ✅ Scaffold + skeleton (this repo).
2. Build chain green; lift trivial functions (`mov r0, imm; exit`).
3. Core ISA coverage; bpf_conformance differential harness.
4. Run over kernel selftests; measure coverage, solver timeouts, inline-asm
   rejection rate.
5. Symbolic CO-RE relocations (v1, the novel part).
6. Engage: LLVM Discourse + BPF maintainers (Yonghong Song, Eduard
   Zingerman); LPC eBPF track / LSFMM+BPF talk; eBPF Foundation 2026 call.
7. Verifier-acceptability checking: separate tool, separate decision.
