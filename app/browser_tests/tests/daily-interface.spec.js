const { test, expect } = require("@playwright/test");
const path = require("node:path");

const PUBLIC = path.resolve(__dirname, "..", "..", "public");
const REPOS_PAGE = path.join(PUBLIC, "dashboard", "repos", "index.html");
const REPO_PAGE = path.join(PUBLIC, "dashboard", "repo.html");
const DASHBOARD_JS = path.join(PUBLIC, "dashboard.js");
const DASHBOARD_CSS = path.join(PUBLIC, "dashboard", "tailwind.css");
const BUDGET_CONFIG = require(path.resolve(
  __dirname,
  "..",
  "..",
  "..",
  "www",
  "docs",
  "engineering",
  "daily-interface-budgets.json",
));

const REPOSITORIES = [
  { owner: "alpha", name: "engine", description: "Physics core" },
  { owner: "beta", name: "atlas", description: "Map tiles" },
  { owner: "gamma", name: "beacon", description: "Signal relay" },
].map((repo, index) => ({
  ...repo,
  visibility: "public",
  source: "remote-clone",
  cloneUrl: `https://forkmesh.test/${repo.owner}/${repo.name}`,
  rootCommit: `root-${index}`,
  stateHash: `state-${index}`,
  updatedAt: String(index + 1),
}));

const budgetRows = new Map(BUDGET_CONFIG.budgets.map((row) => [row.id, row]));

function maximumFor(metric, source = "remote") {
  const row = budgetRows.get(`daily-${metric === "browse" ? "repository-browse" : metric === "diff" ? "diff-render" : metric}`);
  return row[source === "local" ? "localMaximum" : "remoteMaximum"];
}

async function installDashboardRoutes(page, { network = { online: true }, repoPage = false } = {}) {
  let repositoryRequests = 0;
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
  await page.route("**/dashboard.js*", (route) => route.fulfill({
    path: DASHBOARD_JS,
    contentType: "text/javascript; charset=utf-8",
  }));
  await page.route("**/dashboard/tailwind.css", (route) => route.fulfill({
    path: DASHBOARD_CSS,
    contentType: "text/css; charset=utf-8",
  }));
  await page.route("**/dashboard/repos", (route) => route.fulfill({
    path: REPOS_PAGE,
    contentType: "text/html; charset=utf-8",
  }));
  if (repoPage) {
    await page.route("**/alpha/engine", (route) => route.fulfill({
      path: REPO_PAGE,
      contentType: "text/html; charset=utf-8",
    }));
  }
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    if (url.pathname === "/api/repositories") {
      repositoryRequests += 1;
      if (!network.online) {
        return route.fulfill({
          status: 503,
          contentType: "application/json; charset=utf-8",
          body: JSON.stringify({ ok: false, error: "temporarily_unavailable" }),
        });
      }
      return route.fulfill({
        status: 200,
        contentType: "application/json; charset=utf-8",
        body: JSON.stringify({ ok: true, repositories: REPOSITORIES }),
      });
    }
    let body = { ok: true };
    if (url.pathname === "/api/repo/alpha/engine/branches") {
      body.branches = [{ name: "main", commit: "0123456789abcdef" }];
    } else if (url.pathname === "/api/repo/alpha/engine/tree") {
      body = {
        ok: true,
        commit: "0123456789abcdef",
        entries: [{ name: "README.md", path: "README.md", type: "blob", message: "Docs" }],
        counts: {},
      };
    } else if (url.pathname === "/api/repo/alpha/engine/mirrors") {
      body = {
        ok: true,
        mirrors: [{ node: "alpha", status: "online", cloneAvailable: true }],
        summary: { mirrors: 1 },
      };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
  return () => repositoryRequests;
}

async function metric(page, name, source) {
  await expect.poll(async () => page.evaluate(({ name, source }) => {
    const rows = window.ForkMeshInterfaceMetrics || [];
    return rows.findLast((row) => row.name === name && (!source || row.source === source)) || null;
  }, { name, source })).not.toBeNull();
  return page.evaluate(({ name, source }) => {
    const rows = window.ForkMeshInterfaceMetrics || [];
    return rows.findLast((row) => row.name === name && (!source || row.source === source));
  }, { name, source });
}

async function auditPrimaryAccessibility(page) {
  return page.evaluate(() => {
    const violations = [];
    const visible = (element) => {
      const style = getComputedStyle(element);
      const rect = element.getBoundingClientRect();
      return style.display !== "none" && style.visibility !== "hidden"
        && rect.width > 0 && rect.height > 0;
    };
    const accessibleName = (element) => {
      const labelledBy = (element.getAttribute("aria-labelledby") || "")
        .split(/\s+/).filter(Boolean)
        .map((id) => document.getElementById(id)?.textContent || "").join(" ");
      const explicitLabel = element.id
        ? document.querySelector(`label[for="${CSS.escape(element.id)}"]`)?.textContent || ""
        : "";
      return [
        element.getAttribute("aria-label"),
        labelledBy,
        explicitLabel,
        element.textContent,
        element.getAttribute("title"),
      ].find((value) => String(value || "").trim()) || "";
    };

    const ids = Array.from(document.querySelectorAll("[id]"), (element) => element.id)
      .filter(Boolean);
    for (const id of new Set(ids)) {
      if (ids.filter((candidate) => candidate === id).length > 1) {
        violations.push({ severity: "serious", rule: "duplicate-id", target: id });
      }
    }
    const roots = document.querySelectorAll(
      "[data-app-header], [data-repo-panel], [data-command-palette]:not([hidden])",
    );
    for (const root of roots) {
      for (const element of root.querySelectorAll(
        "button, a[href], input:not([type='hidden']), select, textarea, [role='option']",
      )) {
        if (visible(element) && !accessibleName(element)) {
          violations.push({
            severity: "critical",
            rule: "interactive-name",
            target: element.outerHTML.slice(0, 120),
          });
        }
      }
      for (const image of root.querySelectorAll("img")) {
        if (visible(image) && !image.hasAttribute("alt")) {
          violations.push({
            severity: "serious",
            rule: "image-alt",
            target: image.getAttribute("src") || "img",
          });
        }
      }
    }
    if (document.querySelectorAll("main").length !== 1) {
      violations.push({ severity: "serious", rule: "single-main", target: "main" });
    }
    return violations;
  });
}

test("daily interface startup and repository browse stay within budget", async ({ page }) => {
  await installDashboardRoutes(page);
  await page.goto("/dashboard/repos");
  await expect(page.locator("#repoList .repo-card")).toHaveCount(REPOSITORIES.length);

  const startup = await metric(page, "startup", "remote");
  const browse = await metric(page, "browse", "remote");
  expect(startup.duration).toBeLessThanOrEqual(maximumFor("startup"));
  expect(browse.duration).toBeLessThanOrEqual(maximumFor("browse"));
  expect(startup.passed).toBe(true);
  expect(browse.passed).toBe(true);

  const search = page.locator("#repoSearch");
  await search.fill("atlas");
  await expect(page.locator("#repoList .repo-card")).toHaveCount(1);
  const searchMetric = await metric(page, "search", "local");
  expect(searchMetric.duration).toBeLessThanOrEqual(maximumFor("search", "local"));
});

test("daily interface cached startup and browse use the tighter local budget", async ({ page }) => {
  await installDashboardRoutes(page);
  await page.addInitScript((repositories) => {
    localStorage.setItem("forkmesh.repositoryCatalog.v1", JSON.stringify({
      savedAt: Date.now(),
      repositories,
    }));
  }, REPOSITORIES);
  await page.goto("/dashboard/repos");
  await expect(page.locator("#repoList .repo-card")).toHaveCount(REPOSITORIES.length);
  const startup = await metric(page, "startup", "local");
  const browse = await metric(page, "browse", "local");
  expect(startup.duration).toBeLessThanOrEqual(maximumFor("startup", "local"));
  expect(browse.duration).toBeLessThanOrEqual(maximumFor("browse", "local"));
});

test("daily interface command palette is keyboard complete and restores focus", async ({ page }) => {
  await installDashboardRoutes(page);
  await page.goto("/dashboard/repos");
  await expect(page.locator("#repoList .repo-card")).toHaveCount(REPOSITORIES.length);

  const filter = page.locator("#repoSearch");
  await filter.focus();
  await page.keyboard.press("Control+k");
  const dialog = page.getByRole("dialog", { name: "Command palette" });
  await expect(dialog).toBeVisible();
  const input = dialog.getByRole("combobox", { name: "Filter commands" });
  await expect(input).toBeFocused();
  await expect(input).toHaveAttribute("aria-expanded", "true");
  await expect(dialog.getByRole("option")).toHaveCount(8);
  const violations = await auditPrimaryAccessibility(page);
  expect(violations.filter((row) => row.severity === "critical")).toHaveLength(
    BUDGET_CONFIG.accessibility.criticalViolations,
  );
  expect(violations.filter((row) => row.severity === "serious")).toHaveLength(
    BUDGET_CONFIG.accessibility.seriousViolations,
  );

  await page.keyboard.press("ArrowDown");
  await expect(dialog.getByRole("option").nth(1)).toHaveAttribute("aria-selected", "true");
  await page.keyboard.press("Tab");
  await expect(input).toBeFocused();
  await input.fill("theme");
  await expect(dialog.getByRole("option")).toHaveCount(1);
  await page.keyboard.press("Escape");
  await expect(dialog).toBeHidden();
  await expect(filter).toBeFocused();

  const feedback = await metric(page, "feedback", "local");
  expect(feedback.duration).toBeLessThanOrEqual(maximumFor("feedback", "local"));
  const outline = await filter.evaluate((element) => {
    const style = getComputedStyle(element);
    return { width: style.outlineWidth, style: style.outlineStyle, color: style.outlineColor };
  });
  expect(Number.parseFloat(outline.width)).toBeGreaterThanOrEqual(1);
  expect(outline.style).toBe("solid");
  expect(outline.color).not.toMatch(/^rgba\(.*,\s*0\)$|^transparent$/);
});

test("daily interface skip navigation, contrast, zoom, and reduced motion are accessible", async ({ page }) => {
  await installDashboardRoutes(page);
  await page.emulateMedia({ reducedMotion: "reduce", colorScheme: "dark" });
  await page.goto("/dashboard/repos");
  await expect(page.locator("#repoList .repo-card")).toHaveCount(REPOSITORIES.length);

  await page.keyboard.press("Tab");
  const skip = page.getByRole("link", { name: "Skip to main content" });
  await expect(skip).toBeFocused();
  await page.keyboard.press("Enter");
  await expect(page.getByRole("main")).toBeFocused();

  const transition = await page.getByRole("link", { name: "New repository" })
    .evaluate((element) => getComputedStyle(element).transitionDuration);
  expect(transition.split(",").every((part) => Number.parseFloat(part) === 0)).toBe(true);

  const contrasts = await page.evaluate(() => {
    const parse = (value) => value.trim().split(/\s+/).map(Number);
    const luminance = (rgb) => {
      const values = rgb.map((part) => {
        const channel = part / 255;
        return channel <= 0.04045 ? channel / 12.92 : ((channel + 0.055) / 1.055) ** 2.4;
      });
      return 0.2126 * values[0] + 0.7152 * values[1] + 0.0722 * values[2];
    };
    return ["dark", "light"].map((theme) => {
      document.documentElement.dataset.dashboardTheme = theme;
      const style = getComputedStyle(document.documentElement);
      const foreground = luminance(parse(style.getPropertyValue("--dashboard-primary-foreground-rgb")));
      const background = luminance(parse(style.getPropertyValue("--dashboard-primary-rgb")));
      return (Math.max(foreground, background) + 0.05) / (Math.min(foreground, background) + 0.05);
    });
  });
  for (const contrast of contrasts) {
    expect(contrast).toBeGreaterThanOrEqual(BUDGET_CONFIG.accessibility.minimumTextContrast);
  }

  await page.setViewportSize({ width: 640, height: 720 });
  const reflowsAtTwoHundredPercent = await page.evaluate(() => {
    const main = document.querySelector("[data-dashboard-main]");
    return main.scrollWidth <= main.clientWidth + 1;
  });
  expect(reflowsAtTwoHundredPercent).toBe(true);
});

test.describe("daily interface mobile", () => {
  test.use({ viewport: { width: 390, height: 844 }, hasTouch: true, isMobile: true });

  test("daily interface coarse-pointer controls meet the touch budget", async ({ page }) => {
    await installDashboardRoutes(page);
    await page.goto("/dashboard/repos");
    await expect(page.locator("#repoList .repo-card")).toHaveCount(REPOSITORIES.length);
    const menu = page.getByRole("button", { name: "Open dashboard menu" });
    const size = await menu.evaluate((element) => {
      const rect = element.getBoundingClientRect();
      return { width: rect.width, height: rect.height };
    });
    expect(size.width).toBeGreaterThanOrEqual(BUDGET_CONFIG.accessibility.minimumCoarsePointerTargetCssPixels);
    expect(size.height).toBeGreaterThanOrEqual(BUDGET_CONFIG.accessibility.minimumCoarsePointerTargetCssPixels);

    await page.getByRole("button", { name: "Open command palette" }).tap();
    await expect(page.getByRole("dialog", { name: "Command palette" })).toBeVisible();
  });
});

test("daily interface offline state retries and recovers with announced feedback", async ({ page }) => {
  const network = { online: false };
  const requestCount = await installDashboardRoutes(page, { network });
  await page.goto("/dashboard/repos");
  const status = page.getByRole("status").filter({ hasText: "Repositories are unavailable" });
  await expect(status).toBeVisible();
  const failedRequests = requestCount();
  network.online = true;
  const retry = page.getByRole("button", { name: "Retry repository list" });
  await retry.click();
  await expect(page.locator("#repoList .repo-card")).toHaveCount(REPOSITORIES.length);
  expect(requestCount()).toBeGreaterThan(failedRequests);
  await expect(page.locator("[data-interface-status]")).toContainText("Retrying repository list");
  const feedback = await metric(page, "feedback", "local");
  expect(feedback.duration).toBeLessThanOrEqual(maximumFor("feedback", "local"));
});

test("daily interface clone commands are copy-ready and feedback is announced", async ({ page, context }) => {
  await context.grantPermissions(["clipboard-read", "clipboard-write"]);
  await installDashboardRoutes(page, { repoPage: true });
  await page.goto("/alpha/engine");
  const code = page.locator("[data-repo-code-button]").first();
  await expect(code).toBeVisible();
  await code.click();
  const command = page.locator("[data-repo-code-menu] code").filter({ hasText: "git clone" }).first();
  const origin = new URL(page.url()).origin;
  await expect(command).toContainText(`git clone ${origin}/alpha/engine`);
  await page.getByRole("button", { name: "Copy git clone command" }).click();
  await expect(page.locator("[data-interface-status]")).toContainText("Copy git clone command copied");
});
