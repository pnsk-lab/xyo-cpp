import { expect, test } from "bun:test";
import { analyzeProject, parseCsv } from "../../../tools/quickening_corpus.ts";

test("parse quoted commas, quotes and multiline CSV", () => {
  const data = 'one,"a,b","hello ""world""",yes,ok\n' +
    'two,"line\nbreak",x,y,z\npartial,';
  expect(parseCsv(data)).toEqual([
    ["one", "a,b", 'hello "world"', "yes", "ok"],
    ["two", "line\nbreak", "x", "y", "z"],
  ]);
});

test("count opcode chains separately from expression inputs", () => {
  const result = analyzeProject({
    targets: [{
      blocks: {
        a: { opcode: "event_whenflagclicked", topLevel: true, next: "b" },
        b: { opcode: "data_setvariableto", next: "c", inputs: { VALUE: [2, "d"] } },
        c: { opcode: "data_changevariableby", next: null },
        d: { opcode: "operator_add", next: null },
      },
    }],
  });
  expect(result.blocks).toBe(4);
  expect(result.pairs["event_whenflagclicked > data_setvariableto"]).toBe(1);
  expect(result.ngrams[
    "event_whenflagclicked > data_setvariableto > data_changevariableby"
  ]).toBe(1);
  expect(result.inputEdges["data_setvariableto <- operator_add"]).toBe(1);
});

test("independent objects have repeatable fingerprint", () => {
  const p = { targets: [{ blocks: {
    a: { opcode: "event_whenflagclicked", topLevel: true, next: "b" },
    b: { opcode: "motion_movesteps", next: null },
  } }] };
  expect(analyzeProject(p).fingerprint).toBe(analyzeProject(p).fingerprint);
});

test("different branch bodies are distinguishable in structural fingerprint", () => {
  const make = (body: string) => ({
    targets: [{ blocks: {
      a: { opcode: "event_whenflagclicked", topLevel: true, next: "b" },
      b: { opcode: "control_forever", inputs: { SUBSTACK: [2, "c"] } },
      c: { opcode: body },
    } }],
  });
  const x = analyzeProject(make("data_changevariableby"));
  const y = analyzeProject(make("looks_hide"));
  expect(x.fingerprint).not.toBe(y.fingerprint);
  expect(x.inputEdges["control_forever <- data_changevariableby"]).toBeUndefined();
});

test("modified randomized IDs preserve normalized structure", () => {
  const make = (a: string, b: string) => ({
    targets: [{ blocks: {
      [a]: { opcode: "event_whenflagclicked", topLevel: true, next: b },
      [b]: { opcode: "motion_movesteps", next: null },
    } }],
  });
  expect(analyzeProject(make("a", "b")).fingerprint)
    .toBe(analyzeProject(make("x", "y")).fingerprint);
});
