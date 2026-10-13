const { test, expect } = require("@playwright/test");
const path = require("node:path");

const PUBLIC = path.resolve(__dirname, "..", "..", "public");
const REPO_PAGE_PATH = path.join(PUBLIC, "dashboard", "repo.html");
const DASHBOARD_JS_PATH = path.join(PUBLIC, "dashboard.js");
const DASHBOARD_CSS_PATH = path.join(PUBLIC, "dashboard", "tailwind.css");

// The publisher buckets its commits into 52 windows ending at the moment the
// catalog row was advertised, so the labels are pinned to updatedAt, not to the
// day this test runs.
const ANCHOR_MS = Date.UTC(2026, 7, 17); // 2026-08-17T00:00:00Z
const ACTIVITY_WEEKS = Array.from({ length: 52 }, (_, index) => {
  if (index === 51) return 7;
  if (index === 50) return 1;
  if (index === 25) return 4;
  return 0;
});

// Deterministic date labels: the graph formats through toLocaleDateString.
test.use({ timezoneId: "UTC", locale: "en-US" });

async function openInsights(page) {
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
  await page.route("**/alice/proj/insights", (route) =>
    route.fulfill({
      path: REPO_PAGE_PATH,
      contentType: "text/html; charset=utf-8",
    }),
  );
  await page.route("**/dashboard.js*", (route) =>
    route.fulfill({
      path: DASHBOARD_JS_PATH,
      contentType: "text/javascript; charset=utf-8",
    }),
  );
  await page.route("**/dashboard/tailwind.css", (route) =>
    route.fulfill({
      path: DASHBOARD_CSS_PATH,
      contentType: "text/css; charset=utf-8",
    }),
  );
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body = {
        ok: true,
        repositories: [
          {
            owner: "alice",
            name: "proj",
            visibility: "public",
            source: "local-node",
            cloneUrl: "https://forkmesh.test/alice/proj",
            rootCommit: "root",
            commitCount: "12",
            updatedAt: String(ANCHOR_MS),
            activityWeeks: ACTIVITY_WEEKS,
          },
        ],
      };
    } else if (url.pathname === "/api/repo/alice/proj/history") {
      body = {
        ok: true,
        commits: [
          { author: "alice", date: "2026-08-16", subject: "ship it" },
        ],
      };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });

  await page.goto("/alice/proj/insights");
  const graph = page.locator("[data-repo-activity-graph]");
  await expect(graph).toBeVisible();
  return graph;
}

test("each activity bar carries its week and commit count for pointer users", async ({
  page,
}) => {
  const graph = await openInsights(page);
  const tooltip = graph.locator("[data-repo-activity-tooltip]");
  await expect(tooltip).toBeHidden();

  // The graph itself dates the span it covers, before anyone hovers anything.
  await expect(graph).toContainText("Aug 18, 2025");
  await expect(graph).toContainText("Aug 16, 2026");

  const latest = graph.locator('[data-repo-activity-week="51"]');
  await latest.hover();
  await expect(tooltip).toBeVisible();
  await expect(tooltip).toHaveText("Aug 10, 2026 - Aug 16, 2026: 7 commits");

  // A quiet week is 2px of bar, so the whole column is the target - and it
  // still reports its own range rather than the neighbouring week's.
  const quiet = graph.locator('[data-repo-activity-week="49"]');
  await quiet.hover();
  await expect(tooltip).toHaveText("Jul 27, 2026 - Aug 2, 2026: 0 commits");

  // Singular for one commit.
  await graph.locator('[data-repo-activity-week="50"]').hover();
  await expect(tooltip).toHaveText("Aug 3, 2026 - Aug 9, 2026: 1 commit");

  // Leaving the graph puts the tooltip away.
  await page.mouse.move(0, 0);
  await expect(tooltip).toBeHidden();
});

test("keyboard users walk the weeks from a single tab stop", async ({ page }) => {
  const graph = await openInsights(page);
  const tooltip = graph.locator("[data-repo-activity-tooltip]");
  const weeks = graph.locator("[data-repo-activity-week]");
  await expect(weeks).toHaveCount(52);
  // Roving tabindex: 52 bars must not become 52 stops in the page tab order.
  await expect(graph.locator('[data-repo-activity-week][tabindex="0"]')).toHaveCount(1);

  await weeks.first().focus();
  await expect(tooltip).toBeVisible();
  await expect(tooltip).toHaveText("Aug 18, 2025 - Aug 24, 2025: 0 commits");

  await page.keyboard.press("ArrowRight");
  await expect(graph.locator('[data-repo-activity-week="1"]')).toBeFocused();
  await expect(tooltip).toHaveText("Aug 25, 2025 - Aug 31, 2025: 0 commits");

  await page.keyboard.press("End");
  const latest = graph.locator('[data-repo-activity-week="51"]');
  await expect(latest).toBeFocused();
  await expect(latest).toHaveAttribute("tabindex", "0");
  await expect(graph.locator('[data-repo-activity-week][tabindex="0"]')).toHaveCount(1);
  await expect(tooltip).toHaveText("Aug 10, 2026 - Aug 16, 2026: 7 commits");

  // Screen readers get the same fact without the tooltip.
  await expect(latest).toHaveAttribute("role", "img");
  await expect(latest).toHaveAttribute(
    "aria-label",
    "Aug 10, 2026 - Aug 16, 2026: 7 commits",
  );

  await page.keyboard.press("ArrowLeft");
  await expect(graph.locator('[data-repo-activity-week="50"]')).toBeFocused();
  await expect(tooltip).toHaveText("Aug 3, 2026 - Aug 9, 2026: 1 commit");

  await page.keyboard.press("Home");
  await expect(weeks.first()).toBeFocused();
  // Focus leaving the graph takes the tooltip with it.
  await weeks.first().evaluate((week) => week.blur());
  await expect(tooltip).toBeHidden();
});
