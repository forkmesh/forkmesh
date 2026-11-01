const { test, expect } = require("@playwright/test");
const path = require("node:path");

const NOTES_PAGE_PATH = path.resolve(__dirname, "..", "..", "public", "dashboard", "notes", "index.html");
const NOTE_ID = "5".repeat(32);
const SVG = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 40 20"><rect width="40" height="20" fill="#0969da"/></svg>';
const MARKDOWN = [
  "<!-- page: size=letter; cover=yes -->",
  "",
  "# Summary",
  "",
  "Intro text",
  "",
  "```svg Figure 1. Architecture",
  SVG,
  "```",
  "",
  "<!-- pagebreak -->",
  "",
  "## Detail",
].join("\n");

async function openNotes(page, { role = "owner", calls = [] } = {}) {
  const note = { id: NOTE_ID, title: "Project Proposal", emoji: "📄", markdown: MARKDOWN, version: 3, role,
    visibility: "private", updatedAt: Date.now(), shares: [], links: [] };
  await page.addInitScript(() => {
    localStorage.setItem("forkmesh.session", JSON.stringify({
      nodeName: "alice", email: "alice@example.test", emailVerified: true,
      sessionToken: "test-session", status: "active",
    }));
    localStorage.setItem("forkmesh.notes.activity", "closed");
    window.ForkMeshDashboard = {
      _registrations: [["notes", ({ builtinPage }) => builtinPage("notes")]],
      registerPage(pageId, initializer) { this._registrations.push([pageId, initializer]); },
    };
    class FakeSocket {
      constructor() { this.readyState = 0; setTimeout(() => { this.readyState = 1; this.onopen?.(); }, 0); }
      send() {}
      close() { this.readyState = 3; }
    }
    FakeSocket.CONNECTING = 0; FakeSocket.OPEN = 1; FakeSocket.CLOSING = 2; FakeSocket.CLOSED = 3;
    window.WebSocket = FakeSocket;
  });
  await page.route("https://cdn.tailwindcss.com/**", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.tailwind = {};" }));
  await page.route("https://cdn.jsdelivr.net/npm/lucide@0.468.0/dist/umd/lucide.min.js", (route) =>
    route.fulfill({ contentType: "text/javascript", body: "window.lucide = { createIcons() {} };" }));
  await page.route(/\/dashboard\/notes(\?.*)?$/, (route) => route.fulfill({
    path: NOTES_PAGE_PATH, contentType: "text/html; charset=utf-8",
  }));
  await page.route("**/api/**", async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    const method = request.method();
    const body = request.postDataJSON();
    calls.push({ method, path: url.pathname, body });
    let answer = { ok: true };
    if (url.pathname === "/api/notes") {
      const { markdown, ...summary } = note;
      answer = { ok: true, notes: [summary] };
    } else if (url.pathname === "/api/notes/pages") {
      answer = { ok: true, pages: [] };
    } else if (url.pathname === `/api/notes/${NOTE_ID}`) {
      if (method === "PATCH") Object.assign(note, { title: body.title, markdown: body.markdown, version: note.version + 1 });
      answer = { ok: true, note };
    } else if (url.pathname.endsWith("/activity")) {
      answer = { ok: true, counts: {}, events: [] };
    } else if (url.pathname.endsWith("/comments")) {
      answer = { ok: true, comments: [], people: [], canResolve: true };
    }
    return route.fulfill({ contentType: "application/json; charset=utf-8", body: JSON.stringify(answer) });
  });
  await page.goto("/dashboard/notes");
  await expect(page.locator("[data-note-title]")).toHaveValue("Project Proposal");
  return note;
}

test("the preview draws svg blocks as figures, shows page breaks and hides the layout line", async ({ page }) => {
  await openNotes(page);
  const preview = page.locator("[data-note-preview]");
  const figure = preview.locator("figure.note-figure");
  await expect(figure).toHaveCount(1);
  await expect(figure.locator("img")).toHaveAttribute("src", /^data:image\/svg\+xml;base64,/);
  await expect(figure.locator("figcaption")).toHaveText("Figure 1. Architecture");
  expect(await figure.locator("img").evaluate((img) => img.decode().then(() => img.naturalWidth > 0))).toBe(true);
  await expect(preview.locator(".note-page-break")).toHaveText("Page break");
  await expect(preview).not.toContainText("size=letter");
  await expect(preview.locator("svg")).toHaveCount(0);
});

test("Export PDF opens the layout sheet, saves the layout and prints a paginated document", async ({ page }) => {
  const calls = [];
  const note = await openNotes(page, { calls });
  await page.locator("[data-note-menu-open]").click();
  await page.locator("[data-note-export-pdf]").click();
  const dialog = page.locator("[data-note-pdf-dialog]");
  await expect(dialog).toBeVisible();
  await expect(dialog.locator('[data-note-pdf-field="size"]')).toHaveValue("letter");
  await expect(dialog.locator('[data-note-pdf-field="cover"]')).toBeChecked();
  await expect(dialog.locator('[data-note-pdf-field="contents"]')).not.toBeChecked();
  await expect(dialog.locator('[data-note-pdf-field="numbers"]')).toBeChecked();
  await expect(dialog.locator("[data-note-pdf-save]")).toBeChecked();
  await dialog.locator('[data-note-pdf-field="orientation"]').selectOption("landscape");
  await dialog.locator('[data-note-pdf-field="contents"]').check();
  await dialog.locator("[data-note-pdf-confirm]").click();
  await expect(dialog).toBeHidden();

  const line = "<!-- page: size=letter; orientation=landscape; margins=normal; cover=yes; contents=yes; numbers=yes -->";
  await expect(page.locator("[data-note-markdown]")).toHaveValue(new RegExp(`^${line.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}\\n\\n# Summary`));
  await expect.poll(() => note.markdown.startsWith(line)).toBe(true);
  expect(calls.filter((call) => call.method === "PATCH").at(-1).body.baseVersion).toBe(3);

  const frame = page.locator("iframe[data-note-print-frame]");
  await expect(frame).toHaveCount(1);
  await expect(frame).toHaveAttribute("sandbox", "allow-same-origin allow-modals");
  const srcdoc = await frame.getAttribute("srcdoc");
  expect(srcdoc).toContain("@page { size: 279.4mm 215.9mm; margin: 25.4mm;");
  expect(srcdoc).toContain('<section class="note-cover"><div class="note-cover-emoji">📄</div><h1>Project Proposal</h1>');
  expect(srcdoc).toContain('<a href="#note-summary">Summary</a>');
  expect(srcdoc).toContain('<a href="#note-detail">Detail</a>');
  expect(srcdoc).toContain("<title>project-proposal</title>");
  expect(srcdoc).not.toContain("<script");
});

test("a viewer can export with other settings but cannot save the layout", async ({ page }) => {
  const calls = [];
  await openNotes(page, { role: "viewer", calls });
  await page.locator("[data-note-menu-open]").click();
  await page.locator("[data-note-export-pdf]").click();
  const dialog = page.locator("[data-note-pdf-dialog]");
  await expect(dialog.locator("[data-note-pdf-save-row]")).toBeHidden();
  await dialog.locator('[data-note-pdf-field="size"]').selectOption("a4");
  await dialog.locator("[data-note-pdf-confirm]").click();
  await expect(page.locator("iframe[data-note-print-frame]")).toHaveAttribute("srcdoc", /size: 210mm 297mm/);
  expect(calls.filter((call) => call.method === "PATCH")).toHaveLength(0);
  await expect(page.locator("[data-note-markdown]")).toHaveValue(/^<!-- page: size=letter; cover=yes -->/);
});
