import { defineConfig } from "@playwright/test";
export default defineConfig({
  testDir: "tests",
  testMatch: "ui.spec.ts",
  use: { headless: true, viewport: { width: 1280, height: 900 } },
  webServer: {
    command: "npx vite --host 127.0.0.1 --port 4178",
    url: "http://127.0.0.1:4178",
    reuseExistingServer: false,
  },
});
