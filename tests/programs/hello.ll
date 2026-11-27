@message = private unnamed_addr constant [15 x i8] c"hello from bob\0A"

declare void @bobRuntimeWrite(i32, ptr, i64)

define i32 @bobMain() {
  call void @bobRuntimeWrite(i32 1, ptr @message, i64 15)
  ret i32 0
}
