const { test, expect } = require("@playwright/test");
const path = require("node:path");

const SETTINGS_PAGE_PATH = path.resolve(
  __dirname, "..", "..", "public", "dashboard", "settings", "index.html",
);

const FAKE_DESKTOP_MODULE = `
const calls = [];
window.__desktopCalls = calls;
const listeners = {};
const emit = (event, data) => (listeners[event] || []).forEach((fn) => fn(data));
window.__desktopEmit = emit;
const b64 = (text) => btoa(text);
let active = { "claude-code": "default", codex: "default" };
const labels = { "claude-code": { default: "Personal", work: "Work" }, codex: { default: "Default" } };
let terminals = [{ terminal: "run1", provider: "codex", label: "Codex", repositoryOwner: "alpha", repositoryName: "engine", device: "Phone", running: true }];
function usage() {
  const accounts = (provider) => Object.entries(labels[provider]).map(([id, label]) => ({
    id, label, active: active[provider] === id,
    signedIn: id !== "work",
    identity: id === "work" ? "" : label + " <me@example.test>",
    windows: [{ id: "five_hour", label: "5-hour", usedPercent: id === "default" ? 42 : 7, state: "ok", resetAtMs: Date.now() + 3600000 }],
  }));
  return {
    activeProvider: "claude-code",
    providers: [
      { id: "claude-code", label: "Claude Code", windows: [{ id: "five_hour", label: "5-hour", usedPercent: 42, state: "ok", resetAtMs: Date.now() + 3600000 }, { id: "seven_day", label: "Weekly", usedPercent: 91, state: "high" }], accounts: accounts("claude-code") },
      { id: "codex", label: "Codex", windows: [], accounts: accounts("codex") },
    ],
    spend: { claudeUsd: 3.5 },
    atMs: Date.now(),
  };
}
const connection = {
  info: { name: "studio", node: "studio" },
  isOpen: true,
  closeReason: "",
  on(event, fn) { (listeners[event] ||= []).push(fn); return () => {}; },
  onClose() { return () => {}; },
  async request(method, params = {}) {
    calls.push({ method, params });
    if (method === "status") return { capabilities: ["agents", "repos", "agentops.accounts", "terminal.agents", "terminal.accounts"] };
    if (method === "agentops.usage") return usage();
    if (method === "repos.list") return { repos: [{ repo: "alpha/engine", owner: "alpha", name: "engine", checkout: true }, { repo: "alpha/docs", checkout: false }] };
    if (method === "terminal.list") return { terminals };
    if (method === "agentops.accounts.select") { active[params.provider] = params.account; return usage(); }
    if (method === "agentops.accounts.rename") { labels[params.provider][params.account] = params.label; return usage(); }
    if (method === "agentops.accounts.add") { labels[params.provider].fresh = params.label; active[params.provider] = "fresh"; return { ...usage(), account: "fresh" }; }
    if (method === "terminal.start") {
      setTimeout(() => emit("terminal.output", { terminal: "t-" + params.mode, data: b64("Open https://claude.ai/oauth/authorize?code=1 to sign in") }), 50);
      return { terminal: "t-" + params.mode, provider: params.provider, mode: params.mode, replay: "" };
    }
    if (method === "terminal.close") { terminals = terminals.filter((t) => t.terminal !== params.terminal); return {}; }
    return {};
  },
};
export function createDesktopControlHub() {
  return {
    async desktops() { return [{ key: "K1", node: "studio" }, { key: "K2", node: "old-laptop" }]; },
    async connect(node) {
      if (node === "studio") return connection;
      const error = new Error("The desktop did not answer.");
      error.code = "timeout";
      throw error;
    },
    connection(node) { return node === "studio" ? connection : null; },
    closeAll() {},
  };
}
`;

const RUNNERS = {
  ok: true,
  runners: [{
    name: "runner-one",
    scope: "personal",
    status: "active",
    lastSeenAt: Date.now(),
    availability: {
      "claude-code": {
        binaryFound: true,
        loginState: "available",
        source: "oauth",
        accounts: [{ id: "r1", label: "Team", email: "team@example.test", plan: "max", active: true,
          usage: { windows: [{ id: "five_hour", label: "5-hour", usedPercent: 63 }], fetchedAt: Date.now() } }],
      },
      codex: { binaryFound: true, loginState: "missing", accounts: [] },
    },
    logins: [],
    accountRequests: [],
  }],
};

async function openSettings(page) {
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alpha",
      email: "alpha@example.test",
      emailVerified: true,
      sessionToken: "test-session",
      status: "active",
    }));
  });
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.tailwind = {};" }));
  await page.route(/cdn\.jsdelivr\.net\/npm\/lucide/, (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.lucide = { createIcons() {} };" }));
  await page.route("**/desktop-control.js", (route) =>
    route.fulfill({ contentType: "text/javascript", body: FAKE_DESKTOP_MODULE }));
  await page.route("**/dashboard/settings/agents", (route) =>
    route.fulfill({ path: SETTINGS_PAGE_PATH, contentType: "text/html; charset=utf-8" }));
  await page.route("**/api/**", (route) => {
    const url = new URL(route.request().url());
    const body = url.pathname === "/api/runners" ? RUNNERS : { ok: true };
    return route.fulfill({ status: 200, contentType: "application/json; charset=utf-8", body: JSON.stringify(body) });
  });
  await page.goto("/dashboard/settings/agents");
  await expect(page.locator('[data-agent-profile-desktop="studio"]')).toContainText("Connected");
}

async function desktopCalls(page, method) {
  return page.evaluate((name) => window.__desktopCalls.filter((c) => c.method === name), method);
}

const SCREENSHOT_DIR = process.env.SETTINGS_AGENTS_SCREENSHOT_DIR || "";

test("settings lists every desktop account with its usage, and the runners", async ({ page }) => {
  await openSettings(page);
  const studio = page.locator('[data-agent-profile-desktop="studio"]');
  const claude = studio.locator('[data-agent-profile-provider="claude-code"]');
  await expect(claude).toContainText("New agents");
  await expect(claude.locator('[data-agent-profile-account="default"]')).toContainText("In use");
  await expect(claude.locator('[data-agent-profile-account="default"]')).toContainText("Personal <me@example.test>");
  await expect(claude.locator('[data-agent-profile-account="default"]')).toContainText("91% used");
  await expect(claude.locator('[data-agent-profile-account="work"]')).toContainText("Not signed in");
  await expect(claude.locator('[data-agent-profile-account="work"]')).toContainText("7% used");
  await expect(studio.locator('[data-agent-profile-provider="codex"]')).toContainText("Default");
  await expect(studio).toContainText("API spend this month: Anthropic API $3.50");
  await expect(studio.locator('[data-agent-profile-terminal="run1"]')).toContainText("Codex · alpha/engine");
  await expect(page.locator('[data-agent-profile-desktop="old-laptop"]')).toContainText("Not reachable");
  const runner = page.locator('[data-runner="runner-one"]');
  await expect(runner).toContainText("Team");
  await expect(runner).toContainText("63% used");
  if (SCREENSHOT_DIR) await page.screenshot({ path: path.join(SCREENSHOT_DIR, "settings-agents.png"), fullPage: true });
});

test("an account is chosen, renamed, and signed in from settings", async ({ page }) => {
  await openSettings(page);
  const work = page.locator('[data-agent-profile-provider="claude-code"] [data-agent-profile-account="work"]');
  await work.getByRole("button", { name: "Use for new agents" }).click();
  await expect(work).toContainText("In use");
  expect((await desktopCalls(page, "agentops.accounts.select")).at(-1).params)
    .toEqual({ provider: "claude-code", account: "work" });

  page.once("dialog", (dialog) => dialog.accept("Office"));
  await work.getByRole("button", { name: "Rename" }).click();
  await expect(page.locator('[data-agent-profile-provider="claude-code"] [data-agent-profile-account="work"]')).toContainText("Office");

  await page.locator('[data-agent-profile-provider="claude-code"] [data-agent-profile-account="work"]')
    .getByRole("button", { name: "Log in", exact: true }).click();
  const modal = page.locator("[data-agent-login-modal]");
  await expect(modal).toBeVisible();
  await expect(modal.locator("[data-agent-login-title]")).toContainText("Log in to Claude Code (Office) on studio");
  await expect(modal.locator("[data-agent-login-surface]")).toContainText("https://claude.ai/oauth/authorize?code=1");
  await expect(modal.locator("[data-agent-login-status]")).toContainText("Live on studio");
  const start = (await desktopCalls(page, "terminal.start")).at(-1).params;
  expect(start).toMatchObject({ provider: "claude-code", mode: "login", account: "work" });
  if (SCREENSHOT_DIR) await page.screenshot({ path: path.join(SCREENSHOT_DIR, "settings-agents-login.png") });
  await modal.locator("[data-agent-login-close]").click();
  await expect(modal).toBeHidden();
  expect((await desktopCalls(page, "terminal.close")).at(-1).params).toEqual({ terminal: "t-login" });
});

test("adding an account opens its sign-in, and terminals open and end", async ({ page }) => {
  await openSettings(page);
  page.once("dialog", (dialog) => dialog.accept("Side project"));
  await page.locator('[data-agent-profile-provider="codex"]').getByRole("button", { name: "Add account" }).click();
  const modal = page.locator("[data-agent-login-modal]");
  await expect(modal.locator("[data-agent-login-title]")).toContainText("Log in to Codex (Side project) on studio");
  expect((await desktopCalls(page, "agentops.accounts.add")).at(-1).params).toEqual({ provider: "codex", label: "Side project" });
  expect((await desktopCalls(page, "terminal.start")).at(-1).params)
    .toMatchObject({ provider: "codex", mode: "login", account: "fresh" });
  await modal.locator("[data-agent-login-close]").click();

  await page.locator('[data-agent-profile-provider="claude-code"]').getByRole("button", { name: "New terminal" }).click();
  await expect(modal.locator("[data-agent-login-title]")).toContainText("Claude Code in alpha/engine on studio");
  await expect(modal.locator("[data-agent-login-footer]")).toContainText("full permissions");
  expect((await desktopCalls(page, "terminal.start")).at(-1).params)
    .toMatchObject({ provider: "claude-code", mode: "agent", owner: "alpha", name: "engine" });
  await modal.locator("[data-agent-login-close]").click();
  expect((await desktopCalls(page, "terminal.detach")).at(-1).params).toEqual({ terminal: "t-agent" });

  page.once("dialog", (dialog) => dialog.accept());
  await page.locator('[data-agent-profile-terminal="run1"]').getByRole("button", { name: "End" }).click();
  await expect(page.locator('[data-agent-profile-terminal="run1"]')).toHaveCount(0);
  expect((await desktopCalls(page, "terminal.close")).at(-1).params).toEqual({ terminal: "run1" });
});
