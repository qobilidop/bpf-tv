define i64 @max(i64 %a, i64 %b) {
  %c = icmp sgt i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 %b
}
