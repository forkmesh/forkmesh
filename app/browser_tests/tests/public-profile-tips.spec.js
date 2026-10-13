const { test, expect } = require("@playwright/test");
const path = require("node:path");

const REPOSITORY = path.resolve(__dirname, "..", "..", "..");
const PROFILE_PAGE_PATH = path.resolve(
  __dirname, "..", "..", "public", "dashboard", "profile", "index.html");
const BUNDLE_PATH = path.resolve(
  __dirname, "..", "..", "public", "dashboard.js");
// The generator App stages onto its own origin (build_site_assets.py). Serving
// it from the source file keeps this spec honest about which code drew the QR.
const QR_PATH = path.join(REPOSITORY, "world", "public", "qr.js");

const TIP_ADDRESS = "9WzDXwBbmkg8ZTbNMqUxvQRAyrZzDsGYdLVL9zYtAWWM";

// A signed-out visitor reading someone else's /@name: no session in storage,
// and every /api/** answer comes from the public account payload built here.
async function visitPublicProfile(page, account) {
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.tailwind = {};" }),
  );
  await page.route(
    "https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js",
    (route) => route.fulfill({
      contentType: "text/javascript",
      body: "window.lucide = { createIcons() {} };",
    }),
  );
  await page.route(`**/@${account.name}`, (route) => route.fulfill({
    path: PROFILE_PAGE_PATH,
    contentType: "text/html; charset=utf-8",
  }));
  await page.route("**/dashboard.js*", (route) => route.fulfill({
    path: BUNDLE_PATH,
    contentType: "text/javascript; charset=utf-8",
  }));
  await page.route("**/qr.js", (route) => route.fulfill({
    path: QR_PATH,
    contentType: "text/javascript; charset=utf-8",
  }));
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body.repositories = [];
    } else if (url.pathname === `/api/accounts/${account.name}`) {
      body = { ok: true, exists: true, status: "active", ...account };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
  await page.goto(`/@${account.name}`);
}

test("a published tip address shows as a scannable code and copyable text", async ({ page }) => {
  await visitPublicProfile(page, {
    name: "cenzo",
    kind: "user",
    solana: TIP_ADDRESS,
    hasPayoutAddress: true,
  });

  const card = page.locator("[data-profile-sidebar-slot] [data-profile-tips]").first();
  await expect(card).toBeVisible();
  await expect(card.locator("[data-profile-tips-address]")).toHaveText(TIP_ADDRESS);
  await expect(card.locator("[data-profile-tips-empty]")).toBeHidden();

  // The QR is drawn, not stubbed: assert a real canvas of plausible size.
  const canvas = card.locator("[data-profile-tips-qr] canvas");
  await expect(canvas).toBeVisible();
  const box = await canvas.boundingBox();
  expect(box.width).toBeGreaterThan(80);
  expect(box.width).toEqual(box.height);
});

test("a profile with no tip address says so instead of showing an empty card", async ({ page }) => {
  await visitPublicProfile(page, {
    name: "cenzo",
    kind: "user",
    solana: "",
    hasPayoutAddress: false,
  });

  const card = page.locator("[data-profile-sidebar-slot] [data-profile-tips]").first();
  await expect(card).toBeVisible();
  await expect(card.locator("[data-profile-tips-empty]")).toHaveText(
    "@cenzo has not entered a Solana address for tips.");
  await expect(card.locator("[data-profile-tips-detail]")).toBeHidden();
  await expect(card.locator("[data-profile-tips-qr] canvas")).toHaveCount(0);
});
