; BPF map usage: a dso_local `.maps`-section global whose address is
; materialized by LD_imm64 (`r1 = the_map ll`) and passed to
; bpf_map_lookup_elem (helper 1), with the returned pointer null-checked
; and dereferenced. dso_local definitions put their data bytes under the
; ".Lsym$local" alias label in the asm; the lifter must resolve that
; back to the real symbol.
;
; The key arrives as a pointer parameter: a stack-allocated key would
; also exercise the known escaping-stack residual (a helper returning
; a stack-derived pointer that is then dereferenced), which is a
; separate documented limitation, not part of maps support.
%struct.anon = type { ptr, ptr, ptr, ptr }
@the_map = dso_local global %struct.anon zeroinitializer, section ".maps", align 8

define i64 @map_lookup(ptr %key) {
entry:
  %v = call ptr inttoptr (i64 1 to ptr)(ptr @the_map, ptr %key)
  %isnull = icmp eq ptr %v, null
  br i1 %isnull, label %miss, label %hit
hit:
  %x = load i64, ptr %v, align 8
  ret i64 %x
miss:
  ret i64 0
}
