target triple = "wasm32-unknown-emscripten"

@xyo_aot_slot_0 = global ptr null, align 4

declare void @xyo_var_set(ptr, ptr, ptr, i64)

define void @xyo_browser_script(ptr %ctx) {
entry:
  %id = load ptr, ptr @xyo_aot_slot_0, align 4
  call void @xyo_var_set(ptr %ctx, ptr %id, ptr null, i64 4636666922610458624)
  ret void
}
