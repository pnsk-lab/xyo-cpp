// Component-only dependency scheduling must match the full graph exactly.
#include <cstdio>
#include <cstdint>
#include <vector>

#include "../../src/engine/deps.hpp"
#include "../../src/engine/runtime.hpp"
#include "../../src/model/target.hpp"
#include "../../src/model/variable.hpp"

namespace {
using namespace xyo;
using Groups = std::vector<std::vector<size_t>>;
int checks = 0, failures = 0;

void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL deps: %s\n", name); }
}

std::vector<const AccessSet*> pointers(const std::vector<AccessSet>& sets) {
    std::vector<const AccessSet*> out;
    for (const auto& set : sets) out.push_back(&set);
    return out;
}

void compare(const std::vector<AccessSet>& sets, const char* name) {
    const auto ptrs = pointers(sets);
    check(buildDepGroups(ptrs) == buildDepGraph(ptrs).groups(), name);
}
// Build only the model: recursive scripts are analyzed, never executed.
struct AnalysisFixture {
    Runtime runtime;
    Sprite sprite{&runtime};
    Target target{&runtime, &sprite};

    AnalysisFixture() {
        for (const char* id : {"a", "b", "c"}) {
            Variable variable;
            variable.id = id;
            target.variables.emplace(id, std::move(variable));
        }
        Block top = make("hat", "event_whenflagclicked", "entry");
        top.topLevel = true;
        add(std::move(top));
    }
    static Block make(const char* id, const char* opcode, const char* next = "") {
        Block block;
        block.id = id;
        block.opcode = opcode;
        block.next = next;
        return block;
    }
    void add(Block block) { sprite.blocks->createBlock(std::move(block)); }
    static Mutation mutation(const char* code) {
        Mutation result;
        result.attrs["proccode"] = code;
        result.attrs["warp"] = "true";
        result.attrs["argumentids"] = "[]";
        result.attrs["argumentnames"] = "[]";
        result.attrs["argumentdefaults"] = "[]";
        return result;
    }
    void call(const char* id, const char* code, const char* next = "") {
        Block block = make(id, "procedures_call", next);
        block.mutation = mutation(code);
        add(std::move(block));
    }
    void definition(const char* id, const char* prototype, const char* code,
                    const char* first) {
        Block def = make(id, "procedures_definition", first);
        def.topLevel = true;
        def.inputs.set("custom_block", {prototype, prototype});
        add(std::move(def));
        Block proto = make(prototype, "procedures_prototype");
        proto.shadow = true;
        proto.mutation = mutation(code);
        add(std::move(proto));
    }
    void variable(const char* id, const char* var, bool write,
                  const char* next = "", const char* input = "") {
        Block block = make(id, write ? "data_setvariableto" : "data_variable", next);
        block.fields["VARIABLE"].id = var;
        if (*input) block.inputs.set("VALUE", {input, ""});
        add(std::move(block));
    }
    AccessSet analyze() {
        return analyzeScript(runtime, target, *sprite.blocks,
                             sprite.blocks->getBlock("hat"));
    }
    MemKey key(const char* id) {
        return MemKey::var(target.lookupVariableById(id));
    }
};

void testAnalysis() {
    {
        AnalysisFixture fixture;
        fixture.call("entry", "A");
        fixture.definition("defA", "protoA", "A", "writeA");
        fixture.variable("writeA", "a", true, "recurse", "readB");
        fixture.variable("readB", "b", false);
        fixture.call("recurse", "A", "tail");
        fixture.variable("tail", "b", true, "", "readA");
        fixture.variable("readA", "a", false);
        const AccessSet set = fixture.analyze();
        check(!set.unknown && !set.spawn && set.workerSafe(),
              "self-recursive warp procedure remains analyzable and worker-safe");
        check(set.reads.count(fixture.key("a")) && set.reads.count(fixture.key("b")) &&
              set.writes.count(fixture.key("a")) && set.writes.count(fixture.key("b")),
              "self-recursive summary includes reads/writes after recursive call");
        fixture.sprite.blocks->getBlock("tail")->next = "move";
        fixture.add(AnalysisFixture::make("move", "motion_setx"));
        const AccessSet withMotion = fixture.analyze();
        check(!withMotion.unknown && withMotion.safe() && withMotion.mainThread &&
              !withMotion.workerSafe() &&
              withMotion.writes.count(MemKey::field(&fixture.target, TF::X)),
              "motion after recursive call is visited and caller-thread-only");
    }
    {
        AnalysisFixture fixture;
        fixture.call("entry", "A");
        fixture.definition("defA", "protoA", "A", "writeA");
        fixture.definition("defB", "protoB", "B", "writeB");
        fixture.variable("writeA", "a", true, "callB", "readB");
        fixture.variable("readB", "b", false);
        fixture.call("callB", "B", "tailA");
        fixture.variable("tailA", "c", true);
        fixture.variable("writeB", "b", true, "callA", "readA");
        fixture.variable("readA", "a", false);
        fixture.call("callA", "A", "tailB");
        fixture.variable("tailB", "c", false);
        const AccessSet set = fixture.analyze();
        check(!set.unknown && !set.spawn && set.workerSafe(),
              "mutual-recursive warp procedures remain analyzable");
        bool all = true;
        for (const char* var : {"a", "b", "c"})
            all &= set.reads.count(fixture.key(var)) && set.writes.count(fixture.key(var));
        check(all, "mutual recursion collects both bodies and both recursive-call tails");
        fixture.sprite.blocks->getBlock("tailB")->next = "move";
        fixture.add(AnalysisFixture::make("move", "motion_setx"));
        const AccessSet withMotion = fixture.analyze();
        check(!withMotion.unknown && withMotion.mainThread && !withMotion.workerSafe(),
              "mutual-recursive callee tail propagates caller-thread requirement");
    }
    {
        AnalysisFixture fixture;
        Block list = AnalysisFixture::make("entry", "data_lengthoflist");
        list.fields["LIST"].id = "a";
        fixture.add(std::move(list));
        const auto cold = fixture.analyze();
        check(cold.safe() && cold.mainThread && !cold.workerSafe(),
              "null list read lazily initializes storage on caller thread");
        fixture.target.lookupVariableById("a")->list = SList::create();
        check(fixture.analyze().workerSafe(),
              "initialized list readers can run in parallel");
    }
    for (const char* opcode : {"sensing_keypressed", "sensing_loudness", "motion_setx"}) {
        AnalysisFixture fixture;
        fixture.add(AnalysisFixture::make("entry", opcode));
        const AccessSet set = fixture.analyze();
        check(set.safe() && set.mainThread && !set.workerSafe(),
              "mutable IO caches and target writes require caller thread");
    }
    {
        AnalysisFixture fixture;
        fixture.add(AnalysisFixture::make("entry", "sensing_mousex"));
        const AccessSet set = fixture.analyze();
        check(set.workerSafe() && !set.mainThread &&
              set.reads.count(MemKey::global(GK::Io)),
              "snapshot mouse input stays worker-safe");
    }
    {
        AnalysisFixture fixture;
        fixture.add(AnalysisFixture::make("entry", "extension_unknown_operation"));
        const AccessSet set = fixture.analyze();
        check(set.unknown && !set.safe() && !set.workerSafe(),
              "unknown operations still bar worker scheduling");
    }
    {
        AnalysisFixture fixture;
        fixture.add(AnalysisFixture::make("entry", "event_broadcast"));
        const AccessSet set = fixture.analyze();
        check(set.spawn && !set.safe() && !set.workerSafe(),
              "scheduler-mutating operations still bar worker scheduling");
    }
}
} // namespace

int test_deps_main() {
    using namespace xyo;
    testAnalysis();
    Variable vars[32];
    const MemKey a = MemKey::var(&vars[0]);
    const MemKey b = MemKey::var(&vars[1]);
    compare({}, "empty");
    compare(std::vector<AccessSet>(1), "singleton");
    compare(std::vector<AccessSet>(8), "empty access sets");

    std::vector<AccessSet> sets(5);
    for (auto& set : sets) set.reads.insert(a);
    compare(sets, "shared read-only");
    check(buildDepGroups(pointers(sets)) == Groups{{0}, {1}, {2}, {3}, {4}},
          "readers stay independent");
    sets[3].writes.insert(a);
    compare(sets, "writer after and before readers");
    check(buildDepGroups(pointers(sets)) == Groups{{0, 1, 2, 3, 4}},
          "writer connects all readers");
    sets[1].writes.insert(a);
    compare(sets, "multiple writers and read-write overlap");

    sets.assign(6, {});
    sets[0].writes.insert(a);
    sets[4].reads.insert(a);
    sets[4].writes.insert(b);
    sets[2].reads.insert(b);
    sets[1].writes.insert(MemKey::var(&vars[2]));
    sets[5].reads.insert(MemKey::var(&vars[2]));
    compare(sets, "nonadjacent transitive bridge");
    check(buildDepGroups(pointers(sets)) == Groups{{0, 2, 4}, {1, 5}, {3}},
          "stable component and member order");
    std::vector<const AccessSet*> duplicates{&sets[0], &sets[0], &sets[3]};
    check(buildDepGroups(duplicates) == buildDepGraph(duplicates).groups(),
          "duplicate access-set pointers");

    Sprite sprite(nullptr);
    Target t1(nullptr, &sprite), t2(nullptr, &sprite);
    std::vector<MemKey> keys;
    for (const auto& var : vars) keys.push_back(MemKey::var(&var));
    keys.push_back(MemKey::field(&t1, TF::X));
    keys.push_back(MemKey::field(&t1, TF::Y));
    keys.push_back(MemKey::field(&t2, TF::X));
    keys.push_back(MemKey::global(GK::Timer));
    keys.push_back(MemKey::global(GK::Counter));
    // Same address/sub but different kind must not alias.
    MemKey alternate = a;
    alternate.kind = MemKey::Global;
    keys.push_back(alternate);
    sets.assign(keys.size(), {});
    for (size_t i = 0; i < keys.size(); ++i) sets[i].writes.insert(keys[i]);
    compare(sets, "key kinds, subfields, and pointer identities");
    check(buildDepGroups(pointers(sets)).size() == keys.size(),
          "distinct keys stay independent");

    uint32_t state = 0x12569abdu;
    auto random = [&]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    for (size_t trial = 0; trial < 400; ++trial) {
        sets.assign(random() % 65, {});
        for (auto& set : sets) {
            for (size_t access = 0, count = random() % 9; access < count;
                 ++access) {
                const MemKey key = keys[random() % keys.size()];
                if (random() & 1) set.reads.insert(key);
                else set.writes.insert(key);
            }
        }
        compare(sets, "deterministic randomized oracle");
    }

    // Larger cases exercise the path that must not construct quadratic edges.
    sets.assign(4096, {});
    for (auto& set : sets) set.reads.insert(a);
    check(buildDepGroups(pointers(sets)).size() == sets.size(),
          "large shared read-only batch");
    for (auto& set : sets) set.writes.insert(a);
    const Groups dense = buildDepGroups(pointers(sets));
    bool ordered = dense.size() == 1 && dense[0].size() == sets.size();
    if (ordered)
        for (size_t i = 0; i < sets.size(); ++i)
            if (dense[0][i] != i) ordered = false;
    check(ordered, "large shared-writer batch preserves order");

    std::printf("deps: %d checks, %d failures\n", checks, failures);
    return failures;
}
