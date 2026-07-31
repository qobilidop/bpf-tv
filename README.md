# bpf-tv

Translation validation for the LLVM BPF backend, built on
[Alive2](https://github.com/regehr/alive2)'s `arm-tv` branch (the
[arm-tv](https://users.cs.utah.edu/~regehr/papers/arm-tv.pdf) /
riscv-tv lineage).

Given an LLVM IR function, bpf-tv runs the real BPF backend to produce
assembly, lifts that assembly back to LLVM IR with a hand-written BPF lifter
(`bpf2llvm`), and asks Alive2 whether the round trip is a refinement — i.e.
whether the backend miscompiled. The kernel verifier checks *safety*; this
checks *correctness*, the pipeline stage nothing else covers.

See [DESIGN.md](DESIGN.md) for architecture, scope, and staging.

## Layout

```
src/                       BPF lifter (bpf2llvm) + driver (bpf-tv)
third_party/alive2         pinned submodule: regehr/alive2 @ arm-tv branch
third_party/llvm-project   pinned submodule: llvm/llvm-project
scripts/build-deps.sh      builds both, out-of-tree under build*/
.devcontainer/             reproducible Ubuntu build environment
```

Everything is pinned: both toolchain dependencies are submodules at exact
commits (alive2's arm-tv head and an LLVM main commit of the same date, so
their APIs agree), and the canonical build environment is the devcontainer.

## Building

The canonical environment is the devcontainer (Ubuntu 24.04; open the repo in
VS Code → "Reopen in Container", or use the devcontainer CLI). Inside it:

```sh
git submodule update --init --depth 1   # pinned SHAs; shallow is fine
./scripts/build-deps.sh                 # builds LLVM, then alive2 (long)
cmake -S . -B "$BUILD_ROOT/bpf-tv" -G Ninja
ninja -C "$BUILD_ROOT/bpf-tv"
```

`BUILD_ROOT` defaults to `build/`; the devcontainer sets it to `build-linux/`
so a native macOS build tree can coexist. A native (non-container) build works
too with `brew install cmake ninja re2c z3 antlr antlr4-cpp-runtime`.

An LLVM *source build* is required (RTTI, EH, assertions; targets
AArch64;RISCV;BPF) because the lifters use build-tree tablegen headers.

## Usage

```sh
build/bpf-tv foo.ll                  # validate first function
build/bpf-tv --fn=my_func foo.ll     # validate a specific function
build/bpf-tv --asm-only foo.ll       # just show the backend's asm
```
