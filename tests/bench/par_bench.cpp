// Deterministic dependency-planning and renderer-attached scheduler benchmark.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include "core/cast.hpp"
#include "engine/deps.hpp"
#include "engine/runtime.hpp"
#include "engine/sequencer.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
#include "model/variable.hpp"
using namespace xyo;
using BenchClock = std::chrono::steady_clock;

static int graphBench(int n, int rounds, const std::string& kind, bool old) {
    std::vector<Variable> vars(size_t(n) + 1);
    std::vector<AccessSet> owned(n);
    std::vector<const AccessSet*> sets;
    for (int i = 0; i < n; ++i) {
        vars[i].name = u"v";
        if (kind == "dense") owned[i].writes.insert(MemKey::var(&vars[0]));
        else if (kind == "readers") owned[i].reads.insert(MemKey::var(&vars[0]));
        else owned[i].writes.insert(MemKey::var(&vars[i]));
        sets.push_back(&owned[i]);
    }
    size_t checksum = 0;
    std::vector<double> ms;
    for (int r = -2; r < rounds; ++r) {
        auto t = BenchClock::now();
        std::vector<std::vector<size_t>> groups;
#if defined(XYO_PAR_BENCH_BASE)
        groups = buildDepGraph(sets).groups();
#else
        groups = old ? buildDepGraph(sets).groups() : buildDepGroups(sets);
#endif
        double elapsed = std::chrono::duration<double, std::milli>(BenchClock::now()-t).count();
        checksum += groups.size();
        if (r >= 0) ms.push_back(elapsed);
    }
    std::sort(ms.begin(), ms.end());
    std::printf("GRAPH n=%d kind=%s old=%d median=%.6fms checksum=%zu\n",
                n, kind.c_str(), int(old), ms[ms.size()/2], checksum);
    return 0;
}

static std::string project(int workers, int work) {
    std::ostringstream s;
    s << R"({"targets":[{"isStage":true,"name":"Stage","variables":{},"lists":{},"broadcasts":{},"blocks":{},"costumes":[],"sounds":[]})";
    for (int i=0;i<workers;++i) {
        s << ",{\"isStage\":false,\"name\":\"W" << i
          << "\",\"layerOrder\":" << i+1
          << R"(,"variables":{"v":["v",0]},"lists":{},"broadcasts":{},"costumes":[],"sounds":[],"blocks":{)"
          << R"("hat":{"opcode":"event_whenflagclicked","topLevel":true,"next":"init","inputs":{},"fields":{}},)"
          << R"("init":{"opcode":"data_setvariableto","next":"call","inputs":{"VALUE":[1,[4,"0"]]},"fields":{"VARIABLE":["v","v"]}},)"
          << R"("call":{"opcode":"procedures_call","inputs":{},"fields":{},"mutation":{"tagName":"mutation","proccode":"work","argumentids":"[]","warp":"true"}},)"
          << R"("def":{"opcode":"procedures_definition","topLevel":true,"next":"repeat","inputs":{"custom_block":[1,"proto"]},"fields":{}},)"
          << R"("proto":{"opcode":"procedures_prototype","shadow":true,"inputs":{},"fields":{},"mutation":{"tagName":"mutation","proccode":"work","argumentids":"[]","argumentnames":"[]","argumentdefaults":"[]","warp":"true"}},)"
          << R"("repeat":{"opcode":"control_repeat","inputs":{"TIMES":[1,[4,")" << work
          << R"("]],"SUBSTACK":[2,"update"]},"fields":{}},)"
          << R"("update":{"opcode":"data_setvariableto","inputs":{"VALUE":[2,"add"]},"fields":{"VARIABLE":["v","v"]}},)"
          << R"("add":{"opcode":"operator_add","inputs":{"NUM1":[2,"mul"],"NUM2":[1,[4,"1"]]},"fields":{}},)"
          << R"("mul":{"opcode":"operator_multiply","inputs":{"NUM1":[2,"read"],"NUM2":[1,[4,"1.000000001"]]},"fields":{}},)"
          << R"("read":{"opcode":"data_variable","inputs":{},"fields":{"VARIABLE":["v","v"]}}}})";
    }
    s << R"(],"monitors":[],"extensions":[],"meta":{"semver":"3.0.0"}})";
    return s.str();
}

static int computeBench(int workers, int work, int rounds, bool renderer) {
    setJitMode(JitMode::Eager);
    RendererAPI ren; // outlive Runtime, including early failure/disposal paths
    Runtime rt;
    auto text = project(workers,work);
    auto loaded = loadSb3(rt, text, nullptr);
    if (!loaded.ok) { std::fprintf(stderr,"load: %s\n",loaded.error.c_str()); return 2; }
    if (renderer) rt.renderer = &ren;
    double expected = 0;
    for (int i=0;i<work;++i) expected=expected*1.000000001+1;
    std::vector<double> ms;
    for (int r=-3;r<rounds;++r) {
        auto t=BenchClock::now();
        rt.greenFlag();
        rt.runUntilDone(100000);
        double elapsed=std::chrono::duration<double,std::milli>(BenchClock::now()-t).count();
        if (!rt.threads.empty()) return 3;
        for (Target* target:rt.targets) if (!target->isStage) {
            Variable* v=target->lookupVariableById("v");
            if (!v || cast::toNumber(v->value)!=expected) {
                std::fprintf(stderr,"value mismatch for %s: %.17g vs %.17g\n",
                    target->id.c_str(),v?cast::toNumber(v->value):-1,expected);
                return 4;
            }
        }
        if(r>=0) ms.push_back(elapsed);
    }
    std::sort(ms.begin(),ms.end());
    std::printf("COMPUTE workers=%d work=%d rounds=%d renderer=%d median=%.3fms min=%.3fms\n",
                workers,work,rounds,int(renderer),ms[ms.size()/2],ms.front());
#if !defined(XYO_PAR_BENCH_BASE)
    auto stats=rt.sequencer->parallelStats();
    std::printf("PAR batches=%llu dispatches=%llu threads=%llu groups=%llu\n",
        (unsigned long long)stats.batches,(unsigned long long)stats.dispatches,
        (unsigned long long)stats.threads,(unsigned long long)stats.groups);
#endif
    rt.renderer=nullptr;
    return 0;
}

int main(int argc,char**argv) {
    int n=0,rounds=9,workers=8,work=2000000;
    bool old=false,renderer=true;
    std::string kind="disjoint";
    for(int i=1;i<argc;++i) {
        auto val=[&](){return i+1<argc?argv[++i]:"";};
        if(!std::strcmp(argv[i],"--graph")) n=std::atoi(val());
        else if(!std::strcmp(argv[i],"--rounds")) rounds=std::atoi(val());
        else if(!std::strcmp(argv[i],"--kind")) kind=val();
        else if(!std::strcmp(argv[i],"--workers")) workers=std::atoi(val());
        else if(!std::strcmp(argv[i],"--work")) work=std::atoi(val());
        else if(!std::strcmp(argv[i],"--headless")) renderer=false;
        else if(!std::strcmp(argv[i],"--old")) old=true;
        else return 2;
    }
    if(rounds<1||workers<1||work<1||n<0) return 2;
    if(n) return graphBench(n,rounds,kind,old);
    return computeBench(workers,work,rounds,renderer);
}
