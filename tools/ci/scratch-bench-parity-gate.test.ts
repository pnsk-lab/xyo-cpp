import { expect, test } from "bun:test";
import {
  EXPECTED_PARITY_CHECKS,
  parityGate,
} from "./scratch-bench-parity-gate.ts";

for (const variant of ["hotspot", "interpreter", "eager", "aot"]) {
  test(`${variant}: exactly 2656 of 2656 with PASSED succeeds`, () => {
    expect(parityGate({
      variant, verdict: "passed",
      passed: EXPECTED_PARITY_CHECKS, total: EXPECTED_PARITY_CHECKS,
    })).toBeNull();
  });

  test(`${variant}: a single failure is rejected`, () => {
    expect(parityGate({
      variant, verdict: "failed",
      passed: EXPECTED_PARITY_CHECKS - 1, total: EXPECTED_PARITY_CHECKS,
    })).toContain("not 100%");
  });

  test(`${variant}: stale 2123-case source is rejected`, () => {
    expect(parityGate({
      variant, verdict: "passed", passed: 2123, total: 2123,
    })).toContain("Unexpected parity fixture");
  });

  test(`${variant}: failed verdict is rejected even with full score`, () => {
    expect(parityGate({
      variant, verdict: "failed",
      passed: EXPECTED_PARITY_CHECKS, total: EXPECTED_PARITY_CHECKS,
    })).toContain("not 100%");
  });
}

test("Unknown variants fail closed", () => {
  expect(parityGate({
    variant: "jit-with-typo", verdict: "passed",
    passed: EXPECTED_PARITY_CHECKS, total: EXPECTED_PARITY_CHECKS,
  })).toContain("Unexpected xyo-cpp parity variant");
});
