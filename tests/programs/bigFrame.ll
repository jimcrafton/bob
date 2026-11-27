declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

define i32 @bobMain() {
  %a = alloca [16384 x i8], align 16
  %b = alloca [16384 x i8], align 16
  call void @llvm.memset.p0.i64(ptr %a, i8 7, i64 16384, i1 false)
  call void @llvm.memcpy.p0.p0.i64(ptr %b, ptr %a, i64 16384, i1 false)
  %p = getelementptr [16384 x i8], ptr %b, i64 0, i64 16383
  %v = load volatile i8, ptr %p
  %r = zext i8 %v to i32
  ret i32 %r
}
