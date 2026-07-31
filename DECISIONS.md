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

**Genuine residual (1 corpus function, `simplifycfg.ll`), now
diagnosed to a minimal reproducer (2026-07-30):** a pointer *written by
one callee* into an escaped slot, loaded back, and *passed as an
argument to a second call*. Loading and branching on such a pointer
verifies; loading an integer verifies; only the loaded-pointer-as-
call-arg shape fails. Mechanism (consistent with the Alive2 study):
the lifted side materializes the pointer from raw stack-block bytes —
physical — while the source's is logical, and asm mode's
`isLogical() == other.isLogical()` conjunct in `Pointer::fninputRefined`
rejects the call-argument match. No ptrtoint chain exists for our
collapse to rewrite, so the only fixes are upstream: drop the conjunct
(one line, precedent #1133/#1153) or model callee writes to escaped
locals. Kept as the one documented INCORRECT until then.

**Inherited soundness caveat (from the same study, worth its own line):**
because callee writes to escaped local blocks are unmodeled, refinement
can MISS miscompiles in which the backend mis-lays-out stack data that a
helper writes. This affects arm-tv/riscv-tv equally; upstream knows
(open TODOs, issue #969). Log as a documented bug-missing window
alongside the (now-closed) freeze-refinement one.

**Upstream context (same study):** the exact fi_ri-class mechanism at
the Alive2 level is the asm-mode-only `isLogical() == other.isLogical()`
conjunct in `Pointer::fninputRefined` — an asymmetry with return-value
refinement, where AliveToolkit#1133/#1153 already removed it. Physical
(inttoptr) call args hit it; our default-on ptr-int collapse turns args
logical, dodging it — upstream's equivalent pass is off by default, and
`-O3` never folds `ptrtoint→add→inttoptr` itself. A one-line upstream
experiment (drop the conjunct) has clear precedent and is queued for the
drafts.

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

## 2026-07-31 — linux corpus is script-pinned, not a submodule

**Context:** M4 needs the kernel selftests (`tools/testing/selftests/bpf`,
1009 prog files at linux `8ba098e6b6ff`). Even a blob-filtered submodule
clone of linux carries gigabytes of history for every fresh checkout.

**Decision:** the one exception to the pin-as-submodule policy:
`scripts/fetch-linux-corpus.sh` pins the exact SHA and does a depth-1
sparse checkout (~200 MB) into gitignored `third_party/linux/`.
`third_party/vendored/vmlinux.h` (libbpf/vmlinux.h, x86 6.19) is
committed directly — small enough to vendor.

**Revisit when:** git submodule gains usable shallow+sparse support, or
the corpus needs to move in lockstep with other pins.

## 2026-07-31 — Container mount point standardized at /work

**Context:** CMake build trees bake absolute paths. The devcontainer CLI
mounted at `/workspaces/bpf-tv` while `dev.sh`'s original docker path and
the long-lived `build-linux` tree used `/work` — the mismatch made the
devcontainer CLI unable to reuse existing builds (cache paths broke).

**Decision:** `/work` everywhere: `devcontainer.json` sets
`workspaceMount`/`workspaceFolder`, matching raw `docker run -v $PWD:/work`
and CI's build trees. A ccache volume (`bpf-tv-ccache`) persists across
containers, and `build-deps.sh` uses ccache when present plus
memory-bounded parallelism (min(cores, mem/3GB) — LLVM's largest TUs need
~3 GB and an over-parallel build in a memory-capped VM gets OOM-killed).

**Host note (not in-repo):** local docker is colima; resized 2026-07-31
from 4 CPU / 8 GB to 12 CPU / 24 GB (`colima start --cpu 12 --memory 24`).

**Revisit when:** publishing a prebuilt toolchain image (see PLAN), which
must also bake /work paths.

## 2026-07-31 — Multiple escaping stack objects rejected (genuine model limit)

**Context:** M4's 20 INCORRECTs (kfunc_call_*, verifier_bits_iter,
dynptr_fail) minimized to: **two distinct allocas passed to callees**
(one call or two — irrelevant; one alloca with any number of calls
verifies). The source has one local block per alloca; the lifted target
has one block for the whole frame, and Alive2's per-block call-input
matching cannot map two distinct source blocks onto one target block.

**Decision:** detect in `checkFuncSupport` (distinct alloca underlying
objects among pointer call args > 1) and reject with a clear message
(`unsupported:stack-escape`), rather than emit a false INCORRECT.
`--allow-stack-escape` (hidden) keeps the failure reachable for study.

**Why:** same rule as before — a false INCORRECT poisons a verifier's
credibility; an honest coverage gap does not. This is the same family
as the `simplifycfg` residual, now with a crisper boundary.

**Cost:** real BPF code that passes two stack buffers to helpers is out
of scope. Proper fix is upstream/model-level: per-object stack blocks
in the lifter (a big change: the frame would stop being one alloca), or
Alive2 matching multiple source blocks into one target block.

**Revisit when:** attempting per-object stack modeling, or if upstream
relaxes local-block matching.

## 2026-07-31 — Helper calls by number: one uninterpreted function per ID

**Context:** selftest-style BPF calls kernel helpers through an
integer-constant function pointer (`call i64 inttoptr (i64 N to ptr)(...)`);
the backend emits `call N`. This blocked 334 selftest functions and
the conformance `call 5` test. Alive2 accepts the source-side indirect
call, but the lifted target would have no matching callee — an
indirect call through constant N and a named external call don't unify
as the same uninterpreted function.

**Decision:** synthesize one external declaration per helper ID,
`@__bpf_helper_N`, and make both sides call it. The driver rewrites
only the SEMANTIC copy of the source (after `CloneModule`, so codegen
still sees the original and still emits `call N`); the lifter maps a
JAL with an immediate operand back to `@__bpf_helper_N`, typed from
the source call site, and requires the source-side callee name to
match the immediate (a wrong helper ID in the asm is a hard error, not
a silent unification — verified by mutation test). Bytes mode
(conformance) has no source module, so helpers get the generic BPF
helper ABI `i64 (i64 x 5)` and the harness resolves the symbol at JIT
time.

**Why per-ID uninterpreted, not modeled semantics:** helper ID → C
signature is a kernel-version-dependent table; the refinement question
(registers/ABI/scheduling around the call) doesn't need helper
semantics, only "same unknown function on both sides" — exactly the
existing named-call path. Naming by number keeps bpf-tv independent of
any helper table.

**Cost:** helper-specific properties (e.g. a helper that never
returns, or argument-memory read-only-ness) are not modeled, so code
whose correctness depends on them may fail to prove (not INCORRECT).
Local calls (`call` with src_reg=1) remain excluded and are now
detected from the raw encoding in bytes mode — the MCInst drops the
src field that distinguishes them.

**Revisit when:** bpf2bpf local calls land, or a helper-signature
table becomes worth carrying (e.g. to model `bpf_tail_call`'s
noreturn-ish behavior or writable args precisely).

## 2026-07-31 — barrier_var passthrough (identity inline asm)

**Context:** the 2026-07-30 empty-template-asm entry's revisit hook:
`asm volatile("" : "+r"(x))` (`barrier_var`) is an empty template
whose register outputs are tied to inputs — no instructions execute,
every output equals its tied input; it exists only to constrain the
optimizer.

**Decision:** `replaceIdentityInlineAsm` in the driver: for
empty-template, non-void asm calls where every output is a direct
register with a tied input, replace results with the tied inputs in
the semantic copy (aggregate returns via their extractvalues; bail on
indirect outputs or any other use shape). Codegen still sees the asm
and its regalloc constraints. Clobbers are ignored — with no
instructions they, like the constraint itself, only affect the
optimizer, which has already run.

**Measured effect (honest):** small. The kernel-selftest corpus has
51 such call sites across 26 files; re-running the 1721 inline-asm-
bucket functions recovers 4 (1 verified, 2 now maps-blocked, 1
varargs). The bucket is 1653 `__naked` functions (asm *inputs*, out
of scope for backend TV by nature) + 64 with genuinely non-identity
asm (bpf_throw guards, `.8byte`, may_goto). The passthrough's real
value is unblocking barrier_var-containing functions whose *other*
blockers (maps, volatile) will fall later.

**Revisit when:** modeling may_goto (nondeterministic branch) or the
verifier-directive asm idioms, if ever worth it.
