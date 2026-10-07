import {expect, it} from "vitest";

it("routes safety confirmations through the awaited dialog helper", () => {
  const sources = import.meta.glob("./**/*.{ts,tsx}", {query: "?raw", import: "default", eager: true}) as Record<string, string>;
  const direct = Object.entries(sources).filter(([path, source]) =>
    !path.includes(".test.") && path !== "./transport/dialogs.ts" && /window\s*\.\s*confirm\s*\(/.test(source)
  ).map(([path]) => path);
  expect(direct).toEqual([]);
});
