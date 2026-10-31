const fs = require("fs");
const path = require("path");

const source = fs.readFileSync(
  path.join(__dirname, "..", "content", "web", "page.js"), "utf8");
const body = source.slice(source.indexOf("=> {") + 4,
                          source.lastIndexOf("return initCrmPage();"));

function element(name) {
  const base = {
    name, value: "", textContent: "", innerHTML: "", disabled: false, open: true, checked: false,
    hidden: false, length: 0, dataset: {}, style: {}, children: [], closed: 0,
    classes: new Set(["hidden"]),
  };
  base.classList = {
    toggle(cls, on) { if (on === undefined ? !base.classes.has(cls) : on) base.classes.add(cls); else base.classes.delete(cls); },
    add(cls) { base.classes.add(cls); },
    remove(cls) { base.classes.delete(cls); },
    contains(cls) { return base.classes.has(cls); },
  };
  base.close = () => { base.closed += 1; base.open = false; };
  return new Proxy(base, {
    get(target, prop) {
      if (prop in target) return target[prop];
      if (typeof prop === "symbol") return undefined;
      return () => element(`${name}.${String(prop)}`);
    },
    set(target, prop, value) { target[prop] = value; return true; },
  });
}

const elements = new Map();
function $(selector) {
  if (!elements.has(selector)) elements.set(selector, element(selector));
  return elements.get(selector);
}

let replies = [];
const requests = [];
globalThis.fetch = async (url, init = {}) => {
  const method = init.method || "GET";
  requests.push({ url, method, body: init.body ? JSON.parse(init.body) : null });
  if (method === "GET") {
    return { ok: true, status: 200, json: async () => ({ ok: true, organizations: [], contacts: [] }) };
  }
  const next = replies.shift() || { status: 201, json: { ok: true, contact: { id: "c".repeat(32) } } };
  if (next.throws) throw new TypeError("Failed to fetch");
  return { ok: next.status < 300, status: next.status, json: async () => next.json };
};
globalThis.setTimeout = (fn) => { fn(); return 0; };

const page = new Function(
  "$", "$$", "session", "escapeHtml", "relativeTimeLabel",
  `${body}\nreturn { saveCrmContact, openCrmContact, state };`,
)($, () => [], () => ({ name: "Alice" }), (value) => String(value), (value) => String(value));

page.state.crmView = Object.assign(page.state.crmView || {}, { organization: "acme" });

let failures = 0;
function check(condition, what) {
  if (!condition) { failures += 1; console.error(`FAIL ${what}`); }
}

function type(name, email) {
  $("[data-crm-contact-name]").value = name;
  $("[data-crm-contact-email]").value = email;
}

const posts = () => requests.filter((item) => item.method === "POST");
const event = { preventDefault() {} };

(async () => {
  page.openCrmContact(null);
  const first = page.state.crmView.contactRequestId;
  check(/^[0-9a-f]{32}$/.test(first), "an add form gets a request id");

  type("", "");
  await page.saveCrmContact(event);
  check(posts().length === 0, "an empty form is not sent");
  check(!$("[data-crm-contact-error]").classList.contains("hidden"), "an empty form says why inside the dialog");

  type("Ada", "ada@example.com");
  replies = [{ status: 409, json: { error: "contact_email_exists", contact: { id: "a".repeat(32), name: "Ada Lovelace" } } }];
  await page.saveCrmContact(event);
  check(posts().length === 1, "a duplicate is sent once");
  check(posts()[0].body.requestId === first, "the add carries the form's request id");
  check($("[data-crm-contact-error-text]").textContent.startsWith("Ada Lovelace already uses that email address"),
        `the duplicate is named in the dialog: ${$("[data-crm-contact-error-text]").textContent}`);
  check(!$("[data-crm-contact-existing]").classList.contains("hidden"), "the dialog offers to open the existing contact");
  check($("[data-crm-contact-existing]").dataset.crmContactExisting === "a".repeat(32), "the open button names that contact");
  check($("[data-crm-contact-dialog]").closed === 0, "the dialog stays open on a duplicate");

  type("Ada", "ada2@example.com");
  replies = [{ throws: true }, { status: 503, json: { error: "unavailable" } },
             { status: 201, json: { ok: true, contact: { id: "b".repeat(32) } } }];
  await page.saveCrmContact(event);
  check(posts().length === 4, `a failed add is retried: ${posts().length}`);
  check(posts().slice(1).every((item) => item.body.requestId === first), "every retry carries the same request id");
  check($("[data-crm-contact-dialog]").closed === 1, "the dialog closes once the add lands");
  check(page.state.crmView.contactRequestId === "", "a landed add forgets its request id");

  page.openCrmContact(null);
  check(page.state.crmView.contactRequestId !== first, "a new form gets a new request id");
  type("Grace", "grace@example.com");
  replies = [{ status: 201, json: { ok: true, contact: { id: "d".repeat(32) } } }];
  const before = posts().length;
  await Promise.all([page.saveCrmContact(event), page.saveCrmContact(event)]);
  check(posts().length === before + 1, "a double submit sends one add");

  if (failures) process.exit(1);
  console.log("contact save checks passed");
})().catch((error) => { console.error(error); process.exit(1); });
