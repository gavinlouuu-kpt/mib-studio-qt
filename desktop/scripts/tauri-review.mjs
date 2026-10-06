// Runs the Tauri CLI for YOFO Review (ADR 0014): `node scripts/tauri-review.mjs
// <build|dev> [tauri options] [-- runner args]`. YOFO Review is this crate's
// binary built with `--no-default-features --features review-only` and
// tauri.review.conf.json; the default `studio` feature is MIB Studio. Cargo's
// `--no-default-features` can only reach the runner after `--`, so this wrapper
// adds it (and the config and feature) wherever callers append their own
// options — `npm run tauri:review:build -- --ci --bundles nsis` keeps working.
import { spawnSync } from "node:child_process";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const desktop = join(dirname(fileURLToPath(import.meta.url)), "..");
const [command, ...rest] = process.argv.slice(2);
if (command !== "build" && command !== "dev") {
  console.error("usage: tauri-review.mjs <build|dev> [tauri options] [-- runner args]");
  process.exit(2);
}
const sep = rest.indexOf("--");
const cli = sep === -1 ? rest : rest.slice(0, sep);
const runner = sep === -1 ? [] : rest.slice(sep + 1);
const hasFeatures = cli.some((a) => a === "-f" || a === "--features" || a.startsWith("--features="));
const args = [
  join(desktop, "node_modules", "@tauri-apps", "cli", "tauri.js"),
  command,
  "--config",
  "src-tauri/tauri.review.conf.json",
  ...(hasFeatures ? [] : ["--features", "review-only"]),
  ...cli,
  "--",
  "--no-default-features",
  ...runner,
];
const result = spawnSync(process.execPath, args, { cwd: desktop, stdio: "inherit" });
process.exit(result.status ?? 1);
