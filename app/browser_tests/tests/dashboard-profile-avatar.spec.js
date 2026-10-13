const { test, expect } = require("@playwright/test");
const path = require("node:path");

const HOME_PAGE_PATH = path.resolve(
  __dirname,
  "..",
  "..",
  "public",
  "dashboard",
  "index.html",
);
const SETTINGS_PAGE_PATH = path.resolve(
  __dirname,
  "..",
  "..",
  "public",
  "dashboard",
  "settings",
  "index.html",
);
const TINY_PNG = Buffer.from(
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=",
  "base64",
);

test("clicking the top-bar avatar opens account settings", async ({ page }) => {
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alice",
      email: "alice@example.test",
      emailVerified: true,
      sessionToken: "test-session",
      status: "active",
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
  await page.route("**/dashboard", (route) => route.fulfill({
    path: HOME_PAGE_PATH,
    contentType: "text/html; charset=utf-8",
  }));
  await page.route("**/dashboard/settings", (route) => route.fulfill({
    path: SETTINGS_PAGE_PATH,
    contentType: "text/html; charset=utf-8",
  }));
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    const body = url.pathname === "/api/repositories"
      ? { ok: true, repositories: [] }
      : url.pathname === "/api/notifications"
        ? { ok: true, notifications: [], unread: 0 }
        : url.pathname === "/api/accounts/alice"
          ? {
              ok: true,
              name: "alice",
              email: "alice@example.test",
              emailVerified: true,
            }
          : { ok: true };
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });

  await page.goto("/dashboard");
  const avatar = page.locator("[data-profile-settings-button]");
  await expect(avatar).toHaveAttribute("href", "/dashboard/settings");
  await avatar.click();
  await expect(page).toHaveURL(/\/dashboard\/settings$/);
});

test("profile Edit picks, crops, and saves an avatar", async ({ page }) => {
  let savedAvatar = "";
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alice",
      email: "alice@example.test",
      emailVerified: true,
      sessionToken: "test-session",
      status: "active",
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
  await page.route("**/dashboard/settings", (route) => route.fulfill({
    path: SETTINGS_PAGE_PATH,
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
      savedAvatar = String(payload.avatarPng || "");
      body = {
        ok: true,
        nodeName: "alice",
        email: "alice@example.test",
        emailVerified: true,
        sessionToken: "test-session",
        status: "active",
        avatarPng: savedAvatar.split(",", 2)[1] || "",
      };
    } else if (url.pathname === "/api/accounts/alice") {
      body = {
        ok: true,
        name: "alice",
        email: "alice@example.test",
        emailVerified: true,
      };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });

  await page.goto("/dashboard/settings");
  const chooserPromise = page.waitForEvent("filechooser");
  await page.getByRole("button", { name: "Edit" }).click();
  const chooser = await chooserPromise;
  await chooser.setFiles({ name: "avatar.png", mimeType: "image/png", buffer: TINY_PNG });

  const dialog = page.getByRole("dialog", { name: "Crop profile picture" });
  await expect(dialog).toBeVisible();
  const save = dialog.getByRole("button", { name: "Save profile picture" });
  await expect(save).toBeEnabled();
  await save.click();

  await expect(page.locator("[data-profile-avatar-hint]")).toHaveText("Profile picture saved.");
  expect(savedAvatar).toMatch(/^data:image\/png;base64,/);
  await expect(page.locator("[data-settings-profile-picture] img")).toHaveAttribute(
    "src",
    /^data:image\/png;base64,/,
  );
});
