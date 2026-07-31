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

## 2026-07-30 — `tail` markers stripped from lifted calls (SUPERSEDES the stack-escape rejection below)

**Context:** The entire "escaping stack pointer" false-alarm class turned
out to be an artifact, diagnosed by exact-pair bisection: while lifted
code still contains `ptrtoint/add/inttoptr` chains, -O3 cannot see that a
call argument derives from the stack alloca and marks calls `tail`; our
round-trip collapse then rewrites the argument into a direct alloca GEP.
A `tail` call passing an alloca-derived pointer means the callee may not
access it (LangRef), so every callee access became UB → "source is more
defined than target". The reference riscv-tv has the identical collapse
and -O3 recipe, which is why it reproduced there — shared artifact, not
shared fundamental limitation.

**Decision:** `fixupOptimizedTgt` clears `tail` on every lifted call.
Sound: removing `tail` only enlarges the target's behavior set, so a
miscompiled target cannot start verifying. The blanket stack-escape
rejection is removed.

**Measured:** 7 of the 9 former INCORRECTs now genuinely verify; corpus
verified 196 → 204; `pr57872` is a slow-but-honest failed-to-prove.

**Genuine residual (1 corpus function, `simplifycfg.ll`):** an opaque
callee that *writes a pointer through* an escaped slot could, in the
single-block lifted stack, produce a pointer aliasing sibling stack
data — impossible with the source's per-alloca blocks. Not statically
detectable with opaque pointers; documented as the one known false-alarm
class instead of rejected. Proper fix would need block-granular stack
modeling or callee-behavior axioms (see the arm-tv ABI-axiom study).

## 2026-07-30 — [superseded] Escaping-stack-pointer false alarms: rejected up front (option b)

**Context:** Functions that pass a pointer to their stack frame to an
external callee fail refinement: the lifted callee receives a pointer into
the one big lifted stack block and so can "see" sibling stack data the
source callee cannot. Reproduced identically with the reference riscv-tv
on all 7 corpus instances — a limitation of the inherited setup, not our
port. It accounted for the *entire* remaining INCORRECT column (the
i128 and bpf-fastcall-3 alarms turned out to be this class too), plus 2
of 3 crashes and several failed-to-proves.

**Decision:** detect the pattern in `checkFuncSupport` (call argument
whose underlying object is an alloca, following loads through stack
slots for mem2reg-unoptimized IR) and reject with a clear
known-limitation message → `unsupported:stack-escape` in the corpus
taxonomy (17 functions). A false INCORRECT is poison for a verifier's
credibility; an honest coverage gap is not.

**Cost:** real-world code passing stack buffers to helpers
(`bpf_probe_read(&local, ...)`) is temporarily out of scope — this is a
significant real-world pattern, so option (a) remains queued:

**Revisit when:** studying how arm-tv's aarch64 path handles escaping
stack pointers (per-callee ABI axioms?) — required before the kernel
selftests campaign (M4), where this pattern is pervasive.

**Note:** `mc2llvm::checkFuncSupport` was a declared-but-never-called
hook in the reference; bpf-tv now wires it into `checkSupport`.

## 2026-07-30 — bpf_fastcall callees preserve r1–r5

**Context:** Callees with the `"bpf_fastcall"` attribute preserve all
registers except r0, and the backend relies on this. Modeling all calls
as clobbering r1–r5 made such code fail refinement (corpus:
`bpf-fastcall-2.ll` borderline, `bpf-fastcall-3.ll`).

**Decision:** `doCall` skips the r1–r5 clobber when the callee or call
site carries the attribute.

**Revisit when:** the backend's bpf_fastcall handling changes (it has a
wrong-code history: llvm #110618).

## 2026-07-30 — Driver flag is `--cpu`, not `--mcpu`

**Context:** LLVM's `RegisterCodeGenFlags` (constructed inside alive2's
`optimize_module`) registers `mcpu` itself; duplicate `cl::opt` names are
fatal at runtime.

**Decision:** our flag is `--cpu` (default `v3`).

## 2026-07-30 — Lifted code still optimized at -O3 by default, despite a known false-negative window

**Context:** The lifter models unspecified bits (call-return upper halves,
sub-64 argument padding) as `freeze poison`. The -O3 compaction of lifted
code may legally *refine* that nondeterminism (observed: it chose 0),
which can convert a non-refining target into a refining one — false
negatives, demonstrated concretely in the M2 experiment: the reintroduced
2019 zext bug is caught at `--optimize-tgt=O0` and missed at the default
-O3. Optimizing the target side can only shrink its behavior set, so this
window is structural, and it exists in the arm-tv/riscv-tv reference too
(same `enforceSExtZExt`, same -O3 default).

**Measured trade (CodeGen/BPF corpus):** O3 — 195 verified, 9 INCORRECT
(all known-cause), median 0.02s. O0 — 175 verified, **25 INCORRECT**
(16 additional, likely false alarms from unoptimized register-file memory
traffic), 21 failed-to-prove, median 0.07s / max 3.6s.

**Decision:** default stays `-O3` (usable, no spurious alarms);
`--optimize-tgt=O0` remains available.

**Resolution (same day):** window closed by modeling unspecified bits as
loads from `@__bpf_tv_unspecified`, an external **constant** global with
no initializer (`bpf2llvm::unspecifiedValue`, overriding a new
`mc2llvm::unspecifiedValue` hook whose default stays reference-faithful
freeze(poison)). Unknown-but-fixed contents are unfoldable by -O3 and
adversarially chosen by the solver; each junk site reads a distinct
offset. Measured: planted 2019 bug now caught at default -O3; corpus
unchanged (195 verified, same 9 INCORRECT, median 0.02s). Constraints
discovered on the way, worth knowing: Alive2 rejects *non-constant*
globals introduced only in the target, and a source-side declaration
doesn't help because llvm2alive only registers globals the source
function uses — hence the constant-global formulation. Its accepted
approximation: re-executions of one site (loops) and reloads across
calls see the same value; per-site independence is preserved.

**Revisit when:** a bug class hinges on per-iteration junk freshness in
loops, or upstream adopts a native mechanism.

## 2026-07-30 — M2 reproduction: bug-reintroduction in place, candidate = 2019 zext/COPY-physreg bug

**Context:** M2 needs a historical BPF miscompile demonstrably caught by
bpf-tv. Candidates considered: #208244 (2026 subreg misfold) — merged
*after* our LLVM pin so technically live, but latent: it needs a
`MOV_32_64` with subreg source that no IR path produces at our pin (its own
regression test is hand-written MIR); #208984 (2026, open) — reproducer
requires inline-asm `barrier()`, which v0 rejects; PR48578 (2021) — symptom
was a compiler crash, not silent wrong code.

**Decision:** reproduce the 2019 bug fixed by `a0841dfe8594` (zext
elimination wrongly trusting a `COPY` from a physical register, i.e. a
call return value whose upper 32 bits are unknown). Method: invert the
`Reg.isVirtual()` guard in `isCopyFrom32Def` in the in-place LLVM tree,
incremental rebuild (~seconds), validate `zext i32 (call ...)` to i64,
restore tree, rebuild, re-validate. Reintroduction-in-spirit (guard
inversion), semantically matching bug #1 of the fix's commit message.

**Result:** buggy compiler emits `call get; exit` (zext gone) → bpf-tv
reports INCORRECT (with `--optimize-tgt=O0`; see the next entry for why O3
misses it); pristine compiler emits `call get; w0 = w0; exit` → verified.

**Why this method:** in-place revert + incremental rebuild costs seconds;
a parallel scratch LLVM build costs ~40 minutes and disk. The tree is
restored and re-verified afterwards (e2e suite green).

## 2026-07-30 — "rN = wM" rewritten to "wN = wM" before asm parsing

**Context:** LLVM's BPF asm printer emits `MOV_32_64` (zext of a w
register into an r register) as `rN = wM`; LLVM's own BPF asm parser
rejects that spelling (minimized: `llvm-mc -triple=bpfel` accepts
`w0 = w1`, rejects `r0 = w0`). A printer/parser round-trip hole at our
LLVM pin — upstream-reportable.

**Decision:** `generateAsm` rewrites `rN = wM` → `wN = wM` textually
before parsing.

**Why sound:** at the encoding level `MOV_32_64` and `MOV_rr_32` are the
same machine instruction (0xbc, BPF_ALU | BPF_MOV | BPF_X), whose ISA
semantics zero the upper 32 bits; the rewrite is a byte-level identity on
emitted code, and the lifter's `MOV_rr_32` case models exactly those
semantics.

**Revisit when:** the parser is fixed upstream, or lifting moves from
textual asm to captured MCInsts (planned M3 direction) — then delete the
rewrite.

## 2026-07-30 — Empty inline asm stripped from the semantic side only

**Context:** `asm volatile("" ::: "memory")` — the `barrier()` /
`barrier_var` macro family — is pervasive in real BPF code and was v0's
biggest single coverage exclusion. An empty template emits no
instructions and a void call produces no values: at runtime it is a
no-op; it exists only to constrain the optimizer.

**Decision:** the driver strips empty-template void inline asm from the
copy of the source used for refinement, while codegen runs on an
unstripped clone (barriers affect scheduling/layout, and the whole point
is to validate what the backend does *with* them). Non-empty templates
and value-producing asm remain rejected.

**Ordering lesson (cost of getting it wrong: 9 crashes):** the Alive2
pre-flight check must run *between* stripping and codegen — inputs
Alive2 rejects anyway (atomics, remaining asm) can drive the backend
into `report_fatal_error` aborts if compiled first.

**Revisit when:** adding `barrier_var` (empty template with tied "+r"
operands — an identity function, modelable the same way).

## 2026-07-30 — (see DESIGN.md) substrate, pins, EXTERNAL_PROJECTS

The larger decisions — official alive2 + vendored plumbing instead of the
arm-tv fork, the three-way pin triple and its bump recipe, consuming alive2
via its `EXTERNAL_PROJECTS` hook — are recorded with rationale in
[DESIGN.md](DESIGN.md).
