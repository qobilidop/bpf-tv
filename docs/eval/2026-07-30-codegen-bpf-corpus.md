# bpf-tv corpus run: third_party/llvm-project/llvm/test/CodeGen/BPF

- 157 files, 429 functions
- smt-to=10000ms, wall-cap=120s

| outcome | count | % |
|---|---|---|
| verified | 189 | 44.1% |
| unsupported:src-ir | 110 | 25.6% |
| other | 22 | 5.1% |
| asm-parse-error | 21 | 4.9% |
| unsupported:inline-asm | 12 | 2.8% |
| failed-to-prove | 11 | 2.6% |
| unsupported:varargs | 9 | 2.1% |
| backend-error | 9 | 2.1% |
| INCORRECT | 9 | 2.1% |
| input-error | 7 | 1.6% |
| unsupported:many-args | 7 | 1.6% |
| unsupported:arg-type | 7 | 1.6% |
| unsupported:insn | 6 | 1.4% |
| crash | 3 | 0.7% |
| unsupported:global-lookup | 3 | 0.7% |
| unsupported:wide-int | 2 | 0.5% |
| unsupported:aggregate | 2 | 0.5% |

## Unsupported-instruction histogram

| opcode | count |
|---|---|
| LD_pseudo | 2 |
| JALX | 1 |
| LD_ABS_B | 1 |
| LD_ABS_H | 1 |
| LD_ABS_W | 1 |

## Verification time (verified functions)

- n=189, median=0.02s, p90=0.06s, max=0.73s

Slowest verified:
- 0.7s unaligned_load_store.ll:test_store_i64
- 0.5s unaligned_load_store.ll:test_load_i64
- 0.4s cttz-ctlz.ll:cttz_i64
- 0.4s cttz-ctlz.ll:cttz_i64_zdef
- 0.4s sdiv_to_mul.ll:foo1
- 0.2s unaligned_load_store.ll:test_store_i32
- 0.2s adjust-opt-icmp5.ll:test
- 0.2s unaligned_load_store.ll:test_load_i32
- 0.2s adjust-opt-icmp6.ll:test
- 0.1s is_trunc_free.ll:test

## INCORRECT transformations (investigate!)

- third_party/llvm-project/llvm/test/CodeGen/BPF/bpf-fastcall-3.ll:foo
- third_party/llvm-project/llvm/test/CodeGen/BPF/fi_ri.ll:test
- third_party/llvm-project/llvm/test/CodeGen/BPF/i128.ll:test
- third_party/llvm-project/llvm/test/CodeGen/BPF/remove_truncate_5.ll:test
- third_party/llvm-project/llvm/test/CodeGen/BPF/loop-exit-cond.ll:test
- third_party/llvm-project/llvm/test/CodeGen/BPF/rodata_5.ll:test
- third_party/llvm-project/llvm/test/CodeGen/BPF/simplifycfg.ll:test
- third_party/llvm-project/llvm/test/CodeGen/BPF/warn-stack.ll:nowarn
- third_party/llvm-project/llvm/test/CodeGen/BPF/pr57872.ll:foo
