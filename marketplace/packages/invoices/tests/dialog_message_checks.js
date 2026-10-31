const fs = require("fs");
const path = require("path");

const web = path.join(__dirname, "..", "content", "web");
const source = fs.readFileSync(process.env.INVOICES_PAGE_JS || path.join(web, "page.js"), "utf8");
const view = fs.readFileSync(process.env.INVOICES_VIEW_HTML || path.join(web, "view.html"), "utf8");
const body = source.slice(source.indexOf("=> {") + 4, source.lastIndexOf("return init();"));

let failures = 0;
function check(condition, what) {
  if (!condition) { failures += 1; console.error(`FAIL ${what}`); }
}

class FakeClassList {
  constructor(names) { this.names = new Set(names); }
  add(name) { this.names.add(name); }
  remove(name) { this.names.delete(name); }
  contains(name) { return this.names.has(name); }
  toggle(name, force) { const on = force === undefined ? !this.names.has(name) : Boolean(force); if (on) this.names.add(name); else this.names.delete(name); return on; }
}
class FakeElement {
  constructor(name, hidden = false) {
    this.name = name; this.textContent = ""; this.value = ""; this.placeholder = ""; this.disabled = false;
    this.classList = new FakeClassList(hidden ? ["hidden"] : []); this.children = {}; this.open = false;
    this.text = new FakeElement.Text();
    this.listeners = {};
  }
  querySelector(selector) {
    if (selector === "[data-invoice-message-text]") return this.text;
    return this.children[selector] || null;
  }
  querySelectorAll(selector) {
    return selector.split(",").map((part) => this.children[part.trim()]).filter(Boolean);
  }
  showModal() { if (this.open) throw new Error(`${this.name} is already open`); this.open = true; }
  close() { if (!this.open) return; this.open = false; (this.listeners.close || []).forEach((handler) => handler()); }
  addEventListener(type, handler) { (this.listeners[type] ||= []).push(handler); }
  get shown() { return !this.classList.contains("hidden") && this.text.textContent !== ""; }
}
FakeElement.Text = class { constructor() { this.textContent = ""; } };

const dialogs = [...view.matchAll(/<dialog (data-invoice-[a-z-]+)([\s\S]*?)<\/dialog>/g)].map((match) => {
  const dialog = new FakeElement(match[1]);
  for (const kind of ["error", "notice"]) {
    if (match[2].includes(`data-invoice-dialog-${kind} `)) dialog.children[`[data-invoice-dialog-${kind}]`] = new FakeElement(`${match[1]} ${kind}`, true);
  }
  dialog.markup = match[2];
  return dialog;
});
const byDialog = Object.fromEntries(dialogs.map((dialog) => [`[${dialog.name}]`, dialog]));
const elements = { ...byDialog, "[data-invoice-error]": new FakeElement("page error", true), "[data-invoice-notice]": new FakeElement("page notice", true) };
const $ = (selector) => elements[selector] ||= new FakeElement(selector);
const $$ = (selector) => selector === "dialog[open]" ? dialogs.filter((dialog) => dialog.open) : [];

const responses = {};
global.fetch = async (url, options = {}) => {
  const key = `${options.method || "GET"} ${url}`;
  const [status, payload] = responses[key] || [404, { ok: false, error: `no stub for ${key}` }];
  return { ok: status < 400, status, json: async () => payload };
};
global.location = { href: "https://forkmesh.test/dashboard/invoices" };
global.history = { state: null, replaceState() {} };
global.window = {};

const page = new Function(
  "$", "$$", "session", "escapeHtml", "relativeTimeLabel",
  `${body}\nreturn { state, message, openDetail, submitSend, submitPaid, saveEditor, ${body.includes("function showDialog(") ? "showDialog" : "showDialog: null"} };`,
)($, $$, () => ({ sessionToken: "t" }), (value) => String(value), () => "now");

const pageError = elements["[data-invoice-error]"];
const detail = byDialog["[data-invoice-detail]"];
const send = byDialog["[data-invoice-send-dialog]"];
const paid = byDialog["[data-invoice-paid-dialog]"];
const editor = byDialog["[data-invoice-editor]"];
function slot(dialog, kind = "error") { return dialog.children[`[data-invoice-dialog-${kind}]`] || null; }
function reset() {
  dialogs.forEach((dialog) => { dialog.open = false; Object.values(dialog.children).forEach((node) => { node.classList.add("hidden"); node.text.textContent = ""; }); });
  for (const node of [pageError, elements["[data-invoice-notice]"]]) { node.classList.add("hidden"); node.text.textContent = ""; }
}

for (const dialog of dialogs) {
  check(slot(dialog), `${dialog.name} has its own role="alert" error slot`);
  check(/data-invoice-dialog-error role="alert"/.test(dialog.markup), `${dialog.name} error slot is announced`);
  const label = dialog.markup.match(/^ aria-labelledby="([^"]+)"/);
  check(label, `${dialog.name} is named by aria-labelledby`);
  if (label) check(dialog.markup.includes(`id="${label[1]}"`) || source.includes(`id="${label[1]}"`), `${dialog.name} label #${label?.[1]} exists`);
}
check(slot(detail, "notice"), "the invoice detail has a notice slot for copy and payment confirmations");

const invoice = { id: "inv1", number: "INV-0001", status: "sent", currency: "USD", total: 100, balance: 100, items: [], client: { email: "ada@example.com" } };
page.state.organization = "acme"; page.state.invoices = [invoice]; page.state.detailId = "inv1";

(async () => {
  reset(); detail.open = true; send.open = true;
  responses["POST /api/invoices/acme/items/inv1/send"] = [400, { ok: false, error: "invalid_sender" }];
  await page.submitSend({ preventDefault() {} });
  check(send.open, "the email dialog stays open after a failed send");
  check(slot(send)?.shown, "a failed send shows its error inside the email dialog");
  check(/Add a From email in Settings/.test(slot(send)?.text.textContent || ""), "the failed-send error keeps its next step");
  check(!pageError.shown, "a failed send does not write to the page flash hidden behind the modal");

  reset(); detail.open = true; paid.open = true;
  responses["POST /api/invoices/acme/items/inv1/paid"] = [400, { ok: false, error: "amount_too_large" }];
  await page.submitPaid({ preventDefault() {} });
  check(slot(paid)?.shown && !slot(detail)?.shown, "a failed payment shows in the payment dialog, the topmost one");

  reset(); editor.open = true;
  elements["[data-invoice-id]"] = Object.assign(new FakeElement("id"), { value: "" });
  responses["POST /api/invoices/acme/items"] = [400, { ok: false, error: "client_email_invalid" }];
  await page.saveEditor({ preventDefault() {} });
  check(slot(editor)?.shown && /client email invalid/.test(slot(editor).text.textContent), "a failed save shows inside the invoice editor");
  check(!pageError.shown, "a failed save does not write to the page flash");

  reset();
  responses["GET /api/invoices/acme/items/inv1"] = [500, { ok: false, error: "storage_down" }];
  await page.openDetail("inv1");
  check(!detail.open, "a detail that cannot load closes");
  check(pageError.shown && /Could not open the invoice/.test(pageError.text.textContent), "the load error stays visible on the page after the detail closes");

  reset(); page.message("notice", "Invoices saved.");
  check(elements["[data-invoice-notice]"].shown, "with no dialog open, notices still go to the page");

  reset(); send.open = true; page.message("error", "stale");
  send.open = false;
  if (page.showDialog) page.showDialog(send);
  check(!slot(send)?.shown, "reopening a dialog clears the error left from its last attempt");

  if (failures) { console.error(`${failures} invoice dialog message checks failed`); process.exit(1); }
  console.log("invoice dialog message checks passed");
})().catch((error) => { console.error(error); process.exit(1); });
