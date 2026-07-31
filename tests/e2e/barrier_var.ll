; barrier_var passthrough: asm volatile("" : "+r"(x)) -- an empty
; template with a register output tied to an input emits no
; instructions and is an identity function at runtime. The semantic
; copy drops it (replaceIdentityInlineAsm); codegen keeps the
; register-allocation constraint.
define i64 @barrier_var(i64 %x, i64 %y) {
entry:
  %b = call i64 asm sideeffect "", "=r,0"(i64 %x)
  %s = add i64 %b, %y
  %b2 = call i64 asm sideeffect "", "=r,0"(i64 %s)
  ret i64 %b2
}
