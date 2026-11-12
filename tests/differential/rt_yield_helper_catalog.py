#!/usr/bin/env python3
"""RT-001: detect yield-helper classifier drift between Codegen and probe.

The yield-edge diagnostic must never silently omit a helper that Codegen
already knows might suspend. This is a source-level contract check, NOT
proof that those helpers are safe or that the list is globally complete.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/jit/codegen.cpp"
DIAGNOSTIC = ROOT / "src/ir/rt_yield_edge_stackmap.hpp"


def names(text: str, pattern: str) -> set[str]:
    match = re.search(pattern, text, flags=re.DOTALL)
    if not match:
        raise AssertionError("missing explicitly matched yield helper list")
    items = re.findall(r'"([^"]+)"', match.group(1))
    if not items or len(items) != len(set(items)):
        raise AssertionError("empty or duplicate yield helper names")
    return set(items)


def main() -> None:
    source = names(
        SOURCE.read_text(encoding="utf-8"),
        r"static bool helperMayYield\(const char\* n\)\s*\{.*?"
        r"static const std::unordered_set<std::string_view> k = "
        r"\{(.*?)\};",
    )
    diagnostics = names(
        DIAGNOSTIC.read_text(encoding="utf-8"),
        r"inline bool knownPotentiallyYieldingHelper\("
        r"llvm::StringRef name\)\s*\{.*?"
        r"static constexpr const char\* helpers\[\] = "
        r"\{(.*?)\};",
    )
    expected = source | {"xyo_rt_async_reporter_rooted"}
    if diagnostics != expected:
        raise AssertionError(
            f"unsafe yield-helper classifier drift: "
            f"missing={sorted(expected - diagnostics)}, "
            f"unaccounted={sorted(diagnostics - expected)}"
        )
    print(
        f"PASS RT-001 yield-helper catalog: {len(source)} native helpers "
        "+ rooted async reporter recognized; all non-direct helpers "
        "remain unverified for machine GC; gc_safe=0"
    )


if __name__ == "__main__":
    main()
