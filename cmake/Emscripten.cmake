set(WEB_RUNTIME_C_SOURCES
    ${RUNTIME_C_SOURCES}
    ${QUICKJS_SUBSET_C_SOURCES}
)

# The native scheduler can use a persistent pthread pool.  Web builds run the
# same scheduler sequentially on the browser thread so that the generated
# module also works without cross-origin isolation headers.
list(REMOVE_ITEM WEB_RUNTIME_C_SOURCES runtime/sjit_thread_pool.c)
list(APPEND WEB_RUNTIME_C_SOURCES runtime/sjit_thread_pool_web.c)

add_library(sjit_runtime_web STATIC ${WEB_RUNTIME_C_SOURCES})
target_include_directories(sjit_runtime_web PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/quickjs
    ${CMAKE_CURRENT_SOURCE_DIR}/quickjs_subset
)
target_compile_definitions(sjit_runtime_web PUBLIC
    SJIT_WEB=1
    _POSIX_C_SOURCE=200809L
)

add_executable(xyo-web
    src/project_loader.cpp
    src/web_jit_stub.cpp
    src/web_main.cpp
)
target_include_directories(xyo-web PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/quickjs
    ${CMAKE_CURRENT_SOURCE_DIR}/quickjs_subset
)
target_compile_definitions(xyo-web PRIVATE SJIT_WEB=1)
target_compile_options(xyo-web PRIVATE
    -fexceptions
    -sUSE_ZLIB=1
)
target_link_libraries(xyo-web PRIVATE sjit_runtime_web)
target_link_options(xyo-web PRIVATE
    --no-entry
    -fexceptions
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sEXPORT_NAME=XyoModule
    -sENVIRONMENT=web
    -sFORCE_FILESYSTEM=1
    -sUSE_ZLIB=1
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=33554432
    -sMAXIMUM_MEMORY=536870912
    -sNO_EXIT_RUNTIME=1
    "-sEXPORTED_FUNCTIONS=['_malloc','_free','_sjit_web_load_project_bytes','_sjit_web_last_error','_sjit_web_start','_sjit_web_stop','_sjit_web_tick','_sjit_web_set_mouse','_sjit_web_set_key','_sjit_web_blur','_sjit_web_draw_count','_sjit_web_draw_command','_sjit_web_pen_count','_sjit_web_pen_command','_sjit_web_target_count','_sjit_web_target_id','_sjit_web_target_is_stage','_sjit_web_target_name','_sjit_web_render_target_id','_sjit_web_current_costume','_sjit_web_costume_count','_sjit_web_costume_format','_sjit_web_costume_data','_sjit_web_costume_data_size','_sjit_web_costume_width','_sjit_web_costume_height','_sjit_web_costume_rotation_center_x','_sjit_web_costume_rotation_center_y','_sjit_web_answer_pending','_sjit_web_question','_sjit_web_answer','_sjit_web_bubble_text','_sjit_web_bubble_thought']"
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap']"
)

set_target_properties(xyo-web PROPERTIES
    OUTPUT_NAME "xyo-web"
)
