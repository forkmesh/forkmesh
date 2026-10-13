const { test, expect } = require("@playwright/test");
const path = require("node:path");
const { execFileSync } = require("node:child_process");

const APP = path.resolve(__dirname, "..", "..");
const REPO_PAGE_PATH = path.join(APP, "public", "dashboard", "repo.html");
const DASHBOARD_JS_PATH = path.join(APP, "public", "dashboard.js");
const DASHBOARD_CSS_PATH = path.join(APP, "public", "dashboard", "tailwind.css");
const SHOTS = process.env.FORKMESH_WEB_KIOSK_SHOTS || "";

const CATALOG = JSON.parse(execFileSync("python3", [
  "-c",
  "import json,sys; sys.path.insert(0, sys.argv[1]); import repository_provider_mirror as m; print(json.dumps(m.catalog()))",
  path.join(APP, "src"),
]).toString());

function mirrorView(saved) {
  return {
    provider: saved.provider,
    providerLabel: saved.provider === "gitlab" ? "GitLab" : "GitHub",
    remote: saved.remote,
    remoteUrl: `https://${saved.provider}.com/${saved.remote}`,
    tokenHint: "…1234",
    active: true,
    prune: Boolean(saved.prune),
    resources: saved.resources,
    status: {
      login: "octo",
      verifiedAt: 1,
      remoteExists: true,
      remotePrivate: false,
      canPush: true,
      error: "",
      codePush: { at: Date.now() - 120000, ok: true, refs: 3, error: "" },
      lastSync: { at: Date.now() - 60000, ok: true, imported: 2, comments: 5, error: "" },
    },
    history: [
      { at: Date.now() - 60000, kind: "sync", direction: "pull", ok: true, message: "Imported 2 item(s) and 5 comment(s)" },
      { at: Date.now() - 120000, kind: "code", direction: "push", ok: true, message: "Pushed 3 ref(s)" },
    ],
  };
}

async function mountRepo(page, calls) {
  let saved = null;
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.dashboard.theme", "light");
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alice",
      account: "alice",
      sessionToken: "test-session",
    }));
  });
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.tailwind = {};" }));
  await page.route("https://cdn.jsdelivr.net/npm/lucide@*/**", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.lucide = { createIcons() {} };" }));
  await page.route("**/alice/project**", (route) =>
    route.fulfill({ path: REPO_PAGE_PATH, contentType: "text/html; charset=utf-8" }));
  await page.route("**/dashboard.js*", (route) =>
    route.fulfill({ path: DASHBOARD_JS_PATH, contentType: "text/javascript; charset=utf-8" }));
  await page.route("**/dashboard/tailwind.css", (route) =>
    route.fulfill({ path: DASHBOARD_CSS_PATH, contentType: "text/css; charset=utf-8" }));
  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body = {
        ok: true,
        repositories: [{
          owner: "alice",
          name: "project",
          visibility: "public",
          source: "local-node",
          cloneUrl: "https://forkmesh.test/alice/project",
          description: "Mirrored project",
          defaultBranch: "main",
          branches: ["main"],
          rootCommit: "0123456789abcdef0123456789abcdef01234567",
          updatedAt: "2",
        }],
      };
    } else if (url.pathname === "/api/repo/alice/project/tree") {
      body = { ok: true, commit: "0123456789abcdef0123456789abcdef01234567", entries: [], counts: { commits: 1, branches: 1 } };
    } else if (url.pathname === "/api/repo/alice/project/branches") {
      body = { ok: true, branches: [{ name: "main" }] };
    } else if (url.pathname === "/api/repo/alice/project/mirrors") {
      body = { ok: true, mirrors: [] };
    } else if (url.pathname === "/api/repo/alice/project/blobs") {
      body = { ok: true, blobs: {} };
    } else if (url.pathname === "/api/repo/alice/project/webhooks") {
      body = { ok: true, webhooks: [], events: [], deliveries: [] };
    } else if (url.pathname === "/api/repo/alice/project/provider-mirror") {
      const payload = request.postDataJSON();
      calls.push(payload);
      const base = { ok: true, catalog: CATALOG, codeHosted: true, private: false, pending: 0, counts: {} };
      if (payload.action === "save") {
        saved = payload;
        body = { ...base, mirror: mirrorView(saved), saved: true, created: true };
      } else if (payload.action === "candidates") {
        body = {
          ok: true,
          kind: payload.kind,
          page: 1,
          more: false,
          items: [
            { kind: payload.kind, number: 12, key: "12", title: "Speed up the cache", author: "erin", state: "open", url: "https://github.com/octo/project/pull/12", updatedAt: Date.now() - 3600000, labels: [], commentCount: 3, excerpt: "Halves cold start.", imported: false, mirrored: false, head: "erin:cache", base: "main", merged: false },
            { kind: payload.kind, number: 11, key: "11", title: "Fix typo in README", author: "dan", state: "closed", url: "https://github.com/octo/project/pull/11", updatedAt: Date.now() - 86400000, labels: [], commentCount: 0, excerpt: "", imported: true, mirrored: false, head: "dan:typo", base: "main", merged: true },
          ],
        };
      } else if (payload.action === "import") {
        body = { ...base, mirror: mirrorView(saved), results: { 12: "imported" }, imported: 1 };
      } else {
        body = { ...base, mirror: saved ? mirrorView(saved) : null };
      }
    }
    await route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
}

test("the code page mirror button opens settings where the owner connects GitHub and picks imports", async ({ page }) => {
  const calls = [];
  await page.setViewportSize({ width: 1440, height: 1000 });
  await mountRepo(page, calls);
  await page.goto("/alice/project");

  const button = page.locator("[data-repo-root-toolbar] [data-repo-forge-mirror-open]");
  await expect(button).toBeVisible();
  await expect(button).toContainText("Mirror");
  if (SHOTS) await page.locator("[data-repo-root-toolbar]").screenshot({ path: path.join(SHOTS, "mirror-toolbar.png") });

  await button.click();
  const card = page.locator("[data-repo-forge-mirror-card]");
  await expect(card).toBeVisible();
  await expect(card.locator("[data-repo-forge-mirror-form]")).toBeVisible();
  await expect(card.locator("[data-repo-forge-mirror-summary]")).toContainText("Not connected");
  const discussions = card.locator('[data-repo-forge-mirror-resource="discussions"] [data-repo-forge-mirror-direction]');
  await expect(discussions).toBeEnabled();
  if (SHOTS) await card.screenshot({ path: path.join(SHOTS, "mirror-connect.png") });

  await card.locator('[data-repo-forge-mirror-provider="gitlab"]').click();
  await expect(card.locator('[data-repo-forge-mirror-resource="discussions"] [data-repo-forge-mirror-direction]')).toBeDisabled();
  await expect(card.locator('[data-repo-forge-mirror-resource="pulls"]')).toContainText("Merge requests");
  await card.locator('[data-repo-forge-mirror-provider="github"]').click();

  await card.locator("[data-repo-forge-mirror-remote]").fill("octo/project");
  await card.locator("[data-repo-forge-mirror-token]").fill("github_pat_1234");
  const issues = card.locator('[data-repo-forge-mirror-resource="issues"]');
  await issues.locator("[data-repo-forge-mirror-direction]").selectOption("both");
  const pulls = card.locator('[data-repo-forge-mirror-resource="pulls"]');
  await expect(pulls.locator("[data-repo-forge-mirror-mode]")).toBeDisabled();
  await pulls.locator("[data-repo-forge-mirror-direction]").selectOption("pull");
  await expect(pulls.locator("[data-repo-forge-mirror-mode]")).toBeEnabled();
  await pulls.locator("[data-repo-forge-mirror-mode]").selectOption("manual");
  await card.locator('button[type="submit"]').click();

  await expect(card.locator("[data-repo-forge-mirror-status]")).toContainText("Created octo/project and connected.");
  const save = calls.find((call) => call.action === "save");
  expect(save).toMatchObject({
    provider: "github",
    remote: "octo/project",
    token: "github_pat_1234",
    createRemote: true,
    sessionToken: "test-session",
  });
  expect(save.resources.code.direction).toBe("push");
  expect(save.resources.issues).toMatchObject({ direction: "both", mode: "auto", comments: true });
  expect(save.resources.pulls).toMatchObject({ direction: "pull", mode: "manual" });
  await expect(card.locator("[data-repo-forge-mirror-summary]")).toContainText("GitHub · octo/project");
  await expect(card).toContainText("Pushed 3 branches and tags");
  if (SHOTS) await card.screenshot({ path: path.join(SHOTS, "mirror-overview.png") });

  await card.locator('[data-repo-forge-mirror-pick="pulls"]').click();
  const picker = card.locator("[data-repo-forge-mirror-picker]");
  await expect(picker).toContainText("Speed up the cache");
  await expect(picker.locator('[data-repo-forge-mirror-pick-item="11"]')).toBeDisabled();
  await picker.locator('[data-repo-forge-mirror-pick-item="12"]').check();
  if (SHOTS) await card.screenshot({ path: path.join(SHOTS, "mirror-picker.png") });
  await picker.locator("[data-repo-forge-mirror-import]").click();
  await expect(card.locator("[data-repo-forge-mirror-status]")).toContainText("Imported 1 item.");
  const imported = calls.find((call) => call.action === "import");
  expect(imported).toMatchObject({ kind: "pulls", keys: ["12"] });
});

test("the mirror button is hidden from visitors", async ({ page }) => {
  const calls = [];
  await mountRepo(page, calls);
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "mallory",
      account: "mallory",
      sessionToken: "other",
    }));
  });
  await page.goto("/alice/project");
  await expect(page.locator("[data-repo-root-toolbar]")).toBeVisible();
  await expect(page.locator("[data-repo-forge-mirror-open]")).toHaveCount(0);
  expect(calls).toHaveLength(0);
});
