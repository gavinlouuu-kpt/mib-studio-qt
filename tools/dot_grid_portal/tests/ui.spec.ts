import { test, expect } from "@playwright/test";
test("guided click-through gates actions and exposes only device and dot exports", async ({
  page,
}) => {
  let state = "empty";
  let feedback = false;
  await page.route("**/api/**", async (route) => {
    const url = new URL(route.request().url());
    const path = url.pathname;
    const method = route.request().method();
    let result: unknown = {};
    if (path === "/api/me")
      result = { email: "test@example.test", role: "member" };
    else if (path === "/api/designs" && method === "POST") {
      state = "awaiting_confirmation";
      result = { id: "dg-aaaa" };
    } else if (path === "/api/designs")
      result =
        state === "empty"
          ? []
          : [{ id: "dg-aaaa", name: "Test drawing", status: state }];
    else if (path.endsWith("/confirm")) {
      state = "ready";
      result = { status: state };
    } else if (path === "/api/designs/dg-aaaa")
      result = {
        id: "dg-aaaa",
        name: "Test drawing",
        status: state,
        inspection: {
          extent_um: [6000, 6000],
          entity_count: 2,
          source_units: "um",
          converter: null,
          paths: [
            [
              [0, 0],
              [6000, 0],
              [6000, 6000],
              [0, 6000],
              [0, 0],
            ],
          ],
          warnings: ["Synthetic UI fixture — not a CAD engine test."],
        },
      };
    else if (path === "/api/feedback") {
      feedback = true;
      result = { saved: true };
    } else return route.fulfill({ status: 404, json: { error: "Not found" } });
    await route.fulfill({ json: result });
  });
  await page.goto("http://127.0.0.1:4178");
  await page.getByRole("button", { name: "Take a tour" }).click();
  await page.locator(".driver-popover-next-btn").click();
  await page.locator(".driver-popover-next-btn").click();
  await expect(page.locator(".driver-popover-title")).toHaveText(
    "Inspect before generation",
  );
  await page.locator(".driver-popover-close-btn").click();
  await page.locator("#name").fill("Test drawing");
  await page.locator("#file").setInputFiles({
    name: "device.dxf",
    mimeType: "application/octet-stream",
    buffer: Buffer.from("UI fixture"),
  });
  await page.locator("#units").selectOption("um");
  await page.getByRole("button", { name: "Upload & inspect" }).click();
  await page.getByRole("button", { name: "Refresh", exact: true }).click();
  await expect(page.locator("#review")).toBeVisible();
  await expect(page.locator("#generate")).toBeDisabled();
  await page.locator("#confirmed").check();
  await page.locator("#generate").click();
  await expect(page.locator("#downloads")).toBeVisible();
  await expect(page.locator("#links a")).toHaveCount(2);
  await page.getByRole("button", { name: "Take a tour" }).click();
  for (let n = 0; n < 4; n++)
    await page.locator(".driver-popover-next-btn").click();
  await expect(page.locator(".driver-popover-title")).toHaveText(
    "Feedback checkpoint",
  );
  await page.locator(".driver-popover-next-btn").click();
  await page.locator("#feedback-text").fill("Preview is clear");
  await page.getByRole("button", { name: "Save feedback" }).click();
  await expect(page.getByRole("status")).toContainText("Feedback saved");
  expect(feedback).toBe(true);
  await page.screenshot({
    path: "test-results/portal-workflow.png",
    fullPage: true,
  });
});

test("realistic example compares exported geometry and loads source units", async ({
  page,
}) => {
  await page.route("**/api/**", (route) =>
    route.fulfill({
      json: route.request().url().endsWith("/me")
        ? { email: "review@example.test", role: "member" }
        : [],
    }),
  );
  await page.goto("http://127.0.0.1:4178");
  await expect(page.locator("#demo-result")).toContainText("generated dots");
  await page.getByRole("button", { name: "Hide added dots" }).click();
  await expect(page.locator("#demo-image")).toHaveAttribute(
    "src",
    "/demo/before.svg",
  );
  await page.getByRole("button", { name: "Show added dots" }).click();
  await expect(page.locator("#demo-image")).toHaveAttribute(
    "src",
    "/demo/after.svg",
  );
  await page.getByRole("button", { name: "Explain this example" }).click();
  for (const title of [
    "Start with recognizable device CAD",
    "See exactly what gets added",
    "Download device and dots",
    "Try the real workflow",
  ]) {
    await expect(page.locator(".driver-popover-title")).toHaveText(title);
    await page.locator(".driver-popover-next-btn").click();
  }
  await page.getByRole("button", { name: "Use this sample CAD" }).click();
  await expect(page.locator("#units")).toHaveValue("mm");
  expect(
    await page
      .locator("#file")
      .evaluate((e: HTMLInputElement) => e.files?.[0].name),
  ).toBe("sorter-concept.dxf");
  await expect(page.locator("#message")).toContainText("Sample loaded");
});
