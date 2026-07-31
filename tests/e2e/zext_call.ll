declare i32 @get()

define i64 @f() {
  %v = call i32 @get()
  %z = zext i32 %v to i64
  ret i64 %z
}
