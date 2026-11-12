import { expect, test } from "bun:test";
import { median, schedule, summarize, type Reading } from "./jit-cfg-selective-ab";

const make = (mode: Reading["mode"], rate: number): Reading => ({
  mode, name: "math_mandel", iteration: 0, rate,
  checksum: "1957", wallMs: 1000, ok: true,
});

test("schedule includes every mode exactly once", () => {
  for (let round = 0; round < 12; round++)
    expect(new Set(schedule(round))).toEqual(new Set(["off", "broad", "selective"]));
});

test("median and multi-mode ratios", () => {
  expect(median([4, 1, 2, 3])).toBe(2.5);
  const data = [
    make("off", 10), make("off", 20),
    make("broad", 15), make("broad", 15),
    make("selective", 30), make("selective", 30),
  ];
  expect(summarize(data)).toEqual({
    off: 15, broad: 15, selective: 30,
    broadRatio: 1, selectiveRatio: 2, selectiveVsBroad: 2,
  });
});

test("refuse failed runs", () => {
  const bad = make("off", 42);
  bad.ok = false;
  expect(() => summarize([bad])).toThrow();
});
