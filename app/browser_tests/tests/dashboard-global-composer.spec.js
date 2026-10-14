const { test, expect, devices } = require("@playwright/test");
const path = require("node:path");

const PUBLIC = path.resolve(__dirname, "..", "..", "public");
const REPO_PAGE = path.join(PUBLIC, "dashboard", "repo.html");
const SHOTS = process.env.COMPOSER_SCREENSHOT_DIR || "";
const PNG = Buffer.from(
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==",
  "base64",
);
const SESSION = {
  nodeName: "alice",
  email: "alice@example.test",
  emailVerified: true,
  sessionToken: "test-session",
  status: "active",
  kind: "user",
  nodes: ["alice"],
};
const REPOSITORY = {
  owner: "alice",
  name: "engine",
  description: "Composer fixture",
  visibility: "public",
  source: "local-node",
  cloneUrl: "https://forkmesh.test/alice/engine",
  rootCommit: "root-1",
  stateHash: "state-1",
  updatedAt: "1",
};
const AGENT_SESSIONS = {
  ok: true,
  scope: "owner",
  canQueueAgent: true,
  canViewAgentSessions: true,
  targets: [
    { node: "laptop", providers: ["claude-code", "codex"], online: true, source: false },
    { node: "alice", providers: ["claude-code"], online: false, source: true },
    { node: "fmnext-n1", providers: ["claude-code", "codex"], online: true, source: true, runtimeMode: "runner-pool" },
  ],
  providers: ["claude-code", "codex"],
  models: {
    "claude-code": ["opus", "opus-5", "sonnet", "haiku", "fable"],
    codex: ["spark", "sol", "luna", "terra"],
  },
  sessions: [
    {
      id: "a".repeat(32),
      status: "completed",
      title: "Fix the flaky sync test",
      localAgentId: 12,
      updatedAt: 200,
      agentInfo: { contextTokens: 120000, contextWindow: 200000 },
    },
    { id: "b".repeat(32), status: "queued", title: "Not started", localAgentId: 0, updatedAt: 300 },
  ],
};

async function shot(page, name, locator) {
  if (!SHOTS) return;
  await (locator || page).screenshot({ path: path.join(SHOTS, `composer-${name}.png`) });
}

async function shotAround(page, name, locators) {
  if (!SHOTS) return;
  const boxes = (await Promise.all(locators.map((locator) => locator.boundingBox()))).filter(Boolean);
  const x = Math.max(0, Math.min(...boxes.map((box) => box.x)) - 8);
  const y = Math.max(0, Math.min(...boxes.map((box) => box.y)) - 8);
  const right = Math.max(...boxes.map((box) => box.x + box.width)) + 8;
  const bottom = Math.max(...boxes.map((box) => box.y + box.height)) + 8;
  const viewport = page.viewportSize();
  await page.screenshot({
    path: path.join(SHOTS, `composer-${name}.png`),
    clip: { x, y, width: Math.min(right, viewport.width) - x, height: Math.min(bottom, viewport.height) - y },
  });
}

async function prepare(page, { theme = "dark", calls = [], targets = null } = {}) {
  await page.addInitScript(({ session, theme: chosen }) => {
    localStorage.setItem("forkmesh.session", JSON.stringify(session));
    localStorage.setItem("forkmesh.dashboard.theme", chosen);
    if (!sessionStorage.getItem("composer-seeded")) {
      sessionStorage.setItem("composer-seeded", "1");
      localStorage.removeItem("forkmesh.composer.v1:alice");
      localStorage.setItem("forkmesh.agent.recentRepositories.v1:alice", JSON.stringify(["alice/engine"]));
    }
  }, { session: SESSION, theme });
  await page.route("**/alice/engine/**", (route) => route.fulfill({
    path: REPO_PAGE,
    contentType: "text/html; charset=utf-8",
  }));
  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    const pathname = url.pathname;
    let body = { ok: true };
    let status = 200;
    if (pathname === "/api/repositories") {
      body = { ok: true, repositories: [REPOSITORY] };
    } else if (pathname === "/api/repo/alice/engine/agent-sessions") {
      if (request.method() === "GET") {
        body = targets ? { ...AGENT_SESSIONS, targets } : AGENT_SESSIONS;
      } else {
        calls.push({ kind: "new", path: pathname, body: request.postDataJSON() });
        status = 202;
        body = { ok: true, targetNode: "laptop", queueState: "ready_for_claim" };
      }
    } else if (pathname.startsWith("/api/repo/alice/engine/agent-sessions/")) {
      calls.push({ kind: "add", path: pathname, body: request.postDataJSON() });
      status = 202;
      body = { ok: true, message: "Follow-up sent. It will run as soon as the node picks it up." };
    } else if (pathname === "/api/tasks" && request.method() === "POST") {
      calls.push({ kind: "task", path: pathname, body: request.postDataJSON() });
      body = { ok: true, task: { id: "t1", title: request.postDataJSON().title } };
    } else if (pathname === "/api/apps/commands") {
      body = {
        ok: true,
        commands: [{ command: "standup", description: "Post a standup summary", app: { name: "Standup Bot" } }],
        mentionable: [],
      };
    }
    await route.fulfill({ status, contentType: "application/json; charset=utf-8", body: JSON.stringify(body) });
  });
}

test("composer disables what cannot run and links to runner and desktop setup", async ({ page }) => {
  const calls = [];
  await page.setViewportSize({ width: 1360, height: 860 });
  await prepare(page, { calls, targets: [] });
  await page.goto("/alice/engine/issues/7");
  await page.keyboard.press("Control+/");
  const input = page.locator("[data-composer-input]");
  await expect(input).toBeFocused();
  const create = page.locator("[data-composer-new]");
  const cloud = page.locator("[data-composer-cloud]");
  const setup = page.locator("[data-composer-setup]");
  await expect(create).toBeDisabled();
  await expect(cloud).toBeDisabled();
  await expect(setup).toBeVisible();
  await expect(setup).toContainText("To run agents on alice/engine");
  await expect(setup.locator("a")).toHaveText(["Set up a runner", "Get the desktop app"]);
  await expect(setup.locator("a").first()).toHaveAttribute("href", "/dashboard/runners?setup=1");
  await expect(setup.locator("a").last()).toHaveAttribute("href", "/desktop");
  await expect(page.locator("[data-composer-task]")).toBeEnabled();
  await input.fill("Make the sync test deterministic");
  await input.press("Enter");
  await expect(page.locator("[data-composer-status]")).not.toHaveAttribute("data-tone", "bad");
  expect(calls.filter((call) => call.kind === "new")).toHaveLength(0);
  await shot(page, "no-runner", page.locator("[data-composer-panel]"));
});

test("composer with only a desktop keeps cloud off and locked", async ({ page }) => {
  const calls = [];
  await page.setViewportSize({ width: 1360, height: 860 });
  await prepare(page, {
    calls,
    targets: [{ node: "laptop", providers: ["claude-code", "codex"], online: true, runtimeMode: "desktop" }],
  });
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.composer.v1:alice", JSON.stringify({ cloud: true }));
  });
  await page.goto("/alice/engine/issues/7");
  await page.keyboard.press("Control+/");
  const cloud = page.locator("[data-composer-cloud]");
  await expect(cloud).toBeDisabled();
  await expect(cloud).toHaveAttribute("aria-pressed", "false");
  await expect(cloud).toHaveAttribute("title", /no ForkMesh cloud runner serves alice\/engine/);
  await expect(page.locator("[data-composer-setup]")).toBeHidden();
  const input = page.locator("[data-composer-input]");
  await input.fill("Run it locally");
  await page.locator("[data-composer-new]").click();
  await expect.poll(() => calls.filter((call) => call.kind === "new").length).toBe(1);
  expect(calls[0].body.targetNode).toBe("laptop");
});

test("composer on a repo issue page starts, adds and files tasks with context", async ({ page }) => {
  const calls = [];
  await page.setViewportSize({ width: 1360, height: 860 });
  await prepare(page, { calls });
  await page.goto("/alice/engine/issues/7");

  const pill = page.locator("[data-composer-pill]");
  await expect(pill).toBeVisible();
  await shot(page, "pill-desktop");

  await page.keyboard.press("Control+/");
  const panel = page.locator("[data-composer-panel]");
  await expect(panel).toBeVisible();
  const input = page.locator("[data-composer-input]");
  await expect(input).toBeFocused();
  await expect(panel).not.toHaveClass(/fmc-expanded/);
  await expect(page.locator("[data-composer-expand]")).toHaveAttribute("aria-pressed", "false");
  const compact = await panel.boundingBox();
  expect(compact.width).toBeLessThanOrEqual(560);
  expect((await input.boundingBox()).height).toBeLessThan(120);

  await expect(page.locator("[data-composer-chip-label]")).toHaveText("alice/engine issue #7");
  await expect(page.locator("[data-composer-model]")).toHaveAttribute("data-value", "opus");
  await expect(page.locator("[data-composer-model]")).toHaveText("Opus 5.5");
  await expect(page.locator("[data-composer-model] img")).toHaveAttribute("src", "/assets/agent-ui/model-opus-face.webp");
  await expect(page.locator("[data-composer-mode]")).toHaveAttribute("data-value", "auto");
  await expect(page.locator("[data-composer-mode]")).toHaveText("Auto");
  await expect(page.locator("[data-composer-mode] img")).toHaveAttribute("src", "/assets/agent-ui/mode-auto.webp");
  await expect(page.locator("[data-composer-speed]")).toHaveAttribute("data-value", "high");
  await expect(page.locator("[data-composer-speed] img")).toHaveAttribute("src", "/assets/agent-ui/effort-high.webp");
  await page.locator("[data-composer-mode]").click();
  const modeMenu = page.locator('[data-composer-choice-menu="mode"]');
  await expect(modeMenu).toBeFocused();
  await expect(modeMenu.locator('[role="option"]')).toHaveText([/Auto/, /Ask/, /Plan/, /Edit/]);
  await expect(modeMenu.locator('[role="option"] img')).toHaveCount(4);
  await shot(page, "mode-menu", panel);
  await modeMenu.locator('[role="option"]', { hasText: "Plan" }).click();
  await expect(modeMenu).toBeHidden();
  await expect(page.locator("[data-composer-mode]")).toHaveAttribute("data-value", "plan");
  await page.locator("[data-composer-speed]").click();
  const speedMenu = page.locator('[data-composer-choice-menu="speed"]');
  await expect(speedMenu.locator('[role="option"]')).toHaveText(["Low", "Medium", "High", "Ultra", "Max"]);
  await page.keyboard.press("ArrowDown");
  await page.keyboard.press("Enter");
  await expect(speedMenu).toBeHidden();
  await expect(page.locator("[data-composer-speed]")).toHaveAttribute("data-value", "xhigh");
  await expect(page.locator("[data-composer-speed]")).toBeFocused();
  await input.focus();
  await expect(page.locator("[data-composer-session-field]")).toBeHidden();
  await expect(page.locator("[data-composer-add]")).toBeHidden();
  await expect(page.locator("[data-composer-target]")).toHaveValue("laptop");
  await expect(page.locator("[data-composer-gauge]")).toHaveAttribute("data-level", "none");

  await input.fill("Make the sync test deterministic");
  await page.locator("[data-composer-file]").setInputFiles({ name: "shot.png", mimeType: "image/png", buffer: PNG });
  await expect(page.locator("[data-composer-attachment]")).toHaveCount(1);
  const expected = 8000 - "Make the sync test deterministic".length -
    "\n\nContext: Issue #7 in alice/engine (http://127.0.0.1:4287/alice/engine/issues/7)".length;
  await expect(page.locator("[data-composer-count]")).toHaveText(String(expected));
  await shot(page, "desktop-dark", panel);

  await input.press("Enter");
  await expect(page.locator("[data-composer-status]")).toContainText("Saved for laptop");
  const started = calls.find((call) => call.kind === "new");
  expect(started.body.provider).toBe("auto");
  expect(started.body.model).toBe("opus");
  expect(started.body.mode).toBe("plan");
  expect(started.body.effort).toBe("xhigh");
  expect(started.body.targetNode).toBe("laptop");
  expect(started.body.createPr).toBe(false);
  expect(started.body.sessionToken).toBe("test-session");
  expect(started.body.prompt).toContain("Make the sync test deterministic");
  expect(started.body.prompt).toContain("Context: Issue #7 in alice/engine");
  expect(started.body.attachments).toEqual([
    { name: "shot.png", mediaType: "image/png", data: PNG.toString("base64") },
  ]);
  await expect(input).toHaveValue("");
  await expect(page.locator("[data-composer-attachment]")).toHaveCount(0);

  const cloud = page.locator("[data-composer-cloud]");
  await expect(cloud).toHaveAttribute("aria-pressed", "false");
  await expect(page.locator("[data-composer-target] option")).toHaveText(["laptop", "alice (offline)"]);
  await cloud.click();
  await expect(cloud).toHaveAttribute("aria-pressed", "true");
  await expect(page.locator("[data-composer-target-field]")).toBeHidden();
  await input.fill("Run the slow suite in the cloud");
  await page.locator("[data-composer-new]").click();
  await expect.poll(() => calls.filter((call) => call.kind === "new").length).toBe(2);
  const cloudRun = calls.filter((call) => call.kind === "new")[1];
  expect(cloudRun.body.targetNode).toBe("fmnext-n1");
  await cloud.click();
  await expect(cloud).toHaveAttribute("aria-pressed", "false");
  await expect(page.locator("[data-composer-target]")).toHaveValue("laptop");

  await page.evaluate(() => {
    window.ForkMeshComposerHost = { ...window.ForkMeshComposerHost, selectedAgentId: () => "a".repeat(32) };
  });
  await input.blur();
  await input.focus();
  await expect(page.locator("[data-composer-session-field]")).toBeVisible();
  await expect(page.locator("[data-composer-session]")).toHaveValue("a".repeat(32));
  await expect(page.locator("[data-composer-add]")).toBeVisible();
  await expect(page.locator("[data-composer-gauge]")).toHaveAttribute("data-level", "low");
  await expect(page.locator('[data-composer-gauge] span[data-on="1"]')).toHaveCount(3);
  await input.fill("Also cover the retry path");
  await page.locator("[data-composer-add]").click();
  await expect(page.locator("[data-composer-status]")).toContainText("Follow-up sent");
  const added = calls.find((call) => call.kind === "add");
  expect(added.path).toBe(`/api/repo/alice/engine/agent-sessions/${"a".repeat(32)}`);
  expect(added.body.prompt).toContain("Also cover the retry path");

  await input.fill("Document the sync retry policy\nIt lives in docs/sync.md");
  await page.locator("[data-composer-task]").click();
  await expect(page.locator("[data-composer-status]")).toHaveText("Task added to General.");
  const task = calls.find((call) => call.kind === "task");
  expect(task.body.title).toBe("Document the sync retry policy");
  expect(task.body.details).toContain("It lives in docs/sync.md");
  expect(task.body.details).toContain("Context: Issue #7 in alice/engine");
  expect(task.body.repository).toBe("alice/engine");
  expect(task.body.department).toBe("general");
  expect(task.body.assigneeKind).toBe("unassigned");

  await input.fill("/");
  const menu = page.locator("[data-composer-menu]");
  await expect(menu).toBeVisible();
  await expect(menu.getByText("/standup")).toBeVisible();
  await shotAround(page, "slash-menu", [panel, menu]);
  await input.fill("/stand");
  await expect(menu.locator("[data-composer-menu-item]")).toHaveCount(1);
  await input.press("Enter");
  await expect(menu).toBeHidden();
  await expect(input).toHaveValue("/standup ");

  await input.fill("Draft kept across pages");
  await page.locator("[data-composer-chip-remove]").click();
  await expect(page.locator("[data-composer-chip]")).toBeHidden();
  await expect(page.locator("[data-composer-repo]")).toBeVisible();
  await page.waitForTimeout(400);

  await page.goto("/dashboard/settings/");
  await expect(page.locator("[data-composer-panel]")).toBeVisible();
  await expect(page.locator("[data-composer-input]")).toHaveValue("Draft kept across pages");
  await expect(page.locator("[data-composer-input]")).toBeFocused();
  expect(await page.locator("[data-composer-input]").evaluate((node) => node.selectionStart)).toBe("Draft kept across pages".length);
  await page.locator("[data-composer-expand]").click();
  await expect(page.locator("[data-composer-panel]")).toHaveClass(/fmc-expanded/);
  await expect(page.locator("[data-composer-input]")).toBeFocused();
  await expect(page.locator("[data-composer-chip]")).toBeHidden();

  await page.locator("[data-composer-collapse]").click();
  await expect(page.locator("[data-composer-panel]")).toBeHidden();
  await expect(pill).toBeVisible();
  await expect(page.locator("[data-composer-draft-dot]")).toBeVisible();
  await page.goto("/dashboard/tasks/");
  await expect(pill).toBeVisible();
  await expect(page.locator("[data-composer-panel]")).toBeHidden();
});

test("composer follows the light theme and picks a repository off repo pages", async ({ page }) => {
  await page.setViewportSize({ width: 1280, height: 800 });
  await prepare(page, { theme: "light" });
  await page.goto("/dashboard/settings/");
  await page.locator("[data-composer-pill]").click();
  const panel = page.locator("[data-composer-panel]");
  await expect(panel).toBeVisible();
  await expect(page.locator("[data-composer-chip]")).toBeHidden();
  const repo = page.locator("[data-composer-repo]");
  await expect(repo).toBeVisible();
  await repo.selectOption("alice/engine");
  await expect(page.locator("[data-composer-target-field]")).toBeVisible();
  await expect(page.locator("[data-composer-session-field]")).toBeHidden();
  await expect(page.locator("[data-composer-add]")).toBeHidden();
  const background = await panel.evaluate((node) => getComputedStyle(node).backgroundColor);
  expect(background).toBe("rgb(255, 255, 255)");
  await page.locator("[data-composer-model]").click();
  const modelMenu = page.locator('[data-composer-choice-menu="model"]');
  await expect(modelMenu.locator(".fmc-menu-group")).toHaveText(["Claude Code", "Codex", "Gemma"]);
  await modelMenu.locator('[role="option"]', { hasText: "GPT-5.6 Sol" }).click();
  await expect(page.locator("[data-composer-model]")).toHaveAttribute("data-value", "sol");
  await expect(page.locator("[data-composer-model] img")).toHaveAttribute("src", "/assets/agent-ui/model-sol-face.webp");
  await page.locator("[data-composer-model]").click();
  await modelMenu.locator('[role="option"]', { hasText: "Opus 5.5" }).click();
  await expect(page.locator("[data-composer-model]")).toHaveAttribute("data-value", "opus");
  await page.locator("[data-composer-input]").fill("Check the release checklist against docs/releases.md");
  await shot(page, "desktop-light", panel);
  await page.locator("[data-composer-collapse]").click();
  await page.goto("/dashboard/chat/");
  const pill = page.locator("[data-composer-pill]");
  await expect(pill).toBeVisible();
  const pillBox = await pill.boundingBox();
  expect(page.viewportSize().height - (pillBox.y + pillBox.height)).toBeGreaterThanOrEqual(170);
  await shot(page, "chat-page-light");
});

test.describe("phone", () => {
  const { defaultBrowserType, ...iphone } = devices["iPhone 13"];
  test.use(iphone);

  test("composer is a full-width bottom sheet with 44px targets", async ({ page }) => {
    await prepare(page);
    await page.goto("/alice/engine/pulls/3");
    const pill = page.locator("[data-composer-pill]");
    await expect(pill).toBeVisible();
    const pillBox = await pill.boundingBox();
    expect(pillBox.height).toBeGreaterThanOrEqual(44);
    await shot(page, "pill-mobile");
    await pill.click();
    const panel = page.locator("[data-composer-panel]");
    await expect(panel).toBeVisible();
    await expect(page.locator("[data-composer-chip-label]")).toHaveText("alice/engine PR #3");
    const box = await panel.boundingBox();
    const viewport = page.viewportSize();
    expect(Math.round(box.x)).toBe(0);
    expect(Math.round(box.width)).toBe(viewport.width);
    expect(Math.round(box.y + box.height)).toBe(viewport.height);
    for (const selector of ["[data-composer-new]", "[data-composer-task]", "[data-composer-attach]", "[data-composer-slash]", "[data-composer-collapse]"]) {
      const target = await page.locator(selector).boundingBox();
      expect(target.height, selector).toBeGreaterThanOrEqual(44);
      expect(target.width, selector).toBeGreaterThanOrEqual(44);
    }
    const overflow = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
    expect(overflow).toBeLessThanOrEqual(0);
    const input = await page.locator("[data-composer-input]").boundingBox();
    expect(input.width).toBeGreaterThan(viewport.width - 40);
    const send = await page.locator("[data-composer-new]").boundingBox();
    expect(send.y).toBeGreaterThanOrEqual(input.y + input.height);
    const spill = await page.evaluate(() => [...document.querySelectorAll("[data-composer-panel] *")]
      .filter((node) => node.getClientRects().length && !node.closest("[data-composer-menu]"))
      .filter((node) => node.getBoundingClientRect().right > window.innerWidth + 0.5)
      .map((node) => node.className || node.tagName));
    expect(spill).toEqual([]);
    for (const selector of ["[data-composer-model]", "[data-composer-mode]", "[data-composer-speed]"]) {
      const target = await page.locator(selector).boundingBox();
      expect(target.height, selector).toBeGreaterThanOrEqual(44);
      expect(target.width, selector).toBeGreaterThanOrEqual(44);
    }
    await page.locator("[data-composer-input]").fill("Review this pull request for missing tests");
    await shot(page, "mobile-sheet");
  });

  test("composer sheet rides above the on-screen keyboard", async ({ page }) => {
    await page.addInitScript(() => {
      const fake = new EventTarget();
      Object.assign(fake, { height: window.innerHeight, offsetTop: 0, scale: 1 });
      Object.defineProperty(window, "visualViewport", { configurable: true, get: () => fake });
      window.__fakeViewport = fake;
    });
    await prepare(page);
    await page.goto("/alice/engine/pulls/3");
    await page.locator("[data-composer-pill]").click();
    const panel = page.locator("[data-composer-panel]");
    await expect(panel).toBeVisible();
    await page.evaluate(() => {
      window.__fakeViewport.height = 380;
      window.__fakeViewport.dispatchEvent(new Event("resize"));
    });
    await expect(page.locator("[data-composer]")).toHaveAttribute("data-keyboard", "1");
    const box = await panel.boundingBox();
    expect(Math.round(box.y + box.height)).toBe(380);
    expect(box.y).toBeGreaterThanOrEqual(0);
    await shot(page, "mobile-keyboard");
    await page.evaluate(() => {
      window.__fakeViewport.height = window.innerHeight;
      window.__fakeViewport.dispatchEvent(new Event("resize"));
    });
    await expect(page.locator("[data-composer]")).not.toHaveAttribute("data-keyboard", "1");
    const restored = await panel.boundingBox();
    expect(Math.round(restored.y + restored.height)).toBe(page.viewportSize().height);
  });
});
