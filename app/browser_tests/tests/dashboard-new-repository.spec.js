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
  await page.route("**/dashboard/repos/new**", (route) =>
    route.fulfill({
      path: path.join(PUBLIC, "dashboard", "repos", "new", "index.html"),
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

test("an import this relay cannot host advances to a visible desktop handoff", async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 600 });
  await stubDashboard(page);
  await page.goto("/dashboard/repos");

  await page.getByRole("link", { name: "New repository" }).click();
  await expect(page).toHaveURL(/\/dashboard\/repos\/new$/);
  await expect(page.locator("[data-new-repo-submit-label]")).toHaveText(
    "Create repository",
  );
  await expect(page.locator("[data-new-repo-node-status]")).toContainText("Desktop or phone app");
  const cloneUrl = page.getByRole("button", { name: "Import from a URL" });
  await cloneUrl.click();
  await expect(cloneUrl).toHaveAttribute("aria-pressed", "true");
  await page.locator("[data-new-repo-name]").fill("test-repo-for-playwright");
  await page.locator("[data-new-repo-source-value]").fill(
    "https://github.com/octocat/Hello-World.git",
  );
  await page.getByRole("button", { name: "Import repository", exact: true }).click();

  await expect(page.locator("[data-new-repo-title]")).toHaveText(
    "Finish creating your repository",
  );
  await expect(page.locator("[data-new-repo-details]")).toBeHidden();
  const handoff = page.locator("[data-new-repo-steps]");
  await expect(handoff).toBeVisible();
  await expect(handoff).toContainText("Finish on your desktop node");
  await expect(handoff).toContainText("Nothing has been published yet");
  await expect(handoff).toContainText("test-repo-for-playwright");
  await expect(handoff).toContainText(
    "https://github.com/octocat/Hello-World.git",
  );
  await expect(page.locator("[data-new-repo-submit]")).toBeHidden();
  await expect(page.locator("[data-new-repo-cancel]")).toHaveText("Close");

  await page.locator("[data-new-repo-cancel]").click();
  await expect(page).toHaveURL(/\/dashboard\/repos$/);
  await page.getByRole("link", { name: "New repository" }).click();
  await expect(page.locator("[data-new-repo-title]")).toHaveText(
    "Create a new repository",
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

  await page.goto("/dashboard/repos/new");
  await expect(page.locator("[data-new-repo-submit-label]")).toHaveText(
    "Create repository",
  );
  await expect(page.locator("[data-new-repo-node-status]")).toContainText("Desktop or phone app");
  await page.getByRole("button", { name: "Import from a URL" }).click();
  await page.locator("[data-new-repo-import-code]").uncheck();
  await page.locator("[data-new-repo-import-issues]").check();
  await expect(page.locator("[data-new-repo-name]")).toBeHidden();
  await page.locator("[data-new-repo-source-value]").fill("https://github.com/acme/widgets");
  await page.locator("[data-new-repo-provider-token]").fill(token);
  await page.getByRole("button", { name: "Import repository" }).click();

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
// tests can only pin that the link exists; this drives both real documents to
// prove a click actually lands on the create page.
test("home rail New reaches the new-repository flow", async ({ page }) => {
  await page.addInitScript((session) => {
    localStorage.setItem("forkmesh.session", JSON.stringify(session));
  }, SESSION);
  await stubDashboard(page);
  await page.goto("/dashboard");

  const control = page.getByRole("link", { name: "New", exact: true });
  await expect(control).toBeVisible();
  await control.click();

  // It leaves home for the create page, with the form ready on arrival rather
  // than needing a second click.
  await expect(page).toHaveURL(/\/dashboard\/repos\/new$/);
  await expect(page.locator("[data-new-repo-page]")).toBeVisible();
  await expect(page.locator("[data-new-repo-title]")).toHaveText(
    "Create a new repository",
  );
  await expect(page.locator("[data-new-repo-submit-label]")).toHaveText(
    "Create repository",
  );
  await expect(page.locator("[data-new-repo-node-status]")).toContainText("Desktop or phone app");

  await page.reload();
  await expect(page.locator("[data-new-repo-details]")).toBeVisible();

  await page.goto("/dashboard/repos?new=1");
  await expect(page).toHaveURL(/\/dashboard\/repos\/new$/);
  await expect(page.locator("[data-new-repo-details]")).toBeVisible();
});

test("the page asks where the repository lives and explains the servers", async ({ page }) => {
  await page.addInitScript((session) => {
    localStorage.setItem("forkmesh.session", JSON.stringify(session));
  }, SESSION);
  await stubDashboard(page, (request) => {
    const pathname = new URL(request.url()).pathname;
    if (pathname === "/api/hosted-repositories" && request.method() === "GET") {
      return {
        body: {
          ok: true, available: true, mode: "core", privateSupported: true,
          initSupported: true, importSupported: true, importHosts: ["github.com"],
          gitignoreTemplates: [], licenseTemplates: [], coreNodeCount: 1,
          limits: { repositoriesPerAccount: 10, repositoryBytes: 1073741824 },
          used: 1, organizations: [],
        },
      };
    }
    if (pathname === "/api/hosted-repositories" && request.method() === "POST") {
      return {
        status: 201,
        body: {
          ok: true,
          repository: { owner: "alice", name: "demo", visibility: "public" },
          push: {
            commands: ["git clone https://example.test/alice/demo", "cd demo"],
            username: "alice", password: "fmp1.fixture", expiresAt: Date.now() + 7 * 864e5,
          },
          hosting: { mode: "core", nodes: ["core-1"], initialized: true },
        },
      };
    }
    return null;
  });
  await page.goto("/dashboard/repos/new");
  await expect(page.locator("[data-new-repo-node-status]")).toContainText("Desktop or phone app");

  const device = page.getByRole("radio", { name: /Your own device/ });
  const servers = page.getByRole("radio", { name: /ForkMesh servers/ });
  await expect(servers).toHaveAttribute("aria-checked", "true");
  await expect(page.locator("[data-new-repo-quota]")).toContainText("1 of 10 repositories used");
  await expect(page.locator("[data-new-repo-quota]")).toContainText("Each up to 1 GB");
  await expect(page.locator("[data-new-repo-copies]")).toContainText("keeps one copy");
  await expect(page.locator("[data-new-repo-hosted-meta]")).toContainText("1 server");

  await device.click();
  await expect(device).toHaveAttribute("aria-checked", "true");
  const guide = page.locator("[data-new-repo-node-guide]");
  await expect(guide).toBeVisible();
  await expect(guide).toContainText("Import folder…");
  await expect(guide).toContainText("Import from Files");
  await expect(page.locator("[data-new-repo-hosted-form]")).toBeHidden();
  await expect(page.locator("[data-new-repo-submit]")).toBeHidden();

  await servers.click();
  await expect(page.locator("[data-new-repo-submit]")).toBeVisible();
  await page.locator("[data-new-repo-name]").fill("bad name");
  await expect(page.locator("[data-new-repo-name-check]")).toContainText("Use letters, numbers");
  await page.locator("[data-new-repo-name]").fill("demo");
  await expect(page.locator("[data-new-repo-name-check]")).toHaveText("");
  await page.getByRole("button", { name: "Create repository", exact: true }).click();

  const result = page.locator("[data-new-repo-push]");
  await expect(result).toBeVisible();
  await expect(page.locator("[data-new-repo-push-where]")).toHaveText(
    "Stored on 1 server of ForkMesh, with a first commit.");
  await expect(page.locator("[data-new-repo-push-copies]")).toContainText("keeps one copy");
  await expect(result).toContainText("Shown only once");
  await expect(result.getByRole("link", { name: /Push code over HTTPS/ })).toHaveAttribute(
    "href", "/dashboard/settings/ssh-keys");
  await expect(page.locator("[data-new-repo-push-copy-field]")).toHaveCount(2);
});
