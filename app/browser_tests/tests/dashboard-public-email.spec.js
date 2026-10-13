const { test, expect } = require("@playwright/test");
const path = require("node:path");

const SETTINGS_PAGE_PATH = path.resolve(
  __dirname,
  "..",
  "..",
  "public",
  "dashboard",
  "settings",
  "index.html",
);
const PROFILE_PAGE_PATH = path.resolve(
  __dirname,
  "..",
  "..",
  "public",
  "dashboard",
  "profile",
  "index.html",
);

// One stub per test: the settings/profile documents are static assets, and every
// /api/** call the page makes answers from the account payload built here.
async function stubDashboard(page, { account, page: pagePath, onSave }) {
  await page.addInitScript((session) => {
    localStorage.setItem("forkmesh.session", JSON.stringify(session));
  }, { ...account, sessionToken: "test-session", status: "active" });
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
  await page.route("**/dashboard/settings", (route) => route.fulfill({
    path: SETTINGS_PAGE_PATH,
    contentType: "text/html; charset=utf-8",
  }));
  await page.route("**/dashboard/profile", (route) => route.fulfill({
    path: PROFILE_PAGE_PATH,
    contentType: "text/html; charset=utf-8",
  }));
  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body.repositories = [];
    } else if (url.pathname === "/api/accounts/profile" && request.method() === "POST") {
      const payload = request.postDataJSON();
      onSave?.(payload);
      body = { ok: true, ...account, sessionToken: "test-session", status: "active" };
    } else if (url.pathname === `/api/accounts/${account.nodeName}`) {
      body = { ok: true, name: account.nodeName, ...account };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
  await page.goto(pagePath);
}

test("a verified address unlocks the Public email dropdown and saves", async ({ page }) => {
  let saved = null;
  await stubDashboard(page, {
    page: "/dashboard/settings",
    onSave: (payload) => { saved = payload; },
    account: {
      nodeName: "alice",
      email: "alice@example.test",
      emailVerified: true,
      publicEmail: "",
    },
  });

  const select = page.locator("[data-profile-public-email]");
  await expect(select).toBeEnabled();
  await expect(select).toHaveValue("");
  await expect(select.locator("option")).toHaveText([
    "Don't show my email",
    "alice@example.test",
  ]);

  await select.selectOption("alice@example.test");
  await page.locator("[data-profile-public-save]").click();

  await expect(page.locator("[data-profile-public-hint]")).toHaveText(
    "Public profile saved.");
  expect(saved?.publicEmail).toBe("alice@example.test");
});

// A web account with an unconfirmed address is bounced to /verify-pending
// before it can reach settings, so the reachable "nothing to publish" state is
// a session carrying no address at all (a key-bound node).
test("a session with no address on file leaves the dropdown disabled", async ({ page }) => {
  await stubDashboard(page, {
    page: "/dashboard/settings",
    account: {
      nodeName: "bob",
      email: "",
      emailVerified: false,
      publicEmail: "",
    },
  });

  const select = page.locator("[data-profile-public-email]");
  await expect(select).toBeDisabled();
  await expect(select.locator("option")).toHaveText([
    "Select a verified email to display",
  ]);
});

test("a saved public email reopens on the stored choice and shows on the profile", async ({ page }) => {
  const account = {
    nodeName: "carol",
    email: "carol@example.test",
    emailVerified: true,
    publicEmail: "carol@example.test",
  };

  await stubDashboard(page, { page: "/dashboard/settings", account });
  await expect(page.locator("[data-profile-public-email]")).toHaveValue(
    "carol@example.test");

  await stubDashboard(page, { page: "/dashboard/profile", account });
  const row = page.locator("[data-profile-email-row]").first();
  await expect(row).toBeVisible();
  await expect(row.locator("[data-profile-email-text]")).toHaveText(
    "carol@example.test");
  await expect(row.locator("[data-profile-email-text]")).toHaveAttribute(
    "href", "mailto:carol@example.test");
});
