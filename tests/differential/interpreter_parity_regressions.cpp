// Regression tests for interpreter-only event semantics.
#include <cstdio>
#include <string>

#include "core/cast.hpp"
#include "engine/runtime.hpp"
#include "load/sb3.hpp"
#include "load/sb3_save.hpp"
#include "model/target.hpp"
#include "model/variable.hpp"

using namespace xyo;

namespace {
int checks = 0;
int failures = 0;

void check(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL interpreter regression: %s\n", message);
    }
}

void testBroadcastIdCollision() {
    // A deliberately valid SB3 whose scalar and broadcast share the ID 'dupe'.
    // The scalar must retain its value, while the broadcast remains searchable
    // by both ID and case-insensitive dynamically generated name.
    constexpr const char* project = R"JSON({
      "targets": [{
        "isStage":true,"name":"Stage",
        "variables":{"dupe":["dupe",7]},
        "lists":{},"broadcasts":{"dupe":"dupe"},
        "blocks":{},"costumes":[],"sounds":[]
      }],
      "monitors":[],"extensions":[],"meta":{}
    })JSON";
    Runtime rt;
    const auto loaded = loadSb3(rt, project, nullptr);
    check(loaded.ok, "load SB3 with a broadcast/scalar ID collision");
    if (!loaded.ok) return;
    Target* stage = rt.getTargetForStage();
    check(stage != nullptr, "loaded stage exists");
    if (!stage) return;

    Variable* scalar = stage->lookupVariableById("dupe");
    Variable* broadcastId = stage->lookupBroadcastMsg("dupe", u"dupe");
    Variable* broadcastName = stage->lookupBroadcastByInputValue(u"DUPE");
    check(scalar && scalar->type == Variable::kScalarType &&
          cast::toNumber(scalar->value) == 7, "colliding scalar retains value");
    check(broadcastId && broadcastId->type == Variable::kBroadcastType,
          "colliding broadcast lookup by ID");
    check(broadcastName == broadcastId,
          "colliding broadcast lookup by case-insensitive name");

    const auto saved = saveSb3(rt);
    check(saved.ok && !saved.data.empty(), "serialize both colliding definitions");
    if (!saved.ok || saved.data.empty()) return;
    Runtime rt2;
    const auto reloaded = loadSb3File(rt2, saved.data.data(), saved.data.size());
    check(reloaded.ok, "reload serialized colliding definitions");
    if (reloaded.ok) {
        Target* roundtrip = rt2.getTargetForStage();
        check(roundtrip &&
              roundtrip->lookupBroadcastByInputValue(u"DUPE") &&
              roundtrip->lookupVariableById("dupe") &&
              roundtrip->lookupVariableById("dupe")->type == Variable::kScalarType,
              "scalar and broadcast survive SB3 round trip");
    }
}

void testReentrantBackdropWait() {
    // The flag script broadcasts to a pending receiver, then starts backdrop
    // receivers and waits. Its final snapshot must include the broadcast.
    // Before the utility-context fix, nested execute() redirected yield() to
    // the backdrop thread, letting this script read counter=0 prematurely.
    constexpr const char* project = R"JSON({
      "targets":[
        {
          "isStage":true,"name":"Stage",
          "variables":{"counter":["counter",0],
                       "snapshot":["snapshot",-1],
                       "backdrops":["backdrops",0]},
          "lists":{},"broadcasts":{"ping":"ping"},
          "blocks":{},
          "costumes":[{"name":"blank","assetId":"dummy","dataFormat":"svg",
                       "rotationCenterX":0,"rotationCenterY":0}],
          "sounds":[],"currentCostume":0
        },
        {
          "isStage":false,"name":"Worker","variables":{},"lists":{},
          "broadcasts":{},
          "blocks":{
            "flag":{"opcode":"event_whenflagclicked","topLevel":true,
                    "parent":null,"next":"send","inputs":{},"fields":{}},
            "send":{"opcode":"event_broadcast","parent":"flag","next":"wait",
                    "inputs":{"BROADCAST_INPUT":[1,"menu"]},"fields":{}},
            "menu":{"opcode":"event_broadcast_menu","parent":"send","next":null,
                    "shadow":true,"inputs":{},
                    "fields":{"BROADCAST_OPTION":["ping","ping"]}},
            "wait":{"opcode":"looks_switchbackdroptoandwait","parent":"send",
                    "next":"snapshot",
                    "inputs":{"BACKDROP":[1,"bgmenu"]},"fields":{}},
            "bgmenu":{"opcode":"looks_backdrops","parent":"wait","next":null,
                      "shadow":true,"inputs":{},"fields":{"BACKDROP":["blank",null]}},
            "snapshot":{"opcode":"data_setvariableto","parent":"wait",
                        "next":null,"inputs":{"VALUE":[2,"read"]},
                        "fields":{"VARIABLE":["snapshot","snapshot"]}},
            "read":{"opcode":"data_variable","parent":"snapshot","next":null,
                    "inputs":{},"fields":{"VARIABLE":["counter","counter"]}},
            "receiver":{"opcode":"event_whenbroadcastreceived",
                        "topLevel":true,"parent":null,"next":"inc",
                        "inputs":{},"fields":{"BROADCAST_OPTION":["ping","ping"]}},
            "inc":{"opcode":"data_changevariableby","parent":"receiver",
                   "next":null,"inputs":{"VALUE":[1,[4,"1"]]},
                   "fields":{"VARIABLE":["counter","counter"]}},
            "backdrop":{"opcode":"event_whenbackdropswitchesto",
                        "topLevel":true,"parent":null,"next":"touch",
                        "inputs":{},"fields":{"BACKDROP":["blank",null]}},
            "touch":{"opcode":"data_changevariableby","parent":"backdrop",
                     "next":null,"inputs":{"VALUE":[1,[4,"1"]]},
                     "fields":{"VARIABLE":["backdrops","backdrops"]}}
          },
          "costumes":[],"sounds":[]
        }
      ],
      "monitors":[],"extensions":[],"meta":{}
    })JSON";

    Runtime rt;
    const auto loaded = loadSb3(rt, project, nullptr);
    check(loaded.ok, "load reentrant backdrop-wait fixture");
    if (!loaded.ok) return;
    rt.greenFlag();
    rt.runUntilDone(150);
    Target* stage = rt.getTargetForStage();
    check(stage != nullptr && rt.threads.empty(),
          "reentrant backdrop-wait fixture finishes");
    if (!stage) return;
    auto val = [&](const char* key) {
        Variable* v = stage->lookupVariableById(key);
        return v ? cast::toNumber(v->value) : -1000;
    };
    check(val("counter") == 1, "pending broadcast receiver runs");
    check(val("backdrops") == 1, "backdrop event receiver runs");
    check(val("snapshot") == 1,
          "switch backdrop and wait yields the original caller thread");
}
} // namespace

int main() {
    testBroadcastIdCollision();
    testReentrantBackdropWait();
    std::printf("interpreter event regressions: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
