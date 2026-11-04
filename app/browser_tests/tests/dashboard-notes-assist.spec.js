const { test, expect } = require("@playwright/test");
const path = require("node:path");

const NOTES_PAGE_PATH = path.resolve(
  __dirname,
  "..",
  "..",
  "public",
  "dashboard",
  "notes",
  "index.html",
);

const NOTE_ID = "c".repeat(32);
const MARKDOWN = "Intro line.\n\nthis are rong\n\nLast line.";

async function openNotes(page, { ready = true, assistCalls = [], answer = "This is right." } = {}) {
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alice",
      email: "alice@example.test",
      emailVerified: true,
      sessionToken: "test-session",
      status: "active",
    }));
    localStorage.setItem("forkmesh.notes.layout", "split");
    window.ForkMeshDashboard = {
      _registrations: [["notes", ({ builtinPage }) => builtinPage("notes")]],
      registerPage(pageId, initializer) { this._registrations.push([pageId, initializer]); },
    };
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
  await page.route(/\/dashboard\/notes(\?.*)?$/, (route) => route.fulfill({
    path: NOTES_PAGE_PATH,
    contentType: "text/html; charset=utf-8",
  }));
  const note = {
    id: NOTE_ID, title: "Draft", markdown: MARKDOWN, version: 1, role: "owner",
    visibility: "private", updatedAt: new Date().toISOString(), shares: [], links: [],
  };
  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    let status = 200;
    let body = { ok: true };
    if (url.pathname === "/api/notes") {
      const { markdown, ...row } = note;
      body = { ok: true, notes: [row] };
    } else if (url.pathname === "/api/notes/assist") {
      if (request.method() === "GET") {
        body = { ok: true, ready, actions: [], remaining: 100 };
      } else {
        assistCalls.push(request.postDataJSON());
        body = { ok: true, text: answer, action: request.postDataJSON().action };
      }
    } else if (url.pathname === `/api/notes/${NOTE_ID}`) {
      body = { ok: true, note };
    }
    return route.fulfill({
      status,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify(body),
    });
  });
  await page.goto("/dashboard/notes");
  await expect(page.locator("[data-note-markdown]")).toHaveValue(MARKDOWN);
}

async function selectInNote(page, text) {
  const area = page.locator("[data-note-markdown]");
  await area.evaluate((element, wanted) => {
    const start = element.value.indexOf(wanted);
    element.focus();
    element.setSelectionRange(start, start + wanted.length);
    element.dispatchEvent(new MouseEvent("mouseup", { bubbles: true, clientX: 40, clientY: 40 }));
  }, text);
  return area;
}

test("a selection is rewritten with a quick action and replaces the text", async ({ page }) => {
  const assistCalls = [];
  await openNotes(page, { assistCalls });
  await selectInNote(page, "this are rong");
  const float = page.locator("[data-note-assist-float]");
  await expect(float).toBeVisible();
  await float.click();
  const panel = page.locator("[data-note-assist-panel]");
  await expect(panel).toBeVisible();
  await expect(panel.locator("[data-note-assist-heading]")).toHaveText("Rewrite with AI");
  await expect(panel.locator("[data-note-assist-source]")).toHaveText("this are rong");
  await panel.locator("[data-note-assist-action=\"grammar\"]").click();
  await expect(panel.locator("[data-note-assist-result]")).toHaveValue("This is right.");
  expect(assistCalls).toHaveLength(1);
  expect(assistCalls[0]).toMatchObject({ action: "grammar", text: "this are rong", prompt: "" });
  expect(assistCalls[0].before).toContain("Intro line.");
  expect(assistCalls[0].after).toContain("Last line.");

  await panel.locator("[data-note-assist-result]").fill("This is right!");
  await panel.locator("[data-note-assist-apply=\"replace\"]").click();
  await expect(panel).toBeHidden();
  await expect(page.locator("[data-note-markdown]")).toHaveValue("Intro line.\n\nThis is right!\n\nLast line.");
});

test("a typed instruction rewrites the selection and can go below it", async ({ page }) => {
  const assistCalls = [];
  await openNotes(page, { assistCalls, answer: "Bonjour." });
  await selectInNote(page, "Intro line.");
  await page.locator("[data-note-assist-open]").click();
  const panel = page.locator("[data-note-assist-panel]");
  await panel.locator("[data-note-assist-prompt]").fill("translate to French");
  await panel.locator("[data-note-assist-prompt]").press("Enter");
  await expect(panel.locator("[data-note-assist-result]")).toHaveValue("Bonjour.");
  expect(assistCalls[0]).toMatchObject({ action: "custom", prompt: "translate to French", text: "Intro line." });
  await panel.locator("[data-note-assist-apply=\"below\"]").click();
  await expect(page.locator("[data-note-markdown]")).toHaveValue(
    "Intro line.\n\nBonjour.\n\nthis are rong\n\nLast line.");
});

test("without a selection the prompt writes new text at the cursor", async ({ page }) => {
  const assistCalls = [];
  await openNotes(page, { assistCalls, answer: "A new idea." });
  const area = page.locator("[data-note-markdown]");
  await area.evaluate((element) => {
    const at = element.value.indexOf("Last line.");
    element.focus();
    element.setSelectionRange(at, at);
  });
  await area.press("Control+j");
  const panel = page.locator("[data-note-assist-panel]");
  await expect(panel).toBeVisible();
  await expect(panel.locator("[data-note-assist-heading]")).toHaveText("Write with AI");
  await expect(panel.locator("[data-note-assist-actions]")).toBeHidden();
  await expect(panel.locator("[data-note-assist-apply=\"below\"]")).toBeHidden();
  await panel.locator("[data-note-assist-ask]").click();
  await expect(panel.locator("[data-note-notice=\"assist\"]")).toContainText("Type what you want");
  expect(assistCalls).toHaveLength(0);
  await panel.locator("[data-note-assist-prompt]").fill("add an idea");
  await panel.locator("[data-note-assist-ask]").click();
  await expect(panel.locator("[data-note-assist-result]")).toHaveValue("A new idea.");
  expect(assistCalls[0]).toMatchObject({ action: "custom", text: "", prompt: "add an idea" });
  await panel.locator("[data-note-assist-apply=\"replace\"]").click();
  await expect(area).toHaveValue("Intro line.\n\nthis are rong\n\nA new idea.\n\nLast line.");
});

test("the AI buttons stay hidden when the site has no AI provider", async ({ page }) => {
  await openNotes(page, { ready: false });
  await expect(page.locator("[data-note-assist-open]")).toBeHidden();
  await selectInNote(page, "this are rong");
  await expect(page.locator("[data-note-comment-float]")).toBeVisible();
  await expect(page.locator("[data-note-assist-float]")).toBeHidden();
});
