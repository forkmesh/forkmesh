const { test, expect } = require("@playwright/test");
const path = require("node:path");

// The header's "Reconnect node" pill deep-links here. Serving the prebuilt
// settings document for the sub-tab path mirrors the Worker, which hands
// /dashboard/settings/<tab> the same asset (issue #640).
const PUBLIC = path.resolve(__dirname, "..", "..", "public");
const WWW_PUBLIC = path.resolve(__dirname, "..", "..", "..", "www", "public");
const SETTINGS_PAGE_PATH = path.join(PUBLIC, "dashboard", "settings", "index.html");

async function openNodesTab(page) {
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alice",
      email: "alice@example.test",
      emailVerified: true,
      sessionToken: "test-session",
      status: "active",
      nodes: [],
    }));
  });
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
  await page.route("**/dashboard/settings/nodes**", (route) => route.fulfill({
    path: SETTINGS_PAGE_PATH,
    contentType: "text/html; charset=utf-8",
  }));
  // Serve the bundle and stylesheet from this checkout rather than whatever a
  // reused :4179 server has staged, so the spec always exercises this build.
  await page.route("**/dashboard.js*", (route) => route.fulfill({
    path: path.join(PUBLIC, "dashboard.js"),
    contentType: "text/javascript; charset=utf-8",
  }));
  await page.route("**/dashboard/tailwind.css*", (route) => route.fulfill({
    path: path.join(PUBLIC, "dashboard", "tailwind.css"),
    contentType: "text/css; charset=utf-8",
  }));
  await page.route("**/api-client.js*", (route) => route.fulfill({
    path: path.join(WWW_PUBLIC, "api-client.js"),
    contentType: "text/javascript; charset=utf-8",
  }));
  await page.route("**/analytics.js*", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "" }));
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    const body = { ok: true, repositories: [] };
    // The link completes on claim-confirm, which answers with the account's
    // new node list.
    if (url.pathname === "/api/accounts/claim-confirm") body.nodes = ["mirror-01"];
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
  await page.goto("/dashboard/settings/nodes?from=reconnect");
}

test("reconnect arrival opens Nodes with guidance instead of a bare field", async ({ page }) => {
  await openNodesTab(page);

  // The tab the click asked for, not the public-profile default.
  await expect(page.locator("#settings-nodes")).toBeVisible();

  // Why you are here, and what the missing link costs.
  const callout = page.locator("[data-node-reconnect-callout]");
  await expect(callout).toBeVisible();
  await expect(callout).toContainText("No node is linked to this account yet");
  await expect(callout).toContainText("Reconnect node");

  // What a node ID is, in both accepted forms, with a full-length key example.
  const help = page.locator("#claim-node-id-help");
  await expect(help).toContainText("What is a node ID?");
  await expect(help).toContainText("mirror-01");
  await expect(help).toContainText("J2MJT8PgCi665wAlfuHdmpl-WDH2xB6z3Tp6T0OPsOs");
  await expect(help).toContainText("Copy node ID");

  // And a way out for someone who has no node at all.
  const webOnly = page.locator("[data-no-node-help]");
  await expect(webOnly).toContainText("No node yet?");
  await expect(webOnly).toContainText("curl -fsSL https://forkmesh.com/install.sh | bash");
  await expect(webOnly.locator('a[href="/desktop"]')).toHaveCount(1);
});

test("a mistyped node ID says which form was missed", async ({ page }) => {
  await openNodesTab(page);

  const status = page.locator("[data-claim-node-status]");
  await page.locator("[data-claim-node-input]").fill("My Laptop");
  await page.locator("[data-claim-node-send]").click();
  await expect(status).toContainText("A node name starts with a letter");
  await expect(status).toContainText("mirror-01");

  // A truncated public-key paste is answered as a key, not as a bad name.
  await page.locator("[data-claim-node-input]")
    .fill("J2MJT8PgCi665wAlfuHdmpl_WDH2xB6z3Tp6T0OPsO");
  await page.locator("[data-claim-node-send]").click();
  await expect(status).toContainText("exactly 43 characters, unpadded. This one is 42.");

  // As is a base64 paste that kept its padding.
  await page.locator("[data-claim-node-input]")
    .fill("J2MJT8PgCi665wAlfuHdmpl-WDH2xB6z3Tp6T0OPsOs=");
  await page.locator("[data-claim-node-send]").click();
  await expect(status).toContainText('Drop the trailing "=" padding.');
});

test("linking a node clears the reconnect callout", async ({ page }) => {
  await openNodesTab(page);
  await expect(page.locator("[data-node-reconnect-callout]")).toBeVisible();

  await page.locator("[data-claim-node-input]").fill("mirror-01");
  await page.locator("[data-claim-node-password]").fill("hunter2");
  await page.locator("[data-claim-node-send]").click();
  await expect(page.locator("[data-claim-code-row]")).toBeVisible();
  await page.locator("[data-claim-code-input]").fill("123456");
  await page.locator("[data-claim-code-confirm]").click();

  await expect(page.locator("[data-claim-node-status]"))
    .toContainText('Linked "mirror-01" to your account.');
  await expect(page.locator("[data-claim-node-list]")).toContainText("mirror-01");
  // The callout describes a problem that no longer exists, marker or not.
  await expect(page.locator("[data-node-reconnect-callout]")).toBeHidden();
});
