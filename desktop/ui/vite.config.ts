import { resolve } from "node:path";
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Library build of the UI language kit: one ES module plus one stylesheet
// (fonts emitted next to it). React stays external — the host app provides it.
export default defineConfig({
  plugins: [react()],
  build: {
    outDir: "dist",
    emptyOutDir: true,
    target: "es2021",
    cssCodeSplit: false,
    assetsInlineLimit: 0,
    lib: {
      entry: resolve(__dirname, "src/index.ts"),
      formats: ["es"],
      fileName: () => "index.js",
    },
    rollupOptions: {
      external: ["react", "react-dom", "react/jsx-runtime"],
      output: { assetFileNames: "style.css" },
    },
  },
});
