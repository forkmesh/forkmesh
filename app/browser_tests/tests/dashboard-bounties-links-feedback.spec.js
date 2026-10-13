const { test, expect } = require("@playwright/test");
const path = require("node:path");

const PUBLIC = path.resolve(__dirname, "..", "..", "public");
const WWW_PUBLIC = path.resolve(__dirname, "..", "..", "..", "www", "public");
const SHOT_DIR = process.env.FORKMESH_WEB_KIOSK_SHOTS || "";
const NOW = Date.now();
const WALLET = "7Np41oeYqPefeNQEHSv1UDhYrehxin3NStELsSKCT4K2";

function task(id, overrides) {
  return {
    id,
    kind: "task",
    title: "Task",
    details: "",
    status: "idle",
    completedAt: 0,
    assignee: "alice",
    assigneeKind: "user",
    createdBy: "alice",
    department: "engineering",
    bountyRequest: null,
    createdAt: NOW - 7_200_000,
    updatedAt: NOW - 60_000,
    ...overrides,
  };
}

const BIDS = [
  task("aaaaaaaabbbbccccddddeeeeffff0001", {
    kind: "bid",
    title: "Write the self-hosting guide for small teams",
    details: "A step-by-step guide from an empty VPS to a running mirror, with screenshots and a troubleshooting section.",
    assignee: "wendy",
    createdBy: "wendy",
    department: "community",
    bountyRequest: { currency: "SOL", amountSol: "0.25", lamports: 250000000, status: "requested" },
    createdAt: NOW - 3_600_000,
  }),
  task("aaaaaaaabbbbccccddddeeeeffff0002", {
    kind: "bid",
    title: "Profile the World load on low-end Android phones",
    assignee: "alice",
    createdBy: "alice",
    status: "active",
    bountyRequest: { currency: "SOL", amountSol: "0.4", lamports: 400000000, status: "requested" },
    createdAt: NOW - 86_400_000,
  }),
  task("aaaaaaaabbbbccccddddeeeeffff0003", {
    kind: "bid",
    title: "Translate the onboarding docs to Spanish",
    assignee: "jett",
    createdBy: "jett",
    status: "done",
    completedAt: NOW - 600_000,
    department: "marketing",
    bountyRequest: { currency: "SOL", amountSol: "0.15", lamports: 150000000, status: "requested" },
  }),
];
const PLAIN = task("aaaaaaaabbbbccccddddeeeeffff0009", { title: "Ordinary task without a bounty" });

const LINKS = [
  {
    id: "l1", url: "https://dev.example.com/posts/forkmesh-mirrors", host: "dev.example.com",
    title: "Why I moved my repositories to a mesh", submittedBy: "wendy", channel: "article",
    score: 72, potentialTraffic: { low: 180, high: 720 }, walletAddress: WALLET,
    rewardSol: 0.0004, paymentStatus: "unpaid", paidAt: 0, createdAt: NOW - 5_400_000,
  },
  {
    id: "l2", url: "https://video.example.org/watch/forkmesh-world", host: "video.example.org",
    title: "A walk through the ForkMesh World", submittedBy: "alice", channel: "video",
    score: 41, potentialTraffic: { low: 40, high: 160 }, walletAddress: "",
    rewardSol: 0.0002, paymentStatus: "unpaid", paidAt: 0, createdAt: NOW - 86_400_000,
  },
  {
    id: "l3", url: "https://forum.example.net/t/forkmesh", host: "forum.example.net",
    title: "ForkMesh thread", submittedBy: "jett", channel: "community",
    score: 18, potentialTraffic: { low: 5, high: 22 }, walletAddress: WALLET,
    rewardSol: 0.0001, paymentStatus: "paid", paidAt: NOW - 3_600_000, createdAt: NOW - 172_800_000,
  },
];

async function preparePage(page, { signedIn = true, taskError = "", sent = [] } = {}) {
  if (signedIn) {
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
  }
  await page.route("**/assets/vendor/lucide-0.468.0.min.js", (route) => route.fulfill({
    path: path.join(WWW_PUBLIC, "assets", "vendor", "lucide-0.468.0.min.js"),
    contentType: "text/javascript",
  }));
  await page.route("**/api-client.js*", (route) => route.fulfill({
    path: path.join(WWW_PUBLIC, "api-client.js"),
    contentType: "text/javascript",
  }));
  await page.route(/\/dashboard\/bounties(?:\?.*)?$/, (route) => route.fulfill({
    path: path.join(PUBLIC, "dashboard", "bounties", "index.html"),
    contentType: "text/html; charset=utf-8",
  }));
  await page.route(/\/dashboard\/links(?:\?.*)?$/, (route) => route.fulfill({
    path: path.join(PUBLIC, "dashboard", "links", "index.html"),
    contentType: "text/html; charset=utf-8",
  }));
  const links = LINKS.map((link) => ({ ...link }));
  const tasks = [...BIDS, PLAIN].map((item) => ({ ...item }));
  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    let status = 200;
    let body = { ok: true };
    if (url.pathname === "/api/tasks") {
      if (request.method() === "POST") {
        const payload = JSON.parse(request.postData() || "{}");
        sent.push({ path: url.pathname, payload });
        tasks.unshift(task("aaaaaaaabbbbccccddddeeeeffff0010", {
          kind: "bid",
          title: payload.title,
          details: payload.details,
          department: payload.department,
          bountyRequest: { currency: "SOL", amountSol: payload.bountyAmountSol, lamports: 1, status: "requested" },
          createdAt: Date.now(),
        }));
        status = 201;
        body = { ok: true };
      } else if (taskError) {
        status = 403;
        body = { error: taskError };
      } else {
        body = { ok: true, tasks, hasMore: false, taskLimit: 2000 };
      }
    } else if (url.pathname === "/api/world/link-kiosk") {
      if (request.method() === "POST") {
        const payload = JSON.parse(request.postData() || "{}");
        sent.push({ path: url.pathname, payload });
        links.unshift({
          id: "l0", url: payload.url, host: new URL(payload.url).hostname, title: payload.title,
          submittedBy: "alice", channel: payload.channel, score: 55, potentialTraffic: { low: 60, high: 240 },
          walletAddress: "", rewardSol: 0.0003, paymentStatus: "unpaid", paidAt: 0, createdAt: Date.now(),
        });
        status = 201;
        body = {
          ok: true,
          submission: { score: 55, potentialTraffic: { low: 60, high: 240 }, rewardSol: 0.0003 },
          links,
        };
      } else {
        body = { ok: true, links };
      }
    } else if (url.pathname === "/api/feedback") {
      sent.push({ path: url.pathname, payload: JSON.parse(request.postData() || "{}") });
    }
    return route.fulfill({
      status,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
}

async function shoot(page, name) {
  if (!SHOT_DIR) return;
  await page.waitForTimeout(250);
  await page.screenshot({ path: path.join(SHOT_DIR, `${name}.png`), fullPage: false });
}

async function shootList(page, selector, name) {
  if (!SHOT_DIR) return;
  await page.locator(selector).scrollIntoViewIfNeeded();
  await shoot(page, name);
}

async function shootSidebar(page, name) {
  if (!SHOT_DIR) return;
  await page.locator("[data-mobile-menu-toggle]").click();
  await shoot(page, name);
  await page.locator("[data-mobile-sidebar-backdrop]").dispatchEvent("click");
  await expect(page.locator("[data-mobile-menu-toggle]")).toHaveAttribute("aria-expanded", "false");
}

test("bounties lists only bids, filters them and submits a new bid", async ({ page }) => {
  const sent = [];
  await preparePage(page, { sent });
  await page.goto("/dashboard/bounties");
  const rows = page.locator("[data-bounties-list] article");
  await expect(rows).toHaveCount(2);
  await expect(page.locator("[data-bounties-list]")).not.toContainText("Ordinary task without a bounty");
  await expect(rows.first()).toContainText("Write the self-hosting guide");
  await expect(rows.first()).toContainText("0.25 SOL");
  await expect(page.locator("[data-bounties-list]")).toContainText("your bid");
  await expect(page.locator("[data-bounties-count]")).toHaveText("2 open");
  await expect(page.locator('a[data-nav="bounties"]')).toHaveAttribute("aria-current", "page");
  await shoot(page, `bounties-${page.viewportSize().width}`);
  await shootList(page, "[data-bounties-list]", `bounties-list-${page.viewportSize().width}`);
  await shootSidebar(page, `sidebar-${page.viewportSize().width}`);

  await page.locator('[data-bounties-filter="done"]').click();
  await expect(rows).toHaveCount(1);
  await expect(rows.first()).toContainText("Completed");
  await page.locator('[data-bounties-filter="open"]').click();

  const form = page.locator("[data-bounties-form]");
  await form.locator('[name="title"]').fill("Record a screencast of the desktop setup");
  await form.locator('[name="amountSol"]').fill("abc");
  await form.locator("[data-bounties-submit]").click();
  await expect(page.locator("[data-bounties-notice]")).toContainText("positive SOL amount");
  expect(sent).toHaveLength(0);

  await form.locator('[name="amountSol"]').fill("0.3");
  await form.locator('[name="department"]').selectOption("marketing");
  await form.locator("[data-bounties-submit]").click();
  await expect(page.locator("[data-bounties-notice]")).toContainText("0.3 SOL request");
  expect(sent).toEqual([{
    path: "/api/tasks",
    payload: {
      kind: "bid",
      title: "Record a screencast of the desktop setup",
      details: "",
      bountyAmountSol: "0.3",
      department: "marketing",
      destination: "department",
      assigneeKind: "user",
    },
  }]);
  await expect(rows).toHaveCount(3);
});

test("bounties explain organization membership to non-members", async ({ page }) => {
  await preparePage(page, { taskError: "org_member_required" });
  await page.goto("/dashboard/bounties");
  await expect(page.locator("[data-bounties-start]")).toContainText("Ask an organization owner");
  await expect(page.locator("[data-bounties-form]")).toBeHidden();
  await expect(page.locator("[data-bounties-count]")).toHaveText("Members only");
});

test("bounties ask signed-out visitors to log in", async ({ page }) => {
  await preparePage(page, { signedIn: false });
  await page.goto("/dashboard/bounties");
  await expect(page.locator("[data-bounties-start] a[href^='/login']")).toBeVisible();
  await expect(page.locator("[data-bounties-form]")).toBeHidden();
});

test("link rewards list public links and submit a consented link", async ({ page }) => {
  const sent = [];
  await preparePage(page, { sent });
  await page.goto("/dashboard/links");
  const rows = page.locator("[data-links-list] article");
  await expect(rows).toHaveCount(3);
  await expect(rows.nth(0)).toContainText("72");
  await expect(rows.nth(0)).toContainText("0.00040 SOL");
  await expect(rows.nth(0).locator('a[href^="solana:"]')).toHaveAttribute(
    "href", `solana:${WALLET}?amount=0.00040000&label=ForkMesh%20link%20reward`);
  await expect(rows.nth(0).locator('a[target="_blank"]')).toHaveAttribute("rel", "nofollow ugc noopener noreferrer");
  await expect(rows.nth(1)).toContainText("Add a Solana address");
  await expect(rows.nth(2)).toContainText("Paid");
  await expect(page.locator('a[data-nav="links"]')).toHaveAttribute("aria-current", "page");
  await shoot(page, `links-${page.viewportSize().width}`);
  await shootList(page, "[data-links-list]", `links-list-${page.viewportSize().width}`);

  const form = page.locator("[data-links-form]");
  await form.locator('[name="url"]').fill("https://blog.example.com/forkmesh");
  await form.locator('[name="title"]').fill("My ForkMesh review");
  await form.locator('[name="channel"]').selectOption("social");
  await form.locator("[data-links-submit]").click();
  await expect(page.locator("[data-links-notice]")).toContainText("Tick the consent box");
  expect(sent).toHaveLength(0);

  await form.locator('[name="consent"]').check();
  await form.locator("[data-links-submit]").click();
  await expect(page.locator("[data-links-notice]")).toContainText("reach 55/100");
  expect(sent).toEqual([{
    path: "/api/world/link-kiosk",
    payload: {
      url: "https://blog.example.com/forkmesh",
      title: "My ForkMesh review",
      channel: "social",
      consent: true,
    },
  }]);
  await expect(rows).toHaveCount(4);
});

test("link rewards stay readable when signed out", async ({ page }) => {
  await preparePage(page, { signedIn: false });
  await page.goto("/dashboard/links");
  await expect(page.locator("[data-links-list] article")).toHaveCount(3);
  await expect(page.locator("[data-links-start] a[href^='/login']")).toBeVisible();
  await expect(page.locator("[data-links-form]")).toBeHidden();
});

test("send feedback opens from the account menu and from ?feedback=1", async ({ page }) => {
  const sent = [];
  await preparePage(page, { sent });
  await page.goto("/dashboard/links?feedback=1");
  const modal = page.locator("[data-feedback-modal]");
  await expect(modal).toBeVisible();
  await expect(page).toHaveURL(/\/dashboard\/links$/);
  await expect(page.locator("[data-feedback-message]")).toBeFocused();
  await page.keyboard.press("Escape");
  await expect(modal).toBeHidden();

  const viewport = page.viewportSize();
  if (viewport.width >= 1024) {
    await page.locator("#accountMenuToggle").click();
    await page.locator("#accountMenuDropdown [data-feedback-open]").click();
  } else {
    await page.goto("/dashboard/links?feedback=1");
  }
  await expect(modal).toBeVisible();
  await page.locator("[data-feedback-message]").fill("The new bounties page is much easier than the lobby desk.");
  await shoot(page, `feedback-${viewport.width}`);
  await page.locator("[data-feedback-submit]").click();
  await expect(page.locator("[data-feedback-status]")).toContainText("Thank you");
  expect(sent).toEqual([{
    path: "/api/feedback",
    payload: {
      source: "web",
      vote: "feedback",
      message: "The new bounties page is much easier than the lobby desk.",
    },
  }]);
  await expect(modal).toBeHidden({ timeout: 4000 });
});
