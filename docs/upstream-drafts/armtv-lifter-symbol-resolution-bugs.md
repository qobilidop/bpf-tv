# [DRAFT — do not send] Two symbol/name-resolution bugs in backend_tv shared plumbing

- **Status**: draft
- **Target**: arm-tv authors (regehr/alive2 arm-tv branch, backend_tv/)
- **Confidence**: high that the code patterns exist upstream (both
  confirmed verbatim in the vendored reference at our pin); medium
  that they reproduce under arm-tv itself — that depends on (1) the
  AArch64 asm printer emitting `.Lsym$local` alias labels for
  dso_local data in their configurations and (2) their corpora
  containing the trigger shapes. Before sending: reproduce at least
  one under arm-tv/riscv-tv, or soften to "found in the shared code,
  fixed in our derivative, flagging in case it affects you."

Both were found while sweeping the Linux kernel BPF selftests
(bpf-tv 2026-07-31); both fixes are small and portable.

## 1. dso_local data lands under the `.Lsym$local` alias label

The ELF asm printer emits dso_local definitions as

```
sym:
.Lsym$local:
        .zero 32
```

`MCStreamerWrapper::emitLabel` flushes accumulated data on each label
(`addConstant`) and then tracks only the newest label, so the data
bytes are recorded under `.Lsym$local`, and any later lookup of `sym`
(e.g. from a relocation operand) fails "global symbol not found".
Every `.maps`-section global in kernel BPF code is dso_local, which
made this the single largest coverage gap in our corpus (491 of 4316
functions) until fixed. Fix (ours): teach `demangle` that
`.L<sym>$local` denotes `<sym>`; the existing `.L.`-prefix case is
untouched.

## 2. A function named `entry` corrupts the CFG

`MCFunction::checkEntryBlock` prepends a synthetic block literally
named `"entry"` so that asm branch targets never point at the LLVM
entry block. A function whose own label is `entry` (three Linux
selftests have one) creates an MC block with the same name; branch
resolution by name (`findBlockByName` / lifted-side `getBBByName`)
then picks the synthetic block, the fallthrough branch becomes a
self-edge into the function entry, and module verification fails
("Entry block to function must not have predecessors"). Fix (ours):
name the synthetic block something no assembler label can be
(`bpf-tv#entry`).

## Why report

Both live in the target-independent backend_tv plumbing, not in
anything BPF-specific, so arm-tv and riscv-tv likely inherit them.
The first silently caps coverage on any corpus with dso_local data
definitions (i.e. most real-world ELF code); the second is a
correctness-of-tooling crash on a legal symbol name.
