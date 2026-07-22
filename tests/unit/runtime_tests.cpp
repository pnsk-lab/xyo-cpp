#include "sjit/abi.hpp"
#include "sjit_clone.h"
#include "sjit_frame.h"
#include "sjit/jit.hpp"
#include "sjit/project_loader.hpp"
#include "sjit_opcode_effects.h"
#include "sjit_ownership.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int g_broadcast_count = 0;
int g_self_restart_backdrop_count = 0;

class ScopedEnvironment {
public:
    ScopedEnvironment(const char *name, const char *value) : name_(name) {
        const char *existing = std::getenv(name);
        existed_ = existing != nullptr;
        if (existing) {
            previous_ = existing;
        }
        const int result = value ?
            setenv(name, value, 1) : unsetenv(name);
        if (result != 0) {
            throw std::runtime_error("failed to configure test environment");
        }
    }

    ~ScopedEnvironment() {
        if (existed_) {
            setenv(name_.c_str(), previous_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

    ScopedEnvironment(const ScopedEnvironment &) = delete;
    ScopedEnvironment &operator=(const ScopedEnvironment &) = delete;

private:
    std::string name_;
    std::string previous_;
    bool existed_ = false;
};

struct ParallelGate {
    std::mutex mutex;
    std::condition_variable condition;
    int entered = 0;
    int active = 0;
    int peak_active = 0;
    bool released = false;
    bool timed_out = false;
};

struct ParallelGateEntryContext {
    SCompiledScript proof_script{};
    ParallelGate *gate = nullptr;

    ParallelGateEntryContext(int target_id, ParallelGate *gate_value) :
        gate(gate_value) {
        proof_script.target_id = target_id;
    }
};

struct OrderedStatusEntryContext {
    SCompiledScript proof_script{};
    SRuntimeStatus first_status = SJIT_STATUS_DONE;
    int calls = 0;

    OrderedStatusEntryContext(
        int target_id,
        SRuntimeStatus status) : first_status(status) {
        proof_script.target_id = target_id;
    }
};

SRuntimeStatus parallelGateEntry(
    SRuntime *runtime,
    SThread *thread,
    SFrame *frame) {
    (void)runtime;
    (void)thread;
    (void)frame;
    ParallelGateEntryContext *context =
        static_cast<ParallelGateEntryContext *>(thread->script_data);
    ParallelGate *gate = context ? context->gate : nullptr;
    if (!gate) {
        return SJIT_STATUS_ERROR;
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    ++gate->entered;
    ++gate->active;
    gate->peak_active = std::max(gate->peak_active, gate->active);
    if (gate->entered >= 2) {
        gate->released = true;
        gate->condition.notify_all();
    } else if (!gate->condition.wait_for(
                   lock,
                   std::chrono::seconds(2),
                   [&] { return gate->released; })) {
        gate->timed_out = true;
        gate->released = true;
        gate->condition.notify_all();
    }
    --gate->active;
    return SJIT_STATUS_DONE;
}

SRuntimeStatus rawDoneEntry(
    SRuntime *runtime,
    SThread *thread,
    SFrame *frame) {
    (void)runtime;
    (void)thread;
    (void)frame;
    return SJIT_STATUS_DONE;
}

SRuntimeStatus orderedStatusEntry(
    SRuntime *runtime,
    SThread *thread,
    SFrame *frame) {
    (void)runtime;
    (void)frame;
    OrderedStatusEntryContext *context =
        static_cast<OrderedStatusEntryContext *>(thread->script_data);
    if (!context) {
        return SJIT_STATUS_ERROR;
    }
    if (context->calls++ == 0) {
        return context->first_status;
    }
    return SJIT_STATUS_DONE;
}

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

SRuntimeStatus drawScript(SRuntime *runtime, SThread *thread, SFrame *frame) {
    (void)thread;
    SSprite *sprite = sjit_runtime_get_sprite(runtime, 1);
    assert(sprite);
    if (frame->pc == 0) {
        sjit_motion_goto_xy(runtime, sprite, sjit_make_number(10.0), sjit_make_number(20.0));
        frame->pc = 1;
        return SJIT_STATUS_YIELDED;
    }
    return SJIT_STATUS_DONE;
}

SRuntimeStatus broadcastReceiver(SRuntime *runtime, SThread *thread, SFrame *frame) {
    (void)runtime;
    (void)thread;
    if (frame->pc == 0) {
        ++g_broadcast_count;
        frame->pc = 1;
        return SJIT_STATUS_YIELDED;
    }
    return SJIT_STATUS_DONE;
}

SRuntimeStatus selfRestartingBackdropReceiver(
    SRuntime *runtime,
    SThread *thread,
    SFrame *frame) {
    (void)thread;
    ++g_self_restart_backdrop_count;
    if (g_self_restart_backdrop_count == 1) {
        SValue same_backdrop = sjit_make_string("b");
        sjit_looks_switch_backdrop(runtime, same_backdrop);
        sjit_value_destroy(same_backdrop);
        /* Simulate the continuation-PC store performed after a native helper
           returns.  The scheduler must reset this only after the entry has
           fully unwound. */
        frame->pc = 77;
    }
    return SJIT_STATUS_DONE;
}

SRuntimeStatus waitScript(SRuntime *runtime, SThread *thread, SFrame *frame) {
    (void)thread;
    switch (frame->pc) {
    case 0:
        frame->pc = 1;
        return SJIT_STATUS_YIELDED;
    case 1: {
        SRuntimeStatus status = sjit_control_wait(runtime, frame, sjit_make_number(0.1), 1);
        if (status != SJIT_STATUS_OK) {
            return status;
        }
        frame->pc = 2;
        return SJIT_STATUS_DONE;
    }
    default:
        return SJIT_STATUS_DONE;
    }
}

void writeLe16(std::ofstream &file, uint16_t value) {
    file.put(static_cast<char>(value & 0xffu));
    file.put(static_cast<char>((value >> 8) & 0xffu));
}

void writeLe32(std::ofstream &file, uint32_t value) {
    writeLe16(file, static_cast<uint16_t>(value & 0xffffu));
    writeLe16(file, static_cast<uint16_t>((value >> 16) & 0xffffu));
}

std::string writeSb3Fixture(const std::string &project_json) {
    const std::string path = "/tmp/xyo-loader-ops.sb3";
    const std::string name = "project.json";
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("failed to create sb3 fixture");
    }
    writeLe32(file, 0x04034b50u);
    writeLe16(file, 20);
    writeLe16(file, 0);
    writeLe16(file, 0);
    writeLe16(file, 0);
    writeLe16(file, 0);
    writeLe32(file, 0);
    writeLe32(file, static_cast<uint32_t>(project_json.size()));
    writeLe32(file, static_cast<uint32_t>(project_json.size()));
    writeLe16(file, static_cast<uint16_t>(name.size()));
    writeLe16(file, 0);
    file.write(name.data(), static_cast<std::streamsize>(name.size()));
    file.write(project_json.data(), static_cast<std::streamsize>(project_json.size()));
    return path;
}

SExpr *literalNumber(double number) {
    SValue value = sjit_make_number(number);
    return sjit_expr_create_literal(value);
}

SExpr *literalBool(bool value) {
    SValue scratch_value = sjit_make_bool(value ? 1 : 0);
    return sjit_expr_create_literal(scratch_value);
}

SExpr *literalString(const char *text) {
    SValue value = sjit_make_string(text);
    SExpr *expr = sjit_expr_create_literal(value);
    sjit_value_destroy(value);
    return expr;
}

void setOneArgumentProcedureCall(
    SStatement &statement,
    const char *procedure_name,
    const char *argument_name,
    SExpr *value) {
    statement.opcode = SJIT_STMT_PROCEDURE_CALL;
    statement.procedure_name = sjit_string_new(procedure_name);
    statement.argument_count = 1;
    statement.arguments = static_cast<SArgumentExpr *>(
        std::calloc(1, sizeof(SArgumentExpr)));
    require(statement.arguments, "allocate differential procedure argument");
    statement.arguments[0].name = sjit_string_new(argument_name);
    statement.arguments[0].value = value;
}

void setNoArgumentProcedureCall(
    SStatement &statement,
    const char *procedure_name) {
    statement.opcode = SJIT_STMT_PROCEDURE_CALL;
    statement.procedure_name = sjit_string_new(procedure_name);
}

SCompiledScript *makeOwnedScalarSetScript(
    int target_id,
    const char *variable_name,
    double value) {
    SCompiledScript *script = sjit_compiled_script_create(target_id, 1);
    require(script, "create ownership scalar script");
    script->statements[0].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[0].variable_name = sjit_string_new(variable_name);
    script->statements[0].value = literalNumber(value);
    return script;
}

void markRegistrationParallelSafeForSchedulerTest(
    SRuntime *runtime,
    int script_id,
    const SCompiledScript *proof_script) {
    require(runtime, "scheduler test runtime exists");
    for (int i = 0; i < runtime->script_count; ++i) {
        SScriptRegistration &registration = runtime->scripts[i];
        if (registration.script_id != script_id) {
            continue;
        }
        registration.ownership_analyzed = 1;
        registration.parallel_safe = 1;
        registration.parallel_owner_target_id = registration.target_id;
        registration.ownership_reject_flags = 0;
        registration.ownership_script = proof_script;
        registration.ownership_entry = registration.entry;
        registration.ownership_procedure_count = 0;
        require(
            static_cast<const void *>(proof_script) ==
                registration.script_data,
            "scheduler proof script shares the registered context address");
        return;
    }
    throw std::runtime_error("scheduler test registration not found");
}

void test_ownership_analysis_is_fail_closed() {
    SValue shared_literal = sjit_make_string("37");
    SExpr *first_literal = sjit_expr_create_literal(shared_literal);
    SExpr *second_literal = sjit_expr_create_literal(shared_literal);
    require(first_literal && second_literal &&
            first_literal->literal.ptr != second_literal->literal.ptr,
        "compiled string literals own independent refcounts and number caches");
    sjit_expr_destroy(first_literal);
    sjit_expr_destroy(second_literal);
    sjit_value_destroy(shared_literal);

    SRuntime *runtime = sjit_runtime_create();
    require(runtime, "create ownership-analysis runtime");
    SSprite *stage = sjit_runtime_create_sprite(runtime, "Stage", 1);
    SSprite *first = sjit_runtime_create_sprite(runtime, "OwnedA", 0);
    SSprite *second = sjit_runtime_create_sprite(runtime, "OwnedB", 0);
    require(stage && first && second, "create ownership-analysis targets");

    require(sjit_runtime_lookup_or_create_variable(
                runtime, first->base.id, "local", SJIT_VAR_SCALAR),
        "create owned local scalar");
    require(sjit_runtime_lookup_or_create_variable(
                runtime, stage->base.id, "local", SJIT_VAR_SCALAR),
        "create shadowed stage scalar");
    require(sjit_runtime_lookup_or_create_variable(
                runtime, stage->base.id, "globalOnly", SJIT_VAR_SCALAR),
        "create global scalar");
    require(sjit_runtime_lookup_or_create_variable(
                runtime, first->base.id, "items", SJIT_VAR_LIST),
        "create local list alias hazard");
    SVariable *dynamic = sjit_runtime_lookup_or_create_variable(
        runtime, first->base.id, "dynamic", SJIT_VAR_SCALAR);
    require(dynamic, "create dynamic scalar hazard");
    sjit_variable_set_scalar_kind(dynamic, SJIT_SCALAR_DYNAMIC);

    std::vector<SCompiledScript *> scripts;
    auto register_and_analyze = [&]
        (SCompiledScript *script, int script_id, int registered_target_id) {
        require(script, "ownership fixture script exists");
        scripts.push_back(script);
        require(sjit_runtime_register_script_with_data(
                    runtime,
                    registered_target_id,
                    script_id,
                    SJIT_HAT_EVENT_WHENFLAGCLICKED,
                    "",
                    1,
                    0,
                    sjit_script_interpreter_entry,
                    script),
            "register ownership fixture script");
        return sjit_runtime_analyze_script_ownership(
            runtime, script_id, script);
    };
    auto require_reject = [&]
        (int script_id, uint64_t flag, const char *message) {
        require(!sjit_runtime_script_parallel_safe(runtime, script_id) &&
                (sjit_runtime_script_ownership_reject_flags(
                    runtime, script_id) & flag) != 0,
            message);
    };

    SCompiledScript *safe = makeOwnedScalarSetScript(
        first->base.id, "local", 7.0);
    require(register_and_analyze(safe, 1200, first->base.id),
        "a shadowing original-sprite local number is ownership-proven");
    require(sjit_runtime_script_parallel_safe(runtime, 1200) &&
            sjit_runtime_script_ownership_reject_flags(runtime, 1200) == 0,
        "safe ownership metadata is explicit and hazard-free");

    SCompiledScript *safe_procedure = sjit_compiled_script_create(
        first->base.id, 1);
    require(safe_procedure, "create safe ownership procedure script");
    setNoArgumentProcedureCall(
        safe_procedure->statements[0], "pure procedure");
    safe_procedure->procedure_count = 1;
    safe_procedure->procedures = static_cast<SCompiledProcedure *>(
        std::calloc(1, sizeof(SCompiledProcedure)));
    require(safe_procedure->procedures, "allocate safe ownership procedure");
    safe_procedure->procedures[0].name = sjit_string_new("pure procedure");
    safe_procedure->procedures[0].statement_count = 1;
    safe_procedure->procedures[0].statements = static_cast<SStatement *>(
        std::calloc(1, sizeof(SStatement)));
    require(safe_procedure->procedures[0].statements,
        "allocate safe ownership procedure body");
    safe_procedure->procedures[0].statements[0].opcode =
        SJIT_STMT_CHANGE_VARIABLE;
    safe_procedure->procedures[0].statements[0].variable_name =
        sjit_string_new("local");
    safe_procedure->procedures[0].statements[0].value = literalNumber(1.0);
    require(register_and_analyze(
                safe_procedure, 1201, first->base.id),
        "a reachable pure local procedure is ownership-proven");

    SCompiledScript *global = makeOwnedScalarSetScript(
        first->base.id, "globalOnly", 1.0);
    require(!register_and_analyze(global, 1202, first->base.id),
        "stage fallback is not ownership-proven");
    require_reject(
        1202,
        SJIT_OWNERSHIP_REJECT_NONLOCAL_VARIABLE,
        "stage/global scalar forces sequential fallback");

    SCompiledScript *list = sjit_compiled_script_create(first->base.id, 1);
    require(list, "create list ownership hazard script");
    list->statements[0].opcode = SJIT_STMT_LIST_ADD;
    list->statements[0].variable_name = sjit_string_new("items");
    list->statements[0].value = literalNumber(1.0);
    require(!register_and_analyze(list, 1203, first->base.id),
        "local list deep aliases are not ownership-proven");
    require_reject(
        1203,
        SJIT_OWNERSHIP_REJECT_LIST_ALIAS,
        "list storage/refcounts force sequential fallback");

    SCompiledScript *pen = sjit_compiled_script_create(first->base.id, 1);
    require(pen, "create pen ownership hazard script");
    pen->statements[0].opcode = SJIT_STMT_PEN_DOWN;
    require(!register_and_analyze(pen, 1204, first->base.id),
        "pen state is shared with the renderer");
    require_reject(
        1204,
        SJIT_OWNERSHIP_REJECT_SHARED_EFFECT,
        "pen mutation forces sequential fallback");

    SCompiledScript *random = makeOwnedScalarSetScript(
        first->base.id, "local", 0.0);
    sjit_expr_destroy(random->statements[0].value);
    random->statements[0].value = sjit_expr_create_binary(
        SJIT_EXPR_RANDOM, literalNumber(1.0), literalNumber(10.0));
    require(!register_and_analyze(random, 1205, first->base.id),
        "process-global random state is not parallelized");
    require_reject(
        1205,
        SJIT_OWNERSHIP_REJECT_SHARED_RANDOM,
        "random reporter forces sequential fallback");

    SCompiledScript *wait = sjit_compiled_script_create(first->base.id, 1);
    require(wait, "create wait ownership hazard script");
    wait->statements[0].opcode = SJIT_STMT_WAIT;
    wait->statements[0].value = literalNumber(0.0);
    require(!register_and_analyze(wait, 1206, first->base.id),
        "wait redraw mutation is not parallelized");
    require_reject(
        1206,
        SJIT_OWNERSHIP_REJECT_SHARED_REDRAW,
        "wait forces sequential fallback despite owning its frame");

    SCompiledScript *missing = sjit_compiled_script_create(first->base.id, 1);
    require(missing, "create missing procedure ownership script");
    setNoArgumentProcedureCall(missing->statements[0], "missing");
    require(!register_and_analyze(missing, 1207, first->base.id),
        "unresolved procedure is not parallelized");
    require_reject(
        1207,
        SJIT_OWNERSHIP_REJECT_MISSING_PROCEDURE,
        "missing procedure forces sequential fallback");

    SCompiledScript *recursive = sjit_compiled_script_create(first->base.id, 1);
    require(recursive, "create recursive ownership script");
    setNoArgumentProcedureCall(recursive->statements[0], "recursive");
    recursive->procedure_count = 1;
    recursive->procedures = static_cast<SCompiledProcedure *>(
        std::calloc(1, sizeof(SCompiledProcedure)));
    require(recursive->procedures, "allocate recursive ownership procedure");
    recursive->procedures[0].name = sjit_string_new("recursive");
    recursive->procedures[0].statement_count = 1;
    recursive->procedures[0].statements = static_cast<SStatement *>(
        std::calloc(1, sizeof(SStatement)));
    require(recursive->procedures[0].statements,
        "allocate recursive ownership procedure body");
    setNoArgumentProcedureCall(
        recursive->procedures[0].statements[0], "recursive");
    require(!register_and_analyze(recursive, 1208, first->base.id),
        "recursive procedure remains conservative");
    require_reject(
        1208,
        SJIT_OWNERSHIP_REJECT_RECURSIVE_PROCEDURE,
        "recursive procedure forces sequential fallback");

    SCompiledScript *unknown = sjit_compiled_script_create(first->base.id, 1);
    require(unknown, "create unknown ownership script");
    unknown->statements[0].opcode = 999;
    require(!register_and_analyze(unknown, 1209, first->base.id),
        "unknown opcode is fail-closed");
    require_reject(
        1209,
        SJIT_OWNERSHIP_REJECT_UNKNOWN_OPCODE,
        "unknown opcode forces sequential fallback");

    SCompiledScript *dynamic_script = makeOwnedScalarSetScript(
        first->base.id, "dynamic", 1.0);
    require(!register_and_analyze(
                dynamic_script, 1210, first->base.id),
        "dynamic owning scalar is not parallelized");
    require_reject(
        1210,
        SJIT_OWNERSHIP_REJECT_OWNING_VALUE,
        "dynamic/string-capable scalar forces sequential fallback");

    SSprite *clone = sjit_clone_create(runtime, first);
    require(clone && !clone->base.is_original,
        "create non-original ownership target");
    SCompiledScript *clone_script = makeOwnedScalarSetScript(
        clone->base.id, "local", 2.0);
    require(!register_and_analyze(
                clone_script, 1211, clone->base.id),
        "clone target is not parallelized");
    require_reject(
        1211,
        SJIT_OWNERSHIP_REJECT_TARGET_NOT_EXCLUSIVE,
        "clone/COW target forces sequential fallback");

    SCompiledScript *mismatch = makeOwnedScalarSetScript(
        first->base.id, "local", 3.0);
    require(!register_and_analyze(mismatch, 1212, second->base.id),
        "registration/AST target mismatch is not parallelized");
    require_reject(
        1212,
        SJIT_OWNERSHIP_REJECT_INVALID_REGISTRATION,
        "registration provenance mismatch is fail-closed");

    require(sjit_runtime_register_script(
                runtime,
                second->base.id,
                1213,
                SJIT_HAT_EVENT_WHENFLAGCLICKED,
                "",
                1,
                0,
                rawDoneEntry),
        "register raw scheduler entry");
    require_reject(
        1213,
        SJIT_OWNERSHIP_REJECT_INVALID_REGISTRATION,
        "untyped raw registration remains sequential by default");

    SCompiledScript *forged_entry = makeOwnedScalarSetScript(
        second->base.id, "local", 4.0);
    scripts.push_back(forged_entry);
    require(sjit_runtime_register_script_with_data(
                runtime,
                second->base.id,
                1214,
                SJIT_HAT_EVENT_WHENFLAGCLICKED,
                "",
                1,
                0,
                rawDoneEntry,
                forged_entry) &&
            !sjit_runtime_analyze_script_ownership(
                runtime, 1214, forged_entry),
        "an arbitrary entry cannot borrow a safe-looking AST certificate");
    require_reject(
        1214,
        SJIT_OWNERSHIP_REJECT_INVALID_REGISTRATION,
        "entry-to-AST provenance mismatch is fail-closed");

    SVariable *stage_local = sjit_target_lookup_variable(
        &stage->base, 0, "local", SJIT_VAR_SCALAR);
    int stage_local_index = -1;
    for (int i = 0; i < stage->base.variable_count; ++i) {
        if (&stage->base.variables[i] == stage_local) {
            stage_local_index = i;
            break;
        }
    }
    require(stage_local_index >= 0, "locate stage cache hazard fixture");
    SCompiledScript *stale_variable_cache = makeOwnedScalarSetScript(
        first->base.id, "local", 5.0);
    SStatement &cached_statement = stale_variable_cache->statements[0];
    cached_statement.variable_cache_target_id = first->base.id;
    cached_statement.variable_cache_owner_target_id = stage->base.id;
    cached_statement.variable_cache_index = stage_local_index;
    cached_statement.variable_cache_type = SJIT_VAR_SCALAR;
    cached_statement.variable_cache_owner_is_original = 1;
    cached_statement.variable_cache_runtime = runtime;
    cached_statement.variable_cache_runtime_instance_id = runtime->instance_id;
    require(!register_and_analyze(
                stale_variable_cache, 1215, first->base.id),
        "a stale prebound stage cache cannot masquerade as a local access");
    require_reject(
        1215,
        SJIT_OWNERSHIP_REJECT_INVALID_REGISTRATION,
        "variable cache ownership mismatch is fail-closed");

    SCompiledScript *stale_procedure_cache = sjit_compiled_script_create(
        first->base.id, 1);
    require(stale_procedure_cache, "create stale procedure-cache script");
    setNoArgumentProcedureCall(
        stale_procedure_cache->statements[0], "safe procedure");
    stale_procedure_cache->statements[0].procedure_cache_valid = 1;
    stale_procedure_cache->statements[0].procedure_cache_index = 1;
    stale_procedure_cache->procedure_count = 2;
    stale_procedure_cache->procedures = static_cast<SCompiledProcedure *>(
        std::calloc(2, sizeof(SCompiledProcedure)));
    require(stale_procedure_cache->procedures,
        "allocate stale procedure-cache fixture");
    stale_procedure_cache->procedures[0].name =
        sjit_string_new("safe procedure");
    stale_procedure_cache->procedures[1].name =
        sjit_string_new("unsafe procedure");
    stale_procedure_cache->procedures[1].statement_count = 1;
    stale_procedure_cache->procedures[1].statements =
        static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(stale_procedure_cache->procedures[1].statements,
        "allocate unsafe cached procedure body");
    stale_procedure_cache->procedures[1].statements[0].opcode =
        SJIT_STMT_PEN_DOWN;
    require(!register_and_analyze(
                stale_procedure_cache, 1216, first->base.id),
        "procedure cache must resolve to the analyzed exact-name callee");
    require_reject(
        1216,
        SJIT_OWNERSHIP_REJECT_INVALID_TREE,
        "stale procedure cache cannot redirect a proven call");

    SCompiledScript *shared_literal_script = makeOwnedScalarSetScript(
        first->base.id, "local", 0.0);
    sjit_expr_destroy(shared_literal_script->statements[0].value);
    shared_literal_script->statements[0].value = literalString("12");
    SValue shared_literal_alias = sjit_value_clone(
        shared_literal_script->statements[0].value->literal);
    require(!register_and_analyze(
                shared_literal_script, 1217, first->base.id),
        "a refcount-shared literal is not ownership-proven");
    require_reject(
        1217,
        SJIT_OWNERSHIP_REJECT_OWNING_VALUE,
        "shared literal refcount/cache forces sequential fallback");
    sjit_value_destroy(shared_literal_alias);

    sjit_runtime_destroy(runtime);
    for (SCompiledScript *script : scripts) {
        sjit_compiled_script_destroy(script);
    }
}

void test_ownership_proven_scripts_use_thread_pool() {
    ScopedEnvironment pool_size("SJIT_THREAD_POOL_SIZE", "2");
    ScopedEnvironment pool_enabled("SJIT_DISABLE_THREAD_POOL", nullptr);
    SRuntime *runtime = sjit_runtime_create();
    SSprite *first = sjit_runtime_create_sprite(runtime, "ParallelA", 0);
    SSprite *second = sjit_runtime_create_sprite(runtime, "ParallelB", 0);
    require(runtime && first && second, "create analyzed parallel runtime");
    SVariable *first_value = sjit_runtime_lookup_or_create_variable(
        runtime, first->base.id, "value", SJIT_VAR_SCALAR);
    SVariable *second_value = sjit_runtime_lookup_or_create_variable(
        runtime, second->base.id, "value", SJIT_VAR_SCALAR);
    require(first_value && second_value, "create analyzed parallel scalars");
    SCompiledScript *first_script = makeOwnedScalarSetScript(
        first->base.id, "value", 11.0);
    SCompiledScript *second_script = makeOwnedScalarSetScript(
        second->base.id, "value", 22.0);
    require(sjit_runtime_register_script_with_data(
                runtime,
                first->base.id,
                1220,
                SJIT_HAT_EVENT_WHENFLAGCLICKED,
                "",
                1,
                0,
                sjit_script_interpreter_entry,
                first_script) &&
            sjit_runtime_register_script_with_data(
                runtime,
                second->base.id,
                1221,
                SJIT_HAT_EVENT_WHENFLAGCLICKED,
                "",
                1,
                0,
                sjit_script_interpreter_entry,
                second_script),
        "register analyzed parallel scripts");
    require(sjit_runtime_analyze_script_ownership(
                runtime, 1220, first_script) &&
            sjit_runtime_analyze_script_ownership(
                runtime, 1221, second_script),
        "both independent scripts pass ownership analysis");
    sjit_runtime_green_flag(runtime);
    require(sjit_runtime_tick(runtime) == SJIT_STATUS_DONE,
        "analyzed parallel scripts finish normally");
#ifdef SJIT_PROFILE_RUNTIME
    require(sjit_runtime_thread_pool_parallelism(runtime) == 1 &&
            sjit_runtime_parallel_batch_count(runtime) == 0 &&
            sjit_runtime_parallel_task_count(runtime) == 0,
        "non-atomic profiling counters force the sequential scheduler path");
#else
    require(sjit_runtime_thread_pool_parallelism(runtime) == 2 &&
            sjit_runtime_parallel_batch_count(runtime) == 1 &&
            sjit_runtime_parallel_task_count(runtime) == 2,
        "ownership metadata reaches one fixed two-task ThreadPool batch");
#endif
    require(first_value->value.tag == SJIT_VALUE_NUMBER &&
            first_value->value.number == 11.0 &&
            second_value->value.tag == SJIT_VALUE_NUMBER &&
            second_value->value.number == 22.0,
        "parallel interpreter entries preserve owned target state");
    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(first_script);
    sjit_compiled_script_destroy(second_script);
}

void test_parallel_scheduler_revalidates_owned_values() {
    ScopedEnvironment pool_size("SJIT_THREAD_POOL_SIZE", "2");
    ScopedEnvironment pool_enabled("SJIT_DISABLE_THREAD_POOL", nullptr);
    SRuntime *runtime = sjit_runtime_create();
    SSprite *first = sjit_runtime_create_sprite(runtime, "StaleA", 0);
    SSprite *second = sjit_runtime_create_sprite(runtime, "StaleB", 0);
    require(runtime && first && second, "create stale-certificate runtime");
    SVariable *first_value = sjit_runtime_lookup_or_create_variable(
        runtime, first->base.id, "value", SJIT_VAR_SCALAR);
    SVariable *second_value = sjit_runtime_lookup_or_create_variable(
        runtime, second->base.id, "value", SJIT_VAR_SCALAR);
    require(first_value && second_value, "create stale-certificate scalars");
    SCompiledScript *first_script = makeOwnedScalarSetScript(
        first->base.id, "value", 31.0);
    SCompiledScript *second_script = makeOwnedScalarSetScript(
        second->base.id, "value", 32.0);
    require(sjit_runtime_register_script_with_data(
                runtime,
                first->base.id,
                1222,
                SJIT_HAT_EVENT_WHENFLAGCLICKED,
                "",
                1,
                0,
                sjit_script_interpreter_entry,
                first_script) &&
            sjit_runtime_register_script_with_data(
                runtime,
                second->base.id,
                1223,
                SJIT_HAT_EVENT_WHENFLAGCLICKED,
                "",
                1,
                0,
                sjit_script_interpreter_entry,
                second_script) &&
            sjit_runtime_analyze_script_ownership(
                runtime, 1222, first_script) &&
            sjit_runtime_analyze_script_ownership(
                runtime, 1223, second_script),
        "create two initially valid ownership certificates");

    SValue stale_value = sjit_make_string("became owning after analysis");
    sjit_variable_set(first_value, stale_value);
    sjit_value_destroy(stale_value);
    require(sjit_runtime_script_parallel_safe(runtime, 1222),
        "registration retains its structural ownership certificate");

    sjit_runtime_green_flag(runtime);
    require(sjit_runtime_tick(runtime) == SJIT_STATUS_DONE &&
            sjit_runtime_parallel_batch_count(runtime) == 0 &&
            sjit_runtime_parallel_task_count(runtime) == 0,
        "stale owning state is revalidated and falls back sequentially");
    require(first_value->value.tag == SJIT_VALUE_NUMBER &&
            first_value->value.number == 31.0 &&
            second_value->value.tag == SJIT_VALUE_NUMBER &&
            second_value->value.number == 32.0,
        "runtime guard preserves sequential results while restoring primitives");
    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(first_script);
    sjit_compiled_script_destroy(second_script);
}

void test_parallel_manifest_ignores_unrelated_owning_locals() {
    ScopedEnvironment pool_size("SJIT_THREAD_POOL_SIZE", "2");
    ScopedEnvironment pool_enabled("SJIT_DISABLE_THREAD_POOL", nullptr);
    SRuntime *runtime = sjit_runtime_create();
    SSprite *first = sjit_runtime_create_sprite(runtime, "ManifestA", 0);
    SSprite *second = sjit_runtime_create_sprite(runtime, "ManifestB", 0);
    require(runtime && first && second, "create dependency-manifest runtime");

    SVariable *unrelated = sjit_runtime_lookup_or_create_variable(
        runtime, first->base.id, "unrelated", SJIT_VAR_SCALAR);
    require(unrelated, "create unrelated string local");
    SVariable *first_value = sjit_runtime_lookup_or_create_variable(
        runtime, first->base.id, "value", SJIT_VAR_SCALAR);
    SVariable *second_value = sjit_runtime_lookup_or_create_variable(
        runtime, second->base.id, "value", SJIT_VAR_SCALAR);
    unrelated = sjit_target_lookup_variable(
        &first->base, 0, "unrelated", SJIT_VAR_SCALAR);
    require(unrelated, "reacquire append-only unrelated local");
    sjit_variable_set_scalar_kind(unrelated, SJIT_SCALAR_STRING);
    SValue unrelated_value = sjit_make_string("not accessed by the script");
    sjit_variable_set(unrelated, unrelated_value);
    sjit_value_destroy(unrelated_value);
    SCompiledScript *first_script = makeOwnedScalarSetScript(
        first->base.id, "value", 61.0);
    SCompiledScript *second_script = makeOwnedScalarSetScript(
        second->base.id, "value", 62.0);
    require(first_value && second_value &&
            sjit_runtime_register_script_with_data(
                runtime,
                first->base.id,
                1228,
                SJIT_HAT_EVENT_WHENFLAGCLICKED,
                "",
                1,
                0,
                sjit_script_interpreter_entry,
                first_script) &&
            sjit_runtime_register_script_with_data(
                runtime,
                second->base.id,
                1229,
                SJIT_HAT_EVENT_WHENFLAGCLICKED,
                "",
                1,
                0,
                sjit_script_interpreter_entry,
                second_script) &&
            sjit_runtime_analyze_script_ownership(
                runtime, 1228, first_script) &&
            sjit_runtime_analyze_script_ownership(
                runtime, 1229, second_script),
        "only reached primitive locals enter the ownership manifest");
    require(runtime->scripts[0].ownership_variable_count == 1 &&
            runtime->scripts[0].ownership_variable_indices &&
            runtime->scripts[0].ownership_variable_indices[0] == 1 &&
            runtime->scripts[1].ownership_variable_count == 1 &&
            runtime->scripts[1].ownership_variable_indices &&
            runtime->scripts[1].ownership_variable_indices[0] == 0,
        "ownership certificates contain compact stable dependency indices");

    sjit_runtime_green_flag(runtime);
    require(sjit_runtime_tick(runtime) == SJIT_STATUS_DONE,
        "dependency-manifest scripts finish normally");
#ifdef SJIT_PROFILE_RUNTIME
    require(sjit_runtime_parallel_batch_count(runtime) == 0,
        "profile builds retain their forced sequential path");
#else
    require(sjit_runtime_parallel_batch_count(runtime) == 1 &&
            sjit_runtime_parallel_task_count(runtime) == 2,
        "unrelated owning locals do not reject a safe dependency batch");
#endif
    require(unrelated->value.tag == SJIT_VALUE_STRING &&
            first_value->value.tag == SJIT_VALUE_NUMBER &&
            first_value->value.number == 61.0 &&
            second_value->value.tag == SJIT_VALUE_NUMBER &&
            second_value->value.number == 62.0,
        "parallel execution preserves unrelated state and script results");

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(first_script);
    sjit_compiled_script_destroy(second_script);
}

void test_parallel_scheduler_rejects_stale_procedure_storage() {
    ScopedEnvironment pool_size("SJIT_THREAD_POOL_SIZE", "2");
    ScopedEnvironment pool_enabled("SJIT_DISABLE_THREAD_POOL", nullptr);
    SRuntime *runtime = sjit_runtime_create();
    SSprite *first = sjit_runtime_create_sprite(runtime, "StaleProcA", 0);
    SSprite *second = sjit_runtime_create_sprite(runtime, "StaleProcB", 0);
    require(runtime && first && second,
        "create stale-procedure certificate runtime");
    require(sjit_runtime_lookup_or_create_variable(
                runtime, first->base.id, "value", SJIT_VAR_SCALAR) &&
            sjit_runtime_lookup_or_create_variable(
                runtime, second->base.id, "value", SJIT_VAR_SCALAR),
        "create stale-procedure certificate scalars");

    SCompiledScript *first_script = sjit_compiled_script_create(
        first->base.id, 0);
    require(first_script, "create procedure-backed ownership script");
    first_script->procedure_count = 1;
    first_script->procedures = static_cast<SCompiledProcedure *>(
        std::calloc(1, sizeof(SCompiledProcedure)));
    require(first_script->procedures,
        "allocate procedure-backed ownership script");
    first_script->procedures[0].name = sjit_string_new("unused procedure");
    SCompiledScript *second_script = makeOwnedScalarSetScript(
        second->base.id, "value", 52.0);

    require(sjit_runtime_register_script_with_data(
                runtime,
                first->base.id,
                1226,
                SJIT_HAT_EVENT_WHENFLAGCLICKED,
                "",
                1,
                0,
                sjit_script_interpreter_entry,
                first_script) &&
            sjit_runtime_register_script_with_data(
                runtime,
                second->base.id,
                1227,
                SJIT_HAT_EVENT_WHENFLAGCLICKED,
                "",
                1,
                0,
                sjit_script_interpreter_entry,
                second_script) &&
            sjit_runtime_analyze_script_ownership(
                runtime, 1226, first_script) &&
            sjit_runtime_analyze_script_ownership(
                runtime, 1227, second_script),
        "create certificates before procedure storage changes");

    SCompiledProcedure *saved_procedures = first_script->procedures;
    first_script->procedures = nullptr;
    sjit_runtime_green_flag(runtime);
    require(sjit_runtime_tick(runtime) == SJIT_STATUS_DONE &&
            sjit_runtime_parallel_batch_count(runtime) == 0 &&
            sjit_runtime_parallel_task_count(runtime) == 0,
        "missing certified procedure storage fails closed without dispatch");
    first_script->procedures = saved_procedures;

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(first_script);
    sjit_compiled_script_destroy(second_script);
}

void test_duplicate_script_ids_disable_parallel_scheduling() {
    ScopedEnvironment pool_size("SJIT_THREAD_POOL_SIZE", "2");
    ScopedEnvironment pool_enabled("SJIT_DISABLE_THREAD_POOL", nullptr);
    SRuntime *runtime = sjit_runtime_create();
    SSprite *first = sjit_runtime_create_sprite(runtime, "DuplicateA", 0);
    SSprite *second = sjit_runtime_create_sprite(runtime, "DuplicateB", 0);
    SSprite *third = sjit_runtime_create_sprite(runtime, "DuplicateRaw", 0);
    require(runtime && first && second && third,
        "create duplicate-id runtime");
    SVariable *first_value = sjit_runtime_lookup_or_create_variable(
        runtime, first->base.id, "value", SJIT_VAR_SCALAR);
    SVariable *second_value = sjit_runtime_lookup_or_create_variable(
        runtime, second->base.id, "value", SJIT_VAR_SCALAR);
    SCompiledScript *first_script = makeOwnedScalarSetScript(
        first->base.id, "value", 41.0);
    SCompiledScript *second_script = makeOwnedScalarSetScript(
        second->base.id, "value", 42.0);
    require(first_value && second_value &&
            sjit_runtime_register_script_with_data(
                runtime,
                first->base.id,
                1224,
                SJIT_HAT_EVENT_WHENFLAGCLICKED,
                "",
                1,
                0,
                sjit_script_interpreter_entry,
                first_script) &&
            sjit_runtime_register_script_with_data(
                runtime,
                second->base.id,
                1225,
                SJIT_HAT_EVENT_WHENFLAGCLICKED,
                "",
                1,
                0,
                sjit_script_interpreter_entry,
                second_script) &&
            sjit_runtime_analyze_script_ownership(
                runtime, 1224, first_script) &&
            sjit_runtime_analyze_script_ownership(
                runtime, 1225, second_script),
        "create valid certificates before duplicate registration");
    require(sjit_runtime_register_script(
                runtime,
                third->base.id,
                1224,
                SJIT_HAT_EVENT_WHENFLAGCLICKED,
                "",
                1,
                0,
                rawDoneEntry),
        "register duplicate script id after certification");
    sjit_runtime_green_flag(runtime);
    require(sjit_runtime_tick(runtime) == SJIT_STATUS_DONE &&
            sjit_runtime_parallel_batch_count(runtime) == 0 &&
            sjit_runtime_parallel_task_count(runtime) == 0,
        "duplicate script identity invalidates pool eligibility at dispatch");
    require(first_value->value.tag == SJIT_VALUE_NUMBER &&
            first_value->value.number == 41.0 &&
            second_value->value.tag == SJIT_VALUE_NUMBER &&
            second_value->value.number == 42.0,
        "duplicate-id fallback preserves sequential script results");
    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(first_script);
    sjit_compiled_script_destroy(second_script);
}

void test_project_loader_parallelizes_proven_native_entries() {
#ifdef SJIT_PROFILE_RUNTIME
    return;
#else
    const std::string project_json = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "variables": {}, "lists": {}, "blocks": {}
        }, {
            "isStage": false,
            "name": "ParallelLoaderA",
            "variables": {"aId": ["value", 0]}, "lists": {},
            "blocks": {
                "hatA": {"opcode": "event_whenflagclicked", "next": "changeA", "topLevel": true,
                    "inputs": {}, "fields": {}},
                "changeA": {"opcode": "data_changevariableby", "next": null, "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "101"]]}, "fields": {"VARIABLE": ["value", "aId"]}}
            }
        }, {
            "isStage": false,
            "name": "ParallelLoaderB",
            "variables": {"bId": ["value", 0]}, "lists": {},
            "blocks": {
                "hatB": {"opcode": "event_whenflagclicked", "next": "changeB", "topLevel": true,
                    "inputs": {}, "fields": {}},
                "changeB": {"opcode": "data_changevariableby", "next": null, "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "202"]]}, "fields": {"VARIABLE": ["value", "bId"]}}
            }
        }]
    })json";
    ScopedEnvironment pool_size("SJIT_THREAD_POOL_SIZE", "2");
    ScopedEnvironment pool_enabled("SJIT_DISABLE_THREAD_POOL", nullptr);
    SRuntime *runtime = sjit_runtime_create();
    require(runtime, "create loader ownership runtime");
    sjit::ProjectLoadResult loaded = sjit::loadProjectIntoRuntime(
        runtime,
        writeSb3Fixture(project_json));
    require(loaded.ok && loaded.program.scripts.size() == 2 &&
            loaded.message.find("2 ownership-proven parallel scripts") !=
                std::string::npos,
        "project loader analyzes both independent sprite scripts");
    require(runtime->script_count == 2 &&
            runtime->scripts[0].entry != sjit_script_interpreter_entry &&
            runtime->scripts[1].entry != sjit_script_interpreter_entry &&
            sjit_runtime_script_parallel_safe(
                runtime, runtime->scripts[0].script_id) &&
            sjit_runtime_script_parallel_safe(
                runtime, runtime->scripts[1].script_id),
        "ownership certificates are attached to native JIT entries");
    sjit_runtime_green_flag(runtime);
    require(sjit_runtime_tick(runtime) == SJIT_STATUS_DONE &&
            sjit_runtime_parallel_batch_count(runtime) == 1 &&
            sjit_runtime_parallel_task_count(runtime) == 2,
        "loader-produced native entries execute in one ThreadPool batch");
    SSprite *first = sjit_runtime_get_sprite_by_name(
        runtime, "ParallelLoaderA");
    SSprite *second = sjit_runtime_get_sprite_by_name(
        runtime, "ParallelLoaderB");
    SVariable *first_value = first ? sjit_target_lookup_variable(
        &first->base, 0, "value", SJIT_VAR_SCALAR) : nullptr;
    SVariable *second_value = second ? sjit_target_lookup_variable(
        &second->base, 0, "value", SJIT_VAR_SCALAR) : nullptr;
    require(first_value && first_value->value.tag == SJIT_VALUE_NUMBER &&
            first_value->value.number == 101.0 &&
            second_value && second_value->value.tag == SJIT_VALUE_NUMBER &&
            second_value->value.number == 202.0,
        "parallel native loader entries preserve both local results");
    sjit_runtime_destroy(runtime);
#endif
}

void test_jit_runtime_pool_workers_outlive_module() {
#ifdef SJIT_PROFILE_RUNTIME
    return;
#else
    ScopedEnvironment pool_size("SJIT_THREAD_POOL_SIZE", "2");
    ScopedEnvironment pool_enabled("SJIT_DISABLE_THREAD_POOL", nullptr);
    SRuntime *runtime = sjit_runtime_create();
    SSprite *first = sjit_runtime_create_sprite(runtime, "PoolHostA", 0);
    SSprite *second = sjit_runtime_create_sprite(runtime, "PoolHostB", 0);
    SCompiledScript *first_script = first ?
        sjit_compiled_script_create(first->base.id, 0) : nullptr;
    SCompiledScript *second_script = second ?
        sjit_compiled_script_create(second->base.id, 0) : nullptr;
    require(runtime && first && second && first_script && second_script,
        "create JIT-runtime pool lifetime fixtures");

    {
        sjit::JitEngine jit;
        sjit::SRuntimeTickFn tick = jit.runtimeTick();
        sjit::SRuntimeVoidFn green_flag = jit.runtimeGreenFlag();
        require(tick && green_flag,
            "load LLVM runtime with host-resident pool symbols");
        require(sjit_runtime_register_script_with_data(
                    runtime,
                    first->base.id,
                    1228,
                    SJIT_HAT_EVENT_WHENFLAGCLICKED,
                    "",
                    1,
                    0,
                    sjit_script_interpreter_entry,
                    first_script) &&
                sjit_runtime_register_script_with_data(
                    runtime,
                    second->base.id,
                    1229,
                    SJIT_HAT_EVENT_WHENFLAGCLICKED,
                    "",
                    1,
                    0,
                    sjit_script_interpreter_entry,
                    second_script) &&
                sjit_runtime_analyze_script_ownership(
                    runtime, 1228, first_script) &&
                sjit_runtime_analyze_script_ownership(
                    runtime, 1229, second_script),
            "certify two LLVM-runtime pool lifetime entries");
        green_flag(runtime);
        require(tick(runtime) == SJIT_STATUS_DONE &&
                sjit_runtime_thread_pool_parallelism(runtime) == 2 &&
                sjit_runtime_parallel_batch_count(runtime) == 1,
            "LLVM scheduler initializes a host-resident worker pool");
    }

    /* JitEngine and its runtime object are now unloaded.  Pool teardown must
       remain executable because worker start routines live in the host. */
    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(first_script);
    sjit_compiled_script_destroy(second_script);
#endif
}

void test_scheduler_thread_pool_overlaps_and_serializes_owner_conflicts() {
#ifdef SJIT_PROFILE_RUNTIME
    return;
#else
    ScopedEnvironment pool_size("SJIT_THREAD_POOL_SIZE", "2");
    ScopedEnvironment pool_enabled("SJIT_DISABLE_THREAD_POOL", nullptr);
    {
        SRuntime *runtime = sjit_runtime_create();
        SSprite *first = sjit_runtime_create_sprite(runtime, "GateA", 0);
        SSprite *second = sjit_runtime_create_sprite(runtime, "GateB", 0);
        require(runtime && first && second, "create scheduler gate targets");
        ParallelGate gate;
        ParallelGateEntryContext first_context(first->base.id, &gate);
        ParallelGateEntryContext second_context(second->base.id, &gate);
        require(sjit_runtime_register_script_with_data(
                    runtime,
                    first->base.id,
                    1230,
                    SJIT_HAT_EVENT_WHENFLAGCLICKED,
                    "",
                    1,
                    0,
                    parallelGateEntry,
                    &first_context) &&
                sjit_runtime_register_script_with_data(
                    runtime,
                    second->base.id,
                    1231,
                    SJIT_HAT_EVENT_WHENFLAGCLICKED,
                    "",
                    1,
                    0,
                    parallelGateEntry,
                    &second_context),
            "register scheduler gate entries");
        /* This test isolates scheduler mechanics.  The preceding analyzer
           test proves that production code alone creates these fields. */
        markRegistrationParallelSafeForSchedulerTest(
            runtime, 1230, &first_context.proof_script);
        markRegistrationParallelSafeForSchedulerTest(
            runtime, 1231, &second_context.proof_script);
        sjit_runtime_green_flag(runtime);
        sjit_runtime_tick(runtime);
        require(!gate.timed_out && gate.entered == 2 &&
                gate.peak_active == 2,
            "two distinct proven owners overlap on persistent pool workers");
        require(sjit_runtime_parallel_batch_count(runtime) == 1 &&
                sjit_runtime_parallel_task_count(runtime) == 2,
            "scheduler records the actual overlapping batch");
        sjit_runtime_destroy(runtime);
    }

    {
        SRuntime *runtime = sjit_runtime_create();
        SSprite *owner = sjit_runtime_create_sprite(runtime, "SameOwner", 0);
        require(runtime && owner, "create same-owner serialization runtime");
        SVariable *first_value = sjit_runtime_lookup_or_create_variable(
            runtime, owner->base.id, "first", SJIT_VAR_SCALAR);
        SVariable *second_value = sjit_runtime_lookup_or_create_variable(
            runtime, owner->base.id, "second", SJIT_VAR_SCALAR);
        require(first_value && second_value, "create same-owner scalars");
        SCompiledScript *first_script = makeOwnedScalarSetScript(
            owner->base.id, "first", 1.0);
        SCompiledScript *second_script = makeOwnedScalarSetScript(
            owner->base.id, "second", 2.0);
        require(sjit_runtime_register_script_with_data(
                    runtime,
                    owner->base.id,
                    1232,
                    SJIT_HAT_EVENT_WHENFLAGCLICKED,
                    "",
                    1,
                    0,
                    sjit_script_interpreter_entry,
                    first_script) &&
                sjit_runtime_register_script_with_data(
                    runtime,
                    owner->base.id,
                    1233,
                    SJIT_HAT_EVENT_WHENFLAGCLICKED,
                    "",
                    1,
                    0,
                    sjit_script_interpreter_entry,
                    second_script),
            "register same-owner scripts");
        require(sjit_runtime_analyze_script_ownership(
                    runtime, 1232, first_script) &&
                sjit_runtime_analyze_script_ownership(
                    runtime, 1233, second_script),
            "same-owner scripts are individually ownership-safe");
        sjit_runtime_green_flag(runtime);
        sjit_runtime_tick(runtime);
        require(sjit_runtime_parallel_batch_count(runtime) == 0 &&
                sjit_runtime_parallel_task_count(runtime) == 0,
            "same owner is split into sequential single-entry batches");
        require(first_value->value.number == 1.0 &&
                second_value->value.number == 2.0,
            "same-owner sequential fallback preserves both writes");
        sjit_runtime_destroy(runtime);
        sjit_compiled_script_destroy(first_script);
        sjit_compiled_script_destroy(second_script);
    }
#endif
}

void test_parallel_scheduler_matches_sequential_status_order() {
#ifdef SJIT_PROFILE_RUNTIME
    return;
#else
    struct ThreadState {
        int id;
        int target_id;
        int script_id;
        int status;

        bool operator==(const ThreadState &other) const {
            return id == other.id && target_id == other.target_id &&
                script_id == other.script_id && status == other.status;
        }
    };
    struct Snapshot {
        SRuntimeStatus first_tick = SJIT_STATUS_ERROR;
        SRuntimeStatus second_tick = SJIT_STATUS_ERROR;
        std::vector<ThreadState> first_threads;
        int final_thread_count = -1;
        int calls[3] = {0, 0, 0};
        uint64_t batches = 0;
        uint64_t tasks = 0;
    };

    const auto run = [](int parallelism) {
        ScopedEnvironment pool_size(
            "SJIT_THREAD_POOL_SIZE",
            parallelism > 1 ? "2" : "1");
        ScopedEnvironment pool_enabled("SJIT_DISABLE_THREAD_POOL", nullptr);
        Snapshot snapshot;
        SRuntime *runtime = sjit_runtime_create();
        require(runtime, "create ordered-status runtime");
        SSprite *targets[3] = {
            sjit_runtime_create_sprite(runtime, "StatusA", 0),
            sjit_runtime_create_sprite(runtime, "StatusB", 0),
            sjit_runtime_create_sprite(runtime, "StatusC", 0)};
        require(targets[0] && targets[1] && targets[2],
            "create ordered-status targets");
        OrderedStatusEntryContext contexts[3] = {
            {targets[0]->base.id, SJIT_STATUS_YIELD_TICK},
            {targets[1]->base.id, SJIT_STATUS_WAITING},
            {targets[2]->base.id, SJIT_STATUS_DONE}};
        for (int i = 0; i < 3; ++i) {
            const int script_id = 1240 + i;
            require(sjit_runtime_register_script_with_data(
                        runtime,
                        targets[i]->base.id,
                        script_id,
                        SJIT_HAT_EVENT_WHENFLAGCLICKED,
                        "",
                        1,
                        0,
                        orderedStatusEntry,
                        &contexts[i]),
                "register ordered-status entry");
            markRegistrationParallelSafeForSchedulerTest(
                runtime, script_id, &contexts[i].proof_script);
        }
        sjit_runtime_green_flag(runtime);
        snapshot.first_tick = sjit_runtime_tick(runtime);
        for (int i = 0; i < runtime->thread_count; ++i) {
            const SThread *thread = runtime->threads[i];
            snapshot.first_threads.push_back({
                thread->id,
                thread->target_id,
                thread->script_id,
                thread->status});
            if (thread->status == SJIT_THREAD_PROMISE_WAIT) {
                runtime->threads[i]->status = SJIT_THREAD_RUNNING;
            }
        }
        snapshot.second_tick = sjit_runtime_tick(runtime);
        snapshot.final_thread_count = runtime->thread_count;
        for (int i = 0; i < 3; ++i) {
            snapshot.calls[i] = contexts[i].calls;
        }
        snapshot.batches = sjit_runtime_parallel_batch_count(runtime);
        snapshot.tasks = sjit_runtime_parallel_task_count(runtime);
        sjit_runtime_destroy(runtime);
        return snapshot;
    };

    const Snapshot sequential = run(1);
    const Snapshot parallel = run(2);
    require(parallel.first_tick == sequential.first_tick &&
            parallel.second_tick == sequential.second_tick &&
            parallel.first_threads == sequential.first_threads &&
            parallel.final_thread_count == sequential.final_thread_count &&
            std::equal(
                std::begin(parallel.calls),
                std::end(parallel.calls),
                std::begin(sequential.calls)),
        "parallel completion is committed in exact sequential status order");
    require(sequential.batches == 0 && sequential.tasks == 0 &&
            parallel.batches == 2 && parallel.tasks == 5,
        "status differential exercised sequential and parallel scheduler paths");
    require(parallel.first_threads.size() == 2 &&
            parallel.first_threads[0].status == SJIT_THREAD_PROMISE_WAIT &&
            parallel.first_threads[1].status == SJIT_THREAD_YIELD_TICK &&
            parallel.final_thread_count == 0 &&
            parallel.calls[0] == 2 && parallel.calls[1] == 2 &&
            parallel.calls[2] == 1,
        "yield-tick, waiting, done, and resumed cleanup retain stable order");
#endif
}

SCompiledScript *makeDirectionFallbackScript(int target_id) {
    SCompiledScript *script = sjit_compiled_script_create(target_id, 3);
    require(script, "create direction fallback script");

    script->statements[0].opcode = SJIT_STMT_IF;
    script->statements[0].condition = sjit_expr_create_binary(
        SJIT_EXPR_GT,
        sjit_expr_create_direction(),
        literalNumber(100.0));
    script->statements[0].substack_count = 1;
    script->statements[0].substack = static_cast<SStatement *>(
        std::calloc(1, sizeof(SStatement)));
    require(script->statements[0].substack, "allocate direction condition body");
    script->statements[0].substack[0].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[0].substack[0].variable_name = sjit_string_new("condition_result");
    script->statements[0].substack[0].value = literalNumber(1.0);

    script->statements[1].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[1].variable_name = sjit_string_new("numeric_result");
    script->statements[1].value = sjit_expr_create_binary(
        SJIT_EXPR_ADD,
        sjit_expr_create_direction(),
        literalNumber(1.0));

    script->statements[2].opcode = SJIT_STMT_LIST_ADD;
    script->statements[2].variable_name = sjit_string_new("direction_items");
    script->statements[2].value = sjit_expr_create_direction();
    return script;
}

void test_opcode_effects_and_reporter_differential_fallback() {
    const OpcodeEffects direction_effects = sjit_expr_opcode_effects(SJIT_EXPR_DIRECTION);
    const OpcodeEffects contains_effects = sjit_expr_opcode_effects(SJIT_EXPR_CONTAINS);
    const OpcodeEffects wait_effects = sjit_statement_opcode_effects(SJIT_STMT_WAIT);
    const OpcodeEffects set_effects = sjit_statement_opcode_effects(SJIT_STMT_SET_VARIABLE);
    require(direction_effects.requiresInterpreter, "direction reporter is explicitly interpreter-only");
    require(!contains_effects.requiresInterpreter,
        "contains reporter has checked native value-slot lowering");
    require(wait_effects.canYield, "wait is a centralized yield effect");
    require(set_effects.canMutateTarget && set_effects.canChangeValueType,
        "variable set effects describe target and type mutation");
    require(sjit_statement_opcode_effects(SJIT_STMT_STOP_ALL).canCallUnknown,
        "stop-all records its scheduler-wide control effect");
    require(sjit_expr_opcode_effects(999).requiresInterpreter,
        "unknown reporter effects conservatively require interpreter");

    struct Results {
        double condition = 0.0;
        double numeric = 0.0;
        double list_item = 0.0;
        bool used_fallback = false;
        std::string reason;
    };
    auto run = [](bool jit_enabled) -> Results {
        Results results;
        SRuntime *runtime = sjit_runtime_create();
        SSprite *sprite = sjit_runtime_create_sprite(runtime, "DirectionSprite", 0);
        require(runtime && sprite, "create direction differential runtime");
        sprite->direction = 123.0;
        SVariable *list_variable = sjit_runtime_lookup_or_create_variable(
            runtime, sprite->base.id, "direction_items", SJIT_VAR_LIST);
        require(list_variable && list_variable->value.tag == SJIT_VALUE_LIST,
            "create direction differential list");

        SCompiledScript *script = makeDirectionFallbackScript(sprite->base.id);
        std::unique_ptr<sjit::JitEngine> jit;
        SScriptEntryFn entry = sjit_script_interpreter_entry;
        if (jit_enabled) {
            jit = std::make_unique<sjit::JitEngine>();
            entry = jit->compileScript(*script, "sjit_direction_fallback", runtime);
            results.used_fallback = entry == sjit_script_interpreter_entry;
            results.reason = jit->lastFallbackReason();
        }
        require(sjit_runtime_register_script_with_data(
            runtime,
            sprite->base.id,
            jit_enabled ? 702 : 701,
            SJIT_HAT_EVENT_WHENFLAGCLICKED,
            "",
            1,
            0,
            entry,
            script),
            "register direction differential script");
        sjit_runtime_green_flag(runtime);
        sjit_runtime_tick(runtime);

        SVariable *condition = sjit_runtime_lookup_or_create_variable(
            runtime, sprite->base.id, "condition_result", SJIT_VAR_SCALAR);
        SVariable *numeric = sjit_runtime_lookup_or_create_variable(
            runtime, sprite->base.id, "numeric_result", SJIT_VAR_SCALAR);
        SList *items = static_cast<SList *>(list_variable->value.ptr);
        SValue item = sjit_list_get(items, 1);
        results.condition = condition ? sjit_to_number(runtime, condition->value) : 0.0;
        results.numeric = numeric ? sjit_to_number(runtime, numeric->value) : 0.0;
        results.list_item = sjit_to_number(runtime, item);
        sjit_value_destroy(item);

        sjit_runtime_destroy(runtime);
        jit.reset();
        sjit_compiled_script_destroy(script);
        return results;
    };

    const Results interpreted = run(false);
    const Results enabled = run(true);
    require(enabled.used_fallback, "JIT-enabled execution selects explicit reporter fallback");
    require(enabled.reason.find("direction") != std::string::npos,
        "fallback reason identifies the reporter");
    require(interpreted.condition == 1.0 && interpreted.numeric == 124.0 && interpreted.list_item == 123.0,
        "interpreter evaluates direction in condition, arithmetic, and list mutation");
    require(enabled.condition == interpreted.condition &&
            enabled.numeric == interpreted.numeric &&
            enabled.list_item == interpreted.list_item,
        "JIT-enabled and interpreter-only executions are differential matches");

    SCompiledScript *contains_script = sjit_compiled_script_create(1, 1);
    require(contains_script, "create contains fallback script");
    contains_script->statements[0].opcode = SJIT_STMT_SET_VARIABLE;
    contains_script->statements[0].variable_name = sjit_string_new("contains_result");
    contains_script->statements[0].value = sjit_expr_create_binary(
        SJIT_EXPR_CONTAINS,
        literalString("Scratch"),
        literalString("rat"));
    SRuntime *contains_runtime = sjit_runtime_create();
    SSprite *contains_stage = sjit_runtime_create_sprite(
        contains_runtime,
        "Stage",
        1);
    require(contains_runtime && contains_stage,
        "create native contains runtime");
    {
        sjit::JitEngine jit;
        SScriptEntryFn contains_entry = jit.compileScript(
            *contains_script,
            "sjit_contains_native",
            contains_runtime);
        require(contains_entry != sjit_script_interpreter_entry &&
                jit.lastFallbackReason().empty(),
            "contains reporter lowers to the shared native coercion helper");
        require(sjit_runtime_register_script_with_data(
                    contains_runtime,
                    contains_stage->base.id,
                    703,
                    SJIT_HAT_EVENT_WHENFLAGCLICKED,
                    "",
                    1,
                    0,
                    contains_entry,
                    contains_script),
            "register native contains script");
        sjit_runtime_green_flag(contains_runtime);
        sjit_runtime_tick(contains_runtime);
        SVariable *contains_result = sjit_runtime_lookup_or_create_variable(
            contains_runtime,
            contains_stage->base.id,
            "contains_result",
            SJIT_VAR_SCALAR);
        require(contains_result && sjit_to_bool(
                    contains_runtime,
                    contains_result->value),
            "native contains preserves case-insensitive Scratch semantics");
    }
    sjit_runtime_destroy(contains_runtime);
    sjit_compiled_script_destroy(contains_script);

    const std::string unknown_project = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "variables": {"outVar": ["out", 0]},
            "lists": {},
            "blocks": {
                "hat": {"opcode": "event_whenflagclicked", "next": "set", "topLevel": true, "inputs": {}, "fields": {}},
                "set": {"opcode": "data_setvariableto", "next": null, "topLevel": false,
                    "inputs": {"VALUE": [2, "unknown"]}, "fields": {"VARIABLE": ["out", "outVar"]}},
                "unknown": {"opcode": "sensing_coloristouchingcolor", "next": null, "topLevel": false,
                    "inputs": {}, "fields": {}}
            }
        }]
    })json";
    SRuntime *unknown_runtime = sjit_runtime_create();
    sjit::ProjectLoadResult unknown = sjit::loadProjectIntoRuntime(
        unknown_runtime, writeSb3Fixture(unknown_project));
    require(!unknown.ok && unknown.message.find("unsupported reporter opcode") != std::string::npos,
        "truly unknown reporters fail explicitly instead of becoming empty literals");
    sjit_runtime_destroy(unknown_runtime);

    const std::string stop_all_project = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "variables": {},
            "lists": {},
            "blocks": {
                "hat": {"opcode": "event_whenflagclicked", "next": "stop", "topLevel": true, "inputs": {}, "fields": {}},
                "stop": {"opcode": "control_stop", "next": null, "topLevel": false,
                    "inputs": {}, "fields": {"STOP_OPTION": ["all", null]},
                    "mutation": {"tagName": "mutation", "children": [], "hasnext": "false"}}
            }
        }]
    })json";
    SRuntime *stop_runtime = sjit_runtime_create();
    sjit::ProjectLoadResult stop_result = sjit::loadProjectIntoRuntime(
        stop_runtime, writeSb3Fixture(stop_all_project));
    require(stop_result.ok && stop_result.program.scripts.size() == 1,
        "loader accepts Scratch stop-all explicitly");
    require(stop_result.program.scripts[0]->statement_count == 1 &&
            stop_result.program.scripts[0]->statements[0].opcode == SJIT_STMT_STOP_ALL,
        "loader preserves stop-all instead of converting it to a no-op");
    sjit_runtime_green_flag(stop_runtime);
    sjit_runtime_tick(stop_runtime);
    require(stop_runtime->stopped != 0 && sjit_runtime_thread_count(stop_runtime) == 0,
        "stop-all kills every scheduled thread and terminates its own entry");
    stop_result.program.jit.reset();
    sjit_runtime_destroy(stop_runtime);
}

void test_backdrop_switch_loader_interpreter_and_semantics() {
    const OpcodeEffects effects = sjit_statement_opcode_effects(
        SJIT_STMT_LOOKS_SWITCH_BACKDROP);
    require(!effects.requiresInterpreter && effects.canMutateTarget &&
            effects.canCallUnknown,
        "backdrop switching has a checked JIT bridge and schedules hats");

    SRuntime *runtime = sjit_runtime_create();
    SSprite *stage = sjit_runtime_create_sprite(runtime, "Stage", 1);
    require(runtime && stage, "create backdrop semantics runtime");
    const char *names[] = {"a", "b", "c", "2"};
    require(sjit_sprite_set_costume_names(stage, names, 4),
        "store runtime-owned backdrop names");

    sjit_looks_switch_backdrop(runtime, sjit_make_number(2.0));
    require(stage->current_costume == 1,
        "numeric backdrop inputs are one-based");

    SValue named_number = sjit_make_string("2");
    sjit_looks_switch_backdrop(runtime, named_number);
    sjit_value_destroy(named_number);
    require(stage->current_costume == 3,
        "backdrop names take priority over numeric string coercion");

    SValue wrapped_number = sjit_make_string("10");
    sjit_looks_switch_backdrop(runtime, wrapped_number);
    sjit_value_destroy(wrapped_number);
    require(stage->current_costume == 1,
        "numeric backdrop strings wrap after one-based conversion");

    sjit_looks_switch_backdrop(runtime, sjit_make_number(-1.0));
    require(stage->current_costume == 2,
        "negative numeric backdrop indices wrap like Scratch");

    sjit_looks_switch_backdrop(runtime, sjit_make_number(0.5));
    require(stage->current_costume == 0,
        "half-positive backdrop indices use Scratch rounding");
    sjit_looks_switch_backdrop(runtime, sjit_make_number(-0.5));
    require(stage->current_costume == 3,
        "half-negative backdrop indices round toward positive infinity");
    sjit_looks_switch_backdrop(
        runtime,
        sjit_make_number(std::numeric_limits<double>::quiet_NaN()));
    require(stage->current_costume == 0,
        "NaN backdrop indices select the first backdrop");
    sjit_looks_switch_backdrop(
        runtime,
        sjit_make_number(std::numeric_limits<double>::infinity()));
    require(stage->current_costume == 0,
        "infinite backdrop indices select the first backdrop");

    sjit_looks_switch_backdrop(runtime, sjit_make_bool(0));
    require(stage->current_costume == 3,
        "false falls back to numeric zero and selects the last backdrop");

    SValue unknown = sjit_make_string("missing");
    sjit_looks_switch_backdrop(runtime, unknown);
    sjit_value_destroy(unknown);
    require(stage->current_costume == 3,
        "unknown nonnumeric backdrop names leave the selection unchanged");

    SValue next = sjit_make_string("next backdrop");
    sjit_looks_switch_backdrop(runtime, next);
    sjit_value_destroy(next);
    require(stage->current_costume == 0,
        "next backdrop wraps at the end");

    SValue previous = sjit_make_string("previous backdrop");
    sjit_looks_switch_backdrop(runtime, previous);
    sjit_value_destroy(previous);
    require(stage->current_costume == 3,
        "previous backdrop wraps at the beginning");

    const char *token_names[] = {"next backdrop", "b", "c", "2"};
    require(sjit_sprite_set_costume_names(stage, token_names, 4),
        "replace backdrop metadata atomically");
    sjit_sprite_set_costume(runtime, stage, 2);
    next = sjit_make_string("next backdrop");
    sjit_looks_switch_backdrop(runtime, next);
    sjit_value_destroy(next);
    require(stage->current_costume == 0,
        "a backdrop name overrides the next-backdrop token");

    sjit_sprite_set_costume(runtime, stage, 1);
    SValue random = sjit_make_string("random backdrop");
    sjit_looks_switch_backdrop(runtime, random);
    sjit_value_destroy(random);
    require(stage->current_costume != 1,
        "random backdrop excludes the currently selected backdrop");

    sjit_sprite_set_costume(runtime, stage, 1);
    require(sjit_runtime_register_script(
                runtime,
                stage->base.id,
                719,
                SJIT_HAT_EVENT_WHENBACKDROPSWITCHESTO,
                "B",
                1,
                0,
                broadcastReceiver),
        "register backdrop-switch hat");
    unknown = sjit_make_string("missing");
    sjit_looks_switch_backdrop(runtime, unknown);
    sjit_value_destroy(unknown);
    require(sjit_runtime_script_invocation_count(runtime, 719) == 1,
        "invalid input still starts the final backdrop's case-insensitive hat");
    unknown = sjit_make_string("missing");
    sjit_looks_switch_backdrop(runtime, unknown);
    sjit_value_destroy(unknown);
    require(sjit_runtime_script_invocation_count(runtime, 719) == 2 &&
            sjit_runtime_thread_count(runtime) == 1,
        "backdrop hats restart an existing matching thread");

    SSprite *clone = sjit_sprite_clone(stage, 99, 99);
    require(clone && clone->costume_count == 4 &&
            sjit_sprite_costume_index_by_name(clone, "2") == 3,
        "sprite clones own a valid copy of costume-name metadata");
    sjit_sprite_destroy(clone);
    sjit_runtime_destroy(runtime);

    SRuntime *self_restart_runtime = sjit_runtime_create();
    SSprite *self_restart_stage = sjit_runtime_create_sprite(
        self_restart_runtime,
        "Stage",
        1);
    require(self_restart_runtime && self_restart_stage,
        "create self-restarting backdrop runtime");
    const char *self_restart_names[] = {"a", "b"};
    require(sjit_sprite_set_costume_names(
                self_restart_stage,
                self_restart_names,
                2),
        "store self-restarting backdrop names");
    require(sjit_runtime_register_script(
                self_restart_runtime,
                self_restart_stage->base.id,
                720,
                SJIT_HAT_EVENT_WHENBACKDROPSWITCHESTO,
                "B",
                1,
                0,
                selfRestartingBackdropReceiver),
        "register self-restarting backdrop hat");
    g_self_restart_backdrop_count = 0;
    SValue initial_backdrop = sjit_make_string("b");
    sjit_looks_switch_backdrop(self_restart_runtime, initial_backdrop);
    sjit_value_destroy(initial_backdrop);
    sjit_runtime_set_turbo_mode(self_restart_runtime, 1);
    sjit_runtime_tick(self_restart_runtime);
    require(g_self_restart_backdrop_count == 1 &&
            sjit_runtime_script_invocation_count(self_restart_runtime, 720) == 2 &&
            sjit_runtime_thread_count(self_restart_runtime) == 1 &&
            self_restart_runtime->threads[0]->frame.pc == 0 &&
            !self_restart_runtime->threads[0]->restart_pending,
        "an active backdrop hat defers its self-restart until the entry unwinds");
    sjit_runtime_tick(self_restart_runtime);
    require(g_self_restart_backdrop_count == 2 &&
            sjit_runtime_thread_count(self_restart_runtime) == 0,
        "a deferred backdrop self-restart executes from a fresh frame");
    sjit_runtime_destroy(self_restart_runtime);

    SRuntime *empty_name_runtime = sjit_runtime_create();
    SSprite *empty_name_stage = sjit_runtime_create_sprite(
        empty_name_runtime,
        "Stage",
        1);
    require(empty_name_runtime && empty_name_stage,
        "create empty-name backdrop runtime");
    const char *empty_name_costumes[] = {"", "named"};
    require(sjit_sprite_set_costume_names(
                empty_name_stage,
                empty_name_costumes,
                2),
        "store an empty backdrop name");
    require(sjit_runtime_register_script(
                empty_name_runtime,
                empty_name_stage->base.id,
                721,
                SJIT_HAT_EVENT_WHENBACKDROPSWITCHESTO,
                "",
                1,
                0,
                broadcastReceiver) &&
            sjit_runtime_register_script(
                empty_name_runtime,
                empty_name_stage->base.id,
                722,
                SJIT_HAT_EVENT_WHENBACKDROPSWITCHESTO,
                "named",
                1,
                0,
                broadcastReceiver),
        "register exact empty and named backdrop hats");
    SValue empty_backdrop = sjit_make_string("");
    sjit_looks_switch_backdrop(empty_name_runtime, empty_backdrop);
    sjit_value_destroy(empty_backdrop);
    require(sjit_runtime_script_invocation_count(empty_name_runtime, 721) == 1 &&
            sjit_runtime_script_invocation_count(empty_name_runtime, 722) == 0,
        "an empty backdrop name matches only the exact empty-name hat");
    sjit_runtime_destroy(empty_name_runtime);

    SRuntime *procedure_runtime = sjit_runtime_create();
    SSprite *procedure_stage = sjit_runtime_create_sprite(
        procedure_runtime,
        "Stage",
        1);
    require(procedure_runtime && procedure_stage,
        "create backdrop procedure-argument runtime");
    const char *procedure_backdrops[] = {"a", "b"};
    require(sjit_sprite_set_costume_names(
                procedure_stage,
                procedure_backdrops,
                2),
        "store backdrop procedure-argument names");
    SCompiledScript *procedure_script = sjit_compiled_script_create(
        procedure_stage->base.id,
        1);
    require(procedure_script, "create backdrop procedure-argument script");
    setOneArgumentProcedureCall(
        procedure_script->statements[0],
        "switch backdrop %s",
        "backdrop",
        literalString("b"));
    procedure_script->procedure_count = 1;
    procedure_script->procedures = static_cast<SCompiledProcedure *>(
        std::calloc(1, sizeof(SCompiledProcedure)));
    require(procedure_script->procedures,
        "allocate backdrop procedure metadata");
    SCompiledProcedure &backdrop_procedure = procedure_script->procedures[0];
    backdrop_procedure.name = sjit_string_new("switch backdrop %s");
    backdrop_procedure.warp_mode = 1;
    backdrop_procedure.argument_count = 1;
    backdrop_procedure.argument_names = static_cast<SString **>(
        std::calloc(1, sizeof(SString *)));
    require(backdrop_procedure.argument_names,
        "allocate backdrop procedure argument name");
    backdrop_procedure.argument_names[0] = sjit_string_new("backdrop");
    backdrop_procedure.statement_count = 1;
    backdrop_procedure.statements = static_cast<SStatement *>(
        std::calloc(1, sizeof(SStatement)));
    require(backdrop_procedure.statements,
        "allocate backdrop procedure body");
    backdrop_procedure.statements[0].opcode = SJIT_STMT_LOOKS_SWITCH_BACKDROP;
    backdrop_procedure.statements[0].value = sjit_expr_create_argument("backdrop");
    {
        sjit::JitEngine jit;
        SScriptEntryFn entry = jit.compileScript(
            *procedure_script,
            "sjit_backdrop_procedure_argument",
            procedure_runtime);
        require(entry && entry != sjit_script_interpreter_entry &&
                backdrop_procedure.jit_entry,
            "JIT natively compiles a backdrop switch from a custom-block argument");
        require(sjit_runtime_register_script_with_data(
                    procedure_runtime,
                    procedure_stage->base.id,
                    723,
                    SJIT_HAT_EVENT_WHENFLAGCLICKED,
                    "",
                    1,
                    0,
                    entry,
                    procedure_script),
            "register native backdrop procedure-argument script");
        sjit_runtime_set_turbo_mode(procedure_runtime, 1);
        sjit_runtime_green_flag(procedure_runtime);
        sjit_runtime_tick(procedure_runtime);
        require(procedure_stage->current_costume == 1,
            "native custom-block arguments preserve the requested backdrop value");
        sjit_runtime_destroy(procedure_runtime);
    }
    sjit_compiled_script_destroy(procedure_script);

    const std::string project_json = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "currentCostume": 0,
            "costumes": [
                {"name": "Empty", "assetId": "", "dataFormat": "svg"},
                {"name": "Generating Skybox", "assetId": "", "dataFormat": "svg"},
                {"name": "Gerstner6", "assetId": "", "dataFormat": "png"}
            ],
            "variables": {"seenId": ["seen", 0]},
            "lists": {},
            "blocks": {
                "hat": {"opcode": "event_whenflagclicked", "next": "switch", "topLevel": true,
                    "inputs": {}, "fields": {}},
                "switch": {"opcode": "looks_switchbackdropto", "next": null, "topLevel": false,
                    "inputs": {"BACKDROP": [1, "menu"]}, "fields": {}},
                "menu": {"opcode": "looks_backdrops", "next": null, "topLevel": false,
                    "shadow": true, "inputs": {}, "fields": {"BACKDROP": ["Generating Skybox", null]}},
                "backdropHat": {"opcode": "event_whenbackdropswitchesto", "next": "seen", "topLevel": true,
                    "inputs": {}, "fields": {"BACKDROP": ["generating skybox", null]}},
                "seen": {"opcode": "data_setvariableto", "next": null, "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "1"]]}, "fields": {"VARIABLE": ["seen", "seenId"]}}
            }
        }]
    })json";
    SRuntime *loaded_runtime = sjit_runtime_create();
    sjit::ProjectLoadResult loaded = sjit::loadProjectIntoRuntime(
        loaded_runtime,
        writeSb3Fixture(project_json));
    require(loaded.ok && loaded.program.scripts.size() == 2,
        "loader accepts backdrop statements and menu shadows");
    SSprite *loaded_stage = sjit_runtime_get_sprite(loaded_runtime, 1);
    require(loaded_stage && loaded_stage->costume_count == 3 &&
            sjit_sprite_costume_index_by_name(
                loaded_stage,
                "Generating Skybox") == 1,
        "loader transfers costume names into runtime ownership");
    SCompiledScript *switch_script = nullptr;
    for (SCompiledScript *script : loaded.program.scripts) {
        if (script && script->statement_count == 1 &&
            script->statements[0].opcode == SJIT_STMT_LOOKS_SWITCH_BACKDROP) {
            switch_script = script;
        }
    }
    require(switch_script,
        "loader preserves switch-backdrop as an explicit statement");
    {
        sjit::JitEngine jit;
        require(jit.compileScript(
                    *switch_script,
                    "sjit_backdrop_native",
                    loaded_runtime) != sjit_script_interpreter_entry,
            "JIT lowers backdrop switching through the shared runtime bridge");
        require(jit.lastFallbackReason().empty(),
            "native backdrop lowering does not report an interpreter fallback");
    }
    sjit_runtime_green_flag(loaded_runtime);
    sjit_runtime_tick(loaded_runtime);
    require(loaded_stage->current_costume == 1,
        "interpreter switches the stage backdrop by menu name");
    sjit_runtime_tick(loaded_runtime);
    SVariable *seen = sjit_runtime_lookup_or_create_variable(
        loaded_runtime,
        loaded_stage->base.id,
        "seen",
        SJIT_VAR_SCALAR);
    require(seen && sjit_to_number(loaded_runtime, seen->value) == 1.0,
        "loader registers case-insensitive backdrop hats with restart semantics");
    loaded.program.jit.reset();
    sjit_runtime_destroy(loaded_runtime);

    const std::string invalid_current_costume_project = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "currentCostume": 1e999,
            "costumes": [
                {"name": "first", "assetId": "", "dataFormat": "svg"},
                {"name": "second", "assetId": "", "dataFormat": "svg"},
                {"name": "third", "assetId": "", "dataFormat": "svg"}
            ],
            "variables": {}, "lists": {}, "blocks": {}
        }, {
            "isStage": false,
            "name": "Sprite",
            "currentCostume": 1e100,
            "costumes": [
                {"name": "first", "assetId": "", "dataFormat": "svg"},
                {"name": "second", "assetId": "", "dataFormat": "svg"},
                {"name": "third", "assetId": "", "dataFormat": "svg"}
            ],
            "variables": {}, "lists": {}, "blocks": {}
        }]
    })json";
    SRuntime *invalid_costume_runtime = sjit_runtime_create();
    sjit::ProjectLoadResult invalid_costume_loaded =
        sjit::loadProjectIntoRuntime(
            invalid_costume_runtime,
            writeSb3Fixture(invalid_current_costume_project));
    SSprite *invalid_costume_stage = sjit_runtime_get_sprite(
        invalid_costume_runtime,
        1);
    SSprite *invalid_costume_sprite = sjit_runtime_get_sprite(
        invalid_costume_runtime,
        2);
    require(invalid_costume_loaded.ok && invalid_costume_stage &&
            invalid_costume_sprite &&
            invalid_costume_stage->current_costume == 0 &&
            invalid_costume_sprite->current_costume == 0,
        "loader safely defaults nonfinite and out-of-range currentCostume values");
    invalid_costume_loaded.program.jit.reset();
    sjit_runtime_destroy(invalid_costume_runtime);
}

void test_graphic_effect_helpers_clamp_clear_and_clone() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "GraphicEffectSprite", 0);
    require(runtime && sprite, "create graphic-effect helper runtime");

    sjit_looks_set_effect_number(
        runtime,
        sprite,
        SJIT_GRAPHIC_EFFECT_GHOST,
        140.0);
    require(sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_GHOST] == 100.0,
        "ghost set clamps to one hundred");
    sjit_looks_change_effect_number(
        runtime,
        sprite,
        SJIT_GRAPHIC_EFFECT_GHOST,
        -175.0);
    require(sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_GHOST] == 0.0,
        "ghost change clamps to zero");

    sjit_looks_set_effect_number(
        runtime,
        sprite,
        SJIT_GRAPHIC_EFFECT_BRIGHTNESS,
        -150.0);
    require(sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_BRIGHTNESS] == -100.0,
        "brightness set clamps to minus one hundred");
    sjit_looks_change_effect_number(
        runtime,
        sprite,
        SJIT_GRAPHIC_EFFECT_BRIGHTNESS,
        250.0);
    require(sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_BRIGHTNESS] == 100.0,
        "brightness change clamps to one hundred");

    sjit_looks_set_effect_number(
        runtime,
        sprite,
        SJIT_GRAPHIC_EFFECT_PIXELATE,
        -37.5);
    sjit_looks_change_effect_number(
        runtime,
        sprite,
        SJIT_GRAPHIC_EFFECT_PIXELATE,
        7.25);
    require(sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_PIXELATE] == -30.25,
        "pixelate preserves raw signed fractional values");
    sjit_sprite_set_draggable(sprite, 1);

    SSprite *clone = sjit_sprite_clone(sprite, 99, 99);
    require(clone && clone->draggable == 1 &&
            clone->graphic_effects[SJIT_GRAPHIC_EFFECT_GHOST] == 0.0 &&
            clone->graphic_effects[SJIT_GRAPHIC_EFFECT_BRIGHTNESS] == 100.0 &&
            clone->graphic_effects[SJIT_GRAPHIC_EFFECT_PIXELATE] == -30.25,
        "clones copy drag mode and every observable graphic-effect value");

    sjit_looks_clear_effects(runtime, sprite);
    for (int effect = 0; effect < SJIT_GRAPHIC_EFFECT_COUNT; ++effect) {
        require(sprite->graphic_effects[effect] == 0.0,
            "clear graphic effects resets each effect slot");
    }
    require(clone->graphic_effects[SJIT_GRAPHIC_EFFECT_BRIGHTNESS] == 100.0 &&
            clone->graphic_effects[SJIT_GRAPHIC_EFFECT_PIXELATE] == -30.25,
        "cloned graphic effects are independent after the copy");

    sjit_sprite_destroy(clone);
    sjit_runtime_destroy(runtime);
}

void test_costume_switch_and_layer_operations() {
    const OpcodeEffects switch_effects = sjit_statement_opcode_effects(
        SJIT_STMT_LOOKS_SWITCH_COSTUME);
    const OpcodeEffects layer_effects = sjit_statement_opcode_effects(
        SJIT_STMT_LOOKS_GO_TO_FRONT_BACK);
    require(switch_effects.requiresInterpreter && layer_effects.requiresInterpreter,
        "costume and layer Looks operations use the checked interpreter path");

    SRuntime *runtime = sjit_runtime_create();
    SSprite *stage = sjit_runtime_create_sprite(runtime, "Stage", 1);
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "Sprite", 0);
    SSprite *other = sjit_runtime_create_sprite(runtime, "Other", 0);
    require(runtime && stage && sprite && other,
        "create costume/layer operation runtime");
    const char *names[] = {"first", "second", "third"};
    require(sjit_sprite_set_costume_names(sprite, names, 3),
        "store sprite costume names");

    SValue named = sjit_make_string("third");
    sjit_looks_switch_costume(runtime, sprite, named);
    sjit_value_destroy(named);
    require(sprite->current_costume == 2,
        "switch costume accepts a costume name");
    sjit_looks_switch_costume(runtime, sprite, sjit_make_number(1.0));
    require(sprite->current_costume == 0,
        "numeric costume selection is one-based");

    sjit_looks_go_to_front_back(runtime, sprite, 1);
    require(sprite->layer_order > other->layer_order,
        "go to front moves a sprite above every other target");
    sjit_looks_go_to_front_back(runtime, sprite, 0);
    require(sprite->layer_order < other->layer_order &&
            sprite->layer_order < stage->layer_order,
        "go to back moves a sprite below every other target");

    sjit_runtime_tick(runtime);
    const SDrawCommandBuffer *draw = sjit_runtime_get_draw_commands(runtime);
    require(draw && draw->length == 2 &&
            draw->items[0].target_id == sprite->base.id &&
            draw->items[1].target_id == other->base.id,
        "draw commands follow the updated Scratch layer order");
    sjit_runtime_destroy(runtime);
}

void test_graphic_effect_interpreter_statements() {
    const OpcodeEffects set_effects = sjit_statement_opcode_effects(
        SJIT_STMT_LOOKS_SET_EFFECT);
    const OpcodeEffects change_effects = sjit_statement_opcode_effects(
        SJIT_STMT_LOOKS_CHANGE_EFFECT);
    const OpcodeEffects clear_effects = sjit_statement_opcode_effects(
        SJIT_STMT_LOOKS_CLEAR_EFFECTS);
    const OpcodeEffects drag_effects = sjit_statement_opcode_effects(
        SJIT_STMT_SENSING_SET_DRAG_MODE);
    require(!set_effects.requiresInterpreter && set_effects.canMutateTarget &&
            !change_effects.requiresInterpreter && change_effects.canMutateTarget &&
            !clear_effects.requiresInterpreter && clear_effects.canMutateTarget &&
            !drag_effects.requiresInterpreter && drag_effects.canMutateTarget,
        "graphic effects and drag mode expose checked native effect metadata");

    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "GraphicEffectInterpreter", 0);
    require(runtime && sprite, "create graphic-effect interpreter runtime");
    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 9);
    require(script, "create graphic-effect interpreter script");

    script->statements[0].opcode = SJIT_STMT_LOOKS_SET_EFFECT;
    script->statements[0].looks_effect_cache_valid = 1;
    script->statements[0].looks_effect = SJIT_GRAPHIC_EFFECT_GHOST;
    script->statements[0].value = literalNumber(130.0);
    script->statements[1].opcode = SJIT_STMT_LOOKS_CHANGE_EFFECT;
    script->statements[1].looks_effect_cache_valid = 1;
    script->statements[1].looks_effect = SJIT_GRAPHIC_EFFECT_GHOST;
    script->statements[1].value = literalNumber(-35.0);
    script->statements[2].opcode = SJIT_STMT_LOOKS_SET_EFFECT;
    script->statements[2].looks_effect_cache_valid = 1;
    script->statements[2].looks_effect = SJIT_GRAPHIC_EFFECT_BRIGHTNESS;
    script->statements[2].value = literalNumber(-150.0);
    script->statements[3].opcode = SJIT_STMT_LOOKS_CHANGE_EFFECT;
    script->statements[3].looks_effect_cache_valid = 1;
    script->statements[3].looks_effect = SJIT_GRAPHIC_EFFECT_BRIGHTNESS;
    script->statements[3].value = literalNumber(260.0);
    script->statements[4].opcode = SJIT_STMT_LOOKS_SET_EFFECT;
    script->statements[4].looks_effect_cache_valid = 1;
    script->statements[4].looks_effect = SJIT_GRAPHIC_EFFECT_PIXELATE;
    script->statements[4].value = literalNumber(-37.5);
    script->statements[5].opcode = SJIT_STMT_LOOKS_CHANGE_EFFECT;
    script->statements[5].looks_effect_cache_valid = 1;
    script->statements[5].looks_effect = SJIT_GRAPHIC_EFFECT_PIXELATE;
    script->statements[5].value = literalNumber(7.5);
    script->statements[6].opcode = SJIT_STMT_SENSING_SET_DRAG_MODE;
    script->statements[6].drag_mode = 1;
    script->statements[7].opcode = SJIT_STMT_LOOKS_CLEAR_EFFECTS;
    script->statements[8].opcode = SJIT_STMT_SENSING_SET_DRAG_MODE;
    script->statements[8].drag_mode = 0;

    for (int index = 0; index <= 6; ++index) {
        require(sjit_script_execute_statement(runtime, script, index) == SJIT_STATUS_OK,
            "interpreter executes graphic-effect and drag statements");
    }
    require(sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_GHOST] == 65.0,
        "interpreter set/change applies the clamped ghost result");
    require(sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_BRIGHTNESS] == 100.0,
        "interpreter set/change applies the clamped brightness result");
    require(sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_PIXELATE] == -30.0,
        "interpreter set/change keeps raw pixelate arithmetic");
    require(sprite->draggable == 1,
        "interpreter enables sprite dragging");

    require(sjit_script_execute_statement(runtime, script, 7) == SJIT_STATUS_OK,
        "interpreter executes clear graphic effects");
    for (int effect = 0; effect < SJIT_GRAPHIC_EFFECT_COUNT; ++effect) {
        require(sprite->graphic_effects[effect] == 0.0,
            "interpreter clear resets all graphic effects");
    }
    require(sprite->draggable == 1,
        "clearing graphic effects does not change drag mode");
    require(sjit_script_execute_statement(runtime, script, 8) == SJIT_STATUS_OK &&
            sprite->draggable == 0,
        "interpreter disables sprite dragging");

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_graphic_effect_top_level_native_jit() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "GraphicEffectJit", 0);
    require(runtime && sprite, "create top-level graphic-effect JIT runtime");
    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 6);
    require(script, "create top-level graphic-effect JIT script");

    script->statements[0].opcode = SJIT_STMT_LOOKS_SET_EFFECT;
    script->statements[0].looks_effect_cache_valid = 1;
    script->statements[0].looks_effect = SJIT_GRAPHIC_EFFECT_GHOST;
    script->statements[0].value = literalNumber(75.0);
    script->statements[1].opcode = SJIT_STMT_LOOKS_CHANGE_EFFECT;
    script->statements[1].looks_effect_cache_valid = 1;
    script->statements[1].looks_effect = SJIT_GRAPHIC_EFFECT_GHOST;
    script->statements[1].value = literalNumber(40.0);
    script->statements[2].opcode = SJIT_STMT_LOOKS_SET_EFFECT;
    script->statements[2].looks_effect_cache_valid = 1;
    script->statements[2].looks_effect = SJIT_GRAPHIC_EFFECT_BRIGHTNESS;
    script->statements[2].value = literalNumber(-75.0);
    script->statements[3].opcode = SJIT_STMT_LOOKS_CHANGE_EFFECT;
    script->statements[3].looks_effect_cache_valid = 1;
    script->statements[3].looks_effect = SJIT_GRAPHIC_EFFECT_BRIGHTNESS;
    script->statements[3].value = literalNumber(-40.0);
    script->statements[4].opcode = SJIT_STMT_LOOKS_SET_EFFECT;
    script->statements[4].looks_effect_cache_valid = 1;
    script->statements[4].looks_effect = SJIT_GRAPHIC_EFFECT_PIXELATE;
    script->statements[4].value = literalNumber(-37.5);
    script->statements[5].opcode = SJIT_STMT_SENSING_SET_DRAG_MODE;
    script->statements[5].drag_mode = 1;

    auto jit = std::make_unique<sjit::JitEngine>();
    const std::string ir_path = "/tmp/xyo-jit-graphic-effects.ll";
    jit->emitScriptLl(*script, "sjit_unit_graphic_effects_ir", ir_path);
    std::ifstream ir_file(ir_path);
    const std::string ir(
        (std::istreambuf_iterator<char>(ir_file)),
        std::istreambuf_iterator<char>());
    require(ir.find("call void @sjit_looks_set_effect_number") != std::string::npos &&
            ir.find("call void @sjit_looks_change_effect_number") != std::string::npos &&
            ir.find("call void @sjit_sprite_set_draggable") != std::string::npos,
        "top-level JIT directly lowers graphic effects and drag mode");

    SScriptEntryFn entry = jit->compileScript(
        *script,
        "sjit_unit_graphic_effects_entry",
        runtime);
    require(entry && entry != sjit_script_interpreter_entry &&
            jit->lastFallbackReason().empty(),
        "top-level graphic-effect script compiles natively");
    require(sjit_runtime_register_script_with_data(
                runtime,
                sprite->base.id,
                724,
                SJIT_HAT_EVENT_WHENFLAGCLICKED,
                "",
                1,
                0,
                entry,
                script),
        "register top-level graphic-effect JIT script");
    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);
    require(sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_GHOST] == 100.0 &&
            sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_BRIGHTNESS] == -100.0 &&
            sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_PIXELATE] == -37.5 &&
            sprite->draggable == 1,
        "top-level native JIT preserves effect clamps, raw pixelate, and drag mode");

    SCompiledScript *clear_script = sjit_compiled_script_create(sprite->base.id, 1);
    require(clear_script, "create native clear graphic-effects script");
    clear_script->statements[0].opcode = SJIT_STMT_LOOKS_CLEAR_EFFECTS;
    SScriptEntryFn clear_entry = jit->compileScript(
        *clear_script,
        "sjit_unit_clear_graphic_effects_entry",
        runtime);
    require(clear_entry && clear_entry != sjit_script_interpreter_entry,
        "clear graphic effects compiles natively");
    require(sjit_runtime_register_script_with_data(
                runtime,
                sprite->base.id,
                725,
                SJIT_HAT_EVENT_WHENKEYPRESSED,
                "space",
                1,
                0,
                clear_entry,
                clear_script),
        "register native clear graphic-effects script");
    sjit_runtime_start_hats(
        runtime,
        SJIT_HAT_EVENT_WHENKEYPRESSED,
        "space");
    sjit_runtime_tick(runtime);
    for (int effect = 0; effect < SJIT_GRAPHIC_EFFECT_COUNT; ++effect) {
        require(sprite->graphic_effects[effect] == 0.0,
            "native clear resets every graphic-effect slot");
    }
    require(sprite->draggable == 1,
        "native clear leaves drag mode unchanged");

    jit.reset();
    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(clear_script);
    sjit_compiled_script_destroy(script);
}

void test_graphic_effect_custom_procedure_argument_native_jit() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "GraphicEffectProc", 0);
    require(runtime && sprite, "create procedure graphic-effect JIT runtime");
    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 1);
    require(script, "create procedure graphic-effect JIT script");
    setOneArgumentProcedureCall(
        script->statements[0],
        "effects %s",
        "amount",
        literalNumber(37.5));

    script->procedure_count = 1;
    script->procedures = static_cast<SCompiledProcedure *>(
        std::calloc(1, sizeof(SCompiledProcedure)));
    require(script->procedures, "allocate graphic-effect JIT procedure");
    SCompiledProcedure &procedure = script->procedures[0];
    procedure.name = sjit_string_new("effects %s");
    procedure.warp_mode = 1;
    procedure.argument_count = 1;
    procedure.argument_names = static_cast<SString **>(
        std::calloc(1, sizeof(SString *)));
    require(procedure.argument_names,
        "allocate graphic-effect JIT procedure arguments");
    procedure.argument_names[0] = sjit_string_new("amount");
    procedure.statement_count = 5;
    procedure.statements = static_cast<SStatement *>(
        std::calloc(5, sizeof(SStatement)));
    require(procedure.statements, "allocate graphic-effect JIT procedure body");

    procedure.statements[0].opcode = SJIT_STMT_LOOKS_SET_EFFECT;
    procedure.statements[0].looks_effect_cache_valid = 1;
    procedure.statements[0].looks_effect = SJIT_GRAPHIC_EFFECT_PIXELATE;
    procedure.statements[0].value = sjit_expr_create_argument("amount");
    procedure.statements[1].opcode = SJIT_STMT_LOOKS_SET_EFFECT;
    procedure.statements[1].looks_effect_cache_valid = 1;
    procedure.statements[1].looks_effect = SJIT_GRAPHIC_EFFECT_GHOST;
    procedure.statements[1].value = literalNumber(90.0);
    procedure.statements[2].opcode = SJIT_STMT_LOOKS_CHANGE_EFFECT;
    procedure.statements[2].looks_effect_cache_valid = 1;
    procedure.statements[2].looks_effect = SJIT_GRAPHIC_EFFECT_GHOST;
    procedure.statements[2].value = sjit_expr_create_argument("amount");
    procedure.statements[3].opcode = SJIT_STMT_LOOKS_SET_EFFECT;
    procedure.statements[3].looks_effect_cache_valid = 1;
    procedure.statements[3].looks_effect = SJIT_GRAPHIC_EFFECT_BRIGHTNESS;
    procedure.statements[3].value = literalNumber(-80.0);
    procedure.statements[4].opcode = SJIT_STMT_LOOKS_CHANGE_EFFECT;
    procedure.statements[4].looks_effect_cache_valid = 1;
    procedure.statements[4].looks_effect = SJIT_GRAPHIC_EFFECT_BRIGHTNESS;
    procedure.statements[4].value = sjit_expr_create_argument("amount");

    auto jit = std::make_unique<sjit::JitEngine>();
    const std::string ir_path = "/tmp/xyo-jit-procedure-graphic-effects.ll";
    jit->emitScriptLl(
        *script,
        "sjit_unit_procedure_graphic_effects_ir",
        ir_path);
    std::ifstream ir_file(ir_path);
    const std::string ir(
        (std::istreambuf_iterator<char>(ir_file)),
        std::istreambuf_iterator<char>());
    require(ir.find("call void @sjit_looks_set_effect_number") != std::string::npos &&
            ir.find("call void @sjit_looks_change_effect_number") != std::string::npos &&
            ir.find("call i32 @sjit_script_execute_procedure_statement") ==
                std::string::npos,
        "procedure JIT lowers numeric graphic-effect arguments without fallback");

    SScriptEntryFn entry = jit->compileScript(
        *script,
        "sjit_unit_procedure_graphic_effects_entry",
        runtime);
    require(entry && entry != sjit_script_interpreter_entry && procedure.jit_entry,
        "graphic-effect custom procedure compiles to a native entry");
    require(sjit_runtime_register_script_with_data(
                runtime,
                sprite->base.id,
                726,
                SJIT_HAT_EVENT_WHENFLAGCLICKED,
                "",
                1,
                0,
                entry,
                script),
        "register graphic-effect custom procedure JIT script");
    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);
    require(sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_PIXELATE] == 37.5 &&
            sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_GHOST] == 100.0 &&
            sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_BRIGHTNESS] == -42.5,
        "native custom procedure consumes its numeric argument for each effect");

    jit.reset();
    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_project_loader_graphic_effects_and_drag_mode() {
    const std::string project_json = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "variables": {}, "lists": {}, "blocks": {}
        }, {
            "isStage": false,
            "name": "EffectSprite",
            "variables": {}, "lists": {},
            "blocks": {
                "hat": {"opcode": "event_whenflagclicked", "next": "clear", "topLevel": true,
                    "inputs": {}, "fields": {}},
                "clear": {"opcode": "looks_cleargraphiceffects", "next": "setGhost", "topLevel": false,
                    "inputs": {}, "fields": {}},
                "setGhost": {"opcode": "looks_seteffectto", "next": "changePixelate", "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "125"]]}, "fields": {"EFFECT": ["GHOST", null]}},
                "changePixelate": {"opcode": "looks_changeeffectby", "next": "dragOn", "topLevel": false,
                    "inputs": {"CHANGE": [1, [4, "-4"]]}, "fields": {"EFFECT": ["PIXELATE", null]}},
                "dragOn": {"opcode": "sensing_setdragmode", "next": "dragOff", "topLevel": false,
                    "inputs": {}, "fields": {"DRAG_MODE": ["draggable", null]}},
                "dragOff": {"opcode": "sensing_setdragmode", "next": null, "topLevel": false,
                    "inputs": {}, "fields": {"DRAG_MODE": ["not draggable", null]}}
            }
        }]
    })json";

    SRuntime *runtime = sjit_runtime_create();
    sjit::ProjectLoadResult loaded = sjit::loadProjectIntoRuntime(
        runtime,
        writeSb3Fixture(project_json));
    require(loaded.ok && loaded.program.scripts.size() == 1,
        "loader accepts graphic-effect and drag-mode Scratch blocks");
    SCompiledScript *script = loaded.program.scripts[0];
    require(script && script->statement_count == 5,
        "loader preserves every graphic-effect and drag-mode statement");
    require(script->statements[0].opcode == SJIT_STMT_LOOKS_CLEAR_EFFECTS,
        "loader parses clear graphic effects");
    require(script->statements[1].opcode == SJIT_STMT_LOOKS_SET_EFFECT &&
            script->statements[1].looks_effect_cache_valid &&
            script->statements[1].looks_effect == SJIT_GRAPHIC_EFFECT_GHOST,
        "loader parses set ghost effect");
    require(script->statements[2].opcode == SJIT_STMT_LOOKS_CHANGE_EFFECT &&
            script->statements[2].looks_effect_cache_valid &&
            script->statements[2].looks_effect == SJIT_GRAPHIC_EFFECT_PIXELATE,
        "loader parses change pixelate effect");
    require(script->statements[3].opcode == SJIT_STMT_SENSING_SET_DRAG_MODE &&
            script->statements[3].drag_mode == 1 &&
            script->statements[4].opcode == SJIT_STMT_SENSING_SET_DRAG_MODE &&
            script->statements[4].drag_mode == 0,
        "loader parses both draggable and not-draggable modes");

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);
    SSprite *sprite = sjit_runtime_get_sprite(runtime, script->target_id);
    require(sprite &&
            sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_GHOST] == 100.0 &&
            sprite->graphic_effects[SJIT_GRAPHIC_EFFECT_PIXELATE] == -4.0 &&
            sprite->draggable == 0,
        "loaded native script executes clamps, raw pixelate, and drag transitions");

    loaded.program.jit.reset();
    sjit_runtime_destroy(runtime);
}

SCompiledScript *makeCoreDifferentialScript(int target_id) {
    SCompiledScript *script = sjit_compiled_script_create(target_id, 10);
    require(script, "create core differential script");

    script->statements[0].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[0].variable_name = sjit_string_new("total");
    script->statements[0].value = literalNumber(0.0);

    script->statements[1].opcode = SJIT_STMT_REPEAT;
    script->statements[1].times = literalNumber(3.0);
    script->statements[1].substack_count = 1;
    script->statements[1].substack = static_cast<SStatement *>(
        std::calloc(1, sizeof(SStatement)));
    require(script->statements[1].substack, "allocate non-yield repeat body");
    script->statements[1].substack[0].opcode = SJIT_STMT_CHANGE_VARIABLE;
    script->statements[1].substack[0].variable_name = sjit_string_new("total");
    script->statements[1].substack[0].value = literalNumber(1.0);

    script->statements[2].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[2].variable_name = sjit_string_new("dynamic_input");
    script->statements[2].value = literalNumber(4.0);
    setOneArgumentProcedureCall(
        script->statements[3], "inner %s", "n", sjit_expr_create_variable("dynamic_input"));

    script->statements[4].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[4].variable_name = sjit_string_new("dynamic_input");
    script->statements[4].value = literalString("8");
    setOneArgumentProcedureCall(
        script->statements[5], "inner %s", "n", sjit_expr_create_variable("dynamic_input"));
    setOneArgumentProcedureCall(
        script->statements[6], "outer %s", "n", literalNumber(2.0));

    script->statements[7].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[7].variable_name = sjit_string_new("polymorphic");
    script->statements[7].value = literalNumber(9.0);
    script->statements[8].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[8].variable_name = sjit_string_new("polymorphic");
    script->statements[8].value = literalString("text");

    script->statements[9].opcode = SJIT_STMT_REPEAT;
    script->statements[9].times = literalNumber(2.0);
    script->statements[9].substack_count = 2;
    script->statements[9].substack = static_cast<SStatement *>(
        std::calloc(2, sizeof(SStatement)));
    require(script->statements[9].substack, "allocate yielding repeat body");
    script->statements[9].substack[0].opcode = SJIT_STMT_CHANGE_VARIABLE;
    script->statements[9].substack[0].variable_name = sjit_string_new("yield_count");
    script->statements[9].substack[0].value = literalNumber(1.0);
    script->statements[9].substack[1].opcode = SJIT_STMT_WAIT;
    script->statements[9].substack[1].value = literalNumber(0.0);

    script->procedure_count = 2;
    script->procedures = static_cast<SCompiledProcedure *>(
        std::calloc(2, sizeof(SCompiledProcedure)));
    require(script->procedures, "allocate nested differential procedures");

    SCompiledProcedure &outer = script->procedures[0];
    outer.name = sjit_string_new("outer %s");
    outer.warp_mode = 1;
    outer.argument_count = 1;
    outer.argument_names = static_cast<SString **>(std::calloc(1, sizeof(SString *)));
    require(outer.argument_names, "allocate outer argument names");
    outer.argument_names[0] = sjit_string_new("n");
    outer.statement_count = 1;
    outer.statements = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(outer.statements, "allocate outer body");
    setOneArgumentProcedureCall(
        outer.statements[0], "inner %s", "n", sjit_expr_create_argument("n"));

    SCompiledProcedure &inner = script->procedures[1];
    inner.name = sjit_string_new("inner %s");
    inner.warp_mode = 1;
    inner.argument_count = 1;
    inner.argument_names = static_cast<SString **>(std::calloc(1, sizeof(SString *)));
    require(inner.argument_names, "allocate inner argument names");
    inner.argument_names[0] = sjit_string_new("n");
    inner.statement_count = 1;
    inner.statements = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(inner.statements, "allocate inner body");
    inner.statements[0].opcode = SJIT_STMT_CHANGE_VARIABLE;
    inner.statements[0].variable_name = sjit_string_new("total");
    inner.statements[0].value = sjit_expr_create_argument("n");
    return script;
}

struct CoreDifferentialResults {
    double total = 0.0;
    double yield_count = 0.0;
    double original_total = 0.0;
    int total_tag = SJIT_VALUE_NULL;
    int yield_count_tag = SJIT_VALUE_NULL;
    int dynamic_input_tag = SJIT_VALUE_NULL;
    int polymorphic_tag = SJIT_VALUE_NULL;
    std::string dynamic_input;
    std::string polymorphic;
    uint64_t invocation_count = 0;
    bool native_entry = false;
};

CoreDifferentialResults runCoreDifferential(bool jit_enabled, bool run_on_clone) {
    CoreDifferentialResults result;
    SRuntime *runtime = sjit_runtime_create();
    SSprite *original = sjit_runtime_create_sprite(runtime, "DifferentialSprite", 0);
    require(runtime && original, "create core differential runtime");
    SSprite *execution_target = run_on_clone ? sjit_clone_create(runtime, original) : original;
    require(execution_target, "create core differential execution target");
    SCompiledScript *script = makeCoreDifferentialScript(original->base.id);

    std::unique_ptr<sjit::JitEngine> jit;
    SScriptEntryFn entry = sjit_script_interpreter_entry;
    if (jit_enabled) {
        jit = std::make_unique<sjit::JitEngine>();
        entry = jit->compileScript(*script, "sjit_core_differential", runtime);
        result.native_entry = entry != sjit_script_interpreter_entry;
    }
    const int script_id = run_on_clone ? 812 : 811;
    require(sjit_runtime_register_script_with_data(
        runtime,
        execution_target->base.id,
        script_id,
        SJIT_HAT_EVENT_WHENFLAGCLICKED,
        "",
        1,
        0,
        entry,
        script),
        "register core differential script");
    sjit_runtime_green_flag(runtime);
    for (int tick = 0; tick < 16 && sjit_runtime_has_threads(runtime); ++tick) {
        sjit_runtime_set_time(runtime, static_cast<double>(tick), 1.0);
        sjit_runtime_tick(runtime);
    }
    require(!sjit_runtime_has_threads(runtime), "core differential script completes");

    SVariable *total = sjit_runtime_lookup_or_create_variable(
        runtime, execution_target->base.id, "total", SJIT_VAR_SCALAR);
    SVariable *yield_count = sjit_runtime_lookup_or_create_variable(
        runtime, execution_target->base.id, "yield_count", SJIT_VAR_SCALAR);
    SVariable *dynamic_input = sjit_runtime_lookup_or_create_variable(
        runtime, execution_target->base.id, "dynamic_input", SJIT_VAR_SCALAR);
    SVariable *polymorphic = sjit_runtime_lookup_or_create_variable(
        runtime, execution_target->base.id, "polymorphic", SJIT_VAR_SCALAR);
    SVariable *original_total = sjit_runtime_lookup_or_create_variable(
        runtime, original->base.id, "total", SJIT_VAR_SCALAR);
    result.total = sjit_to_number(runtime, total->value);
    result.yield_count = sjit_to_number(runtime, yield_count->value);
    result.original_total = sjit_to_number(runtime, original_total->value);
    result.total_tag = total->value.tag;
    result.yield_count_tag = yield_count->value.tag;
    result.dynamic_input_tag = dynamic_input->value.tag;
    result.polymorphic_tag = polymorphic->value.tag;
    if (dynamic_input->value.tag == SJIT_VALUE_STRING) {
        result.dynamic_input = sjit_string_cstr(
            static_cast<const SString *>(dynamic_input->value.ptr));
    }
    if (polymorphic->value.tag == SJIT_VALUE_STRING) {
        result.polymorphic = sjit_string_cstr(
            static_cast<const SString *>(polymorphic->value.ptr));
    }
    result.invocation_count = sjit_runtime_script_invocation_count(runtime, script_id);

    jit.reset();
    for (int i = 0; i < script->procedure_count; ++i) {
        require(script->procedures[i].jit_entry == nullptr,
            "destroying the JIT invalidates borrowed procedure entries");
    }
    sjit_compiled_script_destroy(script);
    sjit_runtime_destroy(runtime);
    return result;
}

void test_core_interpreter_jit_differential_and_hotness() {
    SCompiledScript *ir_script = makeCoreDifferentialScript(1);
    {
        sjit::JitEngine jit;
        const std::string ir_path = "/tmp/xyo-jit-numeric-abi-differential.ll";
        jit.emitScriptLl(*ir_script, "sjit_numeric_abi_differential_ir", ir_path);
        std::ifstream ir_file(ir_path);
        const std::string ir(
            (std::istreambuf_iterator<char>(ir_file)),
            std::istreambuf_iterator<char>());
        require(ir.find("procedure_numeric_args") != std::string::npos,
            "numeric-only procedure calls use the unboxed argument ABI");
        require(ir.find("procedure_numeric_guard_ok") != std::string::npos &&
                ir.find("procedure_numeric_guard_fallback") != std::string::npos,
            "dynamic numeric arguments emit success and generic fallback guards");
    }
    sjit_compiled_script_destroy(ir_script);

    for (bool clone_target : {false, true}) {
        const CoreDifferentialResults interpreted = runCoreDifferential(false, clone_target);
        const CoreDifferentialResults compiled = runCoreDifferential(true, clone_target);
        require(compiled.native_entry, "JIT-enabled differential run receives a native entry");
        require(interpreted.total == 17.0 && interpreted.yield_count == 2.0 &&
                interpreted.total_tag == SJIT_VALUE_NUMBER &&
                interpreted.yield_count_tag == SJIT_VALUE_NUMBER,
            "interpreter covers numeric procedures and both repeat kinds");
        require(interpreted.dynamic_input_tag == SJIT_VALUE_STRING &&
                interpreted.dynamic_input == "8" &&
                interpreted.polymorphic_tag == SJIT_VALUE_STRING &&
                interpreted.polymorphic == "text",
            "interpreter preserves numeric-to-string type transitions");
        require(compiled.total == interpreted.total &&
                compiled.yield_count == interpreted.yield_count &&
                compiled.total_tag == interpreted.total_tag &&
                compiled.yield_count_tag == interpreted.yield_count_tag &&
                compiled.dynamic_input_tag == interpreted.dynamic_input_tag &&
                compiled.dynamic_input == interpreted.dynamic_input &&
                compiled.polymorphic_tag == interpreted.polymorphic_tag &&
                compiled.polymorphic == interpreted.polymorphic,
            "numeric, nested procedure, type-guard, and repeat side effects match");
        if (clone_target) {
            require(interpreted.original_total == 0.0 && compiled.original_total == 0.0,
                "dynamic-target execution mutates the clone, not the original");
        }
        if (interpreted.invocation_count != 1 || compiled.invocation_count != 1) {
            throw std::runtime_error(
                "script hotness counts invocations rather than scheduler re-entry (interpreter=" +
                std::to_string(interpreted.invocation_count) + ", JIT=" +
                std::to_string(compiled.invocation_count) + ", clone=" +
                std::to_string(clone_target ? 1 : 0) + ")");
        }
    }
}

void test_procedure_entry_identity_guards() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *original = sjit_runtime_create_sprite(runtime, "ProcedureGuard", 0);
    require(runtime && original, "create procedure guard runtime");
    SSprite *clone = sjit_clone_create(runtime, original);
    require(clone, "create procedure guard clone");
    SCompiledScript *script = makeCoreDifferentialScript(original->base.id);
    auto jit = std::make_unique<sjit::JitEngine>();
    SScriptEntryFn entry = jit->compileScript(*script, "sjit_procedure_identity_guard", runtime);
    require(entry != sjit_script_interpreter_entry && script->procedures[1].jit_entry,
        "compile guarded procedure entry");

    SThread *thread = sjit_thread_create(900, clone->base.id, 901, entry, script);
    require(thread, "create direct procedure guard thread");
    double numeric_argument[1] = {5.0};
    SValue value_argument[1] = {sjit_make_number(5.0)};
    SRuntimeStatus status = script->procedures[1].jit_entry(
        runtime,
        thread,
        &thread->frame,
        script,
        1,
        numeric_argument,
        value_argument);
    require(status == SJIT_STATUS_ERROR,
        "exported procedure entry rejects a clone target before using original handles");

    thread->target_id = original->base.id;
    thread->script_data = nullptr;
    status = script->procedures[1].jit_entry(
        runtime,
        thread,
        &thread->frame,
        script,
        1,
        numeric_argument,
        value_argument);
    require(status == SJIT_STATUS_ERROR,
        "exported procedure entry rejects mismatched thread script data");
    status = script->procedures[1].jit_entry(
        runtime,
        nullptr,
        &thread->frame,
        script,
        1,
        numeric_argument,
        value_argument);
    require(status == SJIT_STATUS_ERROR,
        "exported procedure entry rejects null ABI pointers without dereferencing them");

    SVariable *total = sjit_runtime_lookup_or_create_variable(
        runtime, original->base.id, "total", SJIT_VAR_SCALAR);
    require(total && sjit_to_number(runtime, total->value) == 0.0,
        "failed direct procedure guards leave the original target unchanged");

    STarget &original_target = original->base;
    const int relocated_capacity = original_target.variable_capacity + 8;
    SVariable *relocated = static_cast<SVariable *>(
        std::calloc(static_cast<size_t>(relocated_capacity), sizeof(SVariable)));
    require(relocated, "allocate relocated procedure variable storage");
    for (int i = 0; i < original_target.variable_count; ++i) {
        relocated[i] = original_target.variables[i];
    }
    std::free(original_target.variables);
    original_target.variables = relocated;
    original_target.variable_capacity = relocated_capacity;
    thread->script_data = script;
    status = script->procedures[1].jit_entry(
        runtime,
        thread,
        &thread->frame,
        script,
        1,
        numeric_argument,
        value_argument);
    require(status == SJIT_STATUS_DONE || status == SJIT_STATUS_OK,
        "guarded procedure entry accepts the original target after storage relocation");
    total = sjit_runtime_lookup_or_create_variable(
        runtime, original->base.id, "total", SJIT_VAR_SCALAR);
    require(total && total->value.tag == SJIT_VALUE_NUMBER && total->value.number == 5.0,
        "procedure handle reloads a relocated variable-array base");
    sjit_thread_destroy(thread);
    jit.reset();
    sjit_compiled_script_destroy(script);
    sjit_runtime_destroy(runtime);
}

void test_checked_variable_handles_survive_runtime_replacement() {
    SCompiledScript *script = sjit_compiled_script_create(1, 1);
    require(script, "create checked-handle lifetime script");
    script->statements[0].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[0].variable_name = sjit_string_new("isolated");
    script->statements[0].value = literalNumber(42.0);

    SRuntime *first_runtime = sjit_runtime_create();
    SSprite *first = sjit_runtime_create_sprite(first_runtime, "First", 0);
    require(first, "create first lifetime target");
    auto jit = std::make_unique<sjit::JitEngine>();
    SScriptEntryFn entry = jit->compileScript(*script, "sjit_checked_handle_lifetime", first_runtime);
    require(entry != sjit_script_interpreter_entry, "compile checked-handle lifetime script");

    /* Model the realloc performed when a target's variable capacity grows.
       The compiled handle must reload this base at entry instead of retaining
       the SVariable pointer observed during prebinding. */
    STarget &first_target = first->base;
    const int relocated_capacity = first_target.variable_capacity + 8;
    SVariable *relocated = static_cast<SVariable *>(
        std::calloc(static_cast<size_t>(relocated_capacity), sizeof(SVariable)));
    require(relocated, "allocate relocated variable storage");
    for (int i = 0; i < first_target.variable_count; ++i) {
        relocated[i] = first_target.variables[i];
    }
    std::free(first_target.variables);
    first_target.variables = relocated;
    first_target.variable_capacity = relocated_capacity;

    require(sjit_runtime_register_script_with_data(
        first_runtime, 1, 814, SJIT_HAT_EVENT_WHENFLAGCLICKED, "", 1, 0, entry, script),
        "register relocated-storage script");
    sjit_runtime_green_flag(first_runtime);
    sjit_runtime_tick(first_runtime);
    SVariable *first_isolated = sjit_runtime_lookup_or_create_variable(
        first_runtime, 1, "isolated", SJIT_VAR_SCALAR);
    require(first_isolated && first_isolated->value.tag == SJIT_VALUE_NUMBER &&
            first_isolated->value.number == 42.0,
        "native entry reloads a relocated variable-array base");
    const uint64_t first_runtime_id = first_runtime->instance_id;
    sjit_runtime_destroy(first_runtime);

    SRuntime *second_runtime = sjit_runtime_create();
    require(second_runtime && second_runtime->instance_id != 0 &&
            second_runtime->instance_id != first_runtime_id,
        "runtime identity does not alias a destroyed runtime allocation");
    SSprite *second = sjit_runtime_create_sprite(second_runtime, "Second", 0);
    require(second && second->base.id == 1, "create replacement runtime target");
    SVariable *decoy = sjit_runtime_lookup_or_create_variable(
        second_runtime, 1, "decoy", SJIT_VAR_SCALAR);
    require(decoy, "create same-index replacement-runtime decoy");
    sjit_variable_set_number(decoy, 7.0);
    require(sjit_runtime_register_script_with_data(
        second_runtime, 1, 813, SJIT_HAT_EVENT_WHENFLAGCLICKED, "", 1, 0, entry, script),
        "register checked-handle script in replacement runtime");
    sjit_runtime_green_flag(second_runtime);
    sjit_runtime_tick(second_runtime);
    SVariable *isolated = sjit_runtime_lookup_or_create_variable(
        second_runtime, 1, "isolated", SJIT_VAR_SCALAR);
    require(isolated && isolated->value.tag == SJIT_VALUE_NUMBER && isolated->value.number == 42.0,
        "runtime-identity mismatch falls back before dereferencing stale owners");
    require(decoy->value.tag == SJIT_VALUE_NUMBER && decoy->value.number == 7.0,
        "checked handles validate the expected variable name after runtime-address reuse");

    sjit_runtime_destroy(second_runtime);
    jit.reset();
    sjit_compiled_script_destroy(script);
}

void test_interpreter_cache_rejects_recycled_runtime_identity() {
    SCompiledScript *script = sjit_compiled_script_create(2, 1);
    require(script, "create recycled-runtime cache script");
    script->statements[0].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[0].variable_name = sjit_string_new("out");
    script->statements[0].value = sjit_expr_create_variable("shared");

    SRuntime *first_runtime = sjit_runtime_create();
    SSprite *first_stage = sjit_runtime_create_sprite(first_runtime, "Stage", 1);
    SSprite *first_target = sjit_runtime_create_sprite(first_runtime, "FirstTarget", 0);
    require(first_runtime && first_stage && first_target && first_target->base.id == 2,
        "create first cache runtime targets");
    SVariable *first_shared = sjit_runtime_lookup_or_create_variable(
        first_runtime, first_stage->base.id, "shared", SJIT_VAR_SCALAR);
    SVariable *first_out = sjit_runtime_lookup_or_create_variable(
        first_runtime, first_stage->base.id, "out", SJIT_VAR_SCALAR);
    require(first_shared && first_out, "seed first runtime stage variables");
    sjit_variable_set_number(first_shared, 11.0);

    auto jit = std::make_unique<sjit::JitEngine>();
    SScriptEntryFn entry = jit->compileScript(
        *script, "sjit_recycled_runtime_cache", first_runtime);
    require(entry != sjit_script_interpreter_entry &&
            script->statements[0].variable_cache_owner_target_id == first_stage->base.id &&
            script->statements[0].value->variable_cache_owner_target_id == first_stage->base.id,
        "prebind statement and reporter caches to the first runtime stage");
    const uint64_t first_runtime_id = first_runtime->instance_id;
    sjit_runtime_destroy(first_runtime);

    SRuntime *second_runtime = sjit_runtime_create();
    SSprite *decoy = sjit_runtime_create_sprite(second_runtime, "NotAStage", 0);
    SSprite *second_target = sjit_runtime_create_sprite(second_runtime, "SecondTarget", 0);
    require(second_runtime && decoy && second_target && second_target->base.id == 2 &&
            second_runtime->instance_id != first_runtime_id,
        "create second runtime with the same target ids but different ownership semantics");
    SVariable *decoy_shared = sjit_runtime_lookup_or_create_variable(
        second_runtime, decoy->base.id, "shared", SJIT_VAR_SCALAR);
    SVariable *decoy_out = sjit_runtime_lookup_or_create_variable(
        second_runtime, decoy->base.id, "out", SJIT_VAR_SCALAR);
    require(decoy_shared && decoy_out, "seed recycled-address decoy variables");
    sjit_variable_set_number(decoy_shared, 7.0);
    sjit_variable_set_number(decoy_out, 9.0);

    /* Deterministically model malloc returning the same SRuntime address.
       The stale instance ids intentionally remain unchanged. */
    script->statements[0].variable_cache_runtime = second_runtime;
    script->statements[0].value->variable_cache_runtime = second_runtime;
    require(script->statements[0].variable_cache_runtime_instance_id == first_runtime_id &&
            script->statements[0].value->variable_cache_runtime_instance_id == first_runtime_id,
        "retain stale cache generations while modeling raw-address reuse");
    require(sjit_runtime_register_script_with_data(
        second_runtime,
        second_target->base.id,
        815,
        SJIT_HAT_EVENT_WHENFLAGCLICKED,
        "",
        1,
        0,
        entry,
        script),
        "register recycled-runtime cache script");
    sjit_runtime_green_flag(second_runtime);
    sjit_runtime_tick(second_runtime);

    SVariable *second_shared = sjit_runtime_lookup_or_create_variable(
        second_runtime, second_target->base.id, "shared", SJIT_VAR_SCALAR);
    SVariable *second_out = sjit_runtime_lookup_or_create_variable(
        second_runtime, second_target->base.id, "out", SJIT_VAR_SCALAR);
    require(second_shared && second_out && sjit_to_number(second_runtime, second_out->value) == 0.0,
        "runtime mismatch fallback resolves variables with second-runtime ownership");
    require(decoy_shared->value.tag == SJIT_VALUE_NUMBER && decoy_shared->value.number == 7.0 &&
            decoy_out->value.tag == SJIT_VALUE_NUMBER && decoy_out->value.number == 9.0,
        "stale statement/reporter caches cannot mutate same-id decoy variables");

    sjit_runtime_destroy(second_runtime);
    jit.reset();
    sjit_compiled_script_destroy(script);
}

void test_values_cast_compare() {
    SRuntime *runtime = sjit_runtime_create();
    SValue empty = sjit_make_string("");
    SValue falseText = sjit_make_string("false");
    SValue zeroText = sjit_make_string("0");
    SValue numericText = sjit_make_string("123");
    SValue hexText = sjit_make_string("0x10");
    SValue binaryText = sjit_make_string("0b101");
    SValue alphaText = sjit_make_string("abc");
    SValue whitespace = sjit_make_string(" ");
    SValue hiraganaA = sjit_make_string("あ");
    SValue hiraganaI = sjit_make_string("い");
    SValue nonNumericText = sjit_make_string("n");
    SValue nanText = sjit_make_string("NaN");
    SValue infinityText = sjit_make_string("Infinity");
    SValue trueText = sjit_make_string("true");
    SValue onePointFiveText = sjit_to_string(runtime, sjit_make_number(1.5));

    assert(sjit_to_number(runtime, empty) == 0.0);
    const double parsedHex = sjit_to_number(runtime, hexText);
    assert(parsedHex == 16.0);
    assert(sjit_to_number(runtime, binaryText) == 5.0);
    SString *cachedHex = static_cast<SString *>(hexText.ptr);
    require(
        cachedHex && cachedHex->number_cache_valid && cachedHex->number_cache_ok &&
            cachedHex->number_cache == 16.0 && parsedHex == 16.0,
        "numeric strings cache their parsed number");
    assert(sjit_to_number(runtime, hexText) == 16.0);
    assert(!sjit_to_bool(runtime, falseText));
    assert(!sjit_to_bool(runtime, zeroText));
    require(sjit_eq(runtime, numericText, sjit_make_number(123.0)), "numeric string equals number");
    require(!sjit_eq(runtime, sjit_make_number(123.0), nonNumericText), "finite number differs from nonnumeric text");
    require(sjit_eq(runtime, sjit_make_number(NAN), nanText), "NaN retains text comparison semantics");
    require(sjit_eq(runtime, sjit_make_number(INFINITY), infinityText), "infinity retains comparison semantics");
    require(sjit_eq(runtime, sjit_make_bool(1), trueText), "boolean retains text comparison semantics");
    assert(sjit_op_add(runtime, alphaText, sjit_make_number(5.0)).number == 5.0);
    assert(sjit_compare(runtime, whitespace, sjit_make_number(0.0)) < 0);
    assert(std::string(sjit_string_cstr(static_cast<const SString *>(onePointFiveText.ptr))) == "1.5");
    assert(sjit_lt(runtime, hiraganaA, hiraganaI));
    SValue number_123 = sjit_make_number(123.0);
    require(
        sjit_jit_value_compare(runtime, &numericText, &number_123, SJIT_EXPR_EQ) == 1,
        "JIT value comparison preserves numeric-string equality");
    require(
        sjit_jit_value_compare(runtime, &whitespace, &number_123, SJIT_EXPR_LT) == 1,
        "JIT value comparison preserves Scratch text ordering");
    require(
        sjit_jit_value_compare(runtime, &number_123, &nonNumericText, SJIT_EXPR_EQ) == 0,
        "JIT value comparison keeps finite numbers distinct from nonnumeric text");

    sjit_value_destroy(empty);
    sjit_value_destroy(falseText);
    sjit_value_destroy(zeroText);
    sjit_value_destroy(numericText);
    sjit_value_destroy(hexText);
    sjit_value_destroy(binaryText);
    sjit_value_destroy(alphaText);
    sjit_value_destroy(whitespace);
    sjit_value_destroy(hiraganaA);
    sjit_value_destroy(hiraganaI);
    sjit_value_destroy(nonNumericText);
    sjit_value_destroy(nanText);
    sjit_value_destroy(infinityText);
    sjit_value_destroy(trueText);
    sjit_value_destroy(onePointFiveText);
    sjit_runtime_destroy(runtime);
}

void test_operator_and_list() {
    SRuntime *runtime = sjit_runtime_create();
    SValue mod = sjit_op_mod(runtime, sjit_make_number(-1.0), sjit_make_number(10.0));
    assert(mod.number == 9.0);
    require(sjit_op_mathop(runtime, "abs", sjit_make_number(-5.0)).number == 5.0, "abs mathop");
    require(sjit_op_mathop(runtime, "floor", sjit_make_number(2.9)).number == 2.0, "floor mathop");
    require(sjit_op_mathop(runtime, "ceiling", sjit_make_number(2.1)).number == 3.0, "ceiling mathop");
    require(sjit_op_round(runtime, sjit_make_number(-1.5)).number == -1.0, "round ties toward positive infinity");
    require(sjit_op_mathop(runtime, "sqrt", sjit_make_number(9.0)).number == 3.0, "sqrt mathop");
    require(sjit_op_mathop(runtime, "sin", sjit_make_number(90.0)).number == 1.0, "sin mathop");
    require(sjit_op_mathop(runtime, "cos", sjit_make_number(90.0)).number == 0.0, "cos mathop");
    assert(std::isinf(sjit_op_mathop(runtime, "tan", sjit_make_number(90.0)).number));
    assert(sjit_op_mathop(runtime, "asin", sjit_make_number(1.0)).number == 90.0);
    assert(sjit_op_mathop(runtime, "acos", sjit_make_number(1.0)).number == 0.0);
    assert(sjit_op_mathop(runtime, "atan", sjit_make_number(1.0)).number > 44.999);
    assert(sjit_op_mathop(runtime, "ln", sjit_make_number(1.0)).number == 0.0);
    assert(sjit_op_mathop(runtime, "log", sjit_make_number(100.0)).number == 2.0);
    assert(sjit_op_mathop(runtime, "e ^", sjit_make_number(0.0)).number == 1.0);
    assert(sjit_op_mathop(runtime, "10 ^", sjit_make_number(2.0)).number == 100.0);

    SValue one = sjit_make_number(1.0);
    SValue text_one = sjit_make_string("1");
    SValue zero = sjit_make_number(0.0);
    SValue text_empty = sjit_make_string("");
    assert(sjit_op_eq(runtime, one, text_one));
    assert(sjit_op_gt(runtime, sjit_make_number(3.0), sjit_make_number(2.0)));
    assert(sjit_op_and(runtime, one, text_one));
    assert(sjit_op_or(runtime, zero, text_one));
    assert(sjit_op_not(runtime, text_empty));
    SValue mixed_case_text = sjit_make_string("Map_Kd texture.png");
    SValue mixed_case_needle = sjit_make_string("map_kd");
    SValue missing_needle = sjit_make_string("usemtl");
    require(
        sjit_op_contains(runtime, mixed_case_text, mixed_case_needle),
        "contains ignores case like Scratch");
    require(
        !sjit_op_contains(runtime, mixed_case_text, missing_needle),
        "contains rejects missing substring");

    SList *list = sjit_list_create();
    SValue a = sjit_make_string("a");
    SValue b = sjit_make_string("b");
    SValue ten = sjit_make_number(10.0);
    assert(sjit_list_push(list, a));
    assert(sjit_list_push(list, b));
    assert(sjit_list_push(list, ten));
    assert(sjit_list_length(list) == 3);
    assert(sjit_list_to_index(runtime, sjit_make_string("last"), sjit_list_length(list), 0) == 3);
    assert(sjit_list_contains(runtime, list, sjit_make_string("10")));
    assert(sjit_list_item_number(runtime, list, sjit_make_string("B")) == 2);
    SValue contents = sjit_list_contents(list);
    assert(sjit_compare(runtime, contents, sjit_make_string("a b 10")) == 0);

    SList *numeric_characters = sjit_list_create();
    require(sjit_list_push(numeric_characters, sjit_make_number(1)), "push numeric character");
    require(sjit_list_push(numeric_characters, sjit_make_number(2)), "push numeric character");
    SValue numeric_contents = sjit_list_contents(numeric_characters);
    require(
        std::string(sjit_string_cstr(static_cast<const SString *>(numeric_contents.ptr))) == "1 2",
        "numeric list items are separated like Scratch");

    SString *unicode = sjit_string_new("あ😀b");
    require(sjit_string_utf16_length(unicode) == 4, "string length uses JavaScript UTF-16 units");
    SString *hiragana = sjit_string_utf16_char_at(unicode, 0);
    require(std::string(sjit_string_cstr(hiragana)) == "あ", "letter-of returns a complete UTF-8 character");

    SValue last = sjit_make_string("last");
    const int append_index = sjit_list_to_index(runtime, last, sjit_list_length(list) + 1, 0);
    sjit_value_destroy(last);
    SValue tail = sjit_make_string("tail");
    assert(append_index == 4);
    assert(sjit_list_insert(list, append_index, tail));
    SValue got_tail = sjit_list_get(list, 4);
    assert(sjit_compare(runtime, got_tail, tail) == 0);
    SValue replacement = sjit_make_string("B");
    assert(sjit_list_replace(list, 2, replacement));
    assert(sjit_list_item_number(runtime, list, b) == 2);

    SList *shared = sjit_list_clone(list);
    SValue shared_replacement = sjit_make_string("shared");
    assert(sjit_list_replace(shared, 2, shared_replacement));
    SValue original_item = sjit_list_get(list, 2);
    SValue shared_item = sjit_list_get(shared, 2);
    assert(sjit_compare(runtime, original_item, replacement) == 0);
    assert(sjit_compare(runtime, shared_item, shared_replacement) == 0);

    assert(sjit_list_delete(list, 1));
    assert(sjit_list_length(list) == 3);
    sjit_list_clear(list);
    assert(sjit_list_length(list) == 0);

    sjit_value_destroy(a);
    sjit_value_destroy(b);
    sjit_value_destroy(ten);
    sjit_value_destroy(tail);
    sjit_value_destroy(got_tail);
    sjit_value_destroy(replacement);
    sjit_value_destroy(shared_replacement);
    sjit_value_destroy(original_item);
    sjit_value_destroy(shared_item);
    sjit_value_destroy(one);
    sjit_value_destroy(text_one);
    sjit_value_destroy(zero);
    sjit_value_destroy(text_empty);
    sjit_value_destroy(mixed_case_text);
    sjit_value_destroy(mixed_case_needle);
    sjit_value_destroy(missing_needle);
    sjit_value_destroy(contents);
    sjit_value_destroy(numeric_contents);
    sjit_string_destroy(hiragana);
    sjit_string_destroy(unicode);
    sjit_list_destroy(numeric_characters);
    sjit_list_destroy(shared);
    sjit_list_destroy(list);
    sjit_runtime_destroy(runtime);
}

void test_large_numeric_list_limit() {
    SList *list = sjit_list_create();
    require(list, "create large-list regression fixture");
    require(
        sjit_list_push_repeated(list, sjit_make_number(7.0), 200001),
        "append more than the legacy 200k list ceiling");
    require(
        sjit_list_length(list) == 200001,
        "large lists are not truncated at the legacy ceiling");
    double last = 0.0;
    require(
        sjit_list_get_number(list, 200001, &last) && last == 7.0,
        "large-list tail remains readable");
    sjit_list_destroy(list);
}

void test_compatibility_modes_and_turbowarp_keys() {
    SRuntime *runtime = sjit_runtime_create();
    require(runtime, "create compatibility runtime");
    require(
        sjit_runtime_compatibility_mode(runtime) == SJIT_COMPATIBILITY_MODE_TURBOWARP &&
            sjit_runtime_list_item_limit(runtime) == SJIT_TURBOWARP_LIST_ITEM_LIMIT,
        "runtime defaults to TurboWarp compatibility");

    SSprite *sprite = sjit_runtime_create_sprite(runtime, "Sprite", 0);
    SVariable *variable = sjit_runtime_lookup_or_create_variable(
        runtime, sprite->base.id, "items", SJIT_VAR_LIST);
    require(variable && variable->value.tag == SJIT_VALUE_LIST,
        "create compatibility list variable");
    SList *list = static_cast<SList *>(variable->value.ptr);
    require(sjit_list_item_limit(list) == SJIT_TURBOWARP_LIST_ITEM_LIMIT,
        "new list inherits the TurboWarp limit");

    require(sjit_runtime_set_compatibility_mode(runtime, SJIT_COMPATIBILITY_MODE_SCRATCH),
        "switch to Scratch compatibility");
    require(
        sjit_runtime_list_item_limit(runtime) == SJIT_SCRATCH_LIST_ITEM_LIMIT &&
            sjit_list_item_limit(list) == SJIT_SCRATCH_LIST_ITEM_LIMIT,
        "Scratch compatibility applies its list limit to existing lists");
    require(sjit_runtime_set_list_item_limit(runtime, 3),
        "override the compatibility list limit");
    require(sjit_list_push_repeated(list, sjit_make_number(1.0), 5),
        "append up to the overridden list limit");
    require(sjit_list_length(list) == 3,
        "list writes respect the runtime override");

    runtime->input.key_down[SJIT_KEY_DELETE] = 1;
    runtime->input.key_down[SJIT_KEY_SHIFT] = 1;
    runtime->input.key_down['A'] = 1;
    SValue delete_key = sjit_make_string("delete");
    SValue any_key = sjit_make_string("any");
    SValue lower_letter = sjit_make_string("a");
    require(sjit_jit_key_pressed_value(runtime, &delete_key),
        "TurboWarp delete key is recognized");
    require(sjit_jit_key_pressed_value(runtime, &any_key),
        "TurboWarp any key query is recognized");
    require(sjit_jit_key_pressed_value(runtime, &lower_letter),
        "letter key queries are case-insensitive");
    sjit_value_destroy(delete_key);
    sjit_value_destroy(any_key);
    sjit_value_destroy(lower_letter);
    sjit_runtime_destroy(runtime);
}

void test_numeric_list_cache_tracks_mutations() {
    SRuntime *runtime = sjit_runtime_create();
    SList *list = sjit_list_create();
    require(runtime && list, "create numeric cache fixture");
    require(sjit_list_push_number(list, 10.0), "cache numeric push");
    require(sjit_list_push_number(list, 30.0), "cache second numeric push");

    double number = 0.0;
    require(
        sjit_list_get_cached_number(list, 2, &number) && number == 30.0,
        "numeric cache exposes pushed values");

    SList *copy = sjit_list_clone(list);
    require(copy, "clone numeric cache fixture");
    require(sjit_list_insert_move(copy, 2, sjit_make_number(20.0)), "cache numeric insert");
    require(
        sjit_list_get_cached_number(copy, 2, &number) && number == 20.0,
        "numeric cache tracks insertion");
    require(
        sjit_list_get_cached_number(copy, 3, &number) && number == 30.0,
        "numeric cache shifts inserted tail");
    require(
        sjit_list_get_cached_number(list, 2, &number) && number == 30.0,
        "copy-on-write keeps the original numeric cache");

    require(sjit_list_replace_number(copy, 2, 25.0), "cache numeric replacement");
    require(
        sjit_list_get_cached_number(copy, 2, &number) && number == 25.0,
        "numeric cache tracks replacement");
    require(sjit_list_delete(copy, 1), "cache numeric delete");
    require(
        sjit_list_get_cached_number(copy, 1, &number) && number == 25.0,
        "numeric cache shifts deletion");
    require(
        sjit_list_push_repeated(copy, sjit_make_number(7.0), 2),
        "cache repeated numeric push");
    require(
        sjit_list_get_cached_number(copy, 4, &number) && number == 7.0,
        "numeric cache tracks repeated values");

    SValue text = sjit_make_string("9");
    require(sjit_list_replace(copy, 1, text), "replace cached value with text");
    sjit_value_destroy(text);
    require(
        !sjit_list_get_cached_number(copy, 2, &number),
        "mixed list invalidates exact numeric cache");
    require(
        sjit_list_get_coerced_number(runtime, copy, 1) == 9.0,
        "mixed list keeps numeric coercion semantics");

    sjit_list_clear(copy);
    require(sjit_list_push_number(copy, 42.0), "cache numeric push after clear");
    require(
        sjit_list_get_cached_number(copy, 1, &number) && number == 42.0,
        "clearing a mixed list re-enables numeric caching");

    sjit_list_destroy(copy);
    sjit_list_destroy(list);
    sjit_runtime_destroy(runtime);
}

void test_jit_borrowed_list_helpers() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "ListHelpers", 0);
    require(runtime && sprite, "create list helper runtime");

    SVariable *source_variable = sjit_runtime_lookup_or_create_variable(
        runtime, sprite->base.id, "source", SJIT_VAR_LIST);
    SVariable *destination_variable = sjit_runtime_lookup_or_create_variable(
        runtime, sprite->base.id, "destination", SJIT_VAR_LIST);
    SVariable *destination_index = sjit_runtime_lookup_or_create_variable(
        runtime, sprite->base.id, "destination index", SJIT_VAR_SCALAR);
    SVariable *source_index = sjit_runtime_lookup_or_create_variable(
        runtime, sprite->base.id, "source index", SJIT_VAR_SCALAR);
    require(
        source_variable && destination_variable && destination_index && source_index,
        "create list helper variables");
    SList *source = static_cast<SList *>(source_variable->value.ptr);
    SList *destination = static_cast<SList *>(destination_variable->value.ptr);
    for (const char *text : {"red", "green", "blue"}) {
        SValue value = sjit_make_string(text);
        require(sjit_list_push(source, value), "populate source list");
        sjit_value_destroy(value);
    }
    require(sjit_list_push(destination, sjit_make_number(0.0)), "populate destination list");
    require(sjit_list_push(destination, sjit_make_number(0.0)), "populate destination list");

    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 1);
    require(script, "create list helper script");
    SVariable *picked = sjit_runtime_lookup_or_create_variable(
        runtime, sprite->base.id, "picked", SJIT_VAR_SCALAR);
    require(picked, "create numeric list helper destination");
    SStatement &replace = script->statements[0];
    replace.opcode = SJIT_STMT_LIST_REPLACE;
    replace.variable_name = sjit_string_new("destination");
    replace.value = sjit_expr_create_list_item("source", literalNumber(1.0));

    SValue borrowed_item = sjit_make_null_fast();
    sjit_jit_list_variable_item_value_at_number(
        runtime,
        source_variable,
        2.0,
        &borrowed_item);
    SValue expected_green = sjit_make_string("green");
    require(
        sjit_eq(runtime, borrowed_item, expected_green),
        "numeric list value helper preserves the borrowed item");
    sjit_value_destroy(borrowed_item);
    sjit_value_destroy(expected_green);

    sjit_jit_variable_set_from_list_item_at_number(
        runtime,
        sprite->base.id,
        picked,
        replace.value,
        3.0);
    SValue expected_blue = sjit_make_string("blue");
    require(
        sjit_eq(runtime, picked->value, expected_blue),
        "numeric list assignment helper preserves the item value");
    sjit_value_destroy(expected_blue);

    sjit_variable_set_number(destination_index, 2.0);
    SValue last = sjit_make_string("last");
    sjit_variable_set(source_index, last);
    sjit_value_destroy(last);
    sjit_jit_statement_list_replace_list_item_at_variables(
        runtime,
        script,
        &replace,
        destination_index,
        replace.value,
        source_index);
    SValue item = sjit_list_get(destination, 2);
    SValue blue = sjit_make_string("blue");
    require(sjit_eq(runtime, item, blue), "variable-index list helper preserves the 'last' index");
    sjit_value_destroy(item);
    sjit_value_destroy(blue);

    sjit_variable_set_number(destination_index, 1.0);
    sjit_jit_statement_list_replace_list_item_at_variable_number(
        runtime,
        script,
        &replace,
        destination_index,
        replace.value,
        2.0);
    item = sjit_list_get(destination, 1);
    SValue green = sjit_make_string("green");
    require(sjit_eq(runtime, item, green), "numeric source-index list helper preserves the borrowed item");
    sjit_value_destroy(item);
    sjit_value_destroy(green);

    SExpr *blue_literal = literalString("blue");
    require(
        sjit_jit_expr_list_item_compare_literal_at_variable(
            runtime,
            sprite->base.id,
            replace.value,
            source_index,
            blue_literal,
            SJIT_EXPR_EQ) == 1,
        "borrowed list comparison preserves Scratch string equality");
    sjit_expr_destroy(blue_literal);

    sjit_variable_set_number(destination_index, 99.0);
    sjit_jit_statement_list_replace_list_item_at_variable_number(
        runtime,
        script,
        &replace,
        destination_index,
        replace.value,
        1.0);
    require(sjit_list_length(destination) == 2, "out-of-range destination index leaves the list unchanged");

    sjit_compiled_script_destroy(script);
    sjit_runtime_destroy(runtime);
}

void test_script_boolean_and_list_item_expr() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "Sprite1", 0);
    assert(sprite);

    SVariable *items_variable = sjit_target_lookup_or_create_variable(&sprite->base, 0, "items", SJIT_VAR_LIST);
    assert(items_variable && items_variable->value.tag == SJIT_VALUE_LIST);
    SList *items = static_cast<SList *>(items_variable->value.ptr);
    SValue zero = sjit_make_string("zero");
    SValue one = sjit_make_string("one");
    SValue two = sjit_make_string("two");
    require(sjit_list_push(items, zero), "push zero list item");
    require(sjit_list_push(items, one), "push one list item");
    require(sjit_list_push(items, two), "push two list item");
    sjit_value_destroy(zero);
    sjit_value_destroy(one);
    sjit_value_destroy(two);

    SVariable *dynamic_text = sjit_target_lookup_or_create_variable(&sprite->base, 0, "dynamic_text", SJIT_VAR_SCALAR);
    require(dynamic_text, "create dynamic text variable");
    sjit_variable_set_scalar_kind(dynamic_text, SJIT_SCALAR_STRING);
    SValue dynamic_text_value = sjit_make_string("not a number");
    sjit_variable_set(dynamic_text, dynamic_text_value);
    sjit_value_destroy(dynamic_text_value);

    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 14);
    assert(script);

    script->statements[0].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[0].variable_name = sjit_string_new("picked");
    script->statements[0].value = sjit_expr_create_list_item("items", literalNumber(2.0));

    script->statements[1].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[1].variable_name = sjit_string_new("item_count");
    script->statements[1].value = sjit_expr_create_list_length("items");

    script->statements[2].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[2].variable_name = sjit_string_new("has_two");
    script->statements[2].value = sjit_expr_create_list_contains("items", literalString("TWO"));

    script->statements[3].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[3].variable_name = sjit_string_new("one_position");
    script->statements[3].value = sjit_expr_create_list_item_number("items", literalString("one"));

    script->statements[4].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[4].variable_name = sjit_string_new("logic_and_not");
    script->statements[4].value = sjit_expr_create_binary(
        SJIT_EXPR_AND,
        sjit_expr_create_binary(SJIT_EXPR_GT, literalNumber(3.0), literalNumber(2.0)),
        sjit_expr_create_unary(
            SJIT_EXPR_NOT,
            sjit_expr_create_binary(SJIT_EXPR_EQ, literalString("a"), literalString("b"))));

    script->statements[5].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[5].variable_name = sjit_string_new("logic_or");
    script->statements[5].value = sjit_expr_create_binary(
        SJIT_EXPR_OR,
        literalBool(false),
        sjit_expr_create_binary(SJIT_EXPR_EQ, literalString("x"), literalString("x")));

    script->statements[6].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[6].variable_name = sjit_string_new("coerced_sum");
    script->statements[6].value = sjit_expr_create_binary(
        SJIT_EXPR_ADD,
        sjit_expr_create_variable("dynamic_text"),
        literalNumber(2.0));

    script->statements[7].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[7].variable_name = sjit_string_new("nested_nan_sum");
    script->statements[7].value = sjit_expr_create_binary(
        SJIT_EXPR_ADD,
        sjit_expr_create_binary(SJIT_EXPR_DIV, literalNumber(0.0), literalNumber(0.0)),
        literalNumber(1.0));

    script->statements[8].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[8].variable_name = sjit_string_new("list_coerced_sum");
    script->statements[8].value = sjit_expr_create_binary(
        SJIT_EXPR_ADD,
        sjit_expr_create_list_item("items", literalNumber(2.0)),
        literalNumber(3.0));

    script->statements[9].opcode = SJIT_STMT_IF;
    script->statements[9].condition = literalBool(true);
    script->statements[9].substack_count = 1;
    script->statements[9].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(script->statements[9].substack, "allocate synchronous if substack");
    script->statements[9].substack[0].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[9].substack[0].variable_name = sjit_string_new("sync_if_value");
    script->statements[9].substack[0].value = literalNumber(7.0);

    script->statements[10].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[10].variable_name = sjit_string_new("finite_vs_text");
    script->statements[10].value = sjit_expr_create_binary(
        SJIT_EXPR_EQ,
        literalNumber(123.0),
        literalString("n"));

    script->statements[11].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[11].variable_name = sjit_string_new("nan_vs_text");
    script->statements[11].value = sjit_expr_create_binary(
        SJIT_EXPR_EQ,
        sjit_expr_create_binary(SJIT_EXPR_DIV, literalNumber(0.0), literalNumber(0.0)),
        literalString("NaN"));

    script->statements[12].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[12].variable_name = sjit_string_new("bool_vs_text");
    script->statements[12].value = sjit_expr_create_binary(
        SJIT_EXPR_EQ,
        literalBool(true),
        literalString("true"));

    script->statements[13].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[13].variable_name = sjit_string_new("casefolded_text");
    script->statements[13].value = sjit_expr_create_binary(
        SJIT_EXPR_EQ,
        literalString("N"),
        literalString("n"));

    require(
        sjit_runtime_register_script_with_data(
            runtime,
            sprite->base.id,
            42,
            SJIT_HAT_EVENT_WHENFLAGCLICKED,
            "",
            1,
            0,
            sjit_script_interpreter_entry,
            script),
        "register interpreter expression script");

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);

    SVariable *picked = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "picked", SJIT_VAR_SCALAR);
    SVariable *item_count = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "item_count", SJIT_VAR_SCALAR);
    SVariable *has_two = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "has_two", SJIT_VAR_SCALAR);
    SVariable *one_position = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "one_position", SJIT_VAR_SCALAR);
    SVariable *logic_and_not = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "logic_and_not", SJIT_VAR_SCALAR);
    SVariable *logic_or = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "logic_or", SJIT_VAR_SCALAR);
    SVariable *coerced_sum = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "coerced_sum", SJIT_VAR_SCALAR);
    SVariable *nested_nan_sum = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "nested_nan_sum", SJIT_VAR_SCALAR);
    SVariable *list_coerced_sum = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "list_coerced_sum", SJIT_VAR_SCALAR);
    SVariable *sync_if_value = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "sync_if_value", SJIT_VAR_SCALAR);
    SVariable *finite_vs_text = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "finite_vs_text", SJIT_VAR_SCALAR);
    SVariable *nan_vs_text = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "nan_vs_text", SJIT_VAR_SCALAR);
    SVariable *bool_vs_text = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "bool_vs_text", SJIT_VAR_SCALAR);
    SVariable *casefolded_text = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "casefolded_text", SJIT_VAR_SCALAR);
    SValue expected = sjit_make_string("one");
    assert(picked && sjit_compare(runtime, picked->value, expected) == 0);
    assert(item_count && item_count->value.tag == SJIT_VALUE_NUMBER && item_count->value.number == 3.0);
    assert(has_two && has_two->value.tag == SJIT_VALUE_BOOL && has_two->value.number == 1.0);
    assert(one_position && one_position->value.tag == SJIT_VALUE_NUMBER && one_position->value.number == 2.0);
    assert(logic_and_not && logic_and_not->value.tag == SJIT_VALUE_BOOL && logic_and_not->value.number == 1.0);
    assert(logic_or && logic_or->value.tag == SJIT_VALUE_BOOL && logic_or->value.number == 1.0);
    require(
        coerced_sum && coerced_sum->value.tag == SJIT_VALUE_NUMBER && coerced_sum->value.number == 2.0,
        "numeric expression coerces only its dynamic string operand");
    require(
        nested_nan_sum && nested_nan_sum->value.tag == SJIT_VALUE_NUMBER && nested_nan_sum->value.number == 1.0,
        "nested numeric expression coerces an intermediate NaN like the generic operator path");
    require(
        list_coerced_sum && list_coerced_sum->value.tag == SJIT_VALUE_NUMBER && list_coerced_sum->value.number == 3.0,
        "numeric expression coerces a borrowed nonnumeric list item");
    require(
        sync_if_value && sync_if_value->value.tag == SJIT_VALUE_NUMBER && sync_if_value->value.number == 7.0,
        "synchronous if substack executes without changing its result");
    require(
        finite_vs_text && finite_vs_text->value.tag == SJIT_VALUE_BOOL && finite_vs_text->value.number == 0.0,
        "finite number differs from fixed nonnumeric text in interpreter comparison");
    require(
        nan_vs_text && nan_vs_text->value.tag == SJIT_VALUE_BOOL && nan_vs_text->value.number == 1.0,
        "NaN keeps text fallback in interpreter comparison");
    require(
        bool_vs_text && bool_vs_text->value.tag == SJIT_VALUE_BOOL && bool_vs_text->value.number == 1.0,
        "boolean keeps text fallback in interpreter comparison");
    require(
        casefolded_text && casefolded_text->value.tag == SJIT_VALUE_BOOL && casefolded_text->value.number == 1.0,
        "fixed nonnumeric text comparison remains case insensitive");
    sjit_value_destroy(expected);

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_project_loader_boolean_and_list_blocks() {
    const std::string project_json = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "variables": {
                "pickedVar": ["picked", 0],
                "okVar": ["ok", false]
            },
            "lists": {
                "itemsList": ["items", ["alpha", "beta", "gamma"]]
            },
            "blocks": {
                "hat": {
                    "opcode": "event_whenflagclicked",
                    "next": "setPicked",
                    "topLevel": true,
                    "inputs": {},
                    "fields": {}
                },
                "setPicked": {
                    "opcode": "data_setvariableto",
                    "next": "setOk",
                    "topLevel": false,
                    "inputs": {"VALUE": [2, "item"]},
                    "fields": {"VARIABLE": ["picked", "pickedVar"]}
                },
                "item": {
                    "opcode": "data_itemoflist",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"INDEX": [1, [4, "2"]]},
                    "fields": {"LIST": ["items", "itemsList"]}
                },
                "setOk": {
                    "opcode": "data_setvariableto",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [2, "andBlock"]},
                    "fields": {"VARIABLE": ["ok", "okVar"]}
                },
                "andBlock": {
                    "opcode": "operator_and",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"OPERAND1": [2, "orBlock"], "OPERAND2": [2, "notBlock"]},
                    "fields": {}
                },
                "orBlock": {
                    "opcode": "operator_or",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"OPERAND1": [1, [10, ""]], "OPERAND2": [2, "gtBlock"]},
                    "fields": {}
                },
                "gtBlock": {
                    "opcode": "operator_gt",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"OPERAND1": [1, [4, "3"]], "OPERAND2": [1, [4, "2"]]},
                    "fields": {}
                },
                "notBlock": {
                    "opcode": "operator_not",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"OPERAND": [2, "eqBlock"]},
                    "fields": {}
                },
                "eqBlock": {
                    "opcode": "operator_equals",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"OPERAND1": [1, [10, "a"]], "OPERAND2": [1, [10, "b"]]},
                    "fields": {}
                }
            }
        }]
    })json";

    SRuntime *runtime = sjit_runtime_create();
    sjit::ProjectLoadResult result = sjit::loadProjectIntoRuntime(runtime, writeSb3Fixture(project_json));
    assert(result.ok);
    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);

    SVariable *picked = sjit_runtime_lookup_or_create_variable(runtime, 1, "picked", SJIT_VAR_SCALAR);
    SVariable *ok = sjit_runtime_lookup_or_create_variable(runtime, 1, "ok", SJIT_VAR_SCALAR);
    SValue expected = sjit_make_string("beta");
    assert(picked && sjit_compare(runtime, picked->value, expected) == 0);
    assert(ok && ok->value.tag == SJIT_VALUE_BOOL && ok->value.number == 1.0);
    sjit_value_destroy(expected);
    sjit_runtime_destroy(runtime);
}

void test_project_loader_list_reporters_and_mutations() {
    const std::string project_json = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "variables": {
                "lenVar": ["len", 0],
                "hasVar": ["hasBeta", false],
                "indexVar": ["deltaIndex", 0],
                "itemVar": ["lastItem", ""]
            },
            "lists": {
                "itemsList": ["items", ["alpha", "beta", "gamma"]]
            },
            "blocks": {
                "hat": {
                    "opcode": "event_whenflagclicked",
                    "next": "addDelta",
                    "topLevel": true,
                    "inputs": {},
                    "fields": {}
                },
                "addDelta": {
                    "opcode": "data_addtolist",
                    "next": "insertStart",
                    "topLevel": false,
                    "inputs": {"ITEM": [1, [10, "delta"]]},
                    "fields": {"LIST": ["items", "itemsList"]}
                },
                "insertStart": {
                    "opcode": "data_insertatlist",
                    "next": "replaceThird",
                    "topLevel": false,
                    "inputs": {"INDEX": [1, [4, "1"]], "ITEM": [1, [10, "start"]]},
                    "fields": {"LIST": ["items", "itemsList"]}
                },
                "replaceThird": {
                    "opcode": "data_replaceitemoflist",
                    "next": "deleteFourth",
                    "topLevel": false,
                    "inputs": {"INDEX": [1, [4, "3"]], "ITEM": [1, [10, "BETA"]]},
                    "fields": {"LIST": ["items", "itemsList"]}
                },
                "deleteFourth": {
                    "opcode": "data_deleteoflist",
                    "next": "setLen",
                    "topLevel": false,
                    "inputs": {"INDEX": [1, [4, "4"]]},
                    "fields": {"LIST": ["items", "itemsList"]}
                },
                "setLen": {
                    "opcode": "data_setvariableto",
                    "next": "setHas",
                    "topLevel": false,
                    "inputs": {"VALUE": [2, "lenExpr"]},
                    "fields": {"VARIABLE": ["len", "lenVar"]}
                },
                "lenExpr": {
                    "opcode": "data_lengthoflist",
                    "next": null,
                    "topLevel": false,
                    "inputs": {},
                    "fields": {"LIST": ["items", "itemsList"]}
                },
                "setHas": {
                    "opcode": "data_setvariableto",
                    "next": "setIndex",
                    "topLevel": false,
                    "inputs": {"VALUE": [2, "containsExpr"]},
                    "fields": {"VARIABLE": ["hasBeta", "hasVar"]}
                },
                "containsExpr": {
                    "opcode": "data_listcontainsitem",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"ITEM": [1, [10, "beta"]]},
                    "fields": {"LIST": ["items", "itemsList"]}
                },
                "setIndex": {
                    "opcode": "data_setvariableto",
                    "next": "setItem",
                    "topLevel": false,
                    "inputs": {"VALUE": [2, "indexExpr"]},
                    "fields": {"VARIABLE": ["deltaIndex", "indexVar"]}
                },
                "indexExpr": {
                    "opcode": "data_itemnumoflist",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"ITEM": [1, [10, "delta"]]},
                    "fields": {"LIST": ["items", "itemsList"]}
                },
                "setItem": {
                    "opcode": "data_setvariableto",
                    "next": "deleteAll",
                    "topLevel": false,
                    "inputs": {"VALUE": [2, "itemExpr"]},
                    "fields": {"VARIABLE": ["lastItem", "itemVar"]}
                },
                "itemExpr": {
                    "opcode": "data_itemoflist",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"INDEX": [1, [10, "last"]]},
                    "fields": {"LIST": ["items", "itemsList"]}
                },
                "deleteAll": {
                    "opcode": "data_deletealloflist",
                    "next": null,
                    "topLevel": false,
                    "inputs": {},
                    "fields": {"LIST": ["items", "itemsList"]}
                }
            }
        }]
    })json";

    SRuntime *runtime = sjit_runtime_create();
    sjit::ProjectLoadResult result = sjit::loadProjectIntoRuntime(runtime, writeSb3Fixture(project_json));
    assert(result.ok);
    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);

    SVariable *len = sjit_runtime_lookup_or_create_variable(runtime, 1, "len", SJIT_VAR_SCALAR);
    SVariable *has_beta = sjit_runtime_lookup_or_create_variable(runtime, 1, "hasBeta", SJIT_VAR_SCALAR);
    SVariable *delta_index = sjit_runtime_lookup_or_create_variable(runtime, 1, "deltaIndex", SJIT_VAR_SCALAR);
    SVariable *last_item = sjit_runtime_lookup_or_create_variable(runtime, 1, "lastItem", SJIT_VAR_SCALAR);
    SVariable *items_variable = sjit_runtime_lookup_or_create_variable(runtime, 1, "items", SJIT_VAR_LIST);
    SList *items = items_variable && items_variable->value.tag == SJIT_VALUE_LIST ?
        static_cast<SList *>(items_variable->value.ptr) : nullptr;
    SValue expected = sjit_make_string("delta");

    assert(len && len->value.tag == SJIT_VALUE_NUMBER && len->value.number == 4.0);
    assert(has_beta && has_beta->value.tag == SJIT_VALUE_BOOL && has_beta->value.number == 1.0);
    assert(delta_index && delta_index->value.tag == SJIT_VALUE_NUMBER && delta_index->value.number == 4.0);
    assert(last_item && sjit_compare(runtime, last_item->value, expected) == 0);
    assert(items && sjit_list_length(items) == 0);

    sjit_value_destroy(expected);
    sjit_runtime_destroy(runtime);
}

void test_project_loader_variable_monitors_use_scratch_ids() {
    const std::string project_json = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "variables": {
                "stageFpsId": ["fps", 11]
            },
            "lists": {},
            "blocks": {}
        }, {
            "isStage": false,
            "name": "Sprite",
            "visible": true,
            "variables": {
                "spriteFpsId": ["fps", 22]
            },
            "lists": {},
            "blocks": {
                "hat": {
                    "opcode": "event_whenflagclicked",
                    "next": "hideStageFps",
                    "topLevel": true,
                    "inputs": {},
                    "fields": {}
                },
                "hideStageFps": {
                    "opcode": "data_hidevariable",
                    "next": "showSpriteFps",
                    "topLevel": false,
                    "inputs": {},
                    "fields": {"VARIABLE": ["fps", "stageFpsId"]}
                },
                "showSpriteFps": {
                    "opcode": "data_showvariable",
                    "next": null,
                    "topLevel": false,
                    "inputs": {},
                    "fields": {"VARIABLE": ["fps", "spriteFpsId"]}
                }
            }
        }],
        "monitors": [{
            "id": "stageFpsId",
            "mode": "default",
            "opcode": "data_variable",
            "params": {"VARIABLE": "fps"},
            "spriteName": null,
            "value": 11,
            "width": 0,
            "height": 0,
            "x": 17,
            "y": 23,
            "visible": true,
            "sliderMin": 0,
            "sliderMax": 100,
            "isDiscrete": true
        }, {
            "id": "spriteFpsId",
            "mode": "large",
            "opcode": "data_variable",
            "params": {"VARIABLE": "fps"},
            "spriteName": "Sprite",
            "value": 22,
            "width": 80,
            "height": 30,
            "x": 41,
            "y": 53,
            "visible": false,
            "sliderMin": -10,
            "sliderMax": 10,
            "isDiscrete": false
        }]
    })json";

    SRuntime *runtime = sjit_runtime_create();
    sjit::ProjectLoadResult result = sjit::loadProjectIntoRuntime(runtime, writeSb3Fixture(project_json));
    require(result.ok, "load variable monitor fixture");
    require(sjit_runtime_variable_monitor_count(runtime) == 2, "register declared variable monitors");

    SVariableMonitor *stage_monitor = sjit_runtime_lookup_variable_monitor(runtime, "stageFpsId");
    SVariableMonitor *sprite_monitor = sjit_runtime_lookup_variable_monitor(runtime, "spriteFpsId");
    require(stage_monitor && stage_monitor->visible == 1, "load visible stage monitor");
    require(stage_monitor->target_id == 1 && stage_monitor->x == 17.0 && stage_monitor->y == 23.0,
        "bind stage monitor layout and owner");
    require(sprite_monitor && sprite_monitor->visible == 0, "load hidden sprite monitor");
    require(sprite_monitor->target_id == 2 && sprite_monitor->mode == SJIT_MONITOR_MODE_LARGE,
        "bind sprite monitor mode and owner");

    SVariable *stage_value = sjit_runtime_variable_for_monitor(runtime, stage_monitor);
    SVariable *sprite_value = sjit_runtime_variable_for_monitor(runtime, sprite_monitor);
    require(stage_value && stage_value->value.tag == SJIT_VALUE_NUMBER && stage_value->value.number == 11.0,
        "resolve stage monitor by Scratch id binding");
    require(sprite_value && sprite_value->value.tag == SJIT_VALUE_NUMBER && sprite_value->value.number == 22.0,
        "resolve same-named sprite monitor independently");

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);

    stage_monitor = sjit_runtime_lookup_variable_monitor(runtime, "stageFpsId");
    sprite_monitor = sjit_runtime_lookup_variable_monitor(runtime, "spriteFpsId");
    require(stage_monitor && stage_monitor->visible == 0, "hide variable targets exact Scratch id");
    require(sprite_monitor && sprite_monitor->visible == 1, "show variable targets exact Scratch id");

    sjit_runtime_destroy(runtime);
}

void test_project_loader_variable_and_list_ids_resolve_same_names() {
    const std::string project_json = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "variables": {
                "globalVarId": ["same", 10],
                "globalResultId": ["globalResult", 0]
            },
            "lists": {
                "globalListId": ["items", ["global"]]
            },
            "blocks": {}
        }, {
            "isStage": false,
            "name": "Sprite",
            "visible": true,
            "variables": {
                "localVarId": ["same", 20],
                "listResultId": ["listResult", ""]
            },
            "lists": {
                "localListId": ["items", ["local"]]
            },
            "blocks": {
                "hat": {
                    "opcode": "event_whenflagclicked",
                    "next": "changeLocal",
                    "topLevel": true,
                    "inputs": {},
                    "fields": {}
                },
                "changeLocal": {
                    "opcode": "data_changevariableby",
                    "next": "changeGlobal",
                    "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "1"]]},
                    "fields": {"VARIABLE": ["same", "localVarId"]}
                },
                "changeGlobal": {
                    "opcode": "data_changevariableby",
                    "next": "addLocal",
                    "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "1"]]},
                    "fields": {"VARIABLE": ["same", "globalVarId"]}
                },
                "addLocal": {
                    "opcode": "data_addtolist",
                    "next": "addGlobal",
                    "topLevel": false,
                    "inputs": {"ITEM": [1, [10, "L"]]},
                    "fields": {"LIST": ["items", "localListId"]}
                },
                "addGlobal": {
                    "opcode": "data_addtolist",
                    "next": "setGlobalResult",
                    "topLevel": false,
                    "inputs": {"ITEM": [1, [10, "G"]]},
                    "fields": {"LIST": ["items", "globalListId"]}
                },
                "setGlobalResult": {
                    "opcode": "data_setvariableto",
                    "next": "setListResult",
                    "topLevel": false,
                    "inputs": {"VALUE": [2, "globalReporter"]},
                    "fields": {"VARIABLE": ["globalResult", "globalResultId"]}
                },
                "globalReporter": {
                    "opcode": "data_variable",
                    "next": null,
                    "topLevel": false,
                    "inputs": {},
                    "fields": {"VARIABLE": ["same", "globalVarId"]}
                },
                "setListResult": {
                    "opcode": "data_setvariableto",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [2, "globalListItem"]},
                    "fields": {"VARIABLE": ["listResult", "listResultId"]}
                },
                "globalListItem": {
                    "opcode": "data_itemoflist",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"INDEX": [1, [4, "2"]]},
                    "fields": {"LIST": ["items", "globalListId"]}
                }
            }
        }]
    })json";

    SRuntime *runtime = sjit_runtime_create();
    sjit::ProjectLoadResult result = sjit::loadProjectIntoRuntime(runtime, writeSb3Fixture(project_json));
    require(result.ok, "load same-name variable and list ID fixture");

    SVariable *stage_same = sjit_runtime_lookup_or_create_variable(runtime, 1, "same", SJIT_VAR_SCALAR);
    SVariable *sprite_same = sjit_runtime_lookup_or_create_variable(runtime, 2, "same", SJIT_VAR_SCALAR);
    SVariable *stage_result = sjit_runtime_lookup_or_create_variable(runtime, 1, "globalResult", SJIT_VAR_SCALAR);
    SVariable *sprite_result = sjit_runtime_lookup_or_create_variable(runtime, 2, "listResult", SJIT_VAR_SCALAR);
    SVariable *stage_items = sjit_runtime_lookup_or_create_variable(runtime, 1, "items", SJIT_VAR_LIST);
    SVariable *sprite_items = sjit_runtime_lookup_or_create_variable(runtime, 2, "items", SJIT_VAR_LIST);
    require(stage_same && sprite_same && stage_result && sprite_result && stage_items && sprite_items,
        "resolve declared same-name variables and lists");
    require(stage_same->scratch_id &&
            std::strcmp(sjit_string_cstr(stage_same->scratch_id), "globalVarId") == 0,
        "preserve global variable Scratch ID");
    require(sprite_same->scratch_id &&
            std::strcmp(sjit_string_cstr(sprite_same->scratch_id), "localVarId") == 0,
        "preserve local variable Scratch ID");
    require(stage_items->scratch_id &&
            std::strcmp(sjit_string_cstr(stage_items->scratch_id), "globalListId") == 0,
        "preserve global list Scratch ID");
    require(sprite_items->scratch_id &&
            std::strcmp(sjit_string_cstr(sprite_items->scratch_id), "localListId") == 0,
        "preserve local list Scratch ID");

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);

    require(stage_same->value.tag == SJIT_VALUE_NUMBER && stage_same->value.number == 11.0,
        "same-name global variable reference uses its Scratch ID");
    require(sprite_same->value.tag == SJIT_VALUE_NUMBER && sprite_same->value.number == 21.0,
        "same-name local variable reference uses its Scratch ID");
    require(stage_result->value.tag == SJIT_VALUE_NUMBER && stage_result->value.number == 11.0,
        "cross-target global variable reporter uses its Scratch ID");
    require(sprite_result->value.tag == SJIT_VALUE_STRING &&
            std::string(sjit_string_cstr(static_cast<const SString *>(sprite_result->value.ptr))) == "G",
        "cross-target global list reporter uses its Scratch ID");

    SList *stage_list = stage_items->value.tag == SJIT_VALUE_LIST ?
        static_cast<SList *>(stage_items->value.ptr) : nullptr;
    SList *sprite_list = sprite_items->value.tag == SJIT_VALUE_LIST ?
        static_cast<SList *>(sprite_items->value.ptr) : nullptr;
    require(stage_list && sjit_list_length(stage_list) == 2,
        "global list mutation resolves its Scratch ID");
    require(sprite_list && sjit_list_length(sprite_list) == 2,
        "local list mutation resolves its Scratch ID");
    SValue stage_item = stage_list ? sjit_list_get(stage_list, 2) : sjit_make_string("");
    SValue sprite_item = sprite_list ? sjit_list_get(sprite_list, 2) : sjit_make_string("");
    require(stage_item.tag == SJIT_VALUE_STRING &&
            std::string(sjit_string_cstr(static_cast<const SString *>(stage_item.ptr))) == "G",
        "global list keeps the global-ID item");
    require(sprite_item.tag == SJIT_VALUE_STRING &&
            std::string(sjit_string_cstr(static_cast<const SString *>(sprite_item.ptr))) == "L",
        "local list keeps the local-ID item");
    sjit_value_destroy(stage_item);
    sjit_value_destroy(sprite_item);

    sjit_runtime_destroy(runtime);

    SRuntime *interpreter_runtime = sjit_runtime_create();
    sjit::ProjectLoadResult interpreter_result = sjit::loadProjectIntoRuntime(
        interpreter_runtime,
        writeSb3Fixture(project_json));
    require(interpreter_result.ok && interpreter_result.program.scripts.size() == 1,
        "load same-name ID fixture for interpreter regression");
    SCompiledScript *interpreter_script = interpreter_result.program.scripts[0];
    for (int statement_index = 0;
         statement_index < interpreter_script->statement_count;
         ++statement_index) {
        require(sjit_script_execute_statement(
                    interpreter_runtime,
                    interpreter_script,
                    statement_index) == SJIT_STATUS_OK,
            "interpreter executes ID-bound same-name script");
    }
    SVariable *interpreter_stage_same = sjit_runtime_lookup_variable_by_scratch_id(
        interpreter_runtime, 1, "globalVarId", "same", SJIT_VAR_SCALAR);
    SVariable *interpreter_sprite_same = sjit_runtime_lookup_variable_by_scratch_id(
        interpreter_runtime, 2, "localVarId", "same", SJIT_VAR_SCALAR);
    SVariable *interpreter_stage_items = sjit_runtime_lookup_variable_by_scratch_id(
        interpreter_runtime, 1, "globalListId", "items", SJIT_VAR_LIST);
    SVariable *interpreter_sprite_items = sjit_runtime_lookup_variable_by_scratch_id(
        interpreter_runtime, 2, "localListId", "items", SJIT_VAR_LIST);
    require(interpreter_stage_same && interpreter_stage_same->value.tag == SJIT_VALUE_NUMBER &&
            interpreter_stage_same->value.number == 11.0,
        "interpreter resolves the global same-name variable by Scratch ID");
    require(interpreter_sprite_same && interpreter_sprite_same->value.tag == SJIT_VALUE_NUMBER &&
            interpreter_sprite_same->value.number == 21.0,
        "interpreter resolves the local same-name variable by Scratch ID");
    require(interpreter_stage_items && interpreter_stage_items->value.tag == SJIT_VALUE_LIST &&
            sjit_list_length(static_cast<SList *>(interpreter_stage_items->value.ptr)) == 2,
        "interpreter resolves the global same-name list by Scratch ID");
    require(interpreter_sprite_items && interpreter_sprite_items->value.tag == SJIT_VALUE_LIST &&
            sjit_list_length(static_cast<SList *>(interpreter_sprite_items->value.ptr)) == 2,
        "interpreter resolves the local same-name list by Scratch ID");
    sjit_runtime_destroy(interpreter_runtime);
}

void test_project_loader_procedure_forever_for_each_and_pen() {
    const std::string project_json = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "variables": {},
            "lists": {},
            "blocks": {}
        }, {
            "isStage": false,
            "name": "Renderer",
            "visible": true,
            "variables": {
                "iVar": ["i", 0],
                "branchVar": ["branch", 0]
            },
            "lists": {
                "xsList": ["xs", [0, 10]],
                "ysList": ["ys", [10, 20]]
            },
            "blocks": {
                "proto": {
                    "opcode": "procedures_prototype",
                    "next": null,
                    "topLevel": false,
                    "inputs": {},
                    "fields": {},
                    "mutation": {
                        "tagName": "mutation",
                        "children": [],
                        "proccode": "draw",
                        "argumentids": "[]",
                        "argumentnames": "[]",
                        "argumentdefaults": "[]",
                        "warp": "true"
                    }
                },
                "def": {
                    "opcode": "procedures_definition",
                    "next": "clear",
                    "topLevel": true,
                    "inputs": {"custom_block": [2, "proto"]},
                    "fields": {}
                },
                "clear": {
                    "opcode": "pen_clear",
                    "next": "ifElse",
                    "topLevel": false,
                    "inputs": {},
                    "fields": {}
                },
                "ifElse": {
                    "opcode": "control_if_else",
                    "next": "forEach",
                    "topLevel": false,
                    "inputs": {
                        "CONDITION": [2, "mouseDown"],
                        "SUBSTACK": [2, "setBranchDown"],
                        "SUBSTACK2": [2, "setBranchUp"]
                    },
                    "fields": {}
                },
                "mouseDown": {
                    "opcode": "sensing_mousedown",
                    "next": null,
                    "topLevel": false,
                    "inputs": {},
                    "fields": {}
                },
                "setBranchDown": {
                    "opcode": "data_setvariableto",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "1"]]},
                    "fields": {"VARIABLE": ["branch", "branchVar"]}
                },
                "setBranchUp": {
                    "opcode": "data_setvariableto",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "2"]]},
                    "fields": {"VARIABLE": ["branch", "branchVar"]}
                },
                "forEach": {
                    "opcode": "control_for_each",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [2, "lenXs"], "SUBSTACK": [2, "penUpStart"]},
                    "fields": {"VARIABLE": ["i", "iVar"]}
                },
                "lenXs": {
                    "opcode": "data_lengthoflist",
                    "next": null,
                    "topLevel": false,
                    "inputs": {},
                    "fields": {"LIST": ["xs", "xsList"]}
                },
                "penUpStart": {
                    "opcode": "pen_penUp",
                    "next": "gotoStart",
                    "topLevel": false,
                    "inputs": {},
                    "fields": {}
                },
                "gotoStart": {
                    "opcode": "motion_gotoxy",
                    "next": "penDown",
                    "topLevel": false,
                    "inputs": {"X": [2, "itemXStart"], "Y": [1, [4, "0"]]},
                    "fields": {}
                },
                "itemXStart": {
                    "opcode": "data_itemoflist",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"INDEX": [2, [12, "i", "iVar"]]},
                    "fields": {"LIST": ["xs", "xsList"]}
                },
                "penDown": {
                    "opcode": "pen_penDown",
                    "next": "gotoEnd",
                    "topLevel": false,
                    "inputs": {},
                    "fields": {}
                },
                "gotoEnd": {
                    "opcode": "motion_gotoxy",
                    "next": "penUpEnd",
                    "topLevel": false,
                    "inputs": {"X": [2, "itemXEnd"], "Y": [2, "itemYEnd"]},
                    "fields": {}
                },
                "itemXEnd": {
                    "opcode": "data_itemoflist",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"INDEX": [2, [12, "i", "iVar"]]},
                    "fields": {"LIST": ["xs", "xsList"]}
                },
                "itemYEnd": {
                    "opcode": "data_itemoflist",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"INDEX": [2, [12, "i", "iVar"]]},
                    "fields": {"LIST": ["ys", "ysList"]}
                },
                "penUpEnd": {
                    "opcode": "pen_penUp",
                    "next": null,
                    "topLevel": false,
                    "inputs": {},
                    "fields": {}
                },
                "hat": {
                    "opcode": "event_whenflagclicked",
                    "next": "hide",
                    "topLevel": true,
                    "inputs": {},
                    "fields": {}
                },
                "hide": {
                    "opcode": "looks_hide",
                    "next": "penSize",
                    "topLevel": false,
                    "inputs": {},
                    "fields": {}
                },
                "penSize": {
                    "opcode": "pen_setPenSizeTo",
                    "next": "penColor",
                    "topLevel": false,
                    "inputs": {"SIZE": [1, [4, "1"]]},
                    "fields": {}
                },
                "penColor": {
                    "opcode": "pen_setPenColorToColor",
                    "next": "forever",
                    "topLevel": false,
                    "inputs": {"COLOR": [1, [10, "#123456"]]},
                    "fields": {}
                },
                "forever": {
                    "opcode": "control_forever",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"SUBSTACK": [2, "callDraw"]},
                    "fields": {}
                },
                "callDraw": {
                    "opcode": "procedures_call",
                    "next": null,
                    "topLevel": false,
                    "inputs": {},
                    "fields": {},
                    "mutation": {
                        "tagName": "mutation",
                        "children": [],
                        "proccode": "draw",
                        "argumentids": "[]",
                        "warp": "true"
                    }
                }
            }
        }]
    })json";

    SRuntime *runtime = sjit_runtime_create();
    sjit::ProjectLoadResult result = sjit::loadProjectIntoRuntime(runtime, writeSb3Fixture(project_json));
    assert(result.ok);

    SHostInputSnapshot input{};
    input.mouse_down = 0;
    sjit_runtime_set_input(runtime, &input);
    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);

    SSprite *sprite = sjit_runtime_get_sprite(runtime, 2);
    SVariable *branch = sjit_runtime_lookup_or_create_variable(runtime, 2, "branch", SJIT_VAR_SCALAR);
    const SDrawCommandBuffer *draw = sjit_runtime_get_draw_commands(runtime);
    SValue expected_branch = sjit_make_string("2");
    assert(runtime->thread_count == 1);
    assert(sprite && sprite->visible == 0);
    assert(branch && branch->value.tag == SJIT_VALUE_STRING && sjit_compare(runtime, branch->value, expected_branch) == 0);
    sjit_value_destroy(expected_branch);
    assert(draw && draw->length == 4);
    assert(draw->items[0].kind == SJIT_DRAW_PEN_STROKE);
    assert(draw->items[0].x == 0.0 && draw->items[0].y == 0.0);
    assert(draw->items[0].x2 == 0.0 && draw->items[0].y2 == 0.0);
    assert(draw->items[1].x == 0.0 && draw->items[1].y == 0.0);
    assert(draw->items[1].x2 == 0.0 && draw->items[1].y2 == 10.0);
    assert(draw->items[2].x == 10.0 && draw->items[2].y == 0.0);
    assert(draw->items[2].x2 == 10.0 && draw->items[2].y2 == 0.0);
    assert(draw->items[3].x == 10.0 && draw->items[3].y == 0.0);
    assert(draw->items[3].x2 == 10.0 && draw->items[3].y2 == 20.0);

    input.mouse_down = 0;
    input.mouse_pressed_edge = 1;
    sjit_runtime_set_input(runtime, &input);
    sjit_runtime_tick(runtime);
    branch = sjit_runtime_lookup_or_create_variable(runtime, 2, "branch", SJIT_VAR_SCALAR);
    draw = sjit_runtime_get_draw_commands(runtime);
    expected_branch = sjit_make_string("1");
    assert(branch && branch->value.tag == SJIT_VALUE_STRING && sjit_compare(runtime, branch->value, expected_branch) == 0);
    sjit_value_destroy(expected_branch);
    assert(draw && draw->length == 4);

    sjit_runtime_destroy(runtime);
}

void test_project_loader_control_loops_yield_like_scratch() {
    const std::string repeat_project_json = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "variables": {},
            "lists": {},
            "blocks": {}
        }, {
            "isStage": false,
            "name": "Loops",
            "visible": true,
            "variables": {
                "xVar": ["x", 0],
                "doneVar": ["done", 0]
            },
            "blocks": {
                "hat": {
                    "opcode": "event_whenflagclicked",
                    "next": "repeat",
                    "topLevel": true,
                    "inputs": {},
                    "fields": {}
                },
                "repeat": {
                    "opcode": "control_repeat",
                    "next": "done",
                    "topLevel": false,
                    "inputs": {"TIMES": [1, [4, "3"]], "SUBSTACK": [2, "change"]},
                    "fields": {}
                },
                "change": {
                    "opcode": "data_changevariableby",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "1"]]},
                    "fields": {"VARIABLE": ["x", "xVar"]}
                },
                "done": {
                    "opcode": "data_setvariableto",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "1"]]},
                    "fields": {"VARIABLE": ["done", "doneVar"]}
                }
            }
        }]
    })json";

    SRuntime *runtime = sjit_runtime_create();
    sjit::ProjectLoadResult result = sjit::loadProjectIntoRuntime(runtime, writeSb3Fixture(repeat_project_json));
    require(result.ok, "repeat project should load");
    sjit_runtime_green_flag(runtime);

    sjit_runtime_tick(runtime);
    SVariable *x = sjit_runtime_lookup_or_create_variable(runtime, 2, "x", SJIT_VAR_SCALAR);
    SVariable *done = sjit_runtime_lookup_or_create_variable(runtime, 2, "done", SJIT_VAR_SCALAR);
    require(x && x->value.tag == SJIT_VALUE_NUMBER && x->value.number == 3.0,
        "data-only repeat should run multiple branches in one runtime tick");
    require(done->value.tag == SJIT_VALUE_STRING &&
        std::string(sjit_string_cstr(static_cast<const SString *>(done->value.ptr))) == "1",
        "data-only repeat should continue after exhausted counter in the same runtime tick");
    sjit_runtime_destroy(runtime);

    const std::string motion_repeat_project_json = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "variables": {},
            "lists": {},
            "blocks": {}
        }, {
            "isStage": false,
            "name": "MotionLoop",
            "visible": true,
            "variables": {
                "doneVar": ["done", 0]
            },
            "blocks": {
                "hat": {
                    "opcode": "event_whenflagclicked",
                    "next": "repeat",
                    "topLevel": true,
                    "inputs": {},
                    "fields": {}
                },
                "repeat": {
                    "opcode": "control_repeat",
                    "next": "done",
                    "topLevel": false,
                    "inputs": {"TIMES": [1, [4, "3"]], "SUBSTACK": [2, "move"]},
                    "fields": {}
                },
                "move": {
                    "opcode": "motion_changexby",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"DX": [1, [4, "1"]]},
                    "fields": {}
                },
                "done": {
                    "opcode": "data_setvariableto",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "1"]]},
                    "fields": {"VARIABLE": ["done", "doneVar"]}
                }
            }
        }]
    })json";

    runtime = sjit_runtime_create();
    result = sjit::loadProjectIntoRuntime(runtime, writeSb3Fixture(motion_repeat_project_json));
    require(result.ok, "motion repeat project should load");
    sjit_runtime_green_flag(runtime);
    SSprite *motion_sprite = sjit_runtime_get_sprite(runtime, 2);
    done = sjit_runtime_lookup_or_create_variable(runtime, 2, "done", SJIT_VAR_SCALAR);

    sjit_runtime_tick(runtime);
    require(motion_sprite && motion_sprite->x == 1.0, "redraw loop should stop after one branch in normal mode");
    require(done && done->value.tag == SJIT_VALUE_NUMBER && done->value.number == 0.0,
        "normal mode should not continue past redraw loop branch in the same runtime tick");
    sjit_runtime_tick(runtime);
    require(motion_sprite->x == 2.0, "normal mode redraw loop should resume on following tick");
    sjit_runtime_tick(runtime);
    require(motion_sprite->x == 3.0, "normal mode redraw loop should run final branch on later tick");
    sjit_runtime_tick(runtime);
    require(done->value.tag == SJIT_VALUE_STRING &&
        std::string(sjit_string_cstr(static_cast<const SString *>(done->value.ptr))) == "1",
        "normal mode redraw loop should continue after exhausted counter on a later tick");
    sjit_runtime_destroy(runtime);

    runtime = sjit_runtime_create();
    sjit_runtime_set_turbo_mode(runtime, 1);
    result = sjit::loadProjectIntoRuntime(runtime, writeSb3Fixture(motion_repeat_project_json));
    require(result.ok, "turbo motion repeat project should load");
    sjit_runtime_green_flag(runtime);
    motion_sprite = sjit_runtime_get_sprite(runtime, 2);
    done = sjit_runtime_lookup_or_create_variable(runtime, 2, "done", SJIT_VAR_SCALAR);
    sjit_runtime_tick(runtime);
    require(motion_sprite && motion_sprite->x == 3.0,
        "turbo mode should keep stepping redraw loop branches in one runtime tick");
    require(done->value.tag == SJIT_VALUE_STRING &&
        std::string(sjit_string_cstr(static_cast<const SString *>(done->value.ptr))) == "1",
        "turbo mode should continue after redraw loop in the same runtime tick");
    sjit_runtime_destroy(runtime);

    const std::string nested_repeat_project_json = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "variables": {},
            "lists": {},
            "blocks": {}
        }, {
            "isStage": false,
            "name": "NestedLoops",
            "visible": true,
            "variables": {
                "xVar": ["x", 0],
                "doneVar": ["done", 0]
            },
            "blocks": {
                "hat": {
                    "opcode": "event_whenflagclicked",
                    "next": "outer",
                    "topLevel": true,
                    "inputs": {},
                    "fields": {}
                },
                "outer": {
                    "opcode": "control_repeat",
                    "next": "done",
                    "topLevel": false,
                    "inputs": {"TIMES": [1, [4, "2"]], "SUBSTACK": [2, "inner"]},
                    "fields": {}
                },
                "inner": {
                    "opcode": "control_repeat",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"TIMES": [1, [4, "3"]], "SUBSTACK": [2, "change"]},
                    "fields": {}
                },
                "change": {
                    "opcode": "data_changevariableby",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "1"]]},
                    "fields": {"VARIABLE": ["x", "xVar"]}
                },
                "done": {
                    "opcode": "data_setvariableto",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "1"]]},
                    "fields": {"VARIABLE": ["done", "doneVar"]}
                }
            }
        }]
    })json";

    runtime = sjit_runtime_create();
    result = sjit::loadProjectIntoRuntime(runtime, writeSb3Fixture(nested_repeat_project_json));
    require(result.ok, "nested repeat project should load");
    sjit_runtime_green_flag(runtime);
    x = sjit_runtime_lookup_or_create_variable(runtime, 2, "x", SJIT_VAR_SCALAR);
    done = sjit_runtime_lookup_or_create_variable(runtime, 2, "done", SJIT_VAR_SCALAR);

    sjit_runtime_tick(runtime);
    require(x->value.tag == SJIT_VALUE_NUMBER && x->value.number == 6.0,
        "nested data-only repeat should run all inner loop branches in one runtime tick");
    require(done->value.tag == SJIT_VALUE_STRING &&
        std::string(sjit_string_cstr(static_cast<const SString *>(done->value.ptr))) == "1",
        "nested repeat should continue after outer counter exhausts in the same runtime tick");
    sjit_runtime_destroy(runtime);

    const std::string for_each_project_json = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "variables": {},
            "lists": {},
            "blocks": {}
        }, {
            "isStage": false,
            "name": "ForEach",
            "visible": true,
            "variables": {
                "iVar": ["i", 0],
                "sumVar": ["sum", 0],
                "doneVar": ["done", 0]
            },
            "blocks": {
                "hat": {
                    "opcode": "event_whenflagclicked",
                    "next": "forEach",
                    "topLevel": true,
                    "inputs": {},
                    "fields": {}
                },
                "forEach": {
                    "opcode": "control_for_each",
                    "next": "done",
                    "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "2.7"]], "SUBSTACK": [2, "change"]},
                    "fields": {"VARIABLE": ["i", "iVar"]}
                },
                "change": {
                    "opcode": "data_changevariableby",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [2, [12, "i", "iVar"]]},
                    "fields": {"VARIABLE": ["sum", "sumVar"]}
                },
                "done": {
                    "opcode": "data_setvariableto",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "1"]]},
                    "fields": {"VARIABLE": ["done", "doneVar"]}
                }
            }
        }]
    })json";

    runtime = sjit_runtime_create();
    result = sjit::loadProjectIntoRuntime(runtime, writeSb3Fixture(for_each_project_json));
    require(result.ok, "for each project should load");
    sjit_runtime_green_flag(runtime);

    sjit_runtime_tick(runtime);
    SVariable *i = sjit_runtime_lookup_or_create_variable(runtime, 2, "i", SJIT_VAR_SCALAR);
    SVariable *sum = sjit_runtime_lookup_or_create_variable(runtime, 2, "sum", SJIT_VAR_SCALAR);
    done = sjit_runtime_lookup_or_create_variable(runtime, 2, "done", SJIT_VAR_SCALAR);
    require(i && i->value.tag == SJIT_VALUE_NUMBER && i->value.number == 3.0,
        "data-only for each should run all branches in one runtime tick");
    require(sum->value.tag == SJIT_VALUE_NUMBER && sum->value.number == 6.0, "for each should add index 3 for a 2.7 limit");
    require(done->value.tag == SJIT_VALUE_STRING &&
        std::string(sjit_string_cstr(static_cast<const SString *>(done->value.ptr))) == "1",
        "for each should continue after exhausted index in the same runtime tick");

    sjit_runtime_destroy(runtime);

    const std::string while_project_json = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "variables": {},
            "lists": {},
            "blocks": {}
        }, {
            "isStage": false,
            "name": "WhileLoop",
            "visible": true,
            "variables": {
                "xVar": ["x", 0],
                "doneVar": ["done", 0],
                "daysVar": ["days", 0]
            },
            "blocks": {
                "hat": {
                    "opcode": "event_whenflagclicked",
                    "next": "setDays",
                    "topLevel": true,
                    "inputs": {},
                    "fields": {}
                },
                "setDays": {
                    "opcode": "data_setvariableto",
                    "next": "while",
                    "topLevel": false,
                    "inputs": {"VALUE": [2, "daysSince"]},
                    "fields": {"VARIABLE": ["days", "daysVar"]}
                },
                "daysSince": {
                    "opcode": "sensing_dayssince2000",
                    "next": null,
                    "topLevel": false,
                    "inputs": {},
                    "fields": {}
                },
                "while": {
                    "opcode": "control_while",
                    "next": "done",
                    "topLevel": false,
                    "inputs": {"CONDITION": [2, "condition"], "SUBSTACK": [2, "change"]},
                    "fields": {}
                },
                "condition": {
                    "opcode": "operator_lt",
                    "next": null,
                    "topLevel": false,
                    "inputs": {
                        "OPERAND1": [2, [12, "x", "xVar"]],
                        "OPERAND2": [1, [4, "3"]]
                    },
                    "fields": {}
                },
                "change": {
                    "opcode": "data_changevariableby",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "1"]]},
                    "fields": {"VARIABLE": ["x", "xVar"]}
                },
                "done": {
                    "opcode": "data_setvariableto",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "1"]]},
                    "fields": {"VARIABLE": ["done", "doneVar"]}
                }
            }
        }]
    })json";

    runtime = sjit_runtime_create();
    result = sjit::loadProjectIntoRuntime(runtime, writeSb3Fixture(while_project_json));
    require(result.ok, "while project should load");
    sjit_runtime_green_flag(runtime);

    sjit_runtime_tick(runtime);
    x = sjit_runtime_lookup_or_create_variable(runtime, 2, "x", SJIT_VAR_SCALAR);
    done = sjit_runtime_lookup_or_create_variable(runtime, 2, "done", SJIT_VAR_SCALAR);
    SVariable *days = sjit_runtime_lookup_or_create_variable(runtime, 2, "days", SJIT_VAR_SCALAR);
    require(x && x->value.tag == SJIT_VALUE_NUMBER && x->value.number == 3.0,
        "while should run until the condition becomes false");
    require(done->value.tag == SJIT_VALUE_STRING &&
        std::string(sjit_string_cstr(static_cast<const SString *>(done->value.ptr))) == "1",
        "while should continue to the following statement after completion");
    require(days && days->value.tag == SJIT_VALUE_NUMBER && days->value.number > 0.0,
        "days since 2000 should load as a numeric sensing reporter");

    sjit_runtime_destroy(runtime);

    const std::string large_procedure_repeat_project_json = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "variables": {},
            "lists": {},
            "blocks": {}
        }, {
            "isStage": false,
            "name": "BulkProcedure",
            "visible": true,
            "variables": {
                "xVar": ["x", 0],
                "doneVar": ["done", 0]
            },
            "blocks": {
                "proto": {
                    "opcode": "procedures_prototype",
                    "next": null,
                    "topLevel": false,
                    "inputs": {},
                    "fields": {},
                    "mutation": {
                        "tagName": "mutation",
                        "children": [],
                        "proccode": "bulk",
                        "argumentids": "[]",
                        "argumentnames": "[]",
                        "argumentdefaults": "[]",
                        "warp": "false"
                    }
                },
                "def": {
                    "opcode": "procedures_definition",
                    "next": "repeat",
                    "topLevel": true,
                    "inputs": {"custom_block": [2, "proto"]},
                    "fields": {}
                },
                "repeat": {
                    "opcode": "control_repeat",
                    "next": "done",
                    "topLevel": false,
                    "inputs": {"TIMES": [1, [4, "17000"]], "SUBSTACK": [2, "change"]},
                    "fields": {}
                },
                "change": {
                    "opcode": "data_changevariableby",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "1"]]},
                    "fields": {"VARIABLE": ["x", "xVar"]}
                },
                "done": {
                    "opcode": "data_setvariableto",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "1"]]},
                    "fields": {"VARIABLE": ["done", "doneVar"]}
                },
                "hat": {
                    "opcode": "event_whenflagclicked",
                    "next": "callBulk",
                    "topLevel": true,
                    "inputs": {},
                    "fields": {}
                },
                "callBulk": {
                    "opcode": "procedures_call",
                    "next": null,
                    "topLevel": false,
                    "inputs": {},
                    "fields": {},
                    "mutation": {
                        "tagName": "mutation",
                        "children": [],
                        "proccode": "bulk",
                        "argumentids": "[]",
                        "warp": "false"
                    }
                }
            }
        }]
    })json";

    runtime = sjit_runtime_create();
    result = sjit::loadProjectIntoRuntime(runtime, writeSb3Fixture(large_procedure_repeat_project_json));
    require(result.ok, "large procedure repeat project should load");
    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);

    x = sjit_runtime_lookup_or_create_variable(runtime, 2, "x", SJIT_VAR_SCALAR);
    done = sjit_runtime_lookup_or_create_variable(runtime, 2, "done", SJIT_VAR_SCALAR);
    require(x && x->value.tag == SJIT_VALUE_NUMBER && x->value.number == 17000.0,
        "large repeat inside a procedure should not restart at the loop batch boundary");
    require(done->value.tag == SJIT_VALUE_STRING &&
        std::string(sjit_string_cstr(static_cast<const SString *>(done->value.ptr))) == "1",
        "large repeat inside a procedure should continue after completion");

    sjit_runtime_destroy(runtime);
}

void test_project_loader_procedure_arguments_and_stop_return() {
    const std::string project_json = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "variables": {},
            "lists": {},
            "blocks": {}
        }, {
            "isStage": false,
            "name": "Calc",
            "visible": true,
            "variables": {
                "resultVar": ["result", 0],
                "afterVar": ["after", 0]
            },
            "lists": {},
            "blocks": {
                "proto": {
                    "opcode": "procedures_prototype",
                    "next": null,
                    "topLevel": false,
                    "inputs": {
                        "argA": [1, "argAReporter"],
                        "argB": [1, "argBReporter"]
                    },
                    "fields": {},
                    "mutation": {
                        "tagName": "mutation",
                        "children": [],
                        "proccode": "add %s %s",
                        "argumentids": "[\"argA\",\"argB\"]",
                        "argumentnames": "[\"a\",\"b\"]",
                        "argumentdefaults": "[\"\",\"\"]",
                        "warp": "true"
                    }
                },
                "argAReporter": {
                    "opcode": "argument_reporter_string_number",
                    "next": null,
                    "topLevel": false,
                    "inputs": {},
                    "fields": {"VALUE": ["a", null]}
                },
                "argBReporter": {
                    "opcode": "argument_reporter_string_number",
                    "next": null,
                    "topLevel": false,
                    "inputs": {},
                    "fields": {"VALUE": ["b", null]}
                },
                "def": {
                    "opcode": "procedures_definition",
                    "next": "setResult",
                    "topLevel": true,
                    "inputs": {"custom_block": [2, "proto"]},
                    "fields": {}
                },
                "setResult": {
                    "opcode": "data_setvariableto",
                    "next": "stopProcedure",
                    "topLevel": false,
                    "inputs": {"VALUE": [2, "sumArgs"]},
                    "fields": {"VARIABLE": ["result", "resultVar"]}
                },
                "sumArgs": {
                    "opcode": "operator_add",
                    "next": null,
                    "topLevel": false,
                    "inputs": {
                        "NUM1": [2, "argAReporter"],
                        "NUM2": [2, "argBReporter"]
                    },
                    "fields": {}
                },
                "stopProcedure": {
                    "opcode": "control_stop",
                    "next": null,
                    "topLevel": false,
                    "inputs": {},
                    "fields": {"STOP_OPTION": ["this script", null]},
                    "mutation": {"tagName": "mutation", "children": [], "hasnext": "false"}
                },
                "hat": {
                    "opcode": "event_whenflagclicked",
                    "next": "callAdd",
                    "topLevel": true,
                    "inputs": {},
                    "fields": {}
                },
                "callAdd": {
                    "opcode": "procedures_call",
                    "next": "setAfter",
                    "topLevel": false,
                    "inputs": {
                        "argA": [1, [4, "2"]],
                        "argB": [1, [4, "3"]]
                    },
                    "fields": {},
                    "mutation": {
                        "tagName": "mutation",
                        "children": [],
                        "proccode": "add %s %s",
                        "argumentids": "[\"argA\",\"argB\"]",
                        "warp": "true"
                    }
                },
                "setAfter": {
                    "opcode": "data_setvariableto",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [2, "afterExpr"]},
                    "fields": {"VARIABLE": ["after", "afterVar"]}
                },
                "afterExpr": {
                    "opcode": "operator_add",
                    "next": null,
                    "topLevel": false,
                    "inputs": {
                        "NUM1": [2, [12, "result", "resultVar"]],
                        "NUM2": [1, [4, "10"]]
                    },
                    "fields": {}
                }
            }
        }]
    })json";

    SRuntime *runtime = sjit_runtime_create();
    sjit::ProjectLoadResult result = sjit::loadProjectIntoRuntime(runtime, writeSb3Fixture(project_json));
    assert(result.ok);

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);

    SVariable *sum = sjit_runtime_lookup_or_create_variable(runtime, 2, "result", SJIT_VAR_SCALAR);
    SVariable *after = sjit_runtime_lookup_or_create_variable(runtime, 2, "after", SJIT_VAR_SCALAR);
    assert(sum && sum->value.tag == SJIT_VALUE_NUMBER && sum->value.number == 5.0);
    assert(after && after->value.tag == SJIT_VALUE_NUMBER && after->value.number == 15.0);

    sjit_runtime_destroy(runtime);
}

void test_project_loader_passes_list_reporter_to_procedure() {
    const std::string project_json = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "variables": {
                "resultVar": ["result", ""]
            },
            "lists": {
                "itemsList": ["items", ["front", "back"]]
            },
            "blocks": {
                "proto": {
                    "opcode": "procedures_prototype",
                    "next": null,
                    "topLevel": false,
                    "inputs": {
                        "argValue": [1, "argReporter"]
                    },
                    "fields": {},
                    "mutation": {
                        "tagName": "mutation",
                        "children": [],
                        "proccode": "capture %s",
                        "argumentids": "[\"argValue\"]",
                        "argumentnames": "[\"value\"]",
                        "argumentdefaults": "[\"\"]",
                        "warp": "true"
                    }
                },
                "argReporter": {
                    "opcode": "argument_reporter_string_number",
                    "next": null,
                    "topLevel": false,
                    "inputs": {},
                    "fields": {"VALUE": ["value", null]}
                },
                "def": {
                    "opcode": "procedures_definition",
                    "next": "setResult",
                    "topLevel": true,
                    "inputs": {"custom_block": [2, "proto"]},
                    "fields": {}
                },
                "setResult": {
                    "opcode": "data_setvariableto",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [2, "joinArg"]},
                    "fields": {"VARIABLE": ["result", "resultVar"]}
                },
                "joinArg": {
                    "opcode": "operator_join",
                    "next": null,
                    "topLevel": false,
                    "inputs": {
                        "STRING1": [2, "argReporter"],
                        "STRING2": [1, [10, ""]]
                    },
                    "fields": {}
                },
                "hat": {
                    "opcode": "event_whenflagclicked",
                    "next": "callCapture",
                    "topLevel": true,
                    "inputs": {},
                    "fields": {}
                },
                "callCapture": {
                    "opcode": "procedures_call",
                    "next": null,
                    "topLevel": false,
                    "inputs": {
                        "argValue": [1, [13, "items", "itemsList"]]
                    },
                    "fields": {},
                    "mutation": {
                        "tagName": "mutation",
                        "children": [],
                        "proccode": "capture %s",
                        "argumentids": "[\"argValue\"]",
                        "warp": "true"
                    }
                }
            }
        }]
    })json";

    SRuntime *runtime = sjit_runtime_create();
    sjit::ProjectLoadResult result = sjit::loadProjectIntoRuntime(runtime, writeSb3Fixture(project_json));
    require(result.ok, "load list reporter procedure fixture");

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);

    SVariable *captured = sjit_runtime_lookup_or_create_variable(runtime, 1, "result", SJIT_VAR_SCALAR);
    SValue expected = sjit_make_string("front back");
    require(captured && captured->value.tag == SJIT_VALUE_STRING, "list reporter procedure result is text");
    require(sjit_compare(runtime, captured->value, expected) == 0, "list reporter passes list contents");

    sjit_value_destroy(expected);
    sjit_runtime_destroy(runtime);
}

void test_project_loader_sprite_click_hat() {
    const std::string project_json = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "variables": {},
            "lists": {},
            "blocks": {}
        }, {
            "isStage": false,
            "name": "Button",
            "visible": true,
            "variables": {
                "clickedVar": ["clicked", 0]
            },
            "blocks": {
                "clickHat": {
                    "opcode": "event_whenthisspriteclicked",
                    "next": "setClicked",
                    "topLevel": true,
                    "inputs": {},
                    "fields": {}
                },
                "setClicked": {
                    "opcode": "data_setvariableto",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "1"]]},
                    "fields": {"VARIABLE": ["clicked", "clickedVar"]}
                }
            }
        }]
    })json";

    SRuntime *runtime = sjit_runtime_create();
    sjit::ProjectLoadResult result = sjit::loadProjectIntoRuntime(runtime, writeSb3Fixture(project_json));
    require(result.ok, "sprite click project should load");

    SHostInputSnapshot input{};
    input.sprite_clicked_id = 2;
    sjit_runtime_set_input(runtime, &input);
    sjit_runtime_tick(runtime);

    SVariable *clicked = sjit_runtime_lookup_or_create_variable(runtime, 2, "clicked", SJIT_VAR_SCALAR);
    require(clicked && clicked->value.tag == SJIT_VALUE_STRING &&
        std::string(sjit_string_cstr(static_cast<const SString *>(clicked->value.ptr))) == "1",
        "sprite click input should start event_whenthisspriteclicked hats");

    sjit_runtime_destroy(runtime);
}

void test_deep_procedure_recursion_uses_dynamic_argument_frames() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "Recursive", 0);
    require(sprite, "create recursive sprite");

    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 2);
    require(script, "create recursive script");
    script->procedure_count = 2;
    script->procedures = static_cast<SCompiledProcedure *>(std::calloc(2, sizeof(SCompiledProcedure)));
    require(script->procedures, "allocate recursive procedure");

    script->statements[0].opcode = SJIT_STMT_PROCEDURE_CALL;
    script->statements[0].procedure_name = sjit_string_new("walk %s %s");
    script->statements[0].argument_count = 2;
    script->statements[0].arguments = static_cast<SArgumentExpr *>(std::calloc(2, sizeof(SArgumentExpr)));
    require(script->statements[0].arguments, "allocate first recursive entry arguments");
    script->statements[0].arguments[0].name = sjit_string_new("n");
    script->statements[0].arguments[0].value = literalNumber(300.0);
    script->statements[0].arguments[1].name = sjit_string_new("stopAt");
    script->statements[0].arguments[1].value = literalNumber(250.0);

    script->statements[1].opcode = SJIT_STMT_PROCEDURE_CALL;
    script->statements[1].procedure_name = sjit_string_new("walk %s %s");
    script->statements[1].argument_count = 2;
    script->statements[1].arguments = static_cast<SArgumentExpr *>(std::calloc(2, sizeof(SArgumentExpr)));
    require(script->statements[1].arguments, "allocate second recursive entry arguments");
    script->statements[1].arguments[0].name = sjit_string_new("n");
    script->statements[1].arguments[0].value = literalNumber(300.0);
    script->statements[1].arguments[1].name = sjit_string_new("stopAt");
    script->statements[1].arguments[1].value = literalNumber(-1.0);

    SCompiledProcedure &procedure = script->procedures[0];
    procedure.name = sjit_string_new("walk %s %s");
    procedure.warp_mode = 1;
    procedure.argument_count = 2;
    procedure.argument_names = static_cast<SString **>(std::calloc(2, sizeof(SString *)));
    require(procedure.argument_names, "allocate recursive procedure argument names");
    procedure.argument_names[0] = sjit_string_new("n");
    procedure.argument_names[1] = sjit_string_new("stopAt");
    procedure.statement_count = 3;
    procedure.statements = static_cast<SStatement *>(std::calloc(3, sizeof(SStatement)));
    require(procedure.statements, "allocate recursive procedure body");

    procedure.statements[0].opcode = SJIT_STMT_IF;
    procedure.statements[0].condition = sjit_expr_create_binary(
        SJIT_EXPR_EQ,
        sjit_expr_create_argument("n"),
        sjit_expr_create_argument("stopAt"));
    procedure.statements[0].substack_count = 1;
    procedure.statements[0].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(procedure.statements[0].substack, "allocate stop branch");
    procedure.statements[0].substack[0].opcode = SJIT_STMT_STOP_THIS_SCRIPT;

    procedure.statements[1].opcode = SJIT_STMT_CHANGE_VARIABLE;
    procedure.statements[1].variable_name = sjit_string_new("visits");
    procedure.statements[1].value = literalNumber(1.0);

    SStatement &branch = procedure.statements[2];
    branch.opcode = SJIT_STMT_IF;
    branch.condition = sjit_expr_create_binary(SJIT_EXPR_GT, sjit_expr_create_argument("n"), literalNumber(0.0));
    branch.substack_count = 1;
    branch.substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(branch.substack, "allocate recursive call branch");
    branch.substack[0].opcode = SJIT_STMT_PROCEDURE_CALL;
    branch.substack[0].procedure_name = sjit_string_new("walk %s %s");
    branch.substack[0].argument_count = 2;
    branch.substack[0].arguments = static_cast<SArgumentExpr *>(std::calloc(2, sizeof(SArgumentExpr)));
    require(branch.substack[0].arguments, "allocate recursive call arguments");
    branch.substack[0].arguments[0].name = sjit_string_new("n");
    branch.substack[0].arguments[0].value = sjit_expr_create_binary(
        SJIT_EXPR_SUB,
        sjit_expr_create_argument("n"),
        literalNumber(1.0));
    branch.substack[0].arguments[1].name = sjit_string_new("stopAt");
    branch.substack[0].arguments[1].value = sjit_expr_create_argument("stopAt");

    SFrame frame;
    sjit_frame_init(&frame);
    SRuntimeStatus status = sjit_script_execute_statement_with_frame(runtime, script, 0, &frame);
    require(status == SJIT_STATUS_OK, "deep recursive procedure should handle stop return");
    require(frame.loop_state_count == 0, "procedure return should clear scoped control state");
    status = sjit_script_execute_statement_with_frame(runtime, script, 1, &frame);
    require(status == SJIT_STATUS_OK, "deep recursive procedure should complete after prior stop return");
    require(frame.loop_state_count == 0, "second procedure return should clear scoped control state");

    SVariable *visits = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "visits", SJIT_VAR_SCALAR);
    require(
        visits && visits->value.tag == SJIT_VALUE_NUMBER && visits->value.number == 351.0,
        "deep recursive procedure should not reuse stale stop-branch state");

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

uint64_t nativePenRowDoubleBits(double value) {
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

struct NativePenRowValueSnapshot {
    int tag = SJIT_VALUE_NULL;
    uint64_t number_bits = 0;
    std::string text;

    bool operator==(const NativePenRowValueSnapshot &) const = default;
};

NativePenRowValueSnapshot snapshotNativePenRowValue(SValue value) {
    NativePenRowValueSnapshot result;
    result.tag = value.tag;
    if (value.tag == SJIT_VALUE_NUMBER || value.tag == SJIT_VALUE_BOOL) {
        result.number_bits = nativePenRowDoubleBits(value.number);
    } else if (value.tag == SJIT_VALUE_STRING && value.ptr) {
        const auto *string = static_cast<const SString *>(value.ptr);
        if (string->bytes && string->length > 0) {
            result.text.assign(
                string->bytes,
                static_cast<std::size_t>(string->length));
        }
    } else if (value.tag == SJIT_VALUE_LIST && value.ptr) {
        SValue contents = sjit_list_contents(static_cast<SList *>(value.ptr));
        const auto *string = contents.tag == SJIT_VALUE_STRING ?
            static_cast<const SString *>(contents.ptr) : nullptr;
        if (string && string->bytes && string->length > 0) {
            result.text.assign(
                string->bytes,
                static_cast<std::size_t>(string->length));
        }
        sjit_value_destroy(contents);
    }
    return result;
}

std::vector<NativePenRowValueSnapshot> snapshotNativePenRowList(const SList *list) {
    std::vector<NativePenRowValueSnapshot> result;
    const int length = sjit_list_length(list);
    result.reserve(static_cast<std::size_t>(length));
    for (int index = 1; index <= length; ++index) {
        SValue value = sjit_list_get(list, index);
        result.push_back(snapshotNativePenRowValue(value));
        sjit_value_destroy(value);
    }
    return result;
}

struct NativePenRowDrawSnapshot {
    int kind = 0;
    int target_id = 0;
    int drawable_id = 0;
    int costume_id = 0;
    uint64_t x = 0;
    uint64_t y = 0;
    uint64_t x2 = 0;
    uint64_t y2 = 0;
    uint64_t direction = 0;
    uint64_t size = 0;
    uint64_t pen_width = 0;
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 0;
    int visible = 0;
    int layer = 0;

    bool operator==(const NativePenRowDrawSnapshot &) const = default;
};

NativePenRowDrawSnapshot snapshotNativePenRowDraw(const SDrawCommand &command) {
    NativePenRowDrawSnapshot result;
    result.kind = command.kind;
    result.target_id = command.target_id;
    result.drawable_id = command.drawable_id;
    result.costume_id = command.costume_id;
    result.x = nativePenRowDoubleBits(command.x);
    result.y = nativePenRowDoubleBits(command.y);
    result.x2 = nativePenRowDoubleBits(command.x2);
    result.y2 = nativePenRowDoubleBits(command.y2);
    result.direction = nativePenRowDoubleBits(command.direction);
    result.size = nativePenRowDoubleBits(command.size);
    result.pen_width = nativePenRowDoubleBits(command.pen_width);
    result.r = command.r;
    result.g = command.g;
    result.b = command.b;
    result.a = command.a;
    result.visible = command.visible;
    result.layer = command.layer;
    return result;
}

struct NativePenRowSnapshot {
    NativePenRowValueSnapshot row;
    NativePenRowValueSnapshot column;
    NativePenRowValueSnapshot index;
    int row_scalar_kind = SJIT_SCALAR_DYNAMIC;
    int column_scalar_kind = SJIT_SCALAR_DYNAMIC;
    int index_scalar_kind = SJIT_SCALAR_DYNAMIC;
    std::vector<NativePenRowValueSnapshot> colors;
    std::vector<NativePenRowValueSnapshot> brightness;
    uint64_t sprite_x = 0;
    uint64_t sprite_y = 0;
    uint64_t sprite_direction = 0;
    uint64_t sprite_size = 0;
    int sprite_visible = 0;
    int pen_down = 0;
    uint64_t pen_size = 0;
    int pen_r = 0;
    int pen_g = 0;
    int pen_b = 0;
    int pen_a = 0;
    uint64_t pen_hue = 0;
    uint64_t pen_saturation = 0;
    uint64_t pen_brightness = 0;
    uint64_t pen_transparency = 0;
    int redraw_requested = 0;
    int pen_revision = 0;
    std::vector<NativePenRowDrawSnapshot> pen_path;

    bool operator==(const NativePenRowSnapshot &) const = default;
};

void pushNativePenRowValue(SList *list, SValue value) {
    const int pushed = sjit_list_push(list, value);
    sjit_value_destroy(value);
    require(pushed, "populate native pen-row list");
}

struct NativePenRowFixture {
    SRuntime *runtime = nullptr;
    SSprite *sprite = nullptr;
    SVariable *colors_variable = nullptr;
    SVariable *brightness_variable = nullptr;
    SVariable *row_variable = nullptr;
    SVariable *column_variable = nullptr;
    SVariable *index_variable = nullptr;
    SExpr *replacement = nullptr;
    SExpr *alternate_replacement = nullptr;
    SList *shared_color_alias = nullptr;

    explicit NativePenRowFixture(bool mixed_values) {
        runtime = sjit_runtime_create();
        sprite = sjit_runtime_create_sprite(runtime, "NativePenRow", 0);
        require(runtime && sprite, "create native pen-row fixture");

        require(
            sjit_runtime_lookup_or_create_variable(
                runtime, sprite->base.id, "colors", SJIT_VAR_LIST),
            "create native pen-row color list");
        require(
            sjit_runtime_lookup_or_create_variable(
                runtime, sprite->base.id, "brightness", SJIT_VAR_LIST),
            "create native pen-row brightness list");
        require(
            sjit_runtime_lookup_or_create_variable(
                runtime, sprite->base.id, "row", SJIT_VAR_SCALAR),
            "create native pen-row row variable");
        require(
            sjit_runtime_lookup_or_create_variable(
                runtime, sprite->base.id, "column", SJIT_VAR_SCALAR),
            "create native pen-row column variable");
        require(
            sjit_runtime_lookup_or_create_variable(
                runtime, sprite->base.id, "index", SJIT_VAR_SCALAR),
            "create native pen-row index variable");

        /* The target variable array may have moved while the fixture was
           being populated, so only retain handles after all creations. */
        colors_variable = sjit_runtime_lookup_or_create_variable(
            runtime, sprite->base.id, "colors", SJIT_VAR_LIST);
        brightness_variable = sjit_runtime_lookup_or_create_variable(
            runtime, sprite->base.id, "brightness", SJIT_VAR_LIST);
        row_variable = sjit_runtime_lookup_or_create_variable(
            runtime, sprite->base.id, "row", SJIT_VAR_SCALAR);
        column_variable = sjit_runtime_lookup_or_create_variable(
            runtime, sprite->base.id, "column", SJIT_VAR_SCALAR);
        index_variable = sjit_runtime_lookup_or_create_variable(
            runtime, sprite->base.id, "index", SJIT_VAR_SCALAR);
        require(
            colors_variable && brightness_variable && row_variable &&
                column_variable && index_variable,
            "resolve stable native pen-row handles");

        SList *colors = colorList();
        SList *brightness = brightnessList();
        require(colors && brightness, "resolve native pen-row list storage");
        if (mixed_values) {
            for (int index = 0; index < 4; ++index) {
                pushNativePenRowValue(colors, sjit_make_number(1000.0 + index));
                pushNativePenRowValue(brightness, sjit_make_number(0.0));
            }
            pushNativePenRowValue(colors, sjit_make_number(2147549699.0));
            pushNativePenRowValue(brightness, sjit_make_number(-17.25));
            pushNativePenRowValue(colors, sjit_make_string("n"));
            pushNativePenRowValue(brightness, sjit_make_bool(1));
            pushNativePenRowValue(colors, sjit_make_string("#0fa"));
            pushNativePenRowValue(brightness, sjit_make_string("not a number"));
            pushNativePenRowValue(colors, sjit_make_string("13154480"));
            pushNativePenRowValue(brightness, sjit_make_string("-10.5"));
        } else {
            for (int index = 0; index < 8; ++index) {
                pushNativePenRowValue(
                    colors,
                    sjit_make_number(2147549184.0 + static_cast<double>(index * 257)));
                pushNativePenRowValue(
                    brightness,
                    sjit_make_number(-20.0 + static_cast<double>(index * 5)));
            }
        }

        sjit_variable_set_number(row_variable, 1.0);
        sjit_variable_set_number(column_variable, 0.0);
        sjit_variable_set_number(index_variable, -77.0);
        sprite->x = -2.0;
        sprite->y = 3.0;
        sprite->pen_size = 2.5;
        replacement = literalString("n");
        require(replacement, "create native pen-row replacement marker");
        runtime->redraw_requested = 0;
    }

    ~NativePenRowFixture() {
        sjit_list_destroy(shared_color_alias);
        sjit_expr_destroy(alternate_replacement);
        sjit_expr_destroy(replacement);
        sjit_runtime_destroy(runtime);
    }

    NativePenRowFixture(const NativePenRowFixture &) = delete;
    NativePenRowFixture &operator=(const NativePenRowFixture &) = delete;

    SList *colorList() const {
        return colors_variable && colors_variable->value.tag == SJIT_VALUE_LIST ?
            static_cast<SList *>(colors_variable->value.ptr) : nullptr;
    }

    SList *brightnessList() const {
        return brightness_variable && brightness_variable->value.tag == SJIT_VALUE_LIST ?
            static_cast<SList *>(brightness_variable->value.ptr) : nullptr;
    }
};

struct NativePenRowCall {
    SVariable *colors = nullptr;
    SVariable *brightness = nullptr;
    SVariable *row = nullptr;
    SVariable *column = nullptr;
    SVariable *index = nullptr;
    double columns = 4.0;
    double x_step = 1.25;
    SExpr *replacement = nullptr;
    int param_id = 3;
};

NativePenRowCall defaultNativePenRowCall(NativePenRowFixture &fixture) {
    NativePenRowCall call;
    call.colors = fixture.colors_variable;
    call.brightness = fixture.brightness_variable;
    call.row = fixture.row_variable;
    call.column = fixture.column_variable;
    call.index = fixture.index_variable;
    call.replacement = fixture.replacement;
    return call;
}

int executeNativePenRow(NativePenRowFixture &fixture, const NativePenRowCall &call) {
    return sjit_jit_pen_render_row_from_variables(
        fixture.runtime,
        fixture.sprite,
        call.colors,
        call.brightness,
        call.row,
        call.column,
        call.index,
        call.columns,
        call.x_step,
        call.replacement,
        call.param_id);
}

NativePenRowSnapshot snapshotNativePenRow(const NativePenRowFixture &fixture) {
    NativePenRowSnapshot result;
    result.row = snapshotNativePenRowValue(fixture.row_variable->value);
    result.column = snapshotNativePenRowValue(fixture.column_variable->value);
    result.index = snapshotNativePenRowValue(fixture.index_variable->value);
    result.row_scalar_kind = fixture.row_variable->scalar_kind;
    result.column_scalar_kind = fixture.column_variable->scalar_kind;
    result.index_scalar_kind = fixture.index_variable->scalar_kind;
    result.colors = snapshotNativePenRowList(fixture.colorList());
    result.brightness = snapshotNativePenRowList(fixture.brightnessList());
    result.sprite_x = nativePenRowDoubleBits(fixture.sprite->x);
    result.sprite_y = nativePenRowDoubleBits(fixture.sprite->y);
    result.sprite_direction = nativePenRowDoubleBits(fixture.sprite->direction);
    result.sprite_size = nativePenRowDoubleBits(fixture.sprite->size);
    result.sprite_visible = fixture.sprite->visible;
    result.pen_down = fixture.sprite->pen_down;
    result.pen_size = nativePenRowDoubleBits(fixture.sprite->pen_size);
    result.pen_r = fixture.sprite->pen_r;
    result.pen_g = fixture.sprite->pen_g;
    result.pen_b = fixture.sprite->pen_b;
    result.pen_a = fixture.sprite->pen_a;
    result.pen_hue = nativePenRowDoubleBits(fixture.sprite->pen_hue);
    result.pen_saturation = nativePenRowDoubleBits(fixture.sprite->pen_saturation);
    result.pen_brightness = nativePenRowDoubleBits(fixture.sprite->pen_brightness);
    result.pen_transparency = nativePenRowDoubleBits(fixture.sprite->pen_transparency);
    result.redraw_requested = fixture.runtime->redraw_requested;
    result.pen_revision = fixture.runtime->pen.revision;
    const int pen_count = sjit_runtime_pen_path_count(fixture.runtime);
    const SDrawCommand *pen_path = sjit_runtime_pen_path_data(fixture.runtime);
    require(
        pen_count == 0 || pen_path,
        "native pen-row public path data materializes every logical stamp");
    result.pen_path.reserve(static_cast<std::size_t>(pen_count));
    for (int index = 0; index < pen_count; ++index) {
        result.pen_path.push_back(
            snapshotNativePenRowDraw(pen_path[index]));
    }
    return result;
}

void executeSafePenRow(
    NativePenRowFixture &fixture,
    int columns,
    double x_step,
    int param_id) {
    const double row = sjit_to_number_fast(
        fixture.runtime,
        fixture.row_variable->value);
    double column = sjit_to_number_fast(
        fixture.runtime,
        fixture.column_variable->value);
    for (int offset = 0; offset < columns; ++offset) {
        const double index_number =
            (row * static_cast<double>(columns) + column) + 1.0;
        sjit_variable_set_number(fixture.index_variable, index_number);
        SValue color = sjit_list_get(
            fixture.colorList(),
            static_cast<int>(index_number));
        const int is_marker = sjit_eq(
            fixture.runtime,
            color,
            fixture.replacement->literal);
        sjit_value_destroy(color);
        if (!is_marker) {
            sjit_jit_pen_render_list_pixel_from_variables(
                fixture.runtime,
                fixture.sprite,
                fixture.colors_variable,
                fixture.brightness_variable,
                fixture.index_variable,
                fixture.replacement,
                param_id);
        }
        sjit_sprite_set_xy(
            fixture.runtime,
            fixture.sprite,
            fixture.sprite->x + x_step,
            fixture.sprite->y,
            0);
        sjit_variable_change_by_number(
            fixture.runtime,
            fixture.column_variable,
            1.0);
        column += 1.0;
    }
}

void fillNativePenRowInitialPenCapacity(NativePenRowFixture &fixture) {
    for (int index = 0; index < SJIT_INITIAL_CAPACITY; ++index) {
        sjit_pen_stamp(fixture.runtime, fixture.sprite);
    }
    require(
        fixture.runtime->pen.length == fixture.runtime->pen.capacity,
        "native pen-row fixture fills the initial pen capacity");
    fixture.runtime->redraw_requested = 0;
}

void test_native_pen_row_matches_safe_pixel_loop() {
    for (const bool mixed_values : {false, true}) {
        NativePenRowFixture native(mixed_values);
        NativePenRowFixture reference(mixed_values);
        fillNativePenRowInitialPenCapacity(native);
        fillNativePenRowInitialPenCapacity(reference);

        const NativePenRowCall call = defaultNativePenRowCall(native);
        require(
            executeNativePenRow(native, call) == 1,
            "native pen-row kernel accepts a guarded row");
        executeSafePenRow(reference, 4, call.x_step, call.param_id);

        if (!(snapshotNativePenRow(native) == snapshotNativePenRow(reference))) {
            throw std::runtime_error(
                mixed_values ?
                    "native pen-row mixed-value result differs from safe pixel loop" :
                    "native pen-row numeric result differs from safe pixel loop");
        }
        const int expected_stamps = mixed_values ? 3 : 4;
        require(
            native.runtime->pen.length == SJIT_INITIAL_CAPACITY + expected_stamps,
            "native pen-row appends exactly the non-marker pixels");
        require(
            native.runtime->pen.capacity > SJIT_INITIAL_CAPACITY,
            "native pen-row safely grows a full pen command buffer");
    }

    NativePenRowFixture native_zero(false);
    NativePenRowFixture reference_zero(false);
    NativePenRowCall zero_call = defaultNativePenRowCall(native_zero);
    zero_call.columns = 0.0;
    require(
        executeNativePenRow(native_zero, zero_call) == 1,
        "native pen-row accepts a zero-length row as a no-op");
    executeSafePenRow(reference_zero, 0, zero_call.x_step, zero_call.param_id);
    require(
        snapshotNativePenRow(native_zero) == snapshotNativePenRow(reference_zero),
        "native zero-length row matches the safe no-op");
}

enum class NativePenTileMode {
    OpaqueMixed,
    MarkerOnly,
    HexColor,
    TranslucentColor
};

void configureNativePenTileRow(
    NativePenRowFixture &fixture,
    NativePenTileMode mode) {
    SList *colors = fixture.colorList();
    SList *brightness = fixture.brightnessList();
    require(colors && brightness, "resolve compact pen-tile lists");
    sjit_list_clear(colors);
    sjit_list_clear(brightness);
    for (int column = 0; column < SJIT_PEN_RASTER_TILE_WIDTH; ++column) {
        if (mode == NativePenTileMode::MarkerOnly ||
            (mode == NativePenTileMode::OpaqueMixed &&
             (column == 17 || column == 401))) {
            pushNativePenRowValue(colors, sjit_make_string("n"));
        } else if (mode == NativePenTileMode::HexColor && column == 0) {
            pushNativePenRowValue(colors, sjit_make_string("#0fa"));
        } else if (mode == NativePenTileMode::TranslucentColor && column == 0) {
            pushNativePenRowValue(colors, sjit_make_number(2147549699.0));
        } else if (mode == NativePenTileMode::OpaqueMixed && column == 99) {
            pushNativePenRowValue(colors, sjit_make_string("not a number"));
        } else {
            const int packed = 0x102030 + (column % 2) * 0x8000 +
                ((column % 13) << 8);
            if ((column & 1) == 0) {
                const std::string text = std::to_string(packed);
                pushNativePenRowValue(colors, sjit_make_string(text.c_str()));
            } else {
                pushNativePenRowValue(
                    colors,
                    sjit_make_number(static_cast<double>(packed)));
            }
        }
        if ((column % 3) == 0) {
            const std::string delta = std::to_string((column % 41) - 20);
            pushNativePenRowValue(brightness, sjit_make_string(delta.c_str()));
        } else {
            pushNativePenRowValue(
                brightness,
                sjit_make_number(static_cast<double>((column % 41) - 20)));
        }
    }
    sjit_variable_set_number(fixture.row_variable, 0.0);
    sjit_variable_set_number(fixture.column_variable, 0.0);
    sjit_variable_set_number(fixture.index_variable, -77.0);
    fixture.sprite->x = -239.5;
    fixture.sprite->y = -179.5;
    fixture.sprite->pen_down = 0;
    fixture.sprite->pen_size = 1.0;
    sjit_pen_clear(fixture.runtime);
    fixture.runtime->redraw_requested = 0;
}

void requireCompactTilePixelsMatchMaterializedPath(
    NativePenRowFixture &fixture) {
    const SPenRasterTile &tile = fixture.runtime->pen_raster_tile;
    const int count = sjit_runtime_pen_path_count(fixture.runtime);
    const SDrawCommand *path = sjit_runtime_pen_path_data(fixture.runtime);
    require(tile.active && tile.pixels && tile.active_bits,
        "compact pen tile owns bounded RGBA and active-mask storage");
    require(count == 0 || path,
        "compact pen tile lazily materializes its logical public path");
    for (int index = 0; index < count; ++index) {
        const SDrawCommand &command = path[index];
        const int column = static_cast<int>(std::floor(command.x + 240.0));
        const int pixel_y = static_cast<int>(std::floor(180.0 - command.y));
        require(column >= 0 && column < tile.width &&
                pixel_y >= 0 && pixel_y < tile.height,
            "materialized compact command maps inside the 480x360 tile");
        const int logical_row = tile.height - 1 - pixel_y;
        const int logical_index = logical_row * tile.width + column;
        require(
            (tile.active_bits[logical_index >> 3] &
             static_cast<unsigned char>(1u << (logical_index & 7))) != 0,
            "materialized compact command retains its active-mask bit");
        const unsigned char *pixel = tile.pixels +
            static_cast<std::size_t>(pixel_y * tile.stride + column * 4);
        require(command.r == pixel[0] && command.g == pixel[1] &&
                command.b == pixel[2] && command.a == pixel[3],
            "compact RGBA bytes exactly match materialized pen colors");
    }
}

void requireCompactTilePixelsMatchSafePath(
    const NativePenRowFixture &compact,
    NativePenRowFixture &reference) {
    std::vector<unsigned char> expected(
        SJIT_PEN_RASTER_TILE_PIXEL_BYTES,
        static_cast<unsigned char>(0));
    const int count = sjit_runtime_pen_path_count(reference.runtime);
    const SDrawCommand *path = sjit_runtime_pen_path_data(reference.runtime);
    require(count == 0 || path, "safe pen path is available for tile comparison");
    for (int index = 0; index < count; ++index) {
        const SDrawCommand &command = path[index];
        const int x = static_cast<int>(std::floor(command.x + 240.0));
        const int y = static_cast<int>(std::floor(180.0 - command.y));
        require(x >= 0 && x < SJIT_PEN_RASTER_TILE_WIDTH &&
                y >= 0 && y < SJIT_PEN_RASTER_TILE_HEIGHT &&
                command.x == command.x2 && command.y == command.y2 &&
                command.pen_width ==
                    compact.runtime->pen_raster_tile.pen_width &&
                std::lround(std::max(1.0, command.pen_width)) / 2 == 0 &&
                command.a == 255,
            "safe strict-row command is one opaque in-bounds pixel");
        unsigned char *pixel = expected.data() +
            static_cast<std::size_t>(
                (y * SJIT_PEN_RASTER_TILE_WIDTH + x) * 4);
        require(pixel[3] == 0, "safe strict-row pixels do not overlap");
        pixel[0] = static_cast<unsigned char>(command.r);
        pixel[1] = static_cast<unsigned char>(command.g);
        pixel[2] = static_cast<unsigned char>(command.b);
        pixel[3] = static_cast<unsigned char>(command.a);
    }
    require(
        std::memcmp(
            compact.runtime->pen_raster_tile.pixels,
            expected.data(),
            expected.size()) == 0,
        "compact RGBA tile exactly matches safe dense pixel rasterization");
}

void test_native_pen_raster_tile_matches_safe_path_and_ordering() {
    NativePenRowFixture native(true);
    NativePenRowFixture reference(true);
    configureNativePenTileRow(native, NativePenTileMode::OpaqueMixed);
    configureNativePenTileRow(reference, NativePenTileMode::OpaqueMixed);

    NativePenRowCall call = defaultNativePenRowCall(native);
    call.columns = SJIT_PEN_RASTER_TILE_WIDTH;
    call.x_step = 1.0;
    require(executeNativePenRow(native, call) == 1,
        "strict 480-pixel row executes through the native helper");
    executeSafePenRow(
        reference,
        SJIT_PEN_RASTER_TILE_WIDTH,
        call.x_step,
        call.param_id);

    require(native.runtime->pen_raster_tile.active == 1 &&
            native.runtime->pen_raster_tile.rows_filled == 1,
        "strict grid row activates one compact raster-tile row");
    require(native.runtime->pen.length == 0 &&
            native.runtime->pen.capacity < SJIT_PEN_RASTER_TILE_WIDTH,
        "compact row does not allocate one SDrawCommand per pixel");
    require(
        sjit_runtime_pen_path_count(native.runtime) ==
            SJIT_PEN_RASTER_TILE_WIDTH - 2,
        "marker holes are excluded from the logical pen command count");
    requireCompactTilePixelsMatchMaterializedPath(native);
    requireCompactTilePixelsMatchSafePath(native, reference);
    const SDrawCommand *first_materialized =
        sjit_runtime_pen_path_data(native.runtime);
    require(first_materialized == sjit_runtime_pen_path_data(native.runtime),
        "public materialized pen data is stable until the next path mutation");
    require(
        snapshotNativePenRow(native) == snapshotNativePenRow(reference),
        "compact raster row exactly matches safe row state and public path");

    native.sprite->x = reference.sprite->x = 12.25;
    native.sprite->y = reference.sprite->y = -7.5;
    sjit_pen_stamp(native.runtime, native.sprite);
    sjit_pen_stamp(reference.runtime, reference.sprite);
    require(native.runtime->pen.length == 1 &&
            native.runtime->pen_materialized_valid == 0,
        "ordinary stamp remains an ordered tail and invalidates materialization");
    require(
        snapshotNativePenRow(native) == snapshotNativePenRow(reference),
        "tile followed by an ordinary stamp preserves exact public ordering");

    require(sjit_runtime_pen_path_data(native.runtime) &&
            native.runtime->pen_materialized_valid == 1,
        "ordered tile tail can be lazily rematerialized");
    native.sprite->pen_down = reference.sprite->pen_down = 1;
    sjit_sprite_set_xy(native.runtime, native.sprite, 13.25, -6.5, 0);
    sjit_sprite_set_xy(reference.runtime, reference.sprite, 13.25, -6.5, 0);
    require(native.runtime->pen.length == 2 &&
            native.runtime->pen_materialized_valid == 0,
        "pen-down motion appends after the tile and invalidates materialization");
    require(
        snapshotNativePenRow(native) == snapshotNativePenRow(reference),
        "tile followed by pen-down motion preserves exact public ordering");

    sjit_runtime_tick(native.runtime);
    const SDrawCommandBuffer *draw =
        sjit_runtime_get_draw_commands(native.runtime);
    require(draw && draw->length == 4 &&
            native.runtime->draw_pen_length == 3 &&
            draw->items[0].kind == SJIT_DRAW_PEN_RASTER_TILE &&
            draw->items[1].kind == SJIT_DRAW_PEN_STROKE &&
            draw->items[2].kind == SJIT_DRAW_PEN_STROKE &&
            draw->items[3].kind == SJIT_DRAW_SPRITE,
        "draw buffer orders compact tile, ordinary tail, then sprite sentinel");

    const unsigned char *pixels = native.runtime->pen_raster_tile.pixels;
    const int revision = sjit_runtime_pen_path_revision(native.runtime);
    sjit_pen_clear(native.runtime);
    sjit_pen_clear(reference.runtime);
    require(!native.runtime->pen_raster_tile.active &&
            sjit_runtime_pen_path_count(native.runtime) == 0 &&
            sjit_runtime_pen_path_revision(native.runtime) == revision + 1,
        "pen clear invalidates tile history and increments revision once");
    configureNativePenTileRow(native, NativePenTileMode::OpaqueMixed);
    call = defaultNativePenRowCall(native);
    call.columns = SJIT_PEN_RASTER_TILE_WIDTH;
    call.x_step = 1.0;
    require(executeNativePenRow(native, call) == 1 &&
            native.runtime->pen_raster_tile.pixels == pixels,
        "a later clear-bounded tile reuses the bounded RGBA allocation");

    NativePenRowFixture rounded_native(true);
    NativePenRowFixture rounded_reference(true);
    configureNativePenTileRow(rounded_native, NativePenTileMode::OpaqueMixed);
    configureNativePenTileRow(rounded_reference, NativePenTileMode::OpaqueMixed);
    const double rounded_single_pixel_width = std::sqrt(2.0);
    rounded_native.sprite->pen_size = rounded_single_pixel_width;
    rounded_reference.sprite->pen_size = rounded_single_pixel_width;
    NativePenRowCall rounded_call = defaultNativePenRowCall(rounded_native);
    rounded_call.columns = SJIT_PEN_RASTER_TILE_WIDTH;
    rounded_call.x_step = 1.0;
    require(executeNativePenRow(rounded_native, rounded_call) == 1,
        "a sub-1.5 pen width uses the same single-pixel compact raster");
    executeSafePenRow(
        rounded_reference,
        SJIT_PEN_RASTER_TILE_WIDTH,
        rounded_call.x_step,
        rounded_call.param_id);
    require(rounded_native.runtime->pen_raster_tile.active &&
            rounded_native.runtime->pen_raster_tile.pen_width ==
                rounded_single_pixel_width,
        "compact tile preserves the exact rounded single-pixel pen width");
    requireCompactTilePixelsMatchSafePath(rounded_native, rounded_reference);
    require(
        snapshotNativePenRow(rounded_native) ==
            snapshotNativePenRow(rounded_reference),
        "rounded single-pixel compact raster exactly matches safe execution");
}

void test_native_pen_raster_tile_marker_and_strict_fallbacks() {
    NativePenRowFixture marker_native(false);
    NativePenRowFixture marker_reference(false);
    configureNativePenTileRow(marker_native, NativePenTileMode::MarkerOnly);
    configureNativePenTileRow(marker_reference, NativePenTileMode::MarkerOnly);
    NativePenRowCall marker_call = defaultNativePenRowCall(marker_native);
    marker_call.columns = SJIT_PEN_RASTER_TILE_WIDTH;
    marker_call.x_step = 1.0;
    require(executeNativePenRow(marker_native, marker_call) == 1,
        "marker-only strict row executes in the compact representation");
    executeSafePenRow(
        marker_reference,
        SJIT_PEN_RASTER_TILE_WIDTH,
        1.0,
        marker_call.param_id);
    require(marker_native.runtime->pen_raster_tile.active &&
            marker_native.runtime->pen_raster_tile.command_count == 0 &&
            sjit_runtime_pen_path_count(marker_native.runtime) == 0,
        "marker-only compact row records no logical strokes");
    require(
        snapshotNativePenRow(marker_native) ==
            snapshotNativePenRow(marker_reference),
        "marker-only compact row preserves final sprite and list state");

    for (NativePenTileMode mode : {
             NativePenTileMode::HexColor,
             NativePenTileMode::TranslucentColor}) {
        NativePenRowFixture guarded(false);
        configureNativePenTileRow(guarded, mode);
        NativePenRowCall guarded_call = defaultNativePenRowCall(guarded);
        guarded_call.columns = SJIT_PEN_RASTER_TILE_WIDTH;
        guarded_call.x_step = 1.0;
        require(executeNativePenRow(guarded, guarded_call) == 1,
            "non-tile color still executes through the safe native row");
        require(!guarded.runtime->pen_raster_tile.active &&
                guarded.runtime->pen.length > 0,
            "hex or translucent color strictly falls back to ordinary commands");
    }
}

void test_native_pen_row_guards_are_non_mutating() {
    const auto require_rejected = [](
        const char *guard_name,
        auto configure) {
        NativePenRowFixture fixture(false);
        NativePenRowCall call = defaultNativePenRowCall(fixture);
        configure(fixture, call);
        const NativePenRowSnapshot before = snapshotNativePenRow(fixture);
        if (executeNativePenRow(fixture, call) != 0) {
            throw std::runtime_error(
                std::string("native pen-row unexpectedly accepted guard case: ") +
                guard_name);
        }
        if (!(snapshotNativePenRow(fixture) == before)) {
            throw std::runtime_error(
                std::string("native pen-row changed state before fallback: ") +
                guard_name);
        }
    };

    require_rejected("pen already down", [](
        NativePenRowFixture &fixture,
        NativePenRowCall &) {
        fixture.sprite->pen_down = 1;
    });
    require_rejected("aliased scalar handles", [](
        NativePenRowFixture &,
        NativePenRowCall &call) {
        call.column = call.row;
    });
    require_rejected("boolean column scalar kind", [](
        NativePenRowFixture &fixture,
        NativePenRowCall &) {
        sjit_variable_set_scalar_kind(
            fixture.column_variable,
            SJIT_SCALAR_BOOL);
    });
    require_rejected("boolean index scalar kind", [](
        NativePenRowFixture &fixture,
        NativePenRowCall &) {
        sjit_variable_set_scalar_kind(
            fixture.index_variable,
            SJIT_SCALAR_BOOL);
    });
    require_rejected("list-valued scalar", [](
        NativePenRowFixture &fixture,
        NativePenRowCall &) {
        SList *list_value = sjit_list_create();
        require(list_value, "create native pen-row list-valued scalar");
        pushNativePenRowValue(list_value, sjit_make_string("row"));
        sjit_variable_set_move(
            fixture.row_variable,
            sjit_make_list(list_value));
    });
    require_rejected("fractional row", [](
        NativePenRowFixture &fixture,
        NativePenRowCall &) {
        sjit_variable_set_number(fixture.row_variable, 1.5);
    });
    require_rejected("non-finite column", [](
        NativePenRowFixture &fixture,
        NativePenRowCall &) {
        sjit_variable_set_number(
            fixture.column_variable,
            std::numeric_limits<double>::infinity());
    });
    require_rejected("fractional column count", [](
        NativePenRowFixture &,
        NativePenRowCall &call) {
        call.columns = 3.5;
    });
    require_rejected("non-finite x step", [](
        NativePenRowFixture &,
        NativePenRowCall &call) {
        call.x_step = std::numeric_limits<double>::infinity();
    });
    require_rejected("invalid marker", [](
        NativePenRowFixture &fixture,
        NativePenRowCall &call) {
        fixture.alternate_replacement = literalString("x");
        call.replacement = fixture.alternate_replacement;
    });
    require_rejected("non-string replacement", [](
        NativePenRowFixture &fixture,
        NativePenRowCall &call) {
        fixture.alternate_replacement = literalNumber(0.0);
        call.replacement = fixture.alternate_replacement;
    });
    require_rejected("invalid color parameter", [](
        NativePenRowFixture &,
        NativePenRowCall &call) {
        call.param_id = 0;
    });
    require_rejected("missing color variable", [](
        NativePenRowFixture &,
        NativePenRowCall &call) {
        call.colors = nullptr;
    });
    require_rejected("short color list", [](
        NativePenRowFixture &fixture,
        NativePenRowCall &) {
        require(
            sjit_list_delete(
                fixture.colorList(),
                sjit_list_length(fixture.colorList())),
            "shorten native pen-row color list");
    });
    require_rejected("short brightness list", [](
        NativePenRowFixture &fixture,
        NativePenRowCall &) {
        require(
            sjit_list_delete(
                fixture.brightnessList(),
                sjit_list_length(fixture.brightnessList())),
            "shorten native pen-row brightness list");
    });
    require_rejected("index arithmetic overflow", [](
        NativePenRowFixture &fixture,
        NativePenRowCall &) {
        sjit_variable_set_number(
            fixture.row_variable,
            static_cast<double>(std::numeric_limits<int>::max()));
    });
}

void test_native_pen_row_shared_storage_replays_safely() {
    NativePenRowFixture replay(false);
    NativePenRowFixture reference(false);
    replay.shared_color_alias = sjit_list_clone(replay.colorList());
    require(replay.shared_color_alias, "clone native pen-row color storage");
    const std::vector<NativePenRowValueSnapshot> alias_before =
        snapshotNativePenRowList(replay.shared_color_alias);
    const NativePenRowSnapshot before = snapshotNativePenRow(replay);

    const NativePenRowCall call = defaultNativePenRowCall(replay);
    require(
        executeNativePenRow(replay, call) == 0,
        "native pen-row rejects shared mutable color storage");
    require(
        snapshotNativePenRow(replay) == before,
        "shared-storage rejection leaves the ordinary loop replayable");
    require(
        snapshotNativePenRowList(replay.shared_color_alias) == alias_before,
        "shared-storage rejection leaves the alias unchanged");

    executeSafePenRow(replay, 4, call.x_step, call.param_id);
    executeSafePenRow(reference, 4, call.x_step, call.param_id);
    require(
        snapshotNativePenRow(replay) == snapshotNativePenRow(reference),
        "safe fallback after shared-storage rejection matches an ordinary row");
    require(
        snapshotNativePenRowList(replay.shared_color_alias) == alias_before,
        "safe fallback preserves the copy-on-write color alias");
}

SString *setNativePenRowStringAndRetain(
    SVariable *variable,
    const char *text) {
    SValue value = sjit_make_string(text);
    sjit_variable_set(variable, value);
    sjit_value_destroy(value);
    require(
        variable && variable->value.tag == SJIT_VALUE_STRING &&
            variable->value.ptr,
        "set native pen-row scalar string");
    return sjit_string_clone(
        static_cast<const SString *>(variable->value.ptr));
}

void test_native_pen_row_string_scalar_commit_releases_ownership() {
    NativePenRowFixture native(false);
    NativePenRowFixture reference(false);
    SString *native_column = setNativePenRowStringAndRetain(
        native.column_variable,
        "0");
    SString *native_index = setNativePenRowStringAndRetain(
        native.index_variable,
        "-77");
    SString *reference_column = setNativePenRowStringAndRetain(
        reference.column_variable,
        "0");
    SString *reference_index = setNativePenRowStringAndRetain(
        reference.index_variable,
        "-77");
    require(
        native_column->ref_count == 2 && native_index->ref_count == 2 &&
            reference_column->ref_count == 2 &&
            reference_index->ref_count == 2,
        "retained scalar strings start with variable and test owners");

    const NativePenRowCall call = defaultNativePenRowCall(native);
    require(
        executeNativePenRow(native, call) == 1,
        "native pen-row accepts numeric scalar strings");
    executeSafePenRow(reference, 4, call.x_step, call.param_id);

    require(
        native.column_variable->value.tag == SJIT_VALUE_NUMBER &&
            native.index_variable->value.tag == SJIT_VALUE_NUMBER,
        "native pen-row commits column and index as numbers");
    require(
        native_column->ref_count == 1 && native_index->ref_count == 1,
        "native numeric commit releases both variable string owners");
    require(
        reference_column->ref_count == 1 && reference_index->ref_count == 1,
        "safe loop releases both variable string owners");
    require(
        snapshotNativePenRow(native) == snapshotNativePenRow(reference),
        "native numeric-string commit matches the safe loop");

    sjit_string_destroy(native_column);
    sjit_string_destroy(native_index);
    sjit_string_destroy(reference_column);
    sjit_string_destroy(reference_index);
}

void replaceNativePenRowColorWithMarkerList(NativePenRowFixture &fixture) {
    SList *marker_list = sjit_list_create();
    require(marker_list, "create native pen-row marker list");
    pushNativePenRowValue(marker_list, sjit_make_string("n"));
    require(
        sjit_list_replace_move(
            fixture.colorList(),
            5,
            sjit_make_list(marker_list)),
        "replace native pen-row color with marker list");
}

void requireNativePenRowMarkerListPreserved(
    const NativePenRowFixture &fixture) {
    SValue item = sjit_list_get(fixture.colorList(), 5);
    require(
        item.tag == SJIT_VALUE_LIST && item.ptr,
        "marker-equivalent list remains a list");
    SValue contents = sjit_list_contents(static_cast<SList *>(item.ptr));
    SValue marker = sjit_make_string("n");
    require(
        sjit_eq(fixture.runtime, contents, marker),
        "marker-equivalent list contents remain n");
    sjit_value_destroy(marker);
    sjit_value_destroy(contents);
    sjit_value_destroy(item);
}

void test_native_pen_row_list_marker_matches_scratch_equality() {
    NativePenRowFixture native(false);
    NativePenRowFixture reference(false);
    replaceNativePenRowColorWithMarkerList(native);
    replaceNativePenRowColorWithMarkerList(reference);

    const NativePenRowCall call = defaultNativePenRowCall(native);
    require(
        executeNativePenRow(native, call) == 1,
        "native pen-row accepts a list-valued color item");
    executeSafePenRow(reference, 4, call.x_step, call.param_id);
    require(
        snapshotNativePenRow(native) == snapshotNativePenRow(reference),
        "native list marker skip matches Scratch equality");
    require(
        native.runtime->pen.length == 3,
        "list whose contents are n skips exactly one pixel");
    requireNativePenRowMarkerListPreserved(native);
    requireNativePenRowMarkerListPreserved(reference);
}

void replaceNativePenRowColorWithEmbeddedNullMarker(
    NativePenRowFixture &fixture) {
    const char bytes[] = {'n', '\0', 'x'};
    require(
        sjit_list_replace_move(
            fixture.colorList(),
            5,
            sjit_make_string_len(bytes, 3)),
        "replace native pen-row color with embedded-null marker");
}

void requireNativePenRowEmbeddedNullMarkerPreserved(
    const NativePenRowFixture &fixture) {
    SValue item = sjit_list_get(fixture.colorList(), 5);
    const auto *string = item.tag == SJIT_VALUE_STRING ?
        static_cast<const SString *>(item.ptr) : nullptr;
    require(
        string && string->bytes && string->length == 3 &&
            string->bytes[0] == 'n' && string->bytes[1] == '\0' &&
            string->bytes[2] == 'x',
        "embedded-null marker remains byte-for-byte unchanged");
    sjit_value_destroy(item);
}

void test_native_pen_row_embedded_null_marker_matches_c_string_equality() {
    NativePenRowFixture native(false);
    NativePenRowFixture reference(false);
    replaceNativePenRowColorWithEmbeddedNullMarker(native);
    replaceNativePenRowColorWithEmbeddedNullMarker(reference);

    const NativePenRowCall call = defaultNativePenRowCall(native);
    require(
        executeNativePenRow(native, call) == 1,
        "native pen-row accepts an embedded-null color string");
    executeSafePenRow(reference, 4, call.x_step, call.param_id);
    require(
        snapshotNativePenRow(native) == snapshotNativePenRow(reference),
        "native embedded-null marker skip matches C-string equality");
    require(
        native.runtime->pen.length == 3,
        "n followed by an embedded null skips exactly one pixel");
    requireNativePenRowEmbeddedNullMarkerPreserved(native);
    requireNativePenRowEmbeddedNullMarkerPreserved(reference);
}

enum class NativePenRowAstVariant {
    Exact,
    XStepUsesIndex,
    XStepUsesCaseAliasedIndex,
    StrideUsesColumn
};

SCompiledScript *makeNativePenRowAstScript(NativePenRowAstVariant variant) {
    SCompiledScript *script = sjit_compiled_script_create(1, 1);
    require(script, "create native pen-row AST script");
    script->statements[0].opcode = SJIT_STMT_PROCEDURE_CALL;
    script->statements[0].procedure_name = sjit_string_new("draw row");

    script->procedure_count = 1;
    script->procedures = static_cast<SCompiledProcedure *>(
        std::calloc(1, sizeof(SCompiledProcedure)));
    require(script->procedures, "allocate native pen-row AST procedure");
    SCompiledProcedure &procedure = script->procedures[0];
    procedure.name = sjit_string_new("draw row");
    procedure.warp_mode = 1;
    procedure.statement_count = 1;
    procedure.statements = static_cast<SStatement *>(
        std::calloc(1, sizeof(SStatement)));
    require(procedure.statements, "allocate native pen-row AST body");

    SStatement &repeat = procedure.statements[0];
    repeat.opcode = SJIT_STMT_REPEAT;
    repeat.times = variant == NativePenRowAstVariant::StrideUsesColumn ?
        sjit_expr_create_variable("column") : literalNumber(4.0);
    repeat.substack_count = 4;
    repeat.substack = static_cast<SStatement *>(
        std::calloc(4, sizeof(SStatement)));
    require(repeat.substack, "allocate native pen-row repeat body");

    SStatement &set_index = repeat.substack[0];
    set_index.opcode = SJIT_STMT_SET_VARIABLE;
    set_index.variable_name = sjit_string_new("index");
    SExpr *stride = variant == NativePenRowAstVariant::StrideUsesColumn ?
        sjit_expr_create_variable("column") : literalNumber(4.0);
    set_index.value = sjit_expr_create_binary(
        SJIT_EXPR_ADD,
        sjit_expr_create_binary(
            SJIT_EXPR_ADD,
            sjit_expr_create_binary(
                SJIT_EXPR_MUL,
                sjit_expr_create_variable("row"),
                stride),
            sjit_expr_create_variable("column")),
        literalNumber(1.0));

    SStatement &condition = repeat.substack[1];
    condition.opcode = SJIT_STMT_IF;
    condition.condition = sjit_expr_create_unary(
        SJIT_EXPR_NOT,
        sjit_expr_create_binary(
            SJIT_EXPR_EQ,
            sjit_expr_create_list_item(
                "colors",
                sjit_expr_create_variable("index")),
            literalString("n")));
    condition.substack_count = 5;
    condition.substack = static_cast<SStatement *>(
        std::calloc(5, sizeof(SStatement)));
    require(condition.substack, "allocate native pen-row condition body");

    condition.substack[0].opcode = SJIT_STMT_PEN_SET_COLOR;
    condition.substack[0].value = sjit_expr_create_list_item(
        "colors",
        sjit_expr_create_variable("index"));

    condition.substack[1].opcode = SJIT_STMT_PEN_CHANGE_COLOR_PARAM;
    condition.substack[1].index = literalString("brightness");
    condition.substack[1].value = sjit_expr_create_list_item(
        "brightness",
        sjit_expr_create_variable("index"));

    condition.substack[2].opcode = SJIT_STMT_LIST_REPLACE;
    condition.substack[2].variable_name = sjit_string_new("colors");
    condition.substack[2].index = sjit_expr_create_variable("index");
    condition.substack[2].value = literalString("n");
    condition.substack[3].opcode = SJIT_STMT_PEN_DOWN;
    condition.substack[4].opcode = SJIT_STMT_PEN_UP;

    repeat.substack[2].opcode = SJIT_STMT_MOTION_CHANGE_X;
    if (variant == NativePenRowAstVariant::XStepUsesIndex) {
        repeat.substack[2].value = sjit_expr_create_variable("index");
    } else if (variant == NativePenRowAstVariant::XStepUsesCaseAliasedIndex) {
        /* Scratch variable lookup is case-insensitive, so this reporter
           aliases the modified "index" variable despite the spelling. */
        repeat.substack[2].value = sjit_expr_create_variable("INDEX");
    } else {
        repeat.substack[2].value = literalNumber(1.25);
    }

    repeat.substack[3].opcode = SJIT_STMT_CHANGE_VARIABLE;
    repeat.substack[3].variable_name = sjit_string_new("column");
    repeat.substack[3].value = literalNumber(1.0);
    return script;
}

std::string emitNativePenRowAstIr(
    NativePenRowAstVariant variant,
    const char *module_name,
    const char *path) {
    SCompiledScript *script = makeNativePenRowAstScript(variant);
    sjit::JitEngine jit;
    jit.emitScriptLl(*script, module_name, path);
    std::ifstream file(path);
    require(file.good(), "open emitted native pen-row AST IR");
    const std::string ir(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    sjit_compiled_script_destroy(script);
    return ir;
}

void test_native_pen_row_ast_matcher_only_emits_for_invariant_shape() {
    const std::string exact_ir = emitNativePenRowAstIr(
        NativePenRowAstVariant::Exact,
        "sjit_unit_native_pen_row_exact_ir",
        "/tmp/xyo-jit-native-pen-row-exact.ll");
    const std::string x_step_ir = emitNativePenRowAstIr(
        NativePenRowAstVariant::XStepUsesIndex,
        "sjit_unit_native_pen_row_xstep_ir",
        "/tmp/xyo-jit-native-pen-row-xstep.ll");
    const std::string case_alias_ir = emitNativePenRowAstIr(
        NativePenRowAstVariant::XStepUsesCaseAliasedIndex,
        "sjit_unit_native_pen_row_case_alias_ir",
        "/tmp/xyo-jit-native-pen-row-case-alias.ll");
    const std::string stride_ir = emitNativePenRowAstIr(
        NativePenRowAstVariant::StrideUsesColumn,
        "sjit_unit_native_pen_row_stride_ir",
        "/tmp/xyo-jit-native-pen-row-stride.ll");
    const std::string helper_call =
        "call i32 @sjit_jit_pen_render_row_from_variables";
    require(
        exact_ir.find(helper_call) != std::string::npos &&
            exact_ir.find("procedure_native_pen_row_handled") !=
                std::string::npos,
        "exact native pen-row AST emits the guarded row helper");
    require(
        x_step_ir.find(helper_call) == std::string::npos &&
            x_step_ir.find("procedure_native_pen_row_handled") ==
                std::string::npos,
        "index-dependent x step stays in the ordinary loop");
    require(
        case_alias_ir.find(helper_call) == std::string::npos &&
            case_alias_ir.find("procedure_native_pen_row_handled") ==
                std::string::npos,
        "case-aliased index reporter stays in the ordinary loop");
    require(
        stride_ir.find(helper_call) == std::string::npos &&
            stride_ir.find("procedure_native_pen_row_handled") ==
                std::string::npos,
        "column-dependent stride stays in the ordinary loop");
}

void test_runtime_draw_and_broadcast() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "Sprite1", 0);
    assert(sprite);
    assert(sprite->base.id == 1);

    assert(sjit_runtime_register_script(
        runtime,
        sprite->base.id,
        1,
        SJIT_HAT_EVENT_WHENFLAGCLICKED,
        "",
        1,
        0,
        drawScript));
    assert(sjit_runtime_register_script(
        runtime,
        sprite->base.id,
        2,
        SJIT_HAT_EVENT_WHENBROADCASTRECEIVED,
        "go",
        1,
        0,
        broadcastReceiver));

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);
    const SDrawCommandBuffer *draw = sjit_runtime_get_draw_commands(runtime);
    assert(draw && draw->length == 1);
    assert(draw->items[0].x == 10.0);
    assert(draw->items[0].y == 20.0);

    g_broadcast_count = 0;
    assert(sjit_event_broadcast(runtime, "GO") == 1);
    sjit_runtime_tick(runtime);
    assert(g_broadcast_count == 1);

    sjit_runtime_destroy(runtime);
}

void test_runtime_draw_owned_storage() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "OwnedPenTail", 0);
    require(runtime && sprite, "create draw owned-storage runtime");
    sjit_pen_stamp(runtime, sprite);
    sjit_runtime_tick(runtime);

    const SDrawCommandBuffer *draw = sjit_runtime_get_draw_commands(runtime);
    require(
        draw && draw->items != runtime->pen.items,
        "draw queue owns storage when pen tail capacity is available");
    require(draw->length == 2, "owned draw queue contains pen command and visible sprite");
    require(
        draw->items[0].kind == SJIT_DRAW_PEN_STROKE && draw->items[1].kind == SJIT_DRAW_SPRITE,
        "owned draw queue preserves command ordering");
    sjit_runtime_clear_draw_commands(runtime);
    require(sjit_runtime_pen_path_count(runtime) == 1, "clearing owned draw queue does not clear pen history");
    sjit_runtime_tick(runtime);
    draw = sjit_runtime_get_draw_commands(runtime);
    require(
        draw && draw->items != runtime->pen.items && draw->length == 2,
        "next tick restores owned draw commands");
    sjit_runtime_destroy(runtime);

    runtime = sjit_runtime_create();
    sprite = sjit_runtime_create_sprite(runtime, "OwnedPen", 0);
    require(runtime && sprite, "create draw fallback runtime");
    for (int i = 0; i < SJIT_INITIAL_CAPACITY; ++i) {
        sjit_pen_stamp(runtime, sprite);
    }
    require(
        runtime->pen.length == runtime->pen.capacity,
        "fallback fixture fills initial pen capacity exactly");
    sjit_runtime_tick(runtime);
    draw = sjit_runtime_get_draw_commands(runtime);
    require(draw && draw->items != runtime->pen.items, "draw queue uses owned storage when pen tail is full");
    require(
        draw->length == SJIT_INITIAL_CAPACITY + 1 &&
            draw->items[0].kind == SJIT_DRAW_PEN_STROKE &&
            draw->items[SJIT_INITIAL_CAPACITY].kind == SJIT_DRAW_SPRITE,
        "owned draw fallback preserves all pen commands before sprites");
    sjit_runtime_destroy(runtime);
}

void test_pen_stroke_buffer() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "Pen", 0);
    assert(sprite);

    sjit_pen_set_color_rgb(runtime, sprite, 10, 20, 30);
    sjit_pen_set_size(runtime, sprite, sjit_make_number(3.0));
    sjit_pen_down(runtime, sprite);
    sjit_sprite_set_xy(runtime, sprite, 10.0, 15.0, 0);
    sjit_pen_up(runtime, sprite);
    sjit_runtime_tick(runtime);

    const SDrawCommandBuffer *draw = sjit_runtime_get_draw_commands(runtime);
    require(sjit_runtime_pen_path_count(runtime) == 2, "pen path API reports stored segments");
    const SDrawCommand *path = sjit_runtime_pen_path_data(runtime);
    require(path && path[1].x2 == 10.0 && path[1].y2 == 15.0, "pen path API exposes segment data");
    assert(draw && draw->length >= 2);
    assert(draw->items[0].kind == SJIT_DRAW_PEN_STROKE);
    assert(draw->items[0].x == 0.0);
    assert(draw->items[0].y == 0.0);
    assert(draw->items[0].x2 == 0.0);
    assert(draw->items[0].y2 == 0.0);
    assert(draw->items[0].r == 10);
    assert(draw->items[0].g == 20);
    assert(draw->items[0].b == 30);
    assert(draw->items[0].pen_width == 3.0);
    assert(draw->items[1].kind == SJIT_DRAW_PEN_STROKE);
    assert(draw->items[1].x == 0.0);
    assert(draw->items[1].y == 0.0);
    assert(draw->items[1].x2 == 10.0);
    assert(draw->items[1].y2 == 15.0);

    sjit_pen_down(runtime, sprite);
    sjit_pen_up(runtime, sprite);
    sjit_runtime_tick(runtime);
    draw = sjit_runtime_get_draw_commands(runtime);
    assert(draw && draw->length >= 3);
    assert(draw->items[2].kind == SJIT_DRAW_PEN_STROKE);
    assert(draw->items[2].x == 10.0);
    assert(draw->items[2].y == 15.0);
    assert(draw->items[2].x2 == 10.0);
    assert(draw->items[2].y2 == 15.0);

    sjit_pen_down(runtime, sprite);
    for (int i = 1; i <= 16; ++i) {
        sjit_sprite_set_xy(runtime, sprite, 10.0 + i, 15.0 + i, 0);
    }
    sjit_pen_up(runtime, sprite);
    require(sjit_runtime_pen_path_count(runtime) == 20, "pen path array grows beyond initial capacity");
    path = sjit_runtime_pen_path_data(runtime);
    require(path && path[19].x2 == 26.0 && path[19].y2 == 31.0, "grown pen path preserves segments");

    sjit_pen_set_color_rgb(runtime, sprite, 122, 39, 147);
    sjit_pen_change_color_param(runtime, sprite, "brightness", sjit_make_number(-100.0));
    assert(sprite->pen_r == 0);
    assert(sprite->pen_g == 0);
    assert(sprite->pen_b == 0);
    sjit_pen_change_color_param(runtime, sprite, "brightness", sjit_make_number(100.0));
    assert(sprite->pen_r > 0 || sprite->pen_g > 0 || sprite->pen_b > 0);
    sjit_pen_change_color_param(runtime, sprite, "transparency", sjit_make_number(100.0));
    assert(sprite->pen_a == 0);

    SValue decimal_color = sjit_make_string("13154480");
    sjit_pen_set_color_value(runtime, sprite, decimal_color);
    sjit_value_destroy(decimal_color);
    assert(sprite->pen_r == 200);
    assert(sprite->pen_g == 184);
    assert(sprite->pen_b == 176);
    assert(sprite->pen_a == 255);

    SValue shorthand_hex = sjit_make_string("#0fa");
    sjit_pen_set_color_value(runtime, sprite, shorthand_hex);
    sjit_value_destroy(shorthand_hex);
    assert(sprite->pen_r == 0);
    assert(sprite->pen_g == 255);
    assert(sprite->pen_b == 170);
    assert(sprite->pen_a == 255);

    SValue invalid_hex = sjit_make_string("#nothex");
    sjit_pen_set_color_value(runtime, sprite, invalid_hex);
    sjit_value_destroy(invalid_hex);
    assert(sprite->pen_r == 0);
    assert(sprite->pen_g == 0);
    assert(sprite->pen_b == 0);
    assert(sprite->pen_a == 255);

    sjit_pen_set_color_value(runtime, sprite, sjit_make_number(2147549699.0));
    assert(sprite->pen_r == 1);
    assert(sprite->pen_g == 2);
    assert(sprite->pen_b == 3);
    assert(sprite->pen_a == 128);
    assert(sprite->pen_transparency > 49.0);
    assert(sprite->pen_transparency < 50.0);

    const int revision_before_clear = sjit_runtime_pen_path_revision(runtime);
    sjit_pen_clear(runtime);
    require(sjit_runtime_pen_path_count(runtime) == 0, "pen clear resets path length");
    require(
        sjit_runtime_pen_path_revision(runtime) == revision_before_clear + 1,
        "pen clear increments path revision");
    sjit_runtime_tick(runtime);
    draw = sjit_runtime_get_draw_commands(runtime);
    assert(draw && draw->length == 1);

    sjit_runtime_destroy(runtime);
}

void test_interpreter_adjacent_pen_stamp() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "Stamp", 0);
    require(sprite, "create stamp sprite");
    sprite->x = 12.0;
    sprite->y = -8.0;

    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 2);
    require(script, "create adjacent pen script");
    script->statements[0].opcode = SJIT_STMT_PEN_DOWN;
    script->statements[1].opcode = SJIT_STMT_PEN_UP;
    require(
        sjit_runtime_register_script_with_data(
            runtime,
            sprite->base.id,
            77,
            SJIT_HAT_EVENT_WHENFLAGCLICKED,
            "",
            1,
            0,
            sjit_script_interpreter_entry,
            script),
        "register adjacent pen script");

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);
    require(sprite->pen_down == 0, "adjacent pen pair leaves pen up");
    require(sjit_runtime_pen_path_count(runtime) == 1, "adjacent pen pair emits one stamp");
    const SDrawCommand *path = sjit_runtime_pen_path_data(runtime);
    require(
        path && path[0].x == 12.0 && path[0].y == -8.0 &&
            path[0].x2 == 12.0 && path[0].y2 == -8.0,
        "adjacent pen pair preserves stamp coordinates");

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_interpreter_adjacent_pen_color_change() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "ColorPair", 0);
    SSprite *reference = sjit_runtime_create_sprite(runtime, "ColorReference", 0);
    require(sprite && reference, "create color pair sprites");

    const double packed_color = 2147549699.0;
    const double brightness_delta = -17.25;
    sjit_pen_set_color_value(runtime, reference, sjit_make_number(packed_color));
    sjit_pen_change_color_param_number(runtime, reference, 3, brightness_delta);

    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 2);
    require(script, "create adjacent color script");
    script->statements[0].opcode = SJIT_STMT_PEN_SET_COLOR;
    script->statements[0].value = literalNumber(packed_color);
    script->statements[1].opcode = SJIT_STMT_PEN_CHANGE_COLOR_PARAM;
    script->statements[1].index = literalString("brightness");
    script->statements[1].value = literalNumber(brightness_delta);
    require(
        sjit_runtime_register_script_with_data(
            runtime,
            sprite->base.id,
            78,
            SJIT_HAT_EVENT_WHENFLAGCLICKED,
            "",
            1,
            0,
            sjit_script_interpreter_entry,
            script),
        "register adjacent color script");

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);
    require(
        sprite->pen_r == reference->pen_r &&
            sprite->pen_g == reference->pen_g &&
            sprite->pen_b == reference->pen_b &&
            sprite->pen_a == reference->pen_a &&
            sprite->pen_hue == reference->pen_hue &&
            sprite->pen_saturation == reference->pen_saturation &&
            sprite->pen_brightness == reference->pen_brightness &&
            sprite->pen_transparency == reference->pen_transparency,
        "adjacent pen color pair matches separate operations exactly");

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_interpreter_adjacent_hex_pen_color_change() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "HexColorPair", 0);
    SSprite *reference = sjit_runtime_create_sprite(runtime, "HexColorReference", 0);
    require(sprite && reference, "create hex color pair sprites");

    const double brightness_delta = 10.0;
    SValue hex_color = sjit_make_string("#7a2793");
    sjit_pen_set_color_value(runtime, reference, hex_color);
    sjit_pen_change_color_param_number(runtime, reference, 3, brightness_delta);
    sjit_value_destroy(hex_color);

    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 2);
    require(script, "create adjacent hex color script");
    script->statements[0].opcode = SJIT_STMT_PEN_SET_COLOR;
    script->statements[0].value = literalString("#7a2793");
    script->statements[1].opcode = SJIT_STMT_PEN_CHANGE_COLOR_PARAM;
    script->statements[1].index = literalString("brightness");
    script->statements[1].value = literalNumber(brightness_delta);
    require(
        sjit_runtime_register_script_with_data(
            runtime,
            sprite->base.id,
            79,
            SJIT_HAT_EVENT_WHENFLAGCLICKED,
            "",
            1,
            0,
            sjit_script_interpreter_entry,
            script),
        "register adjacent hex color script");

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);
    require(
        sprite->pen_r == reference->pen_r &&
            sprite->pen_g == reference->pen_g &&
            sprite->pen_b == reference->pen_b &&
            sprite->pen_a == reference->pen_a,
        "adjacent hex pen color pair preserves the hex color");
    require(
        sprite->pen_r == 143 && sprite->pen_g == 45 && sprite->pen_b == 172,
        "hex pen brightness uses Scratch's channel flooring");

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_combined_pen_brightness_matches_separate_operations() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *combined = sjit_runtime_create_sprite(runtime, "CombinedColor", 0);
    SSprite *separate = sjit_runtime_create_sprite(runtime, "SeparateColor", 0);
    require(combined && separate, "create pen brightness comparison sprites");
    const int channels[] = {0, 1, 2, 15, 31, 63, 64, 127, 128, 191, 254, 255};
    const double deltas[] = {-150.0, -100.0, -17.25, -0.5, 0.0, 0.5, 17.25, 100.0, 150.0};
    for (int r : channels) {
        for (int g : channels) {
            for (int b : channels) {
                const uint32_t bits = 0x80000000u |
                    (static_cast<uint32_t>(r) << 16) |
                    (static_cast<uint32_t>(g) << 8) |
                    static_cast<uint32_t>(b);
                for (double delta : deltas) {
                    const SValue color = sjit_make_number(static_cast<double>(bits));
                    sjit_pen_set_color_value(runtime, separate, color);
                    sjit_pen_change_color_param_number(runtime, separate, 3, delta);
                    sjit_pen_set_color_value_and_change_param_number(runtime, combined, color, 3, delta);
                    sjit_pen_set_color_value_and_change_param_number(runtime, combined, color, 3, delta);
                    require(
                        combined->pen_r == separate->pen_r &&
                            combined->pen_g == separate->pen_g &&
                            combined->pen_b == separate->pen_b &&
                            combined->pen_a == separate->pen_a &&
                            combined->pen_hue == separate->pen_hue &&
                            combined->pen_saturation == separate->pen_saturation &&
                            combined->pen_brightness == separate->pen_brightness &&
                            combined->pen_transparency == separate->pen_transparency,
                        "combined brightness must exactly match separate pen operations");
                }
            }
        }
    }
    sjit_runtime_destroy(runtime);
}

void test_wait_resume() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "Sprite1", 0);
    assert(sprite);
    assert(sjit_runtime_register_script(
        runtime,
        sprite->base.id,
        3,
        SJIT_HAT_EVENT_WHENFLAGCLICKED,
        "",
        1,
        0,
        waitScript));

    sjit_runtime_green_flag(runtime);
    sjit_runtime_set_time(runtime, 0.0, 0.0);
    sjit_runtime_tick(runtime);
    assert(runtime->thread_count == 1);
    sjit_runtime_set_time(runtime, 50.0, 50.0);
    sjit_runtime_tick(runtime);
    assert(runtime->thread_count == 1);
    sjit_runtime_set_time(runtime, 160.0, 110.0);
    sjit_runtime_tick(runtime);
    assert(runtime->thread_count == 0);

    sjit_runtime_destroy(runtime);
}

void test_jit_smoke() {
    SValue value = sjit::runSmokeJit();
    assert(value.tag == SJIT_VALUE_NUMBER);
    assert(value.number == 7.0);
}

void test_jit_script_entry_runs_compiled_program() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "JitSprite", 0);
    require(sprite, "create jit sprite");

    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 22);
    require(script, "create jit script");
    script->statements[0].opcode = SJIT_STMT_RESET_TIMER;
    script->statements[1].opcode = SJIT_STMT_LIST_ADD;
    script->statements[1].variable_name = sjit_string_new("items");
    script->statements[1].value = literalString("one");
    script->statements[2].opcode = SJIT_STMT_LIST_ADD;
    script->statements[2].variable_name = sjit_string_new("items");
    script->statements[2].value = literalString("two");
    script->statements[3].opcode = SJIT_STMT_LIST_INSERT;
    script->statements[3].variable_name = sjit_string_new("items");
    script->statements[3].index = literalNumber(2.0);
    script->statements[3].value = literalString("middle");
    script->statements[4].opcode = SJIT_STMT_LIST_REPLACE;
    script->statements[4].variable_name = sjit_string_new("items");
    script->statements[4].index = literalNumber(1.0);
    script->statements[4].value = literalString("zero");
    script->statements[5].opcode = SJIT_STMT_LIST_DELETE;
    script->statements[5].variable_name = sjit_string_new("items");
    script->statements[5].index = literalNumber(2.0);
    script->statements[6].opcode = SJIT_STMT_LIST_DELETE_ALL;
    script->statements[6].variable_name = sjit_string_new("items");
    script->statements[7].opcode = SJIT_STMT_LIST_ADD;
    script->statements[7].variable_name = sjit_string_new("items");
    script->statements[7].value = literalString("final");
    script->statements[8].opcode = SJIT_STMT_PEN_CLEAR;
    script->statements[9].opcode = SJIT_STMT_PEN_SET_SIZE;
    script->statements[9].value = literalNumber(4.0);
    script->statements[10].opcode = SJIT_STMT_LOOKS_HIDE;
    script->statements[11].opcode = SJIT_STMT_PEN_DOWN;
    script->statements[12].opcode = SJIT_STMT_MOTION_SET_X;
    script->statements[12].value = literalNumber(10.0);
    script->statements[13].opcode = SJIT_STMT_MOTION_CHANGE_Y;
    script->statements[13].value = literalNumber(5.0);
    script->statements[14].opcode = SJIT_STMT_MOTION_GOTO_XY;
    script->statements[14].value = literalNumber(20.0);
    script->statements[14].index = literalNumber(30.0);
    script->statements[15].opcode = SJIT_STMT_LOOKS_SET_SIZE;
    script->statements[15].value = literalNumber(80.0);
    script->statements[16].opcode = SJIT_STMT_LOOKS_SHOW;
    script->statements[17].opcode = SJIT_STMT_PEN_UP;
    script->statements[18].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[18].variable_name = sjit_string_new("score");
    script->statements[18].value = literalNumber(10.0);
    script->statements[19].opcode = SJIT_STMT_CHANGE_VARIABLE;
    script->statements[19].variable_name = sjit_string_new("score");
    script->statements[19].value = literalNumber(5.0);
    script->statements[20].opcode = SJIT_STMT_PEN_SET_COLOR;
    script->statements[20].value = literalString("#123456");
    script->statements[21].opcode = SJIT_STMT_PEN_CHANGE_COLOR_PARAM;
    script->statements[21].index = literalString("transparency");
    script->statements[21].value = literalNumber(100.0);

    sjit::JitEngine jit;
    sjit::SRuntimeVoidFn green_flag = sjit_runtime_green_flag;
    sjit::SRuntimeTickFn runtime_tick = sjit_runtime_tick;
    sjit::SRuntimePenPathDataFn pen_path_data = sjit_runtime_pen_path_data;
    sjit::SRuntimeThreadQueryFn pen_path_count = sjit_runtime_pen_path_count;
    sjit::SRuntimeThreadQueryFn pen_path_revision = sjit_runtime_pen_path_revision;
    if (jit.hasRuntimeBitcode()) {
        green_flag = jit.runtimeGreenFlag();
        runtime_tick = jit.runtimeTick();
        pen_path_data = jit.runtimePenPathData();
        pen_path_count = jit.runtimePenPathCount();
        pen_path_revision = jit.runtimePenPathRevision();
        require(green_flag && runtime_tick, "JIT resolves runtime execution entry points");
        require(jit.runtimeHasThreads(), "JIT resolves runtime thread query");
        require(jit.runtimeThreadCount(), "JIT resolves runtime thread count");
        require(
            pen_path_data && pen_path_count && pen_path_revision,
            "JIT resolves pen path read API");

        const std::string runtime_ir_path = "/tmp/xyo-runtime-execution.ll";
        jit.emitRuntimeLl(runtime_ir_path);
        std::ifstream runtime_ir_file(runtime_ir_path);
        const std::string runtime_ir(
            (std::istreambuf_iterator<char>(runtime_ir_file)),
            std::istreambuf_iterator<char>());
        require(runtime_ir.find("define") != std::string::npos, "runtime IR emits definitions");
        require(runtime_ir.find("@sjit_runtime_tick") != std::string::npos, "runtime IR includes tick execution");
        require(runtime_ir.find("@sjit_scheduler_tick") != std::string::npos, "runtime IR includes scheduler execution");
    }

    const std::string ir_path = "/tmp/xyo-jit-blocks.ll";
    jit.emitScriptLl(*script, "sjit_unit_script_entry_ir", ir_path);
    std::ifstream ir_file(ir_path);
    const std::string ir((std::istreambuf_iterator<char>(ir_file)), std::istreambuf_iterator<char>());
    require(ir.find("sjit_jit_reset_timer") != std::string::npos, "JIT IR lowers reset timer");
    require(ir.find("sjit_jit_statement_set_variable") != std::string::npos, "JIT IR lowers set variable");
    require(ir.find("sjit_jit_statement_change_variable") != std::string::npos, "JIT IR lowers change variable");
    require(ir.find("sjit_jit_statement_scalar_variable_ptr") != std::string::npos, "JIT IR looks up scalar variable fast path");
    require(ir.find("set_variable_number_ptr") != std::string::npos, "JIT IR lowers numeric set variable fast path");
    require(ir.find("change_variable_next") != std::string::npos, "JIT IR lowers numeric change variable fast path");
    require(ir.find("set_variable_handle") != std::string::npos,
        "JIT IR resolves set-variable storage through a checked handle");
    require(ir.find("change_variable_handle") != std::string::npos,
        "JIT IR resolves change-variable storage through a checked handle");
    require(ir.find("sjit_jit_statement_list_add") != std::string::npos, "JIT IR lowers list add");
    require(ir.find("sjit_jit_statement_list_delete") != std::string::npos, "JIT IR lowers list delete");
    require(ir.find("sjit_jit_statement_list_delete_all") != std::string::npos, "JIT IR lowers list delete all");
    require(ir.find("sjit_jit_statement_list_insert") != std::string::npos, "JIT IR lowers list insert");
    require(ir.find("sjit_jit_statement_list_replace") != std::string::npos, "JIT IR lowers list replace");
    require(ir.find("sjit_jit_statement_pen_set_color") != std::string::npos, "JIT IR lowers pen set color");
    require(
        ir.find("sjit_jit_statement_pen_change_color_param") != std::string::npos,
        "JIT IR lowers pen color param change");
    require(
        ir.find("call i32 @sjit_script_execute_statement_with_frame") == std::string::npos,
        "JIT IR should not use statement interpreter fallback");

    SCompiledScript *say_script = sjit_compiled_script_create(sprite->base.id, 1);
    require(say_script, "create jit say script");
    say_script->statements[0].opcode = SJIT_STMT_SAY;
    say_script->statements[0].value = literalString("jit");
    const std::string say_ir_path = "/tmp/xyo-jit-say.ll";
    jit.emitScriptLl(*say_script, "sjit_unit_script_say_ir", say_ir_path);
    std::ifstream say_ir_file(say_ir_path);
    const std::string say_ir(
        (std::istreambuf_iterator<char>(say_ir_file)),
        std::istreambuf_iterator<char>());
    require(say_ir.find("sjit_jit_statement_say") != std::string::npos, "JIT IR lowers say");
    sjit_compiled_script_destroy(say_script);

    SCompiledScript *control_script = sjit_compiled_script_create(sprite->base.id, 2);
    require(control_script, "create jit control script");
    control_script->statements[0].opcode = SJIT_STMT_REPEAT;
    control_script->statements[0].times = literalNumber(2.0);
    control_script->statements[0].substack_count = 1;
    control_script->statements[0].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(control_script->statements[0].substack, "allocate repeat substack");
    control_script->statements[0].substack[0].opcode = SJIT_STMT_CHANGE_VARIABLE;
    control_script->statements[0].substack[0].variable_name = sjit_string_new("controlScore");
    control_script->statements[0].substack[0].value = literalNumber(1.0);
    control_script->statements[1].opcode = SJIT_STMT_IF;
    control_script->statements[1].condition = literalBool(true);
    control_script->statements[1].substack_count = 1;
    control_script->statements[1].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(control_script->statements[1].substack, "allocate if substack");
    control_script->statements[1].substack[0].opcode = SJIT_STMT_SET_VARIABLE;
    control_script->statements[1].substack[0].variable_name = sjit_string_new("controlDone");
    control_script->statements[1].substack[0].value = literalNumber(1.0);

    const std::string control_ir_path = "/tmp/xyo-jit-control.ll";
    jit.emitScriptLl(*control_script, "sjit_unit_script_control_ir", control_ir_path);
    std::ifstream control_ir_file(control_ir_path);
    const std::string control_ir(
        (std::istreambuf_iterator<char>(control_ir_file)),
        std::istreambuf_iterator<char>());
    require(
        control_ir.find("sjit_jit_repeat_should_enter_number") != std::string::npos,
        "JIT IR lowers repeat control");
    require(
        control_ir.find("sjit_jit_finish_batched_loop_branch") != std::string::npos,
        "JIT IR lowers repeat branch finish");
    require(
        control_ir.find("sjit_jit_statement_bool") != std::string::npos,
        "JIT IR lowers if condition evaluation");
    require(
        control_ir.find("call i32 @sjit_script_execute_statement_with_frame") == std::string::npos,
        "JIT control IR should not call statement interpreter fallback");
    sjit_compiled_script_destroy(control_script);

    SCompiledScript *numeric_ir_script = sjit_compiled_script_create(sprite->base.id, 5);
    require(numeric_ir_script, "create numeric jit script");
    numeric_ir_script->statements[0].opcode = SJIT_STMT_SET_VARIABLE;
    numeric_ir_script->statements[0].variable_name = sjit_string_new("speed");
    numeric_ir_script->statements[0].value = literalNumber(2.0);
    numeric_ir_script->statements[1].opcode = SJIT_STMT_MOTION_CHANGE_X;
    numeric_ir_script->statements[1].value = sjit_expr_create_binary(
        SJIT_EXPR_ADD,
        sjit_expr_create_variable("speed"),
        literalNumber(3.0));
    numeric_ir_script->statements[2].opcode = SJIT_STMT_IF;
    numeric_ir_script->statements[2].condition = sjit_expr_create_binary(
        SJIT_EXPR_GT,
        sjit_expr_create_variable("speed"),
        literalNumber(1.0));
    numeric_ir_script->statements[2].substack_count = 1;
    numeric_ir_script->statements[2].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(numeric_ir_script->statements[2].substack, "allocate numeric if substack");
    numeric_ir_script->statements[2].substack[0].opcode = SJIT_STMT_MOTION_CHANGE_Y;
    numeric_ir_script->statements[2].substack[0].value = literalNumber(1.0);
    numeric_ir_script->statements[3].opcode = SJIT_STMT_SET_VARIABLE;
    numeric_ir_script->statements[3].variable_name = sjit_string_new("sine");
    numeric_ir_script->statements[3].value = sjit_expr_create_mathop("sin", literalNumber(90.0));
    numeric_ir_script->statements[4].opcode = SJIT_STMT_SET_VARIABLE;
    numeric_ir_script->statements[4].variable_name = sjit_string_new("logarithm");
    numeric_ir_script->statements[4].value = sjit_expr_create_mathop("ln", literalNumber(1.0));

    const std::string numeric_ir_path = "/tmp/xyo-jit-numeric.ll";
    jit.emitScriptLl(*numeric_ir_script, "sjit_unit_script_numeric_ir", numeric_ir_path);
    std::ifstream numeric_ir_file(numeric_ir_path);
    const std::string numeric_ir(
        (std::istreambuf_iterator<char>(numeric_ir_file)),
        std::istreambuf_iterator<char>());
    require(numeric_ir.find("number_add") != std::string::npos, "JIT IR lowers numeric add expression");
    require(numeric_ir.find("number_gt") != std::string::npos, "JIT IR lowers numeric comparison");
    require(numeric_ir.find("expr_variable_handle") != std::string::npos,
        "JIT IR resolves numeric reporter storage through a checked handle");
    require(numeric_ir.find("llvm.sin.f64") != std::string::npos, "JIT IR lowers Scratch sine directly");
    require(numeric_ir.find("llvm.log.f64") != std::string::npos, "JIT IR lowers Scratch logarithm directly");
    require(
        numeric_ir.find("call double @sjit_jit_mathop_number") == std::string::npos,
        "JIT IR avoids generic mathop calls for directly lowered operators");
    sjit_compiled_script_destroy(numeric_ir_script);

    SCompiledScript *list_ir_script = sjit_compiled_script_create(sprite->base.id, 5);
    require(list_ir_script, "create list jit script");
    list_ir_script->statements[0].opcode = SJIT_STMT_LIST_ADD;
    list_ir_script->statements[0].variable_name = sjit_string_new("numbers");
    list_ir_script->statements[0].value = literalNumber(1.0);
    list_ir_script->statements[1].opcode = SJIT_STMT_LIST_ADD;
    list_ir_script->statements[1].variable_name = sjit_string_new("numbers");
    list_ir_script->statements[1].value = literalNumber(2.0);
    list_ir_script->statements[2].opcode = SJIT_STMT_LIST_REPLACE;
    list_ir_script->statements[2].variable_name = sjit_string_new("numbers");
    list_ir_script->statements[2].index = literalNumber(1.0);
    list_ir_script->statements[2].value = literalNumber(3.0);
    list_ir_script->statements[3].opcode = SJIT_STMT_SET_VARIABLE;
    list_ir_script->statements[3].variable_name = sjit_string_new("length");
    list_ir_script->statements[3].value = sjit_expr_create_list_length("numbers");
    list_ir_script->statements[4].opcode = SJIT_STMT_SET_VARIABLE;
    list_ir_script->statements[4].variable_name = sjit_string_new("first");
    list_ir_script->statements[4].value = sjit_expr_create_list_item("numbers", literalNumber(1.0));

    const std::string list_ir_path = "/tmp/xyo-jit-list.ll";
    jit.emitScriptLl(*list_ir_script, "sjit_unit_script_list_ir", list_ir_path);
    std::ifstream list_ir_file(list_ir_path);
    const std::string list_ir(
        (std::istreambuf_iterator<char>(list_ir_file)),
        std::istreambuf_iterator<char>());
    require(list_ir.find("list_add_number") != std::string::npos, "JIT IR lowers numeric list add fast path");
    require(list_ir.find("list_replace_number") != std::string::npos, "JIT IR lowers numeric list replace fast path");
    require(list_ir.find("list_length_direct") != std::string::npos, "JIT IR lowers list length fast path");
    require(list_ir.find("list_item_number_have_items") != std::string::npos, "JIT IR lowers numeric list item read");
    require(
        list_ir.find("sjit_jit_statement_list_variable_ptr") != std::string::npos,
        "JIT IR uses cached statement list lookup");
    require(
        list_ir.find("sjit_jit_expr_list_variable") != std::string::npos,
        "JIT IR uses cached expression list lookup");
    sjit_compiled_script_destroy(list_ir_script);

    SCompiledScript *bool_ir_script = sjit_compiled_script_create(sprite->base.id, 3);
    require(bool_ir_script, "create bool jit script");
    bool_ir_script->statements[0].opcode = SJIT_STMT_SET_VARIABLE;
    bool_ir_script->statements[0].variable_name = sjit_string_new("flag");
    bool_ir_script->statements[0].value = sjit_expr_create_binary(
        SJIT_EXPR_LT,
        literalNumber(1.0),
        literalNumber(2.0));
    bool_ir_script->statements[1].opcode = SJIT_STMT_IF;
    bool_ir_script->statements[1].condition = sjit_expr_create_variable("flag");
    bool_ir_script->statements[1].substack_count = 1;
    bool_ir_script->statements[1].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(bool_ir_script->statements[1].substack, "allocate bool if substack");
    bool_ir_script->statements[2].opcode = SJIT_STMT_IF;
    bool_ir_script->statements[2].condition = sjit_expr_create_key_pressed(literalString("space"));
    bool_ir_script->statements[2].substack_count = 1;
    bool_ir_script->statements[2].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(bool_ir_script->statements[2].substack, "allocate key if substack");

    const std::string bool_ir_path = "/tmp/xyo-jit-bool.ll";
    jit.emitScriptLl(*bool_ir_script, "sjit_unit_script_bool_ir", bool_ir_path);
    std::ifstream bool_ir_file(bool_ir_path);
    const std::string bool_ir(
        (std::istreambuf_iterator<char>(bool_ir_file)),
        std::istreambuf_iterator<char>());
    require(bool_ir.find("set_bool_variable_number") != std::string::npos, "JIT IR lowers bool set variable fast path");
    require(bool_ir.find("bool_expr_variable_truthy") != std::string::npos, "JIT IR lowers bool variable read fast path");
    require(bool_ir.find("bool_expr_variable_handle") != std::string::npos,
        "JIT IR resolves boolean reporter storage through a checked handle");
    require(bool_ir.find("key_pressed_bool") != std::string::npos, "JIT IR lowers literal key pressed fast path");
    sjit_compiled_script_destroy(bool_ir_script);

    SScriptEntryFn entry = jit.compileScript(*script, "sjit_unit_script_entry");
    require(entry, "compile jit script");
    const int registered = sjit_runtime_register_script_with_data(
        runtime,
        sprite->base.id,
        40,
        SJIT_HAT_EVENT_WHENFLAGCLICKED,
        "",
        1,
        0,
        entry,
        script);
    require(registered, "register jit script");

    sjit_runtime_set_time(runtime, 100.0, 0.0);
    green_flag(runtime);
    sjit_runtime_set_time(runtime, 250.0, 150.0);
    runtime_tick(runtime);

    SVariable *score = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "score", SJIT_VAR_SCALAR);
    require(score && score->value.tag == SJIT_VALUE_NUMBER && score->value.number == 15.0, "JIT updates variables");
    SVariable *items_variable = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "items", SJIT_VAR_LIST);
    require(items_variable && items_variable->value.tag == SJIT_VALUE_LIST, "JIT creates list variable");
    SList *items = static_cast<SList *>(items_variable->value.ptr);
    require(sjit_list_length(items) == 1, "JIT mutates list length");
    SValue item = sjit_list_get(items, 1);
    SValue item_text = sjit_to_string(runtime, item);
    require(
        std::string(sjit_string_cstr(static_cast<const SString *>(item_text.ptr))) == "final",
        "JIT mutates list contents");
    sjit_value_destroy(item);
    sjit_value_destroy(item_text);
    require(runtime->timer_start_ms == 250.0, "JIT resets timer");
    require(sprite->x == 20.0, "JIT sets sprite x");
    require(sprite->y == 30.0, "JIT sets sprite y");
    require(sprite->size == 80.0, "JIT sets sprite size");
    require(sprite->pen_size == 4.0, "JIT sets pen size");
    require(sprite->visible == 1, "JIT updates visibility");
    require(sprite->pen_down == 0, "JIT updates pen down state");
    require(sprite->pen_a == 0, "JIT changes pen transparency");
    require(pen_path_count(runtime) == 4, "LLVM runtime stores pen segments in variable-length path");
    const SDrawCommand *jit_pen_path = pen_path_data(runtime);
    require(
        jit_pen_path && jit_pen_path[3].x2 == 20.0 && jit_pen_path[3].y2 == 30.0,
        "C++ reads the final pen segment through the LLVM API");
    require(pen_path_revision(runtime) > 0, "LLVM pen clear updates path revision");

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_jit_bulk_repeat_numeric_change() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "BulkRepeatSprite", 0);
    require(sprite, "create bulk repeat jit sprite");

    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 2);
    require(script, "create bulk repeat jit script");
    script->statements[0].opcode = SJIT_STMT_SET_VARIABLE;
    script->statements[0].variable_name = sjit_string_new("score");
    script->statements[0].value = literalNumber(0.0);
    script->statements[1].opcode = SJIT_STMT_REPEAT;
    script->statements[1].times = literalNumber(1000000.0);
    script->statements[1].substack_count = 1;
    script->statements[1].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(script->statements[1].substack, "allocate bulk repeat substack");
    script->statements[1].substack[0].opcode = SJIT_STMT_CHANGE_VARIABLE;
    script->statements[1].substack[0].variable_name = sjit_string_new("score");
    script->statements[1].substack[0].value = literalNumber(1.0);

    sjit::JitEngine jit;
    const std::string ir_path = "/tmp/xyo-jit-bulk-repeat.ll";
    jit.emitScriptLl(*script, "sjit_unit_bulk_repeat_ir", ir_path);
    std::ifstream ir_file(ir_path);
    const std::string ir((std::istreambuf_iterator<char>(ir_file)), std::istreambuf_iterator<char>());
    require(
        ir.find("sjit_jit_repeat_take_all_number") != std::string::npos,
        "JIT IR lowers simple numeric repeat to bulk helper");

    SScriptEntryFn entry = jit.compileScript(*script, "sjit_unit_bulk_repeat_entry");
    require(entry, "compile bulk repeat jit script");
    const int registered = sjit_runtime_register_script_with_data(
        runtime,
        sprite->base.id,
        42,
        SJIT_HAT_EVENT_WHENFLAGCLICKED,
        "",
        1,
        0,
        entry,
        script);
    require(registered, "register bulk repeat jit script");

    sjit_runtime_set_turbo_mode(runtime, 1);
    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);

    SVariable *score = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "score", SJIT_VAR_SCALAR);
    require(
        score && score->value.tag == SJIT_VALUE_NUMBER && score->value.number == 1000000.0,
        "JIT bulk repeat updates score in one pass");
    require(runtime->thread_count == 0, "JIT bulk repeat finishes in one tick");

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_jit_compiles_recursive_warp_numeric_procedure() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "ProcSprite", 0);
    require(sprite, "create procedure jit sprite");

    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 2);
    require(script, "create procedure jit script");
    script->statements[0].opcode = SJIT_STMT_PROCEDURE_CALL;
    script->statements[0].procedure_name = sjit_string_new("count %s");
    script->statements[0].argument_count = 1;
    script->statements[0].arguments = static_cast<SArgumentExpr *>(std::calloc(1, sizeof(SArgumentExpr)));
    require(script->statements[0].arguments, "allocate procedure call argument");
    script->statements[0].arguments[0].name = sjit_string_new("n");
    script->statements[0].arguments[0].value = literalNumber(5.0);

    script->procedure_count = 1;
    script->procedures = static_cast<SCompiledProcedure *>(std::calloc(1, sizeof(SCompiledProcedure)));
    require(script->procedures, "allocate jit procedure");
    SCompiledProcedure &procedure = script->procedures[0];
    procedure.name = sjit_string_new("count %s");
    procedure.warp_mode = 1;
    procedure.argument_count = 1;
    procedure.argument_names = static_cast<SString **>(std::calloc(1, sizeof(SString *)));
    require(procedure.argument_names, "allocate jit procedure argument names");
    procedure.argument_names[0] = sjit_string_new("n");
    procedure.statement_count = 1;
    procedure.statements = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(procedure.statements, "allocate jit procedure body");

    procedure.statements[0].opcode = SJIT_STMT_IF;
    procedure.statements[0].condition = sjit_expr_create_binary(
        SJIT_EXPR_GT,
        sjit_expr_create_argument("n"),
        literalNumber(0.0));
    procedure.statements[0].substack_count = 2;
    procedure.statements[0].substack = static_cast<SStatement *>(std::calloc(2, sizeof(SStatement)));
    require(procedure.statements[0].substack, "allocate jit procedure branch");
    procedure.statements[0].substack[0].opcode = SJIT_STMT_CHANGE_VARIABLE;
    procedure.statements[0].substack[0].variable_name = sjit_string_new("hits");
    procedure.statements[0].substack[0].value = literalNumber(1.0);
    procedure.statements[0].substack[1].opcode = SJIT_STMT_PROCEDURE_CALL;
    procedure.statements[0].substack[1].procedure_name = sjit_string_new("count %s");
    procedure.statements[0].substack[1].argument_count = 1;
    procedure.statements[0].substack[1].arguments = static_cast<SArgumentExpr *>(std::calloc(1, sizeof(SArgumentExpr)));
    require(procedure.statements[0].substack[1].arguments, "allocate recursive call argument");
    procedure.statements[0].substack[1].arguments[0].name = sjit_string_new("n");
    procedure.statements[0].substack[1].arguments[0].value = sjit_expr_create_binary(
        SJIT_EXPR_SUB,
        sjit_expr_create_argument("n"),
        literalNumber(1.0));

    sjit::JitEngine jit;
    const std::string ir_path = "/tmp/xyo-jit-procedure.ll";
    jit.emitScriptLl(*script, "sjit_unit_procedure_ir", ir_path);
    std::ifstream ir_file(ir_path);
    const std::string ir((std::istreambuf_iterator<char>(ir_file)), std::istreambuf_iterator<char>());
    require(
        ir.find("define i32 @sjit_unit_procedure_ir_procedure_0") != std::string::npos,
        "JIT IR defines recursive warp procedure variant");
    require(
        ir.find("call i32 @sjit_unit_procedure_ir_procedure_0") != std::string::npos,
        "JIT IR directly recurses through a warp procedure variant");

    SScriptEntryFn entry = jit.compileScript(*script, "sjit_unit_procedure_entry");
    require(entry, "compile procedure jit script");
    require(script->procedures[0].jit_entry, "compile stores recursive warp procedure JIT entry");
    const int registered = sjit_runtime_register_script_with_data(
        runtime,
        sprite->base.id,
        41,
        SJIT_HAT_EVENT_WHENFLAGCLICKED,
        "",
        1,
        0,
        entry,
        script);
    require(registered, "register procedure jit script");

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);
    SVariable *hits = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "hits", SJIT_VAR_SCALAR);
    require(hits && hits->value.tag == SJIT_VALUE_NUMBER && hits->value.number == 5.0, "recursive JIT procedure updates variable");

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_jit_compiles_non_warp_procedure_repeat() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "NonWarpProcSprite", 0);
    require(sprite, "create non-warp procedure jit sprite");

    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 2);
    require(script, "create non-warp procedure jit script");
    script->statements[0].opcode = SJIT_STMT_PROCEDURE_CALL;
    script->statements[0].procedure_name = sjit_string_new("bump");
    script->statements[1].opcode = SJIT_STMT_IF;
    script->statements[1].condition = literalBool(false);
    script->statements[1].substack_count = 1;
    script->statements[1].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(script->statements[1].substack, "allocate unreachable loop coverage branch");
    script->statements[1].substack[0].opcode = SJIT_STMT_PROCEDURE_CALL;
    script->statements[1].substack[0].procedure_name = sjit_string_new("loop coverage");

    script->procedure_count = 3;
    script->procedures = static_cast<SCompiledProcedure *>(std::calloc(3, sizeof(SCompiledProcedure)));
    require(script->procedures, "allocate non-warp jit procedure");
    SCompiledProcedure &procedure = script->procedures[0];
    procedure.name = sjit_string_new("bump");
    procedure.warp_mode = 0;
    procedure.statement_count = 5;
    procedure.statements = static_cast<SStatement *>(std::calloc(5, sizeof(SStatement)));
    require(procedure.statements, "allocate non-warp procedure body");

    procedure.statements[0].opcode = SJIT_STMT_CHANGE_VARIABLE;
    procedure.statements[0].variable_name = sjit_string_new("prefix");
    procedure.statements[0].value = literalNumber(1.0);

    procedure.statements[1].opcode = SJIT_STMT_REPEAT;
    procedure.statements[1].times = literalNumber(3.0);
    procedure.statements[1].substack_count = 7;
    procedure.statements[1].substack = static_cast<SStatement *>(std::calloc(7, sizeof(SStatement)));
    require(procedure.statements[1].substack, "allocate non-warp repeat body");
    procedure.statements[1].substack[0].opcode = SJIT_STMT_CHANGE_VARIABLE;
    procedure.statements[1].substack[0].variable_name = sjit_string_new("bumps");
    procedure.statements[1].substack[0].value = literalNumber(1.0);
    procedure.statements[1].substack[1].opcode = SJIT_STMT_CHANGE_VARIABLE;
    procedure.statements[1].substack[1].variable_name = sjit_string_new("bumps_twice");
    procedure.statements[1].substack[1].value = literalNumber(2.0);
    procedure.statements[1].substack[2].opcode = SJIT_STMT_IF;
    procedure.statements[1].substack[2].condition = literalBool(true);
    procedure.statements[1].substack[2].substack_count = 1;
    procedure.statements[1].substack[2].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(procedure.statements[1].substack[2].substack, "allocate non-warp repeat nested if body");
    procedure.statements[1].substack[2].substack[0].opcode = SJIT_STMT_CHANGE_VARIABLE;
    procedure.statements[1].substack[2].substack[0].variable_name = sjit_string_new("guarded_bumps");
    procedure.statements[1].substack[2].substack[0].value = literalNumber(1.0);
    procedure.statements[1].substack[3].opcode = SJIT_STMT_CHANGE_VARIABLE;
    procedure.statements[1].substack[3].variable_name = sjit_string_new("before_child");
    procedure.statements[1].substack[3].value = literalNumber(1.0);
    procedure.statements[1].substack[4].opcode = SJIT_STMT_PROCEDURE_CALL;
    procedure.statements[1].substack[4].procedure_name = sjit_string_new("child");
    procedure.statements[1].substack[5].opcode = SJIT_STMT_CHANGE_VARIABLE;
    procedure.statements[1].substack[5].variable_name = sjit_string_new("after_child");
    procedure.statements[1].substack[5].value = literalNumber(1.0);
    procedure.statements[1].substack[6].opcode = SJIT_STMT_MOTION_CHANGE_X;
    procedure.statements[1].substack[6].value = literalNumber(1.0);

    procedure.statements[2].opcode = SJIT_STMT_WHILE;
    procedure.statements[2].condition = literalBool(false);
    procedure.statements[2].substack_count = 1;
    procedure.statements[2].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(procedure.statements[2].substack, "allocate non-warp while body");
    procedure.statements[2].substack[0].opcode = SJIT_STMT_CHANGE_VARIABLE;
    procedure.statements[2].substack[0].variable_name = sjit_string_new("while_bumps");
    procedure.statements[2].substack[0].value = literalNumber(1.0);

    procedure.statements[3].opcode = SJIT_STMT_REPEAT_UNTIL;
    procedure.statements[3].condition = literalBool(true);
    procedure.statements[3].substack_count = 1;
    procedure.statements[3].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(procedure.statements[3].substack, "allocate non-warp repeat-until body");
    procedure.statements[3].substack[0].opcode = SJIT_STMT_CHANGE_VARIABLE;
    procedure.statements[3].substack[0].variable_name = sjit_string_new("until_bumps");
    procedure.statements[3].substack[0].value = literalNumber(1.0);

    procedure.statements[4].opcode = SJIT_STMT_SET_VARIABLE;
    procedure.statements[4].variable_name = sjit_string_new("done");
    procedure.statements[4].value = literalNumber(1.0);

    SCompiledProcedure &child_procedure = script->procedures[1];
    child_procedure.name = sjit_string_new("child");
    child_procedure.warp_mode = 0;
    child_procedure.statement_count = 3;
    child_procedure.statements = static_cast<SStatement *>(std::calloc(3, sizeof(SStatement)));
    require(child_procedure.statements, "allocate yielding child procedure body");
    child_procedure.statements[0].opcode = SJIT_STMT_CHANGE_VARIABLE;
    child_procedure.statements[0].variable_name = sjit_string_new("child_prefix");
    child_procedure.statements[0].value = literalNumber(1.0);
    child_procedure.statements[1].opcode = SJIT_STMT_REPEAT;
    child_procedure.statements[1].times = literalNumber(1.0);
    child_procedure.statements[1].substack_count = 1;
    child_procedure.statements[1].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(child_procedure.statements[1].substack, "allocate yielding child repeat body");
    child_procedure.statements[1].substack[0].opcode = SJIT_STMT_MOTION_CHANGE_X;
    child_procedure.statements[1].substack[0].value = literalNumber(1.0);
    child_procedure.statements[2].opcode = SJIT_STMT_SET_VARIABLE;
    child_procedure.statements[2].variable_name = sjit_string_new("child_done");
    child_procedure.statements[2].value = literalNumber(1.0);

    SCompiledProcedure &loop_coverage = script->procedures[2];
    loop_coverage.name = sjit_string_new("loop coverage");
    loop_coverage.warp_mode = 0;
    loop_coverage.statement_count = 2;
    loop_coverage.statements = static_cast<SStatement *>(std::calloc(2, sizeof(SStatement)));
    require(loop_coverage.statements, "allocate non-warp loop coverage procedure body");
    loop_coverage.statements[0].opcode = SJIT_STMT_FOR_EACH;
    loop_coverage.statements[0].variable_name = sjit_string_new("j");
    loop_coverage.statements[0].times = literalNumber(2.0);
    loop_coverage.statements[0].substack_count = 1;
    loop_coverage.statements[0].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(loop_coverage.statements[0].substack, "allocate non-warp for-each coverage body");
    loop_coverage.statements[0].substack[0].opcode = SJIT_STMT_CHANGE_VARIABLE;
    loop_coverage.statements[0].substack[0].variable_name = sjit_string_new("for_each_bumps");
    loop_coverage.statements[0].substack[0].value = literalNumber(1.0);
    loop_coverage.statements[1].opcode = SJIT_STMT_FOREVER;
    loop_coverage.statements[1].substack_count = 1;
    loop_coverage.statements[1].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(loop_coverage.statements[1].substack, "allocate non-warp forever coverage body");
    loop_coverage.statements[1].substack[0].opcode = SJIT_STMT_NOOP;

    sjit::JitEngine jit;
    const std::string ir_path = "/tmp/xyo-jit-non-warp-procedure.ll";
    jit.emitScriptLl(*script, "sjit_unit_non_warp_procedure_ir", ir_path);
    std::ifstream ir_file(ir_path);
    const std::string ir((std::istreambuf_iterator<char>(ir_file)), std::istreambuf_iterator<char>());
    require(
        ir.find("define i32 @sjit_unit_non_warp_procedure_ir_procedure_0") != std::string::npos,
        "JIT IR defines non-warp procedure variant");
    require(
        ir.find("procedure_repeat_nonwarp_condition") != std::string::npos &&
            ir.find("procedure_repeat_action") == std::string::npos,
        "non-warp procedure repeat lowers directly");
    require(
        ir.find("procedure_loop_nonwarp_condition") != std::string::npos &&
            ir.find("procedure_loop_action") == std::string::npos,
        "non-warp procedure conditional loops lower directly");
    require(
        ir.find("procedure_for_each_nonwarp_condition") != std::string::npos &&
            ir.find("procedure_for_each_action") == std::string::npos,
        "non-warp procedure for-each lowers directly");
    require(
        ir.find("procedure_forever_nonwarp_condition") != std::string::npos &&
            ir.find("procedure_forever_action") == std::string::npos,
        "non-warp procedure forever lowers directly");

    SScriptEntryFn entry = jit.compileScript(*script, "sjit_unit_non_warp_procedure_entry");
    require(entry, "compile non-warp procedure jit script");
    require(script->procedures[0].jit_entry, "compile stores non-warp procedure jit entry");
    require(script->procedures[1].jit_entry, "compile stores yielding child procedure jit entry");
    require(script->procedures[2].jit_entry, "compile stores non-warp loop coverage procedure jit entry");
    const int registered = sjit_runtime_register_script_with_data(
        runtime,
        sprite->base.id,
        44,
        SJIT_HAT_EVENT_WHENFLAGCLICKED,
        "",
        1,
        0,
        entry,
        script);
    require(registered, "register non-warp procedure jit script");

    sjit_runtime_green_flag(runtime);
    for (int i = 0; i < 16; ++i) {
        sjit_runtime_tick(runtime);
    }

    SVariable *prefix = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "prefix", SJIT_VAR_SCALAR);
    SVariable *bumps = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "bumps", SJIT_VAR_SCALAR);
    SVariable *bumps_twice = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "bumps_twice", SJIT_VAR_SCALAR);
    SVariable *guarded_bumps = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "guarded_bumps", SJIT_VAR_SCALAR);
    SVariable *before_child = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "before_child", SJIT_VAR_SCALAR);
    SVariable *after_child = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "after_child", SJIT_VAR_SCALAR);
    SVariable *child_prefix = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "child_prefix", SJIT_VAR_SCALAR);
    SVariable *child_done = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "child_done", SJIT_VAR_SCALAR);
    SVariable *done = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "done", SJIT_VAR_SCALAR);
    require(prefix && prefix->value.tag == SJIT_VALUE_NUMBER && prefix->value.number == 1.0, "JIT non-warp procedure resumes without re-running prefix");
    require(bumps && bumps->value.tag == SJIT_VALUE_NUMBER && bumps->value.number == 3.0, "JIT non-warp procedure repeat runs");
    require(
        bumps_twice && bumps_twice->value.tag == SJIT_VALUE_NUMBER && bumps_twice->value.number == 6.0,
        "JIT non-warp procedure repeat lowers multiple statements");
    require(
        guarded_bumps && guarded_bumps->value.tag == SJIT_VALUE_NUMBER && guarded_bumps->value.number == 3.0,
        "JIT non-warp procedure repeat lowers nested if body");
    require(
        before_child && before_child->value.tag == SJIT_VALUE_NUMBER && before_child->value.number == 3.0,
        "JIT non-warp procedure loop body resumes before yielding child call");
    require(
        after_child && after_child->value.tag == SJIT_VALUE_NUMBER && after_child->value.number == 3.0,
        "JIT non-warp procedure loop body continues after yielding child call");
    require(
        child_prefix && child_prefix->value.tag == SJIT_VALUE_NUMBER && child_prefix->value.number == 3.0,
        "JIT non-warp child procedure resumes without re-running prefix");
    require(child_done && child_done->value.tag == SJIT_VALUE_NUMBER && child_done->value.number == 1.0, "JIT non-warp child procedure completes");
    require(done && done->value.tag == SJIT_VALUE_NUMBER && done->value.number == 1.0, "JIT non-warp procedure continues after repeat");

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_jit_compiles_wide_procedure_arguments() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "WideProcSprite", 0);
    require(sprite, "create wide procedure jit sprite");

    const int argument_count = SJIT_MAX_PARAMS + 1;
    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 1);
    require(script, "create wide procedure jit script");
    script->statements[0].opcode = SJIT_STMT_PROCEDURE_CALL;
    script->statements[0].procedure_name = sjit_string_new("wide");
    script->statements[0].argument_count = argument_count;
    script->statements[0].arguments = static_cast<SArgumentExpr *>(
        std::calloc((size_t)argument_count, sizeof(SArgumentExpr)));
    require(script->statements[0].arguments, "allocate wide procedure call arguments");
    for (int i = 0; i < argument_count; ++i) {
        const std::string name = "a" + std::to_string(i);
        script->statements[0].arguments[i].name = sjit_string_new(name.c_str());
        script->statements[0].arguments[i].value = literalNumber(1000.0 + (double)i);
    }

    script->procedure_count = 1;
    script->procedures = static_cast<SCompiledProcedure *>(std::calloc(1, sizeof(SCompiledProcedure)));
    require(script->procedures, "allocate wide jit procedure");
    SCompiledProcedure &procedure = script->procedures[0];
    procedure.name = sjit_string_new("wide");
    procedure.argument_count = argument_count;
    procedure.argument_names = static_cast<SString **>(
        std::calloc((size_t)argument_count, sizeof(SString *)));
    require(procedure.argument_names, "allocate wide procedure argument names");
    for (int i = 0; i < argument_count; ++i) {
        const std::string name = "a" + std::to_string(i);
        procedure.argument_names[i] = sjit_string_new(name.c_str());
    }
    procedure.statement_count = 1;
    procedure.statements = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(procedure.statements, "allocate wide procedure body");
    procedure.statements[0].opcode = SJIT_STMT_SET_VARIABLE;
    procedure.statements[0].variable_name = sjit_string_new("wide_last");
    const std::string last_name = "a" + std::to_string(argument_count - 1);
    procedure.statements[0].value = sjit_expr_create_argument(last_name.c_str());

    sjit::JitEngine jit;
    const std::string ir_path = "/tmp/xyo-jit-wide-procedure.ll";
    jit.emitScriptLl(*script, "sjit_unit_wide_procedure_ir", ir_path);
    std::ifstream ir_file(ir_path);
    const std::string ir((std::istreambuf_iterator<char>(ir_file)), std::istreambuf_iterator<char>());
    require(
        ir.find("define i32 @sjit_unit_wide_procedure_ir_procedure_0") != std::string::npos,
        "JIT IR defines wide procedure variant");
    require(
        ir.find("call i32 @sjit_unit_wide_procedure_ir_procedure_0") != std::string::npos,
        "JIT IR directly calls wide procedure variant");

    SScriptEntryFn entry = jit.compileScript(*script, "sjit_unit_wide_procedure_entry");
    require(entry, "compile wide procedure jit script");
    require(script->procedures[0].jit_entry, "compile stores wide procedure jit entry");
    const int registered = sjit_runtime_register_script_with_data(
        runtime,
        sprite->base.id,
        45,
        SJIT_HAT_EVENT_WHENFLAGCLICKED,
        "",
        1,
        0,
        entry,
        script);
    require(registered, "register wide procedure jit script");

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);

    SVariable *last = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "wide_last", SJIT_VAR_SCALAR);
    require(
        last && last->value.tag == SJIT_VALUE_NUMBER &&
            last->value.number == 1000.0 + (double)(argument_count - 1),
        "JIT wide procedure passes arguments beyond fixed frame param storage");

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_jit_batched_loop_yields_in_warp_mode() {
    SRuntime *runtime = sjit_runtime_create();
    require(runtime, "create warp loop runtime");
    runtime->turbo_mode = 1;

    SFrame frame;
    sjit_frame_init(&frame);
    frame.warp_mode = 1;

    SLoopState state{};
    state.branch_active = 1;
    state.sub_pc = 3;
    state.scope_depth = 2;

    const int loop_batch = 16384;
    SRuntimeStatus status = sjit_jit_finish_batched_loop_branch(
        runtime,
        &frame,
        &state,
        loop_batch - 1);
    require(status == SJIT_STATUS_OK, "warp loop should continue before batch boundary");

    state.branch_active = 1;
    state.sub_pc = 3;
    status = sjit_jit_finish_batched_loop_branch(runtime, &frame, &state, loop_batch);
    require(status == SJIT_STATUS_YIELDED, "warp loop should yield at batch boundary");
    require(state.branch_active == 0 && state.sub_pc == 0, "warp loop batch should reset branch state");

    runtime->turbo_mode = 0;
    runtime->redraw_requested = 1;
    runtime->thread_count = 2;
    status = sjit_jit_finish_batched_loop_branch(runtime, &frame, &state, loop_batch - 1);
    require(status == SJIT_STATUS_OK, "warp loop should ignore redraw and sibling-thread yields before batch boundary");

    frame.warp_mode = 0;
    status = sjit_jit_finish_batched_loop_branch(runtime, &frame, &state, loop_batch - 1);
    require(status == SJIT_STATUS_YIELDED, "non-warp loop should still yield after a redraw request");
    runtime->thread_count = 0;

    sjit_runtime_destroy(runtime);
}

void test_jit_fallback_warp_procedure_finishes_redraw_loop_in_one_tick() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "FallbackWarpSprite", 0);
    require(sprite, "create fallback warp procedure sprite");

    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 1);
    require(script, "create fallback warp procedure script");
    script->statements[0].opcode = SJIT_STMT_PROCEDURE_CALL;
    script->statements[0].procedure_name = sjit_string_new("draw");

    script->procedure_count = 2;
    script->procedures = static_cast<SCompiledProcedure *>(std::calloc(2, sizeof(SCompiledProcedure)));
    require(script->procedures, "allocate fallback warp procedures");

    SCompiledProcedure &draw = script->procedures[0];
    draw.name = sjit_string_new("draw");
    draw.warp_mode = 1;
    draw.statement_count = 2;
    draw.statements = static_cast<SStatement *>(std::calloc(2, sizeof(SStatement)));
    require(draw.statements, "allocate fallback warp draw body");
    draw.statements[0].opcode = SJIT_STMT_REPEAT;
    draw.statements[0].times = literalNumber(3.0);
    draw.statements[0].substack_count = 1;
    draw.statements[0].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(draw.statements[0].substack, "allocate fallback warp redraw loop");
    draw.statements[0].substack[0].opcode = SJIT_STMT_MOTION_CHANGE_X;
    draw.statements[0].substack[0].value = literalNumber(1.0);
    draw.statements[1].opcode = SJIT_STMT_PROCEDURE_CALL;
    draw.statements[1].procedure_name = sjit_string_new("unsupported");

    SCompiledProcedure &unsupported = script->procedures[1];
    unsupported.name = sjit_string_new("unsupported");
    unsupported.warp_mode = 1;
    unsupported.statement_count = 1;
    unsupported.statements = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(unsupported.statements, "allocate unsupported fallback warp body");
    unsupported.statements[0].opcode = 999;

    sjit::JitEngine jit;
    SScriptEntryFn entry = jit.compileScript(*script, "sjit_unit_fallback_warp_redraw_entry");
    require(entry, "compile fallback warp redraw script");
    require(!script->procedures[0].jit_entry, "fallback warp draw procedure should use the interpreter");
    const int registered = sjit_runtime_register_script_with_data(
        runtime,
        sprite->base.id,
        49,
        SJIT_HAT_EVENT_WHENFLAGCLICKED,
        "",
        1,
        0,
        entry,
        script);
    require(registered, "register fallback warp redraw script");

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);

    require(sprite->x == 3.0, "fallback warp redraw loop should finish in one runtime tick");
    require(runtime->thread_count == 0, "fallback warp procedure should finish without redraw yielding");

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_jit_procedure_call_noops_for_missing_target() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "MissingCallProcSprite", 0);
    require(sprite, "create missing-call procedure jit sprite");

    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 1);
    require(script, "create missing-call procedure jit script");
    script->statements[0].opcode = SJIT_STMT_PROCEDURE_CALL;
    script->statements[0].procedure_name = sjit_string_new("wrapper");

    script->procedure_count = 2;
    script->procedures = static_cast<SCompiledProcedure *>(std::calloc(2, sizeof(SCompiledProcedure)));
    require(script->procedures, "allocate missing-call jit procedure");
    SCompiledProcedure &procedure = script->procedures[0];
    procedure.name = sjit_string_new("wrapper");
    procedure.warp_mode = 1;
    procedure.statement_count = 2;
    procedure.statements = static_cast<SStatement *>(std::calloc(2, sizeof(SStatement)));
    require(procedure.statements, "allocate missing-call procedure body");

    procedure.statements[0].opcode = SJIT_STMT_PROCEDURE_CALL;
    procedure.statements[0].procedure_name = sjit_string_new("missing");
    procedure.statements[1].opcode = SJIT_STMT_SET_VARIABLE;
    procedure.statements[1].variable_name = sjit_string_new("after_missing");
    procedure.statements[1].value = literalNumber(1.0);

    SCompiledProcedure &unused = script->procedures[1];
    unused.name = sjit_string_new("unused");
    unused.warp_mode = 1;
    unused.statement_count = 1;
    unused.statements = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(unused.statements, "allocate unreachable procedure body");
    unused.statements[0].opcode = SJIT_STMT_SET_VARIABLE;
    unused.statements[0].variable_name = sjit_string_new("unused_value");
    unused.statements[0].value = literalNumber(1.0);

    sjit::JitEngine jit;
    const std::string ir_path = "/tmp/xyo-jit-missing-call-procedure.ll";
    jit.emitScriptLl(*script, "sjit_unit_missing_call_procedure_ir", ir_path);
    std::ifstream ir_file(ir_path);
    const std::string ir((std::istreambuf_iterator<char>(ir_file)), std::istreambuf_iterator<char>());
    require(
        ir.find("define i32 @sjit_unit_missing_call_procedure_ir_procedure_0") != std::string::npos,
        "JIT IR defines procedure with missing nested call");
    require(
        ir.find("define i32 @sjit_unit_missing_call_procedure_ir_procedure_1") == std::string::npos,
        "JIT IR omits unreachable procedure variant");
    require(
        ir.find("procedure_call_action") == std::string::npos,
        "missing nested procedure call lowers to no-op without fallback");

    SScriptEntryFn entry = jit.compileScript(*script, "sjit_unit_missing_call_procedure_entry");
    require(entry, "compile missing-call procedure jit script");
    require(script->procedures[0].jit_entry, "compile stores missing-call procedure jit entry");
    require(!script->procedures[1].jit_entry, "compile leaves unreachable procedure without JIT entry");
    const int registered = sjit_runtime_register_script_with_data(
        runtime,
        sprite->base.id,
        46,
        SJIT_HAT_EVENT_WHENFLAGCLICKED,
        "",
        1,
        0,
        entry,
        script);
    require(registered, "register missing-call procedure jit script");

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);

    SVariable *after = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "after_missing", SJIT_VAR_SCALAR);
    require(after && after->value.tag == SJIT_VALUE_NUMBER && after->value.number == 1.0, "JIT procedure continues after missing nested call");

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_jit_top_level_procedure_call_falls_back_for_non_direct_procedure() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "FallbackProcSprite", 0);
    require(sprite, "create fallback procedure jit sprite");

    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 1);
    require(script, "create fallback procedure jit script");
    script->statements[0].opcode = SJIT_STMT_PROCEDURE_CALL;
    script->statements[0].procedure_name = sjit_string_new("wrapper");

    script->procedure_count = 2;
    script->procedures = static_cast<SCompiledProcedure *>(std::calloc(2, sizeof(SCompiledProcedure)));
    require(script->procedures, "allocate fallback jit procedures");

    SCompiledProcedure &wrapper = script->procedures[0];
    wrapper.name = sjit_string_new("wrapper");
    wrapper.warp_mode = 1;
    wrapper.statement_count = 2;
    wrapper.statements = static_cast<SStatement *>(std::calloc(2, sizeof(SStatement)));
    require(wrapper.statements, "allocate fallback wrapper procedure body");
    wrapper.statements[0].opcode = SJIT_STMT_PROCEDURE_CALL;
    wrapper.statements[0].procedure_name = sjit_string_new("unsupported");
    wrapper.statements[1].opcode = SJIT_STMT_SET_VARIABLE;
    wrapper.statements[1].variable_name = sjit_string_new("fallback_ran");
    wrapper.statements[1].value = literalNumber(7.0);

    SCompiledProcedure &unsupported = script->procedures[1];
    unsupported.name = sjit_string_new("unsupported");
    unsupported.warp_mode = 1;
    unsupported.statement_count = 1;
    unsupported.statements = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(unsupported.statements, "allocate unsupported fallback procedure body");
    unsupported.statements[0].opcode = 999;

    sjit::JitEngine jit;
    const std::string ir_path = "/tmp/xyo-jit-procedure-fallback.ll";
    jit.emitScriptLl(*script, "sjit_unit_procedure_fallback_ir", ir_path);
    std::ifstream ir_file(ir_path);
    const std::string ir((std::istreambuf_iterator<char>(ir_file)), std::istreambuf_iterator<char>());
    require(
        ir.find("procedure_fallback_status") != std::string::npos &&
            ir.find("sjit_script_execute_procedure_statement") != std::string::npos,
        "top-level procedure call should fall back when target is not directly JIT-able");

    SScriptEntryFn entry = jit.compileScript(*script, "sjit_unit_procedure_fallback_entry");
    require(entry, "compile fallback procedure jit script");
    require(!script->procedures[0].jit_entry, "non-direct wrapper procedure should not get a jit entry");
    require(!script->procedures[1].jit_entry, "unsupported procedure should not get a jit entry");
    const int registered = sjit_runtime_register_script_with_data(
        runtime,
        sprite->base.id,
        47,
        SJIT_HAT_EVENT_WHENFLAGCLICKED,
        "",
        1,
        0,
        entry,
        script);
    require(registered, "register fallback procedure jit script");

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);

    SVariable *result = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "fallback_ran", SJIT_VAR_SCALAR);
    require(
        result && result->value.tag == SJIT_VALUE_NUMBER && result->value.number == 7.0,
        "top-level fallback executes non-direct procedure through the interpreter");

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_jit_fallback_procedure_call_resumes_after_yield() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "FallbackResumeSprite", 0);
    require(sprite, "create fallback resume procedure jit sprite");

    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 1);
    require(script, "create fallback resume procedure jit script");
    script->statements[0].opcode = SJIT_STMT_PROCEDURE_CALL;
    script->statements[0].procedure_name = sjit_string_new("wrapper");

    script->procedure_count = 2;
    script->procedures = static_cast<SCompiledProcedure *>(std::calloc(2, sizeof(SCompiledProcedure)));
    require(script->procedures, "allocate fallback resume jit procedures");

    SCompiledProcedure &wrapper = script->procedures[0];
    wrapper.name = sjit_string_new("wrapper");
    wrapper.warp_mode = 0;
    wrapper.statement_count = 4;
    wrapper.statements = static_cast<SStatement *>(std::calloc(4, sizeof(SStatement)));
    require(wrapper.statements, "allocate fallback resume wrapper procedure body");
    wrapper.statements[0].opcode = SJIT_STMT_CHANGE_VARIABLE;
    wrapper.statements[0].variable_name = sjit_string_new("starts");
    wrapper.statements[0].value = literalNumber(1.0);
    wrapper.statements[1].opcode = SJIT_STMT_REPEAT;
    wrapper.statements[1].times = literalNumber(3.0);
    wrapper.statements[1].substack_count = 1;
    wrapper.statements[1].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(wrapper.statements[1].substack, "allocate fallback resume repeat body");
    wrapper.statements[1].substack[0].opcode = SJIT_STMT_CHANGE_VARIABLE;
    wrapper.statements[1].substack[0].variable_name = sjit_string_new("count");
    wrapper.statements[1].substack[0].value = literalNumber(1.0);
    wrapper.statements[2].opcode = SJIT_STMT_SET_VARIABLE;
    wrapper.statements[2].variable_name = sjit_string_new("after");
    wrapper.statements[2].value = literalNumber(1.0);
    wrapper.statements[3].opcode = SJIT_STMT_PROCEDURE_CALL;
    wrapper.statements[3].procedure_name = sjit_string_new("unsupported");

    SCompiledProcedure &unsupported = script->procedures[1];
    unsupported.name = sjit_string_new("unsupported");
    unsupported.warp_mode = 1;
    unsupported.statement_count = 1;
    unsupported.statements = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(unsupported.statements, "allocate unsupported fallback resume procedure body");
    unsupported.statements[0].opcode = 999;

    sjit::JitEngine jit;
    SScriptEntryFn entry = jit.compileScript(*script, "sjit_unit_procedure_fallback_resume_entry");
    require(entry, "compile fallback resume procedure jit script");
    require(!script->procedures[0].jit_entry, "fallback resume wrapper procedure should not get a jit entry");
    const int registered = sjit_runtime_register_script_with_data(
        runtime,
        sprite->base.id,
        48,
        SJIT_HAT_EVENT_WHENFLAGCLICKED,
        "",
        1,
        0,
        entry,
        script);
    require(registered, "register fallback resume procedure jit script");

    sjit_runtime_green_flag(runtime);
    for (int i = 0; i < 5 && sjit_runtime_has_threads(runtime); ++i) {
        sjit_runtime_tick(runtime);
    }

    SVariable *starts = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "starts", SJIT_VAR_SCALAR);
    SVariable *count = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "count", SJIT_VAR_SCALAR);
    SVariable *after = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "after", SJIT_VAR_SCALAR);
    require(
        starts && starts->value.tag == SJIT_VALUE_NUMBER && starts->value.number == 1.0,
        "fallback procedure should not rerun statements before a yielding repeat");
    require(
        count && count->value.tag == SJIT_VALUE_NUMBER && count->value.number == 3.0,
        "fallback procedure should resume and finish yielding repeat body");
    require(
        after && after->value.tag == SJIT_VALUE_NUMBER && after->value.number == 1.0,
        "fallback procedure should continue after yielding repeat");

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_jit_procedure_lowers_numeric_effect_statements() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "EffectProcSprite", 0);
    require(sprite, "create numeric-effect procedure jit sprite");

    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 1);
    require(script, "create numeric-effect procedure jit script");
    script->statements[0].opcode = SJIT_STMT_PROCEDURE_CALL;
    script->statements[0].procedure_name = sjit_string_new("effect %s");
    script->statements[0].argument_count = 1;
    script->statements[0].arguments = static_cast<SArgumentExpr *>(std::calloc(1, sizeof(SArgumentExpr)));
    require(script->statements[0].arguments, "allocate numeric-effect procedure call argument");
    script->statements[0].arguments[0].name = sjit_string_new("n");
    script->statements[0].arguments[0].value = literalNumber(7.0);

    script->procedure_count = 1;
    script->procedures = static_cast<SCompiledProcedure *>(std::calloc(1, sizeof(SCompiledProcedure)));
    require(script->procedures, "allocate numeric-effect jit procedure");
    SCompiledProcedure &procedure = script->procedures[0];
    procedure.name = sjit_string_new("effect %s");
    procedure.warp_mode = 1;
    procedure.argument_count = 1;
    procedure.argument_names = static_cast<SString **>(std::calloc(1, sizeof(SString *)));
    require(procedure.argument_names, "allocate numeric-effect procedure argument names");
    procedure.argument_names[0] = sjit_string_new("n");
    procedure.statement_count = 7;
    procedure.statements = static_cast<SStatement *>(std::calloc(7, sizeof(SStatement)));
    require(procedure.statements, "allocate numeric-effect procedure body");

    procedure.statements[0].opcode = SJIT_STMT_PEN_SET_SIZE;
    procedure.statements[0].value = sjit_expr_create_argument("n");

    procedure.statements[1].opcode = SJIT_STMT_MOTION_GOTO_XY;
    procedure.statements[1].value = sjit_expr_create_argument("n");
    procedure.statements[1].index = sjit_expr_create_binary(SJIT_EXPR_ADD, sjit_expr_create_argument("n"), literalNumber(3.0));

    procedure.statements[2].opcode = SJIT_STMT_MOTION_SET_X;
    procedure.statements[2].value = sjit_expr_create_binary(SJIT_EXPR_ADD, sjit_expr_create_argument("n"), literalNumber(10.0));

    procedure.statements[3].opcode = SJIT_STMT_MOTION_SET_Y;
    procedure.statements[3].value = sjit_expr_create_binary(SJIT_EXPR_ADD, sjit_expr_create_argument("n"), literalNumber(20.0));

    procedure.statements[4].opcode = SJIT_STMT_MOTION_CHANGE_X;
    procedure.statements[4].value = literalNumber(2.0);

    procedure.statements[5].opcode = SJIT_STMT_MOTION_CHANGE_Y;
    procedure.statements[5].value = literalNumber(3.0);

    procedure.statements[6].opcode = SJIT_STMT_LOOKS_SET_SIZE;
    procedure.statements[6].value = sjit_expr_create_binary(SJIT_EXPR_MUL, sjit_expr_create_argument("n"), literalNumber(10.0));

    sjit::JitEngine jit;
    const std::string ir_path = "/tmp/xyo-jit-procedure-effects.ll";
    jit.emitScriptLl(*script, "sjit_unit_procedure_effects_ir", ir_path);
    std::ifstream ir_file(ir_path);
    const std::string ir((std::istreambuf_iterator<char>(ir_file)), std::istreambuf_iterator<char>());
    require(
        ir.find("call void @sjit_jit_pen_set_size_number") != std::string::npos,
        "procedure IR lowers pen size directly");
    require(
        ir.find("procedure_motion_next_x") != std::string::npos,
        "procedure IR lowers motion x directly");
    require(
        ir.find("procedure_motion_next_y") != std::string::npos,
        "procedure IR lowers motion y directly");
    require(
        ir.find("sjit_jit_sprite_set_size") != std::string::npos,
        "procedure IR lowers looks size directly");
    require(
        ir.find("procedure_statement_action") == std::string::npos,
        "numeric-effect procedure IR should not use statement action fallback");

    SScriptEntryFn entry = jit.compileScript(*script, "sjit_unit_procedure_effects_entry");
    require(entry, "compile numeric-effect procedure jit script");
    require(script->procedures[0].jit_entry, "compile stores numeric-effect procedure jit entry");
    const int registered = sjit_runtime_register_script_with_data(
        runtime,
        sprite->base.id,
        47,
        SJIT_HAT_EVENT_WHENFLAGCLICKED,
        "",
        1,
        0,
        entry,
        script);
    require(registered, "register numeric-effect procedure jit script");

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);

    require(sprite->pen_size == 7.0, "JIT procedure directly sets pen size");
    require(sprite->x == 19.0, "JIT procedure directly updates x");
    require(sprite->y == 30.0, "JIT procedure directly updates y");
    require(sprite->size == 70.0, "JIT procedure directly sets looks size");

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_jit_procedure_lowers_numeric_list_mutations() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "ListProcSprite", 0);
    require(sprite, "create numeric-list procedure jit sprite");

    SVariable *numbers_variable = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "numbers", SJIT_VAR_LIST);
    require(numbers_variable && numbers_variable->value.tag == SJIT_VALUE_LIST, "create numeric-list procedure list");

    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 1);
    require(script, "create numeric-list procedure jit script");
    script->statements[0].opcode = SJIT_STMT_PROCEDURE_CALL;
    script->statements[0].procedure_name = sjit_string_new("mutate list");

    script->procedure_count = 1;
    script->procedures = static_cast<SCompiledProcedure *>(std::calloc(1, sizeof(SCompiledProcedure)));
    require(script->procedures, "allocate numeric-list jit procedure");
    SCompiledProcedure &procedure = script->procedures[0];
    procedure.name = sjit_string_new("mutate list");
    procedure.warp_mode = 1;
    procedure.statement_count = 9;
    procedure.statements = static_cast<SStatement *>(std::calloc(9, sizeof(SStatement)));
    require(procedure.statements, "allocate numeric-list procedure body");

    procedure.statements[0].opcode = SJIT_STMT_LIST_ADD;
    procedure.statements[0].variable_name = sjit_string_new("numbers");
    procedure.statements[0].value = literalNumber(1.0);

    procedure.statements[1].opcode = SJIT_STMT_LIST_ADD;
    procedure.statements[1].variable_name = sjit_string_new("numbers");
    procedure.statements[1].value = literalNumber(2.0);

    procedure.statements[2].opcode = SJIT_STMT_LIST_ADD;
    procedure.statements[2].variable_name = sjit_string_new("numbers");
    procedure.statements[2].value = literalNumber(3.0);

    procedure.statements[3].opcode = SJIT_STMT_LIST_DELETE;
    procedure.statements[3].variable_name = sjit_string_new("numbers");
    procedure.statements[3].index = literalNumber(2.0);

    procedure.statements[4].opcode = SJIT_STMT_LIST_INSERT;
    procedure.statements[4].variable_name = sjit_string_new("numbers");
    procedure.statements[4].index = literalNumber(2.0);
    procedure.statements[4].value = literalNumber(9.0);

    procedure.statements[5].opcode = SJIT_STMT_LIST_REPLACE;
    procedure.statements[5].variable_name = sjit_string_new("numbers");
    procedure.statements[5].index = literalNumber(1.0);
    procedure.statements[5].value = literalNumber(5.0);

    procedure.statements[6].opcode = SJIT_STMT_LIST_REPLACE;
    procedure.statements[6].variable_name = sjit_string_new("numbers");
    procedure.statements[6].index = literalString("last");
    procedure.statements[6].value = sjit_expr_create_binary(
        SJIT_EXPR_DIV,
        literalNumber(12.0),
        literalNumber(2.0));

    procedure.statements[7].opcode = SJIT_STMT_LIST_DELETE_ALL;
    procedure.statements[7].variable_name = sjit_string_new("bulk");

    procedure.statements[8].opcode = SJIT_STMT_REPEAT;
    procedure.statements[8].times = literalNumber(4.0);
    procedure.statements[8].substack_count = 1;
    procedure.statements[8].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(procedure.statements[8].substack, "allocate bulk-list repeat body");
    procedure.statements[8].substack[0].opcode = SJIT_STMT_LIST_ADD;
    procedure.statements[8].substack[0].variable_name = sjit_string_new("bulk");
    procedure.statements[8].substack[0].value = literalString("0");

    sjit::JitEngine jit;
    const std::string ir_path = "/tmp/xyo-jit-procedure-list.ll";
    jit.emitScriptLl(*script, "sjit_unit_procedure_list_ir", ir_path);
    std::ifstream ir_file(ir_path);
    const std::string ir((std::istreambuf_iterator<char>(ir_file)), std::istreambuf_iterator<char>());
    require(
        ir.find("sjit_jit_statement_list_delete_at_number") != std::string::npos,
        "procedure IR lowers numeric list delete directly");
    require(
        ir.find("sjit_jit_statement_list_insert_number_at") != std::string::npos,
        "procedure IR lowers numeric list insert directly");
    require(
        ir.find("call void @sjit_jit_statement_list_replace_value_ptr") != std::string::npos,
        "procedure IR preserves the special last list index");
    require(
        ir.find("call i32 @sjit_jit_statement_list_add_literal_repeated") != std::string::npos,
        "procedure IR lowers repeated literal list append in bulk");
    require(
        ir.find("call ptr @sjit_jit_procedure_activation_state") == std::string::npos,
        "non-yielding warp procedure IR omits resumable activation state");
    require(
        ir.find("procedure_list_delete_action") == std::string::npos,
        "numeric-list procedure IR should not fallback for delete");
    require(
        ir.find("procedure_list_insert_action") == std::string::npos,
        "numeric-list procedure IR should not fallback for insert");

    SScriptEntryFn entry = jit.compileScript(*script, "sjit_unit_procedure_list_entry");
    require(entry, "compile numeric-list procedure jit script");
    require(script->procedures[0].jit_entry, "compile stores numeric-list procedure jit entry");
    const int registered = sjit_runtime_register_script_with_data(
        runtime,
        sprite->base.id,
        48,
        SJIT_HAT_EVENT_WHENFLAGCLICKED,
        "",
        1,
        0,
        entry,
        script);
    require(registered, "register numeric-list procedure jit script");

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);

    SList *numbers = static_cast<SList *>(numbers_variable->value.ptr);
    require(sjit_list_length(numbers) == 3, "JIT procedure numeric list mutation length");
    double values[3] = {0.0, 0.0, 0.0};
    for (int i = 1; i <= 3; ++i) {
        SValue item = sjit_list_get(numbers, i);
        values[i - 1] = sjit_to_number(runtime, item);
        sjit_value_destroy(item);
    }
    require(values[0] == 5.0 && values[1] == 9.0 && values[2] == 6.0, "JIT procedure numeric list mutation contents");

    SVariable *bulk_variable = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "bulk", SJIT_VAR_LIST);
    require(bulk_variable && bulk_variable->value.tag == SJIT_VALUE_LIST, "JIT procedure bulk list exists");
    SList *bulk = static_cast<SList *>(bulk_variable->value.ptr);
    require(sjit_list_length(bulk) == 4, "JIT procedure bulk list length");
    SValue bulk_item = sjit_list_get(bulk, 1);
    require(bulk_item.tag == SJIT_VALUE_STRING, "JIT procedure bulk list preserves literal type");
    sjit_value_destroy(bulk_item);

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_jit_procedure_replaces_list_from_variables_without_boxing() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "ListReplaceProcSprite", 0);
    require(sprite, "create variable-list-replace procedure sprite");

    SVariable *items_variable = sjit_runtime_lookup_or_create_variable(
        runtime,
        sprite->base.id,
        "items",
        SJIT_VAR_LIST);
    SVariable *source_variable = sjit_runtime_lookup_or_create_variable(
        runtime,
        sprite->base.id,
        "source",
        SJIT_VAR_LIST);
    require(
        items_variable && items_variable->value.tag == SJIT_VALUE_LIST &&
            source_variable && source_variable->value.tag == SJIT_VALUE_LIST,
        "create variable-list-replace lists");
    SList *items = static_cast<SList *>(items_variable->value.ptr);
    SList *source = static_cast<SList *>(source_variable->value.ptr);
    require(
        sjit_list_push_number(items, 1.0) &&
            sjit_list_push_number(items, 2.0) &&
            sjit_list_push_number(items, 3.0) &&
            sjit_list_push_number(source, 7.0),
        "seed variable-list-replace numeric items");
    SValue copied = sjit_make_string("copied");
    require(sjit_list_push(source, copied), "seed variable-list-replace string item");
    sjit_value_destroy(copied);

    auto scalar = [&](const char *name) {
        return sjit_runtime_lookup_or_create_variable(
            runtime,
            sprite->base.id,
            name,
            SJIT_VAR_SCALAR);
    };
    SVariable *number_index = scalar("number index");
    SVariable *number_value = scalar("number value");
    SVariable *string_index = scalar("string index");
    SVariable *string_value = scalar("string value");
    SVariable *source_destination_index = scalar("source destination index");
    SVariable *source_index = scalar("source index");
    require(
        number_index && number_value && string_index && string_value &&
            source_destination_index && source_index,
        "create variable-list-replace scalars");
    sjit_variable_set_number(number_index, 1.0);
    sjit_variable_set_number(number_value, 9.0);
    sjit_variable_set_move(string_index, sjit_make_string("2"));
    sjit_variable_set_move(string_value, sjit_make_string("hello"));
    sjit_variable_set_number(source_destination_index, 3.0);
    sjit_variable_set_number(source_index, 2.0);

    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 1);
    require(script, "create variable-list-replace procedure script");
    script->statements[0].opcode = SJIT_STMT_PROCEDURE_CALL;
    script->statements[0].procedure_name = sjit_string_new("replace variables");

    script->procedure_count = 1;
    script->procedures = static_cast<SCompiledProcedure *>(std::calloc(1, sizeof(SCompiledProcedure)));
    require(script->procedures, "allocate variable-list-replace procedure");
    SCompiledProcedure &procedure = script->procedures[0];
    procedure.name = sjit_string_new("replace variables");
    procedure.warp_mode = 1;
    procedure.statement_count = 3;
    procedure.statements = static_cast<SStatement *>(std::calloc(3, sizeof(SStatement)));
    require(procedure.statements, "allocate variable-list-replace statements");

    procedure.statements[0].opcode = SJIT_STMT_LIST_REPLACE;
    procedure.statements[0].variable_name = sjit_string_new("items");
    procedure.statements[0].index = sjit_expr_create_variable("number index");
    procedure.statements[0].value = sjit_expr_create_variable("number value");

    procedure.statements[1].opcode = SJIT_STMT_LIST_REPLACE;
    procedure.statements[1].variable_name = sjit_string_new("items");
    procedure.statements[1].index = sjit_expr_create_variable("string index");
    procedure.statements[1].value = sjit_expr_create_variable("string value");

    procedure.statements[2].opcode = SJIT_STMT_LIST_REPLACE;
    procedure.statements[2].variable_name = sjit_string_new("items");
    procedure.statements[2].index = sjit_expr_create_variable("source destination index");
    procedure.statements[2].value = sjit_expr_create_list_item(
        "source",
        sjit_expr_create_variable("source index"));

    sjit::JitEngine jit;
    const std::string ir_path = "/tmp/xyo-jit-procedure-variable-list-replace.ll";
    jit.emitScriptLl(*script, "sjit_unit_procedure_variable_list_replace_ir", ir_path);
    std::ifstream ir_file(ir_path);
    const std::string ir((std::istreambuf_iterator<char>(ir_file)), std::istreambuf_iterator<char>());
    require(
        ir.find("call void @sjit_jit_list_variable_replace_from_variables") != std::string::npos,
        "procedure IR replaces from scalar variables without boxing");
    require(
        ir.find("call void @sjit_jit_list_variable_replace_list_item_at_variables") != std::string::npos,
        "procedure IR replaces from list items without boxing");
    require(
        ir.find("call void @sjit_jit_statement_list_replace_value_ptr") == std::string::npos,
        "variable list replacement avoids boxed value helper");
    require(
        ir.find("call void @sjit_jit_statement_list_replace_list_item_at_variables") == std::string::npos,
        "variable list-item replacement avoids expression lookup helper");

    SScriptEntryFn entry = jit.compileScript(
        *script,
        "sjit_unit_procedure_variable_list_replace_entry",
        runtime);
    require(entry, "compile variable-list-replace procedure script");
    require(script->procedures[0].jit_entry, "compile stores variable-list-replace procedure entry");
    const int registered = sjit_runtime_register_script_with_data(
        runtime,
        sprite->base.id,
        49,
        SJIT_HAT_EVENT_WHENFLAGCLICKED,
        "",
        1,
        0,
        entry,
        script);
    require(registered, "register variable-list-replace procedure script");

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);

    SValue item = sjit_list_get(items, 1);
    require(
        item.tag == SJIT_VALUE_NUMBER && item.number == 9.0,
        "numeric variable list replacement preserves number");
    sjit_value_destroy(item);
    item = sjit_list_get(items, 2);
    require(
        item.tag == SJIT_VALUE_STRING &&
            std::string(sjit_string_cstr(static_cast<const SString *>(item.ptr))) == "hello",
        "variable list replacement preserves string fallback");
    sjit_value_destroy(item);
    item = sjit_list_get(items, 3);
    require(
        item.tag == SJIT_VALUE_STRING &&
            std::string(sjit_string_cstr(static_cast<const SString *>(item.ptr))) == "copied",
        "list-item variable replacement preserves string fallback");
    sjit_value_destroy(item);

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_jit_procedure_expression_slot_fallbacks() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "ProcExprSprite", 0);
    require(sprite, "create procedure expression jit sprite");

    SVariable *items_variable = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "items", SJIT_VAR_LIST);
    require(items_variable && items_variable->value.tag == SJIT_VALUE_LIST, "create procedure expression list");
    SList *items = static_cast<SList *>(items_variable->value.ptr);
    SValue needle = sjit_make_string("needle");
    require(sjit_list_push(items, needle), "seed procedure expression list");
    sjit_value_destroy(needle);
    SVariable *numbers_variable = sjit_runtime_lookup_or_create_variable(
        runtime,
        sprite->base.id,
        "numbers",
        SJIT_VAR_LIST);
    require(
        numbers_variable && numbers_variable->value.tag == SJIT_VALUE_LIST,
        "create procedure numeric expression list");
    SList *numbers = static_cast<SList *>(numbers_variable->value.ptr);
    require(sjit_list_push_number(numbers, 42.0), "seed procedure numeric expression list");
    SVariable *space_key = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "space_key", SJIT_VAR_SCALAR);
    require(space_key, "create dynamic key variable");
    sjit_variable_set_move(space_key, sjit_make_string("space"));
    runtime->input.key_down[' '] = 1;

    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 2);
    require(script, "create procedure expression jit script");
    script->statements[0].opcode = SJIT_STMT_PROCEDURE_CALL;
    script->statements[0].procedure_name = sjit_string_new("expr %s");
    script->statements[0].argument_count = 1;
    script->statements[0].arguments = static_cast<SArgumentExpr *>(std::calloc(1, sizeof(SArgumentExpr)));
    require(script->statements[0].arguments, "allocate procedure expression call argument");
    script->statements[0].arguments[0].name = sjit_string_new("n");
    script->statements[0].arguments[0].value = literalNumber(2.4);

    script->statements[1].opcode = SJIT_STMT_PROCEDURE_CALL;
    script->statements[1].procedure_name = sjit_string_new("store top %s");
    script->statements[1].argument_count = 1;
    script->statements[1].arguments = static_cast<SArgumentExpr *>(std::calloc(1, sizeof(SArgumentExpr)));
    require(script->statements[1].arguments, "allocate top-level boxed call argument");
    script->statements[1].arguments[0].name = sjit_string_new("text");
    script->statements[1].arguments[0].value = literalString("top!");

    script->procedure_count = 4;
    script->procedures = static_cast<SCompiledProcedure *>(std::calloc(4, sizeof(SCompiledProcedure)));
    require(script->procedures, "allocate expression jit procedure");
    SCompiledProcedure &procedure = script->procedures[0];
    procedure.name = sjit_string_new("expr %s");
    procedure.warp_mode = 1;
    procedure.argument_count = 1;
    procedure.argument_names = static_cast<SString **>(std::calloc(1, sizeof(SString *)));
    require(procedure.argument_names, "allocate expression procedure argument names");
    procedure.argument_names[0] = sjit_string_new("n");
    procedure.statement_count = 24;
    procedure.statements = static_cast<SStatement *>(std::calloc(24, sizeof(SStatement)));
    require(procedure.statements, "allocate expression procedure body");

    procedure.statements[0].opcode = SJIT_STMT_SET_VARIABLE;
    procedure.statements[0].variable_name = sjit_string_new("rounded");
    procedure.statements[0].value = sjit_expr_create_unary(SJIT_EXPR_ROUND, sjit_expr_create_argument("n"));

    procedure.statements[1].opcode = SJIT_STMT_SET_VARIABLE;
    procedure.statements[1].variable_name = sjit_string_new("text_len");
    procedure.statements[1].value = sjit_expr_create_unary(SJIT_EXPR_LENGTH, literalString("abcd"));

    procedure.statements[2].opcode = SJIT_STMT_IF;
    procedure.statements[2].condition = sjit_expr_create_list_contains("items", literalString("needle"));
    procedure.statements[2].substack_count = 1;
    procedure.statements[2].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(procedure.statements[2].substack, "allocate expression procedure branch");
    procedure.statements[2].substack[0].opcode = SJIT_STMT_SET_VARIABLE;
    procedure.statements[2].substack[0].variable_name = sjit_string_new("found");
    procedure.statements[2].substack[0].value = literalNumber(1.0);

    procedure.statements[3].opcode = SJIT_STMT_SET_VARIABLE;
    procedure.statements[3].variable_name = sjit_string_new("label");
    procedure.statements[3].value = sjit_expr_create_binary(SJIT_EXPR_JOIN, literalString("hi"), literalString("!"));

    procedure.statements[4].opcode = SJIT_STMT_FOR_EACH;
    procedure.statements[4].variable_name = sjit_string_new("i");
    procedure.statements[4].times = literalNumber(3.0);
    procedure.statements[4].substack_count = 1;
    procedure.statements[4].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(procedure.statements[4].substack, "allocate expression procedure for-each body");
    procedure.statements[4].substack[0].opcode = SJIT_STMT_CHANGE_VARIABLE;
    procedure.statements[4].substack[0].variable_name = sjit_string_new("sum");
    procedure.statements[4].substack[0].value = sjit_expr_create_variable("i");

    procedure.statements[5].opcode = SJIT_STMT_PROCEDURE_CALL;
    procedure.statements[5].procedure_name = sjit_string_new("store %s");
    procedure.statements[5].argument_count = 1;
    procedure.statements[5].arguments = static_cast<SArgumentExpr *>(std::calloc(1, sizeof(SArgumentExpr)));
    require(procedure.statements[5].arguments, "allocate expression procedure nested call argument");
    procedure.statements[5].arguments[0].name = sjit_string_new("m");
    procedure.statements[5].arguments[0].value = sjit_expr_create_unary(SJIT_EXPR_ROUND, sjit_expr_create_argument("n"));

    procedure.statements[6].opcode = SJIT_STMT_PROCEDURE_CALL;
    procedure.statements[6].procedure_name = sjit_string_new("store text %s");
    procedure.statements[6].argument_count = 1;
    procedure.statements[6].arguments = static_cast<SArgumentExpr *>(std::calloc(1, sizeof(SArgumentExpr)));
    require(procedure.statements[6].arguments, "allocate expression procedure boxed call argument");
    procedure.statements[6].arguments[0].name = sjit_string_new("text");
    procedure.statements[6].arguments[0].value = sjit_expr_create_binary(SJIT_EXPR_JOIN, literalString("box"), literalString("!"));

    procedure.statements[7].opcode = SJIT_STMT_LIST_ADD;
    procedure.statements[7].variable_name = sjit_string_new("items");
    procedure.statements[7].value = sjit_expr_create_binary(SJIT_EXPR_JOIN, literalString("tail"), literalString("!"));

    procedure.statements[8].opcode = SJIT_STMT_LIST_INSERT;
    procedure.statements[8].variable_name = sjit_string_new("items");
    procedure.statements[8].index = literalString("2");
    procedure.statements[8].value = sjit_expr_create_binary(SJIT_EXPR_JOIN, literalString("middle"), literalString("!"));

    procedure.statements[9].opcode = SJIT_STMT_LIST_REPLACE;
    procedure.statements[9].variable_name = sjit_string_new("items");
    procedure.statements[9].index = literalString("last");
    procedure.statements[9].value = sjit_expr_create_binary(SJIT_EXPR_JOIN, literalString("end"), literalString("!"));

    procedure.statements[10].opcode = SJIT_STMT_LIST_DELETE;
    procedure.statements[10].variable_name = sjit_string_new("items");
    procedure.statements[10].index = literalString("2");

    procedure.statements[11].opcode = SJIT_STMT_SAY;
    procedure.statements[11].value = sjit_expr_create_binary(SJIT_EXPR_JOIN, literalString("proc"), literalString(" say"));

    procedure.statements[12].opcode = SJIT_STMT_PEN_SET_COLOR;
    procedure.statements[12].value = literalString("#123456");

    procedure.statements[13].opcode = SJIT_STMT_PEN_CHANGE_COLOR_PARAM;
    procedure.statements[13].index = literalString("transparency");
    procedure.statements[13].value = literalNumber(100.0);

    procedure.statements[14].opcode = SJIT_STMT_SET_VARIABLE;
    procedure.statements[14].variable_name = sjit_string_new("var_copy");
    procedure.statements[14].value = sjit_expr_create_variable("label");

    procedure.statements[15].opcode = SJIT_STMT_SET_VARIABLE;
    procedure.statements[15].variable_name = sjit_string_new("last_item");
    procedure.statements[15].value = sjit_expr_create_list_item("items", literalString("last"));

    procedure.statements[16].opcode = SJIT_STMT_SET_VARIABLE;
    procedure.statements[16].variable_name = sjit_string_new("last_len");
    procedure.statements[16].value = sjit_expr_create_unary(
        SJIT_EXPR_LENGTH,
        sjit_expr_create_list_item("items", literalString("last")));

    procedure.statements[17].opcode = SJIT_STMT_SET_VARIABLE;
    procedure.statements[17].variable_name = sjit_string_new("letter");
    procedure.statements[17].value = sjit_expr_create_binary(
        SJIT_EXPR_LETTER_OF,
        literalNumber(2.0),
        sjit_expr_create_variable("label"));

    procedure.statements[18].opcode = SJIT_STMT_IF;
    procedure.statements[18].condition = sjit_expr_create_binary(SJIT_EXPR_JOIN, literalString("ye"), literalString("s"));
    procedure.statements[18].substack_count = 1;
    procedure.statements[18].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(procedure.statements[18].substack, "allocate value truthy branch");
    procedure.statements[18].substack[0].opcode = SJIT_STMT_SET_VARIABLE;
    procedure.statements[18].substack[0].variable_name = sjit_string_new("truthy_text");
    procedure.statements[18].substack[0].value = literalNumber(1.0);

    procedure.statements[19].opcode = SJIT_STMT_IF;
    procedure.statements[19].condition = sjit_expr_create_key_pressed(sjit_expr_create_variable("space_key"));
    procedure.statements[19].substack_count = 1;
    procedure.statements[19].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(procedure.statements[19].substack, "allocate dynamic key branch");
    procedure.statements[19].substack[0].opcode = SJIT_STMT_SET_VARIABLE;
    procedure.statements[19].substack[0].variable_name = sjit_string_new("key_found");
    procedure.statements[19].substack[0].value = literalNumber(1.0);

    procedure.statements[20].opcode = SJIT_STMT_IF;
    procedure.statements[20].condition = sjit_expr_create_binary(
        SJIT_EXPR_EQ,
        sjit_expr_create_variable("label"),
        literalString("HI!"));
    procedure.statements[20].substack_count = 1;
    procedure.statements[20].substack = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(procedure.statements[20].substack, "allocate dynamic comparison branch");
    procedure.statements[20].substack[0].opcode = SJIT_STMT_SET_VARIABLE;
    procedure.statements[20].substack[0].variable_name = sjit_string_new("dynamic_equal");
    procedure.statements[20].substack[0].value = literalNumber(1.0);

    procedure.statements[21].opcode = SJIT_STMT_SET_VARIABLE;
    procedure.statements[21].variable_name = sjit_string_new("first_item");
    procedure.statements[21].value = sjit_expr_create_list_item("items", literalNumber(1.0));

    procedure.statements[22].opcode = SJIT_STMT_SET_VARIABLE;
    procedure.statements[22].variable_name = sjit_string_new("cached_item");
    procedure.statements[22].value = sjit_expr_create_list_item("numbers", literalNumber(1.0));

    /* Exercise the runtime-specialized four-argument list fast helper through
       a nested numeric index expression, as used heavily by 3D.sb3. */
    procedure.statements[23].opcode = SJIT_STMT_SET_VARIABLE;
    procedure.statements[23].variable_name = sjit_string_new("computed_item");
    procedure.statements[23].value = sjit_expr_create_binary(
        SJIT_EXPR_ADD,
        sjit_expr_create_list_item(
            "numbers",
            sjit_expr_create_binary(
                SJIT_EXPR_ADD,
                literalNumber(0.0),
                literalNumber(1.0))),
        literalNumber(1.0));

    SCompiledProcedure &store_procedure = script->procedures[1];
    store_procedure.name = sjit_string_new("store %s");
    store_procedure.warp_mode = 1;
    store_procedure.argument_count = 1;
    store_procedure.argument_names = static_cast<SString **>(std::calloc(1, sizeof(SString *)));
    require(store_procedure.argument_names, "allocate nested store procedure argument names");
    store_procedure.argument_names[0] = sjit_string_new("m");
    store_procedure.statement_count = 1;
    store_procedure.statements = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(store_procedure.statements, "allocate nested store procedure body");
    store_procedure.statements[0].opcode = SJIT_STMT_SET_VARIABLE;
    store_procedure.statements[0].variable_name = sjit_string_new("received");
    store_procedure.statements[0].value = sjit_expr_create_argument("m");

    SCompiledProcedure &text_procedure = script->procedures[2];
    text_procedure.name = sjit_string_new("store text %s");
    text_procedure.warp_mode = 1;
    text_procedure.argument_count = 1;
    text_procedure.argument_names = static_cast<SString **>(std::calloc(1, sizeof(SString *)));
    require(text_procedure.argument_names, "allocate boxed text procedure argument names");
    text_procedure.argument_names[0] = sjit_string_new("text");
    text_procedure.statement_count = 1;
    text_procedure.statements = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(text_procedure.statements, "allocate boxed text procedure body");
    text_procedure.statements[0].opcode = SJIT_STMT_SET_VARIABLE;
    text_procedure.statements[0].variable_name = sjit_string_new("received_text");
    text_procedure.statements[0].value = sjit_expr_create_argument("text");

    SCompiledProcedure &top_text_procedure = script->procedures[3];
    top_text_procedure.name = sjit_string_new("store top %s");
    top_text_procedure.warp_mode = 1;
    top_text_procedure.argument_count = 1;
    top_text_procedure.argument_names = static_cast<SString **>(std::calloc(1, sizeof(SString *)));
    require(top_text_procedure.argument_names, "allocate top-level boxed procedure argument names");
    top_text_procedure.argument_names[0] = sjit_string_new("text");
    top_text_procedure.statement_count = 1;
    top_text_procedure.statements = static_cast<SStatement *>(std::calloc(1, sizeof(SStatement)));
    require(top_text_procedure.statements, "allocate top-level boxed procedure body");
    top_text_procedure.statements[0].opcode = SJIT_STMT_SET_VARIABLE;
    top_text_procedure.statements[0].variable_name = sjit_string_new("top_text");
    top_text_procedure.statements[0].value = sjit_expr_create_argument("text");

    sjit::JitEngine jit;
    const std::string ir_path = "/tmp/xyo-jit-procedure-expr.ll";
    jit.emitScriptLl(*script, "sjit_unit_procedure_expr_ir", ir_path);
    std::ifstream ir_file(ir_path);
    const std::string ir((std::istreambuf_iterator<char>(ir_file)), std::istreambuf_iterator<char>());
    require(
        ir.find("define i32 @sjit_unit_procedure_expr_ir_procedure_0") != std::string::npos,
        "JIT IR defines expression procedure variant");
    require(
        ir.find("call double @sjit_script_eval_procedure_statement_number") == std::string::npos,
        "procedure expression IR avoids numeric slot fallback");
    require(
        ir.find("sjit_script_eval_procedure_statement_number") == std::string::npos,
        "procedure expression IR does not declare numeric slot fallback");
    require(
        ir.find("call i32 @sjit_script_eval_procedure_statement_bool") == std::string::npos,
        "procedure expression IR avoids bool slot fallback");
    require(
        ir.find("sjit_script_eval_procedure_statement_bool") == std::string::npos,
        "procedure expression IR does not declare bool slot fallback");
    require(
        ir.find("call double @sjit_jit_round_number") != std::string::npos,
        "procedure expression IR lowers round directly");
    require(
        ir.find("call i32 @sjit_jit_list_variable_contains_value") != std::string::npos,
        "procedure expression IR lowers list contains directly");
    require(
        ir.find("call void @sjit_script_eval_procedure_statement_value") == std::string::npos,
        "procedure expression IR avoids boxed value slot fallback");
    require(
        ir.find("sjit_script_eval_procedure_statement_value") == std::string::npos,
        "procedure expression IR does not declare boxed value slot fallback");
    require(
        ir.find("call void @sjit_jit_expr_literal_value") != std::string::npos,
        "procedure expression IR lowers literal boxed values directly");
    require(
        ir.find("call void @sjit_jit_value_join_ptr") != std::string::npos,
        "procedure expression IR lowers boxed join directly");
    require(
        ir.find("call void @sjit_jit_value_make_number") == std::string::npos,
        "procedure expression IR stores numeric call arguments without boxing");
    require(
        ir.find("call void @sjit_jit_procedure_argument_value_at") == std::string::npos,
        "procedure expression IR avoids boxing direct argument assignments");
    require(
        ir.find("call void @sjit_jit_variable_set_from_argument") != std::string::npos,
        "procedure expression IR assigns procedure arguments directly");
    require(
        ir.find("call void @sjit_jit_variable_value") != std::string::npos,
        "procedure expression IR boxes variable values directly");
    require(
        ir.find("call void @sjit_jit_list_variable_item_value") != std::string::npos,
        "procedure expression IR boxes list item values directly");
    require(
        ir.find("call double @sjit_jit_value_length_number") != std::string::npos,
        "procedure expression IR lowers dynamic length directly");
    require(
        ir.find("call void @sjit_jit_value_letter_of") != std::string::npos,
        "procedure expression IR lowers letter-of directly");
    require(
        ir.find("call i32 @sjit_jit_value_truthy") != std::string::npos,
        "procedure expression IR lowers value truthiness directly");
    require(
        ir.find("call i32 @sjit_jit_key_pressed_value") != std::string::npos,
        "procedure expression IR lowers dynamic key pressed directly");
    require(
        ir.find("call void @sjit_jit_statement_set_variable_value_ptr") != std::string::npos,
        "procedure expression IR lowers boxed variable set");
    require(
        ir.find("call void @sjit_jit_statement_list_add_value_ptr") != std::string::npos,
        "procedure expression IR lowers boxed list add");
    require(
        ir.find("call void @sjit_jit_statement_list_delete_index_value_ptr") != std::string::npos,
        "procedure expression IR lowers boxed list delete");
    require(
        ir.find("call void @sjit_jit_statement_list_insert_value_ptr") != std::string::npos,
        "procedure expression IR lowers boxed list insert");
    require(
        ir.find("call void @sjit_jit_statement_list_replace_value_ptr") != std::string::npos,
        "procedure expression IR lowers boxed list replace");
    require(
        ir.find("call void @sjit_jit_statement_say_value_ptr") != std::string::npos,
        "procedure expression IR lowers boxed say");
    require(
        ir.find("call void @sjit_jit_statement_pen_set_color_value_ptr") != std::string::npos,
        "procedure expression IR lowers boxed pen color");
    require(
        ir.find("call void @sjit_jit_statement_pen_change_color_param_value_ptr") != std::string::npos,
        "procedure expression IR lowers boxed pen color param");
    require(
        ir.find("call i32 @sjit_script_execute_procedure_statement") == std::string::npos,
        "procedure expression IR avoids statement action fallback");
    require(
        ir.find("call i32 @sjit_jit_statement_procedure_call") == std::string::npos,
        "procedure expression IR avoids procedure call fallback");
    require(
        ir.find("call double @sjit_script_value_ptr_to_number") != std::string::npos,
        "procedure expression IR converts evaluated argument values once");
    require(
        ir.find("call void @sjit_script_eval_procedure_argument_value") == std::string::npos,
        "procedure expression IR avoids boxed argument fallback");
    require(
        ir.find("sjit_script_eval_procedure_argument_value") == std::string::npos,
        "procedure expression IR does not declare boxed argument fallback");
    require(
        ir.find("call void @sjit_script_eval_argument_value") == std::string::npos,
        "procedure expression IR lowers top-level call arguments directly");
    require(
        ir.find("sjit_script_eval_argument_value") == std::string::npos,
        "procedure expression IR does not declare top-level argument fallback");
    require(
        ir.find("procedure_for_each_condition") != std::string::npos,
        "procedure expression IR lowers for-each loop");
    require(
        ir.find("call i1 @sjit_unit_procedure_expr_ir_fast_variable_set_from_numeric_list_item") !=
            std::string::npos,
        "procedure expression IR speculates numeric list-item assignment");

    SScriptEntryFn entry = jit.compileScript(
        *script,
        "sjit_unit_procedure_expr_entry",
        runtime);
    require(entry, "compile runtime-specialized procedure expression jit script");
    require(script->procedures[0].jit_entry, "compile stores expression procedure jit entry");
    require(script->procedures[1].jit_entry, "compile stores nested expression procedure jit entry");
    require(script->procedures[2].jit_entry, "compile stores boxed expression procedure jit entry");
    require(script->procedures[3].jit_entry, "compile stores top-level boxed procedure jit entry");
    const int registered = sjit_runtime_register_script_with_data(
        runtime,
        sprite->base.id,
        43,
        SJIT_HAT_EVENT_WHENFLAGCLICKED,
        "",
        1,
        0,
        entry,
        script);
    require(registered, "register procedure expression jit script");

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);
    SVariable *rounded = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "rounded", SJIT_VAR_SCALAR);
    SVariable *text_len = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "text_len", SJIT_VAR_SCALAR);
    SVariable *found = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "found", SJIT_VAR_SCALAR);
    SVariable *label = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "label", SJIT_VAR_SCALAR);
    SVariable *sum = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "sum", SJIT_VAR_SCALAR);
    SVariable *received = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "received", SJIT_VAR_SCALAR);
    SVariable *received_text = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "received_text", SJIT_VAR_SCALAR);
    SVariable *top_text = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "top_text", SJIT_VAR_SCALAR);
    SVariable *var_copy = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "var_copy", SJIT_VAR_SCALAR);
    SVariable *last_item = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "last_item", SJIT_VAR_SCALAR);
    SVariable *last_len = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "last_len", SJIT_VAR_SCALAR);
    SVariable *letter = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "letter", SJIT_VAR_SCALAR);
    SVariable *truthy_text = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "truthy_text", SJIT_VAR_SCALAR);
    SVariable *key_found = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "key_found", SJIT_VAR_SCALAR);
    SVariable *dynamic_equal = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "dynamic_equal", SJIT_VAR_SCALAR);
    SVariable *first_item = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "first_item", SJIT_VAR_SCALAR);
    SVariable *cached_item = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "cached_item", SJIT_VAR_SCALAR);
    SVariable *computed_item = sjit_runtime_lookup_or_create_variable(runtime, sprite->base.id, "computed_item", SJIT_VAR_SCALAR);
    require(rounded && rounded->value.tag == SJIT_VALUE_NUMBER && rounded->value.number == 2.0, "JIT procedure direct lowering rounds argument");
    require(text_len && text_len->value.tag == SJIT_VALUE_NUMBER && text_len->value.number == 4.0, "JIT procedure direct lowering evaluates length");
    require(found && found->value.tag == SJIT_VALUE_NUMBER && found->value.number == 1.0, "JIT procedure direct lowering evaluates list contains");
    require(
        label && label->value.tag == SJIT_VALUE_STRING &&
            std::string(sjit_string_cstr(static_cast<const SString *>(label->value.ptr))) == "hi!",
        "JIT procedure boxed value helper preserves string assignment");
    require(sjit_list_length(items) == 2, "JIT procedure boxed list helpers mutate string items");
    SValue tail = sjit_list_get(items, 2);
    require(
        tail.tag == SJIT_VALUE_STRING &&
            std::string(sjit_string_cstr(static_cast<const SString *>(tail.ptr))) == "end!",
        "JIT procedure boxed list helpers preserve string item");
    sjit_value_destroy(tail);
    require(sprite->pen_r == 17 && sprite->pen_g == 52 && sprite->pen_b == 86, "JIT procedure boxed pen color helper applies string color");
    require(sprite->pen_a == 0, "JIT procedure boxed pen color param helper changes transparency");
    require(sum && sum->value.tag == SJIT_VALUE_NUMBER && sum->value.number == 6.0, "JIT procedure lowers for-each loop");
    require(
        var_copy && var_copy->value.tag == SJIT_VALUE_STRING &&
            std::string(sjit_string_cstr(static_cast<const SString *>(var_copy->value.ptr))) == "hi!",
        "JIT procedure preserves boxed variable value");
    require(
        last_item && last_item->value.tag == SJIT_VALUE_STRING &&
            std::string(sjit_string_cstr(static_cast<const SString *>(last_item->value.ptr))) == "end!",
        "JIT procedure preserves boxed list item value");
    require(last_len && last_len->value.tag == SJIT_VALUE_NUMBER && last_len->value.number == 4.0, "JIT procedure lowers dynamic length");
    require(
        letter && letter->value.tag == SJIT_VALUE_STRING &&
            std::string(sjit_string_cstr(static_cast<const SString *>(letter->value.ptr))) == "i",
        "JIT procedure lowers letter-of value");
    require(truthy_text && truthy_text->value.tag == SJIT_VALUE_NUMBER && truthy_text->value.number == 1.0, "JIT procedure lowers value truthiness");
    require(key_found && key_found->value.tag == SJIT_VALUE_NUMBER && key_found->value.number == 1.0, "JIT procedure lowers dynamic key pressed");
    require(
        dynamic_equal && dynamic_equal->value.tag == SJIT_VALUE_NUMBER && dynamic_equal->value.number == 1.0,
        "JIT procedure lowers dynamic comparison while calling other direct procedures");
    require(
        first_item && first_item->value.tag == SJIT_VALUE_STRING &&
            std::string(sjit_string_cstr(static_cast<const SString *>(first_item->value.ptr))) == "needle",
        "JIT procedure numeric list-item fast path preserves string fallback");
    require(
        cached_item && cached_item->value.tag == SJIT_VALUE_NUMBER && cached_item->value.number == 42.0,
        "JIT procedure numeric list-item fast path stores cached numbers");
    require(
        computed_item && computed_item->value.tag == SJIT_VALUE_NUMBER &&
            computed_item->value.number == 43.0,
        "runtime-specialized procedure list fast path accepts a nested numeric index");
    require(
        received && received->value.tag == SJIT_VALUE_NUMBER && received->value.number == 2.0,
        "JIT procedure direct lowering passes nested call argument");
    require(
        received_text && received_text->value.tag == SJIT_VALUE_STRING &&
            std::string(sjit_string_cstr(static_cast<const SString *>(received_text->value.ptr))) == "box!",
        "JIT procedure boxed argument preserves nested string call argument");
    require(
        top_text && top_text->value.tag == SJIT_VALUE_STRING &&
            std::string(sjit_string_cstr(static_cast<const SString *>(top_text->value.ptr))) == "top!",
        "JIT procedure boxed argument preserves top-level string call argument");

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_jit_procedure_argument_set_tags_match_interpreter() {
    SRuntime *runtime = sjit_runtime_create();
    SSprite *sprite = sjit_runtime_create_sprite(runtime, "ProcedureTagSprite", 0);
    require(sprite, "create procedure argument tag sprite");

    SVariable *numeric_destination = sjit_runtime_lookup_or_create_variable(
        runtime, sprite->base.id, "numeric destination", SJIT_VAR_SCALAR);
    SVariable *text_destination = sjit_runtime_lookup_or_create_variable(
        runtime, sprite->base.id, "text destination", SJIT_VAR_SCALAR);
    SVariable *nan_destination = sjit_runtime_lookup_or_create_variable(
        runtime, sprite->base.id, "nan destination", SJIT_VAR_SCALAR);
    SVariable *nonwarp_numeric_destination = sjit_runtime_lookup_or_create_variable(
        runtime, sprite->base.id, "nonwarp numeric destination", SJIT_VAR_SCALAR);
    SVariable *nonwarp_text_destination = sjit_runtime_lookup_or_create_variable(
        runtime, sprite->base.id, "nonwarp text destination", SJIT_VAR_SCALAR);
    require(
        numeric_destination && text_destination && nan_destination &&
            nonwarp_numeric_destination && nonwarp_text_destination &&
            numeric_destination->scalar_kind == SJIT_SCALAR_NUMBER &&
            text_destination->scalar_kind == SJIT_SCALAR_NUMBER &&
            nan_destination->scalar_kind == SJIT_SCALAR_NUMBER &&
            nonwarp_numeric_destination->scalar_kind == SJIT_SCALAR_NUMBER &&
            nonwarp_text_destination->scalar_kind == SJIT_SCALAR_NUMBER,
        "create number-specialized procedure destinations");

    auto createList = [&](const char *name) {
        SVariable *variable = sjit_runtime_lookup_or_create_variable(
            runtime, sprite->base.id, name, SJIT_VAR_LIST);
        require(variable && variable->value.tag == SJIT_VALUE_LIST, "create procedure tag list");
        return static_cast<SList *>(variable->value.ptr);
    };
    SList *numeric_items = createList("numeric items");
    SList *text_items = createList("text items");
    SList *nan_items = createList("nan items");
    SList *nonwarp_numeric_items = createList("nonwarp numeric items");
    SList *nonwarp_text_items = createList("nonwarp text items");
    SList *numeric_replaced = createList("numeric replaced");
    SList *text_replaced = createList("text replaced");
    SList *nan_replaced = createList("nan replaced");
    SList *nonwarp_numeric_replaced = createList("nonwarp numeric replaced");
    SList *nonwarp_text_replaced = createList("nonwarp text replaced");
    require(sjit_list_push_number(numeric_replaced, -1.0), "seed numeric replacement list");
    require(sjit_list_push_number(text_replaced, -1.0), "seed text replacement list");
    require(sjit_list_push_number(nan_replaced, -1.0), "seed NaN replacement list");
    require(
        sjit_list_push_number(nonwarp_numeric_replaced, -1.0),
        "seed non-warp numeric replacement list");
    require(
        sjit_list_push_number(nonwarp_text_replaced, -1.0),
        "seed non-warp text replacement list");
    SVariable *nan_source = sjit_runtime_lookup_or_create_variable(
        runtime, sprite->base.id, "nan source", SJIT_VAR_SCALAR);
    require(nan_source, "create NaN procedure argument source");
    sjit_variable_set_number(nan_source, std::nan(""));

    SCompiledScript *script = sjit_compiled_script_create(sprite->base.id, 5);
    require(script, "create procedure argument tag script");
    const char *procedureNames[5] = {
        "store numeric %s", "store text %s", "store nan %s",
        "store nonwarp numeric %s", "store nonwarp text %s"};
    const char *argumentValues[5] = {"50", "hello", nullptr, "75", "world"};
    for (int i = 0; i < 5; ++i) {
        SStatement &call = script->statements[i];
        call.opcode = SJIT_STMT_PROCEDURE_CALL;
        call.procedure_name = sjit_string_new(procedureNames[i]);
        call.argument_count = 1;
        call.arguments = static_cast<SArgumentExpr *>(std::calloc(1, sizeof(SArgumentExpr)));
        require(call.arguments, "allocate procedure argument tag call");
        call.arguments[0].name = sjit_string_new("value");
        call.arguments[0].value = i == 2 ?
            sjit_expr_create_variable("nan source") : literalString(argumentValues[i]);
    }

    script->procedure_count = 5;
    script->procedures = static_cast<SCompiledProcedure *>(
        std::calloc(5, sizeof(SCompiledProcedure)));
    require(script->procedures, "allocate procedure argument tag procedures");
    const char *destinationNames[5] = {
        "numeric destination", "text destination", "nan destination",
        "nonwarp numeric destination", "nonwarp text destination"};
    const char *itemListNames[5] = {
        "numeric items", "text items", "nan items",
        "nonwarp numeric items", "nonwarp text items"};
    const char *replaceListNames[5] = {
        "numeric replaced", "text replaced", "nan replaced",
        "nonwarp numeric replaced", "nonwarp text replaced"};
    for (int i = 0; i < 5; ++i) {
        SCompiledProcedure &procedure = script->procedures[i];
        procedure.name = sjit_string_new(procedureNames[i]);
        procedure.warp_mode = i < 3 ? 1 : 0;
        procedure.argument_count = 1;
        procedure.argument_names = static_cast<SString **>(std::calloc(1, sizeof(SString *)));
        require(procedure.argument_names, "allocate procedure argument tag name");
        procedure.argument_names[0] = sjit_string_new("value");
        procedure.statement_count = 3;
        procedure.statements = static_cast<SStatement *>(std::calloc(3, sizeof(SStatement)));
        require(procedure.statements, "allocate procedure argument tag body");

        procedure.statements[0].opcode = SJIT_STMT_SET_VARIABLE;
        procedure.statements[0].variable_name = sjit_string_new(destinationNames[i]);
        procedure.statements[0].value = sjit_expr_create_argument("value");

        procedure.statements[1].opcode = SJIT_STMT_LIST_ADD;
        procedure.statements[1].variable_name = sjit_string_new(itemListNames[i]);
        procedure.statements[1].value = sjit_expr_create_argument("value");

        procedure.statements[2].opcode = SJIT_STMT_LIST_REPLACE;
        procedure.statements[2].variable_name = sjit_string_new(replaceListNames[i]);
        procedure.statements[2].index = literalNumber(1.0);
        procedure.statements[2].value = sjit_expr_create_argument("value");
    }

    sjit::JitEngine jit;
    SScriptEntryFn entry = jit.compileScript(
        *script,
        "sjit_unit_procedure_argument_tags_entry",
        runtime);
    require(entry, "compile procedure argument tag script");
    require(
        script->procedures[0].jit_entry && script->procedures[1].jit_entry &&
            script->procedures[2].jit_entry && script->procedures[3].jit_entry &&
            script->procedures[4].jit_entry,
        "compile procedure argument tag entries");
    require(
        sjit_runtime_register_script_with_data(
            runtime,
            sprite->base.id,
            49,
            SJIT_HAT_EVENT_WHENFLAGCLICKED,
            "",
            1,
            0,
            entry,
            script),
        "register procedure argument tag script");

    sjit_runtime_green_flag(runtime);
    sjit_runtime_tick(runtime);

    /* Creating the lists above may grow and relocate the target's variable
       array, so reacquire scalar handles before inspecting the result. */
    numeric_destination = sjit_runtime_lookup_or_create_variable(
        runtime, sprite->base.id, "numeric destination", SJIT_VAR_SCALAR);
    text_destination = sjit_runtime_lookup_or_create_variable(
        runtime, sprite->base.id, "text destination", SJIT_VAR_SCALAR);
    nan_destination = sjit_runtime_lookup_or_create_variable(
        runtime, sprite->base.id, "nan destination", SJIT_VAR_SCALAR);
    nonwarp_numeric_destination = sjit_runtime_lookup_or_create_variable(
        runtime, sprite->base.id, "nonwarp numeric destination", SJIT_VAR_SCALAR);
    nonwarp_text_destination = sjit_runtime_lookup_or_create_variable(
        runtime, sprite->base.id, "nonwarp text destination", SJIT_VAR_SCALAR);
    require(
        numeric_destination->value.tag == SJIT_VALUE_NUMBER &&
            numeric_destination->value.number == 50.0,
        "numeric string argument assignment matches interpreter number promotion");
    require(
        text_destination->value.tag == SJIT_VALUE_STRING &&
            std::string(sjit_string_cstr(
                static_cast<const SString *>(text_destination->value.ptr))) == "hello",
        "nonnumeric string argument assignment preserves its tag");
    require(
        nan_destination->value.tag == SJIT_VALUE_NUMBER &&
            nan_destination->value.number == 0.0,
        "NaN argument assignment matches interpreter normalization");
    require(
        nonwarp_numeric_destination->value.tag == SJIT_VALUE_NUMBER &&
            nonwarp_numeric_destination->value.number == 75.0,
        "non-warp numeric string argument assignment matches interpreter");
    require(
        nonwarp_text_destination->value.tag == SJIT_VALUE_STRING &&
            std::string(sjit_string_cstr(static_cast<const SString *>(
                nonwarp_text_destination->value.ptr))) == "world",
        "non-warp nonnumeric string argument assignment preserves its tag");

    auto requireListValue = [&](SList *list, int expectedTag, double expectedNumber,
                                const char *expectedText, const char *message) {
        SValue value = sjit_list_get(list, 1);
        const bool matches = value.tag == expectedTag &&
            (expectedTag == SJIT_VALUE_NUMBER ? value.number == expectedNumber :
                std::string(sjit_string_cstr(static_cast<const SString *>(value.ptr))) ==
                    expectedText);
        sjit_value_destroy(value);
        require(matches, message);
    };
    requireListValue(
        numeric_items, SJIT_VALUE_NUMBER, 50.0, "",
        "numeric string argument list add matches interpreter number promotion");
    requireListValue(
        text_items, SJIT_VALUE_STRING, 0.0, "hello",
        "nonnumeric string argument list add preserves its tag");
    requireListValue(
        nan_items, SJIT_VALUE_NUMBER, 0.0, "",
        "NaN argument list add matches interpreter normalization");
    requireListValue(
        nonwarp_numeric_items, SJIT_VALUE_NUMBER, 75.0, "",
        "non-warp numeric string argument list add matches interpreter");
    requireListValue(
        nonwarp_text_items, SJIT_VALUE_STRING, 0.0, "world",
        "non-warp nonnumeric string argument list add preserves its tag");
    requireListValue(
        numeric_replaced, SJIT_VALUE_NUMBER, 50.0, "",
        "numeric string argument list replace matches interpreter number promotion");
    requireListValue(
        text_replaced, SJIT_VALUE_STRING, 0.0, "hello",
        "nonnumeric string argument list replace preserves its tag");
    requireListValue(
        nan_replaced, SJIT_VALUE_NUMBER, 0.0, "",
        "NaN argument list replace matches interpreter normalization");
    requireListValue(
        nonwarp_numeric_replaced, SJIT_VALUE_NUMBER, 75.0, "",
        "non-warp numeric string argument list replace matches interpreter");
    requireListValue(
        nonwarp_text_replaced, SJIT_VALUE_STRING, 0.0, "world",
        "non-warp nonnumeric string argument list replace preserves its tag");

    sjit_runtime_destroy(runtime);
    sjit_compiled_script_destroy(script);
}

void test_project_emit_ll() {
    const std::string project_json = R"json({
        "targets": [{
            "isStage": true,
            "name": "Stage",
            "variables": {
                "scoreVar": ["score", 0]
            },
            "lists": {},
            "blocks": {
                "hat": {
                    "opcode": "event_whenflagclicked",
                    "next": "setScore",
                    "topLevel": true,
                    "inputs": {},
                    "fields": {}
                },
                "setScore": {
                    "opcode": "data_setvariableto",
                    "next": null,
                    "topLevel": false,
                    "inputs": {"VALUE": [1, [4, "42"]]},
                    "fields": {"VARIABLE": ["score", "scoreVar"]}
                }
            }
        }]
    })json";

    const std::string ir_path = "/tmp/xyo-project-emit.ll";
    const std::vector<std::string> paths = sjit::emitProjectLl(writeSb3Fixture(project_json), ir_path);
    require(paths.size() == 1 && paths[0] == ir_path, "project emit should write one IR file");
    std::ifstream ir_file(ir_path);
    const std::string ir((std::istreambuf_iterator<char>(ir_file)), std::istreambuf_iterator<char>());
    require(ir.find("define i32 @sjit_script_entry_1000") != std::string::npos, "project emit writes script entry IR");
    require(ir.find("sjit_jit_statement_set_variable") != std::string::npos, "project emit lowers script body");
}

} // namespace

int main() {
    if (std::getenv("SJIT_TEST_THREAD_POOL_ONLY") != nullptr) {
        test_ownership_analysis_is_fail_closed();
        test_ownership_proven_scripts_use_thread_pool();
        test_parallel_scheduler_revalidates_owned_values();
        test_parallel_manifest_ignores_unrelated_owning_locals();
        test_parallel_scheduler_rejects_stale_procedure_storage();
        test_duplicate_script_ids_disable_parallel_scheduling();
        test_jit_runtime_pool_workers_outlive_module();
        test_scheduler_thread_pool_overlaps_and_serializes_owner_conflicts();
        test_parallel_scheduler_matches_sequential_status_order();
        std::cout << "xyo thread-pool tests passed\n";
        return 0;
    }
    test_ownership_analysis_is_fail_closed();
    test_ownership_proven_scripts_use_thread_pool();
    test_parallel_scheduler_revalidates_owned_values();
    test_parallel_manifest_ignores_unrelated_owning_locals();
    test_parallel_scheduler_rejects_stale_procedure_storage();
    test_duplicate_script_ids_disable_parallel_scheduling();
    test_project_loader_parallelizes_proven_native_entries();
    test_jit_runtime_pool_workers_outlive_module();
    test_scheduler_thread_pool_overlaps_and_serializes_owner_conflicts();
    test_parallel_scheduler_matches_sequential_status_order();
    test_opcode_effects_and_reporter_differential_fallback();
    test_backdrop_switch_loader_interpreter_and_semantics();
    test_graphic_effect_helpers_clamp_clear_and_clone();
    test_costume_switch_and_layer_operations();
    test_graphic_effect_interpreter_statements();
    test_graphic_effect_top_level_native_jit();
    test_graphic_effect_custom_procedure_argument_native_jit();
    test_project_loader_graphic_effects_and_drag_mode();
    test_core_interpreter_jit_differential_and_hotness();
    test_procedure_entry_identity_guards();
    test_checked_variable_handles_survive_runtime_replacement();
    test_interpreter_cache_rejects_recycled_runtime_identity();
    test_values_cast_compare();
    test_operator_and_list();
    test_large_numeric_list_limit();
    test_compatibility_modes_and_turbowarp_keys();
    test_numeric_list_cache_tracks_mutations();
    test_jit_borrowed_list_helpers();
    test_script_boolean_and_list_item_expr();
    test_project_loader_boolean_and_list_blocks();
    test_project_loader_list_reporters_and_mutations();
    test_project_loader_variable_monitors_use_scratch_ids();
    test_project_loader_variable_and_list_ids_resolve_same_names();
    test_project_loader_procedure_forever_for_each_and_pen();
    test_project_loader_control_loops_yield_like_scratch();
    test_project_loader_procedure_arguments_and_stop_return();
    test_project_loader_passes_list_reporter_to_procedure();
    test_project_loader_sprite_click_hat();
    test_deep_procedure_recursion_uses_dynamic_argument_frames();
    test_native_pen_row_matches_safe_pixel_loop();
    test_native_pen_raster_tile_matches_safe_path_and_ordering();
    test_native_pen_raster_tile_marker_and_strict_fallbacks();
    test_native_pen_row_guards_are_non_mutating();
    test_native_pen_row_shared_storage_replays_safely();
    test_native_pen_row_string_scalar_commit_releases_ownership();
    test_native_pen_row_list_marker_matches_scratch_equality();
    test_native_pen_row_embedded_null_marker_matches_c_string_equality();
    test_native_pen_row_ast_matcher_only_emits_for_invariant_shape();
    test_runtime_draw_and_broadcast();
    test_runtime_draw_owned_storage();
    test_pen_stroke_buffer();
    test_interpreter_adjacent_pen_stamp();
    test_interpreter_adjacent_pen_color_change();
    test_interpreter_adjacent_hex_pen_color_change();
    test_combined_pen_brightness_matches_separate_operations();
    test_wait_resume();
    test_jit_smoke();
    test_jit_script_entry_runs_compiled_program();
    test_jit_bulk_repeat_numeric_change();
    test_jit_compiles_recursive_warp_numeric_procedure();
    test_jit_compiles_non_warp_procedure_repeat();
    test_jit_compiles_wide_procedure_arguments();
    test_jit_batched_loop_yields_in_warp_mode();
    test_jit_fallback_warp_procedure_finishes_redraw_loop_in_one_tick();
    test_jit_procedure_call_noops_for_missing_target();
    test_jit_top_level_procedure_call_falls_back_for_non_direct_procedure();
    test_jit_fallback_procedure_call_resumes_after_yield();
    test_jit_procedure_lowers_numeric_effect_statements();
    test_jit_procedure_lowers_numeric_list_mutations();
    test_jit_procedure_replaces_list_from_variables_without_boxing();
    test_jit_procedure_expression_slot_fallbacks();
    test_jit_procedure_argument_set_tags_match_interpreter();
    test_project_emit_ll();
    std::cout << "xyo runtime tests passed\n";
    return 0;
}
