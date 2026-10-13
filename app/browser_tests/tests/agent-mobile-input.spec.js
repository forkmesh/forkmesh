const { test, expect } = require("@playwright/test");
const fs = require("node:fs");
const path = require("node:path");
const publicDir = path.resolve(__dirname, "../../public");

test.use({ viewport: { width: 390, height: 844 }, isMobile: true, hasTouch: true });

test("mobile terminal choices and typed responses reach the running terminal", async ({ page }) => {
  await page.setContent('<div id="terminal" style="width:360px;height:400px"></div>');
  const source = fs.readFileSync(path.join(publicDir, "agent-terminal.js"), "utf8");
  await page.addScriptTag({ content: source.replace(/^export /gm, "") + `
    window.calls = [];
    window.client = createDesktopControlTerminal({
      surface: document.querySelector("#terminal"), label: "Codex", start: { provider: "codex" },
      connection: { isOpen: true, on() { return () => {}; }, onClose() { return () => {}; },
        async request(method, params) { window.calls.push({method, params}); return {terminal: "run1"}; } },
    });
    window.client.start();
  ` });
  await page.getByRole("button", { name: "ArrowDown", exact: true }).tap();
  await page.getByRole("button", { name: "Enter", exact: true }).tap();
  await page.getByRole("textbox", { name: "Terminal response" }).fill("Proceed with option B");
  await page.getByRole("button", { name: "Send", exact: true }).tap();
  await expect.poll(() => page.evaluate(() => window.calls.filter((call) => call.method === "terminal.input").map((call) => atob(call.params.data)).join(""))).toBe("\x1b[B\rProceed with option B\r");
});

test("mobile transcript formats questions and submits mapped answers", async ({ page }) => {
  await page.setContent('<div id="transcript"></div>');
  const source = fs.readFileSync(path.join(publicDir, "dashboard/js/06-repo-content.js"), "utf8");
  const renderers = source.slice(source.indexOf("  function desktopAgentDataHtml("), source.indexOf("  function renderDesktopAgentDetail(") >= 0 ? source.indexOf("  function renderDesktopAgentDetail(") : source.indexOf("  function desktopAgentDetailHtml("));
  const wire = source.slice(source.indexOf("  function wireDesktopAgentActions("), source.indexOf('    container.querySelectorAll("[data-desktop-agent-open]")')) + "\n  }";
  await page.addScriptTag({ content: `
    const escapeHtml = (value) => String(value).replace(/[&<>"']/g, (char) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[char]));
    const desktopAgentSelected = () => ({localId: 5, desktopNode: "studio"});
    window.calls = [];
    const desktopConnection = () => ({async request(method, params) { window.calls.push({method, params}); }});
    const loadDesktopAgentTranscript = async () => {};
    const patchDesktopAgentDetail = () => {};
    const desktopControlErrorText = (error) => error.message;
    ${renderers}
    ${wire}
    const log = JSON.stringify({type: "assistant", message: {role: "assistant", content: [
      {type: "text", text: "Choose a destination <script>"},
      {type: "tool_use", id: "codex-request-2", name: "AskUserQuestion", input: {questions: [
        {question: "Destination?", options: [{label: "Staging", description: "Preview environment"}, {label: "Production"}]},
        {question: "Name?", options: []}
      ]}}
    ]}});
    document.querySelector("#transcript").innerHTML = desktopAgentTranscriptHtml(log);
    wireDesktopAgentActions({}, document.querySelector("#transcript"));
  ` });
  await expect(page.locator("form").getByText("Preview environment")).toBeVisible();
  await expect(page.getByText("Choose a destination <script>").first()).toBeVisible();
  await page.getByRole("radio", { name: "Staging Preview environment" }).check();
  await page.getByRole("textbox", { name: "Name?" }).fill("Phone choice");
  await page.getByRole("button", { name: "Send answers and continue" }).tap();
  await page.getByText("Event details", { exact: true }).tap();
  await expect(page.locator("details").getByText("codex-request-2", { exact: true })).toBeVisible();
  expect(await page.evaluate(() => window.calls)).toEqual([{method: "agents.answer", params: {id: 5, question: "codex-request-2", answer: "Destination? → Staging\nName? → Phone choice"}}]);
});
