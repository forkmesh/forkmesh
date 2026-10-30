// Dialogs no page shows on load are not in the page documents: dashboard.js
// adds their markup the first time one is opened (dashboard_bundle.py
// DEFERRED_MARKUP, 02-helpers.js ensureDeferredMarkup).
const { test, expect } = require("@playwright/test");
const path = require("node:path");

const PUBLIC = path.resolve(__dirname, "..", "..", "public");
const WWW_PUBLIC = path.resolve(__dirname, "..", "..", "..", "www", "public");

async function preparePage(page) {
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alice",
      kind: "user",
      email: "alice@example.test",
      emailVerified: true,
      sessionToken: "test-session",
      status: "active",
    }));
  });
  await page.route("**/assets/vendor/lucide-0.468.0.min.js", (route) => route.fulfill({
    path: path.join(WWW_PUBLIC, "assets", "vendor", "lucide-0.468.0.min.js"),
    contentType: "text/javascript",
  }));
  await page.route("**/api-client.js*", (route) => route.fulfill({
    path: path.join(WWW_PUBLIC, "api-client.js"),
    contentType: "text/javascript",
  }));
  await page.route(/\/dashboard\/links(?:\?.*)?$/, (route) => route.fulfill({
    path: path.join(PUBLIC, "dashboard", "links", "index.html"),
    contentType: "text/html; charset=utf-8",
  }));
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    let body = { ok: true };
    if (url.pathname === "/api/notifications") {
      body = { ok: true, notifications: [{
        id: "n1", kind: "mention", title: "Bob mentioned you", body: "Look at this.",
        createdAt: Date.now() - 60_000, readAt: 0,
      }] };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
}

test("the page document leaves the dialogs out until one is opened", async ({ page }) => {
  await preparePage(page);
  await page.goto("/dashboard/links");
  await expect(page.locator("#agentModalToggle")).toBeAttached();
  await page.waitForFunction(() => typeof window.lucide !== "undefined");
  for (const selector of ["#agentModal", "#notificationModal", "[data-feedback-modal]",
    "[data-profile-about-modal]", "[data-agent-login-modal]"]) {
    await expect(page.locator(selector)).toHaveCount(0);
  }

  const viewport = page.viewportSize();
  if (viewport.width < 1024) await page.goto("/dashboard/links?feedback=1");
  else {
    await page.locator("#accountMenuToggle").click();
    await page.locator("#accountMenuDropdown [data-feedback-open]").click();
  }
  const feedback = page.locator("[data-feedback-modal]");
  await expect(feedback).toBeVisible();
  await expect(feedback).toHaveAttribute("role", "dialog");
  await expect(feedback).toHaveAttribute("aria-modal", "true");
  await expect(page.locator("[data-feedback-message]")).toBeFocused();
  // The shared dialogs came in together, closed; icons were drawn.
  await expect(page.locator("#notificationModal")).toBeHidden();
  await expect(page.locator("[data-feedback-modal] svg.lucide").first()).toBeAttached();
  await page.keyboard.press("Escape");
  await expect(feedback).toBeHidden();

  // A second open reuses the same markup.
  await page.goto("/dashboard/links?feedback=1");
  await expect(page.locator("[data-feedback-modal]")).toBeVisible();
  await page.keyboard.press("Escape");
  if (viewport.width >= 1024) {
    await page.locator("#accountMenuToggle").click();
    await page.locator("#accountMenuDropdown [data-feedback-open]").click();
    await expect(page.locator("[data-feedback-modal]")).toHaveCount(1);
    await expect(page.locator("[data-feedback-modal]")).toBeVisible();
  }
});

test("the agent popover is added under its toggle on first open", async ({ page }) => {
  await preparePage(page);
  await page.goto("/dashboard/links");
  const toggle = page.locator("#agentModalToggle");
  await expect(toggle).toBeVisible();
  await page.waitForFunction(() => typeof window.lucide !== "undefined");
  await expect(page.locator("#agentModal")).toHaveCount(0);
  await toggle.click();
  const popover = page.locator("#agentModal");
  await expect(popover).toBeVisible();
  expect(await page.evaluate(() =>
    document.getElementById("agentModalToggle").nextElementSibling?.id)).toBe("agentModal");
  await expect(popover.locator("[data-repo-agent-new-input]")).toBeFocused();
  await toggle.click();
  await expect(popover).toBeHidden();
  await toggle.click();
  await expect(popover).toBeVisible();
  await expect(page.locator("#agentModal")).toHaveCount(1);
});

test("the pings dialog opens from the bell", async ({ page }) => {
  await preparePage(page);
  await page.goto("/dashboard/links");
  await expect(page.locator("#notificationToggle")).toBeVisible();
  await page.locator("#notificationToggle").click();
  await page.locator("[data-show-all-notifications]").click();
  const modal = page.locator("#notificationModal");
  await expect(modal).toBeVisible();
  await expect(modal.locator("[data-notification-modal-list]")).toBeAttached();
  await page.locator("[data-close-notification-modal]").click();
  await expect(modal).toBeHidden();
});
