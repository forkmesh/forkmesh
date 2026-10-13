// The Calendar page projects every commit and merge from every live
// repository onto the grid, so its cost is set by repository size rather than
// by how many meetings someone booked. This drives the real page with a heavy
// activity set to pin the three things that keep it usable at that size: each
// repository/month is fetched once and reused when the view changes, a busy
// day is summarised rather than rendered whole, and typing in the search box
// does not rebuild the grid per keystroke.
const { test, expect } = require("@playwright/test");
const path = require("node:path");

const CALENDAR_PAGE_PATH = path.resolve(
  __dirname,
  "..",
  "..",
  "public",
  "dashboard",
  "calendar",
  "index.html",
);

const REPOSITORIES = ["engine", "atlas", "beacon", "relay", "ledger"].map(
  (name, index) => ({
    owner: "alice",
    name,
    visibility: "public",
    source: "local-node",
    liveHost: true,
    cloneOnline: true,
    cloneUrl: `https://forkmesh.test/alice/${name}`,
    rootCommit: `root-${index}`,
    stateHash: `state-${index}`,
    updatedAt: String(index + 1),
  }),
);

// Enough per repository that an accidental O(days x commits) pass, or an
// agenda that renders every row, shows up as a hang rather than a slow frame.
const COMMITS_PER_REPOSITORY = 900;

function commitsFor(name, since, until) {
  const start = Number(since) * 1000;
  const end = Number(until) * 1000;
  const span = Math.max(1, end - start);
  const commits = [];
  for (let index = 0; index < COMMITS_PER_REPOSITORY; index += 1) {
    const at = start + Math.floor((span * index) / COMMITS_PER_REPOSITORY);
    // A real commit belongs to exactly one month, so the window has to be part
    // of the identity - otherwise neighbouring months hand back the same
    // hashes and the page correctly folds them into one event.
    const hash = (name.charCodeAt(0).toString(16) + Number(since).toString(16) +
      String(index).padStart(6, "0")).padEnd(40, "0");
    commits.push({
      hash,
      author: "alice",
      date: new Date(at).toISOString(),
      // Every eighth commit is a merge, so both activity calendars are used.
      parents: index % 8 === 0 ? `${hash}1 ${hash}2` : `${hash}1`,
      subject: `${name} change ${index}`,
    });
  }
  return commits;
}

async function openCalendar(page, historyRequests) {
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
  await page.route("**/dashboard/calendar", (route) => route.fulfill({
    path: CALENDAR_PAGE_PATH,
    contentType: "text/html; charset=utf-8",
  }));
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    const history = url.pathname.match(/^\/api\/repo\/alice\/([\w-]+)\/history$/);
    let body = { ok: true };
    if (url.pathname === "/api/repositories") {
      body = { ok: true, repositories: REPOSITORIES };
    } else if (url.pathname === "/api/calendar") {
      body = { ok: true, events: [], calendars: [], actor: "alice" };
    } else if (history) {
      const since = url.searchParams.get("since");
      const until = url.searchParams.get("until");
      historyRequests.push(`${history[1]}:${since}:${until}`);
      body = { ok: true, commit: "0".repeat(40), commits: commitsFor(history[1], since, until) };
    }
    return route.fulfill({
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
  await page.goto("/dashboard/calendar");
  await expect(page.locator("[data-calendar-status]")).not.toContainText(
    "loading repository activity",
    { timeout: 20_000 },
  );
}

test("repository activity is fetched per month and reused across views", async ({
  page,
}) => {
  const historyRequests = [];
  await openCalendar(page, historyRequests);

  // Every request names a whole calendar month, which is what makes the
  // results reusable between views and between neighbouring months.
  expect(historyRequests.length).toBeGreaterThan(0);
  for (const entry of historyRequests) {
    const [, since, until] = entry.split(":");
    const first = new Date(Number(since) * 1000);
    const last = new Date(Number(until) * 1000);
    expect(first.getDate()).toBe(1);
    expect(first.getHours()).toBe(0);
    // `until` is the last second of the month, so the next second is the 1st.
    expect(new Date(last.getTime() + 1000).getDate()).toBe(1);
  }
  // No repository is asked for the same month twice.
  expect(new Set(historyRequests).size).toBe(historyRequests.length);

  // Switching view and opening a day stay inside months already in hand, so
  // they must not go back to the mirrors at all.
  const afterFirstPaint = historyRequests.length;
  await page.locator('[data-calendar-mode="week"]').click();
  await expect(page.locator(".fm-cal-week")).toBeVisible();
  await page.locator('[data-calendar-mode="day"]').click();
  await page.locator('[data-calendar-mode="month"]').click();
  await expect(page.locator(".fm-cal-grid")).toBeVisible();
  expect(historyRequests.length).toBe(afterFirstPaint);

  // Paging to the next month and back re-uses everything it already holds:
  // the return trip asks for nothing.
  await page.locator("[data-calendar-next]").click();
  await expect(page.locator("[data-calendar-status]")).not.toContainText(
    "loading repository activity",
    { timeout: 20_000 },
  );
  const afterNextMonth = historyRequests.length;
  expect(afterNextMonth).toBeGreaterThan(afterFirstPaint);
  await page.locator("[data-calendar-prev]").click();
  await expect(page.locator(".fm-cal-grid")).toBeVisible();
  expect(historyRequests.length).toBe(afterNextMonth);
});

test("a busy day is summarised rather than rendered whole", async ({ page }) => {
  const historyRequests = [];
  await openCalendar(page, historyRequests);

  // Month cells keep their four-event preview plus an overflow link even when
  // the day holds hundreds of commits.
  const cell = page.locator("[data-calendar-day]").filter({
    has: page.locator("[data-calendar-event]"),
  }).first();
  expect(await cell.locator("[data-calendar-event]").count()).toBeLessThanOrEqual(4);
  await expect(cell.locator("[data-calendar-open-day]").last()).toContainText("more");

  // The agenda spans 90 days. It renders a page at a time and offers the rest,
  // instead of building every row of every day up front.
  await page.locator('[data-calendar-mode="agenda"]').click();
  const rows = page.locator("[data-calendar-content] article");
  const firstPage = await rows.count();
  expect(firstPage).toBeGreaterThan(0);
  expect(firstPage).toBeLessThanOrEqual(200);
  const more = page.locator("[data-calendar-agenda-more]");
  await expect(more).toBeVisible();
  await more.click();
  expect(await rows.count()).toBeGreaterThan(firstPage);
});

test("searching the calendar rebuilds the grid once, not once per keystroke", async ({
  page,
}) => {
  const historyRequests = [];
  await openCalendar(page, historyRequests);

  const status = page.locator("[data-calendar-status]");
  const before = Number((await status.textContent()).split(" ")[0]);
  expect(before).toBeGreaterThan(1000);

  // Count how many times the grid is replaced while a word is typed. Each
  // rebuild re-filters every commit in the window, so one per character is
  // what made the box feel stuck.
  await page.evaluate(() => {
    window.__calendarRebuilds = 0;
    const content = document.querySelector("[data-calendar-content]");
    new MutationObserver((records) => {
      for (const record of records) {
        if (record.addedNodes.length) window.__calendarRebuilds += 1;
      }
    }).observe(content, { childList: true });
  });
  await page.locator("[data-calendar-search]").pressSequentially("beacon", {
    delay: 30,
  });
  await expect
    .poll(async () => Number((await status.textContent()).split(" ")[0]), {
      timeout: 8_000,
    })
    .toBeLessThan(before);
  expect(await page.evaluate(() => window.__calendarRebuilds)).toBeLessThan(6);

  // The match is real: only the searched repository is left on the grid.
  await page.locator('[data-calendar-mode="agenda"]').click();
  const rows = page.locator("[data-calendar-content] article");
  expect(await rows.count()).toBeGreaterThan(0);
  await expect(rows.first()).toContainText("beacon");
});
