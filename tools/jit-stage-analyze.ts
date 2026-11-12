#!/usr/bin/env bun
/**
 * PROF-009: summarize existing JIT compilation timing diagnostics.
 *
 * Enable XYO_JIT_TIMING=1, optionally XYO_JIT_PASS_TIMING=1 and
 * XYO_JIT_COMPILE_STATS=1, and redirect stderr to a log.
 *
 * "lookup" includes LLVM ORC materialization, machine-code generation,
 * linking and symbol resolution; it is not an isolated codegen timer.
 * Pass logs are incomplete (the backend prints only >5ms exclusive passes).
 */
import { readFileSync } from "node:fs";

export type JitStage = {
  script: string;
  emitMs: number;
  optMs: number;
  submitMs: number;
  lookupMs: number;
  totalMs: number;
  functions: number;
  irInstructions: number;
  tier: "eager" | "lazy";
};
export type JitPass = {
  script: string;
  name: string;
  exclusiveMs: number;
  invocations: number;
};
export type JitLazyModule = {
  firstFunction: string;
  sourceProcedure?: string;
  functions: number;
  irInstructions: number;
  optMs: number;
};
export type JitTimingLog = {
  stages: JitStage[];
  passes: JitPass[];
  lazyModules: JitLazyModule[];
  lazyOptimizationMs: number[];
  asyncFinishMs: number[];
  syncCompileMs?: number;
};

const timingPattern =
  /^\[jit-timing\] (.+?) emit=([\d.]+)ms opt=([\d.]+)ms submit=([\d.]+)ms lookup=([\d.]+)ms total=([\d.]+)ms funcs=(\d+) ir=(\d+) tier=(eager|lazy)$/gm;
const passPattern = /^\[jit-pass\] (.+?) ([\d.]+)ms (\d+) (.+)$/gm;
const lazyPattern = /^\[jit-lazy-ir\] ([\d.]+)ms$/gm;
const lazyDetailPattern =
  /^\[jit-lazy-detail\] first=(.*?) funcs=(\d+) ir=(\d+) opt_ms=([\d.]+)$/gm;
const procedurePattern = /^\[jit-proc-map\] function=(\S+) key=(.*)$/gm;
const asyncPattern = /^\[jit-async\] .+? finished ([\d.]+)ms$/gm;
const compilePattern =
  /^\[jit-compile\] sync attempts=\d+ success=\d+ total_ms=([\d.]+)$/gm;

export function parseJitTiming(log: string): JitTimingLog {
  const stages: JitStage[] = [];
  for (const m of log.matchAll(timingPattern)) {
    stages.push({
      script: m[1],
      emitMs: Number(m[2]), optMs: Number(m[3]),
      submitMs: Number(m[4]), lookupMs: Number(m[5]),
      totalMs: Number(m[6]), functions: Number(m[7]),
      irInstructions: Number(m[8]), tier: m[9] as JitStage["tier"],
    });
  }
  const passes = [...log.matchAll(passPattern)].map((m) => ({
    script: m[1], exclusiveMs: Number(m[2]),
    invocations: Number(m[3]), name: m[4],
  }));
  const sync = [...log.matchAll(compilePattern)].at(-1);
  const procNames = new Map(
    [...log.matchAll(procedurePattern)].map((m) => [m[1], m[2]]),
  );
  const lazyModules = [...log.matchAll(lazyDetailPattern)].map((m) => ({
    firstFunction: m[1], functions: Number(m[2]),
    irInstructions: Number(m[3]), optMs: Number(m[4]),
    ...(procNames.has(m[1]) ? { sourceProcedure: procNames.get(m[1]) } : {}),
  }));
  return {
    stages, passes, lazyModules,
    lazyOptimizationMs: [...log.matchAll(lazyPattern)].map((m) => Number(m[1])),
    asyncFinishMs: [...log.matchAll(asyncPattern)].map((m) => Number(m[1])),
    ...(sync ? { syncCompileMs: Number(sync[1]) } : {}),
  };
}

export function summarizeJitTiming(logs: JitTimingLog[]) {
  const stages = logs.flatMap((log) => log.stages);
  const sum = (key: "emitMs" | "optMs" | "submitMs" | "lookupMs" | "totalMs") =>
    stages.reduce((acc, stage) => acc + stage[key], 0);
  const emitMs = sum("emitMs");
  const optMs = sum("optMs");
  const submitMs = sum("submitMs");
  const lookupMs = sum("lookupMs");
  const totalMs = sum("totalMs");
  const lazyModules = logs.flatMap((log) => log.lazyModules);
  const byPass = new Map<string, { exclusiveMs: number; invocations: number }>();
  for (const pass of logs.flatMap((log) => log.passes)) {
    const data = byPass.get(pass.name) ?? { exclusiveMs: 0, invocations: 0 };
    data.exclusiveMs += pass.exclusiveMs;
    data.invocations += pass.invocations;
    byPass.set(pass.name, data);
  }
  return {
    scripts: stages.length,
    eager: stages.filter((s) => s.tier === "eager").length,
    lazy: stages.filter((s) => s.tier === "lazy").length,
    emitMs, optMs, submitMs, lookupMs, totalMs,
    unaccountedMs: totalMs - emitMs - optMs - submitMs - lookupMs,
    lazyOptimizationMs: logs.flatMap((log) => log.lazyOptimizationMs)
      .reduce((a, b) => a + b, 0),
    asyncFinishMs: logs.flatMap((log) => log.asyncFinishMs)
      .reduce((a, b) => a + b, 0),
    syncCompileMs: logs.reduce((a, log) => a + (log.syncCompileMs ?? 0), 0),
    lazyModuleCount: lazyModules.length,
    lazyModuleMs: lazyModules.reduce((a, b) => a + b.optMs, 0),
    topLazyModules: [...lazyModules].sort((a, b) => b.optMs - a.optMs)
      .slice(0, 20),
    passSummary: [...byPass].map(([name, counts]) => ({ name, ...counts }))
      .sort((a, b) => b.exclusiveMs - a.exclusiveMs || a.name.localeCompare(b.name)),
    topScripts: [...stages].sort((a, b) => b.totalMs - a.totalMs).slice(0, 15),
    passListingIncomplete: true,
  };
}

if (import.meta.main) {
  const args = process.argv.slice(2);
  const json = args.includes("--json");
  const paths = args.filter((a) => a !== "--json");
  if (paths.some((p) => p.startsWith("--"))) {
    console.error("Usage: bun tools/jit-stage-analyze.ts [--json] [logfile...]");
    process.exit(2);
  }
  const logs = paths.length
    ? paths.map((p) => parseJitTiming(readFileSync(p, "utf8")))
    : [parseJitTiming(readFileSync(0, "utf8"))];
  const summary = summarizeJitTiming(logs);
  if (json) console.log(JSON.stringify(summary, null, 2));
  else {
    console.log("scripts=" + summary.scripts + " eager=" + summary.eager +
                " lazy=" + summary.lazy);
    for (const [name, ms] of [
      ["IR emission", summary.emitMs],
      ["LLVM optimization", summary.optMs],
      ["ORC addIRModule", summary.submitMs],
      ["ORC lookup/materialization", summary.lookupMs],
      ["Other setup", summary.unaccountedMs],
      ["Total", summary.totalMs],
    ] as const) console.log(name + ": " + ms.toFixed(3) + " ms");
    if (summary.lazyOptimizationMs || summary.asyncFinishMs)
      console.log("Lazy-opt=" + summary.lazyOptimizationMs.toFixed(3) +
        "ms async-finish=" + summary.asyncFinishMs.toFixed(3) + "ms");
    console.log("Slow passes (over 5ms, incomplete listing):");
    for (const p of summary.passSummary.slice(0, 12))
      console.log(p.name + ": " + p.exclusiveMs.toFixed(3) +
                  " ms / " + p.invocations + " calls");
    if (summary.lazyModuleCount) {
      console.log("Deferred optimizer modules: " + summary.lazyModuleCount +
                  ", total " + summary.lazyModuleMs.toFixed(3) + " ms");
      for (const mod of summary.topLazyModules.slice(0, 10))
        console.log("  " + (mod.sourceProcedure ?? mod.firstFunction) +
          " [" + mod.firstFunction + "]: " + mod.optMs.toFixed(3) +
          " ms (" + mod.irInstructions + " IR instructions)");
    }
    console.log("Slow scripts:");
    for (const s of summary.topScripts.slice(0, 10))
      console.log(s.script + ": total=" + s.totalMs.toFixed(3) +
        " opt=" + s.optMs.toFixed(3) +
        " lookup=" + s.lookupMs.toFixed(3) + " tier=" + s.tier);
    console.log("Lookup contains ORC machine code emission and linking; " +
                "the displayed LLVM pass list omits shorter passes.");
  }
}
