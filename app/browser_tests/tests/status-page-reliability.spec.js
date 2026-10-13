const { test, expect } = require("@playwright/test");
const fs = require("node:fs");
const path = require("node:path");


const ROOT = path.resolve(__dirname, "..", "..", "..");
const STATUS_HTML = fs.readFileSync(
  path.join(ROOT, "app", "public", "status.html"),
  "utf8",
);


async function serveStatusPage(page) {
  await page.route("https://app.forkmesh.test/**", (route) => {
    const pathname = new URL(route.request().url()).pathname;
    if (pathname === "/status.html") {
      return route.fulfill({
        status: 200,
        contentType: "text/html; charset=utf-8",
        body: STATUS_HTML,
      });
    }
    if (pathname.endsWith(".js")) {
      return route.fulfill({
        status: 200,
        contentType: "text/javascript; charset=utf-8",
        body: "",
      });
    }
    return route.fulfill({ status: 200, body: "" });
  });
}


function system(id, status, monitoringState = null) {
  return {
    id,
    label: id,
    status,
    monitoringState,
    uptimePct: status === "operational" ? 100 : null,
    uptime24hPct: status === "operational" ? 100 : null,
    coveragePct: status === "operational" ? 100 : 0,
    coverage24hPct: status === "operational" ? 100 : 0,
    checks24h: status === "operational" ? 1 : 0,
    failures24h: monitoringState ? 0 : status === "down" ? 1 : 0,
    missing24h: monitoringState ? 1 : 0,
    days: [],
    minutes: [],
    reason: monitoringState ? "Monitoring data is unavailable." : null,
    reasonTs: null,
  };
}


function payload(now, systems) {
  return {
    ok: true,
    now,
    systems,
    current: { lastCronSampleTs: now },
  };
}


async function installFakeStatusSocket(page) {
  await page.addInitScript(() => {
    window.__statusSockets = [];
    class FakeStatusSocket extends EventTarget {
      constructor() {
        super();
        window.__statusSockets.push(this);
        window.setTimeout(() => this.dispatchEvent(new Event("open")), 10);
      }

      close() {
        this.dispatchEvent(new Event("close"));
      }

      emitUpdate() {
        this.dispatchEvent(new MessageEvent("message", {
          data: JSON.stringify({ type: "status-update" }),
        }));
      }
    }
    window.WebSocket = FakeStatusSocket;
  });
}


test("an older WebSocket reread cannot overwrite a newer status generation", async ({ page }) => {
  await serveStatusPage(page);
  await installFakeStatusSocket(page);
  const responses = [
    payload(3_000, [system("website", "operational")]),
    payload(2_000, [system("website", "down")]),
    payload(4_000, [system("website", "down")]),
  ];
  let reads = 0;
  await page.route("**/api/status", async (route) => {
    const index = Math.min(reads, responses.length - 1);
    reads += 1;
    if (index === 0) await new Promise((resolve) => setTimeout(resolve, 40));
    await route.fulfill({
      status: 200,
      contentType: "application/json",
      body: JSON.stringify(responses[index]),
    });
  });

  await page.goto("https://app.forkmesh.test/status.html");
  await expect.poll(() => reads).toBeGreaterThanOrEqual(2);
  await expect(page.locator("#status-banner-text")).toHaveText(
    "All systems operational",
  );
  await expect(page.locator("#system-website .status-dot")).toHaveClass(
    /is-operational/,
  );

  await page.evaluate(() => window.__statusSockets[0].emitUpdate());
  await expect.poll(() => reads).toBeGreaterThanOrEqual(3);
  await expect(page.locator("#status-banner-text")).toHaveText("Partial outage");
  await expect(page.locator("#system-website .status-dot")).toHaveClass(/is-down/);
});


test("missing, stale, and empty monitoring remain red with distinct classes", async ({ page }) => {
  await serveStatusPage(page);
  await page.addInitScript(() => {
    window.WebSocket = undefined;
  });
  await page.route("**/api/status", (route) => route.fulfill({
    status: 200,
    contentType: "application/json",
    body: JSON.stringify(payload(5_000, [
      system("missing", "down", "missing"),
      system("stale", "down", "stale"),
      system("empty", "down", "empty"),
    ])),
  }));

  await page.goto("https://app.forkmesh.test/status.html");
  await expect(page.locator("#status-banner")).toHaveClass(/is-monitoring-stale/);
  for (const state of ["missing", "stale", "empty"]) {
    await expect(page.locator(`#system-${state} .status-dot`)).toHaveClass(
      new RegExp(`is-monitoring-${state}`),
    );
  }
});


test("the 60-minute chart precedes the 24-hour and 30-day histories", async ({ page }) => {
  await serveStatusPage(page);
  await page.addInitScript(() => {
    window.WebSocket = undefined;
  });
  const now = 7_200_000;
  const website = system("website", "operational");
  website.minutes = Array.from({ length: 60 }, (_, index) => ({
    minuteTs: now - (59 - index) * 60_000,
    status: "operational",
  }));
  await page.route("**/api/status", (route) => route.fulfill({
    status: 200,
    contentType: "application/json",
    body: JSON.stringify(payload(now, [website])),
  }));

  await page.goto("https://app.forkmesh.test/status.html");
  const row = page.locator("#system-website");
  await expect(row.locator(".status-minute-slice")).toHaveCount(60);
  expect(await row.locator(":scope > .status-history-row").evaluateAll(
    (rows) => rows.map((row) => row.firstElementChild.className),
  )).toEqual(
    ["status-minute-bar", "status-hour-bar", "status-days"],
  );
});


test("reachable mirrors with converging issue revisions are not red downtime", async ({ page }) => {
  await serveStatusPage(page);
  await page.addInitScript(() => {
    window.WebSocket = undefined;
  });
  const response = payload(6_000, [system("website", "operational")]);
  response.current = {
    lastCronSampleTs: 6_000,
    mainnodeOnline: false,
    mainnodeLastSeenTs: 1_000,
    mainnodeStaleMs: 600_000,
    mainnodeUnacceptedMirrors: 3,
    onlineNodes: 3,
  };
  await page.route("**/api/status", (route) => route.fulfill({
    status: 200,
    contentType: "application/json",
    body: JSON.stringify(response),
  }));

  await page.goto("https://app.forkmesh.test/status.html");
  await expect(page.locator("#status-banner-text")).toHaveText(
    "All systems operational",
  );
  await expect(page.locator("#stat-mainnode")).toHaveText("Mirrors reachable");
  await expect(page.locator("#stat-mainnode-detail")).toContainText(
    "not service downtime",
  );
  await expect(page.locator("#status-reasons li strong")).toContainText(
    "Repository revision sync",
  );
  await expect(page.locator("#status-reasons .status-dot")).toHaveClass(
    /is-degraded/,
  );
  await expect(page.locator("#status-reasons .status-dot")).not.toHaveClass(
    /is-down/,
  );
});


test("cores and runners get their own section with only the 60-minute and 24-hour strips", async ({ page }) => {
  await serveStatusPage(page);
  await page.addInitScript(() => {
    window.WebSocket = undefined;
  });
  const now = 7_200_000;
  const minutes = Array.from({ length: 60 }, (_, index) => ({
    minuteTs: now - (59 - index) * 60_000,
    status: "operational",
  }));
  const rows = ["website", "core:n2", "runner:fmnext-r1", "core:n4", "mirror:m1"].map((id) => {
    const row = system(id, "operational");
    row.minutes = minutes;
    row.days = [{ dayTs: 0, checks: 120, failures: 0, uptimePct: 100, hours: [] }];
    return row;
  });
  await page.route("**/api/status", (route) => route.fulfill({
    status: 200,
    contentType: "application/json",
    body: JSON.stringify(payload(now, rows)),
  }));

  await page.route("**/styles.css", (route) => route.fulfill({
    status: 200,
    contentType: "text/css; charset=utf-8",
    body: fs.readFileSync(path.join(ROOT, "www", "public", "styles.css"), "utf8"),
  }));
  await page.goto("https://app.forkmesh.test/status.html");
  await expect(page.locator("#status-nodes")).toBeVisible();
  const nodeIds = await page.locator("#status-node-list > .status-row").evaluateAll(
    (items) => items.map((item) => item.id),
  );
  expect(nodeIds).toEqual(["system-core-n2", "system-runner-fmnext-r1", "system-core-n4"]);
  const serviceIds = await page.locator("#status-list > .status-row").evaluateAll(
    (items) => items.map((item) => item.id),
  );
  expect(serviceIds).toEqual(["system-website", "system-mirror-m1"]);
  for (const id of nodeIds) {
    expect(await page.locator(`#${id} > .status-history-row`).evaluateAll(
      (strips) => strips.map((strip) => strip.firstElementChild.className),
    )).toEqual(["status-minute-bar", "status-hour-bar"]);
  }
  expect(await page.locator("#system-website > .status-history-row").count()).toBe(3);
  if (process.env.STATUS_SCREENSHOT) {
    await page.setViewportSize({ width: 1100, height: 1400 });
    await page.locator(".status-systems").screenshot({ path: process.env.STATUS_SCREENSHOT });
  }
});


test("the nodes section stays hidden without core or runner rows", async ({ page }) => {
  await serveStatusPage(page);
  await page.addInitScript(() => {
    window.WebSocket = undefined;
  });
  await page.route("**/api/status", (route) => route.fulfill({
    status: 200,
    contentType: "application/json",
    body: JSON.stringify(payload(5_000, [system("website", "operational")])),
  }));
  await page.goto("https://app.forkmesh.test/status.html");
  await expect(page.locator("#system-website")).toBeVisible();
  await expect(page.locator("#status-nodes")).toBeHidden();
});
