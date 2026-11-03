// MIB Studio tours: the checks that read files and git history. Every source an entry names exists (a repository path on disk, a
// before@ path at its commit when git has it, a goals@ document on the allowlist); every ui target has a data-tour attribute in
// production code; every evidence item names a real test or a step of the headless run. Lives outside src/ because it uses Node APIs
// and `tsc` (npm run build) has no Node types (see review-parity-charts.test.ts).
import { execFileSync } from "node:child_process";
import { existsSync, readdirSync, readFileSync, statSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import { sourcesOf } from "../src/tours/registry";
import { MIB_TOURS } from "../src/tours/mib/entries";
import { BEFORE_COMMITS, E2E_STEPS, GOALS_ALLOWLIST, GOALS_ROOT, splitSource } from "../src/tours/mib/sources";

const here = dirname(fileURLToPath(import.meta.url));
const SRC = resolve(here, "../src");           // desktop/src
const ROOT = resolve(here, "../..");           // the repository root
const read = (path: string) => readFileSync(join(ROOT, path), "utf-8");

describe("sources", () => {
  const git = (...args: string[]) => {
    try { return execFileSync("git", ["-C", ROOT, ...args], { stdio: ["ignore", "pipe", "ignore"] }).toString(); } catch { return null; }
  };
  const all = [...new Set(MIB_TOURS.flatMap((e) => sourcesOf(e)))];

  it("every repository path exists on disk", () => {
    const plain = all.filter((s) => !splitSource(s));
    expect(plain.length).toBeGreaterThan(30);
    expect(plain.filter((p) => !existsSync(join(ROOT, p)))).toEqual([]);
  });

  it("goals@845ca15c sources are the two documents on the allowlist, nothing else", () => {
    const goals = all.filter((s) => splitSource(s)?.root === GOALS_ROOT);
    expect(goals.length).toBeGreaterThan(0);
    for (const g of goals) expect(GOALS_ALLOWLIST).toContain(g);
  });

  it("before@ sources name a known commit, and the path exists there when git has the commit", () => {
    const before = all.filter((s) => splitSource(s)?.root.startsWith("before@"));
    expect(before.length).toBeGreaterThan(10);
    let checked = 0;
    for (const s of before) {
      const { root, path } = splitSource(s)!;
      const sha = root.slice("before@".length);
      expect(Object.keys(BEFORE_COMMITS), s).toContain(sha);
      if (git("cat-file", "-e", `${sha}^{commit}`) === null) continue;  // a shallow clone: the commit is not here
      expect(git("cat-file", "-e", `${sha}:${path}`), `${s} does not exist at ${sha}`).not.toBeNull();
      checked++;
    }
    // In a full clone every one is checked; in a shallow one none is, and the format checks above are all there is.
    expect([0, before.length]).toContain(checked);
  });

  it("every ui target has a data-tour attribute in production code", () => {
    const files: string[] = [];
    const walk = (dir: string) => {
      for (const name of readdirSync(dir)) {
        const p = join(dir, name);
        if (statSync(p).isDirectory()) { if (name !== "node_modules" && name !== "entries") walk(p); continue; }
        if (/\.(tsx?)$/.test(name) && !/\.test\.tsx?$/.test(name)) files.push(p);
      }
    };
    walk(SRC);
    const set = new Set<string>();
    for (const f of files) for (const m of readFileSync(f, "utf-8").matchAll(/data-tour="([^"]+)"/g)) set.add(m[1]);
    const targets = MIB_TOURS.flatMap((e) => e.steps.flatMap((s) => ("ui" in s.target ? [`${e.id}: ${s.target.ui}`] : [])));
    expect(targets.length).toBeGreaterThan(25);
    expect(targets.filter((t) => !set.has(t.split(": ")[1]))).toEqual([]);
    // Never a class chain or a guessed selector.
    for (const e of MIB_TOURS) for (const s of e.steps) if ("ui" in s.target) expect(s.target.ui).toMatch(/^[a-z]+(-[a-z]+)*$/);
  });

  it("no tour target is a canvas, so none relies on a guessed canvas selector", () => {
    for (const e of MIB_TOURS) {
      expect(e.canvasRegions).toBeUndefined();
      for (const s of e.steps) expect("canvas" in s.target).toBe(false);
    }
  });
});

describe("evidence", () => {
  const evidenceDoc = read("docs/evidence/fold-in-e2e-2026-11-02.md");
  const cmake = read("tests/CMakeLists.txt");
  const items = (g: "demonstrated" | "tested" | "uncertain") => MIB_TOURS.flatMap((e) => (e.evidence?.[g] ?? []).map((i) => ({ e, i })));

  it("every tour has the three groups, and uncertain is never empty", () => {
    for (const e of MIB_TOURS) {
      expect(e.evidence!.uncertain.length, e.id).toBeGreaterThanOrEqual(2);
      expect(e.evidence!.tested.length, e.id).toBeGreaterThanOrEqual(2);
      expect(e.evidence!.demonstrated.length, e.id).toBeGreaterThanOrEqual(2);
      expect(e.evidence!.demonstratedLabel).toMatch(/never hardware/);
    }
  });

  it("demonstrated items are dated for the browser drive and name their evidence class", () => {
    for (const { e, i } of items("demonstrated")) {
      expect(i.date, e.id).toBe("2026-11-03");
      expect(i.text, `${e.id}: ${i.text}`).toMatch(/mock camera|synthetic data \(production-writer fixture\)|real backend/);
    }
  });

  it("nothing claims hardware, the rig or a vendor-tool comparison as done", () => {
    for (const e of MIB_TOURS) {
      for (const { text } of [...e.evidence!.demonstrated, ...e.evidence!.tested]) {
        expect(text, `${e.id}: ${text}`).not.toMatch(/\b(on the rig|rig log shows|hardware run|vendor tool matches)\b/i);
      }
    }
  });

  it("tested items name a step of the headless run, a ctest, or a test that exists", () => {
    for (const { e, i } of items("tested")) {
      const test = i.test!;
      const where = `${e.id}: ${test}`;
      const step = /^fold_in_e2e: (.+)$/.exec(test);
      if (step) {
        expect(Object.values(E2E_STEPS), where).toContain(step[1]);
        expect(evidenceDoc, where).toContain(`| ${step[1]} |`);
        continue;
      }
      const base = test.replace(/ \(.*\)$/, "");
      const section = /\(section: ([^)]+)\)/.exec(test)?.[1];
      if (/^[a-z0-9_]+\.[a-z0-9_.]+$/.test(base)) {
        expect(cmake, where).toContain(`NAME ${base} `);
        if (section) expect(read(i.source), where).toContain(section);
        continue;
      }
      expect(i.source, where).toMatch(/\.(rs|tsx?)$/);
      expect(read(i.source), where).toContain(base);
    }
  });

  it("the evidence doc's step table names every step the tours cite, with its evidence class", () => {
    for (const step of Object.values(E2E_STEPS)) expect(evidenceDoc).toContain(`| ${step} |`);
  });
});

