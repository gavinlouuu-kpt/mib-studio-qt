## 2026-10-19 — Desktop app imports the UI language kit from source

`desktop/src` can now `import { … } from "@mib/ui"` and its stylesheets (`@mib/ui/style.css`,
`@mib/ui/fonts.css`). `desktop/vite.config.ts` aliases the three names to `desktop/ui/src` and
`desktop/tsconfig.json` maps `@mib/ui` in `paths`, so the dev server, Vitest, `tsc`, both production
pages and the Tauri build use the kit with no prebuild step and the app's single React. The kit's
`dist/` build stays available for consumers outside `desktop/`. Nothing imports the kit in production
code yet: the production build output is byte-identical to before. Guard: `desktop/src/uiKit.test.tsx`.
Step A1 of track A (#716, #713). See [[frontend/UI-Language-Kit]] and [[architecture/Desktop-Shell]].
