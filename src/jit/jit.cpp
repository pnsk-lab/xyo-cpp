// xyo-cpp: LLJIT 基盤 — design.md §7.6。
#include "jit.hpp"

#include "llvm_compat.hpp"

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Passes/PassBuilder.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

#include "../blocks/pen_api.hpp"
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
#include "aot_emit.hpp"

namespace xyo {

bool jitEmitScript(llvm::Module&, llvm::LLVMContext&, Runtime&, Target*,
                   Blocks*, const std::string&, const std::string&,
                   CompiledScript&, JitPinned&,
                   std::vector<std::string>&,
                   std::map<std::string, std::string>&, AotEmit* = nullptr);

// 生成コードが参照する extern "C" ヘルパ（rt.cpp 定義）
extern "C" {
void xyo_yield(JitCtx*);
int32_t xyo_is_stuck(JitCtx*);
int32_t xyo_wait_should_yield(JitCtx*);
void xyo_wait_yield(JitCtx*);
void xyo_loop_yield(JitCtx*);
double xyo_to_num(uint64_t);
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
double xyo_round(double);
uint64_t xyo_var_get(JitCtx*, const char*);
void xyo_var_set(JitCtx*, const char*, uint64_t);
void xyo_var_change(JitCtx*, const char*, uint64_t);
uint64_t xyo_arg_get(JitCtx*, uint64_t);
void xyo_rt_wait(JitCtx*, double);
void xyo_stop_all(JitCtx*);
void xyo_stop_others(JitCtx*);
void xyo_stop_script(JitCtx*);
void xyo_broadcast_v(JitCtx*, uint64_t);
void xyo_broadcast_wait_v(JitCtx*, uint64_t);
double xyo_timer(JitCtx*);
uint64_t xyo_rt_compat(JitCtx*, const void*);
uint64_t xyo_rt_reporter(JitCtx*, const char*);
void xyo_proc_push(JitCtx*, const char*, const void*, int32_t);
void xyo_proc_pop(JitCtx*);
void xyo_rt_setarg(const void*, const char*, uint64_t);

// --- rt_blocks.cpp: ブロック直接ヘルパ ---
uint64_t xyo_join(uint64_t, uint64_t);
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
uint64_t xyo_sns_loudness(JitCtx*);
uint64_t xyo_sns_loud(JitCtx*);
uint64_t xyo_sns_answer(JitCtx*);
void xyo_sns_askwait(JitCtx*, uint64_t);
uint64_t xyo_sns_of(JitCtx*, uint64_t, uint64_t);
uint64_t xyo_sns_username(JitCtx*);
uint64_t xyo_sns_online(JitCtx*);
uint64_t xyo_ctl_getcounter(JitCtx*);
void xyo_ctl_incrcounter(JitCtx*);
void xyo_ctl_clearcounter(JitCtx*);
void xyo_ctl_createclone(JitCtx*, uint64_t);
void xyo_ctl_deleteclone(JitCtx*);
void xyo_snd_play(JitCtx*, uint64_t);
void xyo_snd_playwait(JitCtx*, uint64_t);
void xyo_snd_stopall(JitCtx*);
void xyo_snd_seteffect(JitCtx*, uint64_t, uint64_t);
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

namespace {

// 生成コードが直接参照する extern "C" シンボル
const std::pair<const char*, void*> kRtSymbols[] = {
    {"xyo_yield", (void*)&xyo_yield},
    {"xyo_is_stuck", (void*)&xyo_is_stuck},
    {"xyo_wait_should_yield", (void*)&xyo_wait_should_yield},
    {"xyo_wait_yield", (void*)&xyo_wait_yield},
    {"xyo_loop_yield", (void*)&xyo_loop_yield},
    {"xyo_to_num", (void*)&xyo_to_num},
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
    {"xyo_round", (void*)&xyo_round},
    {"xyo_log", (void*)&xyo_log},
        {"xyo_asin", (void*)&xyo_asin},
        {"xyo_acos", (void*)&xyo_acos},
        {"xyo_atan", (void*)&xyo_atan},
        {"xyo_exp", (void*)&xyo_exp},
    {"xyo_var_get", (void*)&xyo_var_get},
    {"xyo_var_set", (void*)&xyo_var_set},
    {"xyo_var_change", (void*)&xyo_var_change},
    {"xyo_jitVar", (void*)&xyo_jitVar},
    {"xyo_varslot_init", (void*)&xyo_varslot_init},
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
    {"xyo_stop_all", (void*)&xyo_stop_all},
    {"xyo_stop_others", (void*)&xyo_stop_others},
    {"xyo_stop_script", (void*)&xyo_stop_script},
    {"xyo_broadcast_v", (void*)&xyo_broadcast_v},
    {"xyo_broadcast_wait_v", (void*)&xyo_broadcast_wait_v},
    {"xyo_timer", (void*)&xyo_timer},
    {"xyo_rt_compat", (void*)&xyo_rt_compat},
    {"xyo_rt_reporter", (void*)&xyo_rt_reporter},
    {"xyo_proc_push", (void*)&xyo_proc_push},
    {"xyo_proc_pop", (void*)&xyo_proc_pop},
    {"xyo_rt_setarg", (void*)&xyo_rt_setarg},
    {"xyo_join", (void*)&xyo_join},
    {"xyo_letterof", (void*)&xyo_letterof},
    {"xyo_strlen", (void*)&xyo_strlen},
    {"xyo_strcontains", (void*)&xyo_strcontains},
    {"xyo_list_add", (void*)&xyo_list_add},
    {"xyo_list_delete", (void*)&xyo_list_delete},
    {"xyo_list_deleteall", (void*)&xyo_list_deleteall},
    {"xyo_list_insert", (void*)&xyo_list_insert},
    {"xyo_list_replace", (void*)&xyo_list_replace},
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
    {"xyo_sns_loudness", (void*)&xyo_sns_loudness},
    {"xyo_sns_loud", (void*)&xyo_sns_loud},
    {"xyo_sns_answer", (void*)&xyo_sns_answer},
    {"xyo_sns_askwait", (void*)&xyo_sns_askwait},
    {"xyo_sns_of", (void*)&xyo_sns_of},
    {"xyo_sns_username", (void*)&xyo_sns_username},
    {"xyo_sns_online", (void*)&xyo_sns_online},
    {"xyo_ctl_getcounter", (void*)&xyo_ctl_getcounter},
    {"xyo_ctl_incrcounter", (void*)&xyo_ctl_incrcounter},
    {"xyo_ctl_clearcounter", (void*)&xyo_ctl_clearcounter},
    {"xyo_ctl_createclone", (void*)&xyo_ctl_createclone},
    {"xyo_ctl_deleteclone", (void*)&xyo_ctl_deleteclone},
    {"xyo_snd_play", (void*)&xyo_snd_play},
    {"xyo_snd_playwait", (void*)&xyo_snd_playwait},
    {"xyo_snd_stopall", (void*)&xyo_snd_stopall},
    {"xyo_snd_seteffect", (void*)&xyo_snd_seteffect},
    {"xyo_snd_changeeffect", (void*)&xyo_snd_changeeffect},
    {"xyo_snd_cleareffects", (void*)&xyo_snd_cleareffects},
    {"xyo_snd_setvolume", (void*)&xyo_snd_setvolume},
    {"xyo_snd_changevolume", (void*)&xyo_snd_changevolume},
    {"xyo_snd_getvolume", (void*)&xyo_snd_getvolume},
};

} // namespace

namespace {
/// AOT メタの hex 文字列（UTF-16 生バイト）をデコードする。
std::string hexDecode(const std::string& h) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(h.size() / 2);
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
        const int a = nib(h[i]);
        const int b = nib(h[i + 1]);
        if (a < 0 || b < 0) return {};
        out.push_back(char((a << 4) | b));
    }
    return out;
}
} // namespace

/// runtime_helpers.cpp の ASCII 1 文字キャッシュ（AOT slot 束縛用）。
extern "C" SStr* const* xyo_ascii_cache();

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

struct JitEngine::Impl {
    llvm::orc::ThreadSafeContext tsc;
    std::unique_ptr<llvm::orc::LLJIT> jit;
    JitPinned pinned;
    /// AOT: 文字列 c_str の安定置き場（deque は push_back で
    /// 既存要素のアドレスを変えない）
    std::deque<std::string> aotStrings;
    /// AOT: slot のアドレス（GC ルート走査対象）
    std::vector<void**> aotSlots;
    /// AOT: ロード時に再コンパイルした GPU 並列ループの Program の
    /// 実体（slot は &program を指す。エンジンと同寿命）。
    std::vector<std::unique_ptr<gpu::ParallelLoop>> aotGpuProgs;
};

JitEngine::JitEngine() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    impl_ = std::make_unique<Impl>();
    impl_->tsc = llvm::orc::ThreadSafeContext(
        std::make_unique<llvm::LLVMContext>());

    auto jitOr = llvm::orc::LLJITBuilder().create();
    if (!jitOr) {
        std::fprintf(stderr, "xyo-jit: LLJIT init failed\n");
        impl_.reset();
        return;
    }
    auto jit = std::move(*jitOr);

    auto& es = jit->getExecutionSession();
    llvm::orc::SymbolMap symbols;
    for (auto& [name, addr] : kRtSymbols) {
        symbols[es.intern(name)] = llvm::orc::ExecutorSymbolDef(
            llvm::orc::ExecutorAddr::fromPtr(addr),
            llvm::JITSymbolFlags::Exported);
    }
    // バックエンドの libcall（fmod/log/bcmp 等）— 静的リンクでも解決できる
    for (auto& [name, addr] : hostLibcallSymbols()) {
        symbols[es.intern(name)] = llvm::orc::ExecutorSymbolDef(
            llvm::orc::ExecutorAddr::fromPtr(addr),
            llvm::JITSymbolFlags::Exported);
    }
    auto& jd = jit->getMainJITDylib();
    if (auto err =
            jd.define(llvm::orc::absoluteSymbols(std::move(symbols)))) {
        std::fprintf(stderr, "xyo-jit: symbol define failed\n");
        llvm::consumeError(std::move(err));
    }
    if (auto gen =
            llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
                jit->getDataLayout().getGlobalPrefix())) {
        jd.addGenerator(std::move(*gen));
    }
    impl_->jit = std::move(jit);

    // pinned 文字列を GC ルート化（JitEngine 単位でプロバイダ登録）
    Gc::get().addRootProvider(this, [this](Gc& g) {
        if (impl_)
            for (SStr* s : impl_->pinned.strings) g.markPtr(s);
    });
}

namespace {

/// HotSpot のしきい値（環境変数で調整可能）。
struct HotParams {
    uint64_t invocations = 3;    // 呼び出し回数で JIT
    uint64_t blocks = 4096;      // 解釈実行したブロック数で JIT
    uint64_t size = 5000;        // 到達可能ブロック数がこれ以上なら初回 JIT
    bool loopCompile = true;     // ループを含むスクリプトは初回 JIT
    bool stats = false;          // 終了時にホットネス表を出力

    static uint64_t envU64(const char* n, uint64_t d) {
        const char* e = std::getenv(n);
        if (!e || !*e) return d;
        return std::strtoull(e, nullptr, 10);
    }
    static bool envOn(const char* n, bool d) {
        const char* e = std::getenv(n);
        if (!e) return d;
        return !(e[0] == '0' || e[0] == 'n' || e[0] == 'N' ||
                 e[0] == 'f' || e[0] == 'F');
    }
    HotParams() {
        invocations = envU64("XYO_HOT_INVOCATIONS", invocations);
        blocks = envU64("XYO_HOT_BLOCKS", blocks);
        size = envU64("XYO_HOT_SIZE", size);
        loopCompile = envOn("XYO_HOT_LOOP", loopCompile);
        stats = envOn("XYO_HOT_STATS", stats);
    }
};
const HotParams& hotParams() { static HotParams p; return p; }

} // namespace

JitEngine::~JitEngine() {
    if (hotParams().stats && !profiles_.empty()) {
        struct Row {
            uint64_t inv, blocks;
            const std::string* id;
            bool jit, aot, loop;
        };
        std::vector<Row> rows;
        rows.reserve(profiles_.size());
        for (auto& [top, p] : profiles_) {
            auto cit = cache_.find(top);
            bool jit = false;
            bool aot = false;
            if (cit != cache_.end()) {
                aot = cit->second.aot;
                jit = cit->second.code != nullptr;
            }
            rows.push_back({p.invocations, p.interpBlocks, &p.id, jit, aot,
                            p.hasLoop == 2});
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return a.inv > b.inv ||
                   (a.inv == b.inv && a.blocks > b.blocks);
        });
        std::fprintf(stderr,
                     "[hot] %-32s %8s %10s %5s %4s %s\n", "script", "inv",
                     "interp", "jit", "aot", "loop");
        for (const Row& r : rows)
            std::fprintf(stderr, "[hot] %-32s %8llu %10llu %5s %4d %d\n",
                         r.id ? r.id->c_str() : "?",
                         (unsigned long long)r.inv,
                         (unsigned long long)r.blocks,
                         r.jit ? "yes" : "-", int(r.aot), int(r.loop));
    }
    Gc::get().removeRootProvider(this);
    if (impl_) Gc::get().removeRootProvider(impl_.get());
    impl_.reset();
}

bool JitEngine::ok() const { return impl_ && impl_->jit; }

uint8_t JitEngine::scanScriptLoop(Blocks& blocks, const Block* top,
                                  uint32_t* nodes) {
    if (nodes) *nodes = 0;
    if (!top) return 1;
    static const std::unordered_set<std::string> kLoops = {
        "control_forever", "control_repeat", "control_repeat_until",
        "control_while", "control_for_each",
    };
    std::unordered_set<const Block*> seen;
    std::vector<const Block*> work;
    auto push = [&](const Block* b) {
        if (b && seen.insert(b).second) work.push_back(b);
    };
    push(top);
    size_t guard = 0;
    while (!work.empty()) {
        // 走査不能な巨大グラフはループ扱い（= 初回 JIT）で安全側に倒す
        if (++guard > 200000) {
            if (nodes) *nodes = 200001;
            return 2;
        }
        const Block* b = work.back();
        work.pop_back();
        if (kLoops.count(b->opcode)) return 2;
        if (!b->next.empty()) push(blocks.getBlock(b->next));
        for (auto& kv : b->inputs.items) {
            const BlockInput& in = kv.second;
            if (!in.block.empty()) push(blocks.getBlock(in.block));
            if (!in.shadow.empty()) push(blocks.getBlock(in.shadow));
        }
        // 手続き呼び出しの本体も到達可能 — ホットループが手続き側に
        // あるスクリプトを取りこぼさない。
        if (b->opcode == "procedures_call" && b->mutation) {
            const std::string& pc = b->mutation->attr("proccode");
            if (!pc.empty()) {
                const std::string def = blocks.getProcedureDefinition(pc);
                if (!def.empty()) push(blocks.getBlock(def));
            }
        }
    }
    if (nodes) *nodes = uint32_t(guard);
    return 1;
}

bool JitEngine::decideCompile(Thread& thread, const Block* top) {
    const JitMode mode = jitMode();
    if (mode == JitMode::Off || !top) return false;

    auto it = cache_.find(top);
    if (it != cache_.end() && it->second.code) return true;  // 既に JIT/AOT
    if (failed_.count(top)) return false;

    ScriptProfile& p = profiles_[top];
    if (p.id.empty()) p.id = thread.topBlock;
    ++p.invocations;

    if (mode == JitMode::Eager) return true;

    if (p.hasLoop == 0 && thread.blockContainer)
        p.hasLoop = scanScriptLoop(*thread.blockContainer, top,
                                   &p.nodeCount);

    // ループを含むスクリプトは単発でも長時間走り得る。OSR を持たない
    // 以上あとから昇格できないため、初回から JIT する
    // （XYO_HOT_LOOP=0 で無効化できる）。巨大な直線スクリプトも
    // 解釈のまま一度だけ走らせると高くつくので同様に初回 JIT。
    const bool loopCompile = hotParams().loopCompile && p.hasLoop == 2;
    const bool large = p.nodeCount >= hotParams().size;
    const bool hot = p.invocations >= hotParams().invocations ||
                     p.interpBlocks >= hotParams().blocks;
    if (!loopCompile && !large && !hot) return false;

    if (std::getenv("XYO_JIT_VERBOSE"))
        std::fprintf(stderr,
                     "xyo-jit: hot %s (%s) inv=%llu blocks=%llu loop=%d\n",
                     thread.topBlock.c_str(), top->opcode.c_str(),
                     (unsigned long long)p.invocations,
                     (unsigned long long)p.interpBlocks,
                     int(p.hasLoop == 2));
    return true;
}

void JitEngine::noteInterpBlocks(const void* top, uint64_t blocks) {
    if (!top || !blocks) return;
    profiles_[top].interpBlocks += blocks;
}

bool JitEngine::scriptStats(const void* top, ScriptStats* out) const {
    if (!top || !out) return false;
    auto pit = profiles_.find(top);
    auto cit = cache_.find(top);
    if (pit == profiles_.end() && cit == cache_.end()) return false;
    out->invocations = pit != profiles_.end() ? pit->second.invocations : 0;
    out->interpBlocks = pit != profiles_.end() ? pit->second.interpBlocks : 0;
    out->hasLoop = pit != profiles_.end() && pit->second.hasLoop == 2;
    if (cit != cache_.end()) {
        out->aot = cit->second.aot;
        out->compiled = cit->second.code != nullptr;
    }
    return true;
}

const CompiledScript* JitEngine::compile(Thread& thread, Runtime& runtime,
                                         Sequencer& sequencer,
                                         const Block* topHint) {
    static const bool verbose = std::getenv("XYO_JIT_VERBOSE") != nullptr;
    if (!ok() || !thread.target) return nullptr;
    Blocks* blocks = thread.blockContainer;
    if (!blocks) return nullptr;
    const std::string& topId = thread.topBlock;

    // キャッシュヒット経路を最短化（broadcast/clone の大量 spawn 対策）
    const Block* top = topHint ? topHint : blocks->getBlock(topId);
    if (!top) {
        if (verbose) std::fprintf(stderr,
            "xyo-jit: no top block %s\n", topId.c_str());
        return nullptr;
    }
    // edge-activated hat はコンパイルしない（ハット評価は interpreter 側）
    if (runtime.getIsEdgeActivatedHat(top->opcode)) {
        if (verbose) std::fprintf(stderr,
            "xyo-jit: skip edge hat %s\n", top->opcode.c_str());
        failed_.insert(top);
        return nullptr;
    }

    ScriptEntry& entry = cache_[top];
    if (entry.code) return entry.code.get();
    if (failed_.count(top)) return nullptr;

    const auto tStart = std::chrono::steady_clock::now();
    auto cs = std::make_unique<CompiledScript>();
    std::vector<std::string> fnNames;
    std::map<std::string, std::string> procNameMap;
    const std::string entryName =
        "xyo_script_" + std::to_string(scriptCounter_++);
    bool emitOk = false;

    llvmc::withContext(impl_->tsc, [&](llvm::LLVMContext* lc) {
        auto mod = std::make_unique<llvm::Module>("xyo_script", *lc);
        mod->setDataLayout(impl_->jit->getDataLayout());
        mod->setTargetTriple(
            llvmc::tripleArg(impl_->jit->getTargetTriple()));
        emitOk = jitEmitScript(*mod, *lc, runtime, thread.target, blocks,
                               topId, entryName, *cs, impl_->pinned, fnNames,
                               procNameMap, nullptr);
        if (emitOk) {
            // O2 相当の最適化 — varSlotData/alloca の昇格・冗長
            // タグチェック除去・式共有を行わないと生成コードが数倍遅い
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
            llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(
                llvm::OptimizationLevel::O2);
            mpm.run(*mod, mam);
            if (const char* d = std::getenv("XYO_JIT_DUMP2"))
                if (d[0] != '\0') mod->print(llvm::errs(), nullptr);
            if (auto err = impl_->jit->addIRModule(
                    llvm::orc::ThreadSafeModule(std::move(mod),
                                                impl_->tsc))) {
                llvm::consumeError(std::move(err));
                emitOk = false;
            }
        }
    });
    if (!emitOk) {
        if (verbose) std::fprintf(stderr,
            "xyo-jit: emit failed for %s (%s)\n",
            topId.c_str(), top->opcode.c_str());
        failed_.insert(top);
        return nullptr;
    }

    auto addrOr = impl_->jit->lookup(entryName);
    if (!addrOr) {
        llvm::consumeError(addrOr.takeError());
        failed_.insert(top);
        return nullptr;
    }
    cs->entry = reinterpret_cast<ScriptFn>(addrOr->getValue());
    for (auto& [vkey, fname] : procNameMap) {
        auto a = impl_->jit->lookup(fname);
        if (!a) { llvm::consumeError(a.takeError()); continue; }
        cs->procedures[vkey] = reinterpret_cast<ScriptFn>(a->getValue());
    }

    CompiledScript* raw = cs.get();
    entry.code = std::move(cs);
    if (verbose) {
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - tStart)
                              .count();
        std::fprintf(stderr, "xyo-jit: compiled %s (%s, %.1fms)\n",
                     topId.c_str(), top->opcode.c_str(), ms);
    }
    return raw;
}

/// AOT オブジェクトをロードしてスクリプトをキャッシュへ登録する。
/// 生成コードは slot（xyo_aot_slot_N）経由でホスト資源を参照するため、
/// ここで実アドレスを書き込んでからルート登録する。
bool JitEngine::loadAot(Runtime& runtime, const void* object, size_t size,
                        uint64_t projectHash, std::string* why) {
    auto fail = [&](const char* m) {
        if (why) *why = m;
        return false;
    };
    if (!ok()) return fail("JIT unavailable");
    if (!object || size == 0) return fail("empty object");
    if (!impl_->aotSlots.empty()) return fail("AOT already loaded");

    auto mb = llvm::MemoryBuffer::getMemBufferCopy(
        llvm::StringRef(static_cast<const char*>(object), size));
    if (auto err = impl_->jit->addObjectFile(std::move(mb))) {
        llvm::consumeError(std::move(err));
        return fail("addObjectFile failed");
    }
    auto metaSym = impl_->jit->lookup("xyo_aot_meta");
    if (!metaSym) {
        llvm::consumeError(metaSym.takeError());
        return fail("xyo_aot_meta missing");
    }
    const JsonValue meta = jsonParse(metaSym->toPtr<const char*>());
    if (!meta.isObject()) return fail("invalid AOT metadata");

    char hb[32];
    std::snprintf(hb, sizeof(hb), "%016llx",
                  (unsigned long long)projectHash);
    if (meta.get("hash").asString() != hb)
        return fail("project hash mismatch");

    // 文字列リテラル slot は途中で GC に晒されるため、SStr::create を
    // 呼ぶ前にルート登録しておく（未充填 slot は nullptr で無害）。
    Gc::get().addRootProvider(impl_.get(), [this](Gc& g) {
        if (!impl_) return;
        for (void** a : impl_->aotSlots) g.markPtr(*a);
    });

    for (const JsonValue& sv : meta.get("slots").asArray()) {
        const int kind = int(sv.get("k").asNumber());
        const std::string g = sv.get("g").asString();
        auto sym = impl_->jit->lookup(g);
        if (!sym) {
            llvm::consumeError(sym.takeError());
            return fail("AOT slot symbol missing");
        }
        void** addr = sym->toPtr<void**>();
        void* p = nullptr;
        switch (static_cast<AotSlotKind>(kind)) {
        case AotSlotKind::VarId:
        case AotSlotKind::Name:
            impl_->aotStrings.push_back(sv.get("s").asString());
            p = const_cast<char*>(impl_->aotStrings.back().c_str());
            break;
        case AotSlotKind::BlockCached: {
            // キーは "targetIndex\x01blockId"（block id はターゲット間で
            // 一意とは限らない）
            const std::string key = sv.get("s").asString();
            const size_t sep = key.find('\x01');
            const size_t ti =
                sep == std::string::npos
                    ? 0
                    : size_t(std::strtoul(key.substr(0, sep).c_str(),
                                          nullptr, 10));
            const std::string id =
                sep == std::string::npos ? key : key.substr(sep + 1);
            if (ti < runtime.targets.size()) {
                Blocks* b = runtime.targets[ti]->blocks;
                if (b && b->getBlock(id)) p = b->getCachedExecute(id);
            }
            if (!p) return fail("block for AOT slot not found");
            break;
        }
        case AotSlotKind::StrLit: {
            const std::string raw = hexDecode(sv.get("h").asString());
            std::u16string u(raw.size() / 2, u'\0');
            if (!u.empty())
                std::memcpy(u.data(), raw.data(), u.size() * 2);
            p = SStr::create(u);
            break;
        }
        case AotSlotKind::GpuProgram: {
            // キーは "targetIndex\x01loopBlockId"。AOT 時と同じ
            // 解析を再実行して Program を作り直す（決定論的）。
            // 失敗しても nullptr のまま — 生成コードは CPU ループへ。
            const std::string key = sv.get("s").asString();
            const size_t sep = key.find('\x01');
            const size_t ti =
                sep == std::string::npos
                    ? 0
                    : size_t(std::strtoul(key.substr(0, sep).c_str(),
                                          nullptr, 10));
            const std::string id =
                sep == std::string::npos ? key : key.substr(sep + 1);
            if (ti < runtime.targets.size()) {
                Blocks* b = runtime.targets[ti]->blocks;
                const Block* loop = b ? b->getBlock(id) : nullptr;
                if (loop) {
                    std::string gwhy;
                    if (auto pl =
                            gpu::compileParallelLoop(b, loop, &gwhy)) {
                        impl_->aotGpuProgs.push_back(
                            std::make_unique<gpu::ParallelLoop>(
                                std::move(*pl)));
                        p = &impl_->aotGpuProgs.back()->program;
                    } else if (std::getenv("XYO_AOT_VERBOSE")) {
                        std::fprintf(stderr,
                                     "xyo-jit: AOT GPU loop skip (%s): %s\n",
                                     id.c_str(), gwhy.c_str());
                    }
                }
            }
            break;
        }
        case AotSlotKind::AsciiCache:
            p = const_cast<void*>(
                static_cast<const void*>(xyo_ascii_cache()));
            break;
        case AotSlotKind::MovedHook:
            p = reinterpret_cast<void*>(penrt::movedHook());
            break;
        default:
            return fail("unknown AOT slot kind");
        }
        *addr = p;
        impl_->aotSlots.push_back(addr);
    }

    int nScripts = 0;
    for (const JsonValue& sj : meta.get("scripts").asArray()) {
        const std::string topId = sj.get("top").asString();
        auto es = impl_->jit->lookup(sj.get("entry").asString());
        if (!es) {
            llvm::consumeError(es.takeError());
            return fail("AOT script symbol missing");
        }
        auto cs = std::make_unique<CompiledScript>();
        cs->entry = es->toPtr<ScriptFn>();
        cs->mayYield = sj.get("mayYield").asBool();
        // Older objects lack affinity metadata (and may contain plain string
        // cache loads). Keep them serial; new objects also pin compat/GPU work.
        cs->mainThread = !sj.has("mainThread") ||
            sj.get("mainThread").asBool() || !impl_->aotGpuProgs.empty();
        for (const JsonValue& pj : sj.get("procs").asArray()) {
            auto ps = impl_->jit->lookup(pj.at(1).asString());
            if (!ps) {
                llvm::consumeError(ps.takeError());
                continue;
            }
            cs->procedures[pj.at(0).asString()] = ps->toPtr<ScriptFn>();
        }
        const size_t ti = size_t(sj.get("ti").asNumber());
        if (ti >= runtime.targets.size()) continue;
        Target* owner = runtime.targets[ti];
        const Block* top = owner->blocks ? owner->blocks->getBlock(topId)
                                         : nullptr;
        if (!top) continue;   // 実行されないスクリプト — 登録不要
        // AOT は全スクリプトを最適化済みで事前コンパイルしたもの —
        // 通常のコンパイルキャッシュへ登録し、再コンパイルしない。
        ScriptEntry& entry = cache_[top];
        entry.code = std::move(cs);
        entry.aot = true;
        ++nScripts;
    }

    std::fprintf(stderr, "aot: %d scripts precompiled (%zu slots)\n",
                 nScripts, impl_->aotSlots.size());
    if (std::getenv("XYO_AOT_VERBOSE"))
        std::fprintf(stderr, "xyo-jit: AOT loaded (%zu slots)\n",
                     impl_->aotSlots.size());
    return true;
}

void JitEngine::invalidate() {
    cache_.clear();
    profiles_.clear();
    failed_.clear();
    accessCache_.clear();
}

const AccessSet* JitEngine::accessFor(const Thread& thread,
                                      Runtime& runtime) {
    Target* t = thread.target;
    Blocks* blocks = thread.blockContainer;
    if (!t || !blocks) return nullptr;
    const Block* top = blocks->getBlock(thread.topBlock);
    if (!top) return nullptr;
    auto& perT = accessCache_[top];
    auto it = perT.find(t);
    if (it != perT.end()) return &it->second;
    AccessSet as = analyzeScript(runtime, *t, *blocks, top);
    auto [it2, ok] = perT.emplace(t, std::move(as));
    return &it2->second;
}

} // namespace xyo
