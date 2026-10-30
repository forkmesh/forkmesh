"use strict";

const fs = require("fs");
const vm = require("vm");

const [chunkPath, fixturePath, chunkGlobal, mode] = process.argv.slice(2);
const fixture = JSON.parse(fs.readFileSync(fixturePath, "utf8"));
const GB = 1024 ** 3;

const elements = new Map();
function element(selector) {
  const base = {
    selector,
    innerHTML: "",
    textContent: "",
    value: "",
    hidden: false,
    disabled: false,
    checked: false,
    style: {},
    dataset: {},
    listeners: {},
    classList: { add() {}, remove() {}, toggle() {}, contains() { return false; } },
    setAttribute(name, value) { this[name] = String(value); },
    getAttribute(name) { return this[name] ?? null; },
    removeAttribute() {},
    toggleAttribute() {},
    addEventListener(type, listener) { (this.listeners[type] ||= []).push(listener); },
    removeEventListener() {},
    querySelector() { return null; },
    querySelectorAll() { return []; },
    closest() { return null; },
    contains() { return false; },
    getBoundingClientRect() { return { left: 0, top: 0, width: 800, height: 500, right: 800, bottom: 500 }; },
    focus() {},
    blur() {},
    select() {},
    appendChild() {},
    remove() {},
  };
  return new Proxy(base, {
    get(target, prop) {
      if (prop in target) return target[prop];
      if (typeof prop === "symbol") return undefined;
      return () => undefined;
    },
  });
}
function $(selector) {
  if (!elements.has(selector)) elements.set(selector, element(selector));
  return elements.get(selector);
}

globalThis.window = globalThis;
globalThis.document = { activeElement: null, addEventListener() {}, removeEventListener() {}, createElement: element, body: element("body") };
globalThis.location = { href: "https://app.example.com/dashboard/workflows", search: "", pathname: "/dashboard/workflows" };
globalThis.history = { state: null, replaceState() {} };
globalThis.requestAnimationFrame = (fn) => setTimeout(fn, 0);
globalThis.matchMedia = () => ({ matches: false, addEventListener() {} });
globalThis.addEventListener = () => {};
globalThis.confirm = () => true;
globalThis.CSS = { escape: (value) => String(value) };

const workflow = fixture.workflow;
const recorded = [];
let runs = fixture.runs;
let railWhileRunning = "";
let canvasWhileRunning = "";
let releaseStep = null;

async function fetchJson(path) {
  if (path === "/api/workflows/catalog") return fixture.catalog;
  if (path === "/api/workflows") return { workflows: [fixture.summary] };
  if (path === "/api/workflows/credentials") return { credentials: [] };
  if (path === `/api/workflows/${workflow.id}`) return { workflow };
  if (path === `/api/workflows/${workflow.id}/runs`) return { runs };
  if (path === "/api/workflows/desktop-script") return fixture.script;
  return {};
}

globalThis.fetch = async (path, init) => {
  const body = JSON.parse(init.body || "{}");
  if (path === `/api/workflows/${workflow.id}/record`) {
    recorded.push(body);
    const failed = Object.values(body.nodes).some((entry) => entry.status === "error");
    const run = {
      id: "f".repeat(32), trigger: "desktop", status: failed ? "error" : "success", error: "",
      startedAt: body.startedAt, finishedAt: body.finishedAt, nodes: body.nodes,
      output: { text: "Freed 2.0 GB · 50.0 GB free", metrics: [{ label: "Cleaned", value: "2.0 GB" }, { label: "Free", value: "50.0 GB" }], freedBytes: 2 * GB },
    };
    runs = [{ id: run.id, trigger: run.trigger, status: run.status, startedAt: run.startedAt, finishedAt: run.finishedAt, output: run.output }, ...runs];
    return { ok: true, status: 200, json: async () => ({ ok: true, run }) };
  }
  return { ok: false, status: 404, json: async () => ({ ok: false, error: "not_found" }) };
};

const calls = [];
const prompts = [];
const connection = {
  async request(method, params) {
    calls.push([method, params.action, params.step || "", params.token || ""]);
    if (method === "diag.prepare") return { token: `tok-${params.step}` };
    if (mode === "fail" && params.step === "tests") {
      const error = new Error("busy");
      error.code = "busy";
      throw error;
    }
    if (params.step === "agents" && mode !== "fail") {
      await new Promise((resolve) => { releaseStep = resolve; });
    }
    return {
      step: params.step, ms: 5, items: params.step === "agents" ? 3 : 0,
      freedBytes: params.step === "agents" ? 2 * GB : 0,
      freeBytes: 50 * GB, totalBytes: 400 * GB, lines: [`ran ${params.step}`],
    };
  },
};
const hub = { async desktops() { return [{ node: "desk" }]; }, async connect() { return connection; } };

const state = {
  session: { nodeName: "alice", kind: "user", sessionToken: "token", email: "a@example.com" },
  workflowsView: {
    catalog: null, workflows: [], credentials: [], current: null, dirty: false, selected: "", selectedEdge: -1,
    run: null, runs: [], runsOpen: false, loading: true, busy: false, notice: "", noticeTone: "good", zoom: 1,
    panX: 80, panY: 80, paletteQuery: "", paletteCategory: "", paletteAfter: null, credentialIntegration: "",
    lastField: null, mode: "automations", templates: [], repo: null, repoFiles: null, repoOptions: [], repoLoading: false,
  },
};
const scope = {
  $, $$: () => [], blobText: async () => "", desktopControlErrorText: (error) => String(error?.message || ""),
  desktopControlHub: async () => hub, escapeHtml: (value) => String(value ?? "").replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`),
  fetchJson, fetchRepoJson: async () => ({}), formatTimeAgo: () => "an hour ago", state,
  sendTextToPrompt: (text) => { prompts.push(text); return true; },
  platformName: () => "ForkMesh", desktopBrand: () => "ForkMesh ",
};

vm.runInThisContext(fs.readFileSync(chunkPath, "utf8"));
const api = window[chunkGlobal].workflows(scope);

const tick = () => new Promise((resolve) => setTimeout(resolve, 0));
async function waitFor(check) {
  for (let i = 0; i < 400; i += 1) {
    if (check()) return;
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
  throw new Error("timed out waiting");
}

(async () => {
  api.initWorkflowsPage();
  await waitFor(() => state.workflowsView.current?.id === workflow.id && state.workflowsView.runs.length === runs.length);
  const app = $("[data-wf-app]");
  if (mode === "details") {
    const panel = $("[data-wf-panel-body]");
    const press = async (attr, value) => {
      const key = attr.replace(/^data-/, "").replace(/-([a-z])/g, (_all, letter) => letter.toUpperCase());
      const target = { closest: (selector) => (selector === `[${attr}]` ? { dataset: { [key]: value } } : null) };
      for (const listener of panel.listeners.click) await listener({ target, preventDefault() {} });
      await tick();
    };
    await press("data-wf-select-node", "agents");
    const panelHtml = panel.innerHTML;
    await press("data-wf-view-script", "find_candidates");
    await waitFor(() => !$("[data-wf-dialog]").hidden);
    process.stdout.write(JSON.stringify({
      panelHtml,
      canvasHtml: $("[data-wf-nodes]").innerHTML,
      dialogTitle: $("[data-wf-dialog-title]").textContent,
      dialogHtml: $("[data-wf-dialog-body]").innerHTML,
    }));
    return;
  }
  const click = { target: { closest: (selector) => (selector === "[data-wf-run]" ? {} : null) } };
  for (const listener of app.listeners.click) listener(click);
  if (mode !== "fail") {
    await waitFor(() => releaseStep !== null);
    await tick();
    await new Promise((resolve) => setTimeout(resolve, 300));
    railWhileRunning = $("[data-wf-rail]").innerHTML;
    canvasWhileRunning = $("[data-wf-nodes]").innerHTML;
    releaseStep();
  }
  await waitFor(() => recorded.length === 1 && !state.workflowsView.busy && !state.workflowsView.live);
  await tick();
  const railAfter = $("[data-wf-rail]").innerHTML;
  const panelAfter = $("[data-wf-panel-body]").innerHTML;
  const noticeAfter = state.workflowsView.notice;
  const send = { target: { closest: (selector) => (selector === "[data-wf-send-failure]" ? {} : null) } };
  for (const listener of app.listeners.click) listener(send);
  await tick();
  process.stdout.write(JSON.stringify({
    calls,
    recorded: recorded[0],
    railWhileRunning,
    canvasWhileRunning,
    railAfter,
    panelAfter,
    prompts,
    sentNotice: state.workflowsView.notice,
    railHidden: $("[data-wf-rail]").hidden,
    runsHtml: $("[data-wf-runs]").innerHTML,
    runsHidden: $("[data-wf-runs]").hidden,
    notice: noticeAfter,
    timer: state.workflowsView.railTimer,
  }));
})().catch((error) => {
  process.stderr.write(String(error?.stack || error));
  process.exit(1);
});
