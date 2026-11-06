// The desktop package has no @types/node. styles/subtabBodyGroup.test.tsx reads widgets.css from disk
// (vitest turns `?raw` CSS imports into ""), so the one call it needs is declared here.
declare module "node:fs" {
  export function readFileSync(path: string, encoding: "utf-8"): string;
}
