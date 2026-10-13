const { test, expect } = require("@playwright/test");
const path = require("node:path");

const PUBLIC = path.resolve(__dirname, "..", "..", "public");

const SESSION = {
  nodeName: "alice",
  email: "alice@example.test",
  emailVerified: true,
  sessionToken: "test-session",
  status: "active",
};

async function stubDashboard(page, apiHandler = null) {
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({
      contentType: "text/javascript",
      body: "window.tailwind = {};",
    }),
  );
  await page.route(
    "https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js",
    (route) =>
      route.fulfill({
        contentType: "text/javascript",
        body: "window.lucide = { createIcons() {} };",
      }),
  );
  // The trailing ** keeps the ?new=1 arrival (issue #626) on this document.
  await page.route("**/dashboard/repos**", (route) =>
    route.fulfill({
      path: path.join(PUBLIC, "dashboard", "repos", "index.html"),
      contentType: "text/html; charset=utf-8",
    }),
  );
  await page.route("**/dashboard", (route) =>
    route.fulfill({
      path: path.join(PUBLIC, "dashboard", "index.html"),
      contentType: "text/html; charset=utf-8",
    }),
  );
  await page.route("**/dashboard.js*", (route) =>
    route.fulfill({
      path: path.join(PUBLIC, "dashboard.js"),
      contentType: "text/javascript; charset=utf-8",
    }),
  );
  await page.route("**/dashboard/tailwind.css", (route) =>
    route.fulfill({
      path: path.join(PUBLIC, "dashboard", "tailwind.css"),
      contentType: "text/css; charset=utf-8",
    }),
  );
  await page.route("**/api/**", (route) => {
    const custom = apiHandler?.(route.request());
    if (custom) {
      return Promise.resolve(custom).then(({ body, status = 200 }) => route.fulfill({
        status,
        contentType: "application/json; charset=utf-8",
        body: JSON.stringify(body),
      }));
    }
    const pathname = new URL(route.request().url()).pathname;
    const body = pathname === "/api/repositories"
      ? { ok: true, repositories: [] }
      : pathname === "/api/repository-imports"
        ? { ok: true, repositories: [] }
        : { ok: true };
    return route.fulfill({
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
}

test("Create & mirror advances to a visible desktop handoff", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 600 });
  await stubDashboard(page);
  await page.goto("/dashboard/repos");

  await page.getByRole("button", { name: "New repository" }).click();
  await page.locator("[data-new-repo-name]").fill("test-repo-for-playwright");
  await page.locator("[data-new-repo-source-value]").fill(
    "https://github.com/octocat/Hello-World.git",
  );
  await page.getByRole("button", { name: "Create & mirror", exact: true }).click();

  await expect(page.locator("[data-new-repo-title]")).toHaveText(
    "Finish creating your repository",
  );
  await expect(page.locator("[data-new-repo-details]")).toBeHidden();
  const handoff = page.locator("[data-new-repo-steps]");
  await expect(handoff).toBeVisible();
  await expect(handoff).toContainText("Desktop handoff required");
  await expect(handoff).toContainText("Nothing has been published yet");
  await expect(handoff).toContainText("test-repo-for-playwright");
  await expect(handoff).toContainText(
    "https://github.com/octocat/Hello-World.git",
  );
  await expect(page.locator("[data-new-repo-submit]")).toBeHidden();
  await expect(page.locator("[data-new-repo-cancel]")).toHaveText("Close");

  await page.locator("[data-new-repo-cancel]").click();
  await page.getByRole("button", { name: "New repository" }).click();
  await expect(page.locator("[data-new-repo-title]")).toHaveText(
    "Create & mirror a repository",
  );
  await expect(page.locator("[data-new-repo-details]")).toBeVisible();
  await expect(page.locator("[data-new-repo-steps]")).toBeHidden();
  await expect(page.locator("[data-new-repo-submit]")).toBeVisible();
  await expect(page.locator("[data-new-repo-cancel]")).toHaveText("Cancel");
});

test("provider setup starts a resumable transfer and clears its credential", async ({ page }) => {
  const token = "ghp_browser_fixture_not_persisted";
  const setupSamples = [];
  let resumeCalls = 0;
  let completed = false;
  const counts = {
    git_refs: 2,
    issues: 1,
    pull_requests: 1,
    reviews: 1,
    releases: 1,
    documentation: 1,
    labels: 1,
    users: 1,
    collaboration_history: 1,
  };
  const migration = (status, phase, setupMs) => ({
    id: "mig_0123456789abcdef01234567",
    repositoryId: "ext_0123456789abcdef01234567",
    provider: "github",
    sourceUrl: "https://github.com/acme/widgets",
    isPrivate: false,
    status,
    phase,
    generation: 1,
    setup: { durationMs: setupMs, acceptedAt: Date.now(), measurement: "client_monotonic" },
    transfer: {
      startedAt: status === "pending" ? 0 : Date.now() - 250,
      completedAt: status === "completed" ? Date.now() : 0,
      durationMs: status === "completed" ? 250 : 0,
    },
    counts: status === "completed" ? counts : { ...counts, reviews: 0 },
    processed: status === "completed" ? 11 : 5,
    retryCount: 0,
    report: { unsupported: [], skipped: [], failures: [], redactedCredentialFields: 0 },
    state: status === "completed"
      ? { hash: "sha256:fixture", signature: "signed-fixture", algorithm: "hmac-sha256" }
      : null,
  });

  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alice",
      email: "alice@example.test",
      emailVerified: true,
      sessionToken: "test-session",
      status: "active",
    }));
  });
  await stubDashboard(page, (request) => {
    const url = new URL(request.url());
    const pathname = url.pathname;
    if (pathname.endsWith("/resume") && request.method() === "POST") {
      resumeCalls += 1;
      const body = request.postDataJSON();
      expect(body.providerToken).toBe(token);
      completed = resumeCalls >= 2;
      return {
        body: {
          ok: true,
          hasMore: !completed,
          migration: migration(completed ? "completed" : "running", completed ? "complete" : "reviews", setupSamples[0]),
        },
      };
    }
    if (pathname === "/api/repository-imports/migrations" && request.method() === "POST") {
      const body = request.postDataJSON();
      setupSamples.push(body.setupDurationMs);
      expect(body.providerToken).toBe(token);
      return {
        status: 202,
        body: {
          ok: true,
          accepted: true,
          repository: { id: "ext_0123456789abcdef01234567", statusLabel: "External repository" },
          migration: migration("pending", "git_refs", body.setupDurationMs),
        },
      };
    }
    if (pathname === "/api/repository-imports/migrations" && request.method() === "GET") {
      return { body: { ok: true, migrations: completed ? [migration("completed", "complete", setupSamples[0])] : [] } };
    }
    if (pathname === "/api/repository-imports") {
      return { body: { ok: true, repositories: [] } };
    }
    return null;
  });

  await page.goto("/dashboard/repos");
  await page.getByRole("button", { name: "New repository" }).click();
  await page.getByRole("button", { name: "GitHub / GitLab / Codeberg" }).click();
  // Before anything starts, the tab states what an import produces, so the
  // later receipt cannot be the first mention of it (issue #678).
  await expect(page.locator("[data-new-repo-subtitle]")).toContainText(
    "Mirroring the Git contents is a separate step");
  await expect(page.locator("[data-new-repo-source-hint]")).toContainText(
    "an external listing, not a ForkMesh mirror");
  await page.locator("[data-new-repo-source-value]").fill("https://github.com/acme/widgets");
  await page.locator("[data-new-repo-provider-token]").fill(token);
  await page.getByRole("button", { name: "Start resumable import" }).click();

  await expect(page.locator("[data-new-repo-hint]")).toContainText(
    "reached signed metadata completion");
  await expect(page.locator("[data-new-repo-setup-time]")).toContainText("Hands-on setup");
  await expect(page.locator("[data-new-repo-transfer-time]")).toContainText("Transfer");
  await expect(page.locator("[data-new-repo-migration-phase]")).toContainText("signed state");
  // A signed import copies provider records only; the Git contents are never
  // cloned by the browser, so the receipt must say that and must land on the
  // desktop steps that do produce a mirror (issue #678).
  await expect(page.locator("[data-new-repo-migration-phase]")).toContainText(
    "metadata only, no Git contents mirrored");
  await expect(page.locator("[data-new-repo-hint]")).toContainText(
    "No Git contents were mirrored");
  const mirrorSteps = page.locator("[data-new-repo-steps]");
  await expect(mirrorSteps).toBeVisible();
  await expect(mirrorSteps).toContainText("The Git contents are not mirrored");
  await expect(mirrorSteps).toContainText("https://github.com/acme/widgets");
  // The signed receipt stays on screen beside those steps.
  await expect(page.locator("[data-new-repo-details]")).toBeVisible();
  await expect(page.locator("[data-new-repo-provider-token]")).toHaveValue("");
  expect(resumeCalls).toBe(2);
  expect(setupSamples).toHaveLength(1);
  expect(setupSamples[0]).toBeGreaterThanOrEqual(0);
  expect(setupSamples[0]).toBeLessThanOrEqual(180_000);
  const storedSession = await page.evaluate(() => localStorage.getItem("forkmesh.session"));
  expect(storedSession).not.toContain(token);
});

// The home rail's "New" above Top repositories was inert markup: no data hook,
// no href, no listener, so clicking it did nothing (issue #626). Source-text
// tests can only pin that the link and the marker-consuming code exist; this
// drives both real documents to prove a click actually lands on the
// create-and-mirror flow.
test("home rail New reaches the new-repository flow", async ({ page }) => {
  await page.addInitScript((session) => {
    localStorage.setItem("forkmesh.session", JSON.stringify(session));
  }, SESSION);
  await stubDashboard(page);
  await page.goto("/dashboard");

  const control = page.getByRole("link", { name: "New", exact: true });
  await expect(control).toBeVisible();
  await control.click();

  // It leaves home for the document that carries the modal, and the modal is
  // open on arrival rather than needing a second click.
  const modal = page.locator("[data-new-repo-modal]");
  await expect(modal).toBeVisible();
  await expect(page.locator("[data-new-repo-title]")).toHaveText(
    "Create & mirror a repository",
  );
  await expect(page.locator("[data-new-repo-submit-label]")).toHaveText(
    "Create & mirror",
  );

  // The marker is consumed, not left in the address bar, so the URL a user
  // would bookmark or reload is the plain Repositories page.
  await expect(page).toHaveURL(/\/dashboard\/repos$/);
  await page.reload();
  await expect(page.getByRole("button", { name: "New repository" })).toBeVisible();
  await expect(modal).toBeHidden();
});
