/// <reference types="vitest" />
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Tauri expects a fixed port and its own build output. `../dist` (from
// src-tauri) resolves to desktop/dist, which tauri.conf.json points at.
export default defineConfig({
  plugins: [react()],
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
    // Only the frontend sources: test discovery would otherwise crawl src-tauri/target, whose
    // cxx-build `crate` symlink loop makes `vitest run` spin forever once the Rust shell has
    // been built in this checkout.
    dir: "src",
  },
  build: {
    outDir: "dist",
    target: "es2021",
    sourcemap: false,
  },
});
