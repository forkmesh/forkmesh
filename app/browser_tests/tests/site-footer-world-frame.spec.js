const { test, expect } = require("@playwright/test");
const fs = require("node:fs");
const path = require("node:path");


const ROOT = path.resolve(__dirname, "..", "..", "..");
const FOOTER = fs.readFileSync(
  path.join(ROOT, "www", "public", "site-footer.js"),
  "utf8",
);
const HEADERS = fs.readFileSync(
  path.join(ROOT, "www", "public", "_headers"),
  "utf8",
);


function headerRule(routePath) {
  const marker = `${routePath}\n`;
  const start = HEADERS.indexOf(marker);
  expect(start).toBeGreaterThanOrEqual(0);
  return HEADERS.slice(start + marker.length).split("\n\n", 1)[0];
}


test("the official site origins can embed World and an unlisted origin cannot", async ({ browser }) => {
  const rule = headerRule("/world/*");
  const csp = rule
    .split("\n")
    .map((line) => line.trim())
    .find((line) => line.startsWith("Content-Security-Policy: "))
    .slice("Content-Security-Policy: ".length);
  expect(csp.split(";").map((value) => value.trim())).toContain(
    "frame-ancestors 'self' https://forkmesh.com https://www.forkmesh.com",
  );
  expect(rule).toContain("! X-Frame-Options");
  expect(rule).not.toContain("X-Frame-Options: SAMEORIGIN");

  const context = await browser.newContext();
  await context.route("https://world.forkmesh.com/world/", (route) =>
    route.fulfill({
      status: 200,
      headers: {
        "content-type": "text/html; charset=utf-8",
        "content-security-policy": csp,
      },
      body: "<!doctype html><title>ForkMesh World</title><forkmesh-world></forkmesh-world>",
    }),
  );

  for (const origin of [
    "https://forkmesh.com",
    "https://www.forkmesh.com",
    "https://app.forkmesh.com",
  ]) {
    const page = await context.newPage();
    const consoleErrors = [];
    const failures = [];
    page.on("console", (message) => {
      if (message.type() === "error") consoleErrors.push(message.text());
    });
    page.on("requestfailed", (request) => {
      failures.push([request.url(), request.failure()?.errorText]);
    });
    await page.route(`${origin}/onboarding`, (route) =>
      route.fulfill({
        status: 200,
        contentType: "text/html; charset=utf-8",
        body: '<!doctype html><div data-forkmesh-world></div><script src="/site-footer.js"></script>',
      }),
    );
    await page.route(`${origin}/site-footer.js`, (route) =>
      route.fulfill({
        status: 200,
        contentType: "text/javascript; charset=utf-8",
        body: FOOTER,
      }),
    );

    await page.goto(`${origin}/onboarding`);
    await page.waitForTimeout(250);
    const worldFrame = page.frames().find(
      (frame) => frame.url() === "https://world.forkmesh.com/world/",
    );
    if (origin === "https://app.forkmesh.com") {
      await expect(page.locator("iframe.site-footer-world-frame")).toHaveCount(0);
      expect(worldFrame).toBeUndefined();
      expect(failures).toEqual([]);
      expect(consoleErrors).toEqual([]);
    } else {
      await expect(page.locator("iframe.site-footer-world-frame")).toHaveAttribute(
        "src",
        "https://world.forkmesh.com/world/",
      );
      expect(worldFrame).toBeDefined();
      await expect(worldFrame.locator("forkmesh-world")).toHaveCount(1);
      expect(failures).toEqual([]);
      expect(consoleErrors).toEqual([]);
    }
    await page.close();
  }

  const unlisted = await context.newPage();
  const failures = [];
  unlisted.on("requestfailed", (request) => {
    failures.push([request.url(), request.failure()?.errorText]);
  });
  await unlisted.route("https://unlisted.example/", (route) =>
    route.fulfill({
      status: 200,
      contentType: "text/html; charset=utf-8",
      body: '<!doctype html><iframe src="https://world.forkmesh.com/world/"></iframe>',
    }),
  );
  await unlisted.goto("https://unlisted.example/");
  await expect.poll(() => failures).toContainEqual([
    "https://world.forkmesh.com/world/",
    "net::ERR_BLOCKED_BY_RESPONSE",
  ]);
  expect(unlisted.frames().some(
    (frame) => frame.url() === "https://world.forkmesh.com/world/",
  )).toBe(false);
  await unlisted.close();
  await context.close();
});
