# [DRAFT — do not send] Retiring the escaping-stack false-alarm class with per-object stack blocks

- **Status**: draft
- **Target**: arm-tv authors (methodological note; companion to the
  byte-kind alive2 draft). Potentially a patch offer against
  backend_tv rather than just a note.
- **Confidence**: high on the technique and the measured results in
  bpf-tv; medium on portability claims — the AArch64 lifter's stack
  model differs in detail (SP-relative addressing, callee-saved
  spills, red zone), and we have not prototyped the port. Before
  sending: sanity-check the technique against one minimized arm-tv
  false alarm from their own corpus (the class is shared: our M1
  triage reproduced pr57872-shaped escaping-stack false alarms under
  reference riscv-tv).

## The problem (shared across backend-tv derivatives)

The lifted target models the whole stack frame as one allocation,
while the source has one local block per alloca. Alive2's per-block
call-input matching cannot map N escaping source blocks onto the one
target block, so any function passing two distinct stack objects to
callees — or passing one whose derived pointer returns and is
dereferenced — fails refinement spuriously. In our corpus this class
was the entire INCORRECT population (31 kernel-selftest functions)
plus a mandatory up-front rejection for multi-escape functions.

## The technique

1. **Ask the backend for its own frame layout.** After codegen, read
   MachineFrameInfo: every non-dead slot with a `getObjectAllocation`
   gives (FP-relative offset, size, align, originating IR alloca).
   The offsets are exactly what frame-index elimination baked into
   the instruction stream — no inference. Practical detail: the
   legacy `addPassesToEmitFile` pipeline unconditionally appends
   FreeMachineFunctionPass, so we replicate its body (public API:
   createPassConfig / addISelPasses / addMachinePasses /
   addAsmPrinter) minus the free, and read MFI after the run.
2. **Carve the lifted frame post-optimization.** After -O3 and the
   ptrtoint-roundtrip collapse, classify every value derived from the
   stack block: constant-offset geps, `inttoptr(add(ptrtoint(frame),
   -K))` escape shapes, gep-of-gep chains, variable-index geps
   attributed by constant base. Slot-addressed pointers are replaced
   with geps off fresh per-object allocas; everything else stays on
   the residual block. Derived pointers follow automatically because
   only direct users move.
3. **All-or-nothing.** A partial split is the one unsound shape (an
   unattributed access could touch a split object's bytes in the
   machine but miss its block in the model, hiding a real clobber),
   so any unclassifiable flow abandons the rewrite and the faithful
   single-block model stays. Attributed-but-dynamically-OOB accesses
   only add UB to the model — refinement failure, never silent
   acceptance.
4. **Clamp optimizer-stamped alignments.** -O3 proves access
   alignments against the 16-aligned single frame (base + C); a split
   object at a non-16-divisible offset cannot justify them (the
   machine address is congruent to C mod 16, inexpressible as an
   alloca align), which reads as spurious target UB. The ISA imposes
   no alignment; clamp claims on split-object accesses to what the
   model can prove.

## Measured (bpf-tv, kernel selftests, 2026-07-31)

INCORRECT 31 → 1 in one step (18 verify outright, 12 become provable
with more solver budget); the remaining 1 was the unrelated byte-kind
class (separate draft). Combined with admitting previously-rejected
multi-escape functions, verified coverage rose 1212 → 1273 of 4316,
and the LLVM CodeGen/BPF corpus reached 0 INCORRECT.

## Residual (honest limits)

- Opaque callees that WRITE through escaped stack pointers still fail
  under a successful split (~80 functions, bpf_get_func_arg / dynptr
  shapes); we report those as a known limitation post-verification
  rather than as miscompilations. This looks like an escaped-block
  write-back matching question — possibly for Alive2 rather than the
  lifters.
- Pointer bytes moved through integer registers (memcpy/load forms)
  are the byte-kind class — see the companion alive2 draft.
