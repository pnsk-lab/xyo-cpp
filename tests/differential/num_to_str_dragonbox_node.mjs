// Compare a compiled Dragonbox backend with actual V8 String(number).
// Usage: node tests/differential/num_to_str_dragonbox_node.mjs <exe>
import { readFileSync } from "node:fs";

// Read a piped C++ generator. This avoids a nested Node subprocess, which
// restricted local runners may disallow.
const input = readFileSync(0, "utf8");
const buffer = Buffer.allocUnsafe(8);
let checked = 0;
for (const line of input.trimEnd().split("\n")) {
  const firstSpace = line.indexOf(" ");
  if (firstSpace !== 16) throw new Error("malformed: " + line);
  buffer.writeBigUInt64LE(BigInt("0x" + line.slice(0, 16)), 0);
  const value = buffer.readDoubleLE(0);
  const expected = String(value);
  const actual = line.slice(17);
  if (actual !== expected) {
    throw new Error(
      "mismatch bits=" + line.slice(0,16) +
      " xyo=" + actual + " V8=" + expected
    );
  }
  checked++;
}
if (checked !== 602023) throw new Error("unexpected count " + checked);
console.log("PASS " + checked + " number strings match Node.js/V8");
