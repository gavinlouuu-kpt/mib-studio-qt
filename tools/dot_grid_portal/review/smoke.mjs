import { chromium } from "@playwright/test";
const browser = await chromium.launch({ headless: true });
const page = await browser.newPage({ viewport: { width: 1280, height: 950 } });
const url = process.env.REVIEW_ORIGIN;
if (!url) throw new Error("REVIEW_ORIGIN required");
try {
  await page.goto(url);
  await page
    .getByText("Tailnet review · shared workspace", { exact: false })
    .waitFor();
  await page.locator("#name").fill("Tailnet verification — 6 mm sample");
  await page.locator("#file").setInputFiles("/tmp/mib-tailnet-review.dxf");
  await page.locator("#units").selectOption("um");
  await page.getByRole("button", { name: "Upload & inspect" }).click();
  for (let n = 0; n < 60 && !(await page.locator("#review").isVisible()); n++) {
    await page.waitForTimeout(1000);
    await page.getByRole("button", { name: "Refresh", exact: true }).click();
  }
  await page.locator("#confirmed").check();
  await page.locator("#generate").click();
  for (
    let n = 0;
    n < 120 && !(await page.locator("#downloads").isVisible());
    n++
  ) {
    await page.waitForTimeout(1000);
    await page.getByRole("button", { name: "Refresh", exact: true }).click();
  }
  await page.locator("#downloads").waitFor({ state: "visible" });
  const links = await page
    .locator("#links a")
    .evaluateAll((xs) => xs.map((x) => x.href));
  for (const link of links) {
    const r = await page.request.get(link);
    if (!r.ok() || (await r.body()).length < 100)
      throw new Error("Download failed");
  }
  await page.getByRole("button", { name: "Take a tour" }).click();
  await page.locator(".driver-popover-title").waitFor();
  await page.screenshot({
    path: "test-results/tailnet-live-review.png",
    fullPage: true,
  });
  console.log(
    JSON.stringify({ url, downloads: links.length, tour: true, realCad: true }),
  );
} finally {
  await browser.close();
}
