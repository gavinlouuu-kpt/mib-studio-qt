import { resolve } from "node:path";
import { configDefaults, defineConfig } from "vitest/config";
import react from "@vitejs/plugin-react";

// Tauri expects a fixed port and its own build output. `../dist` (from
// src-tauri) resolves to desktop/dist, which tauri.conf.json points at.
//
// Two pages, two products (plan 2026-10-01-standalone-review-app): `index.html`
// is MIB Studio, `review.html` is YOFO Review. Both are built into the same
// dist/; each Tauri config picks its page through `app.windows[].url`.
//
// The UI language kit `@mib/ui` (desktop/ui) is consumed from source: these
// aliases point the package's three entry points at `ui/src`, so the dev
// server, Vitest, `tsc` (tsconfig `paths`) and both production pages use the
// kit without a prebuild step and with the app's single React. The kit's
// source entry imports its own stylesheets, so `@mib/ui/style.css` resolves to
// that entry (one copy of the CSS, however many modules import it).
const kit = resolve(__dirname, "ui/src");
export default defineConfig({
  plugins: [react()],
  resolve: {
    alias: [
      { find: /^@mib\/ui$/, replacement: resolve(kit, "index.ts") },
      { find: /^@mib\/ui\/style\.css$/, replacement: resolve(kit, "index.ts") },
      { find: /^@mib\/ui\/fonts\.css$/, replacement: resolve(kit, "styles/fonts.css") },
    ],
  },
  clearScreen: false,
  server: {
    port: 1420,
    strictPort: true,
    watch: {
      // Never watch the Rust build tree: cxx-build creates a `crate` symlink
      // loop under src-tauri/target/**/cxxbridge that crashes the watcher
      // with ELOOP (kills the dev server moments after startup).
      ignored: ["**/src-tauri/**"],
    },
  },
  test: {
    // Same reason as the watcher: never crawl the Rust build tree (the
    // cxx-build `crate` symlink loop under src-tauri/target can stall
    // vitest's collection once a cargo build has run) or the built pages.
    exclude: [...configDefaults.exclude, "**/src-tauri/**", "**/dist/**"],
  },
  build: {
    outDir: "dist",
    target: "es2021",
    sourcemap: false,
    rollupOptions: {
      input: {
        main: resolve(__dirname, "index.html"),
        review: resolve(__dirname, "review.html"),
      },
    },
  },
});
