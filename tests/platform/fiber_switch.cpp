// Native fiber smoke test: initial entry, yield, resume, completion and reset.
#include "engine/fiber.hpp"
#include <cstdio>

namespace {
struct State { int calls = 0; };
void run(void* arg) {
    auto& state = *static_cast<State*>(arg);
    ++state.calls;
    xyo::currentFiber()->yield();
    ++state.calls;
    xyo::currentFiber()->yield();
    ++state.calls;
}
bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "fiber: %s failed\n", message);
    return condition;
}
} // namespace

int main() {
    State state;
    xyo::Fiber fiber(run, &state);
    if (!check(fiber.stackBase() != nullptr, "stack allocation")) return 1;
    if (!check(!fiber.finished(), "initial state")) return 1;
    fiber.resume();
    if (!check(state.calls == 1 && !fiber.finished(), "first yield")) return 1;
    if (!check(fiber.suspendedSp() != nullptr, "suspended stack")) return 1;
    fiber.resume();
    if (!check(state.calls == 2 && !fiber.finished(), "second yield")) return 1;
    fiber.resume();
    if (!check(state.calls == 3 && fiber.finished(), "completion")) return 1;
    fiber.reset(run, &state);
    fiber.resume();
    if (!check(state.calls == 4 && !fiber.finished(), "reset resume")) return 1;
    fiber.resume();
    fiber.resume();
    if (!check(state.calls == 6 && fiber.finished(), "reset completion")) return 1;
    std::puts("fiber: OK");
    return 0;
}
