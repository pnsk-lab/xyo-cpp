#!/usr/bin/env bun
// xyo-cpp shared agent board. One task per file; one message per file.
// Claims use exclusive mkdir locks + atomic rename to prevent dual ownership.
import {mkdirSync, readFileSync, readdirSync, writeFileSync, renameSync,
  rmSync, existsSync} from "node:fs";
import {randomUUID} from "node:crypto";
import {dirname, join, resolve} from "node:path";
import {fileURLToPath} from "node:url";

const root = resolve(process.env.XYO_BOARD_ROOT ??
  join(dirname(fileURLToPath(import.meta.url)), "..", "board"));
const tasks = join(root, "tasks"), messages = join(root, "messages");
const statuses = ["open", "in_progress", "blocked", "review", "done"];
const today = () => new Date().toISOString().slice(0, 10);
function fail(msg: string): never { throw new Error("board: " + msg); }
function id(v?: string): string {
  if (!v || !/^[A-Z]{2,10}-\d{3}$/.test(v)) fail("ID must be IR-001 or TEST-004");
  return v;
}
function agent(v?: string): string {
  if (!v || !/^[a-zA-Z0-9][\w-]{0,63}$/.test(v)) fail("invalid agent name");
  return v;
}
type Task = {id: string; title: string; status: string; owner: string;
  priority: string; updated: string};
function pathOf(t: string) { return join(tasks, id(t) + ".md"); }
function read(t: string): {task: Task; content: string} {
  const path = pathOf(t);
  if (!existsSync(path)) fail("unknown task " + t);
  const content = readFileSync(path, "utf8");
  const header = /^---\n([\s\S]*?)\n---\n/.exec(content);
  if (!header) fail("invalid frontmatter " + t);
  const data: Record<string, string> = {};
  for (const line of header[1].split("\n")) {
    const k = line.indexOf(": ");
    if (k < 0) fail("invalid metadata line in " + t);
    data[line.slice(0, k)] = line.slice(k + 2);
  }
  if (data.id !== t || !statuses.includes(data.status)) fail("invalid task " + t);
  return {task: data as Task, content};
}
function change(content: string, field: string, value: string): string {
  const end = content.indexOf("\n---\n", 4);
  if (end < 0) fail("missing frontmatter");
  const head = content.slice(0, end), rx = new RegExp("^" + field + ": .*$", "m");
  if (!rx.test(head)) fail("missing " + field);
  return head.replace(rx, field + ": " + value) + content.slice(end);
}
function persist(t: string, data: Task, content: string): void {
  let next = change(content, "owner", data.owner);
  next = change(next, "status", data.status);
  next = change(next, "updated", today());
  const temp = pathOf(t) + ".tmp-" + randomUUID();
  try {
    writeFileSync(temp, next, {flag: "wx"});
    renameSync(temp, pathOf(t));
  } finally { if (existsSync(temp)) rmSync(temp); }
}
function locked(t: string, owner: string, action: () => void): void {
  const dir = join(root, ".locks", id(t));
  mkdirSync(join(root, ".locks"), {recursive: true});
  try { mkdirSync(dir); } catch (e: unknown) {
    if ((e as NodeJS.ErrnoException).code === "EEXIST") {
      fail("task busy: " + t + " (retry; check .locks for crashed writer)");
    }
    throw e;
  }
  try {
    writeFileSync(join(dir, "holder"), owner + " " + new Date().toISOString());
    action();
  } finally { rmSync(dir, {recursive: true, force: true}); }
}
function channel(raw?: string) {
  if (raw === "general") return "general";
  const t = id(raw); read(t); return t;
}
function posts(ch: string) {
  const dir = join(messages, ch);
  const entries = existsSync(dir) ?
    readdirSync(dir).filter(n => n.endsWith(".md")).sort() : [];
  if (!entries.length) console.log("(no messages)");
  for (const file of entries) console.log(readFileSync(join(dir, file), "utf8"));
}
const [cmd, ...a] = process.argv.slice(2);
try {
switch (cmd) {
  case undefined: case "help": {
    console.log([
      "bun tools/board.ts list",
      "bun tools/board.ts show IR-001",
      "bun tools/board.ts claim IR-001 agent-llvm",
      "bun tools/board.ts move IR-001 review agent-llvm",
      "bun tools/board.ts release IR-001 agent-llvm",
      'bun tools/board.ts post IR-001 agent-llvm "message"',
      'bun tools/board.ts post general agent-llvm "message"',
      "bun tools/board.ts read IR-001",
      'bun tools/board.ts new IR-009 "Task name" P1',
    ].join("\n")); break;
  }
  case "list": {
    console.log("ID       STATUS       OWNER             PRIORITY TITLE");
    const entries = readdirSync(tasks).filter(n => /^[A-Z]{2,10}-\d{3}\.md$/.test(n));
    const all = entries.map(n => read(n.slice(0, -3)).task)
      .sort((x, y) => x.priority.localeCompare(y.priority) || x.id.localeCompare(y.id));
    for (const t of all) console.log(t.id.padEnd(8) + " " +
      t.status.padEnd(12) + " " + t.owner.padEnd(17) + " " +
      t.priority.padEnd(8) + " " + t.title);
    break;
  }
  case "show": {
    const t = id(a[0]); console.log(read(t).content);
    const dir = join(messages, t);
    console.log("Recent posts:");
    if (existsSync(dir)) for (const file of readdirSync(dir)
      .filter(n => n.endsWith(".md")).sort().slice(-5)) {
      console.log(readFileSync(join(dir, file), "utf8"));
    }
    break;
  }
  case "claim": {
    const t = id(a[0]), by = agent(a[1]);
    locked(t, by, () => {
      const {task, content} = read(t);
      if (task.status === "done" || (task.owner !== "unassigned" && task.owner !== by) ||
        (task.status === "review" && task.owner !== by)) fail("not claimable: " + t);
      task.owner = by; task.status = "in_progress";
      persist(t, task, content); console.log("claimed " + t + " by " + by);
    }); break;
  }
  case "move": case "release": {
    const t = id(a[0]), next = cmd === "release" ? "open" : a[1],
      by = agent(cmd === "release" ? a[1] : a[2]);
    if (!statuses.includes(next) || (cmd === "move" && next === "open")) fail("bad status");
    locked(t, by, () => {
      const {task, content} = read(t);
      if (task.owner !== by || (task.status === "done" && cmd === "release")) {
        fail("owner mismatch or task completed: " + t);
      }
      task.status = next;
      if (cmd === "release") task.owner = "unassigned";
      persist(t, task, content); console.log(t + " -> " + next);
    }); break;
  }
  case "post": {
    const ch = channel(a[0]), by = agent(a[1]), message = a.slice(2).join(" ").trim();
    if (!message) fail("empty message");
    const dir = join(messages, ch); mkdirSync(dir, {recursive: true});
    const stamp = new Date().toISOString();
    const name = stamp.replace(/[:.]/g, "-") + "-" + randomUUID() + ".md";
    writeFileSync(join(dir, name), "## " + stamp + " — " + by + "\n\n" + message + "\n",
      {flag: "wx"});
    console.log("posted " + ch + "/" + name); break;
  }
  case "read": { posts(channel(a[0])); break; }
  case "new": {
    const t = id(a[0]), title = (a[1] ?? "").trim(), p = a[2] ?? "P2";
    if (!title || /[\r\n]/.test(title) || !/^P[0-3]$/.test(p)) fail("bad title/priority");
    locked(t, "board", () => {
      if (existsSync(pathOf(t))) fail("task exists: " + t);
      mkdirSync(tasks, {recursive: true});
      writeFileSync(pathOf(t), "---\nid: " + t + "\ntitle: " + title +
        "\nstatus: open\nowner: unassigned\npriority: " + p +
        "\nupdated: " + today() + "\n---\n\n## Scope\n\nTBD\n\n" +
        "## Acceptance criteria\n\n- [ ] Validation posted\n\n" +
        "## Files / dependencies\n\nTBD\n", {flag: "wx"});
      console.log("created " + t);
    }); break;
  }
  default: fail("unknown command: " + cmd);
}
} catch (error: unknown) {
  console.error(error instanceof Error ? error.message : String(error));
  process.exitCode = 1;
}
