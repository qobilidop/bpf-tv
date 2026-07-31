define i64 @swapadd(ptr %p, ptr %q) {
  %a = load i64, ptr %p
  %b = load i64, ptr %q
  store i64 %b, ptr %p
  store i64 %a, ptr %q
  %s = add i64 %a, %b
  ret i64 %s
}
