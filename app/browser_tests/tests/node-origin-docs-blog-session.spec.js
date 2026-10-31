const { test, expect } = require("@playwright/test");
const fs = require("node:fs");
const path = require("node:path");


const ROOT = path.resolve(__dirname, "..", "..", "..");
const WWW = path.join(ROOT, "www", "public");
const DOCS = fs.readFileSync(path.join(WWW, "docs", "index.html"), "utf8");
const BLOG = fs.readFileSync(path.join(WWW, "blog.html"), "utf8");
const HEADER = fs.readFileSync(path.join(WWW, "site-header.js"), "utf8");
const API_CLIENT = fs.readFileSync(path.join(WWW, "api-client.js"), "utf8");
const DASHBOARD_SIDEBAR = fs.readFileSync(
  path.join(ROOT, "app", "public", "dashboard", "partials", "sidebar.html"),
  "utf8",
);

const NODE_ORIGIN = "https://pp.forkmesh.test";
const DASHBOARD = `<!doctype html>
  <title>ForkMesh dashboard</title>
  <main data-node-account>issam</main>
  ${DASHBOARD_SIDEBAR}`;


test("an authenticated node session survives Dashboard → Docs/Blog → App", async ({ browser }) => {
  const context = await browser.newContext();
  const sessionReads = [];
  await context.addCookies([{
    name: "forkmesh_account",
    value: "node-only-session",
    url: NODE_ORIGIN,
    httpOnly: true,
    secure: true,
    sameSite: "Strict",
  }]);
  await context.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      kind: "user",
      nodeName: "issam",
      email: "issam@example.test",
      sessionToken: "cookie",
    }));
  });
  await context.route(`${NODE_ORIGIN}/**`, async (route) => {
    const url = new URL(route.request().url());
    if (url.pathname === "/" || url.pathname === "/dashboard") {
      return route.fulfill({
        contentType: "text/html; charset=utf-8",
        body: DASHBOARD,
      });
    }
    if (url.pathname === "/docs" || url.pathname === "/docs/") {
      return route.fulfill({
        contentType: "text/html; charset=utf-8",
        body: DOCS,
      });
    }
    if (url.pathname === "/blog" || url.pathname === "/blog/") {
      return route.fulfill({
        contentType: "text/html; charset=utf-8",
        body: BLOG,
      });
    }
    if (url.pathname === "/site-header.js") {
      return route.fulfill({
        contentType: "text/javascript; charset=utf-8",
        body: HEADER,
      });
    }
    if (url.pathname === "/api-client.js") {
      return route.fulfill({
        contentType: "text/javascript; charset=utf-8",
        body: API_CLIENT,
      });
    }
    if (url.pathname === "/api/accounts/sessions") {
      sessionReads.push(route.request().headers().cookie || "");
      return route.fulfill({
        contentType: "application/json; charset=utf-8",
        body: JSON.stringify({ ok: true, account: { membership: null } }),
      });
    }
    if (url.pathname === "/api/version") {
      return route.fulfill({
        contentType: "application/json; charset=utf-8",
        body: JSON.stringify({ ok: true, version: "1.0.0" }),
      });
    }
    if (url.pathname === "/api/chat/activity") {
      return route.fulfill({
        contentType: "application/json; charset=utf-8",
        body: JSON.stringify({ ok: true, messageCount: 0, userCount: 0 }),
      });
    }
    if (url.pathname.endsWith(".js")) {
      return route.fulfill({ contentType: "text/javascript", body: "" });
    }
    if (url.pathname.endsWith(".css")) {
      return route.fulfill({ contentType: "text/css", body: "" });
    }
    return route.fulfill({ status: 200, body: "" });
  });

  const page = await context.newPage();
  await page.goto(`${NODE_ORIGIN}/dashboard`);
  await expect(page.locator("[data-node-account]")).toHaveText("issam");

  await page.locator('a[href="/docs"]').click();
  await expect(page).toHaveURL(`${NODE_ORIGIN}/docs`);
  await expect(page.locator(".fm-header-account-name")).toHaveText("issam");
  await expect(page.locator(".fm-header-login")).toHaveCount(0);
  await page.locator(".fm-header-app").click();
  await expect(page).toHaveURL(`${NODE_ORIGIN}/dashboard`);

  await page.locator('a[href="/blog"]').click();
  await expect(page).toHaveURL(`${NODE_ORIGIN}/blog`);
  await expect(page.locator(".fm-header-account-name")).toHaveText("issam");
  await expect(page.locator(".fm-header-login")).toHaveCount(0);
  await page.locator(".fm-header-app").click();
  await expect(page).toHaveURL(`${NODE_ORIGIN}/dashboard`);

  await expect.poll(() => sessionReads.length).toBe(1);
  expect(sessionReads).toEqual([
    "forkmesh_account=node-only-session",
  ]);
  await context.close();
});
