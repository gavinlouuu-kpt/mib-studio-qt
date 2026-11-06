import { test, expect } from "@playwright/test";
test("guided click-through gates actions and exposes only device and dot exports", async ({
  page,
}) => {
  let state = "empty";
  let feedback = false;
  let manual: number[][] = [];
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
          generated_preview:
            state === "ready"
              ? {
                  exports: [{name:"design_with_dotgrid.dwg",bytes:44037408},{name:"dotgrid_layer.oas",bytes:1517576},{name:"design_with_dotgrid.dxf",bytes:363000000},{name:"dotgrid_layer.gds",bytes:75000000}],
                  dot_count: 2,
                  overview_png: "",
                  detail_bounds: [0, 0, 1000, 1000],
                  detail_points: [
                    [100, 100],
                    [200, 200],
                  ],
                  dot_radius_um: 6,
                }
              : undefined,
          placement_contract: 1,
          manual_chip_rectangles: manual,
          extent_um: [6000, 6000],
          entity_count: 2,
          no_dot_regions: 1,
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
  await page
    .getByText("Boundary missing? Define a rectangular chip", { exact: true })
    .click();
  for (const [id, v] of [
    ["chip-x0", "-1000"],
    ["chip-y0", "-1000"],
    ["chip-x1", "7000"],
    ["chip-y1", "7000"],
  ])
    await page.locator(`#${id}`).fill(v);
  await page.locator("#add-manual-chip").click();
  await expect(page.locator("#preview")).toHaveAttribute(
    "viewBox",
    "-1000 -1000 8000 8000",
  );
  await page.locator("#region-tour").click();
  for (let n = 0; n < 3; n++)
    await page.locator(".driver-popover-next-btn").click();
  await expect(page.locator(".driver-popover-title")).toHaveText(
    "Feedback checkpoint",
  );
  await page.locator(".driver-popover-close-btn").click();
  await page.locator("#confirmed").check();
  await page.locator("#generate").click();
  await expect(page.locator("#downloads")).toBeVisible();
  await expect(page.locator("#links a:visible")).toHaveCount(2);
  await expect(page.getByRole("link", {name: /DWG/})).toContainText("44.04 MB");
  await page.getByText("Other file formats", {exact:true}).click();
  await expect(page.locator("#links a:visible")).toHaveCount(4);
  await expect(page.locator("#dot-summary")).toContainText("2 generated dots");
  await page.locator("#dot-detail").click();
  await expect(page.locator("[data-dot-detail] circle")).toHaveCount(2);
  await expect(page.locator("#preview")).toHaveAttribute(
    "viewBox",
    "0 5000 1000 1000",
  );
  await page.locator("#reset-view").click();
  await expect(page.locator("[data-dot-detail]")).toBeHidden();
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

test("unlabelled CAD requires explicit area selection and resets confirmation", async ({
  page,
}) => {
  let selected: unknown;
  const inspection = {
    placement_contract: 1,
    extent_um: [6000, 6000],
    entity_count: 2,
    source_units: "um",
    converter: null,
    paths: [],
    warnings: [],
    no_dot_regions: 0,
    generation_supported: true,
    region_candidates: [
      {
        id: 1,
        kind: "polygon",
        points: [
          [0, 0],
          [6000, 0],
          [6000, 6000],
          [0, 6000],
        ],
      },
      {
        id: 0,
        kind: "polygon",
        points: [
          [1000, 1000],
          [5000, 1000],
          [5000, 2000],
          [1000, 2000],
        ],
      },
    ],
  };
  await page.route("**/api/**", async (route) => {
    const path = new URL(route.request().url()).pathname;
    if (path.endsWith("/confirm")) {
      selected = route.request().postDataJSON().selected_regions;
      await route.fulfill({ json: { status: "queued_generate" } });
      return;
    }
    await route.fulfill({
      json: path.endsWith("/me")
        ? { email: "test@test.test", role: "member" }
        : path.endsWith("/designs")
          ? [
              {
                id: "dg-aaaa",
                name: "Plain CAD",
                status: "awaiting_confirmation",
              },
            ]
          : {
              id: "dg-aaaa",
              name: "Plain CAD",
              status: "awaiting_confirmation",
              inspection,
            },
    });
  });
  await page.goto("http://127.0.0.1:4178");
  await page.getByRole("button", { name: /Plain CAD/ }).click();
  await page.locator("#confirmed").check();
  await expect(page.locator("#generate")).toBeDisabled();
  await page
    .getByRole("button", { name: "Guide me through chip placement" })
    .click();
  await page.locator(".driver-popover-next-btn").click();
  await page.locator(".driver-popover-next-btn").click();
  await expect(page.locator(".driver-popover-title")).toHaveText(
    "Separate chip edges from fluid",
  );
  await page.locator(".driver-popover-close-btn").click();
  await page.locator("#region-choice").selectOption("1");
  await page.locator("#toggle-region").click();
  await page.locator("#region-role").selectOption("fluid");
  await page.locator('[data-region="0"]').click();
  await expect(page.locator("#confirmed")).not.toBeChecked();
  await expect(page.locator("#generate")).toBeDisabled();
  await page.locator("#confirmed").check();
  await expect(page.locator("#generate")).toBeEnabled();
  await page
    .getByRole("button", { name: "Guide me through chip placement" })
    .click();
  for (let n = 0; n < 3; n++)
    await page.locator(".driver-popover-next-btn").click();
  await expect(page.locator(".driver-popover-title")).toHaveText(
    "Feedback checkpoint",
  );
  await page.locator(".driver-popover-next-btn").click();
  await page.locator("#generate").click();
  expect(selected).toEqual([0]);
});
