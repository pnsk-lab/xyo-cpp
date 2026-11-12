; Minimal LLVM switched-resume stackless continuation smoke for xyo-cpp.
; Designed to prove that a loop PHI and its exact execution position survive
; scheduler-like suspension without a native Fiber or preserved OS stack.
;
; opt -S -passes='default<O2>' tests/platform/fiberless_llvm_smoke.ll -o /tmp/fiberless-opt.ll
; clang /tmp/fiberless-opt.ll -o /tmp/fiberless-smoke && /tmp/fiberless-smoke
;
; Program returns 0 only when the generated coroutine ran exactly
; 1, then 2, then 3 loop iterations at the three scheduler boundaries.
target triple = "x86_64-pc-linux-gnu"

@count = global i64 0

declare ptr @malloc(i64)
declare void @free(ptr)
declare token @llvm.coro.id(i32, ptr, ptr, ptr)
declare i64 @llvm.coro.size.i64()
declare ptr @llvm.coro.begin(token, ptr)
declare i8 @llvm.coro.suspend(token, i1)
declare ptr @llvm.coro.free(token, ptr)
declare void @llvm.coro.end(ptr, i1, token)
declare void @llvm.coro.resume(ptr)
declare i1 @llvm.coro.done(ptr)
declare void @llvm.coro.destroy(ptr)

define ptr @count_co() presplitcoroutine {
entry:
  %id = call token @llvm.coro.id(i32 0, ptr null, ptr null, ptr null)
  %size = call i64 @llvm.coro.size.i64()
  %allocation = call ptr @malloc(i64 %size)
  %hdl = call ptr @llvm.coro.begin(token %id, ptr %allocation)
  br label %body

body:
  %iteration = phi i64 [ 0, %entry ], [ %next, %resume ]
  %value = load i64, ptr @count
  %incremented = add i64 %value, 1
  store i64 %incremented, ptr @count
  %next = add i64 %iteration, 1
  %last = icmp eq i64 %next, 3
  br i1 %last, label %finish, label %yield

yield:
  %action = call i8 @llvm.coro.suspend(token none, i1 false)
  switch i8 %action, label %suspended [i8 0, label %resume
                                        i8 1, label %cleanup]

resume:
  br label %body

finish:
  %final_action = call i8 @llvm.coro.suspend(token none, i1 true)
  switch i8 %final_action, label %suspended [i8 1, label %cleanup]

cleanup:
  %storage = call ptr @llvm.coro.free(token %id, ptr %hdl)
  call void @free(ptr %storage)
  br label %suspended

suspended:
  call void @llvm.coro.end(ptr %hdl, i1 false, token none)
  ret ptr %hdl
}

define i32 @main() {
entry:
  %hdl = call ptr @count_co()
  %a = load i64, ptr @count
  call void @llvm.coro.resume(ptr %hdl)
  %b = load i64, ptr @count
  call void @llvm.coro.resume(ptr %hdl)
  %c = load i64, ptr @count
  %finished = call i1 @llvm.coro.done(ptr %hdl)
  call void @llvm.coro.destroy(ptr %hdl)
  %valid1 = icmp eq i64 %a, 1
  %valid2 = icmp eq i64 %b, 2
  %valid3 = icmp eq i64 %c, 3
  %valid12 = and i1 %valid1, %valid2
  %valid = and i1 %valid12, %valid3
  %both = and i1 %valid, %finished
  %rc = select i1 %both, i32 0, i32 1
  ret i32 %rc
}
