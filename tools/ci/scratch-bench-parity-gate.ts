/**
 * ScratchBench parity criteria for the pinned fixture in CI.
 *
 * Interpreter, HotSpot, Eager, and AOT were each verified against all 2656
 * checks. The CI must also verify that AOT emitted a nonempty native object,
 * because ScratchBench falls back to JIT if AOT compilation fails.
 */
export const EXPECTED_PARITY_CHECKS = 2656;
const FULL_PARITY_VARIANTS = new Set(["hotspot", "interpreter", "eager", "aot"]);

export type ParityGateInput = {
  variant: string;
  verdict: "passed" | "failed";
  passed: number;
  total: number;
};

/** Return the reason for failure, or null when the variant meets its gate. */
export function parityGate(input: ParityGateInput): string | null {
  const { variant, verdict, passed, total } = input;
  if (FULL_PARITY_VARIANTS.has(variant)) {
    if (total !== EXPECTED_PARITY_CHECKS) {
      return `Unexpected parity fixture for ${variant}: ${total} checks (expected ${EXPECTED_PARITY_CHECKS})`;
    }
    if (verdict !== "passed" || passed !== total) {
      return `Parity not 100% for ${variant}: ${passed}/${total} (${verdict})`;
    }
    return null;
  }
  return `Unexpected xyo-cpp parity variant: ${variant}`;
}
