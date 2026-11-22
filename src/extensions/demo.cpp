// xyo-cpp: 拡張レジストリのデモ実装（Phase 0）。
#include "demo.hpp"

#include <utility>

#include "../core/cast.hpp"

namespace xyo {
namespace {

class DemoExtension final : public ExtensionInstance {
public:
    DemoExtension() {
        def_.id = "demo";
        def_.name = "Demo";
        def_.color = 0x0FBD8C;
        def_.blocks = {
            {"demo_double", "double %1", false, false, false, false, false},
            {"demo_next", "next", false, false, false, false, false},
            {"demo_set", "set counter to %1", false, false, false, false, false},
        };
    }

    const ExtensionDef& def() const override { return def_; }

    Value call(const std::string& opcode, const ArgValues& args,
               BlockUtility&) override {
        if (opcode == "demo_double")
            return Value::num(cast::toNumber(args.get("NUM")) * 2.0);
        if (opcode == "demo_next") return Value::num(next_++);
        if (opcode == "demo_set") {
            next_ = cast::toNumber(args.get("NUM"));
            return Value::undefined();
        }
        return Value::undefined();
    }

private:
    ExtensionDef def_;
    double next_ = 0.0;
};

} // namespace

bool registerDemoExtension(Runtime& rt, ExtensionRegistry& registry,
                           std::string* error) {
    return registry.add(rt, std::make_unique<DemoExtension>(), error);
}

} // namespace xyo
