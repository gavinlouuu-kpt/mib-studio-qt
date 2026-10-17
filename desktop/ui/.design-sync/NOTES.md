# design-sync notes — @mib/ui → Claude Design

- **Package:** `desktop/ui` (`@mib/ui`, global `window.MibUI`). It has no node_modules of its own and resolves react, vite and typescript from `desktop/node_modules`. Run `npm ci` in `desktop/` first, then build from `desktop/ui` with `PATH=../node_modules/.bin:$PATH npm run build`.
- **Converter:** run from `desktop/ui` with `--node-modules ../node_modules --entry ./dist/index.js --out ./ds-bundle`.
- **Fonts:** Inter (OFL) is subset to Latin woff2 in `src/fonts/` with `pyftsubset` plus brotli. Vite library mode inlines fonts as base64, so `scripts/copy-fonts.mjs` copies them to `dist/fonts/` and writes `dist/fonts.css`. design-sync picks them up through `extraFonts: ["dist/fonts.css"]`.
- **Provider:** `UiRoot` (div.mib-root) applies the font. Tokens sit on `:root`, so components still get their colours without it.
- **Guidelines:** `guidelinesGlob` is set to `guides/*.md`. The default (`docs/*.md`) would duplicate the per-component docs as guidelines.
- **Card modes:** Toolbar, AreaDeformChart, RecordIdentity, StatTile, AppShell, Card and StageTabs use `cardMode: column` because they are wider than a grid cell (`[GRID_OVERFLOW]`).
- **Playwright:** 1.62.1 is installed in `.ds-sync/` to match the cached chromium-1234. Install it with `PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1`.
- **Penpot wiring:** `penpot.json` maps every component to its Penpot boards, and each `docs/<Name>.md` ends with a "Penpot reference" section.

## Uploads

- 2026-10-16: first sync into the new project "MIB · YOFO UI language" (`14e57f39-…`). 78 files plus the anchor; render 13/13, 0 bad.

## Known render warns

- None after the authored previews.

## Re-sync risks

- **Penpot board ids** in `penpot.json` and `docs/*.md` change whenever a board is redrawn (the `replace()` helper mints a new id). Re-sync them from the drawing-state file before re-uploading.
- **Illustrative data:** the isoelastic curves in the `AreaDeformChart` preview are hand-made shapes, not the E-LUT. The component itself takes curves as data.
- **Toolchain:** the build was verified with Node 26 and npm 11. npm 11 blocks install scripts, but esbuild works without its postinstall.
