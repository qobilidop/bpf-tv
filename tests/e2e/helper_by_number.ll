; Helper calls by number: selftest-style indirect calls through an
; integer-constant function pointer, which the backend emits as
; `call N`. Covers a no-arg i64 helper (bpf_get_prandom_u32 shape)
; and a pointer-arg/pointer-return helper whose result guards a load
; (bpf_map_lookup_elem shape). Both sides must agree on the
; synthesized @__bpf_helper_N uninterpreted functions.
define i64 @helper_by_number(ptr %ctx, i64 %k) {
entry:
  %r = call i64 inttoptr (i64 7 to ptr)()
  %v = call ptr inttoptr (i64 1 to ptr)(ptr %ctx, i64 %k)
  %isnull = icmp eq ptr %v, null
  br i1 %isnull, label %miss, label %hit
hit:
  %x = load i64, ptr %v, align 8
  %s = add i64 %x, %r
  ret i64 %s
miss:
  ret i64 %r
}
