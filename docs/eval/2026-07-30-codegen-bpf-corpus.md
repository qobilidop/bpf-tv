# bpf-tv corpus run: third_party/llvm-project/llvm/test/CodeGen/BPF

- 157 files, 429 functions
- smt-to=10000ms, wall-cap=120s

| outcome | count | % |
|---|---|---|
| verified | 195 | 45.5% |
| unsupported:src-ir | 110 | 25.6% |
| other | 26 | 6.1% |
| unsupported:stack-escape | 17 | 4.0% |
| failed-to-prove | 15 | 3.5% |
| unsupported:inline-asm | 11 | 2.6% |
| unsupported:insn | 10 | 2.3% |
| backend-error | 9 | 2.1% |
| unsupported:many-args | 8 | 1.9% |
| input-error | 7 | 1.6% |
| unsupported:varargs | 7 | 1.6% |
| unsupported:arg-type | 7 | 1.6% |
| unsupported:wide-int | 2 | 0.5% |
| unsupported:global-lookup | 2 | 0.5% |
| unsupported:aggregate | 2 | 0.5% |
| crash | 1 | 0.2% |

## Unsupported-instruction histogram

| opcode | count |
|---|---|
| LD_IND_H | 2 |
| LD_pseudo | 2 |
| LD_IND_W | 2 |
| JALX | 1 |
| LD_ABS_B | 1 |
| LD_ABS_H | 1 |
| LD_ABS_W | 1 |

## Verification time (verified functions)

- n=195, median=0.02s, p90=0.04s, max=0.69s

Slowest verified:
- 0.7s unaligned_load_store.ll:test_store_i64
- 0.5s unaligned_load_store.ll:test_load_i64
- 0.4s cttz-ctlz.ll:cttz_i64
- 0.4s cttz-ctlz.ll:cttz_i64_zdef
- 0.3s sdiv_to_mul.ll:foo1
- 0.2s unaligned_load_store.ll:test_store_i32
- 0.2s adjust-opt-icmp5.ll:test
- 0.1s unaligned_load_store.ll:test_load_i32
- 0.1s adjust-opt-icmp6.ll:test
- 0.1s is_trunc_free.ll:test
