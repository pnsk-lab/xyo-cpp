// xyo-cpp: LLJIT 基盤 — design.md §7.6。
#include "jit.hpp"
#include "backend.hpp"

#include "llvm_compat.hpp"

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/IRTransformLayer.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/ObjectTransformLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassInstrumentation.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Passes/PassBuilder.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <unordered_set>
#include <vector>

#include "../blocks/pen_api.hpp"
#include "../core/cast.hpp"
#include "../core/gc.hpp"
#include "../core/json.hpp"
#include "../core/jsnum.hpp"
#include "../core/v8math.hpp"
#include "../core/sstr.hpp"
#include "../engine/runtime.hpp"
#include "../engine/sequencer.hpp"
#include "../engine/thread.hpp"
#include "../model/blocks.hpp"
#include "../model/target.hpp"
#include "../ir/cfg_lower_llvm.hpp"
#include "../ir/scratch_f64_kernel.hpp"
#include "../ir/scratch_pipeline.hpp"
#include "../ir/commit_once_effects.hpp"
#include "../ir/source_effect_dispatch.hpp"
#include "../ir/source_effect_runtime_trace.hpp"
#include "../ir/llvm_effect_write_audit.hpp"
#include "../ir/rt_postopt_ssa_audit.hpp"
#include "../ir/rt_yield_edge_stackmap.hpp"
#include "../ir/rt_yield_ssa_machine_stackmap.hpp"
#include <llvm/IR/Verifier.h>
#include "aot_emit.hpp"
#include "nested_warp_osr.hpp"
#include "procedure_frame_state.hpp"
#include "stackless_coro_lower.hpp"
#include "mega_goto.hpp"
#include "mega_partial.hpp"
#include "mega_yield_source.hpp"
#include "guard_fallback_instrument.hpp"
#ifdef XYO_RT001_ORC_FDE_PLUGIN
#include "rt_orc_fde_lifecycle.hpp"
#endif

namespace xyo {

// OSR-010. A nested native body is recoverable at its enclosing repeat
// boundary only if every INNER repeat is short, statically bounded and
// incapable of yielding on its own. This is a *restricted* alternative to
// the flat-only OSR-008 eligibility check; there is no general LLVM
// nested-loop / SSA-PC deoptimization.
//
// Keep this separate from the generic safeNestedLoopBody whitelist: this
// controls the much stronger property "there is no scheduler suspension
// between the beginning and end of a source outer-loop body".
// OSR-012: accept only a finite, acyclic, side-effect-free reporter tree
// with at least one real variable reference. The source proof validates
// reporter *shape*, not the mutable runtime value. LLVM's private counter
// remains authoritative even if the variable changes inside the body.
static bool safeDynamicInnerTimes(const Blocks& blocks,
                                  const std::string& root) {
    std::unordered_set<std::string> seen;
    bool hasVariable = false;
    const auto visit = [&](auto&& self, const std::string& id,
                           uint32_t depth) -> bool {
        if (id.empty() || depth > 4 || seen.size() >= 12 ||
            !seen.insert(id).second) return false;
        const Block* node = blocks.getBlock(id);
        if (!node || !node->next.empty()) return false;
        const std::string& opcode = node->opcode;
        if (opcode == "data_variable") {
            if (!node->inputs.items.empty()) return false;
            const auto field = node->fields.find("VARIABLE");
            if (field == node->fields.end() ||
                field->second.id.empty()) return false;
            hasVariable = true;
            return true;
        }
        if (opcode == "math_number" || opcode == "math_integer" ||
            opcode == "math_whole_number" ||
            opcode == "math_positive_number") {
            if (!node->inputs.items.empty()) return false;
            const auto field = node->fields.find("NUM");
            return field != node->fields.end() &&
                   std::isfinite(cast::toNumber(field->second.value));
        }
        if (opcode != "operator_add" &&
            opcode != "operator_subtract") return false;
        if (node->inputs.items.size() != 2) return false;
        for (const char* name : {"NUM1", "NUM2"}) {
            const BlockInput* operand = node->inputs.find(name);
            if (!operand) return false;
            const std::string& effective = operand->block.empty()
                ? operand->shadow : operand->block;
            if (!self(self, effective, depth + 1)) return false;
        }
        return true;
    };
    return visit(visit, root, 0) && hasVariable;
}

// Source-proven OSR-010..014 continuation: exactly one nested counted
// repeat (or exactly two/three with the explicit deeper-yield opt-ins). Dynamic
// trip-count reporters are deliberately limited to pure variable +/-
// finite numbers. Never infer the live iteration count from the source:
// the native private counter remains the only authority after execution.
static bool safeBoundedNativeInnerRepeat(
    const Blocks& blocks, const Block& outer, bool allowInnerYield,
    bool allowDynamicTimes, uint32_t maxNested,
    std::string* innerId = nullptr, std::string* deepestId = nullptr,
    std::string* fourthId = nullptr) {
    if (outer.opcode != "control_repeat") return false;
    const auto* sub = outer.inputs.find("SUBSTACK");
    if (!sub || sub->block.empty()) return false;
    std::unordered_set<std::string> seen;
    size_t total = 0;
    size_t nestedLoops = 0;
    const auto checkTimes = [&](const Block* node) -> bool {
        const auto* times = node->inputs.find("TIMES");
        const auto* body = node->inputs.find("SUBSTACK");
        if (!times || !body || body->block.empty()) return false;
        const std::string& id =
            times->block.empty() ? times->shadow : times->block;
        const Block* n = blocks.getBlock(id);
        if (!n || !n->next.empty()) return false;
        const bool literal =
            n->opcode == "math_number" ||
            n->opcode == "math_integer" ||
            n->opcode == "math_whole_number" ||
            n->opcode == "math_positive_number";
        if (literal) {
            const auto field = n->fields.find("NUM");
            if (field == n->fields.end()) return false;
            const double v = cast::toNumber(field->second.value);
            return std::isfinite(v) && v >= 1.0 &&
                   v <= (allowInnerYield ? 8192.0 : 8.0) &&
                   std::floor(v) == v;
        }
        return allowInnerYield && allowDynamicTimes &&
               safeDynamicInnerTimes(blocks, id);
    };
    const auto scanChain = [&](auto&& self, std::string id,
                               size_t depth) -> bool {
        size_t inChain = 0;
        while (!id.empty()) {
            if (++total > 24 || ++inChain > 16 ||
                !seen.insert(id).second) return false;
            const Block* node = blocks.getBlock(id);
            if (!node) return false;
            if (node->opcode == "control_repeat") {
                if (depth >= maxNested ||
                    nestedLoops != depth || !checkTimes(node)) return false;
                ++nestedLoops;
                if (depth == 0 && innerId) *innerId = node->id;
                if (depth == 1 && deepestId) *deepestId = node->id;
                if (depth == 2 && fourthId) *fourthId = node->id;
                const auto* body = node->inputs.find("SUBSTACK");
                if (!body || !self(self, body->block, depth+1))
                    return false;
            } else if (node->opcode != "data_changevariableby" &&
                       !(node->opcode == "data_setvariableto" &&
                         (depth == 0 || allowDynamicTimes))) {
                return false;
            }
            // OSR-010's fixed bounded inner repeat still rejects setter
            // bodies, and every nested body remains at most 4 primitive
            // commands. OSR-012/013 allow setters only behind DYNAMIC.
            // Every completed body is committed at its actual yield.
            if (depth > 0 && inChain > 4) return false;
            id = node->next;
        }
        return true;
    };
    return scanChain(scanChain, sub->block, 0) &&
           nestedLoops == maxNested;
}

bool jitEmitScript(llvm::Module&, llvm::LLVMContext&, Runtime&, Target*,
                   Blocks*, const std::string&, const std::string&,
                   CompiledScript&, JitPinned&,
                   std::vector<std::string>&,
                   std::map<std::string, std::string>&, AotEmit* = nullptr,
                   const Block* osrStart = nullptr,
                   const ir::ScratchCompilationUnit* source = nullptr,
                   bool standaloneOsrLoop = false);

// 生成コードが参照する extern "C" ヘルパ（rt.cpp 定義）
extern "C" {
void xyo_yield(JitCtx*);
void xyo_yield_fast(xyo::Fiber*);
void xyo_yield_fast_direct(xyo::Fiber*);
int32_t xyo_is_stuck(JitCtx*);
int32_t xyo_wait_should_yield(JitCtx*);
void xyo_wait_yield(JitCtx*);
void xyo_wait_event_yield(JitCtx*, uint64_t);
void xyo_wait_event_done(JitCtx*);
void xyo_loop_yield(JitCtx*);
int32_t xyo_warp_yield_due(JitCtx*);
int32_t xyo_rt_gc_soft_budget_due(JitCtx*);
void xyo_empty_repeat_warp(JitCtx*, double);
uint64_t xyo_gc_epoch();
double xyo_to_num(uint64_t);
double xyo_to_num_uncached(SStr*);
int32_t xyo_truthy(uint64_t);
uint64_t xyo_num(double);
uint64_t xyo_undefined();
uint64_t xyo_to_str(uint64_t);
uint64_t xyo_add(uint64_t, uint64_t);
uint64_t xyo_sub(uint64_t, uint64_t);
uint64_t xyo_mul(uint64_t, uint64_t);
uint64_t xyo_div(uint64_t, uint64_t);
uint64_t xyo_mod(uint64_t, uint64_t);
double xyo_log(double);
double xyo_asin(double);
double xyo_acos(double);
double xyo_atan(double);
double xyo_exp(double);
int32_t xyo_lt(uint64_t, uint64_t);
int32_t xyo_gt(uint64_t, uint64_t);
int32_t xyo_eq(uint64_t, uint64_t);
int32_t xyo_not(uint64_t);
int32_t xyo_and(uint64_t, uint64_t);
int32_t xyo_or(uint64_t, uint64_t);
uint64_t xyo_random(JitCtx*, uint64_t, uint64_t);
double xyo_mathop(JitCtx*, int32_t, double);
double xyo_math_abs(JitCtx*, double);
double xyo_math_floor(JitCtx*, double);
double xyo_math_ceil(JitCtx*, double);
double xyo_math_sqrt(JitCtx*, double);
double xyo_math_sin(JitCtx*, double);
double xyo_math_cos(JitCtx*, double);
double xyo_math_tan(JitCtx*, double);
double xyo_math_asin(JitCtx*, double);
double xyo_math_acos(JitCtx*, double);
double xyo_math_atan(JitCtx*, double);
double xyo_math_ln(JitCtx*, double);
double xyo_math_log(JitCtx*, double);
double xyo_math_exp(JitCtx*, double);
double xyo_math_pow10(JitCtx*, double);
int32_t xyo_letter_eq(uint64_t, uint64_t, int32_t);
int32_t xyo_letter_eq_i(int32_t, uint64_t, int32_t);
double xyo_round(double);
uint64_t xyo_var_get(JitCtx*, const char*, const char16_t*);
void xyo_var_set(JitCtx*, const char*, const char16_t*, uint64_t);
void xyo_wait_notify_var_store(JitCtx*, Variable*);
void xyo_var_change(JitCtx*, const char*, const char16_t*, uint64_t);
uint64_t xyo_arg_get(JitCtx*, uint64_t);
void xyo_rt_wait(JitCtx*, double);
void xyo_wait_begin(JitCtx*, double);
int32_t xyo_wait_poll(JitCtx*);
void xyo_stop_all(JitCtx*);
void xyo_stop_script_begin(JitCtx*);
void xyo_stop_all_begin(JitCtx*);
void xyo_stop_others(JitCtx*);
void xyo_stop_script(JitCtx*);
void xyo_broadcast_v(JitCtx*, uint64_t);
void xyo_ir001_broadcast_v_source(
    JitCtx*, uint64_t, const char*, uint64_t, uint64_t, uint64_t, uint64_t);
void xyo_ir001_pen_stamp_source(
    JitCtx*, const char*, uint64_t, uint64_t, uint64_t, uint64_t);
void xyo_ir001_effect_checkpoint(
    JitCtx*, const char*, uint64_t, uint64_t, uint64_t, uint64_t,
    uint32_t);
void xyo_ir001_vm_write(JitCtx*);
void xyo_broadcast_wait_v(JitCtx*, uint64_t);
void xyo_broadcast_wait_lit(JitCtx*, const char16_t*, uint32_t);
void xyo_bcastwait_begin_v(JitCtx*, uint64_t);
void xyo_bcastwait_begin_lit(JitCtx*, const char16_t*, uint32_t);
int32_t xyo_bcastwait_step(JitCtx*);
double xyo_timer(JitCtx*);
uint64_t xyo_rt_compat(JitCtx*, const void*);
const void* xyo_rt_aot_cached(void*);
uint64_t xyo_rt_reporter(JitCtx*, const char*);
uint64_t xyo_rt_async_reporter(JitCtx*, const char*);
uint64_t xyo_rt_async_reporter_rooted(
    JitCtx*, const char*, const uint64_t*, uint32_t);
uint64_t xyo_rt_nested_osr_arg_get(JitCtx*, const char*,
                                    const char16_t*, uint32_t);
uint64_t xyo_rt_nested_osr_arg_peek(JitCtx*, const char16_t*, uint32_t);
void xyo_rt_nested_osr_yield_checkpoint(JitCtx*, double);
void xyo_rt_nested_osr_inner_yield_checkpoint(JitCtx*, double, double);
void xyo_rt_nested_osr_triple_yield_checkpoint(
    JitCtx*, double, double, double);
void xyo_rt_nested_osr_quad_yield_checkpoint(
    JitCtx*, double, double, double, double);
void xyo_proc_push(JitCtx*, const char*, const void*, int32_t,
                   const char*, int32_t, int32_t);
int32_t xyo_proc_push_ex(JitCtx*, const char*, const void*, int32_t,
                         const char*, int32_t, int32_t, int32_t);
int32_t xyo_recursion_yield_due(JitCtx*);
void xyo_coro_push_frame(JitCtx*, void*, uint64_t);
void* xyo_coro_top_frame(JitCtx*);
void xyo_coro_pop_frame(JitCtx*, void*);
void xyo_proc_pop(JitCtx*);
void xyo_proc_frame_init_root_verified(
    JitCtx*, uint64_t, uint64_t, uint64_t);
void xyo_ir001_list_content_write_begin();
void xyo_ir001_list_content_write_end();
void xyo_ir001_scalar_read_eval(
    JitCtx*, uint64_t, const char*, uint64_t);
uint64_t xyo_ir001_list_read_epoch();
void xyo_ir001_list_read_eval(
    JitCtx*, uint64_t, const char*, uint64_t, uint64_t, uint64_t);
void xyo_rt_capture_llvm_ssa(
    JitCtx*, const uint64_t*, const uint64_t*,
    uint32_t, uint32_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
    uint32_t, uint32_t, const uint32_t*, uint32_t, uint32_t,
    const uint32_t*, const uint32_t*);
void xyo_proc_frame_enter(JitCtx*, const char*, const char*,
                          const uint64_t*, uint32_t);
void xyo_proc_frame_enter_rev(JitCtx*, const char*, const char*,
                              const uint64_t*, uint32_t, uint64_t);
void xyo_proc_frame_enter_verified(JitCtx*, const char*, const char*,
                                   const uint64_t*, uint32_t, uint64_t,
                                   uint64_t, uint64_t);
void xyo_proc_frame_enter_verified_rootpath(
    JitCtx*, const char*, const char*, const uint64_t*, uint32_t,
    uint64_t, uint64_t, uint64_t, const char* const*,
    const uint64_t*, uint32_t);
void xyo_proc_frame_leave(JitCtx*);
void xyo_proc_frame_body_started(JitCtx*);
void xyo_proc_frame_statement_started(JitCtx*, const char*);
void xyo_proc_frame_checkpoint(JitCtx*, const char*, uint64_t);
void xyo_proc_frame_checkpoint_path(JitCtx*, const char* const*,
                                    const uint64_t*, uint32_t);
void xyo_proc_frame_checkpoint_path_phase(JitCtx*, const char* const*,
                                          const uint64_t*, uint32_t, uint32_t);
void xyo_rt_setarg(const void*, const char*, uint64_t);

// --- rt_blocks.cpp: ブロック直接ヘルパ ---
uint64_t xyo_join(uint64_t, uint64_t);
uint64_t xyo_join_suffix(uint64_t, uint64_t);
void* xyo_transient_begin(uint64_t, uint64_t*);
void xyo_transient_append(void*, uint64_t);
void xyo_transient_append_value(void*, uint64_t);
void xyo_transient_append_packed(void*, uint64_t, uint32_t);
void xyo_transient_append_repeat_packed(void*, uint64_t, uint64_t);
void xyo_transient_append_repeat_string(void*, uint64_t, uint32_t);
void xyo_transient_append_repeat_value(void*, uint64_t, uint32_t);
void xyo_transient_flush(void*, uint64_t*);
void xyo_transient_finish(void*, uint64_t*);
void* xyo_transient_resume(void*, uint64_t*);

#if defined(XYO_STRING_JOIN_SPECIALIZED)
uint64_t xyo_join_strings(uint64_t, uint64_t);
#endif
uint64_t xyo_letterof(uint64_t, uint64_t);
uint64_t xyo_strlen(uint64_t);
uint64_t xyo_strcontains(uint64_t, uint64_t);
void xyo_list_add(JitCtx*, const char*, const char16_t*, uint64_t);
void xyo_list_delete(JitCtx*, const char*, const char16_t*, uint64_t);
void xyo_list_deleteall(JitCtx*, const char*, const char16_t*);
void xyo_list_insert(JitCtx*, const char*, const char16_t*, uint64_t, uint64_t);
void xyo_list_replace(JitCtx*, const char*, const char16_t*, uint64_t, uint64_t);
uint64_t xyo_list_item(JitCtx*, const char*, const char16_t*, uint64_t);
uint64_t xyo_list_itemnum(JitCtx*, const char*, const char16_t*, uint64_t);
uint64_t xyo_list_length(JitCtx*, const char*, const char16_t*);
uint64_t xyo_list_contains(JitCtx*, const char*, const char16_t*, uint64_t);
void xyo_mega_bench_end(JitCtx*);
uint64_t xyo_list_contents(JitCtx*, const char*, const char16_t*);
void xyo_var_monitor(JitCtx*, const char*, int32_t);
void xyo_mot_movesteps(JitCtx*, uint64_t);
void xyo_mot_gotoxy(JitCtx*, uint64_t, uint64_t);
void xyo_mot_setxy_num(JitCtx*, double, double);
void xyo_mot_goto(JitCtx*, uint64_t);
void xyo_mot_turnright(JitCtx*, uint64_t);
void xyo_mot_turnleft(JitCtx*, uint64_t);
void xyo_mot_pointdir(JitCtx*, uint64_t);
void xyo_mot_pointtowards(JitCtx*, uint64_t);
void xyo_mot_glidexy(JitCtx*, uint64_t, uint64_t, uint64_t);
void xyo_mot_glidebegin_xy(JitCtx*, uint64_t, uint64_t, uint64_t);
void xyo_mot_glidebegin_to(JitCtx*, uint64_t, uint64_t);
int32_t xyo_mot_glidestep(JitCtx*);
void xyo_looks_saybegin(JitCtx*, uint64_t, uint64_t);
void xyo_looks_thinkbegin(JitCtx*, uint64_t, uint64_t);
void xyo_snd_playbegin(JitCtx*, uint64_t);
void xyo_mot_glideto(JitCtx*, uint64_t, uint64_t);
void xyo_mot_edgebounce(JitCtx*);
void xyo_mot_setrotstyle(JitCtx*, uint64_t);
void xyo_mot_changex(JitCtx*, uint64_t);
void xyo_mot_setx(JitCtx*, uint64_t);
void xyo_mot_changey(JitCtx*, uint64_t);
void xyo_mot_sety(JitCtx*, uint64_t);
uint64_t xyo_mot_getx(JitCtx*);
uint64_t xyo_mot_gety(JitCtx*);
uint64_t xyo_mot_getdir(JitCtx*);
void xyo_looks_say(JitCtx*, uint64_t);
void xyo_looks_think(JitCtx*, uint64_t);
void xyo_looks_saysecs(JitCtx*, uint64_t, uint64_t);
void xyo_looks_thinksecs(JitCtx*, uint64_t, uint64_t);
void xyo_looks_show(JitCtx*);
void xyo_looks_hide(JitCtx*);
void xyo_looks_switchcostume(JitCtx*, uint64_t);
void xyo_looks_nextcostume(JitCtx*);
void xyo_looks_switchbackdrop(JitCtx*, uint64_t);
void xyo_looks_nextbackdrop(JitCtx*);
void xyo_looks_switchbackdropwait(JitCtx*, uint64_t);
void xyo_looks_backdropbegin(JitCtx*, uint64_t);
int32_t xyo_looks_backdropstep(JitCtx*);
void xyo_looks_changeeffect(JitCtx*, uint64_t, uint64_t);
void xyo_looks_seteffect(JitCtx*, uint64_t, uint64_t);
void xyo_looks_cleareffects(JitCtx*);
void xyo_looks_changesize(JitCtx*, uint64_t);
void xyo_looks_setsize(JitCtx*, uint64_t);
void xyo_looks_frontback(JitCtx*, uint64_t);
void xyo_looks_fwdbacklayers(JitCtx*, uint64_t, uint64_t);
uint64_t xyo_looks_size(JitCtx*);
uint64_t xyo_looks_costumenumname(JitCtx*, uint64_t);
uint64_t xyo_looks_backdropnumname(JitCtx*, uint64_t);
uint64_t xyo_sns_touchingobj(JitCtx*, uint64_t);
uint64_t xyo_sns_touchingcolor(JitCtx*, uint64_t);
uint64_t xyo_sns_coloristouching(JitCtx*, uint64_t, uint64_t);
uint64_t xyo_sns_distanceto(JitCtx*, uint64_t);
uint64_t xyo_sns_mousex(JitCtx*);
uint64_t xyo_sns_mousey(JitCtx*);
uint64_t xyo_sns_mousedown(JitCtx*);
uint64_t xyo_sns_keypressed(JitCtx*, uint64_t);
void xyo_sns_setdragmode(JitCtx*, uint64_t);
void xyo_sns_resettimer(JitCtx*);
uint64_t xyo_sns_current(JitCtx*, uint64_t);
uint64_t xyo_sns_dayssince2000(JitCtx*);
double xyo_sns_dayssince2000_f64(JitCtx*);
uint64_t xyo_sns_loudness(JitCtx*);
uint64_t xyo_sns_loud(JitCtx*);
uint64_t xyo_sns_answer(JitCtx*);
void xyo_sns_askwait(JitCtx*, uint64_t);
void xyo_sns_askbegin(JitCtx*, uint64_t);
uint64_t xyo_sns_of(JitCtx*, uint64_t, uint64_t);
uint64_t xyo_sns_username(JitCtx*);
uint64_t xyo_sns_online(JitCtx*);
uint64_t xyo_ctl_getcounter(JitCtx*);
void xyo_ctl_incrcounter(JitCtx*);
void xyo_ctl_clearcounter(JitCtx*);
void xyo_ctl_createclone(JitCtx*, uint64_t);
void xyo_ctl_deleteclone(JitCtx*);
int32_t xyo_ctl_deleteclone_begin(JitCtx*);
void xyo_snd_play(JitCtx*, uint64_t);
void xyo_snd_playwait(JitCtx*, uint64_t);
void xyo_snd_stopall(JitCtx*);
void xyo_snd_seteffect(JitCtx*, uint64_t, uint64_t);
int32_t xyo_snd_seteffect_begin(JitCtx*, uint64_t, uint64_t);
int32_t xyo_snd_changeeffect_begin(JitCtx*, uint64_t, uint64_t);
int32_t xyo_snd_setvolume_begin(JitCtx*, uint64_t);
int32_t xyo_snd_changevolume_begin(JitCtx*, uint64_t);
void xyo_snd_changeeffect(JitCtx*, uint64_t, uint64_t);
void xyo_snd_cleareffects(JitCtx*);
void xyo_snd_setvolume(JitCtx*, uint64_t);
void xyo_snd_changevolume(JitCtx*, uint64_t);
uint64_t xyo_snd_getvolume(JitCtx*);
Variable* xyo_jitVar(JitCtx*, const char*, const char16_t*, int);
void xyo_pen_up(JitCtx*);
void xyo_pen_down(JitCtx*);
void xyo_pen_clear(JitCtx*);
void xyo_pen_stamp(JitCtx*);
void xyo_pen_setcolor(JitCtx*, uint64_t);
void xyo_pen_setcolor_ps(void*, uint64_t);
void xyo_pen_changeparam(JitCtx*, uint64_t, uint64_t);
void xyo_pen_setparam(JitCtx*, uint64_t, uint64_t);
void xyo_pen_setsize(JitCtx*, uint64_t);
void xyo_pen_changesize(JitCtx*, uint64_t);
void xyo_pen_recolor(JitCtx*);
void xyo_pen_paramnum(JitCtx*, int32_t, int32_t, double);
int32_t xyo_gpu_repeat(JitCtx*, const void*, double);
// par-for（並列 append ループ）— parfor_rt.cpp
void xyo_list_reserve(void* list, int64_t newLen);
void xyo_par_slot(void* list, int64_t idx, int64_t bits);
int xyo_par_for(JitCtx*, int64_t trip, double init, double delta,
                void** lists, int64_t* bases, int64_t nl,
                void (*body)(JitCtx*, int64_t, double, void**, int64_t*));
}

// Native-only guard slow-path telemetry. This function is referenced by
// generated LLVM IR only with XYO_JIT_GUARD_STATS=1.
extern "C" void xyo_jit_guard_slow(uint32_t kind) noexcept {
    JitGuardSlowStats::get().noteHit(kind);
}


// IR-001: opt-in source-location observer around the ORIGINAL synchronous
// runtime helper. The journal counters are evidence of host delivery, not
// an authentication of intra-statement effects or deopt eligibility.
namespace {
ir::SourceEffectHostEpoch ir001ReadHostEpoch(const Runtime* runtime) {
    if (!runtime || !runtime->observableJournal) return {};
    const auto& journal = *runtime->observableJournal;
    return {
        runtime->observableSliceId, journal.revision(),
        runtime->observableImmediateBroadcasts,
        runtime->observableImmediateEffects,
        journal.pendingCount(), journal.rootedPayloads(), true,
        Gc::get().collectionGeneration(),
        Gc::get().explicitCollectionPending()
    };
}

template<class Perform>
void ir001ObserveEffect(
    JitCtx* c, const char* kind, const char* blockId, uint64_t sourcePc,
    uint64_t compiledRevision, uint64_t digestLo, uint64_t digestHi,
    bool broadcast, Perform&& perform) {
    if (!c || !c->runtime) return;
    Runtime* const runtime = c->runtime;
    const bool journalBefore = runtime->observableJournal != nullptr;
    const uint64_t beforePublished = journalBefore
        ? runtime->observableJournal->revision() : 0;
    const uint64_t beforeDelivered = broadcast
        ? runtime->observableCommittedBroadcasts
        : runtime->observableCommittedEffects;
    const uint64_t beforeImmediate = broadcast
        ? runtime->observableImmediateBroadcasts
        : runtime->observableImmediateEffects;
    const uint64_t beforeSlice = runtime->observableSliceId;
    bool sourceRevisionMatches = false;
    if (c->thread && c->thread->blockContainer && blockId) {
        const auto* blocks = c->thread->blockContainer;
        sourceRevisionMatches =
            compiledRevision && sourcePc && (digestLo || digestHi) &&
            blocks->revision() == compiledRevision &&
            blocks->getBlock(blockId) != nullptr;
    }
    ir::SourceEffectDispatchObservation exactSource;
    exactSource.blockId = blockId ? blockId : "";
    exactSource.sourcePc = sourcePc;
    exactSource.sourceRevision = compiledRevision;
    exactSource.digestLo = digestLo;
    exactSource.digestHi = digestHi;
    exactSource.operation = broadcast
        ? ir::ObservableOp::Broadcast : ir::ObservableOp::PenStamp;
    auto& timelines = ir::NativeSourceEffectTimelineRegistry::get();
    const uint64_t beforeMutationEpoch =
        timelines.mutationEpochSnapshot();
    const uint64_t beforeGcGeneration =
        Gc::get().collectionGeneration();
    {
        ir::ScopedSourceEffectDispatch sourceScope(exactSource);
        perform(); // exact existing effect semantics, no replay
    }
    exactSource.hostEpoch = ir001ReadHostEpoch(runtime);
    if (c->thread)
        timelines.observeDuringDelivery(
            c->thread, exactSource, sourceRevisionMatches,
            beforeMutationEpoch, beforeGcGeneration);
    const bool exactDelivery =
        sourceRevisionMatches && exactSource.exactDelivery();
    const bool journalEnabled = runtime->observableJournal != nullptr;
    const uint64_t afterPublished = journalEnabled
        ? runtime->observableJournal->revision() : 0;
    const uint64_t afterDelivered = broadcast
        ? runtime->observableCommittedBroadcasts
        : runtime->observableCommittedEffects;
    const uint64_t afterImmediate = broadcast
        ? runtime->observableImmediateBroadcasts
        : runtime->observableImmediateEffects;
    const uint64_t afterSlice = runtime->observableSliceId;
    std::fprintf(stderr,
        "[ir001-effect-source] kind=%s pc=%llu block=%s "
        "revision=%llu digest=%016llx:%016llx revision_matches=%d "
        "journal=%d published_delta=%llu delivered_delta=%llu "
        "immediate_delta=%llu slice_delta=%llu "
        "pending=%zu rooted=%zu exact_delivery=%d event_published=%u "
        "event_delivered=%u event_slice=%llu\n",
        kind, static_cast<unsigned long long>(sourcePc),
        blockId ? blockId : "<null>",
        static_cast<unsigned long long>(compiledRevision),
        static_cast<unsigned long long>(digestLo),
        static_cast<unsigned long long>(digestHi),
        int(sourceRevisionMatches), int(journalEnabled),
        static_cast<unsigned long long>(
            afterPublished - beforePublished),
        static_cast<unsigned long long>(
            afterDelivered - beforeDelivered),
        static_cast<unsigned long long>(
            afterImmediate - beforeImmediate),
        static_cast<unsigned long long>(afterSlice - beforeSlice),
        journalEnabled ? runtime->observableJournal->pendingCount() : 0,
        journalEnabled ? runtime->observableJournal->rootedPayloads() : 0,
        int(exactDelivery), exactSource.published, exactSource.delivered,
        static_cast<unsigned long long>(exactSource.sliceId));
}
} // namespace

extern "C" void xyo_ir001_broadcast_v_source(
    JitCtx* c, uint64_t message, const char* blockId, uint64_t sourcePc,
    uint64_t compiledRevision, uint64_t digestLo, uint64_t digestHi) {
    ir001ObserveEffect(c, "broadcast", blockId, sourcePc,
        compiledRevision, digestLo, digestHi, true,
        [&] { xyo_broadcast_v(c, message); });
}

extern "C" void xyo_ir001_pen_stamp_source(
    JitCtx* c, const char* blockId, uint64_t sourcePc,
    uint64_t compiledRevision, uint64_t digestLo, uint64_t digestHi) {
    ir001ObserveEffect(c, "pen-stamp", blockId, sourcePc,
        compiledRevision, digestLo, digestHi, false,
        [&] { xyo_pen_stamp(c); });
}

// A conservative, diagnostic-only barrier emitted by the LLVM
// statement lowerer at a possible variable/list write. The interpreter
// is not instrumented here; this can only DISQUALIFY diagnostic
// checkpoints, never certify all Scratch-visible writes are covered.
extern "C" void xyo_ir001_vm_write(JitCtx* c) {
    if (!c || !c->thread) return;
    ir::noteSourceEffectUnjournaledMutation();
    ir::NativeSourceEffectTimelineRegistry::get()
        .noteNonJournalMutation(c->thread);
}

// A native Scratch loop's scheduler-owned checkpoint receives the
// bounded, independently-owned event history (never a native stack ptr).
// A matching timeline remains PARTIAL: uninstrumented Scratch blocks,
// nested procedure effects, compiler effects and GC/live SSA are unknown.
// This helper never enables a detached continuation or native deopt.
extern "C" void xyo_ir001_effect_checkpoint(
    JitCtx* c, const char* blockId, uint64_t loopPc,
    uint64_t compiledRevision, uint64_t digestLo, uint64_t digestHi,
    uint32_t phase) {
    if (!c || !c->thread) return;
    // Only a verified root CallFrame can supply source-level ownership
    // for this initial diagnostic. Do not label callee bodies as root.
    auto* frames = c->thread->nativeProcedureFrames.get();
    const auto sourceCheckpoint = frames
        ? frames->sourceCheckpoint()
        : std::nullopt;
    const auto expectedPhase = static_cast<ir::SourceResumePhase>(phase);
    const bool sourceOwnerValid =
        frames && frames->depth() == 1 &&
        frames->sourceContentMatches() &&
        sourceCheckpoint &&
        sourceCheckpoint->frame.procedureId == 0 &&
        sourceCheckpoint->frame.resumePc == loopPc &&
        sourceCheckpoint->phase == expectedPhase &&
        !sourceCheckpoint->loops.empty() &&
        sourceCheckpoint->loops.back().sourcePc == loopPc &&
        c->thread->blockContainer && blockId &&
        c->thread->blockContainer->revision() == compiledRevision &&
        c->thread->blockContainer->getBlock(blockId) != nullptr;
    const auto source = ir::SourceEffectIdentity{
        compiledRevision, digestLo, digestHi, 0};
    const auto checkpoint =
        ir::NativeSourceEffectTimelineRegistry::get().checkpoint(
            c->thread, sourceOwnerValid ? source : ir::SourceEffectIdentity{},
            loopPc, static_cast<ir::SourceResumePhase>(phase),
            ir001ReadHostEpoch(c->runtime));
    const auto& latest = checkpoint.latest;
    std::fprintf(stderr,
        "[ir001-effect-checkpoint] pc=%llu block=%s phase=%u "
        "captured=%d seq=%llu effect_pc=%llu slice=%llu "
        "source_owner=%d executable=0 vm_write_pending=%d "
        "foreign_vm_write=%d interp_vm_write=%d\n",
        static_cast<unsigned long long>(loopPc),
        blockId ? blockId : "<null>", phase, int(checkpoint.captured),
        static_cast<unsigned long long>(checkpoint.count),
        static_cast<unsigned long long>(
            latest ? latest->sourcePc : 0),
        static_cast<unsigned long long>(
            latest ? latest->journalSlice : 0),
        int(sourceOwnerValid),
        int(checkpoint.pendingNonJournalMutation),
        int(checkpoint.foreignVmWrite),
        int(checkpoint.interpreterVmWrite));
}

namespace {

// 生成コードが直接参照する extern "C" シンボル
const std::pair<const char*, void*> kRtSymbols[] = {
    {"xyo_jit_guard_slow", (void*)&xyo_jit_guard_slow},
    // リスト長上限（データシンボル: 生成コードが直接ロードする）
    {"xyo_list_limit", (void*)&xyo::xyo_list_limit},
    {"xyo_proc_frame_init_root_verified",
     (void*)&xyo_proc_frame_init_root_verified},
    {"xyo_rt_capture_llvm_ssa", (void*)&xyo_rt_capture_llvm_ssa},
    {"xyo_proc_frame_enter", (void*)&xyo_proc_frame_enter},
    {"xyo_proc_frame_enter_rev", (void*)&xyo_proc_frame_enter_rev},
    {"xyo_proc_frame_enter_verified",
     (void*)&xyo_proc_frame_enter_verified},
    {"xyo_proc_frame_enter_verified_rootpath",
     (void*)&xyo_proc_frame_enter_verified_rootpath},
    {"xyo_proc_frame_leave", (void*)&xyo_proc_frame_leave},
    {"xyo_proc_frame_body_started", (void*)&xyo_proc_frame_body_started},
    {"xyo_proc_frame_statement_started",
     (void*)&xyo_proc_frame_statement_started},
    {"xyo_proc_frame_checkpoint", (void*)&xyo_proc_frame_checkpoint},
    {"xyo_proc_frame_checkpoint_path", (void*)&xyo_proc_frame_checkpoint_path},
    {"xyo_proc_frame_checkpoint_path_phase",
     (void*)&xyo_proc_frame_checkpoint_path_phase},
    {"xyo_yield", (void*)&xyo_yield},
    {"xyo_yield_fast", (void*)&xyo_yield_fast},
    {"xyo_is_stuck", (void*)&xyo_is_stuck},
    {"xyo_wait_should_yield", (void*)&xyo_wait_should_yield},
    {"xyo_wait_yield", (void*)&xyo_wait_yield},
    {"xyo_wait_event_yield", (void*)&xyo_wait_event_yield},
    {"xyo_wait_event_done", (void*)&xyo_wait_event_done},
    {"xyo_loop_yield", (void*)&xyo_loop_yield},
    {"xyo_warp_yield_due", (void*)&xyo_warp_yield_due},
    {"xyo_rt_gc_soft_budget_due", (void*)&xyo_rt_gc_soft_budget_due},
    {"xyo_empty_repeat_warp", (void*)&xyo_empty_repeat_warp},
    {"xyo_gc_epoch", (void*)&xyo_gc_epoch},
    {"xyo_to_num", (void*)&xyo_to_num},
    {"xyo_to_num_uncached", (void*)&xyo_to_num_uncached},
    {"xyo_truthy", (void*)&xyo_truthy},
    {"xyo_num", (void*)&xyo_num},
    {"xyo_undefined", (void*)&xyo_undefined},
    {"xyo_to_str", (void*)&xyo_to_str},
    {"xyo_add", (void*)&xyo_add},
    {"xyo_sub", (void*)&xyo_sub},
    {"xyo_mul", (void*)&xyo_mul},
    {"xyo_div", (void*)&xyo_div},
    {"xyo_mod", (void*)&xyo_mod},
    {"xyo_lt", (void*)&xyo_lt},
    {"xyo_gt", (void*)&xyo_gt},
    {"xyo_eq", (void*)&xyo_eq},
    {"xyo_not", (void*)&xyo_not},
    {"xyo_and", (void*)&xyo_and},
    {"xyo_or", (void*)&xyo_or},
    {"xyo_random", (void*)&xyo_random},
    {"xyo_mathop", (void*)&xyo_mathop},
    {"xyo_math_abs", (void*)&xyo_math_abs},
    {"xyo_math_floor", (void*)&xyo_math_floor},
    {"xyo_math_ceil", (void*)&xyo_math_ceil},
    {"xyo_math_sqrt", (void*)&xyo_math_sqrt},
    {"xyo_math_sin", (void*)&xyo_math_sin},
    {"xyo_math_cos", (void*)&xyo_math_cos},
    {"xyo_math_tan", (void*)&xyo_math_tan},
    {"xyo_math_asin", (void*)&xyo_math_asin},
    {"xyo_math_acos", (void*)&xyo_math_acos},
    {"xyo_math_atan", (void*)&xyo_math_atan},
    {"xyo_math_ln", (void*)&xyo_math_ln},
    {"xyo_math_log", (void*)&xyo_math_log},
    {"xyo_math_exp", (void*)&xyo_math_exp},
    {"xyo_math_pow10", (void*)&xyo_math_pow10},
    {"xyo_letter_eq", (void*)&xyo_letter_eq},
    {"xyo_letter_eq_i", (void*)&xyo_letter_eq_i},
    {"xyo_round", (void*)&xyo_round},
    {"xyo_log", (void*)&xyo_log},
        {"xyo_asin", (void*)&xyo_asin},
        {"xyo_acos", (void*)&xyo_acos},
        {"xyo_atan", (void*)&xyo_atan},
        {"xyo_exp", (void*)&xyo_exp},
    {"xyo_var_get", (void*)&xyo_var_get},
    {"xyo_var_set", (void*)&xyo_var_set},
    {"xyo_wait_notify_var_store", (void*)&xyo_wait_notify_var_store},
    {"xyo_var_change", (void*)&xyo_var_change},
    {"xyo_jitVar", (void*)&xyo_jitVar},
    {"xyo_varslot_init", (void*)&xyo_varslot_init},
    {"xyo_varslot_checked", (void*)&xyo_varslot_checked},
    {"xyo_varslot_guard_stage", (void*)&xyo_varslot_guard_stage},
    {"xyo_pen_up", (void*)&xyo_pen_up},
    {"xyo_pen_down", (void*)&xyo_pen_down},
    {"xyo_pen_clear", (void*)&xyo_pen_clear},
    {"xyo_pen_stamp", (void*)&xyo_pen_stamp},
    {"xyo_pen_setcolor", (void*)&xyo_pen_setcolor},
    {"xyo_pen_setcolor_ps", (void*)&xyo_pen_setcolor_ps},
    {"xyo_pen_changeparam", (void*)&xyo_pen_changeparam},
    {"xyo_pen_setparam", (void*)&xyo_pen_setparam},
    {"xyo_pen_setsize", (void*)&xyo_pen_setsize},
    {"xyo_pen_changesize", (void*)&xyo_pen_changesize},
    {"xyo_pen_recolor", (void*)&xyo_pen_recolor},
    {"xyo_pen_paramnum", (void*)&xyo_pen_paramnum},
    {"xyo_gpu_repeat", (void*)&xyo_gpu_repeat},
    {"xyo_list_reserve", (void*)&xyo_list_reserve},
    {"xyo_par_slot", (void*)&xyo_par_slot},
    {"xyo_par_for", (void*)&xyo_par_for},
    {"xyo_arg_get", (void*)&xyo_arg_get},
    {"xyo_rt_wait", (void*)&xyo_rt_wait},
    {"xyo_wait_begin", (void*)&xyo_wait_begin},
    {"xyo_wait_poll", (void*)&xyo_wait_poll},
    {"xyo_stop_all", (void*)&xyo_stop_all},
    {"xyo_stop_script_begin", (void*)&xyo_stop_script_begin},
    {"xyo_stop_all_begin", (void*)&xyo_stop_all_begin},
    {"xyo_stop_others", (void*)&xyo_stop_others},
    {"xyo_stop_script", (void*)&xyo_stop_script},
    {"xyo_broadcast_v", (void*)&xyo_broadcast_v},
    {"xyo_ir001_broadcast_v_source",
     (void*)&xyo_ir001_broadcast_v_source},
    {"xyo_ir001_pen_stamp_source",
     (void*)&xyo_ir001_pen_stamp_source},
    {"xyo_ir001_effect_checkpoint",
     (void*)&xyo_ir001_effect_checkpoint},
    {"xyo_ir001_vm_write", (void*)&xyo_ir001_vm_write},
    {"xyo_broadcast_wait_v", (void*)&xyo_broadcast_wait_v},
    {"xyo_broadcast_wait_lit", (void*)&xyo_broadcast_wait_lit},
    {"xyo_bcastwait_begin_v", (void*)&xyo_bcastwait_begin_v},
    {"xyo_bcastwait_begin_lit", (void*)&xyo_bcastwait_begin_lit},
    {"xyo_bcastwait_step", (void*)&xyo_bcastwait_step},
    {"xyo_timer", (void*)&xyo_timer},
    {"xyo_rt_compat", (void*)&xyo_rt_compat},
    {"xyo_rt_aot_cached", (void*)&xyo_rt_aot_cached},
    {"xyo_rt_reporter", (void*)&xyo_rt_reporter},
    {"xyo_rt_async_reporter", (void*)&xyo_rt_async_reporter},
    {"xyo_rt_async_reporter_rooted",
     (void*)&xyo_rt_async_reporter_rooted},
    {"xyo_rt_nested_osr_arg_get", (void*)&xyo_rt_nested_osr_arg_get},
    {"xyo_rt_nested_osr_arg_peek", (void*)&xyo_rt_nested_osr_arg_peek},
    {"xyo_rt_nested_osr_yield_checkpoint",
     (void*)&xyo_rt_nested_osr_yield_checkpoint},
    {"xyo_rt_nested_osr_inner_yield_checkpoint",
     (void*)&xyo_rt_nested_osr_inner_yield_checkpoint},
    {"xyo_rt_nested_osr_triple_yield_checkpoint",
     (void*)&xyo_rt_nested_osr_triple_yield_checkpoint},
    {"xyo_rt_nested_osr_quad_yield_checkpoint",
     (void*)&xyo_rt_nested_osr_quad_yield_checkpoint},
    {"xyo_proc_push", (void*)&xyo_proc_push},
    {"xyo_proc_push_ex", (void*)&xyo_proc_push_ex},
    {"xyo_recursion_yield_due", (void*)&xyo_recursion_yield_due},
    {"xyo_coro_push_frame", (void*)&xyo_coro_push_frame},
    {"xyo_coro_top_frame", (void*)&xyo_coro_top_frame},
    {"xyo_coro_pop_frame", (void*)&xyo_coro_pop_frame},
    {"xyo_proc_pop", (void*)&xyo_proc_pop},
    {"xyo_rt_setarg", (void*)&xyo_rt_setarg},
    {"xyo_join", (void*)&xyo_join},
    {"xyo_join_suffix", (void*)&xyo_join_suffix},
    {"xyo_transient_begin", (void*)&xyo_transient_begin},
    {"xyo_transient_append", (void*)&xyo_transient_append},
    {"xyo_transient_append_value", (void*)&xyo_transient_append_value},
    {"xyo_transient_append_packed", (void*)&xyo_transient_append_packed},
    {"xyo_transient_append_repeat_packed",
        (void*)&xyo_transient_append_repeat_packed},
    {"xyo_transient_append_repeat_string",
        (void*)&xyo_transient_append_repeat_string},
    {"xyo_transient_append_repeat_value",
        (void*)&xyo_transient_append_repeat_value},
    {"xyo_transient_flush", (void*)&xyo_transient_flush},
    {"xyo_transient_finish", (void*)&xyo_transient_finish},
    {"xyo_transient_resume", (void*)&xyo_transient_resume},

#if defined(XYO_STRING_JOIN_SPECIALIZED)
    {"xyo_join_strings", (void*)&xyo_join_strings},
#endif
    {"xyo_letterof", (void*)&xyo_letterof},
    {"xyo_strlen", (void*)&xyo_strlen},
    {"xyo_strcontains", (void*)&xyo_strcontains},
    {"xyo_list_add", (void*)&xyo_list_add},
    {"xyo_mega_bench_end", (void*)&xyo_mega_bench_end},
    {"xyo_list_delete", (void*)&xyo_list_delete},
    {"xyo_list_deleteall", (void*)&xyo_list_deleteall},
    {"xyo_list_insert", (void*)&xyo_list_insert},
    {"xyo_list_replace", (void*)&xyo_list_replace},
    {"xyo_ir001_scalar_read_eval",
        (void*)&xyo_ir001_scalar_read_eval},
    {"xyo_ir001_list_read_epoch",
        (void*)&xyo_ir001_list_read_epoch},
    {"xyo_ir001_list_read_eval",
        (void*)&xyo_ir001_list_read_eval},
    {"xyo_ir001_list_content_write_begin",
        (void*)&xyo_ir001_list_content_write_begin},
    {"xyo_ir001_list_content_write_end",
        (void*)&xyo_ir001_list_content_write_end},
    {"xyo_list_item", (void*)&xyo_list_item},
    {"xyo_list_itemnum", (void*)&xyo_list_itemnum},
    {"xyo_list_length", (void*)&xyo_list_length},
    {"xyo_list_contains", (void*)&xyo_list_contains},
    {"xyo_list_contents", (void*)&xyo_list_contents},
    {"xyo_var_monitor", (void*)&xyo_var_monitor},
    {"xyo_mot_movesteps", (void*)&xyo_mot_movesteps},
    {"xyo_mot_gotoxy", (void*)&xyo_mot_gotoxy},
    {"xyo_mot_setxy_num", (void*)&xyo_mot_setxy_num},
    {"xyo_mot_goto", (void*)&xyo_mot_goto},
    {"xyo_mot_turnright", (void*)&xyo_mot_turnright},
    {"xyo_mot_turnleft", (void*)&xyo_mot_turnleft},
    {"xyo_mot_pointdir", (void*)&xyo_mot_pointdir},
    {"xyo_mot_pointtowards", (void*)&xyo_mot_pointtowards},
    {"xyo_mot_glidexy", (void*)&xyo_mot_glidexy},
    {"xyo_mot_glidebegin_xy", (void*)&xyo_mot_glidebegin_xy},
    {"xyo_mot_glidebegin_to", (void*)&xyo_mot_glidebegin_to},
    {"xyo_mot_glidestep", (void*)&xyo_mot_glidestep},
    {"xyo_looks_saybegin", (void*)&xyo_looks_saybegin},
    {"xyo_looks_thinkbegin", (void*)&xyo_looks_thinkbegin},
    {"xyo_snd_playbegin", (void*)&xyo_snd_playbegin},
    {"xyo_mot_glideto", (void*)&xyo_mot_glideto},
    {"xyo_mot_edgebounce", (void*)&xyo_mot_edgebounce},
    {"xyo_mot_setrotstyle", (void*)&xyo_mot_setrotstyle},
    {"xyo_mot_changex", (void*)&xyo_mot_changex},
    {"xyo_mot_setx", (void*)&xyo_mot_setx},
    {"xyo_mot_changey", (void*)&xyo_mot_changey},
    {"xyo_mot_sety", (void*)&xyo_mot_sety},
    {"xyo_mot_getx", (void*)&xyo_mot_getx},
    {"xyo_mot_gety", (void*)&xyo_mot_gety},
    {"xyo_mot_getdir", (void*)&xyo_mot_getdir},
    {"xyo_looks_say", (void*)&xyo_looks_say},
    {"xyo_looks_think", (void*)&xyo_looks_think},
    {"xyo_looks_saysecs", (void*)&xyo_looks_saysecs},
    {"xyo_looks_thinksecs", (void*)&xyo_looks_thinksecs},
    {"xyo_looks_show", (void*)&xyo_looks_show},
    {"xyo_looks_hide", (void*)&xyo_looks_hide},
    {"xyo_looks_switchcostume", (void*)&xyo_looks_switchcostume},
    {"xyo_looks_nextcostume", (void*)&xyo_looks_nextcostume},
    {"xyo_looks_switchbackdrop", (void*)&xyo_looks_switchbackdrop},
    {"xyo_looks_nextbackdrop", (void*)&xyo_looks_nextbackdrop},
    {"xyo_looks_switchbackdropwait", (void*)&xyo_looks_switchbackdropwait},
    {"xyo_looks_backdropbegin", (void*)&xyo_looks_backdropbegin},
    {"xyo_looks_backdropstep", (void*)&xyo_looks_backdropstep},
    {"xyo_looks_changeeffect", (void*)&xyo_looks_changeeffect},
    {"xyo_looks_seteffect", (void*)&xyo_looks_seteffect},
    {"xyo_looks_cleareffects", (void*)&xyo_looks_cleareffects},
    {"xyo_looks_changesize", (void*)&xyo_looks_changesize},
    {"xyo_looks_setsize", (void*)&xyo_looks_setsize},
    {"xyo_looks_frontback", (void*)&xyo_looks_frontback},
    {"xyo_looks_fwdbacklayers", (void*)&xyo_looks_fwdbacklayers},
    {"xyo_looks_size", (void*)&xyo_looks_size},
    {"xyo_looks_costumenumname", (void*)&xyo_looks_costumenumname},
    {"xyo_looks_backdropnumname", (void*)&xyo_looks_backdropnumname},
    {"xyo_sns_touchingobj", (void*)&xyo_sns_touchingobj},
    {"xyo_sns_touchingcolor", (void*)&xyo_sns_touchingcolor},
    {"xyo_sns_coloristouching", (void*)&xyo_sns_coloristouching},
    {"xyo_sns_distanceto", (void*)&xyo_sns_distanceto},
    {"xyo_sns_mousex", (void*)&xyo_sns_mousex},
    {"xyo_sns_mousey", (void*)&xyo_sns_mousey},
    {"xyo_sns_mousedown", (void*)&xyo_sns_mousedown},
    {"xyo_sns_keypressed", (void*)&xyo_sns_keypressed},
    {"xyo_sns_setdragmode", (void*)&xyo_sns_setdragmode},
    {"xyo_sns_resettimer", (void*)&xyo_sns_resettimer},
    {"xyo_sns_current", (void*)&xyo_sns_current},
    {"xyo_sns_dayssince2000", (void*)&xyo_sns_dayssince2000},
    {"xyo_sns_dayssince2000_f64", (void*)&xyo_sns_dayssince2000_f64},
    {"xyo_sns_loudness", (void*)&xyo_sns_loudness},
    {"xyo_sns_loud", (void*)&xyo_sns_loud},
    {"xyo_sns_answer", (void*)&xyo_sns_answer},
    {"xyo_sns_askwait", (void*)&xyo_sns_askwait},
    {"xyo_sns_askbegin", (void*)&xyo_sns_askbegin},
    {"xyo_sns_of", (void*)&xyo_sns_of},
    {"xyo_sns_username", (void*)&xyo_sns_username},
    {"xyo_sns_online", (void*)&xyo_sns_online},
    {"xyo_ctl_getcounter", (void*)&xyo_ctl_getcounter},
    {"xyo_ctl_incrcounter", (void*)&xyo_ctl_incrcounter},
    {"xyo_ctl_clearcounter", (void*)&xyo_ctl_clearcounter},
    {"xyo_ctl_createclone", (void*)&xyo_ctl_createclone},
    {"xyo_ctl_deleteclone", (void*)&xyo_ctl_deleteclone},
    {"xyo_ctl_deleteclone_begin", (void*)&xyo_ctl_deleteclone_begin},
    {"xyo_snd_play", (void*)&xyo_snd_play},
    {"xyo_snd_playwait", (void*)&xyo_snd_playwait},
    {"xyo_snd_stopall", (void*)&xyo_snd_stopall},
    {"xyo_snd_seteffect", (void*)&xyo_snd_seteffect},
    {"xyo_snd_seteffect_begin", (void*)&xyo_snd_seteffect_begin},
    {"xyo_snd_changeeffect_begin", (void*)&xyo_snd_changeeffect_begin},
    {"xyo_snd_setvolume_begin", (void*)&xyo_snd_setvolume_begin},
    {"xyo_snd_changevolume_begin", (void*)&xyo_snd_changevolume_begin},
    {"xyo_snd_changeeffect", (void*)&xyo_snd_changeeffect},
    {"xyo_snd_cleareffects", (void*)&xyo_snd_cleareffects},
    {"xyo_snd_setvolume", (void*)&xyo_snd_setvolume},
    {"xyo_snd_changevolume", (void*)&xyo_snd_changevolume},
    {"xyo_snd_getvolume", (void*)&xyo_snd_getvolume},
};

} // namespace
namespace {

/// バックエンドが libcall として生成する libc/libm シンボル。
///
/// `frem` は x86-64 で `fmod` 呼び出しへ、`llvm.log` は `log`、memcmp は
/// `bcmp` へ降格される。動的リンクなら ORC の
/// `DynamicLibrarySearchGenerator`（dlsym）が解決できるが、**完全静的
/// リンク（-static）では dlsym が自プロセスの静的シンボルを見ない**ため
/// `JIT session error: Symbols not found` になる。kRtSymbols と同じく
/// 絶対シンボルとして JITDylib へ define しておく（ジェネレータより優先）。
const std::vector<std::pair<const char*, void*>>& hostLibcallSymbols() {
    static const std::vector<std::pair<const char*, void*>> k = {
        {"fmod", (void*)(double (*)(double, double))std::fmod},
        {"fmodf", (void*)(float (*)(float, float))std::fmod},
        {"pow", (void*)(double (*)(double, double))std::pow},
        {"powf", (void*)(float (*)(float, float))std::pow},
        {"log", (void*)(double (*)(double))xyo::jsnum::jsLog},
        {"asin", (void*)(double (*)(double))xyo::v8math::asin},
        {"acos", (void*)(double (*)(double))xyo::v8math::acos},
        {"atan", (void*)(double (*)(double))xyo::v8math::atan},
        {"exp", (void*)(double (*)(double))xyo::v8math::exp},
        {"logf", (void*)(float (*)(float))std::log},
        {"log2", (void*)(double (*)(double))std::log2},
        {"log10", (void*)(double (*)(double))std::log10},
        {"exp", (void*)(double (*)(double))std::exp},
        {"exp2", (void*)(double (*)(double))std::exp2},
        {"sqrt", (void*)(double (*)(double))std::sqrt},
        {"sqrtf", (void*)(float (*)(float))std::sqrt},
        {"sin", (void*)(double (*)(double))xyo::v8math::sin},
        {"cos", (void*)(double (*)(double))xyo::v8math::cos},
        {"tan", (void*)(double (*)(double))std::tan},
        {"asin", (void*)(double (*)(double))std::asin},
        {"acos", (void*)(double (*)(double))std::acos},
        {"atan", (void*)(double (*)(double))std::atan},
        {"atan2", (void*)(double (*)(double, double))xyo::v8math::atan2},
        {"sinh", (void*)(double (*)(double))std::sinh},
        {"cosh", (void*)(double (*)(double))std::cosh},
        {"tanh", (void*)(double (*)(double))std::tanh},
        {"floor", (void*)(double (*)(double))std::floor},
        {"ceil", (void*)(double (*)(double))std::ceil},
        {"trunc", (void*)(double (*)(double))std::trunc},
        {"round", (void*)(double (*)(double))std::round},
        {"rint", (void*)(double (*)(double))std::rint},
        {"nearbyint", (void*)(double (*)(double))std::nearbyint},
        {"fabs", (void*)(double (*)(double))std::fabs},
        {"copysign", (void*)(double (*)(double, double))std::copysign},
        {"ldexp", (void*)(double (*)(double, int))std::ldexp},
        {"fma", (void*)(double (*)(double, double, double))std::fma},
        {"memcpy", (void*)(void* (*)(void*, const void*, size_t))std::memcpy},
        {"memmove",
         (void*)(void* (*)(void*, const void*, size_t))std::memmove},
        {"memset", (void*)(void* (*)(void*, int, size_t))std::memset},
        {"memcmp",
         (void*)(int (*)(const void*, const void*, size_t))std::memcmp},
        {"bcmp", (void*)(int (*)(const void*, const void*, size_t))std::memcmp},
        {"strlen", (void*)(size_t (*)(const char*))std::strlen},
        {"abort", (void*)(void (*)(void))std::abort},
    };
    return k;
}

} // namespace

struct LLVMBackend final : JitBackend {
    llvm::orc::ThreadSafeContext tsc;
    std::unique_ptr<llvm::orc::LLJIT> jit;
    std::unique_ptr<llvm::orc::LLLazyJIT> lazyJit;
    bool lazyProcedures = false;
    JitPinned pinned;

    LLVMBackend();
    ~LLVMBackend() override;
    bool ok() const override { return static_cast<bool>(jit); }
    std::unique_ptr<CompiledScript> compile(
        Thread&, Runtime&, const Block*, uint64_t serial,
        const ir::ScratchCompilationUnit& source) override;
    std::shared_ptr<NumericJitCode> compileNumeric(
        std::shared_ptr<const ir::ScratchNumericKernel>,
        uint64_t serial) override;
    std::shared_ptr<MegaDispatchCode> compileMegaDispatch(
        const std::vector<Thread*>&, Runtime&, uint64_t serial) override;
    std::shared_ptr<MegaDispatchCode> compileMegaYieldBatch(
        const std::vector<Thread*>&, Runtime&, uint64_t serial) override;
    std::unique_ptr<PendingJitCompilation> prepareAsync(
        Thread&, Runtime&, const Block*, uint64_t serial) override;
    bool addObjectFile(const void* bytes, size_t len) override;
    void* lookupSymbol(const std::string& symbol) override;
private:
    bool ensureLazyJit();
};

// Keep eager and on-demand LLVM pipelines on the same optimization tier.
static llvm::OptimizationLevel jitIrOptimizationLevel() {
    if (const char* setting = std::getenv("XYO_JIT_OPT_LEVEL")) {
        if (std::strcmp(setting, "1") == 0)
            return llvm::OptimizationLevel::O1;
        if (std::strcmp(setting, "2") == 0)
            return llvm::OptimizationLevel::O2;
        if (std::strcmp(setting, "3") == 0)
            return llvm::OptimizationLevel::O3;
    }
    return std::getenv("XYO_JIT_O2")
        ? llvm::OptimizationLevel::O2
        : llvm::OptimizationLevel::O3;
}

// IR-001: inspect the actual POST-optimizer LLVM IR, not only the
// source-level emitStackBlock pattern. Diagnostic-only and disabled
// unless both the source-effect trace and post-opt audit are requested.
static void auditIr001OptimizedWrites(llvm::Module& module,
                                     const char* tier) {
    const char* trace = std::getenv("XYO_IR001_TRACE_EFFECT_PC");
    const char* flag = std::getenv("XYO_IR001_OPT_WRITE_AUDIT");
    if (!trace || trace[0] != '1' || !flag || flag[0] != '1')
        return;
    const auto r = ir::auditLlvmEffectWrites(module);
    // A possible unguarded store or opaque call is NOT silent: make the
    // corresponding native checkpoint fail closed at runtime. The marker
    // is inserted AFTER O2/O3, so optimizer motion cannot bypass it.
    const uint64_t fenced = ir::fenceUnprovenLlvmEffectCheckpoints(module);
    std::fprintf(stderr,
        "[ir001-postopt-write] tier=%s events=%llu checkpoints=%llu "
        "barriers=%llu writes=%llu unguarded=%llu "
        "unguarded_opaque=%llu proven_local_calls=%llu "
        "local_stack=%llu isolated_stack=%llu "
        "unguarded_isolated_stack=%llu "
        "checkpoint_only_functions=%llu fenced=%llu executable=0\n",
        tier,
        static_cast<unsigned long long>(r.tracedEffects),
        static_cast<unsigned long long>(r.checkpoints),
        static_cast<unsigned long long>(r.writeFences),
        static_cast<unsigned long long>(r.memoryWrites),
        static_cast<unsigned long long>(r.unguardedWrites),
        static_cast<unsigned long long>(r.unguardedOpaqueCalls),
        static_cast<unsigned long long>(r.locallyProvenCalls),
        static_cast<unsigned long long>(r.localStackCandidates),
        static_cast<unsigned long long>(r.isolatedStackWrites),
        static_cast<unsigned long long>(r.unguardedIsolatedStackWrites),
        static_cast<unsigned long long>(
            r.functionsWithCheckpointsButNoEffects),
        static_cast<unsigned long long>(fenced));
}

// RT-001: independently inspect SSA CAPTURE calls after O2/O3, on
// BOTH eager and ORC lazy transform paths. A source-level manifest is
// not a machine stack map; no diagnostic field authorizes native GC.
static void auditRt001PostoptSsa(llvm::Module& module, const char* tier) {
    const char* flag = std::getenv("XYO_RT_POSTOPT_SSA_AUDIT");
    if (!flag || flag[0] != '1') return;
    const auto sites = rt::auditPostoptSsaCaptures(module);
    uint64_t unknown = 0;
    uint64_t live = 0;
    uint64_t invalid = 0;
    for (const auto& site : sites) {
        live += site.postoptLivePointers;
        unknown += site.postoptUnknownPointers;
        invalid += !site.reportValid;
        std::fprintf(stderr,
            "[rt001-postopt-ssa] tier=%s fn=%s site=%llu "
            "pre_unresolved=%u captured=%u "
            "post_live_ptr=%u post_unknown=%u "
            "post_stack=%u post_host=%u post_static=%u "
            "post_numeric=%u post_other=%u valid=%d gc_safe=0\n",
            tier, site.functionName.c_str(),
            static_cast<unsigned long long>(site.siteId),
            site.declaredPointers, site.capturedWords,
            site.postoptLivePointers, site.postoptUnknownPointers,
            site.postoptStackPointers, site.postoptHostPointers,
            site.postoptStaticPointers, site.postoptNumeric,
            site.postoptOther, int(site.reportValid));
    }
    std::fprintf(stderr,
        "[rt001-postopt-ssa-total] tier=%s sites=%zu "
        "post_live_ptr=%llu post_unknown=%llu invalid=%llu "
        "machine_roots_proven=0 gc_safe=0\n",
        tier, sites.size(),
        static_cast<unsigned long long>(live),
        static_cast<unsigned long long>(unknown),
        static_cast<unsigned long long>(invalid));
}

// Actual post-optimizer LLVM SSA + exact stack-array initialization
// census at native asynchronous reporter calls. A valid buffer contract
// is not a machine register map; GC remains deferred for live Fibers.
static void auditRt001PostoptAsyncRoots(
    llvm::Module& module, const char* tier) {
    const char* flag = std::getenv("XYO_RT_POSTOPT_ASYNC_ROOT_AUDIT");
    if (!flag || flag[0] != '1') return;
    const auto sites = rt::auditPostoptAsyncRoots(module);
    uint32_t invalid = 0;
    for (const auto& site : sites) {
        const bool valid = site.signatureValid && site.bufferValid &&
                           site.completeStores && site.coveredAllLiveI64;
        invalid += !valid;
        std::fprintf(stderr,
            "[rt001-postopt-async-root] tier=%s fn=%s words=%u "
            "filled=%u live_i64=%u arg_i64=%u covered_i64=%u "
            "unknown_i64=%u live_ptr=%u "
            "signature=%d buffer=%d stores=%d covered=%d valid=%d "
            "machine_roots_proven=0 gc_safe=0\n",
            tier, site.functionName.c_str(), site.declaredWords,
            site.initializedWords, site.liveI64, site.liveArgumentI64,
            site.coveredLiveI64, site.unknownI64, site.livePointers,
            int(site.signatureValid),
            int(site.bufferValid), int(site.completeStores),
            int(site.coveredAllLiveI64), int(valid));
    }
    std::fprintf(stderr,
        "[rt001-postopt-async-root-total] tier=%s sites=%zu "
        "invalid=%u machine_roots_proven=0 gc_safe=0\n",
        tier, sites.size(), invalid);
}

// RT-001 experimental machine stackmap probe. Unlike a compiler-only
// SSA census, the emitted llvm.experimental.stackmap is lowered into
// actual register/stack location records by the target backend. Still
// NOT a GC root-completeness proof and not a safe Fiber collection point.
static void emitRt001MachineStackmapProbe(
    llvm::Module& module, const char* tier) {
    const char* flag = std::getenv("XYO_RT_MACHINE_STACKMAP_PROBE");
    if (!flag || flag[0] != '1') return;
    const auto records = rt::emitExperimentalPostoptStackmaps(module);
    uint64_t emitted = 0;
    uint64_t refused = 0;
    for (const auto& site : records) {
        emitted += site.emitted;
        refused += !site.emitted;
        std::fprintf(stderr,
            "[rt001-machine-stackmap] tier=%s fn=%s site=%llu "
            "stackmap_id=%llu unknown_ptrs=%u "
            "context_ptrs=%d emitted=%d gc_safe=0\n",
            tier, site.functionName.c_str(),
            static_cast<unsigned long long>(site.sourceSite),
            static_cast<unsigned long long>(site.stackmapId),
            site.unknownPointerLocations,
            int(site.emitted), int(site.emitted));
    }
    std::fprintf(stderr,
        "[rt001-machine-stackmap-total] tier=%s sites=%zu "
        "emitted=%llu refused=%llu machine_roots_proven=0 "
        "gc_safe=0\n",
        tier, records.size(),
        static_cast<unsigned long long>(emitted),
        static_cast<unsigned long long>(refused));
}

// RT-001 yield-edge machine-PC geometry diagnostic. This is inserted
// after O2/O3 directly BEFORE a real xyo_yield/_fast call, not next to
// a detached SSA capture. It never authorizes GC on live native Fibers.
static void emitRt001YieldEdgeStackmapProbe(
    llvm::Module& module, const char* tier) {
    const char* flag = std::getenv("XYO_RT_YIELD_EDGE_STACKMAP_PROBE");
    if (!flag || flag[0] != '1') return;
    const auto sites = rt::emitExperimentalYieldEdgeStackmaps(module);
    size_t emitted = 0, refused = 0;
    for (const auto& site : sites) {
        emitted += site.emitted;
        refused += !site.emitted;
        std::fprintf(stderr,
            "[rt001-yield-edge-stackmap] tier=%s fn=%s "
            "ordinal=%u helper=%s stackmap_id=%llu "
            "locations=%u emitted=%d "
            "machine_roots_proven=0 gc_safe=0\n",
            tier, site.functionName.c_str(), site.ordinal,
            site.yieldHelper.c_str(),
            static_cast<unsigned long long>(site.id),
            site.locationCount, int(site.emitted));
    }
    std::fprintf(stderr,
        "[rt001-yield-edge-stackmap-total] tier=%s sites=%zu "
        "emitted=%zu refused=%zu machine_roots_proven=0 gc_safe=0\n",
        tier, sites.size(), emitted, refused);
    // Separate fail-closed census: a helper that yields *inside*
    // its own frame does not have a proven direct-call StackMap.
    // This is deliberately never promoted to a GC-safe result.
    // A second, independently named machine StackMap exposes
    // conservative live i64 + pointer SSA location counts. It must
    // run after the geometry probe, with direct-call identity checked
    // before adding any locations. This is NOT native GC rooting.
    const char* live = std::getenv(
        "XYO_RT_YIELD_LIVE_SSA_STACKMAP");
    if (live && live[0] == '1') {
        // FULL mixed i64+pointer machine StackMaps change Scratch
        // execution semantics in a real Unicode GC stress fixture.
        // They are therefore fail-closed, even behind an opt-in.
        // Separate diagnostic passes avoid that known bad combination
        // but they DO NOT prove that mixing roots is valid.
        const char* filter = std::getenv("XYO_RT_YIELD_SSA_FILTER");
        const bool i64Only = filter &&
            std::strcmp(filter, "i64") == 0;
        const bool i64NoTick = filter &&
            std::strcmp(filter, "i64_no_tick") == 0;
        const bool pointerOnly = filter &&
            std::strcmp(filter, "pointer") == 0;
        const bool contextOnly = filter &&
            std::strcmp(filter, "context") == 0;
        // Even pointer-ONLY has been proven to corrupt the native
        // GC-provenance Unicode negative-controls (x/z truncation).
        // Thus only i64-only and context-only are permitted to emit
        // a machine record in the actual runtime. Untrusted pointer
        // SSA stays compiler-only and never becomes a GC root.
        if (!i64Only && !i64NoTick && !contextOnly) {
            std::fprintf(stderr,
                "[rt001-yield-ssa-refused] tier=%s "
                "reason=%s "
                "ownership_proven=0 machine_roots_proven=0 gc_safe=0\n",
                tier, pointerOnly ? "unsafe_pointer_ssa_probe"
                                  : "unsafe_combined_ssa_probe");
        } else {
        rt::YieldSsaSelection selection = i64NoTick
            ? rt::YieldSsaSelection::I64WithoutSchedulerTicks
            : i64Only ? rt::YieldSsaSelection::I64Only
                      : rt::YieldSsaSelection::ContextOnly;
        uint32_t pointerLimit = UINT32_MAX;
        if (const char* cap = std::getenv(
                "XYO_RT_YIELD_SSA_POINTER_LIMIT")) {
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(cap, &end, 10);
            if (end && !*end && parsed <= UINT32_MAX)
                pointerLimit = static_cast<uint32_t>(parsed);
        }
        const auto snapshots =
            rt::emitExperimentalYieldSsaSnapshots(
                module, selection, pointerLimit,
                static_cast<uint32_t>(offsetof(JitCtx, yieldCheckN)),
                offsetof(JitCtx, varSlotData),
                offsetof(Variable, value));
        uint32_t inserted = 0;
        for (const auto& snapshot : snapshots) {
            inserted += snapshot.emitted;
            std::fprintf(stderr,
                "[rt001-yield-ssa-stackmap] tier=%s fn=%s "
                "ordinal=%u helper=%s stackmap_id=%llu "
                "i64=%u pointers=%u other_int=%u float=%u "
                "other_unsupported=%u expected_locations=%u "
                "liveness_valid=%d emitted=%d "
                "ownership_proven=0 machine_roots_proven=0 gc_safe=0\n",
                tier, snapshot.functionName.c_str(),
                snapshot.ordinal, snapshot.helper.c_str(),
                static_cast<unsigned long long>(
                    snapshot.stackmapId),
                snapshot.liveI64, snapshot.livePointer,
                snapshot.nonI64Numeric, snapshot.liveFloat,
                snapshot.otherUnsupported,
                snapshot.expectedLocations,
                int(snapshot.livenessValid),
                int(snapshot.emitted));
            if (i64NoTick) {
                std::fprintf(stderr,
                    "[rt001-yield-ssa-numeric-excluded] tier=%s fn=%s "
                    "ordinal=%u stackmap_id=%llu scheduler_ticks=%u "
                    "other_unproven_i64=%u "
                    "ownership_proven=0 machine_roots_proven=0 gc_safe=0\n",
                    tier, snapshot.functionName.c_str(),
                    snapshot.ordinal,
                    static_cast<unsigned long long>(
                        snapshot.stackmapId),
                    snapshot.excludedNumericSchedulerTicks,
                    snapshot.liveI64 -
                        snapshot.excludedNumericSchedulerTicks);
            }
            // Optional disassembly-to-LLVM operand provenance. Each
            // 1-based location index maps to exactly one post-O3 SSA
            // definition/argument; never print raw pointer contents.
            const char* traceOperands = std::getenv(
                "XYO_RT_YIELD_SSA_TRACE_OPERANDS");
            if (snapshot.emitted && traceOperands &&
                traceOperands[0] == '1') {
                for (const auto& operand : snapshot.sources) {
                    std::fprintf(stderr,
                        "[rt001-yield-ssa-operand] tier=%s fn=%s "
                        "ordinal=%u stackmap_id=%llu location=%u "
                        "family=%s source_id=%u opcode=%s "
                        "ownership_proven=0 gc_safe=0\n",
                        tier, snapshot.functionName.c_str(),
                        snapshot.ordinal,
                        static_cast<unsigned long long>(
                            snapshot.stackmapId),
                        operand.location, operand.family.c_str(),
                        operand.sourceId, operand.opcode.c_str());
                    // A candidate is only an LLVM producer provenance,
                    // not a heap membership proof or a GC root.
                    std::fprintf(stderr,
                        "[rt001-yield-ssa-origin] tier=%s fn=%s "
                        "ordinal=%u stackmap_id=%llu location=%u "
                        "source_id=%u origin=%s ownership_proven=0 "
                        "gc_safe=0\n",
                        tier, snapshot.functionName.c_str(),
                        snapshot.ordinal,
                        static_cast<unsigned long long>(
                            snapshot.stackmapId),
                        operand.location, operand.sourceId,
                        operand.origin.c_str());
                    if (operand.semantic != "unclassified") {
                        std::fprintf(stderr,
                            "[rt001-yield-ssa-semantic] tier=%s fn=%s "
                            "ordinal=%u stackmap_id=%llu location=%u "
                            "source_id=%u semantic=%s "
                            "root_kind=numeric_not_managed "
                            "gc_safe=0\n",
                            tier, snapshot.functionName.c_str(),
                            snapshot.ordinal,
                            static_cast<unsigned long long>(
                                snapshot.stackmapId),
                            operand.location, operand.sourceId,
                            operand.semantic.c_str());
                    }
                }
            }
        }
        std::fprintf(stderr,
            "[rt001-yield-ssa-stackmap-total] tier=%s "
            "sites=%zu emitted=%u refused=%zu "
            "ownership_proven=0 machine_roots_proven=0 gc_safe=0\n",
            tier, snapshots.size(), inserted,
            snapshots.size() - inserted);
        }
    }
    const auto unmapped = rt::auditUnmappedYieldEdges(module);
    for (const auto& site : unmapped) {
        std::fprintf(stderr,
            "[rt001-yield-edge-unmapped] tier=%s fn=%s "
            "ordinal=%u helper=%s reason=%s "
            "machine_roots_proven=0 gc_safe=0\n",
            tier, site.functionName.c_str(), site.ordinal,
            site.helperName.c_str(), site.reason.c_str());
    }
    std::fprintf(stderr,
        "[rt001-yield-edge-coverage] tier=%s "
        "direct_mapped=%zu direct_refused=%zu "
        "nested_or_unmapped=%zu "
        "machine_roots_proven=0 gc_safe=0\n",
        tier, emitted, refused, unmapped.size());
}

// Experimental native-translation StackMap, distinct from the
// existing RT-001 SSA capture StackMap. Values are sampled immediately
// before the reporter bridge, NOT at the parked Fiber's saved RIP.
// No GC or interpreter restoration permission follows from this probe.
static void emitRt001AsyncMachineStackmapProbe(
    llvm::Module& module, const char* tier) {
    const char* flag = std::getenv("XYO_RT_ASYNC_MACHINE_STACKMAP_PROBE");
    if (!flag || flag[0] != '1') return;
    const auto records = rt::emitExperimentalAsyncStackmaps(
        module, offsetof(JitCtx, varSlotData));
    const char* originFlag = std::getenv(
        "XYO_RT_ASYNC_POINTER_ORIGINS");
    const bool traceOrigins = originFlag && originFlag[0] == '1';
    uint32_t emitted = 0, refused = 0;
    for (const auto& site : records) {
        emitted += site.emitted;
        refused += !site.emitted;
        const auto& p = site.pointerRegions;
        std::fprintf(stderr,
            "[rt001-async-machine-stackmap] tier=%s fn=%s site=%u "
            "id=%llu ptr=%u stack=%u host=%u static=%u unknown=%u "
            "live_i64=%u expected_locations=%u emitted=%d "
            "machine_roots_proven=0 gc_safe=0\n",
            tier, site.functionName.c_str(), site.siteOrdinal,
            static_cast<unsigned long long>(site.stackmapId),
            site.livePointers,
            p[size_t(rt::PointerRegion::NativeStack)],
            p[size_t(rt::PointerRegion::HostContext)],
            p[size_t(rt::PointerRegion::StaticGlobal)],
            p[size_t(rt::PointerRegion::Unknown)],
            site.liveI64, 1u + site.livePointers + site.liveI64,
            int(site.emitted));
        if (traceOrigins && site.emitted) {
            for (const auto& pointer : site.pointerOrigins) {
                const char* regionName = "unknown";
                switch (pointer.region) {
                case rt::PointerRegion::NativeStack:
                    regionName = "native_stack"; break;
                case rt::PointerRegion::HostContext:
                    regionName = "host_context"; break;
                case rt::PointerRegion::StaticGlobal:
                    regionName = "static_global"; break;
                case rt::PointerRegion::Unknown:
                case rt::PointerRegion::Count: break;
                }
                std::fprintf(stderr,
                    "[rt001-async-pointer-origin] tier=%s fn=%s "
                    "site=%u stackmap_id=%llu location=%u "
                    "region=%s cause=%s def=%s callee=%s "
                    "chain_terminal=%s derefs=%u derives=%u host_offset=%lld "
                    "candidate_varslot=%lld ownership_proven=0 gc_safe=0\n",
                    tier, site.functionName.c_str(), site.siteOrdinal,
                    static_cast<unsigned long long>(site.stackmapId),
                    pointer.locationIndex, regionName,
                    rt::unknownPointerCauseName(pointer.unknownCause),
                    pointer.definitionKind.c_str(),
                    pointer.calleeName.empty() ? "none"
                                                : pointer.calleeName.c_str(),
                    pointer.hasUnknownLoadChain
                        ? rt::pointerLoadTerminalName(
                            pointer.unknownLoadChain.terminal)
                        : "none",
                    pointer.hasUnknownLoadChain
                        ? pointer.unknownLoadChain.dereferences : 0,
                    pointer.hasUnknownLoadChain
                        ? pointer.unknownLoadChain.addressDerivations : 0,
                    pointer.hasUnknownLoadChain &&
                        pointer.unknownLoadChain.hostContextFieldOffset
                        ? static_cast<long long>(
                            *pointer.unknownLoadChain.hostContextFieldOffset)
                        : -1LL,
                    pointer.candidateBorrowedVarSlot
                        ? static_cast<long long>(
                            *pointer.candidateBorrowedVarSlot)
                        : -1LL);
            }
        }
    }
    std::fprintf(stderr,
        "[rt001-async-machine-stackmap-total] tier=%s sites=%zu "
        "emitted=%u refused=%u machine_roots_proven=0 gc_safe=0\n",
        tier, records.size(), emitted, refused);
}

// The full LLVM optimizer and ORC machine-code materialization happen on
// this task's worker. All Scratch/GC-dependent IR emission and literal
// pinning has already finished on the runtime thread.
struct LLVMPreparedCompilation final : PendingJitCompilation {
    LLVMBackend* backend;
    llvm::orc::ThreadSafeModule module;
    std::unique_ptr<CompiledScript> result;
    std::string entryName;
    std::string osrEntryName;
    std::string osrLoopId;
    std::string nestedName;
    std::string nestedLoopId;
    std::string nestedCallId;
    std::string nestedParentCallId;
    std::vector<std::string> nestedCallPath;
    uint64_t nestedSourceRevision = 0;
    std::vector<std::string> nestedFramePath;
    std::vector<uint8_t> nestedBranchArms;
    uint32_t nestedArgFrameIndex = 0;
    std::map<std::string, std::string> procedures;
    std::string topId;

    LLVMPreparedCompilation(
        LLVMBackend* owner, llvm::orc::ThreadSafeModule ir,
        std::unique_ptr<CompiledScript> code, std::string name,
        std::map<std::string, std::string> procNames, std::string id,
        std::string osrName, std::string loopId,
        std::string nestedName_, std::string nestedLoop_,
        std::string nestedCall_, std::string nestedParentCall_,
        std::vector<std::string> nestedCallPath_,
        uint64_t nestedSourceRevision_,
        std::vector<std::string> nestedFramePath_,
        std::vector<uint8_t> nestedBranchArms_,
        uint32_t nestedArgFrameIndex_)
        : backend(owner), module(std::move(ir)), result(std::move(code)),
          entryName(std::move(name)), osrEntryName(std::move(osrName)),
          osrLoopId(std::move(loopId)), nestedName(std::move(nestedName_)),
          nestedLoopId(std::move(nestedLoop_)),
          nestedCallId(std::move(nestedCall_)),
          nestedParentCallId(std::move(nestedParentCall_)),
          nestedCallPath(std::move(nestedCallPath_)),
          nestedSourceRevision(nestedSourceRevision_),
          nestedFramePath(std::move(nestedFramePath_)),
          nestedBranchArms(std::move(nestedBranchArms_)),
          nestedArgFrameIndex(nestedArgFrameIndex_),
          procedures(std::move(procNames)), topId(std::move(id)) {}

    std::unique_ptr<CompiledScript> finish() override {
        if (!backend->jit) return {};
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        module.withModuleDo([](llvm::Module& ir) {
            llvm::PassBuilder pb;
            llvm::LoopAnalysisManager lam;
            llvm::FunctionAnalysisManager fam;
            llvm::CGSCCAnalysisManager cgam;
            llvm::ModuleAnalysisManager mam;
            pb.registerModuleAnalyses(mam);
            pb.registerCGSCCAnalyses(cgam);
            pb.registerFunctionAnalyses(fam);
            pb.registerLoopAnalyses(lam);
            pb.crossRegisterProxies(lam, fam, cgam, mam);
            auto mpm = pb.buildPerModuleDefaultPipeline(
                jitIrOptimizationLevel());
            mpm.run(ir, mam);
            auditIr001OptimizedWrites(ir, "prepared");
            if (const char* d = std::getenv("XYO_JIT_DUMP2"))
                if (*d) ir.print(llvm::errs(), nullptr);
        });
        if (auto error = backend->jit->addIRModule(std::move(module))) {
            llvm::consumeError(std::move(error));
            return {};
        }
        auto entry = backend->jit->lookup(entryName);
        if (!entry) {
            llvm::consumeError(entry.takeError());
            return {};
        }
        result->entry = reinterpret_cast<ScriptFn>(entry->getValue());
        if (!osrEntryName.empty()) {
            auto resume = backend->jit->lookup(osrEntryName);
            if (!resume) {
                llvm::consumeError(resume.takeError());
                return {};
            }
            result->osrEntry =
                reinterpret_cast<ScriptFn>(resume->getValue());
            result->osrLoopId = osrLoopId;
        }
        if (!nestedName.empty()) {
            auto nested = backend->jit->lookup(nestedName);
            if (!nested) {
                llvm::consumeError(nested.takeError());
                return {};
            }
            result->nestedOsrEntry =
                reinterpret_cast<ScriptFn>(nested->getValue());
            result->nestedOsrLoopId = nestedLoopId;
            result->nestedOsrCallId = nestedCallId;
            result->nestedOsrParentCallId = nestedParentCallId;
            result->nestedOsrCallPath = std::move(nestedCallPath);
            result->nestedOsrSourceRevision = nestedSourceRevision;
            result->nestedOsrFramePath = std::move(nestedFramePath);
            result->nestedOsrBranchArms = std::move(nestedBranchArms);
            result->nestedOsrArgFrameIndex = nestedArgFrameIndex;
        }
        for (const auto& [key, symbol] : procedures) {
            auto address = backend->jit->lookup(symbol);
            if (!address) {
                llvm::consumeError(address.takeError());
                continue;
            }
            result->procedures[key] =
                reinterpret_cast<ScriptFn>(address->getValue());
        }
        if (std::getenv("XYO_JIT_ASYNC_TIMING")) {
            const double ms = std::chrono::duration<double, std::milli>(
                Clock::now() - start).count();
            std::fprintf(stderr, "[jit-async] %s finished %.3fms\n",
                         topId.c_str(), ms);
        }
        return std::move(result);
    }
};

// A conservative static estimate of likely procedure calls. Follow only
// the script's main sequence, not conditional substack inputs. Count call
// sites rather than unique callees: a long test/dispatch sequence is likely
// to use a large fraction of its procedures even if names repeat.
static size_t countDirectProcedureCalls(const Blocks* blocks,
                                        const Block* top) {
    size_t calls = 0;
    std::unordered_set<const Block*> seen;
    while (top && seen.insert(top).second && seen.size() <= 200000) {
        calls += top->opcode == "procedures_call";
        top = top->next.empty() ? nullptr : blocks->getBlock(top->next);
    }
    return calls;
}

// On-demand materialization pays a trampoline and first-call compile cost.
// Use it automatically only for scripts with substantial cold-procedure
// potential. A script with many direct call sites is treated as eager.
// "on" is an explicit choice, independent of the HotSpot heuristic.
static bool chooseLazyCompile(JitLazyMode policy, JitMode mode,
                              size_t functions, size_t instructions,
                              size_t directCallSites) {
    if (policy == JitLazyMode::Off || mode == JitMode::Off)
        return false;
    if (policy == JitLazyMode::On)
        return functions >= 2;

    // Eager compiles every spawned script on its first invocation. An
    // exceptional, very large compilation unit can nevertheless contain
    // hundreds of cold procedures; optimizing and materializing *all* of
    // them before its entry executes caused a 10+ second GUI stall for a
    // 241-function/440k-IR Scratch parity dispatcher. Keep eager entry
    // compilation, but defer cold procedure bodies for units this large.
    // Smaller eager scripts retain their original fully materialized path.
    // Explicit lazy=off and XYO_JIT_FAST_START=0 both restore that path.
    if (mode == JitMode::Eager) {
        const char* fastStart = std::getenv("XYO_JIT_FAST_START");
        return (!fastStart || fastStart[0] != '0') &&
               functions >= 64 && instructions >= 100000;
    }

    if (mode != JitMode::HotSpot) return false;

    // A multi-procedure Scratch script can spend hundreds of milliseconds
    // optimizing and machine-code-generating routines before its first
    // instruction. Deferring those cold routines to first use cuts startup
    // without changing the selected O3 tier or interpreter semantics.
    // The threshold uses IR complexity, not a project or procedure name.
    // XYO_JIT_FAST_START=0 restores the original auto-tier heuristic.
    // Many direct procedure call sites make lazy trampolines a net loss.
    // This gate must take precedence over the large-IR fast-start heuristic.
    if (functions >= 32 && directCallSites * 4 > functions)
        return false;
    const char* fastStart = std::getenv("XYO_JIT_FAST_START");
    if ((!fastStart || fastStart[0] != '0') &&
        functions >= 4 && instructions >= 3000)
        return true;

    return functions >= 32 && directCallSites * 4 <= functions;
}

// The eager and on-demand engines use the same host calls and diagnostics.
static void configureLazyIR(llvm::orc::LLJIT& jit) {
    jit.getIRTransformLayer().setTransform(
        [](llvm::orc::ThreadSafeModule tsm,
           llvm::orc::MaterializationResponsibility&)
            -> llvm::Expected<llvm::orc::ThreadSafeModule> {
            tsm.withModuleDo([](llvm::Module& module) {
                if (!module.getModuleFlag("xyo.lazy.optimize"))
                    return;
                // PROF-009. Per-materialization metadata is opt-in. The
                // default lazy compiler never walks the IR for diagnostics.
                const char* detailEnv = std::getenv("XYO_JIT_LAZY_DETAIL");
                const bool detail = detailEnv && detailEnv[0] == '1' &&
                                    detailEnv[1] == '\0';
                size_t detailFunctions = 0, detailInstructions = 0;
                std::string detailFirst;
                if (detail) {
                    for (const llvm::Function& function : module) {
                        if (function.isDeclaration()) continue;
                        if (detailFirst.empty())
                            detailFirst = function.getName().str();
                        ++detailFunctions;
                        for (const llvm::BasicBlock& bb : function)
                            detailInstructions += bb.size();
                    }
                }
                const auto began = std::chrono::steady_clock::now();
                llvm::PassBuilder pb;
                llvm::LoopAnalysisManager lam;
                llvm::FunctionAnalysisManager fam;
                llvm::CGSCCAnalysisManager cgam;
                llvm::ModuleAnalysisManager mam;
                pb.registerModuleAnalyses(mam);
                pb.registerCGSCCAnalyses(cgam);
                pb.registerFunctionAnalyses(fam);
                pb.registerLoopAnalyses(lam);
                pb.crossRegisterProxies(lam, fam, cgam, mam);
                auto mpm = pb.buildPerModuleDefaultPipeline(
                    jitIrOptimizationLevel());
                mpm.run(module, mam);
                auditIr001OptimizedWrites(module, "lazy");
                auditRt001PostoptSsa(module, "lazy");
                auditRt001PostoptAsyncRoots(module, "lazy");
                emitRt001MachineStackmapProbe(module, "lazy");
                emitRt001AsyncMachineStackmapProbe(module, "lazy");
                emitRt001YieldEdgeStackmapProbe(module, "lazy");
                // The initial module is deliberately NOT optimized when
                // using lazy materialization. Dump here, after LLVM O3,
                // so DUMP2 consistently shows optimized IR in either mode.
                if (const char* dump = std::getenv("XYO_JIT_DUMP2"))
                    if (*dump) module.print(llvm::errs(), nullptr);
                const bool legacyTiming =
                    std::getenv("XYO_JIT_TIMING") != nullptr;
                if (legacyTiming || detail) {
                    const double elapsed =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - began)
                            .count();
                    if (legacyTiming)
                        std::fprintf(stderr, "[jit-lazy-ir] %.3fms\n",
                                     elapsed);
                    if (detail)
                        std::fprintf(stderr,
                            "[jit-lazy-detail] first=%s funcs=%zu "
                            "ir=%zu opt_ms=%.3f\n",
                            detailFirst.c_str(), detailFunctions,
                            detailInstructions, elapsed);
                }
            });
            return std::move(tsm);
        });
}

static void configureObjectDump(llvm::orc::LLJIT& jit) {
    // Inspect the exact JIT machine code, not an AOT approximation. Disabled
    // by default; prefix-N.o files contain the objects before linking.
    if (const char* p = std::getenv("XYO_JIT_DUMP_OBJECT")) {
        if (p[0]) jit.getObjTransformLayer().setTransform(
            [prefix = std::string(p)](std::unique_ptr<llvm::MemoryBuffer> object)
                -> llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> {
                static std::atomic<uint64_t> serial{0};
                const std::string path = prefix + "-" +
                    std::to_string(serial.fetch_add(1, std::memory_order_relaxed)) + ".o";
                if (FILE* out = std::fopen(path.c_str(), "wb")) {
                    const auto bytes = object->getBuffer();
                    const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), out);
                    const int closed = std::fclose(out);
                    if (written != bytes.size() || closed != 0)
                        std::fprintf(stderr, "xyo-jit: incomplete object dump %s\n", path.c_str());
                } else {
                    std::fprintf(stderr, "xyo-jit: cannot dump object %s\n", path.c_str());
                }
                return std::move(object);
            });
    }

}

static void registerRuntimeSymbols(llvm::orc::LLJIT& jit) {
    auto& es = jit.getExecutionSession();
    llvm::orc::SymbolMap symbols;
    // Opt-in ORC symbol specialization: the emitted call name (and its
    // yield-edge/StackMap identity) remains xyo_yield_fast. The direct
    // address bypasses its per-yield environment/chain dispatch entirely.
    // Fiber::resumeChain serializes when the required chain opt-out is set.
    // Never use this specialization under ASan.
    const auto flagEnabled = [](const char* key, char expected) {
        const char* value = std::getenv(key);
        return value && value[0] == expected && value[1] == '\0';
    };
#if defined(__SANITIZE_ADDRESS__)
    constexpr bool asan = true;
#elif defined(__clang__)
    constexpr bool asan = __has_feature(address_sanitizer);
#else
    constexpr bool asan = false;
#endif
    const bool directSwitch = !asan &&
        flagEnabled("XYO_JIT_DIRECT_SWITCH", '1') &&
        flagEnabled("XYO_FIBER_DIRECT_YIELD", '1') &&
        flagEnabled("XYO_FIBER_CHAIN", '0');
    for (auto& [name, addr] : kRtSymbols) {
        void* target = (directSwitch &&
            std::strcmp(name, "xyo_yield_fast") == 0)
            ? reinterpret_cast<void*>(&xyo_yield_fast_direct) : addr;
        symbols[es.intern(name)] = llvm::orc::ExecutorSymbolDef(
            llvm::orc::ExecutorAddr::fromPtr(target),
            llvm::JITSymbolFlags::Exported);
    }
    // バックエンドの libcall（fmod/log/bcmp 等）— 静的リンクでも解決できる
    for (auto& [name, addr] : hostLibcallSymbols()) {
        symbols[es.intern(name)] = llvm::orc::ExecutorSymbolDef(
            llvm::orc::ExecutorAddr::fromPtr(addr),
            llvm::JITSymbolFlags::Exported);
    }
    auto& jd = jit.getMainJITDylib();
    if (auto err =
            jd.define(llvm::orc::absoluteSymbols(std::move(symbols)))) {
        std::fprintf(stderr, "xyo-jit: symbol define failed\n");
        llvm::consumeError(std::move(err));
    }
    if (auto gen =
            llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
                jit.getDataLayout().getGlobalPrefix())) {
        jd.addGenerator(std::move(*gen));
    }
}

// Diagnostic opt-in only. AddressSanitizer reserves huge virtual ranges;
// lazy ORC can place module text and its trampoline/anonymous symbols more
// than 2 GiB apart. Keep the production code model and optimization options
// unchanged unless the caller explicitly requests a different model.
template <typename Builder>
static void configureExperimentalCodegen(Builder& builder, const char* engine) {
    const char* level = std::getenv("XYO_JIT_CODEGEN_OPT");
    const char* model = std::getenv("XYO_JIT_CODE_MODEL");
    // ASan reserves large sparse virtual-address regions. With the small
    // model, ORC lazy trampolines and code can end up >2 GiB apart, making
    // a Delta32 fixup fail and a call-through fall to address zero.
    // Large is only the sanitizer build default, never Release's default.
#if defined(__SANITIZE_ADDRESS__)
    constexpr bool asanLargeDefault = true;
#elif defined(__clang__)
    constexpr bool asanLargeDefault = __has_feature(address_sanitizer);
#else
    constexpr bool asanLargeDefault = false;
#endif
    // Override even in ASan builds: "large" forces Large, and
    // "default" reverts to LLVM's native target code model.
    const bool useLarge = model
        ? std::strcmp(model, "large") == 0
        : asanLargeDefault;
    if (!level && !useLarge) return;

    auto host = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!host) {
        std::fprintf(stderr,
            "xyo-jit: cannot configure %s target machine\n", engine);
        llvm::consumeError(host.takeError());
        return;
    }
    if (level) {
        auto opt = llvmc::codeGenOptDefault();
        if (std::strcmp(level, "less") == 0)
            opt = llvmc::codeGenOptLess();
        else if (std::strcmp(level, "none") == 0)
            opt = llvmc::codeGenOptNone();
        host->setCodeGenOptLevel(opt);
    }
    if (useLarge)
        host->setCodeModel(llvm::CodeModel::Large);
    builder.setJITTargetMachineBuilder(std::move(*host));
}

LLVMBackend::LLVMBackend() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    tsc = llvm::orc::ThreadSafeContext(
        std::make_unique<llvm::LLVMContext>());

    llvm::orc::LLJITBuilder builder;
    // Experimental backend-only compilation tiers. The IR optimization
    // pipeline stays unchanged; this isolates machine-code lowering cost
    // from O3's middle-end work when investigating large Scratch scripts.
    // The default target/code-generation settings are entirely unchanged.
    configureExperimentalCodegen(builder, "eager");
    // Auto lazy materialization is available in HotSpot and for
    // exceptionally large Eager compilation units. No secondary engine is
    // constructed until a script actually qualifies for lazy IR.
    lazyProcedures =
        jitMode() != JitMode::Off &&
        (jitLazyMode() == JitLazyMode::On ||
         (jitLazyMode() == JitLazyMode::Auto &&
          (jitMode() == JitMode::HotSpot ||
           jitMode() == JitMode::Eager)));
    auto result = builder.create();
    if (!result) {
        std::fprintf(stderr, "xyo-jit: LLJIT init failed\n");
        llvm::consumeError(result.takeError());
        return;
    }
    auto jit = std::move(*result);
    configureObjectDump(*jit);
#ifdef XYO_RT001_ORC_FDE_PLUGIN
    rt::installOrcFdeLifecycleProbe(*jit, "eager");
#endif
    registerRuntimeSymbols(*jit);
    this->jit = std::move(jit);

    // The backend keeps its emitted string constants alive until disposal.
    Gc::get().addRootProvider(this, [this](Gc& g) {
        for (SStr* s : pinned.strings) g.markPtr(s);
    });
}

bool LLVMBackend::ensureLazyJit() {
    if (!jit || !lazyProcedures) return false;
    if (lazyJit) return true;

    llvm::orc::LLLazyJITBuilder builder;
    configureExperimentalCodegen(builder, "lazy");
    // Lazy resolution may happen on different scheduler workers.
    // Separate code-generation contexts are required for concurrent LLVM.
    builder.setNumCompileThreads(2);
    auto result = builder.create();
    if (!result) {
        std::fprintf(stderr, "xyo-jit: lazy engine unavailable; using eager\n");
        llvm::consumeError(result.takeError());
        return false;
    }
    auto lazy = std::move(*result);
    configureLazyIR(*lazy);
    configureObjectDump(*lazy);
#ifdef XYO_RT001_ORC_FDE_PLUGIN
    rt::installOrcFdeLifecycleProbe(*lazy, "lazy");
#endif
    registerRuntimeSymbols(*lazy);
    lazyJit = std::move(lazy);
    return true;
}


LLVMBackend::~LLVMBackend() {
    if (jit) Gc::get().removeRootProvider(this);
}

// This deliberately has a narrow entry contract. The result is an actual
// single LLVM Function with a switch(pc) over separately source-verified,
// non-yielding Scratch scripts, NOT a trampoline of remaining native calls.
// The sequencer still owns thread order and JitCtx initialization.
std::shared_ptr<MegaDispatchCode> LLVMBackend::compileMegaDispatch(
    const std::vector<Thread*>& threads, Runtime& runtime, uint64_t serial) {
    if (!jit || threads.size() < 2 || threads.size() > 8 ||
        runtime.blockCountEnabled || std::getenv("XYO_JIT_CALLFRAME") ||
        std::getenv("XYO_PROF")) return {};
    Blocks* blocks = threads.front()->blockContainer;
    Target* target = threads.front()->target;
    if (!blocks || !target) return {};
    const uint64_t revision = blocks->revision();
    const std::string symbol = "xyo_mega_batch_" + std::to_string(serial);
    llvm::orc::ThreadSafeContext context(std::make_unique<llvm::LLVMContext>());
    std::unique_ptr<llvm::Module> module;
    bool okay = true;
    const auto batchTrace = [] {
        const char* v = std::getenv("XYO_JIT_MEGA_BATCH_TRACE");
        return v && v[0] == '1';
    };
    auto fail = [&](const char* why) {
        if (batchTrace()) std::fprintf(stderr, "[mega-batch-fail] %s\n", why);
        okay = false;
    };
    llvmc::withContext(context, [&](llvm::LLVMContext* lc) {
        module = std::make_unique<llvm::Module>(symbol, *lc);
        module->setDataLayout(jit->getDataLayout());
        module->setTargetTriple(llvmc::tripleArg(jit->getTargetTriple()));

        llvm::Type* ptr = llvm::PointerType::getUnqual(*lc);
        auto* dispatchType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(*lc), {ptr, llvm::Type::getInt32Ty(*lc)},
            false);
        auto* dispatch = llvm::Function::Create(
            dispatchType, llvm::Function::ExternalLinkage, symbol,
            module.get());
        auto* entry = llvm::BasicBlock::Create(*lc, "entry", dispatch);
        auto* invalid = llvm::BasicBlock::Create(*lc, "invalid", dispatch);
        llvm::IRBuilder<> ir(*lc);
        ir.SetInsertPoint(invalid);
        ir.CreateRetVoid();
        ir.SetInsertPoint(entry);
        auto* ctx = dispatch->getArg(0);
        auto* selector = ir.CreateSwitch(dispatch->getArg(1), invalid,
                                         unsigned(threads.size()));
        llvm::SmallVector<llvm::CallInst*, 8> calls;
        std::vector<llvm::Function*> original;
        original.reserve(threads.size());
        // Yield-free procedure functions kept as out-of-line calls (recursive
        // warp procedures cannot be fused into the entry).
        std::vector<llvm::Function*> keptProcs;
        std::unordered_set<std::string> allProcNames;

        for (size_t i = 0; i < threads.size(); ++i) {
            Thread* thread = threads[i];
            if (!thread || thread->target != target ||
                thread->blockContainer != blocks || !thread->isCompiled ||
                thread->jitCtx || thread->fiber || thread->osrActive ||
                thread->nestedOsrActive) {
                okay = false;
                return;
            }
            const auto* current =
                static_cast<const CompiledScript*>(thread->compiled);
            if (!current || !current->entry ||
                (current->mayYield && !current->cooperativeOnly) ||
                current->mainThread || current->wasm ||
                current->coroResume || current->aotSourceRevision) {
                okay = false;
                return;
            }
            const Block* top = blocks->getBlock(thread->topBlock);
            if (!top || top->opcode != "event_whenflagclicked") {
                okay = false;
                return;
            }
            auto captured = ir::ScratchCompilationUnit::capture(*blocks, top);
            if (!captured.valid() || captured.sourceRevision != revision) {
                okay = false;
                return;
            }
            const std::string childName = symbol + "_slot_" + std::to_string(i);
            CompiledScript compiled;
            std::vector<std::string> fnNames;
            std::map<std::string, std::string> procedures;
            if (!jitEmitScript(*module, *lc, runtime, target, blocks,
                               thread->topBlock, childName, compiled,
                               pinned, fnNames, procedures, nullptr,
                               nullptr, &captured) ||
                (compiled.mayYield && !compiled.cooperativeOnly) ||
                compiled.mainThread ||
                !compiled.gpuProgs.empty()) {
                okay = false;
                return;
            }
            auto* child = module->getFunction(childName);
            // Non-yielding scripts may still contain custom blocks. Fuse the
            // reachable acyclic procedure graph into the script entry so the
            // dispatcher keeps its single-executable-function invariant.
            if (!procedures.empty()) {
                const size_t procCount = procedures.size();
                const auto fused = mega_goto::fuse(
                    *module, childName, procedures, /*force=*/true);
                if (const char* dbg = std::getenv("XYO_JIT_MEGA_BATCH_TRACE");
                    dbg && dbg[0] == '1')
                    std::fprintf(stderr,
                        "[mega-proc] script=%s procs=%zu fused=%d reason=%s\n",
                        thread->topBlock.c_str(), procCount,
                        int(fused.fused), fused.reason);
                if (!fused.fused) {
                    // The ordinary emitter may already have inlined the
                    // entry call, leaving a recursive procedure referenced
                    // only by itself. Keep every procedure function that is
                    // still used anywhere in the module.
                    bool called = false;
                    for (const auto& [key, symbol] : procedures) {
                        (void)key;
                        llvm::Function* fn = module->getFunction(symbol);
                        if (fn && !fn->use_empty()) {
                            called = true;
                            break;
                        }
                    }
                    if (called) {
                        // A recursive warp procedure graph cannot be fused,
                        // but the emitter proved the whole entry graph is
                        // yield-free (compiled.mayYield == false). Keep the
                        // procedure functions and let the dispatcher call
                        // them natively: they never suspend, so no Fiber is
                        // needed on the batch worker.
                        if ((compiled.mayYield && !compiled.cooperativeOnly) ||
                            compiled.coroResume) {
                            fail("recursive graph still may yield");
                            return;
                        }
                    } else {
                        procedures.clear();
                    }
                }
            }
            for (const auto& [key, symbol] : procedures) {
                (void)key;
                allProcNames.insert(symbol);
            }
            if (!child || child->isDeclaration() ||
                child->arg_size() != 1) {
                okay = false;
                return;
            }
            original.push_back(child);
            auto* caseBlock = llvm::BasicBlock::Create(
                *lc, "script_" + std::to_string(i), dispatch);
            selector->addCase(
                llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(*lc), i), caseBlock);
            ir.SetInsertPoint(caseBlock);
            calls.push_back(ir.CreateCall(child, {ctx}));
            ir.CreateRetVoid();
        }
        if (blocks->revision() != revision) {
            okay = false;
            return;
        }
        // Erase now-unreachable procedure bodies (fusion internalized
        // them). Anything still used would be a hidden helper. A recursive
        // child keeps its procedure functions alive instead.
        for (auto it = module->begin(); it != module->end();) {
            llvm::Function* fn = &*it++;
            // `dispatch` has no in-module users despite its external
            // linkage; erasing it would free the switch below.
            if (fn == dispatch || fn->isDeclaration() ||
                std::find(original.begin(), original.end(), fn) !=
                    original.end())
                continue;
            if (!fn->use_empty()) {
                // Keep only procedure functions that belong to a
                // yield-free recursive graph.
                if (!allProcNames.count(fn->getName().str())) {
                    fail("used function is not a procedure");
                    return;
                }
                keptProcs.push_back(fn);
                continue;
            }
            fn->eraseFromParent();
        }
        // No untracked native helpers are permitted in this beta. This
        // guarantees that after inlining there is exactly ONE executable
        // LLVM Function, rather than a switch that simply calls out to
        // hidden per-script helpers.
        for (llvm::Function& fn : *module) {
            if (fn.isDeclaration() || &fn == dispatch) continue;
            if (std::find(original.begin(), original.end(), &fn) ==
                    original.end() &&
                std::find(keptProcs.begin(), keptProcs.end(), &fn) ==
                    keptProcs.end()) {
                fail("untracked helper function");
                return;
            }
        }
        llvm::InlineFunctionInfo inlineInfo;
        for (llvm::CallInst* call : calls) {
            if (!llvm::InlineFunction(*call, inlineInfo).isSuccess()) {
                fail("inline child into dispatcher");
                return;
            }
        }
        for (llvm::Function* child : original) {
            if (!child->use_empty()) {
                okay = false;
                return;
            }
            child->eraseFromParent();
        }
        if (llvm::verifyModule(*module, &llvm::errs())) {
            fail("verify module");
            return;
        }
        llvm::PassBuilder pb;
        llvm::LoopAnalysisManager lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager cgam;
        llvm::ModuleAnalysisManager mam;
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        auto pipeline = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O1);
        pipeline.run(*module, mam);
        if (llvm::verifyModule(*module, &llvm::errs())) fail("verify after passes");
        if (const char* trace = std::getenv("XYO_JIT_MEGA_BATCH_TRACE");
            trace && trace[0] == '1') {
            size_t definitions = 0, indirectCalls = 0;
            for (auto& fn : *module) {
                if (!fn.isDeclaration()) ++definitions;
                if (&fn == dispatch)
                    for (auto& bb : fn)
                        for (auto& instruction : bb)
                            if (auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                                call && !call->getCalledFunction()) ++indirectCalls;
            }
            std::fprintf(stderr,
                "[mega-batch] slots=%zu definitions=%zu indirect=%zu "
                "source_revision=%llu valid=%d\n",
                threads.size(), definitions, indirectCalls,
                static_cast<unsigned long long>(revision), int(okay));
        }
    });
    if (!okay || blocks->revision() != revision) return {};
    llvm::Error err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(module), context));
    if (err) {
        llvm::consumeError(std::move(err));
        return {};
    }
    auto address = jit->lookup(symbol);
    if (!address) {
        llvm::consumeError(address.takeError());
        return {};
    }
    auto result = std::make_shared<MegaDispatchCode>();
    result->entry = reinterpret_cast<MegaDispatchCode::Entry>(
        address->getValue());
    result->sourceBlocks = blocks;
    result->sourceRevision = revision;
    result->entries = uint32_t(threads.size());
    return result;
}

// Experimental whole-batch stackless continuation for exactly one
// ordinary source-certified repeat per green-flag Scratch script.
// All script bodies live IN the single LLVM function. The continuation
// pointer and repeat counter belong to Thread, not to a native C++ stack.
std::shared_ptr<MegaDispatchCode> LLVMBackend::compileMegaYieldBatch(
    const std::vector<Thread*>& threads, Runtime& runtime, uint64_t serial) {
    static const bool singleYield = [] {
        const char* receiver = std::getenv("XYO_JIT_MEGA_SINGLE_RECEIVER");
        const char* sender = std::getenv("XYO_JIT_MEGA_SINGLE_SENDER");
        return (receiver && receiver[0] == '1' && receiver[1] == '\0') ||
               (sender && sender[0] == '1' && sender[1] == '\0');
    }();
    if (!jit || threads.empty() || threads.size() > 8 ||
        (threads.size() == 1 && !singleYield) ||
        runtime.blockCountEnabled || std::getenv("XYO_PROF") ||
        std::getenv("XYO_JIT_CALLFRAME")) return {};
    static_assert(Thread::MegaReturnCapacity ==
                  mega_yield::kMaxProcedureDepth);
    static_assert(Thread::MegaScalarBindingCapacity >=
                  2 + mega_yield::kMaxProcedureDepth);
    Blocks* blocks = threads.front()->blockContainer;
    if (!blocks || !threads.front()->target) return {};
    const uint64_t revision = blocks->revision();
    static const bool directScalar = [] {
        const char* env = std::getenv("XYO_JIT_MEGA_DIRECT_SCALAR");
        return env && env[0] == '1';
    }();
    static const bool notifyWaits = [] {
        const char* env = std::getenv("XYO_WAIT_WAKE_REGISTRY");
        return env && env[0] == '1';
    }();
    static const bool requestScalarBatch = [] {
        const char* env = std::getenv("XYO_JIT_MEGA_SCALAR_BATCH");
        return env && env[0] == '1';
    }();
    static const bool requestMultiPcBatch = [] {
        const char* env = std::getenv("XYO_JIT_MEGA_MULTI_PC_BATCH");
        return env && env[0] == '1';
    }();
    // A single-Thread entry is created only for an explicitly enabled
    // experiment. Normal batch code generation stays unchanged otherwise.
    static const bool specializeSingle = [] {
        const char* mode = std::getenv("XYO_JIT_MEGA_SINGLE_SPECIALIZE");
        const char* single = std::getenv("XYO_JIT_MEGA_SINGLE_DISPATCH");
        return mode && mode[0] == '1' && mode[1] == '\0' &&
               single && single[0] == '1';
    }();
    static const bool repeatBurst = [] {
        const char* flag = std::getenv("XYO_JIT_MEGA_REPEAT_BURST");
        return flag && flag[0] == '1' && flag[1] == '\0';
    }() && specializeSingle;
    static const bool multiRepeatBurst = [] {
        const char* flag = std::getenv(
            "XYO_JIT_MEGA_MULTI_REPEAT_BURST");
        return flag && flag[0] == '1' && flag[1] == '\0';
    }() && repeatBurst;
    std::vector<mega_yield::Source> sources;
    sources.reserve(threads.size());
    for (Thread* thread : threads) {
        if (!thread || !thread->target ||
            thread->blockContainer != blocks ||
            !thread->isCompiled || thread->jitCtx || thread->fiber ||
            thread->osrActive || thread->nestedOsrActive ||
            thread->interpreterStarted) return {};
        const Block* top = blocks->getBlock(thread->topBlock);
        if (!top) return {};
        mega_yield::Source source;
        if (!mega_yield::prove(*blocks, *top, source) ||
            (!source.hasBenchRound &&
             !thread->target->lookupVariableById(source.variableId)) ||
            (source.hasBenchRound &&
             (!thread->target->lookupVariableById(source.benchRound.milliseconds.id) ||
              !thread->target->lookupVariableById(source.benchRound.endTime.id) ||
              !thread->target->lookupVariableById(source.benchRound.startTime.id) ||
              !thread->target->lookupVariableById(source.benchRound.round.id) ||
              !thread->target->lookupVariableById(source.benchRound.total.id) ||
              !thread->target->lookupVariableById(source.benchRound.count.id) ||
              !thread->target->lookupVariableById(source.benchRound.chunks.id) ||
              !thread->target->lookupVariableById(source.benchRound.bestRate.id))) ||
            !std::all_of(source.beginSets.begin(), source.beginSets.end(),
                [&](const auto& set) {
                    return thread->target->lookupVariableById(
                        set.variableId) != nullptr;
                }) ||
            (source.hasSecondChange &&
             !thread->target->lookupVariableById(
                 source.secondVariableId)) ||
            (source.kind == mega_yield::Source::Kind::BroadcastUntil &&
             !source.untilClockReporter &&
             !thread->target->lookupVariableById(
                 source.conditionVariableId)) ||
            (source.untilCompareVariable &&
             !thread->target->lookupVariableById(
                 source.untilOtherVariableId)) ||
            (source.untilClockElapsedMs &&
             !thread->target->lookupVariableById(
                 source.untilStartVariableId)) ||
            (source.hasBenchEnd &&
             (!thread->target->lookupVariableById("output") ||
              thread->target->lookupVariableById("output")->type !=
                  Variable::kListType)) ||
            (source.hasOuterUntil &&
             (!thread->target->lookupVariableById(
                  source.outerFirstSet.variableId) ||
              !thread->target->lookupVariableById(
                  source.outerSecondSet.variableId) ||
              (source.hasOuterFinalSet
                  ? (!thread->target->lookupVariableById(
                         source.outerFinalSet.variableId) ||
                     !thread->target->lookupVariableById(
                         source.outerFinalSet.readVariableId))
                  : !thread->target->lookupVariableById(
                        source.outerSuffix.variableId)))) ||
            (source.hasInnerAfterSets &&
             (!thread->target->lookupVariableById(
                  source.innerAfterFirstSet.variableId) ||
              !thread->target->lookupVariableById(
                  source.innerAfterSecondSet.variableId) ||
              (source.innerAfterFirstSet.readVariable &&
               !thread->target->lookupVariableById(
                   source.innerAfterFirstSet.readVariableId)) ||
              (source.innerAfterSecondSet.readVariable &&
               !thread->target->lookupVariableById(
                   source.innerAfterSecondSet.readVariableId)))) ||
            (source.hasConditional &&
             ((source.hasElseBranch &&
               !thread->target->lookupVariableById(
                   source.elseVariableId)) ||
              !thread->target->lookupVariableById(
                  source.conditionVariableId) ||
              (source.hasThenSecondChange &&
               !thread->target->lookupVariableById(
                   source.thenSecondVariableId)) ||
              (source.hasElseSecondChange &&
               !thread->target->lookupVariableById(
                   source.elseSecondVariableId)))) ||
            !std::all_of(source.returnSuffixes.begin(),
                         source.returnSuffixes.end(),
                [&](const auto& suffix) {
                    return thread->target->lookupVariableById(
                        suffix.variableId) != nullptr;
                })) return {};
        sources.push_back(std::move(source));
    }
    if (blocks->revision() != revision) return {};
    // This path has NO scheduler yields, helper calls, Type/GC side effects,
    // or LLVM return PCs. In particular, keep reverse wait notifications
    // on the ordinary entry until a proven equivalent bulk wake is added.
    const bool scalarBatch =
        requestScalarBatch && directScalar && !notifyWaits &&
        std::all_of(sources.begin(), sources.end(),
            [&](const auto& source) {
                return source.kind == mega_yield::Source::Kind::Scalar &&
                       !source.hasSecondChange &&
                       !source.hasConditional &&
                       source.returnSuffixes.empty() &&
                       source.delta == sources.front().delta;
            });
    const bool multiPcBatch =
        requestMultiPcBatch && requestScalarBatch &&
        directScalar && !notifyWaits &&
        std::all_of(sources.begin(), sources.end(),
            [](const auto& source) {
                return (source.kind == mega_yield::Source::Kind::Scalar ||
                        source.kind == mega_yield::Source::Kind::Repeat) &&
                       source.returnSuffixes.empty();
            });
    // A generic Mega batch may mix scalar, conditional or broadcast
    // sources. None can be run ahead without changing Scratch semantics.
    // Do not emit an unused LLVM burst function for those programs:
    // enabling this opt-in flag must not perturb unrelated codegen.
    const bool compileMultiRepeatBurst =
        multiRepeatBurst && multiPcBatch &&
        sources.size() >= 2 && sources.size() <= 8 &&
        std::all_of(sources.begin(), sources.end(),
            [](const auto& source) {
                return source.kind == mega_yield::Source::Kind::Repeat &&
                       !source.hasConditional && !source.hasElseBranch &&
                       !source.hasThenSecondChange &&
                       !source.hasElseSecondChange &&
                       source.returnSuffixes.empty();
            });
    const std::string symbol = "xyo_mega_resume_" + std::to_string(serial);
    llvm::orc::ThreadSafeContext context(std::make_unique<llvm::LLVMContext>());
    std::unique_ptr<llvm::Module> module;
    bool valid = true;
    llvmc::withContext(context, [&](llvm::LLVMContext* lc) {
        module = std::make_unique<llvm::Module>(symbol, *lc);
        module->setDataLayout(jit->getDataLayout());
        module->setTargetTriple(llvmc::tripleArg(jit->getTargetTriple()));
        auto* voidType = llvm::Type::getVoidTy(*lc);
        auto* ptrType = llvm::PointerType::getUnqual(*lc);
        auto* i32 = llvm::Type::getInt32Ty(*lc);
        auto* i64 = llvm::Type::getInt64Ty(*lc);
        auto* type = llvm::FunctionType::get(
            voidType,
            {ptrType, ptrType, ptrType, ptrType, ptrType, ptrType}, false);
        auto* dispatch = llvm::Function::Create(
            type, llvm::Function::ExternalLinkage, symbol, module.get());
        auto iter = dispatch->arg_begin();
        llvm::Argument* ctx = &*iter++;
        llvm::Argument* pcPtr = &*iter++;
        llvm::Argument* remainingPtr = &*iter++;
        llvm::Argument* returnStackPtr = &*iter++;
        llvm::Argument* returnDepthPtr = &*iter++;
        llvm::Argument* scalarBindings = &*iter;
        if (scalarBatch) {
            auto* batchType = llvm::FunctionType::get(
                voidType, {ptrType, i64}, false);
            auto* batch = llvm::Function::Create(
                batchType, llvm::Function::ExternalLinkage,
                symbol + "_scalar_batch", module.get());
            auto* values = batch->getArg(0);
            auto* count = batch->getArg(1);
            auto* begin = llvm::BasicBlock::Create(*lc, "bulk.start", batch);
            auto* header = llvm::BasicBlock::Create(*lc, "bulk.header", batch);
            auto* body = llvm::BasicBlock::Create(*lc, "bulk.body", batch);
            auto* finished = llvm::BasicBlock::Create(
                *lc, "bulk.finished", batch);
            llvm::IRBuilder<> bb(*lc);
            bb.SetInsertPoint(begin);
            bb.CreateBr(header);
            bb.SetInsertPoint(header);
            auto* index = bb.CreatePHI(i64, 2, "i");
            index->addIncoming(bb.getInt64(0), begin);
            bb.CreateCondBr(bb.CreateICmpULT(index, count), body, finished);
            bb.SetInsertPoint(body);
            auto* sourceSlot = bb.CreateInBoundsGEP(
                ptrType, values, index);
            auto* variable = bb.CreateLoad(ptrType, sourceSlot);
            auto* valuePtr = bb.CreateInBoundsGEP(
                bb.getInt8Ty(), variable,
                bb.getInt64(static_cast<uint64_t>(
                    offsetof(Variable, value))));
            auto* currentBits = bb.CreateLoad(i64, valuePtr);
            auto* currentNumber = bb.CreateBitCast(
                currentBits, llvm::Type::getDoubleTy(*lc));
            auto* added = bb.CreateFAdd(
                currentNumber,
                llvm::ConstantFP::get(
                    llvm::Type::getDoubleTy(*lc), sources.front().delta));
            // The host proves each operand is finite, and the source
            // delta is finite. Overflow can yield +/-inf, but not NaN.
            bb.CreateStore(bb.CreateBitCast(added, i64), valuePtr);
            auto* nextIndex = bb.CreateAdd(index, bb.getInt64(1));
            index->addIncoming(nextIndex, body);
            bb.CreateBr(header);
            bb.SetInsertPoint(finished);
            bb.CreateRetVoid();
            if (specializeSingle) {
                // The caller has already proved a single numeric binding.
                // One scalar FAdd is sufficient, without an array/loop.
                auto* single = llvm::Function::Create(
                    llvm::FunctionType::get(voidType, {ptrType}, false),
                    llvm::Function::ExternalLinkage,
                    symbol + "_scalar_single", module.get());
                llvm::IRBuilder<> sb(llvm::BasicBlock::Create(
                    *lc, "scalar.single", single));
                auto* valuePtr = sb.CreateInBoundsGEP(
                    sb.getInt8Ty(), single->getArg(0),
                    sb.getInt64(offsetof(Variable, value)));
                auto* oldBits = sb.CreateLoad(i64, valuePtr);
                auto* number = sb.CreateBitCast(
                    oldBits, llvm::Type::getDoubleTy(*lc));
                auto* added = sb.CreateFAdd(number, llvm::ConstantFP::get(
                    llvm::Type::getDoubleTy(*lc), sources.front().delta));
                sb.CreateStore(sb.CreateBitCast(added, i64), valuePtr);
                sb.CreateRetVoid();
            }
        }
        if (multiPcBatch) {
            // Scratch's source-PC and loop counter are Thread-owned. The
            // LLVM function sees only three explicitly validated pointers
            // per item. Each item is dispatched by its even/odd source PC,
            // preserving one regular Scratch turn per source per pass.
            static_assert(sizeof(MegaDispatchCode::MultiPcItem) ==
                          sizeof(void*) * 7);
            auto* bulkType = llvm::FunctionType::get(
                voidType, {ptrType, i64}, false);
            auto* bulk = llvm::Function::Create(
                bulkType, llvm::Function::ExternalLinkage,
                symbol + "_multi_pc_batch", module.get());
            auto* batchItems = bulk->getArg(0);
            auto* count = bulk->getArg(1);
            auto* begin = llvm::BasicBlock::Create(*lc, "multi.begin", bulk);
            auto* header = llvm::BasicBlock::Create(*lc, "multi.header", bulk);
            auto* current = llvm::BasicBlock::Create(*lc, "multi.current", bulk);
            auto* advance = llvm::BasicBlock::Create(*lc, "multi.advance", bulk);
            auto* finished = llvm::BasicBlock::Create(*lc, "multi.end", bulk);
            llvm::IRBuilder<> bb(*lc);
            bb.SetInsertPoint(begin);
            bb.CreateBr(header);
            bb.SetInsertPoint(header);
            auto* index = bb.CreatePHI(i64, 2, "i");
            index->addIncoming(bb.getInt64(0), begin);
            bb.CreateCondBr(bb.CreateICmpULT(index, count), current, finished);
            bb.SetInsertPoint(current);
            auto* item = bb.CreateInBoundsGEP(
                bb.getInt8Ty(), batchItems,
                bb.CreateMul(index, bb.getInt64(
                    sizeof(MegaDispatchCode::MultiPcItem))));
            auto* variable = bb.CreateLoad(ptrType, item);
            auto* secondField = bb.CreateInBoundsGEP(
                bb.getInt8Ty(), item, bb.getInt64(
                    offsetof(MegaDispatchCode::MultiPcItem, secondVariable)));
            auto* secondVariable = bb.CreateLoad(ptrType, secondField);
            auto* conditionField = bb.CreateInBoundsGEP(
                bb.getInt8Ty(), item, bb.getInt64(
                    offsetof(MegaDispatchCode::MultiPcItem,
                             conditionVariable)));
            auto* conditionVariable = bb.CreateLoad(ptrType, conditionField);
            auto* thenSecondField = bb.CreateInBoundsGEP(
                bb.getInt8Ty(), item, bb.getInt64(
                    offsetof(MegaDispatchCode::MultiPcItem,
                             thenSecondVariable)));
            auto* thenSecondVariable = bb.CreateLoad(ptrType, thenSecondField);
            auto* elseSecondField = bb.CreateInBoundsGEP(
                bb.getInt8Ty(), item, bb.getInt64(
                    offsetof(MegaDispatchCode::MultiPcItem,
                             elseSecondVariable)));
            auto* elseSecondVariable = bb.CreateLoad(ptrType, elseSecondField);
            auto* pcField = bb.CreateInBoundsGEP(
                bb.getInt8Ty(), item,
                bb.getInt64(offsetof(MegaDispatchCode::MultiPcItem, pc)));
            auto* pcAddress = bb.CreateLoad(ptrType, pcField);
            auto* remField = bb.CreateInBoundsGEP(
                bb.getInt8Ty(), item,
                bb.getInt64(offsetof(MegaDispatchCode::MultiPcItem, remaining)));
            auto* remAddress = bb.CreateLoad(ptrType, remField);
            auto* pcValue = bb.CreateLoad(i32, pcAddress);
            auto* choice = bb.CreateSwitch(pcValue, advance);
            // Each source case owns its own constant delta and repetitions.
            // No aggregation or reordering of floating-point additions.
            for (size_t i = 0; i < sources.size(); ++i) {
                const auto& source = sources[i];
                auto* sourceStart = llvm::BasicBlock::Create(
                    *lc, "multi.source_" + std::to_string(i) + ".start", bulk);
                auto* sourceResume = llvm::BasicBlock::Create(
                    *lc, "multi.source_" + std::to_string(i) + ".resume", bulk);
                auto* sourceWrite = llvm::BasicBlock::Create(
                    *lc, "multi.source_" + std::to_string(i) + ".write", bulk);
                auto* sourceDone = llvm::BasicBlock::Create(
                    *lc, "multi.source_" + std::to_string(i) + ".done", bulk);
                choice->addCase(bb.getInt32(uint32_t(2*i)), sourceStart);
                choice->addCase(bb.getInt32(uint32_t(2*i+1)), sourceResume);
                bb.SetInsertPoint(sourceStart);
                if (source.kind == mega_yield::Source::Kind::Repeat) {
                    bb.CreateStore(bb.getInt64(source.repetitions), remAddress);
                    bb.CreateBr(sourceResume);
                } else {
                    bb.CreateBr(sourceWrite);
                }
                bb.SetInsertPoint(sourceResume);
                if (source.kind == mega_yield::Source::Kind::Repeat) {
                    auto* left = bb.CreateLoad(i64, remAddress);
                    bb.CreateCondBr(
                        bb.CreateICmpEQ(left, bb.getInt64(0)),
                        sourceDone, sourceWrite);
                } else {
                    // One-shot scalar resume is invalid and unreachable after
                    // host validation. Never replay the completed write.
                    bb.CreateBr(sourceDone);
                }
                bb.SetInsertPoint(sourceWrite);
                const auto numberType = llvm::Type::getDoubleTy(*lc);
                const auto emitNumberAdd = [&](llvm::Value* target,
                                               double delta) {
                    auto* address = bb.CreateInBoundsGEP(
                        bb.getInt8Ty(), target,
                        bb.getInt64(offsetof(Variable, value)));
                    auto* bits = bb.CreateLoad(i64, address);
                    auto* number = bb.CreateBitCast(bits, numberType);
                    auto* added = bb.CreateFAdd(
                        number, llvm::ConstantFP::get(numberType, delta));
                    bb.CreateStore(bb.CreateBitCast(added, i64), address);
                };
                if (source.hasConditional) {
                    auto* conditionAddress = bb.CreateInBoundsGEP(
                        bb.getInt8Ty(), conditionVariable,
                        bb.getInt64(offsetof(Variable, value)));
                    auto* conditionBits = bb.CreateLoad(i64, conditionAddress);
                    auto* conditionNumber = bb.CreateBitCast(
                        conditionBits, numberType);
                    auto* literal = llvm::ConstantFP::get(
                        numberType, source.conditionLiteral);
                    llvm::Value* lhs = source.conditionLiteralLeft ?
                        static_cast<llvm::Value*>(literal) : conditionNumber;
                    llvm::Value* rhs = source.conditionLiteralLeft ?
                        static_cast<llvm::Value*>(conditionNumber) : literal;
                    llvm::Value* predicate = nullptr;
                    switch (source.comparison) {
                    case mega_yield::Source::Comparison::Greater:
                        predicate = bb.CreateFCmpOGT(lhs, rhs);
                        break;
                    case mega_yield::Source::Comparison::Less:
                        predicate = bb.CreateFCmpOLT(lhs, rhs);
                        break;
                    case mega_yield::Source::Comparison::Equal:
                        predicate = bb.CreateFCmpOEQ(lhs, rhs);
                        break;
                    }
                    auto* thenBlock = llvm::BasicBlock::Create(
                        *lc, "multi.if.then." + std::to_string(i), bulk);
                    auto* elseBlock = llvm::BasicBlock::Create(
                        *lc, "multi.if.else." + std::to_string(i), bulk);
                    auto* mergeBlock = llvm::BasicBlock::Create(
                        *lc, "multi.if.merge." + std::to_string(i), bulk);
                    bb.CreateCondBr(predicate, thenBlock, elseBlock);
                    bb.SetInsertPoint(thenBlock);
                    emitNumberAdd(variable, source.delta);
                    if (source.hasThenSecondChange)
                        emitNumberAdd(thenSecondVariable,
                                      source.thenSecondDelta);
                    bb.CreateBr(mergeBlock);
                    bb.SetInsertPoint(elseBlock);
                    if (source.hasElseBranch) {
                        emitNumberAdd(secondVariable, source.elseDelta);
                        if (source.hasElseSecondChange)
                            emitNumberAdd(elseSecondVariable,
                                          source.elseSecondDelta);
                    }
                    // Without ELSE, false is a genuine no-op, not a
                    // synthetic addition of zero to a variable.
                    bb.CreateBr(mergeBlock);
                    bb.SetInsertPoint(mergeBlock);
                } else {
                    emitNumberAdd(variable, source.delta);
                    if (source.hasSecondChange)
                        emitNumberAdd(secondVariable, source.secondDelta);
                }
                if (source.kind == mega_yield::Source::Kind::Repeat) {
                    auto* left = bb.CreateLoad(i64, remAddress);
                    bb.CreateStore(bb.CreateSub(left, bb.getInt64(1)),
                                   remAddress);
                    bb.CreateStore(bb.getInt32(uint32_t(2*i+1)), pcAddress);
                } else {
                    bb.CreateStore(bb.getInt32(UINT32_MAX), pcAddress);
                }
                bb.CreateBr(advance);
                bb.SetInsertPoint(sourceDone);
                bb.CreateStore(bb.getInt32(UINT32_MAX), pcAddress);
                bb.CreateBr(advance);
            }
            bb.SetInsertPoint(advance);
            auto* next = bb.CreateAdd(index, bb.getInt64(1));
            index->addIncoming(next, advance);
            bb.CreateBr(header);
            bb.SetInsertPoint(finished);
            bb.CreateRetVoid();
            if (specializeSingle) {
                // Inline the exact existing multi-PC state machine with a
                // constant trip count of one. O1 removes batch iteration,
                // count compare and per-item index arithmetic. All source
                // PC cases, FAdd order and Thread-owned state stay identical.
                auto* single = llvm::Function::Create(
                    llvm::FunctionType::get(voidType, {ptrType}, false),
                    llvm::Function::ExternalLinkage,
                    symbol + "_multi_pc_single", module.get());
                llvm::IRBuilder<> sb(llvm::BasicBlock::Create(
                    *lc, "multi.single", single));
                auto* call = sb.CreateCall(
                    bulk, {single->getArg(0), sb.getInt64(1)});
                sb.CreateRetVoid();
                llvm::InlineFunctionInfo info;
                if (!llvm::InlineFunction(*call, info).isSuccess()) {
                    valid = false;
                    return;
                }
            }
            if (compileMultiRepeatBurst) {
                // One native call spans several complete Scratch passes,
                // never executing one Thread's future iteration before
                // another Thread's current iteration. Reusing the verified
                // count-batch executor preserves writes to aliased variables
                // in the exact original Thread order (including FP rounding).
                auto* fn = llvm::Function::Create(
                    llvm::FunctionType::get(i64,
                        {ptrType, i64, i64}, false),
                    llvm::Function::ExternalLinkage,
                    symbol + "_multi_repeat_burst", module.get());
                auto* items = fn->getArg(0);
                auto* count = fn->getArg(1);
                auto* maxPasses = fn->getArg(2);
                auto* entryBlock = llvm::BasicBlock::Create(
                    *lc, "multi_burst.entry", fn);
                auto* passHead = llvm::BasicBlock::Create(
                    *lc, "multi_burst.pass", fn);
                auto* checkHead = llvm::BasicBlock::Create(
                    *lc, "multi_burst.check", fn);
                auto* checkBody = llvm::BasicBlock::Create(
                    *lc, "multi_burst.check_body", fn);
                auto* checkSecond = llvm::BasicBlock::Create(
                    *lc, "multi_burst.check_second", fn);
                auto* checkSecondBody = llvm::BasicBlock::Create(
                    *lc, "multi_burst.check_second_body", fn);
                auto* checkNext = llvm::BasicBlock::Create(
                    *lc, "multi_burst.check_next", fn);
                auto* afterCheck = llvm::BasicBlock::Create(
                    *lc, "multi_burst.after_check", fn);
                auto* finished = llvm::BasicBlock::Create(
                    *lc, "multi_burst.done", fn);
                auto* zero = llvm::BasicBlock::Create(
                    *lc, "multi_burst.zero", fn);
                llvm::IRBuilder<> mb(entryBlock);
                mb.CreateCondBr(
                    mb.CreateOr(
                        mb.CreateICmpEQ(maxPasses, mb.getInt64(0)),
                        mb.CreateICmpEQ(count, mb.getInt64(0))),
                    zero, passHead);
                mb.SetInsertPoint(zero);
                mb.CreateRet(mb.getInt64(0));
                mb.SetInsertPoint(passHead);
                auto* iteration = mb.CreatePHI(i64, 2, "pass");
                iteration->addIncoming(mb.getInt64(0), entryBlock);
                auto* nextPass = mb.CreateAdd(
                    iteration, mb.getInt64(1));
                mb.CreateCall(bulk, {items, count});
                mb.CreateBr(checkHead);
                mb.SetInsertPoint(checkHead);
                auto* itemIndex = mb.CreatePHI(i64, 2, "item");
                itemIndex->addIncoming(mb.getInt64(0), passHead);
                mb.CreateCondBr(
                    mb.CreateICmpULT(itemIndex, count),
                    checkBody, afterCheck);
                mb.SetInsertPoint(checkBody);
                auto* itemAddress = mb.CreateInBoundsGEP(
                    mb.getInt8Ty(), items,
                    mb.CreateMul(itemIndex, mb.getInt64(
                        sizeof(MegaDispatchCode::MultiPcItem))));
                // The first MultiPcItem field is Variable*; all supplied
                // items were checked by C++ for type and structural stamps.
                auto* variable = mb.CreateLoad(ptrType, itemAddress);
                auto* valueAddress = mb.CreateInBoundsGEP(
                    mb.getInt8Ty(), variable,
                    mb.getInt64(offsetof(Variable, value)));
                auto* bits = mb.CreateLoad(i64, valueAddress);
                auto* exponent = mb.CreateAnd(
                    bits, mb.getInt64(0x7ff0000000000000ull));
                auto* expMask = mb.getInt64(0x7ff0000000000000ull);
                mb.CreateCondBr(
                    mb.CreateICmpNE(exponent, expMask),
                    checkSecond, finished);
                mb.SetInsertPoint(checkSecond);
                // Source may contain two sequential writes, potentially
                // to the same Variable*. Verify the second operand too.
                // Null is the one-write source's ABI representation.
                auto* secondField = mb.CreateInBoundsGEP(
                    mb.getInt8Ty(), itemAddress, mb.getInt64(
                        offsetof(MegaDispatchCode::MultiPcItem,
                                 secondVariable)));
                auto* secondVariable = mb.CreateLoad(
                    ptrType, secondField);
                mb.CreateCondBr(
                    mb.CreateIsNotNull(secondVariable),
                    checkSecondBody, checkNext);
                mb.SetInsertPoint(checkSecondBody);
                auto* secondValueAddress = mb.CreateInBoundsGEP(
                    mb.getInt8Ty(), secondVariable, mb.getInt64(
                        offsetof(Variable, value)));
                auto* secondBits = mb.CreateLoad(i64, secondValueAddress);
                auto* secondExponent = mb.CreateAnd(
                    secondBits, expMask);
                mb.CreateCondBr(
                    mb.CreateICmpNE(secondExponent, expMask),
                    checkNext, finished);
                mb.SetInsertPoint(checkNext);
                auto* incremented = mb.CreateAdd(
                    itemIndex, mb.getInt64(1));
                itemIndex->addIncoming(incremented, checkNext);
                mb.CreateBr(checkHead);
                mb.SetInsertPoint(afterCheck);
                iteration->addIncoming(nextPass, afterCheck);
                mb.CreateCondBr(
                    mb.CreateICmpULT(nextPass, maxPasses),
                    passHead, finished);
                mb.SetInsertPoint(finished);
                mb.CreateRet(nextPass);
            }
            if (repeatBurst) {
                for (size_t slot = 0; slot < sources.size(); ++slot) {
                    const auto& source = sources[slot];
                    if (source.kind != mega_yield::Source::Kind::Repeat ||
                        source.hasSecondChange || source.hasConditional ||
                        source.hasElseBranch ||
                        source.hasThenSecondChange ||
                        source.hasElseSecondChange ||
                        !source.returnSuffixes.empty())
                        continue;
                    // Pure fixed-delta repeats can retain their accumulator
                    // in an FP register for one short scheduler time slice.
                    // No fast-math/reassociation: each FAdd rounds exactly
                    // as the corresponding individual Scratch iteration.
                    auto* fn = llvm::Function::Create(
                        llvm::FunctionType::get(i64,
                            {ptrType, ptrType, i64}, false),
                        llvm::Function::ExternalLinkage,
                        symbol + "_repeat_burst_" + std::to_string(slot),
                        module.get());
                    auto* addr = fn->getArg(0);
                    auto* remaining = fn->getArg(1);
                    auto* count = fn->getArg(2);
                    auto* entryBlock = llvm::BasicBlock::Create(
                        *lc, "burst.entry", fn);
                    auto* head = llvm::BasicBlock::Create(
                        *lc, "burst.loop", fn);
                    auto* finished = llvm::BasicBlock::Create(
                        *lc, "burst.done", fn);
                    auto* zero = llvm::BasicBlock::Create(
                        *lc, "burst.zero", fn);
                    llvm::IRBuilder<> rb(entryBlock);
                    auto* valPtr = rb.CreateInBoundsGEP(
                        rb.getInt8Ty(), addr,
                        rb.getInt64(offsetof(Variable, value)));
                    auto* originalBits = rb.CreateLoad(i64, valPtr);
                    auto* original = rb.CreateBitCast(
                        originalBits, llvm::Type::getDoubleTy(*lc));
                    rb.CreateCondBr(rb.CreateICmpEQ(
                        count, rb.getInt64(0)), zero, head);
                    rb.SetInsertPoint(zero);
                    rb.CreateRet(rb.getInt64(0));
                    rb.SetInsertPoint(head);
                    auto* index = rb.CreatePHI(i64, 2, "burst.index");
                    index->addIncoming(rb.getInt64(0), entryBlock);
                    auto* current = rb.CreatePHI(
                        llvm::Type::getDoubleTy(*lc), 2, "burst.acc");
                    current->addIncoming(original, entryBlock);
                    auto* next = rb.CreateFAdd(current,
                        llvm::ConstantFP::get(
                            llvm::Type::getDoubleTy(*lc), source.delta));
                    auto* nextIndex = rb.CreateAdd(index, rb.getInt64(1));
                    index->addIncoming(nextIndex, head);
                    current->addIncoming(next, head);
                    auto* negInf = llvm::ConstantFP::getInfinity(
                        llvm::Type::getDoubleTy(*lc), true);
                    auto* posInf = llvm::ConstantFP::getInfinity(
                        llvm::Type::getDoubleTy(*lc));
                    // The normal one-item dispatch would reject nonfinite
                    // values on its NEXT scheduler pass. Commit the first
                    // overflow and return immediately for safe fallback.
                    auto* finite = rb.CreateAnd(
                        rb.CreateFCmpOGT(next, negInf),
                        rb.CreateFCmpOLT(next, posInf));
                    rb.CreateCondBr(
                        rb.CreateAnd(rb.CreateICmpULT(nextIndex, count),
                                     finite),
                        head, finished);
                    rb.SetInsertPoint(finished);
                    rb.CreateStore(rb.CreateBitCast(next, i64), valPtr);
                    auto* left = rb.CreateLoad(i64, remaining);
                    rb.CreateStore(rb.CreateSub(left, nextIndex), remaining);
                    rb.CreateRet(nextIndex);
                }
            }
        }
        auto* entry = llvm::BasicBlock::Create(*lc, "entry", dispatch);
        auto* invalid = llvm::BasicBlock::Create(*lc, "invalid", dispatch);
        auto* finish = llvm::BasicBlock::Create(*lc, "finish", dispatch);
        auto* popReturn = llvm::BasicBlock::Create(
            *lc, "procedure.pop", dispatch);
        auto* loadReturn = llvm::BasicBlock::Create(
            *lc, "procedure.load_return", dispatch);
        llvm::IRBuilder<> b(*lc);
        b.SetInsertPoint(invalid);
        b.CreateStore(b.getInt32(UINT32_MAX), pcPtr);
        b.CreateStore(b.getInt32(0), returnDepthPtr);
        b.CreateRetVoid();
        b.SetInsertPoint(finish);
        b.CreateStore(b.getInt32(UINT32_MAX), pcPtr);
        b.CreateStore(b.getInt32(0), returnDepthPtr);
        b.CreateRetVoid();
        b.SetInsertPoint(popReturn);
        auto* depth = b.CreateLoad(i32, returnDepthPtr, "return_depth");
        b.CreateCondBr(
            b.CreateICmpEQ(depth, b.getInt32(0)), finish, loadReturn);
        b.SetInsertPoint(loadReturn);
        auto* safe = b.CreateICmpULE(
            depth, b.getInt32(Thread::MegaReturnCapacity));
        auto* readReturn = llvm::BasicBlock::Create(
            *lc, "procedure.read_return", dispatch);
        b.CreateCondBr(safe, readReturn, invalid);
        b.SetInsertPoint(readReturn);
        auto* topIndex = b.CreateSub(depth, b.getInt32(1));
        auto* returnPtr = b.CreateInBoundsGEP(
            i32, returnStackPtr, topIndex);
        auto* nextPc = b.CreateLoad(i32, returnPtr, "return_pc");
        b.CreateStore(topIndex, returnDepthPtr);
        b.CreateStore(nextPc, pcPtr);
        auto* returnSwitch = b.CreateSwitch(nextPc, invalid);
        b.SetInsertPoint(entry);
        auto* pc = b.CreateLoad(i32, pcPtr, "pc");
        auto* sw = b.CreateSwitch(pc, invalid);
        auto change = module->getOrInsertFunction(
            "xyo_var_change",
            llvm::FunctionType::get(
                voidType, {ptrType, ptrType, ptrType, i64}, false));
        auto notify = module->getOrInsertFunction(
            "xyo_wait_notify_var_store",
            llvm::FunctionType::get(
                voidType, {ptrType, ptrType}, false));

        // All constants and return addresses are baked into this ONE LLVM
        // function. Scratch Thread memory owns the only persistent frames.
        auto emitChange = [&](const std::string& variableId,
                              const std::u16string& variableName,
                              double delta, const std::string& label,
                              uint32_t bindingIndex) {
            auto* id = b.CreateGlobalString(variableId, symbol + label + "_id");
            std::vector<uint16_t> wide;
            wide.reserve(variableName.size() + 1);
            for (char16_t c : variableName) wide.push_back(uint16_t(c));
            wide.push_back(0);
            auto* data = llvm::ConstantDataArray::get(
                *lc, llvm::ArrayRef<uint16_t>(wide));
            auto* global = new llvm::GlobalVariable(
                *module, data->getType(), true,
                llvm::GlobalValue::PrivateLinkage, data,
                symbol + label + "_name");
            auto* name = b.CreateInBoundsGEP(
                data->getType(), global, {b.getInt32(0), b.getInt32(0)});
            auto* deltaBits = b.getInt64(Value::numCanonical(delta).bits);
            if (!directScalar) {
                b.CreateCall(change, {ctx, id, name, deltaBits});
                return;
            }
            // A bounded pointer array is owned by the currently selected
            // Thread, refreshed only after its complete binding generation
            // was checked by Sequencer. When any binding is missing, all
            // entries are null, so a slow helper may create variables
            // without invalidating another direct pointer in this slice.
            auto* slot = b.CreateInBoundsGEP(
                ptrType, scalarBindings, b.getInt32(bindingIndex));
            auto* variable = b.CreateLoad(ptrType, slot);
            auto* checkValue = llvm::BasicBlock::Create(
                *lc, label + ".numeric_guard", dispatch);
            auto* fastValue = llvm::BasicBlock::Create(
                *lc, label + ".fast_add", dispatch);
            auto* fallback = llvm::BasicBlock::Create(
                *lc, label + ".fallback", dispatch);
            auto* next = llvm::BasicBlock::Create(
                *lc, label + ".next", dispatch);
            b.CreateCondBr(b.CreateIsNotNull(variable), checkValue, fallback);
            b.SetInsertPoint(checkValue);
            static_assert(sizeof(Value) == sizeof(uint64_t));
            const auto offset = static_cast<uint64_t>(
                offsetof(Variable, value));
            auto* valuePtr = b.CreateInBoundsGEP(
                b.getInt8Ty(), variable, b.getInt64(offset));
            auto* current = b.CreateLoad(i64, valuePtr);
            // Scratch's cast::toNumber maps tagged NaN to zero. The fast
            // lane accepts only ordinary IEEE754 values (not NaNs), and
            // uses the original helper for tagged strings/bools/colors.
            auto* tag = b.CreateAnd(
                b.CreateLShr(current, b.getInt64(48)),
                b.getInt64(0xFFF8));
            auto* ieeeNumber = b.CreateICmpNE(
                tag, b.getInt64(0x7FF8));
            auto* asDouble = b.CreateBitCast(
                current, llvm::Type::getDoubleTy(*lc));
            auto* ordered = b.CreateFCmpORD(
                asDouble, llvm::ConstantFP::get(
                    llvm::Type::getDoubleTy(*lc), 0.0));
            b.CreateCondBr(b.CreateAnd(ieeeNumber, ordered),
                           fastValue, fallback);
            b.SetInsertPoint(fastValue);
            auto* result = b.CreateFAdd(
                asDouble, llvm::ConstantFP::get(
                    llvm::Type::getDoubleTy(*lc), delta));
            // A finite delta plus an ordered IEEE number cannot yield
            // NaN, but retain explicit canonicalization at the ABI edge.
            auto* isNaN = b.CreateFCmpUNO(result, result);
            auto* resultBits = b.CreateSelect(
                isNaN, b.getInt64(val::kNan),
                b.CreateBitCast(result, i64));
            b.CreateStore(resultBits, valuePtr);
            if (notifyWaits)
                b.CreateCall(notify, {ctx, variable});
            b.CreateBr(next);
            b.SetInsertPoint(fallback);
            b.CreateCall(change, {ctx, id, name, deltaBits});
            b.CreateBr(next);
            b.SetInsertPoint(next);
        };
        // The outer repeat's two setters and its completion suffix are
        // emitted as explicit helper calls. Do not use the optional direct
        // scalar slot here: those slots were proved for the INNER suffix,
        // and aliasing them would write the wrong Scratch variable.
        const auto emitOuterSet = [&](const mega_yield::Source::OuterSet& set,
                                      const std::string& label) {
            auto* id = b.CreateGlobalString(
                set.variableId, symbol + label + "_id");
            std::vector<uint16_t> wide;
            wide.reserve(set.variableName.size() + 1);
            for (char16_t ch : set.variableName)
                wide.push_back(uint16_t(ch));
            wide.push_back(0);
            auto* array = llvm::ConstantDataArray::get(
                *lc, llvm::ArrayRef<uint16_t>(wide));
            auto* global = new llvm::GlobalVariable(
                *module, array->getType(), true,
                llvm::GlobalValue::PrivateLinkage, array,
                symbol + label + "_name");
            auto* name = b.CreateInBoundsGEP(
                array->getType(), global, {b.getInt32(0), b.getInt32(0)});
            llvm::Value* value = b.getInt64(
                Value::numCanonical(set.literal).bits);
            if (set.clock) {
                auto days = module->getOrInsertFunction(
                    "xyo_sns_dayssince2000",
                    llvm::FunctionType::get(i64, {ptrType}, false));
                value = b.CreateCall(days, {ctx});
            } else if (set.readVariable) {
                auto* readId = b.CreateGlobalString(
                    set.readVariableId, symbol + label + "_read_id");
                std::vector<uint16_t> readWide;
                readWide.reserve(set.readVariableName.size() + 1);
                for (char16_t ch : set.readVariableName)
                    readWide.push_back(uint16_t(ch));
                readWide.push_back(0);
                auto* readArray = llvm::ConstantDataArray::get(
                    *lc, llvm::ArrayRef<uint16_t>(readWide));
                auto* readGlobal = new llvm::GlobalVariable(
                    *module, readArray->getType(), true,
                    llvm::GlobalValue::PrivateLinkage, readArray,
                    symbol + label + "_read_name");
                auto* readName = b.CreateInBoundsGEP(
                    readArray->getType(), readGlobal,
                    {b.getInt32(0), b.getInt32(0)});
                auto getValue = module->getOrInsertFunction(
                    "xyo_var_get", llvm::FunctionType::get(
                        i64, {ptrType, ptrType, ptrType}, false));
                value = b.CreateCall(
                    getValue, {ctx, readId, readName});
            }
            auto setFn = module->getOrInsertFunction(
                "xyo_var_set", llvm::FunctionType::get(
                    voidType, {ptrType, ptrType, ptrType, i64}, false));
            b.CreateCall(setFn, {ctx, id, name, value});
        };
        const auto emitOuterSuffix = [&](const mega_yield::ReturnSuffix& end,
                                         const std::string& label) {
            auto* id = b.CreateGlobalString(
                end.variableId, symbol + label + "_id");
            std::vector<uint16_t> wide;
            wide.reserve(end.variableName.size() + 1);
            for (char16_t ch : end.variableName)
                wide.push_back(uint16_t(ch));
            wide.push_back(0);
            auto* array = llvm::ConstantDataArray::get(
                *lc, llvm::ArrayRef<uint16_t>(wide));
            auto* global = new llvm::GlobalVariable(
                *module, array->getType(), true,
                llvm::GlobalValue::PrivateLinkage, array,
                symbol + label + "_name");
            auto* name = b.CreateInBoundsGEP(
                array->getType(), global, {b.getInt32(0), b.getInt32(0)});
            b.CreateCall(change, {ctx, id, name,
                b.getInt64(Value::numCanonical(end.delta).bits)});
        };
        // Direct LLVM lowering of the exact, source-certified ScratchBench
        // bench_round procedure. Every variable is fetched in the original
        // Scratch evaluation order and every write uses the usual tagged
        // native helper (including wait-variable notifications).
        // No temporaries or native CallFrame survive a joined broadcast.
        const auto emitBenchRound = [&](const mega_yield::Source::BenchRound& spec,
                                        size_t sourceIndex) {
            const std::string label = "_bench_round_" +
                std::to_string(sourceIndex);
            const auto refs = [&](const mega_yield::Source::BenchVar& var,
                                  const std::string& suffix) {
                auto* id = b.CreateGlobalString(
                    var.id, symbol + label + "_" + suffix + "_id");
                std::vector<uint16_t> wide;
                wide.reserve(var.name.size() + 1);
                for (char16_t ch : var.name)
                    wide.push_back(uint16_t(ch));
                wide.push_back(0);
                auto* array = llvm::ConstantDataArray::get(
                    *lc, llvm::ArrayRef<uint16_t>(wide));
                auto* global = new llvm::GlobalVariable(
                    *module, array->getType(), true,
                    llvm::GlobalValue::PrivateLinkage, array,
                    symbol + label + "_" + suffix + "_name");
                auto* name = b.CreateInBoundsGEP(
                    array->getType(), global,
                    {b.getInt32(0), b.getInt32(0)});
                return std::pair<llvm::Value*, llvm::Value*>{id, name};
            };
            const auto get = [&](const mega_yield::Source::BenchVar& var,
                                 const std::string& suffix) -> llvm::Value* {
                auto [id, name] = refs(var, suffix);
                auto getFn = module->getOrInsertFunction(
                    "xyo_var_get", llvm::FunctionType::get(
                        i64, {ptrType, ptrType, ptrType}, false));
                return b.CreateCall(getFn, {ctx, id, name});
            };
            const auto set = [&](const mega_yield::Source::BenchVar& var,
                                 const std::string& suffix, llvm::Value* value) {
                auto [id, name] = refs(var, suffix);
                auto setFn = module->getOrInsertFunction(
                    "xyo_var_set", llvm::FunctionType::get(
                        voidType, {ptrType, ptrType, ptrType, i64}, false));
                b.CreateCall(setFn, {ctx, id, name, value});
            };
            const auto add = [&](const mega_yield::Source::BenchVar& var,
                                 const std::string& suffix, llvm::Value* value) {
                auto [id, name] = refs(var, suffix);
                b.CreateCall(change, {ctx, id, name, value});
            };
            const auto binary = [&](const char* fn,
                                    llvm::Value* lhs,
                                    llvm::Value* rhs) -> llvm::Value* {
                auto callee = module->getOrInsertFunction(
                    fn, llvm::FunctionType::get(i64, {i64, i64}, false));
                return b.CreateCall(callee, {lhs, rhs});
            };
            const auto greater = [&](llvm::Value* lhs, llvm::Value* rhs)
                                -> llvm::Value* {
                auto gt = module->getOrInsertFunction(
                    "xyo_gt", llvm::FunctionType::get(i32, {i64, i64}, false));
                return b.CreateICmpNE(
                    b.CreateCall(gt, {lhs, rhs}), b.getInt32(0));
            };

            // First statement: b_ms = (b_t1 - b_t0) * 86400000.
            // The order of reads, subtract and multiply matches the
            // source reporter tree; do not reassociate under fast-math.
            auto* finishTime = get(spec.endTime, "end_time");
            auto* startTime = get(spec.startTime, "start_time");
            auto* diff = binary("xyo_sub", finishTime, startTime);
            auto* elapsed = binary("xyo_mul", diff,
                b.getInt64(Value::numCanonical(86400000.0).bits));
            set(spec.milliseconds, "ms", elapsed);

            // Second statement: IF b_round > 0 then apply the two
            // accumulators and the best-rate update.
            auto* round = get(spec.round, "round_cond");
            auto* positive = greater(round,
                b.getInt64(Value::numCanonical(0.0).bits));
            auto* thenBlock = llvm::BasicBlock::Create(
                *lc, "bench_round." + std::to_string(sourceIndex) + ".then",
                dispatch);
            auto* after = llvm::BasicBlock::Create(
                *lc, "bench_round." + std::to_string(sourceIndex) + ".after",
                dispatch);
            b.CreateCondBr(positive, thenBlock, after);
            b.SetInsertPoint(thenBlock);

            add(spec.total, "total", get(spec.milliseconds, "ms_total"));
            add(spec.chunks, "chunks", get(spec.count, "n_chunks"));
            // The current rate reporter is read during the comparison.
            // When the IF succeeds the second Scratch reporter evaluates
            // b_n / b_ms again for the assignment (no hoisting).
            auto* numerator = get(spec.count, "n_rate_cmp");
            auto* denominator = get(spec.milliseconds, "ms_rate_cmp");
            auto* rate = binary("xyo_div", numerator, denominator);
            auto* best = get(spec.bestRate, "best_cmp");
            auto* improve = greater(rate, best);
            auto* update = llvm::BasicBlock::Create(
                *lc, "bench_round." + std::to_string(sourceIndex) + ".best",
                dispatch);
            auto* afterBest = llvm::BasicBlock::Create(
                *lc, "bench_round." + std::to_string(sourceIndex) + ".best_end",
                dispatch);
            b.CreateCondBr(improve, update, afterBest);
            b.SetInsertPoint(update);
            auto* newNumerator = get(spec.count, "n_rate_set");
            auto* newDenominator = get(spec.milliseconds, "ms_rate_set");
            set(spec.bestRate, "best_set",
                binary("xyo_div", newNumerator, newDenominator));
            b.CreateBr(afterBest);
            b.SetInsertPoint(afterBest);
            b.CreateBr(after);

            // Final unconditional statement: change b_round by 1.
            b.SetInsertPoint(after);
            add(spec.round, "round_add",
                b.getInt64(Value::numCanonical(1.0).bits));
        };
        uint32_t nextReturnPc = uint32_t(sources.size() * 2);
        for (size_t i = 0; i < sources.size(); ++i) {
            const auto& source = sources[i];
            auto* start = llvm::BasicBlock::Create(
                *lc, "script_" + std::to_string(i) + ".start", dispatch);
            auto* resume = llvm::BasicBlock::Create(
                *lc, "script_" + std::to_string(i) + ".resume", dispatch);
            auto* body = llvm::BasicBlock::Create(
                *lc, "script_" + std::to_string(i) + ".body", dispatch);
            auto* done = llvm::BasicBlock::Create(
                *lc, "script_" + std::to_string(i) + ".done", dispatch);
            sw->addCase(b.getInt32(uint32_t(2*i)), start);
            sw->addCase(b.getInt32(uint32_t(2*i+1)), resume);

            std::vector<uint32_t> returnPcs;
            returnPcs.reserve(source.returnSuffixes.size());
            for (size_t j = 0; j < source.returnSuffixes.size(); ++j)
                returnPcs.push_back(nextReturnPc++);

            llvm::BasicBlock* outerInit = nullptr;
            llvm::BasicBlock* outerFinish = nullptr;
            b.SetInsertPoint(start);
            // The source-proven bench_begin executes exactly once on
            // initial entry, never on join reentry.
            for (size_t j = 0; j < source.beginSets.size(); ++j)
                emitOuterSet(source.beginSets[j],
                    "_bench_begin_" + std::to_string(i) + "_" +
                    std::to_string(j));
            // The call chain is statically proven acyclic. Push its return
            // PC values OUTERMOST -> INNERMOST, so LIFO executes caller
            // suffixes from the innermost frame to the green-flag script.
            for (size_t j = 0; j < returnPcs.size(); ++j) {
                auto* slot = b.CreateInBoundsGEP(
                    i32, returnStackPtr, b.getInt32(uint32_t(j)));
                b.CreateStore(b.getInt32(returnPcs[j]), slot);
            }
            b.CreateStore(
                b.getInt32(uint32_t(returnPcs.size())), returnDepthPtr);
            if (source.kind == mega_yield::Source::Kind::Scalar) {
                b.CreateBr(body);
                b.SetInsertPoint(resume);
                // A one-shot scalar script has no valid resume state.
                // Reject accidental reentry rather than replaying effects.
                b.CreateBr(invalid);
            } else if (source.kind == mega_yield::Source::Kind::BroadcastWait) {
                b.CreateStore(b.getInt32(uint32_t(2*i+1)), pcPtr);
                b.CreateRetVoid();
                b.SetInsertPoint(resume);
                b.CreateBr(body);
            } else if (source.kind == mega_yield::Source::Kind::BroadcastUntil) {
                // Both first entry and post-join reentry land on this LLVM
                // condition block. The test is evaluated anew after every
                // completed receiver join, never cached across a yield.
                // This is a strict source-certified scalar vs literal
                // comparison; use Scratch's tagged Value helpers, not a
                // speculative floating-point comparison.
                auto* conditionBlock = llvm::BasicBlock::Create(
                    *lc, "script_" + std::to_string(i) + ".until.condition",
                    dispatch);
                auto* launch = llvm::BasicBlock::Create(
                    *lc, "script_" + std::to_string(i) + ".until.launch",
                    dispatch);
                if (source.hasOuterUntil) {
                    outerInit = llvm::BasicBlock::Create(
                        *lc, "script_" + std::to_string(i) + ".outer.init",
                        dispatch);
                    outerFinish = llvm::BasicBlock::Create(
                        *lc, "script_" + std::to_string(i) + ".outer.finish",
                        dispatch);
                    b.CreateStore(
                        b.getInt64(source.repetitions), remainingPtr);
                    b.CreateCondBr(
                        b.getInt1(source.repetitions == 0),
                        outerFinish, outerInit);
                    b.SetInsertPoint(outerInit);
                    emitOuterSet(source.outerFirstSet,
                                 "_outer_first_" + std::to_string(i));
                    emitOuterSet(source.outerSecondSet,
                                 "_outer_second_" + std::to_string(i));
                }
                b.CreateBr(conditionBlock);
                b.SetInsertPoint(resume);
                b.CreateBr(conditionBlock);
                b.SetInsertPoint(conditionBlock);
                auto* conditionId = b.CreateGlobalString(
                    source.conditionVariableId,
                    symbol + "_until_condition_id_" + std::to_string(i));
                std::vector<uint16_t> wide;
                wide.reserve(source.conditionVariableName.size() + 1);
                for (char16_t ch : source.conditionVariableName)
                    wide.push_back(uint16_t(ch));
                wide.push_back(0);
                auto* array = llvm::ConstantDataArray::get(
                    *lc, llvm::ArrayRef<uint16_t>(wide));
                auto* global = new llvm::GlobalVariable(
                    *module, array->getType(), true,
                    llvm::GlobalValue::PrivateLinkage, array,
                    symbol + "_until_condition_name_" + std::to_string(i));
                auto* conditionName = b.CreateInBoundsGEP(
                    array->getType(), global,
                    {b.getInt32(0), b.getInt32(0)});
                auto getValue = module->getOrInsertFunction(
                    "xyo_var_get", llvm::FunctionType::get(
                        i64, {ptrType, ptrType, ptrType}, false));
                llvm::Value* current = nullptr;
                if (source.untilClockReporter) {
                    // Deliberately use the identical synchronous runtime
                    // reporter as regular JIT, not an approximate clock
                    // or a 1ms scheduler snapshot. A time condition can
                    // change between any two broadcasts in one frame.
                    auto getDays = module->getOrInsertFunction(
                        "xyo_sns_dayssince2000",
                        llvm::FunctionType::get(i64, {ptrType}, false));
                    current = b.CreateCall(getDays, {ctx});
                    if (source.untilClockElapsedMs) {
                        auto* startId = b.CreateGlobalString(
                            source.untilStartVariableId,
                            symbol + "_until_start_id_" + std::to_string(i));
                        std::vector<uint16_t> startWide;
                        startWide.reserve(source.untilStartVariableName.size() + 1);
                        for (char16_t ch : source.untilStartVariableName)
                            startWide.push_back(uint16_t(ch));
                        startWide.push_back(0);
                        auto* startArray = llvm::ConstantDataArray::get(
                            *lc, llvm::ArrayRef<uint16_t>(startWide));
                        auto* startGlobal = new llvm::GlobalVariable(
                            *module, startArray->getType(), true,
                            llvm::GlobalValue::PrivateLinkage, startArray,
                            symbol + "_until_start_name_" + std::to_string(i));
                        auto* startName = b.CreateInBoundsGEP(
                            startArray->getType(), startGlobal,
                            {b.getInt32(0), b.getInt32(0)});
                        auto* start = b.CreateCall(
                            getValue, {ctx, startId, startName});
                        // xyo_sub and xyo_mul preserve the same two
                        // canonicalizations and operand evaluation order
                        // as ordinary boxed Scratch JIT evaluation.
                        auto subtract = module->getOrInsertFunction(
                            "xyo_sub", llvm::FunctionType::get(
                                i64, {i64, i64}, false));
                        auto multiply = module->getOrInsertFunction(
                            "xyo_mul", llvm::FunctionType::get(
                                i64, {i64, i64}, false));
                        auto* diff = b.CreateCall(
                            subtract, {current, start});
                        current = b.CreateCall(multiply, {
                            diff,
                            b.getInt64(Value::numCanonical(86400000.0).bits)
                        });
                    }
                } else {
                    current = b.CreateCall(
                        getValue, {ctx, conditionId, conditionName});
                }
                const char* op = nullptr;
                switch (source.comparison) {
                case mega_yield::Source::Comparison::Greater:
                    op = "xyo_gt"; break;
                case mega_yield::Source::Comparison::Less:
                    op = "xyo_lt"; break;
                case mega_yield::Source::Comparison::Equal:
                    op = "xyo_eq"; break;
                }
                auto compare = module->getOrInsertFunction(
                    op, llvm::FunctionType::get(
                        i32, {i64, i64}, false));
                llvm::Value* literal = b.getInt64(
                    Value::numCanonical(source.conditionLiteral).bits);
                if (source.untilCompareVariable) {
                    auto* otherId = b.CreateGlobalString(
                        source.untilOtherVariableId,
                        symbol + "_until_other_id_" + std::to_string(i));
                    std::vector<uint16_t> otherWide;
                    otherWide.reserve(
                        source.untilOtherVariableName.size() + 1);
                    for (char16_t ch : source.untilOtherVariableName)
                        otherWide.push_back(uint16_t(ch));
                    otherWide.push_back(0);
                    auto* otherArray = llvm::ConstantDataArray::get(
                        *lc, llvm::ArrayRef<uint16_t>(otherWide));
                    auto* otherGlobal = new llvm::GlobalVariable(
                        *module, otherArray->getType(), true,
                        llvm::GlobalValue::PrivateLinkage, otherArray,
                        symbol + "_until_other_name_" + std::to_string(i));
                    auto* otherName = b.CreateInBoundsGEP(
                        otherArray->getType(), otherGlobal,
                        {b.getInt32(0), b.getInt32(0)});
                    literal = b.CreateCall(
                        getValue, {ctx, otherId, otherName});
                }
                llvm::Value* lhs = source.conditionLiteralLeft
                    ? literal : current;
                llvm::Value* rhs = source.conditionLiteralLeft
                    ? current : literal;
                auto* result = b.CreateCall(compare, {lhs, rhs});
                llvm::Value* finishedCondition =
                    b.CreateICmpNE(result, b.getInt32(0));
                if (source.untilConditionNegated)
                    finishedCondition = b.CreateNot(finishedCondition);
                b.CreateCondBr(finishedCondition, body, launch);
                b.SetInsertPoint(launch);
                b.CreateStore(b.getInt32(uint32_t(2*i+1)), pcPtr);
                b.CreateRetVoid();
            } else if (source.kind == mega_yield::Source::Kind::BroadcastRepeat) {
                b.CreateStore(b.getInt64(source.repetitions), remainingPtr);
                if (source.repetitions == 0) {
                    b.CreateBr(body);
                } else {
                    b.CreateStore(b.getInt32(uint32_t(2*i+1)), pcPtr);
                    b.CreateRetVoid();
                }
                b.SetInsertPoint(resume);
                auto* left = b.CreateLoad(i64, remainingPtr, "remaining");
                auto* next = b.CreateSelect(
                    b.CreateICmpEQ(left, b.getInt64(0)), b.getInt64(0),
                    b.CreateSub(left, b.getInt64(1)));
                b.CreateStore(next, remainingPtr);
                b.CreateCondBr(b.CreateICmpEQ(next, b.getInt64(0)),
                               body, done);
            } else {
                b.CreateStore(b.getInt64(source.repetitions), remainingPtr);
                b.CreateBr(resume);
                b.SetInsertPoint(resume);
                auto* left = b.CreateLoad(i64, remainingPtr, "remaining");
                b.CreateCondBr(b.CreateICmpEQ(left, b.getInt64(0)), done, body);
            }
            b.SetInsertPoint(body);
            if (source.hasInnerAfterSets) {
                // The dynamic until condition has just become true.
                // Snapshot time first, then snapshot its live counter.
                // Neither setter executes during joined broadcast waits.
                emitOuterSet(source.innerAfterFirstSet,
                             "_inner_after_first_" + std::to_string(i));
                emitOuterSet(source.innerAfterSecondSet,
                             "_inner_after_second_" + std::to_string(i));
            }
            if (source.hasConditional) {
                // The ordinary one-Thread continuation remains authoritative
                // whenever a Scratch operand is not finite numeric. Reuse
                // the existing Scratch cast::compare helpers (rather than
                // casting a tagged string as an IEEE double).
                auto* conditionId = b.CreateGlobalString(
                    source.conditionVariableId,
                    symbol + "_if_condition_id_" + std::to_string(i));
                std::vector<uint16_t> wide;
                for (char16_t ch : source.conditionVariableName)
                    wide.push_back(uint16_t(ch));
                wide.push_back(0);
                auto* literalArray = llvm::ConstantDataArray::get(
                    *lc, llvm::ArrayRef<uint16_t>(wide));
                auto* literalGlobal = new llvm::GlobalVariable(
                    *module, literalArray->getType(), true,
                    llvm::GlobalValue::PrivateLinkage, literalArray,
                    symbol + "_if_condition_name_" + std::to_string(i));
                auto* conditionName = b.CreateInBoundsGEP(
                    literalArray->getType(), literalGlobal,
                    {b.getInt32(0), b.getInt32(0)});
                auto getValue = module->getOrInsertFunction(
                    "xyo_var_get", llvm::FunctionType::get(
                        i64, {ptrType, ptrType, ptrType}, false));
                auto* currentValue = b.CreateCall(
                    getValue, {ctx, conditionId, conditionName});
                const char* op = nullptr;
                switch (source.comparison) {
                case mega_yield::Source::Comparison::Greater:
                    op = "xyo_gt";
                    break;
                case mega_yield::Source::Comparison::Less:
                    op = "xyo_lt";
                    break;
                case mega_yield::Source::Comparison::Equal:
                    op = "xyo_eq";
                    break;
                }
                auto compare = module->getOrInsertFunction(
                    op, llvm::FunctionType::get(
                        i32, {i64, i64}, false));
                auto* boxedLiteral = b.getInt64(
                    Value::numCanonical(source.conditionLiteral).bits);
                llvm::Value* compareLeft = source.conditionLiteralLeft ?
                    static_cast<llvm::Value*>(boxedLiteral) : currentValue;
                llvm::Value* compareRight = source.conditionLiteralLeft ?
                    static_cast<llvm::Value*>(currentValue) : boxedLiteral;
                auto* comparison = b.CreateCall(
                    compare, {compareLeft, compareRight});
                auto* yes = llvm::BasicBlock::Create(
                    *lc, "script_" + std::to_string(i) + ".then", dispatch);
                auto* no = llvm::BasicBlock::Create(
                    *lc, "script_" + std::to_string(i) + ".else", dispatch);
                auto* merge = llvm::BasicBlock::Create(
                    *lc, "script_" + std::to_string(i) + ".merge", dispatch);
                b.CreateCondBr(
                    b.CreateICmpNE(comparison, b.getInt32(0)), yes, no);
                b.SetInsertPoint(yes);
                emitChange(source.variableId, source.variableName,
                           source.delta,
                           "_if_then_" + std::to_string(i), 0);
                if (source.hasThenSecondChange)
                    emitChange(source.thenSecondVariableId,
                               source.thenSecondVariableName,
                               source.thenSecondDelta,
                               "_if_then_second_" + std::to_string(i),
                               source.hasElseBranch ? 3u : 2u);
                b.CreateBr(merge);
                b.SetInsertPoint(no);
                if (source.hasElseBranch) {
                    emitChange(source.elseVariableId,
                               source.elseVariableName, source.elseDelta,
                               "_if_else_" + std::to_string(i), 1);
                    if (source.hasElseSecondChange)
                        emitChange(source.elseSecondVariableId,
                                   source.elseSecondVariableName,
                                   source.elseSecondDelta,
                                   "_if_else_second_" + std::to_string(i),
                                   3u + uint32_t(source.hasThenSecondChange));
                }
                b.CreateBr(merge);
                b.SetInsertPoint(merge);
            } else if (source.hasBenchRound) {
                emitBenchRound(source.benchRound, i);
            } else {
                emitChange(source.variableId, source.variableName, source.delta,
                           "_body_" + std::to_string(i), 0);
                if (source.hasSecondChange)
                    emitChange(source.secondVariableId,
                               source.secondVariableName, source.secondDelta,
                               "_body_second_" + std::to_string(i), 1);
            }
            if (source.hasOuterUntil) {
                // The inner dynamic until completed exactly once for this
                // outer iteration. Its suffix has already committed.
                // No LLVM stack/SSA state survives the next joined send:
                // Thread::megaRemaining stores only the outer counter.
                auto* left = b.CreateLoad(i64, remainingPtr);
                auto* next = b.CreateSub(left, b.getInt64(1));
                b.CreateStore(next, remainingPtr);
                b.CreateCondBr(
                    b.CreateICmpEQ(next, b.getInt64(0)),
                    outerFinish, outerInit);
                b.SetInsertPoint(outerFinish);
                if (source.hasOuterFinalSet)
                    emitOuterSet(source.outerFinalSet,
                        "_outer_final_set_" + std::to_string(i));
                else
                    emitOuterSuffix(source.outerSuffix,
                        "_outer_final_" + std::to_string(i));
                if (source.hasBenchEnd) {
                    auto emit = module->getOrInsertFunction(
                        "xyo_mega_bench_end",
                        llvm::FunctionType::get(voidType, {ptrType}, false));
                    b.CreateCall(emit, {ctx});
                }
                b.CreateBr(popReturn);
            } else if (source.kind != mega_yield::Source::Kind::Repeat) {
                // Procedure completed. Run every saved caller return PC
                // synchronously; do not introduce a scheduler yield here.
                b.CreateBr(popReturn);
            } else {
                auto* left = b.CreateLoad(i64, remainingPtr);
                b.CreateStore(b.CreateSub(left, b.getInt64(1)), remainingPtr);
                b.CreateStore(b.getInt32(uint32_t(2*i+1)), pcPtr);
                b.CreateRetVoid();
            }

            b.SetInsertPoint(done);
            const bool repeatBroadcast =
                source.kind == mega_yield::Source::Kind::BroadcastRepeat;
            if (repeatBroadcast) {
                b.CreateStore(b.getInt32(uint32_t(2*i+1)), pcPtr);
                b.CreateRetVoid();
            } else {
                b.CreateBr(popReturn);
            }
            for (size_t j = 0; j < returnPcs.size(); ++j) {
                const auto& suffix = source.returnSuffixes[j];
                auto* returned = llvm::BasicBlock::Create(
                    *lc, "script_" + std::to_string(i) + ".return_" +
                         std::to_string(j), dispatch);
                returnSwitch->addCase(b.getInt32(returnPcs[j]), returned);
                b.SetInsertPoint(returned);
                emitChange(suffix.variableId, suffix.variableName,
                           suffix.delta,
                           "_return_" + std::to_string(i) + "_" +
                           std::to_string(j),
                           uint32_t(j + 1 + source.hasSecondChange));
                b.CreateBr(popReturn);
            }
        }
        if (llvm::verifyModule(*module, &llvm::errs())) {
            valid = false;
            return;
        }
        llvm::PassBuilder pb;
        llvm::LoopAnalysisManager lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager cgam;
        llvm::ModuleAnalysisManager mam;
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        auto pipeline = pb.buildPerModuleDefaultPipeline(
            llvm::OptimizationLevel::O1);
        pipeline.run(*module, mam);
        if (llvm::verifyModule(*module, &llvm::errs())) {
            valid = false;
            return;
        }
        if (const char* trace = std::getenv("XYO_JIT_MEGA_BATCH_TRACE");
            trace && trace[0] == '1') {
            size_t definitions = 0;
            for (auto& f : *module) if (!f.isDeclaration()) ++definitions;
            std::fprintf(stderr,
                "[mega-yield] slots=%zu definitions=%zu "
                "states=%zu revision=%llu\n",
                sources.size(), definitions, nextReturnPc,
                static_cast<unsigned long long>(revision));
        }
    });
    if (!valid || blocks->revision() != revision) return {};
    if (auto err = jit->addIRModule(
            llvm::orc::ThreadSafeModule(std::move(module), context))) {
        llvm::consumeError(std::move(err));
        return {};
    }
    auto entry = jit->lookup(symbol);
    if (!entry) {
        llvm::consumeError(entry.takeError());
        return {};
    }
    auto result = std::make_shared<MegaDispatchCode>();
    result->yieldEntry = reinterpret_cast<MegaDispatchCode::YieldEntry>(
        entry->getValue());
    if (scalarBatch) {
        auto bulk = jit->lookup(symbol + "_scalar_batch");
        if (!bulk) {
            llvm::consumeError(bulk.takeError());
        } else {
            result->scalarBatchEntry =
                reinterpret_cast<MegaDispatchCode::ScalarBatchEntry>(
                    bulk->getValue());
        }
    }
    if (scalarBatch && specializeSingle && result->scalarBatchEntry) {
        auto single = jit->lookup(symbol + "_scalar_single");
        if (single)
            result->scalarSingleEntry =
                reinterpret_cast<MegaDispatchCode::ScalarSingleEntry>(
                    single->getValue());
        else
            llvm::consumeError(single.takeError());
    }
    if (multiPcBatch) {
        auto bulk = jit->lookup(symbol + "_multi_pc_batch");
        if (!bulk) {
            llvm::consumeError(bulk.takeError());
        } else {
            result->multiPcBatchEntry =
                reinterpret_cast<MegaDispatchCode::MultiPcBatchEntry>(
                    bulk->getValue());
            for (const auto& source : sources) {
                result->multiPcKinds.push_back(
                    source.kind == mega_yield::Source::Kind::Repeat ? 1 : 0);
                result->multiPcRepetitions.push_back(source.repetitions);
                result->multiPcSecondChanges.push_back(
                    source.hasSecondChange);
                result->multiPcConditionals.push_back(
                    source.hasConditional);
                result->multiPcElseBranches.push_back(
                    source.hasElseBranch);
                result->multiPcThenSecondChanges.push_back(
                    source.hasThenSecondChange);
                result->multiPcElseSecondChanges.push_back(
                    source.hasElseSecondChange);
                const bool oneVariable =
                    !source.hasOuterUntil && !source.hasBenchRound &&
                    !source.hasInnerAfterSets &&
                    !source.hasSecondChange && !source.hasConditional &&
                    !source.hasElseBranch && !source.hasThenSecondChange &&
                    !source.hasElseSecondChange &&
                    source.returnSuffixes.empty();
                result->singleFastSourceKinds.push_back(
                    oneVariable
                        ? (source.kind == mega_yield::Source::Kind::Repeat
                               ? uint8_t(2) : uint8_t(1))
                        : uint8_t(0));
            }
        }
    }
    if (multiPcBatch && specializeSingle && result->multiPcBatchEntry) {
        auto single = jit->lookup(symbol + "_multi_pc_single");
        if (single)
            result->multiPcSingleEntry =
                reinterpret_cast<MegaDispatchCode::MultiPcSingleEntry>(
                    single->getValue());
        else
            llvm::consumeError(single.takeError());
    }
    if (compileMultiRepeatBurst && result->multiPcBatchEntry) {
        auto multi = jit->lookup(symbol + "_multi_repeat_burst");
        if (multi) {
            result->multiRepeatBurstEntry =
                reinterpret_cast<MegaDispatchCode::MultiRepeatBurstEntry>(
                    multi->getValue());
            result->multiRepeatSourceArity.reserve(sources.size());
            for (const auto& source : sources)
                result->multiRepeatSourceArity.push_back(
                    source.hasSecondChange ? 2u : 1u);
        } else {
            llvm::consumeError(multi.takeError());
        }
    }
    if (repeatBurst && result->multiPcSingleEntry) {
        result->repeatBurstEntries.resize(sources.size(), nullptr);
        for (size_t slot = 0; slot < sources.size(); ++slot) {
            const auto& source = sources[slot];
            if (source.kind != mega_yield::Source::Kind::Repeat ||
                source.hasSecondChange || source.hasConditional ||
                source.hasElseBranch || source.hasThenSecondChange ||
                source.hasElseSecondChange ||
                !source.returnSuffixes.empty())
                continue;
            auto address = jit->lookup(
                symbol + "_repeat_burst_" + std::to_string(slot));
            if (address)
                result->repeatBurstEntries[slot] =
                    reinterpret_cast<MegaDispatchCode::RepeatBurstEntry>(
                        address->getValue());
            else
                llvm::consumeError(address.takeError());
        }
    }
    result->sourceBlocks = blocks;
    result->sourceRevision = revision;
    result->entries = uint32_t(threads.size());
    result->directScalar = directScalar;
    if (directScalar) {
        result->scalarVariableIds.reserve(sources.size());
        for (const auto& source : sources) {
            std::vector<std::string> ids{source.variableId};
            if (source.hasSecondChange)
                ids.push_back(source.secondVariableId);
            if (source.hasConditional) {
                if (source.hasElseBranch)
                    ids.push_back(source.elseVariableId);
                ids.push_back(source.conditionVariableId);
                if (source.hasThenSecondChange)
                    ids.push_back(source.thenSecondVariableId);
                if (source.hasElseSecondChange)
                    ids.push_back(source.elseSecondVariableId);
            }
            for (const auto& suffix : source.returnSuffixes)
                ids.push_back(suffix.variableId);
            result->scalarVariableIds.push_back(std::move(ids));
        }
    }
    result->broadcastNames.reserve(sources.size());
    result->broadcastRepeatFastCounts.reserve(sources.size());
    result->fixedJoinPairs.reserve(sources.size());
    for (const auto& source : sources) {
        if (source.kind == mega_yield::Source::Kind::BroadcastWait ||
            source.kind == mega_yield::Source::Kind::BroadcastRepeat ||
            source.kind == mega_yield::Source::Kind::BroadcastUntil)
            result->broadcastNames.emplace_back(source.broadcastName);
        else
            result->broadcastNames.emplace_back(std::nullopt);
        result->broadcastRepeatFastCounts.push_back(
            source.kind == mega_yield::Source::Kind::BroadcastRepeat &&
            source.returnSuffixes.empty() ? source.repetitions : 0);
        if (source.kind == mega_yield::Source::Kind::BroadcastUntil &&
            source.hasOuterUntil && source.untilVariablePair &&
            source.untilConditionNegated &&
            source.comparison == mega_yield::Source::Comparison::Less &&
            source.conditionVariableId ==
                source.outerFirstSet.variableId)
            result->fixedJoinPairs.push_back(
                MegaDispatchCode::FixedJoinPair{
                    source.conditionVariableId,
                    source.untilOtherVariableId});
        else
            result->fixedJoinPairs.emplace_back(std::nullopt);
    }
    return result;
}

std::shared_ptr<NumericJitCode> LLVMBackend::compileNumeric(
        std::shared_ptr<const ir::ScratchNumericKernel> kernel,
        uint64_t serial) {
    if (!jit || !kernel || !kernel->executable.resumable ||
        kernel->executable.yieldBoundaries.empty() ||
        kernel->executable.yieldBoundaries.size() > 16 ||
        !ir::verifyExecutable(kernel->executable))
        return {};
    auto context = std::make_unique<llvm::LLVMContext>();
    const std::string symbol = "xyo_common_numeric_" +
        std::to_string(serial);
    auto module = std::make_unique<llvm::Module>(symbol, *context);
    module->setDataLayout(jit->getDataLayout());
    ir::LlvmCfgBackend compiler(*context, *module, symbol);
    if (!ir::lowerExecutable(kernel->executable, compiler) ||
        llvm::verifyModule(*module))
        return {};

    // Reuse the normal JIT's optimization tier. There are no GC pointers,
    // host calls or Scratch side effects in the generated numeric module.
    llvm::PassBuilder pb;
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    pb.buildPerModuleDefaultPipeline(jitIrOptimizationLevel())
        .run(*module, mam);
    if (llvm::verifyModule(*module)) return {};
    if (auto error = jit->addIRModule(
            llvm::orc::ThreadSafeModule(
                std::move(module), std::move(context)))) {
        llvm::consumeError(std::move(error));
        return {};
    }
    auto address = jit->lookup(symbol);
    if (!address) {
        llvm::consumeError(address.takeError());
        return {};
    }
    using Fn = uint64_t (*)(uint64_t*, uint64_t*);
    auto* fn = reinterpret_cast<Fn>(address->getValue());
    auto result = std::make_shared<NumericJitCode>();
    result->kernel = std::move(kernel);
    result->run = [fn](uint64_t* slots, uint64_t* frame) {
        return fn(slots, frame);
    };
    return result;
}

bool LLVMBackend::addObjectFile(const void* bytes, size_t len) {
    if (!jit || !bytes || !len) return false;
    auto mb = llvm::MemoryBuffer::getMemBufferCopy(
        llvm::StringRef(static_cast<const char*>(bytes), len));
    if (auto err = jit->addObjectFile(std::move(mb))) {
        llvm::consumeError(std::move(err));
        return false;
    }
    return true;
}

void* LLVMBackend::lookupSymbol(const std::string& symbol) {
    if (!jit) return nullptr;
    auto found = jit->lookup(symbol);
    if (!found) {
        llvm::consumeError(found.takeError());
        return nullptr;
    }
    return reinterpret_cast<void*>(found->getValue());
}

// An OSR entry can resume at the start of a top-level counted repeat,
// where the interpreter has an exact remaining-counter snapshot. Only
// constant literal TIMES are permitted: interpreter evaluation of dynamic
// reporters might have observable side effects on each iteration.
static const Block* findOsrLoop(Blocks* blocks, const Block* top) {
    if (!blocks || !top) return nullptr;
    std::unordered_set<const Block*> seen;
    const Block* b = blocks->getBlock(top->next);
    while (b && seen.insert(b).second && seen.size() < 4096) {
        // While/until reevaluate their condition at the next loop head.
        // Their interpreter state contains no private induction counter.
        if (b->opcode == "control_repeat_until" ||
            b->opcode == "control_while") {
            const char* conditional =
                std::getenv("XYO_JIT_OSR_CONDITIONAL");
            if (conditional && conditional[0] == '1')
                return b;
        }
        if (b->opcode == "control_repeat") {
            const BlockInput* times = b->inputs.find("TIMES");
            if (times && !times->block.empty()) {
                const Block* constant = blocks->getBlock(times->block);
                if (constant && constant->shadow &&
                    (constant->opcode == "math_number" ||
                     constant->opcode == "math_integer" ||
                     constant->opcode == "math_whole_number" ||
                     constant->opcode == "math_positive_number"))
                    return b;
            }
        }
        b = b->next.empty() ? nullptr : blocks->getBlock(b->next);
    }
    return nullptr;
}

std::unique_ptr<PendingJitCompilation> LLVMBackend::prepareAsync(
    Thread& thread, Runtime& runtime, const Block* top, uint64_t serial) {
    // The caller owns Runtime and all mutable Scratch data. The compilation
    // worker must never observe Thread/Target/Blocks after this function.
    if (!ok() || !thread.target || !thread.blockContainer || !top) return {};
    auto code = std::make_unique<CompiledScript>();
    std::vector<std::string> fnNames;
    std::map<std::string, std::string> procNames;
    const std::string entryName =
        "xyo_script_" + std::to_string(serial);
    auto context = llvm::orc::ThreadSafeContext(
        std::make_unique<llvm::LLVMContext>());
    std::unique_ptr<llvm::Module> mod;
    bool emitted = false;
    std::string osrName;
    std::string osrLoopId;
    std::string nestedName;
    const Block* osrLoop = std::getenv("XYO_JIT_OSR") &&
        std::getenv("XYO_JIT_OSR")[0] == '1'
            ? findOsrLoop(thread.blockContainer, top) : nullptr;
    const char* nestedFlag = std::getenv("XYO_JIT_OSR_NESTED");
    auto nested = nestedFlag && nestedFlag[0] == '1'
        ? jit::selectNestedWarpLoop(*thread.blockContainer, top)
        : jit::NestedWarpLoop{};
    // An in-flight discard is executable ONLY when every scheduler
    // suspension is at the selected *outermost native loop's* completed
    // body boundary. A nested counted loop may suspend inside the body,
    // where there is no single committed parent-loop counter to restore.
    // Do not compile a continuation for that shape when this opt-in is
    // requested. Ordinary OSR without in-flight deopt is unchanged.
    if (const char* guard =
            std::getenv("XYO_JIT_OSR_NESTED_INFLIGHT_DEOPT");
        guard && guard[0] == '1' && nested.loop) {
        const auto* input = nested.loop->inputs.find("SUBSTACK");
        bool flat = input && !input->block.empty();
        std::unordered_set<std::string> visited;
        std::string id = flat ? input->block : std::string{};
        for (size_t budget = 256; flat && !id.empty() && budget--; ) {
            if (!visited.insert(id).second) { flat = false; break; }
            const Block* statement = thread.blockContainer->getBlock(id);
            if (!statement || statement->opcode == "control_repeat") {
                flat = false;
                break;
            }
            id = statement->next;
        }
        if (flat && !id.empty()) flat = false;
        // Strictly opt-in one inner repeat, fixed trip count 1..8 and
        // at most four primitive change-variable statements per body.
        // The corresponding LLVM path suppresses ONLY the nested
        // repeat's yield. The enclosing selected repeat still yields
        // at a fully committed source-body boundary, as in OSR-008.
        const char* bounded =
            std::getenv("XYO_JIT_OSR_NESTED_INNER_BOUNDED");
        const char* sourceProof =
            std::getenv("XYO_JIT_OSR_NESTED_SOURCE_PROOF");
        const char* realInnerYield =
            std::getenv("XYO_JIT_OSR_NESTED_INNER_YIELD_DEOPT");
        const bool innerYieldRequested = realInnerYield &&
            realInnerYield[0] == '1';
        const char* dynamicFlag =
            std::getenv("XYO_JIT_OSR_NESTED_INNER_DYNAMIC");
        const bool allowDynamic = innerYieldRequested && dynamicFlag &&
            dynamicFlag[0] == '1';
        const char* tripleFlag =
            std::getenv("XYO_JIT_OSR_NESTED_TRIPLE_YIELD");
        const bool allowTriple = allowDynamic && tripleFlag &&
            tripleFlag[0] == '1';
        const char* quadFlag =
            std::getenv("XYO_JIT_OSR_NESTED_QUAD_YIELD");
        const bool allowQuad = allowTriple && quadFlag &&
            quadFlag[0] == '1';
        const uint32_t maxNested = allowQuad ? 3U :
                                   allowTriple ? 2U : 1U;
        std::string innerLoopId;
        std::string deepestLoopId;
        std::string fourthLoopId;
        const bool safeNested = !flat && sourceProof &&
            sourceProof[0] == '1' &&
            ((bounded && bounded[0] == '1') || innerYieldRequested) &&
            safeBoundedNativeInnerRepeat(
                *thread.blockContainer, *nested.loop,
                innerYieldRequested, allowDynamic, maxNested,
                &innerLoopId, &deepestLoopId, &fourthLoopId);
        // If both experimental flags are set, inner yields take priority:
        // they must NEVER accidentally use the atomic bounded lowering.
        if (safeNested && innerYieldRequested) {
            code->nestedOsrInnerYieldGuard = true;
            code->nestedOsrInnerLoopId = std::move(innerLoopId);
            code->nestedOsrDeepInnerLoopId = std::move(deepestLoopId);
            code->nestedOsrFourthLoopId = std::move(fourthLoopId);
        }
        if (safeNested &&
            std::getenv("XYO_JIT_OSR_NESTED_TRACE"))
            std::fprintf(stderr,
                "[jit-osr-inflight] %s inner repeat accepted\\n",
                innerYieldRequested ? "yielding" : "bounded");
        if (!flat && !safeNested) nested = {};
    }
    const uint64_t nestedSourceRevision = nested.loop
        ? thread.blockContainer->revision() : 0;
    const char* proofFlag =
        std::getenv("XYO_JIT_OSR_NESTED_SOURCE_PROOF");
    const bool proofRequired =
        nested.loop && proofFlag && proofFlag[0] == '1';
    code->nestedOsrSourceProofRequired = proofRequired;
    const char* inFlightFlag =
        std::getenv("XYO_JIT_OSR_NESTED_INFLIGHT_DEOPT");
    code->nestedOsrInFlightGuard =
        proofRequired && inFlightFlag && inFlightFlag[0] == '1';
    if (proofRequired) {
        // Build exactly once, on the runtime/compiler thread, from the
        // same versioned Scratch source used by LLVM. The async worker
        // only transports immutable source data, never live Blocks*.
        const auto captured =
            ir::ScratchCompilationUnit::capture(*thread.blockContainer, top);
        if (captured.valid() &&
            captured.sourceRevision == nestedSourceRevision) {
            const auto digest = ir::scratchSourceDigest(captured);
            ir::NativeProcedureFrameSession session(
                captured, thread.blockContainer,
                nestedSourceRevision, digest);
            if (session.enabled()) {
                auto image = session.exportSelfContained();
                if (image && image->valid() && image->symbols &&
                    image->symbols->sourceRevision ==
                        nestedSourceRevision &&
                    image->symbols->sourceDigest == digest)
                    code->nestedOsrSourceSymbols = image->symbols;
            }
        }
    }
    if (std::getenv("XYO_JIT_OSR_NESTED_TRACE"))
        std::fprintf(stderr,
            "[jit-nested-selector] script=%s loop=%s call=%s\n",
            thread.topBlock.c_str(),
            nested.loop ? nested.loop->id.c_str() : "(none)",
            nested.callId.c_str());
    llvmc::withContext(context, [&](llvm::LLVMContext* lc) {
        if (!JitGuardSlowStats::enabled() &&
            !std::getenv("XYO_JIT_KEEP_VALUE_NAMES") &&
            !std::getenv("XYO_JIT_DUMP") &&
            !std::getenv("XYO_JIT_DUMP2"))
            lc->setDiscardValueNames(true);
        mod = std::make_unique<llvm::Module>("xyo_async_script", *lc);
        mod->setDataLayout(jit->getDataLayout());
        mod->setTargetTriple(
            llvmc::tripleArg(jit->getTargetTriple()));
        emitted = jitEmitScript(
            *mod, *lc, runtime, thread.target, thread.blockContainer,
            thread.topBlock, entryName, *code, pinned, fnNames,
            procNames, nullptr);
        if (emitted && osrLoop) {
            osrName = entryName + "_osr";
            osrLoopId = osrLoop->id;
            CompiledScript continuation;
            std::vector<std::string> osrFns;
            std::map<std::string, std::string> osrProcNames;
            emitted = jitEmitScript(
                *mod, *lc, runtime, thread.target, thread.blockContainer,
                thread.topBlock, osrName, continuation, pinned, osrFns,
                osrProcNames, nullptr, osrLoop);
            // Keep GPU programs referenced by the continuation alive.
            for (auto& gp : continuation.gpuProgs)
                code->gpuProgs.push_back(std::move(gp));
            code->mayYield = code->mayYield || continuation.mayYield;
            code->mainThread = code->mainThread || continuation.mainThread;
        }
        if (emitted && nested.loop) {
            nestedName = entryName + "_nested_osr";
            CompiledScript continuation;
            std::vector<std::string> nestedFns;
            std::map<std::string, std::string> nestedProcNames;
            emitted = jitEmitScript(
                *mod, *lc, runtime, thread.target, thread.blockContainer,
                thread.topBlock, nestedName, continuation, pinned, nestedFns,
                nestedProcNames, nullptr, nested.loop, nullptr, true);
            for (auto& gp : continuation.gpuProgs)
                code->gpuProgs.push_back(std::move(gp));
            code->mayYield = true; // region may suspend across warp ticks
            code->mainThread = code->mainThread || continuation.mainThread;
        }
        if (emitted && JitGuardSlowStats::enabled())
            instrumentJitGuardSlowPaths(*mod);
    });
    if (!emitted) return {};
    return std::make_unique<LLVMPreparedCompilation>(
        this, llvm::orc::ThreadSafeModule(std::move(mod), context),
        std::move(code), entryName, std::move(procNames), thread.topBlock,
        std::move(osrName), std::move(osrLoopId),
        std::move(nestedName),
        nested.loop ? nested.loop->id : std::string{},
        nested.callId, nested.parentCallId, nested.callPath,
        nestedSourceRevision, nested.framePath, nested.branchArms,
        nested.argFrameIndex);
}

std::unique_ptr<CompiledScript> LLVMBackend::compile(
    Thread& thread, Runtime& runtime, const Block* top, uint64_t serial,
    const ir::ScratchCompilationUnit& source) {
    static const bool verbose = std::getenv("XYO_JIT_VERBOSE") != nullptr;
    if (!ok() || !thread.target) return {};

    Blocks* blocks = thread.blockContainer;
    if (!blocks || !top) return {};
    const std::string& topId = thread.topBlock;

    using Clock = std::chrono::steady_clock;
    const auto tStart = Clock::now();
    const bool timing = std::getenv("XYO_JIT_TIMING") != nullptr;
    double emitMs = 0, optMs = 0, submitMs = 0;
    size_t irFunctions = 0, irInstructions = 0;
    bool lazyThisScript = false;
    bool megaFused = false;
    auto msBetween = [](Clock::time_point begin, Clock::time_point end) {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    };
    auto cs = std::make_unique<CompiledScript>();
    std::vector<std::string> fnNames;
    std::map<std::string, std::string> procNameMap;
    const std::string entryName =
        "xyo_script_" + std::to_string(serial);
    bool emitOk = false;

    // Each script owns its IR context when requested. A global context can
    // retain uniqued types and metadata long after individual JIT modules
    // have been materialized. In the isolated mode, ORC can release the
    // context together with the script's IR.
    const char* contextSetting = std::getenv("XYO_JIT_ISOLATED_CONTEXT");
    const bool isolatedContext = contextSetting
        ? contextSetting[0] != '0'
        : lazyProcedures;
    llvm::orc::ThreadSafeContext scriptContext =
        isolatedContext
            ? llvm::orc::ThreadSafeContext(
                  std::make_unique<llvm::LLVMContext>())
            : tsc;
    llvmc::withContext(scriptContext, [&](llvm::LLVMContext* lc) {
        // Names for temporary SSA values are never used by native code.
        // LLVM otherwise allocates and uniques a string for each one.
        // Preserve them explicitly for IR diagnostics and inspection.
        if (!JitGuardSlowStats::enabled() &&
            !std::getenv("XYO_JIT_KEEP_VALUE_NAMES") &&
            !std::getenv("XYO_JIT_DUMP") &&
            !std::getenv("XYO_JIT_DUMP2"))
            lc->setDiscardValueNames(true);
        auto mod = std::make_unique<llvm::Module>("xyo_script", *lc);
        mod->setDataLayout(jit->getDataLayout());
        mod->setTargetTriple(
            llvmc::tripleArg(jit->getTargetTriple()));
        const auto tEmit = Clock::now();
        emitOk = jitEmitScript(*mod, *lc, runtime, thread.target, blocks,
                               topId, entryName, *cs, pinned, fnNames,
                               procNameMap, nullptr, nullptr, &source);
        // Beta mega-goto: the reachable acyclic Scratch-procedure graph
        // becomes direct LLVM CFG branches inside ONE JIT entry function.
        // Script-to-script scheduling and coroutine resume remain separate.
        // Abort before any transformation if an oversized or recursive
        // call graph cannot be represented as a bounded single function.
        if (emitOk && !cs->mlirFrontend && !runtime.blockCountEnabled &&
            !std::getenv("XYO_JIT_CALLFRAME") &&
            !std::getenv("XYO_PROF") && mega_goto::enabled()) {
            const auto mega = mega_goto::fuse(
                *mod, entryName, procNameMap);
            megaFused = mega.fused;
            if (const char* trace = std::getenv("XYO_JIT_MEGA_TRACE");
                trace && trace[0] == '1')
                std::fprintf(stderr,
                    "[mega-goto] script=%s fused=%d calls=%zu "
                    "estimated_ir=%zu remaining=%zu reason=%s\n",
                    topId.c_str(), int(mega.fused), mega.procedureCalls,
                    mega.estimatedInstructions, mega.remainingCalls,
                    mega.reason);
            if (mega.reason == std::string_view("invalid-llvm"))
                emitOk = false;
        }
        // Size-bounded partial fusion is deliberately separate from the
        // all-or-nothing dispatcher. The IR remains callable through
        // original procedure symbols when only a subset is inlined.
        if (emitOk && !cs->mlirFrontend && !megaFused &&
            !runtime.blockCountEnabled &&
            !std::getenv("XYO_JIT_CALLFRAME") &&
            !std::getenv("XYO_PROF") && mega_partial::enabled()) {
            const auto partial = mega_partial::fuse(*mod, procNameMap);
            cs->megaPartialInlined =
                static_cast<uint32_t>(partial.inlined);
            if (const char* trace = std::getenv("XYO_JIT_MEGA_TRACE");
                trace && trace[0] == '1')
                std::fprintf(stderr,
                    "[mega-partial] script=%s selected=%zu inlined=%zu "
                    "rejected_cycle=%zu rejected_size=%zu valid=%d\n",
                    topId.c_str(), partial.selected, partial.inlined,
                    partial.rejectedCycles, partial.rejectedSize,
                    int(partial.valid));
            if (!partial.valid) emitOk = false;
        }
        // Only a source-authenticated, simple direct-yield script can use
        // the first stackless LLVM experiment. Never transform arbitrary
        // callbacks, procedures, waits, reporters, OSR or AOT code.
        const bool narrowCoro = stackless::eligible(*blocks, *top);
        const bool generalCoro = !narrowCoro &&
            stackless::generalCandidate(*blocks, *top);
        const bool candidate =
            emitOk && !cs->mlirFrontend && !runtime.blockCountEnabled &&
            !std::getenv("XYO_JIT_CALLFRAME") &&
            !std::getenv("XYO_PROF") &&
            (narrowCoro || generalCoro);
        bool lowered = false;
        if (candidate) {
            // Fold the reachable non-warp procedure graph into the entry so
            // the coroutine frame owns all inlined call state.
            bool ready = true;
            if (generalCoro && !procNameMap.empty()) {
                const size_t procCount = procNameMap.size();
                const auto fused =
                    mega_goto::fuse(*mod, entryName, procNameMap, true,
                                      /*budgetOverride=*/8192);
                if (const char* trace = std::getenv("XYO_JIT_CORO_TRACE");
                    trace && trace[0] == '1')
                    std::fprintf(stderr,
                        "[coro-proc] script=%s procs=%zu fused=%d reason=%s\n",
                        topId.c_str(), procCount, int(fused.fused),
                        fused.reason);
                // Codegen's small-procedure inliner may already have
                // folded every callsite into the entry. That is exactly
                // the state the coroutine lowering needs.
                ready = fused.fused ||
                    std::string_view(fused.reason) ==
                        "no-reachable-procedure-calls";
                if (ready && !fused.fused) procNameMap.clear();
                if (!ready && stackless::recursionEnabled()) {
                    // Recursive procedure graph: lower each procedure as a
                    // coroutine and propagate suspension at every call site.
                    std::vector<std::string> procSymbols;
                    procSymbols.reserve(procNameMap.size());
                    for (const auto& [key, symbol] : procNameMap) {
                        (void)key;
                        procSymbols.push_back(symbol);
                    }
                    lowered = stackless::lowerRecursive(
                        *mod, entryName, procSymbols);
                    if (const char* trace =
                            std::getenv("XYO_JIT_CORO_TRACE");
                        trace && trace[0] == '1')
                        std::fprintf(stderr,
                            "[coro-rec] script=%s procs=%zu lowered=%d\n",
                            topId.c_str(), procSymbols.size(),
                            int(lowered));
                    if (lowered) ready = true;
                    if (const char* dump = std::getenv("XYO_JIT_CORO_DUMP");
                        dump && dump[0] == '1')
                        mod->print(llvm::errs(), nullptr);
                }
            }
            if (ready && !lowered) lowered = stackless::lower(*mod, entryName);
        }
        if (lowered) {
            cs->coroSourceBlocks = blocks;
            cs->coroSourceRevision = blocks->revision();
            cs->mayYield = true;
            // General coroutines carry their own heap frame and borrow no
            // renderer state; the ordinary access-set analysis decides
            // worker safety. The narrow experiment stays serial.
            cs->mainThread = narrowCoro;
        }
        if (const char* trace = std::getenv("XYO_JIT_CORO_TRACE");
            trace && trace[0] == '1' &&
            (stackless::enabled() || stackless::generalEnabled()))
            std::fprintf(stderr,
                "[stackless-coro] script=%s candidate=%d lowered=%d\\n",
                topId.c_str(), int(candidate), int(lowered));
        if (timing) emitMs = msBetween(tEmit, Clock::now());
        if (emitOk) {
            // PROF-009: map generated ORC procedure symbols to source
            // proccodes only when the opt-in lazy profiler is active.
            if (const char* detail = std::getenv("XYO_JIT_LAZY_DETAIL");
                detail && detail[0] == '1' && detail[1] == '\0') {
                for (const auto& [key, symbol] : procNameMap) {
                    std::string safeKey = key;
                    for (char& c : safeKey)
                        if (c == '\n' || c == '\r') c = ' ';
                    std::fprintf(stderr,
                        "[jit-proc-map] function=%s key=%s\n",
                        symbol.c_str(), safeKey.c_str());
                }
            }
            if (JitGuardSlowStats::enabled())
                instrumentJitGuardSlowPaths(*mod);
            if (timing || lazyProcedures) {
                for (const llvm::Function& generated : *mod) {
                    if (generated.isDeclaration()) continue;
                    ++irFunctions;
                    for (const llvm::BasicBlock& bb : generated)
                        irInstructions += bb.size();
                }
            }
            // O3 by default (XYO_JIT_O2=1 to revert) — varSlotData/alloca
            // タグチェック除去・式共有を行わないと生成コードが数倍遅い
            struct PassFrame { std::string name; Clock::time_point started; double childMs = 0; };
            std::vector<PassFrame> passFrames;
            std::map<std::string, std::pair<double, uint64_t>> passTimes;
            const bool profilePasses = std::getenv("XYO_JIT_PASS_TIMING") != nullptr;
            llvm::PassInstrumentationCallbacks pic;
            if (profilePasses) {
                pic.registerBeforeNonSkippedPassCallback(
                    [&](llvm::StringRef name, llvm::Any) {
                        passFrames.push_back({name.str(), Clock::now(), 0});
                    });
                auto finish = [&](llvm::StringRef) {
                    if (passFrames.empty()) return;
                    PassFrame frame = std::move(passFrames.back());
                    passFrames.pop_back();
                    double ms = msBetween(frame.started, Clock::now());
                    if (!passFrames.empty()) passFrames.back().childMs += ms;
                    auto& entry = passTimes[frame.name];
                    entry.first += std::max(0.0, ms - frame.childMs);
                    ++entry.second;
                };
                pic.registerAfterPassCallback(
                    [finish](llvm::StringRef name, llvm::Any,
                             const llvm::PreservedAnalyses&) { finish(name); });
                pic.registerAfterPassInvalidatedCallback(
                    [finish](llvm::StringRef name,
                             const llvm::PreservedAnalyses&) { finish(name); });
            }
            llvm::PassBuilder pb(nullptr, llvm::PipelineTuningOptions(), std::nullopt, profilePasses ? &pic : nullptr);
            llvm::LoopAnalysisManager lam;
            llvm::FunctionAnalysisManager fam;
            llvm::CGSCCAnalysisManager cgam;
            llvm::ModuleAnalysisManager mam;
            pb.registerModuleAnalyses(mam);
            pb.registerCGSCCAnalyses(cgam);
            pb.registerFunctionAnalyses(fam);
            pb.registerLoopAnalyses(lam);
            pb.crossRegisterProxies(lam, fam, cgam, mam);
            // Optional IR optimization tier for compile-time experiments.
            // Preserve the original O3 default and legacy O2 override.
            llvm::ModulePassManager mpm =
                pb.buildPerModuleDefaultPipeline(jitIrOptimizationLevel());
            const auto tOptimize = Clock::now();
            // JIT modes can be changed by API clients after construction.
            // The requested tier still needs a matching ORC engine.
            // A par-for loop passes the address of an internal LLVM function
            // to the C++ xyo_par_for host callback. LLLazyJIT may split the
            // module into independently materialized functions, leaving this
            // address-taken body pointing at unmaterialized executable memory.
            // This can SIGSEGV on the *first* iteration, including the
            // sequential (trip < worker threshold) path. Materialize the
            // entire containing script through ordinary LLJIT instead.
            //
            // Keep the normal on-demand tier for scripts without callbacks;
            // an explicit --jit-lazy on must not override this safety guard.
            const llvm::Function* parForDispatch =
                mod->getFunction("xyo_par_for");
            const bool hasParForCallback =
                parForDispatch && !parForDispatch->use_empty();
            if (hasParForCallback &&
                std::getenv("XYO_JIT_LAZY_CALLBACK_TRACE"))
                std::fprintf(stderr,
                    "[jit-lazy-guard] script=%s reason=parfor-callback\n",
                    topId.c_str());
            const bool deferredCandidate = lazyProcedures &&
                !hasParForCallback &&
                chooseLazyCompile(
                    jitLazyMode(), jitMode(), irFunctions, irInstructions,
                    countDirectProcedureCalls(blocks, top));
            // Failure to initialize the secondary ORC engine must not
            // prevent Scratch from running: fall back to normal O3.
            // Eager's large-module startup tier is for interactive GUI
            // responsiveness. A headless batch run normally cares about
            // time-to-completion: deferred per-procedure materialization can
            // increase that total when most procedures are eventually called.
            const bool deferIR =
                !megaFused && !cs->coroSourceRevision && deferredCandidate &&
                (jitMode() != JitMode::Eager || runtime.renderer != nullptr ||
                 jitLazyMode() == JitLazyMode::On) &&
                ensureLazyJit();
            if (deferIR) {
                mod->addModuleFlag(llvm::Module::Warning,
                                   "xyo.lazy.optimize", 1);
            } else {
                mpm.run(*mod, mam);
                auditIr001OptimizedWrites(*mod, "eager");
                auditRt001PostoptSsa(*mod, "eager");
                auditRt001PostoptAsyncRoots(*mod, "eager");
                emitRt001MachineStackmapProbe(*mod, "eager");
                emitRt001AsyncMachineStackmapProbe(*mod, "eager");
                emitRt001YieldEdgeStackmapProbe(*mod, "eager");
            }
            if (timing) optMs = msBetween(tOptimize, Clock::now());
            if (profilePasses) {
                for (const auto& [name, data] : passTimes) {
                    if (data.first > 5.0)
                        std::fprintf(stderr, "[jit-pass] %s %.3fms %llu %s\n", topId.c_str(), data.first, (unsigned long long)data.second, name.c_str());
                }
            }
            // The deferred module has not passed through O3 yet. Avoid
            // labeling that IR as optimized: the lazy transform prints it
            // after O3 instead. Keep a separate opt-in raw IR diagnostic.
            if (const char* d = std::getenv(
                    deferIR ? "XYO_JIT_DUMP_PREOPT" : "XYO_JIT_DUMP2"))
                if (d[0] != '\0') mod->print(llvm::errs(), nullptr);
            const auto tSubmit = Clock::now();
            // Lazy compilation is worthwhile only when a top-level script
            // has substantial distinct procedure bodies. Small scripts use
            // the normal ORC layer and avoid call-through trampoline cost.
            lazyThisScript = deferIR;
            llvm::orc::ThreadSafeModule tsm(
                std::move(mod), scriptContext);
            llvm::Error err = lazyThisScript
                ? lazyJit->addLazyIRModule(std::move(tsm))
                : jit->addIRModule(std::move(tsm));
            if (err) {
                llvm::consumeError(std::move(err));
                emitOk = false;
            }
            if (timing) submitMs = msBetween(tSubmit, Clock::now());
        }
    });
    if (!emitOk) {
        if (verbose) std::fprintf(stderr,
            "xyo-jit: emit failed for %s (%s)\n",
            topId.c_str(), top->opcode.c_str());
        return {};
    }

    const auto tLookup = Clock::now();
    auto& owningJit = lazyThisScript
        ? static_cast<llvm::orc::LLJIT&>(*lazyJit)
        : *jit;
    auto addrOr = owningJit.lookup(entryName);
    if (!addrOr) {
        llvm::consumeError(addrOr.takeError());
        return {};
    }
    cs->entry = reinterpret_cast<ScriptFn>(addrOr->getValue());
    if (cs->coroSourceRevision) {
        const auto lookupCoro = [&](const char* suffix) -> uintptr_t {
            auto address = jit->lookup(entryName + suffix);
            if (!address) {
                llvm::consumeError(address.takeError());
                return 0;
            }
            return static_cast<uintptr_t>(address->getValue());
        };
        cs->coroResume = reinterpret_cast<CompiledScript::CoroResume>(
            lookupCoro("_resume"));
        cs->coroDone = reinterpret_cast<CompiledScript::CoroDone>(
            lookupCoro("_done"));
        cs->coroDestroy = reinterpret_cast<CompiledScript::CoroResume>(
            lookupCoro("_destroy"));
        if (!cs->coroResume || !cs->coroDone || !cs->coroDestroy)
            return {};
    }
    cs->lazy = lazyThisScript;
    // Looking up every procedure in lazy mode defeats on-demand function
    // materialization. Generated Scratch code calls via ORC trampolines;
    // no runtime consumer reads CompiledScript::procedures in this path.
    if (!lazyThisScript) {
        for (auto& [vkey, fname] : procNameMap) {
            auto a = jit->lookup(fname);
            if (!a) { llvm::consumeError(a.takeError()); continue; }
            cs->procedures[vkey] = reinterpret_cast<ScriptFn>(a->getValue());
        }
    }

    if (timing) {
        const auto tDone = Clock::now();
        std::fprintf(stderr,
            "[jit-timing] %s emit=%.3fms opt=%.3fms submit=%.3fms "
            "lookup=%.3fms total=%.3fms funcs=%zu ir=%zu tier=%s\n",
            topId.c_str(), emitMs, optMs, submitMs,
            msBetween(tLookup, tDone), msBetween(tStart, tDone),
            irFunctions, irInstructions,
            lazyThisScript ? "lazy" : "eager");
    }
    if (verbose) {
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - tStart)
                              .count();
        std::fprintf(stderr, "xyo-jit: compiled %s (%s, %.1fms)\n",
                     topId.c_str(), top->opcode.c_str(), ms);
    }
    return cs;
}

} // namespace xyo

// RT-001 diagnostic lookup, with an always-present fail-closed stub
// in builds where the optional ORC FDE plugin was not compiled.
extern "C" int xyo_rt001_orc_fde_live_pc(
    uintptr_t pc, uintptr_t* start, uintptr_t* end) {
#ifdef XYO_RT001_ORC_FDE_PLUGIN
    return xyo::rt::activeOrcFdeIndex().contains(pc, start, end);
#else
    (void)pc;
    (void)start;
    (void)end;
    return 0;
#endif
}

namespace xyo {
std::unique_ptr<JitBackend> createJitBackend() {
    return std::make_unique<LLVMBackend>();
}
} // namespace xyo
