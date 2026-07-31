# Design decisions log

Tricky or judgment-call decisions, so future-us knows what was decided and
why. Architecture-level decisions live in [DESIGN.md](DESIGN.md); this file
collects the fine-grained ones made during development. Add an entry whenever
a decision (a) affects soundness or result interpretation, (b) deviates from
the arm-tv reference, or (c) picks between defensible alternatives.

Format: date, decision, context, why, and when to revisit.

---

## 2026-07-30 — Backend error diagnostics are their own outcome, not INCORRECT

**Context:** For `sdiv`/`srem` below `-mcpu=v4`, the BPF backend emits an
error *diagnostic* (via the LLVMContext diagnostic handler) and then still
"succeeds", producing garbage code — a bare `exit`. Validating that garbage
reported a miscompilation; 5 of the first corpus sweep's 14 INCORRECTs were
this.

**Decision:** `generateAsm` installs a diagnostic handler; if the backend
reported `DS_Error`, bpf-tv reports "backend error" and does not validate.
The corpus runner buckets this as `backend-error`, separate from `INCORRECT`.

**Why:** The compiler signaled failure through its supported channel; a
consumer that checks diagnostics never sees the garbage code. Counting these
as miscompiles would drown the signal we care about. But note the garbage
*is* wrong code if consumed — a build system that ignores diagnostics and
loads the object would ship it.

**Revisit when:** we ever see evidence of real build pipelines consuming
objects despite backend error diagnostics — then these deserve louder
treatment.

## 2026-07-30 — undef inputs disabled by default

**Context:** With undef inputs enabled (Alive2's default), the value
refinement query contains doubly-quantified undef terms; Z3 (4.15.4 and
4.16.0 both) diverges on them even for a two-register `add` whose operands
`-O3` commuted — unprovable in 120s, 16ms once disabled. Also reproduces
against the reference riscv-tv binary.

**Decision:** the driver hard-sets `config::disable_undef_input = true`.

**Why:** trivially-verifiable functions timing out makes the tool useless in
practice; undef is being removed from LLVM; the arm-tv reference contains a
commented-out block contemplating exactly this ("undef is going away, we
don't want to see bugs about it").

**Cost:** miscompiles observable only under undef inputs are missed.

**Revisit when:** LLVM finishes removing undef (moot), or a real BPF
miscompile class is suspected to hide behind undef inputs.

## 2026-07-30 — ptr↔int round trips collapsed to GEP before verification (default on)

**Context:** Lifted stack-pointer arithmetic (`r1 = r10; r1 += -20`) becomes
`ptrtoint → add → inttoptr`, which Alive2 treats as a provenance escape and
produces spurious failures. Inherited behavior from the reference driver
(`run-replace-ptrtoint`, default true there too).

**Decision:** keep it, default on, flag-controlled.

**Why:** matches the reference; the rewrite is a trusted (unverified) step —
it slightly grows the TCB, which is the price of usable results.

**Revisit when:** Alive2 grows native handling for this pattern, or a
soundness question arises about a specific collapse.

## 2026-07-30 — Machine instructions lifted with RFC 9669 *runtime* semantics

**Context:** LLVM IR `udiv %a, 0` is UB; the BPF instruction `div` is total
(dst = 0). Same for mod (dst unchanged), signed overflow (INT_MIN wraps),
and shifts (amount masked by 0x3F/0x1F).

**Decision:** the lifter models the machine's total semantics
(`createCheckedUDiv` etc.), not the IR's partial ones. Refinement then
naturally allows the backend to exploit source UB.

**Why:** the machine semantics are what executes; anything else would be
unsound in the interesting direction.

**Revisit when:** kernel/RFC semantics diverge for new instructions (check
each ISA extension against both documents; the memory-ordering instructions
will need this care).

## 2026-07-30 — Calls are uninterpreted; r0–r5 clobbered, r6–r9 preserved

**Context:** Helpers/kfuncs have no semantics we can encode; K2 treated them
as uninterpreted with matched call sequences, sound because the backend does
not reorder calls.

**Decision:** same treatment; `doCall` marshalls r1–r5, havocs r0–r5,
trusts r6–r9 preservation.

**Known gap:** `bpf_fastcall` (`bpf-fastcall-3.ll` corpus false alarm)
changes the clobber set — the backend may rely on a helper preserving
r1–r5. Needs explicit modeling of the fastcall attribute.

**Revisit when:** implementing bpf_fastcall support (queued in PLAN.md).

## 2026-07-30 — Escaping-stack-pointer false alarms: OPEN

**Context:** Functions that pass a pointer to their stack frame to an
external callee (`fi_ri.ll`, `warn-stack.ll`, `pr57872.ll` class) fail
refinement: the lifted callee receives a pointer into the one big lifted
stack block and so can "see" sibling stack data the source callee cannot.
Reproduces identically with the reference riscv-tv, so it is a limitation of
the inherited setup, not our port.

**Decision:** none yet. Options: (a) study how arm-tv's aarch64 path
handles it (ABI axioms) and port; (b) classify as known-limitation bucket in
the corpus runner and accept the coverage loss.

**Revisit:** next triage session (queued in PLAN.md).

## 2026-07-30 — Driver flag is `--cpu`, not `--mcpu`

**Context:** LLVM's `RegisterCodeGenFlags` (constructed inside alive2's
`optimize_module`) registers `mcpu` itself; duplicate `cl::opt` names are
fatal at runtime.

**Decision:** our flag is `--cpu` (default `v3`).

## 2026-07-30 — (see DESIGN.md) substrate, pins, EXTERNAL_PROJECTS

The larger decisions — official alive2 + vendored plumbing instead of the
arm-tv fork, the three-way pin triple and its bump recipe, consuming alive2
via its `EXTERNAL_PROJECTS` hook — are recorded with rationale in
[DESIGN.md](DESIGN.md).
