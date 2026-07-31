; two distinct allocas passed to callees: the historical
; escaping-stack false-alarm minimization, now verified via
; per-object stack blocks (frame layout from MachineFrameInfo +
; post-lift rewrite; see DECISIONS.md)
declare void @use(ptr)
declare i64 @get(ptr)

define i64 @twoesc(i64 %x) {
entry:
  %a = alloca i64, align 8
  %b = alloca i64, align 8
  store i64 %x, ptr %a, align 8
  store i64 7, ptr %b, align 8
  call void @use(ptr nonnull %a)
  %r = call i64 @get(ptr nonnull %b)
  %va = load i64, ptr %a, align 8
  %s = add i64 %va, %r
  ret i64 %s
}
