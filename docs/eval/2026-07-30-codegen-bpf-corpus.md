# bpf-tv corpus run: third_party/llvm-project/llvm/test/CodeGen/BPF

- 157 files, 429 functions
- smt-to=10000ms, wall-cap=120s

| outcome | count | % |
|---|---|---|
| verified | 205 | 47.8% |
| unsupported:src-ir | 110 | 25.6% |
| other | 29 | 6.8% |
| failed-to-prove | 18 | 4.2% |
| unsupported:insn | 10 | 2.3% |
| unsupported:inline-asm | 10 | 2.3% |
| unsupported:varargs | 9 | 2.1% |
| backend-error | 9 | 2.1% |
| unsupported:many-args | 8 | 1.9% |
| input-error | 7 | 1.6% |
| unsupported:arg-type | 7 | 1.6% |
| unsupported:global-lookup | 3 | 0.7% |
| unsupported:wide-int | 2 | 0.5% |
| unsupported:aggregate | 2 | 0.5% |

## Unsupported-instruction histogram

| opcode | count |
|---|---|
| LD_IND_H | 2 |
| LD_IND_W | 2 |
| LD_pseudo | 2 |
| JALX | 1 |
| LD_ABS_B | 1 |
| LD_ABS_H | 1 |
| LD_ABS_W | 1 |

## Verification time (verified functions)

- n=205, median=0.02s, p90=0.04s, max=0.85s

Slowest verified:
- 0.9s loop-exit-cond.ll:test
- 0.7s unaligned_load_store.ll:test_store_i64
- 0.5s unaligned_load_store.ll:test_load_i64
- 0.4s cttz-ctlz.ll:cttz_i64
- 0.4s cttz-ctlz.ll:cttz_i64_zdef
- 0.4s sdiv_to_mul.ll:foo1
- 0.2s unaligned_load_store.ll:test_store_i32
- 0.2s adjust-opt-icmp5.ll:test
- 0.2s unaligned_load_store.ll:test_load_i32
- 0.1s adjust-opt-icmp6.ll:test

## 2026-07-31 update

Helper-calls-by-number landed: `objdump_trivial.ll:foo` moved
other → verified (204 → 205; still 0 INCORRECT). Full-corpus re-run
confirms no regression from the semantic-copy helper rewrite.
