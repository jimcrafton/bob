@message = private unnamed_addr constant [11 x i8] c"h\C3\A9llo \E2\9C\93\0A"

declare void @bobRuntimeWrite(i32, ptr, i64)

define i32 @bobMain() {
  call void @bobRuntimeWrite(i32 1, ptr @message, i64 11)
  ret i32 0
}
