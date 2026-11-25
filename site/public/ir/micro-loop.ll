; ModuleID = 'xyo_script'
source_filename = "xyo_script"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

@u16 = private constant [6 x i16] [i16 116, i16 105, i16 109, i16 101, i16 115, i16 0]
@0 = private unnamed_addr constant [6 x i8] c"b0014\00", align 1
@u16.1 = private constant [3 x i16] [i16 116, i16 48, i16 0]
@1 = private unnamed_addr constant [6 x i8] c"b0018\00", align 1
@2 = private unnamed_addr constant [9 x i8] c"bench %s\00", align 1
@u16.2 = private constant [3 x i16] [i16 116, i16 49, i16 0]
@u16.3 = private constant [2 x i16] [i16 105, i16 0]
@u16.4 = private constant [6 x i16] [i16 99, i16 104, i16 101, i16 99, i16 107, i16 0]
@xyo_script_0_matpow_2 = private constant [2 x [2 x i64]] [[2 x i64] [i64 1, i64 1], [2 x i64] [i64 0, i64 1]]

define void @xyo_script_0(ptr %0) local_unnamed_addr {
  tail call void @xyo_varslot_init(ptr %0, i64 4, ptr nonnull inttoptr (i64 94293615893240 to ptr), ptr nonnull @u16.4, i32 0)
  tail call void @xyo_varslot_init(ptr %0, i64 3, ptr nonnull inttoptr (i64 94293615883432 to ptr), ptr nonnull @u16.3, i32 0)
  tail call void @xyo_varslot_init(ptr %0, i64 2, ptr nonnull inttoptr (i64 94293615903832 to ptr), ptr nonnull @u16.2, i32 0)
  tail call void @xyo_varslot_init(ptr %0, i64 1, ptr nonnull inttoptr (i64 94293615899832 to ptr), ptr nonnull @u16.1, i32 0)
  tail call void @xyo_varslot_init(ptr %0, i64 0, ptr nonnull inttoptr (i64 94293615895272 to ptr), ptr nonnull @u16, i32 1)
  %2 = getelementptr i8, ptr %0, i64 96
  %varslots = load ptr, ptr %2, align 8
  %varslot = load ptr, ptr %varslots, align 8
  %3 = getelementptr i8, ptr %varslots, i64 8
  %varslot4 = load ptr, ptr %3, align 8
  %4 = getelementptr i8, ptr %varslots, i64 24
  %varslot5 = load ptr, ptr %4, align 8
  %5 = getelementptr i8, ptr %varslots, i64 32
  %varslot6 = load ptr, ptr %5, align 8
  %6 = getelementptr i8, ptr %varslots, i64 16
  %varslot7 = load ptr, ptr %6, align 8
  %7 = getelementptr i8, ptr %varslot, i64 112
  %varlist = load ptr, ptr %7, align 8
  tail call void @xyo_list_deleteall(ptr %0, ptr %varslot)
  tail call void @xyo_proc_push(ptr %0, ptr nonnull @0, ptr nonnull inttoptr (i64 94293615954800 to ptr), i32 1, ptr nonnull @2, i32 0, i32 0)
  tail call void @xyo_script_0_p0(ptr %0, i64 9221541396141638016)
  tail call void @xyo_proc_pop(ptr %0)
  %8 = getelementptr i8, ptr %0, i64 68
  %9 = load i32, ptr %8, align 4
  %.not = icmp eq i32 %9, 0
  br i1 %.not, label %10, label %common.ret

common.ret:                                       ; preds = %31, %34, %1
  ret void

10:                                               ; preds = %1
  %11 = getelementptr i8, ptr %0, i64 104
  %12 = load i64, ptr %11, align 8
  %13 = getelementptr i8, ptr %0, i64 64
  %14 = load i32, ptr %13, align 8
  %15 = getelementptr i8, ptr %0, i64 232
  %16 = load i64, ptr %15, align 8
  %17 = tail call i64 @llvm.umin.i64(i64 %16, i64 256)
  %18 = add nsw i64 %17, -1
  %19 = getelementptr i8, ptr %varslot4, i64 104
  %20 = getelementptr i8, ptr %varslot7, i64 104
  %.not17 = icmp eq ptr %varlist, null
  %21 = getelementptr i8, ptr %varlist, i64 4
  %22 = getelementptr i8, ptr %varlist, i64 16
  %23 = icmp eq i32 %14, 0
  %24 = getelementptr i8, ptr %0, i64 248
  %25 = getelementptr i8, ptr %0, i64 256
  %26 = getelementptr i8, ptr %0, i64 240
  %27 = getelementptr i8, ptr %0, i64 128
  %28 = getelementptr i8, ptr %0, i64 136
  %29 = getelementptr i8, ptr %0, i64 32
  %30 = add i64 %12, 3
  br label %31

31:                                               ; preds = %10, %81
  %ycnt.022 = phi i64 [ %12, %10 ], [ %67, %81 ]
  %qmask.021 = phi i64 [ %18, %10 ], [ %qmask.1, %81 ]
  %rep_i32.020 = phi i32 [ 3, %10 ], [ %rep.i32.dec, %81 ]
  %32 = tail call double @xyo_sns_dayssince2000_f64(ptr nonnull %0)
  store double %32, ptr %19, align 8
  tail call void @xyo_proc_push(ptr nonnull %0, ptr nonnull @1, ptr nonnull inttoptr (i64 94293615999808 to ptr), i32 1, ptr nonnull @2, i32 1, i32 0)
  tail call void @xyo_script_0_p0(ptr nonnull %0, i64 9221541396141638112)
  tail call void @xyo_proc_pop(ptr nonnull %0)
  %33 = load i32, ptr %8, align 4
  %.not14 = icmp eq i32 %33, 0
  br i1 %.not14, label %39, label %common.ret

34:                                               ; preds = %81
  store i64 %30, ptr %11, align 8
  store i64 %16, ptr %15, align 8
  %35 = load i32, ptr %13, align 8
  store i32 1, ptr %13, align 8
  %36 = getelementptr i8, ptr %varslot5, i64 104
  %37 = load i64, ptr %36, align 8
  %38 = getelementptr i8, ptr %varslot6, i64 104
  store i64 %37, ptr %38, align 8
  store i32 %35, ptr %13, align 8
  br label %common.ret

39:                                               ; preds = %31
  %40 = tail call double @xyo_sns_dayssince2000_f64(ptr nonnull %0)
  %41 = bitcast double %40 to i64
  store double %40, ptr %20, align 8
  %42 = load i64, ptr %19, align 8
  %43 = icmp slt i64 %41, 9221120237041090560
  br i1 %43, label %profile.number, label %profile.other, !prof !0

profile.number:                                   ; preds = %39
  %.inv = fcmp ord double %40, 0.000000e+00
  %44 = select i1 %.inv, double %40, double 0.000000e+00
  br label %profile.done

profile.other:                                    ; preds = %39
  %45 = tail call double @xyo_to_num(i64 %41)
  br label %profile.done

profile.done:                                     ; preds = %profile.other, %profile.number
  %46 = phi double [ %44, %profile.number ], [ %45, %profile.other ]
  %47 = icmp slt i64 %42, 9221120237041090560
  br i1 %47, label %profile.number1, label %profile.other2, !prof !0

profile.number1:                                  ; preds = %profile.done
  %48 = bitcast i64 %42 to double
  %.inv15 = fcmp ord double %48, 0.000000e+00
  %49 = select i1 %.inv15, double %48, double 0.000000e+00
  br label %profile.done3

profile.other2:                                   ; preds = %profile.done
  %50 = tail call double @xyo_to_num(i64 %42)
  br label %profile.done3

profile.done3:                                    ; preds = %profile.other2, %profile.number1
  %51 = phi double [ %49, %profile.number1 ], [ %50, %profile.other2 ]
  %52 = fsub double %46, %51
  %.inv16 = fcmp ord double %52, 0.000000e+00
  %53 = fmul double %52, 8.640000e+07
  %54 = bitcast double %53 to i64
  %55 = select i1 %.inv16, i64 %54, i64 0
  br i1 %.not17, label %65, label %56

56:                                               ; preds = %profile.done3
  %57 = load i32, ptr %varlist, align 4
  %58 = load i32, ptr %21, align 4
  %59 = icmp ult i32 %57, %58
  br i1 %59, label %60, label %65

60:                                               ; preds = %56
  %61 = load ptr, ptr %22, align 8
  %62 = zext i32 %57 to i64
  %63 = getelementptr [8 x i8], ptr %61, i64 %62
  store i64 %55, ptr %63, align 8
  %64 = add nuw i32 %57, 1
  store i32 %64, ptr %varlist, align 8
  br label %66

65:                                               ; preds = %56, %profile.done3
  tail call void @xyo_list_add(ptr nonnull %0, ptr %varslot, i64 %55)
  br label %66

66:                                               ; preds = %65, %60
  %67 = add i64 %ycnt.022, 1
  br i1 %23, label %85, label %82

68:                                               ; preds = %82
  %69 = load i64, ptr %24, align 8
  %70 = inttoptr i64 %69 to ptr
  %71 = load atomic i32, ptr %70 monotonic, align 4
  %72 = tail call i32 @llvm.umin.i32(i32 %71, i32 256)
  %73 = zext nneg i32 %72 to i64
  %74 = add nsw i64 %73, -1
  store i64 %73, ptr %15, align 8
  %75 = load i64, ptr %25, align 8
  %76 = inttoptr i64 %75 to ptr
  %77 = load atomic i64, ptr %76 monotonic, align 8
  %78 = load i64, ptr %26, align 8
  %79 = icmp ne i64 %77, %78
  %80 = load ptr, ptr %27, align 8
  %.not18 = icmp eq ptr %80, null
  br i1 %.not18, label %warp.clock.fallback, label %warp.sampled.clock, !prof !1

81:                                               ; preds = %yield.handoff, %warp.clock.merge, %82, %85
  %qmask.1 = phi i64 [ %qmask.021, %85 ], [ %74, %yield.handoff ], [ %74, %warp.clock.merge ], [ %qmask.021, %82 ]
  %rep.i32.dec = add nsw i32 %rep_i32.020, -1
  %.not13 = icmp eq i32 %rep.i32.dec, 0
  br i1 %.not13, label %34, label %31

82:                                               ; preds = %66
  %83 = and i64 %67, %qmask.021
  %84 = icmp eq i64 %83, 0
  br i1 %84, label %68, label %81, !prof !2

85:                                               ; preds = %66
  %86 = load ptr, ptr %29, align 8
  tail call void @xyo_yield_fast(ptr %86)
  br label %81

warp.sampled.clock:                               ; preds = %68
  %87 = load atomic i64, ptr %80 monotonic, align 8
  %88 = load ptr, ptr %28, align 8
  %89 = load atomic i64, ptr %88 monotonic, align 8
  %90 = icmp sgt i64 %87, %89
  br label %warp.clock.merge

warp.clock.fallback:                              ; preds = %68
  %91 = tail call i32 @xyo_warp_yield_due(ptr nonnull %0)
  %92 = icmp ne i32 %91, 0
  br label %warp.clock.merge

warp.clock.merge:                                 ; preds = %warp.clock.fallback, %warp.sampled.clock
  %93 = phi i1 [ %90, %warp.sampled.clock ], [ %92, %warp.clock.fallback ]
  %94 = or i1 %79, %93
  br i1 %94, label %yield.handoff, label %81, !prof !3

yield.handoff:                                    ; preds = %warp.clock.merge
  tail call void @xyo_yield(ptr nonnull %0)
  store i64 %77, ptr %26, align 8
  br label %81
}

declare void @xyo_varslot_init(ptr, i64, ptr, ptr, i32) local_unnamed_addr

declare void @xyo_list_deleteall(ptr, ptr) local_unnamed_addr

define void @xyo_script_0_p0(ptr %0, i64 %1) local_unnamed_addr {
  %3 = getelementptr i8, ptr %0, i64 96
  %varslots = load ptr, ptr %3, align 8
  %4 = getelementptr i8, ptr %varslots, i64 24
  %varslot = load ptr, ptr %4, align 8
  %mp_wr = alloca i8, align 1
  %mp_post = alloca i32, align 4
  %mp_out = alloca double, align 8
  %mp_vars = alloca ptr, align 8
  %pv = alloca i64, align 8
  %5 = getelementptr i8, ptr %0, i64 64
  %6 = load i32, ptr %5, align 8
  store i32 1, ptr %5, align 8
  %7 = getelementptr i8, ptr %varslot, i64 104
  store i64 9221541396141779216, ptr %7, align 8
  %8 = icmp slt i64 %1, 9221120237041090561
  br i1 %8, label %9, label %12, !prof !4

9:                                                ; preds = %2
  %10 = bitcast i64 %1 to double
  %.inv31 = fcmp ord double %10, 0.000000e+00
  %11 = select i1 %.inv31, double %10, double 0.000000e+00
  br label %14

12:                                               ; preds = %2
  %.mask = and i64 %1, 9223090561878065152
  %13 = icmp eq i64 %.mask, 9221401712017801216
  br i1 %13, label %number.string, label %number.other, !prof !4

14:                                               ; preds = %number.other, %34, %39, %9
  %15 = phi double [ %11, %9 ], [ %spec.select, %number.other ], [ %38, %34 ], [ %40, %39 ]
  %16 = getelementptr i8, ptr %0, i64 104
  %17 = load i64, ptr %16, align 8
  %18 = load i32, ptr %5, align 8
  %19 = getelementptr i8, ptr %0, i64 232
  %20 = load i64, ptr %19, align 8
  %21 = load i64, ptr %7, align 8
  store i64 %21, ptr %pv, align 8
  %22 = icmp slt i64 %21, 9221120237041090561
  br i1 %22, label %41, label %44, !prof !4

number.other:                                     ; preds = %12
  %23 = icmp samesign ult i64 %1, 9221120237041090563
  %24 = icmp eq i64 %1, 9221120237041090562
  %25 = icmp eq i64 %1, 9221120237041090561
  %26 = bitcast i64 %1 to double
  %27 = select i1 %25, double 0.000000e+00, double %26
  %28 = select i1 %24, double 1.000000e+00, double %27
  %.inv = fcmp ord double %28, 0.000000e+00
  %29 = and i1 %23, %.inv
  %spec.select = select i1 %29, double %28, double 0.000000e+00
  br label %14

number.string:                                    ; preds = %12
  %30 = and i64 %1, 281474976710655
  %31 = inttoptr i64 %30 to ptr
  %32 = getelementptr i8, ptr %31, i64 16
  %33 = load atomic i32, ptr %32 acquire, align 4
  %.not = icmp eq i32 %33, 0
  br i1 %.not, label %39, label %34

34:                                               ; preds = %number.string
  %35 = getelementptr i8, ptr %31, i64 8
  %36 = load atomic double, ptr %35 monotonic, align 8
  %37 = icmp eq i32 %33, 2
  %38 = select i1 %37, double 0.000000e+00, double %36
  br label %14

39:                                               ; preds = %number.string
  %40 = tail call double @xyo_to_num(i64 %1)
  br label %14

41:                                               ; preds = %14
  %42 = bitcast i64 %21 to double
  %.inv35 = fcmp ord double %42, 0.000000e+00
  %43 = select i1 %.inv35, double %42, double 0.000000e+00
  br label %46

44:                                               ; preds = %14
  %.mask32 = and i64 %21, 9223090561878065152
  %45 = icmp eq i64 %.mask32, 9221401712017801216
  br i1 %45, label %number.string2, label %number.other1, !prof !4

46:                                               ; preds = %number.other1, %60, %65, %41
  %47 = phi double [ %43, %41 ], [ %spec.select11, %number.other1 ], [ %64, %60 ], [ %66, %65 ]
  %.mask36 = and i64 %21, -281474976710656
  %48 = icmp eq i64 %.mask36, 9221401712017801216
  br i1 %48, label %cmpok.string, label %cmpok.done

number.other1:                                    ; preds = %44
  %49 = icmp samesign ult i64 %21, 9221120237041090563
  %50 = icmp eq i64 %21, 9221120237041090562
  %51 = icmp eq i64 %21, 9221120237041090561
  %52 = bitcast i64 %21 to double
  %53 = select i1 %51, double 0.000000e+00, double %52
  %54 = select i1 %50, double 1.000000e+00, double %53
  %.inv33 = fcmp ord double %54, 0.000000e+00
  %55 = and i1 %49, %.inv33
  %spec.select11 = select i1 %55, double %54, double 0.000000e+00
  br label %46

number.string2:                                   ; preds = %44
  %56 = and i64 %21, 281474976710655
  %57 = inttoptr i64 %56 to ptr
  %58 = getelementptr i8, ptr %57, i64 16
  %59 = load atomic i32, ptr %58 acquire, align 4
  %.not34 = icmp eq i32 %59, 0
  br i1 %.not34, label %65, label %60

60:                                               ; preds = %number.string2
  %61 = getelementptr i8, ptr %57, i64 8
  %62 = load atomic double, ptr %61 monotonic, align 8
  %63 = icmp eq i32 %59, 2
  %64 = select i1 %63, double 0.000000e+00, double %62
  br label %46

65:                                               ; preds = %number.string2
  %66 = tail call double @xyo_to_num(i64 %21)
  br label %46

cmpok.string:                                     ; preds = %46
  %67 = and i64 %21, 281474976710655
  %68 = inttoptr i64 %67 to ptr
  %69 = getelementptr i8, ptr %68, i64 16
  %70 = load atomic i32, ptr %69 acquire, align 4
  br label %cmpok.done

cmpok.done:                                       ; preds = %cmpok.string, %46
  store ptr %pv, ptr %mp_vars, align 8
  %71 = load i32, ptr %5, align 8
  %.not37 = icmp eq i32 %71, 0
  br i1 %.not37, label %.preheader, label %125

.preheader:                                       ; preds = %cmpok.done, %125
  %72 = fcmp ult double %15, 5.000000e-01
  br i1 %72, label %.loopexit, label %.lr.ph

.lr.ph:                                           ; preds = %.preheader
  %73 = icmp eq i32 %18, 0
  %74 = getelementptr i8, ptr %0, i64 248
  %75 = getelementptr i8, ptr %0, i64 256
  %76 = getelementptr i8, ptr %0, i64 240
  %77 = getelementptr i8, ptr %0, i64 128
  %78 = getelementptr i8, ptr %0, i64 136
  %79 = getelementptr i8, ptr %0, i64 32
  br i1 %73, label %.lr.ph.split.us, label %.lr.ph.split

.lr.ph.split.us:                                  ; preds = %.lr.ph, %cmpok.done6.us
  %ycnt.051.us = phi i64 [ %81, %cmpok.done6.us ], [ %17, %.lr.ph ]
  %pvd.050.us = phi double [ %109, %cmpok.done6.us ], [ %47, %.lr.ph ]
  %rep_i.049.us = phi double [ %115, %cmpok.done6.us ], [ %15, %.lr.ph ]
  %80 = fadd double %pvd.050.us, 1.000000e+00
  store double %80, ptr %pv, align 8
  %81 = add i64 %ycnt.051.us, 1
  store double %80, ptr %7, align 8
  %82 = load ptr, ptr %79, align 8
  call void @xyo_yield_fast(ptr %82)
  %83 = load volatile i64, ptr %7, align 8
  store i64 %83, ptr %pv, align 8
  %84 = icmp slt i64 %83, 9221120237041090561
  br i1 %84, label %105, label %85, !prof !4

85:                                               ; preds = %.lr.ph.split.us
  %.mask44.us = and i64 %83, 9223090561878065152
  %86 = icmp eq i64 %.mask44.us, 9221401712017801216
  br i1 %86, label %number.string4.us, label %number.other3.us, !prof !4

number.other3.us:                                 ; preds = %85
  %87 = icmp samesign ult i64 %83, 9221120237041090563
  %88 = icmp eq i64 %83, 9221120237041090562
  %89 = icmp eq i64 %83, 9221120237041090561
  %90 = bitcast i64 %83 to double
  %91 = select i1 %89, double 0.000000e+00, double %90
  %92 = select i1 %88, double 1.000000e+00, double %91
  %.inv45.us = fcmp ord double %92, 0.000000e+00
  %93 = and i1 %87, %.inv45.us
  %spec.select12.us = select i1 %93, double %92, double 0.000000e+00
  br label %108

number.string4.us:                                ; preds = %85
  %94 = and i64 %83, 281474976710655
  %95 = inttoptr i64 %94 to ptr
  %96 = getelementptr i8, ptr %95, i64 16
  %97 = load atomic i32, ptr %96 acquire, align 4
  %.not46.us = icmp eq i32 %97, 0
  br i1 %.not46.us, label %103, label %98

98:                                               ; preds = %number.string4.us
  %99 = getelementptr i8, ptr %95, i64 8
  %100 = load atomic double, ptr %99 monotonic, align 8
  %101 = icmp eq i32 %97, 2
  %102 = select i1 %101, double 0.000000e+00, double %100
  br label %108

103:                                              ; preds = %number.string4.us
  %104 = call double @xyo_to_num(i64 %83)
  br label %108

105:                                              ; preds = %.lr.ph.split.us
  %106 = bitcast i64 %83 to double
  %.inv47.us = fcmp ord double %106, 0.000000e+00
  %107 = select i1 %.inv47.us, double %106, double 0.000000e+00
  br label %108

108:                                              ; preds = %105, %103, %98, %number.other3.us
  %109 = phi double [ %107, %105 ], [ %spec.select12.us, %number.other3.us ], [ %102, %98 ], [ %104, %103 ]
  %.mask48.us = and i64 %83, -281474976710656
  %110 = icmp eq i64 %.mask48.us, 9221401712017801216
  br i1 %110, label %cmpok.string5.us, label %cmpok.done6.us

cmpok.string5.us:                                 ; preds = %108
  %111 = and i64 %83, 281474976710655
  %112 = inttoptr i64 %111 to ptr
  %113 = getelementptr i8, ptr %112, i64 16
  %114 = load atomic i32, ptr %113 acquire, align 4
  br label %cmpok.done6.us

cmpok.done6.us:                                   ; preds = %cmpok.string5.us, %108
  %115 = fadd double %rep_i.049.us, -1.000000e+00
  %116 = fcmp ult double %115, 5.000000e-01
  br i1 %116, label %.loopexit, label %.lr.ph.split.us

.lr.ph.split:                                     ; preds = %.lr.ph, %cmpok.done6
  %ycnt.051 = phi i64 [ %119, %cmpok.done6 ], [ %17, %.lr.ph ]
  %pvd.050 = phi double [ %pvd.1, %cmpok.done6 ], [ %47, %.lr.ph ]
  %rep_i.049 = phi double [ %144, %cmpok.done6 ], [ %15, %.lr.ph ]
  %117 = load i64, ptr %19, align 8
  %118 = fadd double %pvd.050, 1.000000e+00
  store double %118, ptr %pv, align 8
  %119 = add i64 %ycnt.051, 1
  %120 = call i64 @llvm.umin.i64(i64 %117, i64 16384)
  %121 = add nsw i64 %120, -1
  %122 = and i64 %121, %119
  %123 = icmp eq i64 %122, 0
  br i1 %123, label %132, label %cmpok.done6, !prof !2

.loopexit:                                        ; preds = %cmpok.done6, %cmpok.done6.us, %.preheader, %127, %125
  %ycnt.1 = phi i64 [ %17, %125 ], [ %17, %127 ], [ %17, %.preheader ], [ %81, %cmpok.done6.us ], [ %119, %cmpok.done6 ]
  store i64 %ycnt.1, ptr %16, align 8
  store i64 %20, ptr %19, align 8
  %124 = load i64, ptr %pv, align 8
  store i64 %124, ptr %7, align 8
  store i32 %6, ptr %5, align 8
  ret void

125:                                              ; preds = %cmpok.done
  store i32 -1, ptr %mp_post, align 4
  store i8 1, ptr %mp_wr, align 1
  %126 = call i32 @xyo_matpow(ptr nonnull %0, double %15, ptr nonnull %mp_vars, i64 1, ptr nonnull @xyo_script_0_matpow_2, i64 2, i64 2, ptr nonnull %mp_out, ptr nonnull %mp_post, ptr nonnull %mp_wr, i32 0, i32 0, i32 0, ptr null)
  switch i32 %126, label %.preheader [
    i32 1, label %127
    i32 2, label %.loopexit
  ]

127:                                              ; preds = %125
  %128 = load double, ptr %mp_out, align 8
  %129 = fcmp uno double %128, 0.000000e+00
  %130 = bitcast double %128 to i64
  %131 = select i1 %129, i64 9221120237041090560, i64 %130
  store i64 %131, ptr %pv, align 8
  br label %.loopexit

132:                                              ; preds = %.lr.ph.split
  %133 = load i64, ptr %74, align 8
  %134 = inttoptr i64 %133 to ptr
  %135 = load atomic i32, ptr %134 monotonic, align 4
  %136 = call i32 @llvm.umin.i32(i32 %135, i32 16384)
  %137 = zext nneg i32 %136 to i64
  store i64 %137, ptr %19, align 8
  %138 = load i64, ptr %75, align 8
  %139 = inttoptr i64 %138 to ptr
  %140 = load atomic i64, ptr %139 monotonic, align 8
  %141 = load i64, ptr %76, align 8
  %142 = icmp ne i64 %140, %141
  %143 = load ptr, ptr %77, align 8
  %.not38 = icmp eq ptr %143, null
  br i1 %.not38, label %warp.clock.fallback, label %warp.sampled.clock, !prof !1

cmpok.done6:                                      ; preds = %cmpok.done10, %warp.clock.merge, %.lr.ph.split
  %pvd.1 = phi double [ %118, %.lr.ph.split ], [ %163, %cmpok.done10 ], [ %118, %warp.clock.merge ]
  %144 = fadd double %rep_i.049, -1.000000e+00
  %145 = fcmp ult double %144, 5.000000e-01
  br i1 %145, label %.loopexit, label %.lr.ph.split

warp.sampled.clock:                               ; preds = %132
  %146 = load atomic i64, ptr %143 monotonic, align 8
  %147 = load ptr, ptr %78, align 8
  %148 = load atomic i64, ptr %147 monotonic, align 8
  %149 = icmp sgt i64 %146, %148
  br label %warp.clock.merge

warp.clock.fallback:                              ; preds = %132
  %150 = call i32 @xyo_warp_yield_due(ptr nonnull %0)
  %151 = icmp ne i32 %150, 0
  br label %warp.clock.merge

warp.clock.merge:                                 ; preds = %warp.clock.fallback, %warp.sampled.clock
  %152 = phi i1 [ %149, %warp.sampled.clock ], [ %151, %warp.clock.fallback ]
  %153 = or i1 %142, %152
  br i1 %153, label %yield.handoff, label %cmpok.done6, !prof !3

yield.handoff:                                    ; preds = %warp.clock.merge
  %154 = load i64, ptr %pv, align 8
  store i64 %154, ptr %7, align 8
  call void @xyo_yield(ptr nonnull %0)
  %155 = load volatile i64, ptr %7, align 8
  store i64 %155, ptr %pv, align 8
  %156 = icmp slt i64 %155, 9221120237041090561
  br i1 %156, label %157, label %160, !prof !4

157:                                              ; preds = %yield.handoff
  %158 = bitcast i64 %155 to double
  %.inv42 = fcmp ord double %158, 0.000000e+00
  %159 = select i1 %.inv42, double %158, double 0.000000e+00
  br label %162

160:                                              ; preds = %yield.handoff
  %.mask39 = and i64 %155, 9223090561878065152
  %161 = icmp eq i64 %.mask39, 9221401712017801216
  br i1 %161, label %number.string8, label %number.other7, !prof !4

162:                                              ; preds = %number.other7, %176, %181, %157
  %163 = phi double [ %159, %157 ], [ %spec.select13, %number.other7 ], [ %180, %176 ], [ %182, %181 ]
  %.mask43 = and i64 %155, -281474976710656
  %164 = icmp eq i64 %.mask43, 9221401712017801216
  br i1 %164, label %cmpok.string9, label %cmpok.done10

number.other7:                                    ; preds = %160
  %165 = icmp samesign ult i64 %155, 9221120237041090563
  %166 = icmp eq i64 %155, 9221120237041090562
  %167 = icmp eq i64 %155, 9221120237041090561
  %168 = bitcast i64 %155 to double
  %169 = select i1 %167, double 0.000000e+00, double %168
  %170 = select i1 %166, double 1.000000e+00, double %169
  %.inv40 = fcmp ord double %170, 0.000000e+00
  %171 = and i1 %165, %.inv40
  %spec.select13 = select i1 %171, double %170, double 0.000000e+00
  br label %162

number.string8:                                   ; preds = %160
  %172 = and i64 %155, 281474976710655
  %173 = inttoptr i64 %172 to ptr
  %174 = getelementptr i8, ptr %173, i64 16
  %175 = load atomic i32, ptr %174 acquire, align 4
  %.not41 = icmp eq i32 %175, 0
  br i1 %.not41, label %181, label %176

176:                                              ; preds = %number.string8
  %177 = getelementptr i8, ptr %173, i64 8
  %178 = load atomic double, ptr %177 monotonic, align 8
  %179 = icmp eq i32 %175, 2
  %180 = select i1 %179, double 0.000000e+00, double %178
  br label %162

181:                                              ; preds = %number.string8
  %182 = call double @xyo_to_num(i64 %155)
  br label %162

cmpok.string9:                                    ; preds = %162
  %183 = and i64 %155, 281474976710655
  %184 = inttoptr i64 %183 to ptr
  %185 = getelementptr i8, ptr %184, i64 16
  %186 = load atomic i32, ptr %185 acquire, align 4
  br label %cmpok.done10

cmpok.done10:                                     ; preds = %cmpok.string9, %162
  store i64 %140, ptr %76, align 8
  br label %cmpok.done6
}

declare void @xyo_proc_push(ptr, ptr, ptr, i32, ptr, i32, i32) local_unnamed_addr

declare void @xyo_proc_pop(ptr) local_unnamed_addr

declare double @xyo_sns_dayssince2000_f64(ptr) local_unnamed_addr

declare double @xyo_to_num(i64) local_unnamed_addr

declare void @xyo_list_add(ptr, ptr, i64) local_unnamed_addr

declare void @xyo_yield_fast(ptr) local_unnamed_addr

declare i32 @xyo_warp_yield_due(ptr) local_unnamed_addr

declare void @xyo_yield(ptr) local_unnamed_addr

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(readwrite, inaccessiblemem: none, target_mem: none)
define void @xyo_script_0_p1(ptr nofree captures(none) %0) local_unnamed_addr #0 {
  %2 = getelementptr i8, ptr %0, i64 96
  %varslots = load ptr, ptr %2, align 8
  %3 = getelementptr i8, ptr %varslots, i64 24
  %varslot = load ptr, ptr %3, align 8
  %4 = getelementptr i8, ptr %varslots, i64 32
  %varslot1 = load ptr, ptr %4, align 8
  %5 = getelementptr i8, ptr %0, i64 64
  %6 = load i32, ptr %5, align 8
  store i32 1, ptr %5, align 8
  %7 = getelementptr i8, ptr %varslot, i64 104
  %8 = load i64, ptr %7, align 8
  %9 = getelementptr i8, ptr %varslot1, i64 104
  store i64 %8, ptr %9, align 8
  store i32 %6, ptr %5, align 8
  ret void
}

declare i32 @xyo_matpow(ptr, double, ptr, i64, ptr, i64, i64, ptr, ptr, ptr, i32, i32, i32, ptr) local_unnamed_addr

; Function Attrs: nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare i64 @llvm.umin.i64(i64, i64) #1

; Function Attrs: nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare i32 @llvm.umin.i32(i32, i32) #1

attributes #0 = { mustprogress nofree norecurse nosync nounwind willreturn memory(readwrite, inaccessiblemem: none, target_mem: none) }
attributes #1 = { nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none) }

!0 = !{!"branch_weights", i32 200, i32 1}
!1 = !{!"branch_weights", i32 1, i32 1000}
!2 = !{!"branch_weights", i32 1, i32 63}
!3 = !{!"branch_weights", i32 1, i32 100000}
!4 = !{!"branch_weights", i32 1000, i32 1}
