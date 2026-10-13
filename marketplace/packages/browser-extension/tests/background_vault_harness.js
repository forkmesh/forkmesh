// Drives the extension's real background vault code (vault.js with the real
// crypto and client) under a fake `chrome`, against the real Worker module.
// Started by test_extension_vault.py; exits non-zero on the first failure.
"use strict";
const fs = require("fs"), path = require("path"), vm = require("vm");
const { spawn } = require("child_process");
const readline = require("readline");

const EXTENSION = path.join(__dirname, "..", "content", "extension");
const VAULT_TESTS = path.join(__dirname, "..", "..", "vault", "tests");
const server = spawn(process.env.PYTHON || "python3", [path.join(VAULT_TESTS, "vault_stdio_server.py")],
  { stdio: ["pipe", "pipe", "inherit"] });
const replies = [];
readline.createInterface({ input: server.stdout }).on("line", (line) => replies.shift()(JSON.parse(line)));
const rpc = (message) => new Promise((resolve) => { replies.push(resolve); server.stdin.write(JSON.stringify(message) + "\n"); });

let checks = 0;
function check(name, ok) {
  checks += 1;
  if (!ok) { console.error("FAIL " + name); server.kill(); process.exit(1); }
  console.log("ok   " + name);
}

// ---- a fake browser -------------------------------------------------------------
const browser = { session: {}, alarms: {}, tab: { id: 7, url: "https://registrar.example/login" }, injected: [], account: "alice" };
const alarmListeners = [];
globalThis.self = globalThis;
globalThis.chrome = {
  storage: { session: {
    async get(keys) { return Object.fromEntries(keys.filter((k) => k in browser.session).map((k) => [k, browser.session[k]])); },
    async set(values) { Object.assign(browser.session, JSON.parse(JSON.stringify(values))); },
    async remove(keys) { for (const key of keys) delete browser.session[key]; },
  } },
  alarms: {
    async create(name, info) { browser.alarms[name] = info; },
    async clear(name) { delete browser.alarms[name]; },
    onAlarm: { addListener(listener) { alarmListeners.push(listener); } },
  },
  tabs: { async query() { return [browser.tab]; }, async create() {} },
  scripting: { async executeScript(call) { browser.injected.push(call); return [{ result: browser.pageResult }]; } },
};

function boot() {
  // A service worker start: fresh globals, the same session storage.
  for (const name of ["ForkMeshVaultCrypto", "ForkMeshVaultClient", "ForkMeshVaultBackground"]) delete globalThis[name];
  for (const file of ["vault-crypto.js", "vault-client.js", "vault.js"]) {
    vm.runInThisContext(fs.readFileSync(path.join(EXTENSION, file), "utf8"), { filename: file });
  }
  // Keep the suite quick: the smallest stretch the client accepts.
  const C = globalThis.ForkMeshVaultCrypto, realNewKdf = C.newKdf;
  C.newKdf = () => ({ ...realNewKdf(), ...C.KDF_FLOOR });
  const handlers = {};
  const api = async (apiPath, method = "GET", body = null) => {
    const reply = await rpc({ actor: browser.account, method, path: apiPath, body });
    if (reply.status >= 400 || reply.data.ok === false) {
      throw Object.assign(new Error(reply.data.error), { code: reply.data.error, status: reply.status });
    }
    return reply.data;
  };
  const background = globalThis.ForkMeshVaultBackground.install({
    api, settings: async () => ({ account: browser.account, baseUrl: "https://app.forkmesh.com" }), handlers });
  const send = async (message) => {
    try { return await handlers[message.type](message); }
    catch (error) { return { ok: false, error: error.code || "request_failed" }; }
  };
  return { send, background, C };
}

(async () => {
  let { send, C } = boot();
  check("no vault yet", (await send({ type: "fm-vault-status" })).installed === false);

  // Alice creates her vault and two logins with the same client the page uses.
  const Client = globalThis.ForkMeshVaultClient;
  const page = Client.create({ crypto: C, request: async (p, m = "GET", b = null) => {
    const reply = await rpc({ actor: "alice", method: m, path: p, body: b });
    if (reply.status >= 400) throw Object.assign(new Error(reply.data.error), { code: reply.data.error });
    return reply.data;
  } });
  await page.setUp("alice master password");
  const registrar = await page.save({ title: "Registrar", username: "ops@acme.test", password: "s3cret!", url: "https://registrar.example/login" });
  const bank = await page.save({ title: "Bank", username: "alice", password: "only-mine", url: "https://bank.example" });

  const locked = await send({ type: "fm-vault-status" });
  check("the extension starts locked", locked.installed && !locked.unlocked);
  check("a locked vault lists nothing", (await send({ type: "fm-vault-items", url: browser.tab.url })).error === "vault_locked");
  check("a wrong master password is refused", (await send({ type: "fm-vault-unlock", password: "nope nope nope" })).error === "wrong_password");
  check("and leaves nothing in session storage", Object.keys(browser.session).length === 0);
  check("the master password unlocks", (await send({ type: "fm-vault-unlock", password: "alice master password" })).unlocked === true);
  check("the idle lock is armed", Boolean(browser.alarms["forkmesh-vault-lock"]) && browser.session.vaultExpiresAt > Date.now());

  const listed = await send({ type: "fm-vault-items", url: browser.tab.url });
  check("the login for this site is offered", listed.matches.map((item) => item.title).join() === "Registrar" && listed.host === "registrar.example");
  check("lists never carry a password", !JSON.stringify(listed).includes("s3cret!") && !JSON.stringify(await send({ type: "fm-vault-items", url: browser.tab.url, query: "bank" })).includes("only-mine"));
  check("search finds other logins to copy from", (await send({ type: "fm-vault-items", url: browser.tab.url, query: "bank" })).others[0].id === bank);

  // Filling: only into the login's own host, as Chrome reports the tab.
  browser.pageResult = { filledUsername: true, filledPassword: true };
  const filled = await send({ type: "fm-vault-fill", id: registrar });
  check("fill types into the active tab", filled.ok && browser.injected.length === 1
    && browser.injected[0].target.tabId === 7 && browser.injected[0].args.join("|") === "ops@acme.test|s3cret!|registrar.example");
  const navigatedAway = await (async () => {
    const saved = globalThis.location;
    globalThis.location = { hostname: "evil.example" };
    try { return browser.injected[0].func(...browser.injected[0].args); } finally { globalThis.location = saved; }
  })();
  check("a tab that navigated away after the check is not filled", navigatedAway.filledPassword === false && navigatedAway.filledUsername === false);
  check("fill targets the top frame only", !("allFrames" in browser.injected[0].target) && !("frameIds" in browser.injected[0].target));
  for (const url of ["https://evil.example/login", "https://registrar.example.evil.test/", "http://registrar.example/login",
    "https://sub.registrar.example/", "https://notregistrar.example/", "chrome://extensions"]) {
    browser.tab.url = url;
    const refused = await send({ type: "fm-vault-fill", id: registrar });
    check(`no fill on ${url}`, refused.error === "site_mismatch" && browser.injected.length === 1);
  }
  browser.tab.url = "https://www.registrar.example/account";
  check("www. is the same site", (await send({ type: "fm-vault-fill", id: registrar })).ok === true);
  check("copy hands over one field on request", (await send({ type: "fm-vault-copy", id: bank, field: "password" })).value === "only-mine");
  check("copy refuses other fields", (await send({ type: "fm-vault-copy", id: bank, field: "notes" })).error === "item_not_found");

  // Saving the login typed into a page.
  browser.tab.url = "https://shop.example/signin?next=/cart";
  browser.pageResult = { found: true, username: "alice@shop", password: "typed-pw", title: "Sign in - Shop" };
  const captured = await send({ type: "fm-vault-capture" });
  check("capture reads the page's login", captured.login.url === "https://shop.example" && captured.login.password === "typed-pw");
  await send({ type: "fm-vault-save", fields: captured.login });
  check("the saved login is offered on that site", (await send({ type: "fm-vault-items", url: browser.tab.url })).matches[0].username === "alice@shop");
  browser.tab.url = "http://shop.example/signin";
  check("nothing is captured from an insecure page", (await send({ type: "fm-vault-capture" })).error === "page_not_secure");

  // Chrome stops the worker; the next message re-enters from session storage.
  ({ send, C } = boot());
  browser.tab.url = "https://registrar.example/login";
  const resumed = await send({ type: "fm-vault-items", url: browser.tab.url });
  check("a restarted worker resumes the unlocked vault", resumed.ok && resumed.matches[0].title === "Registrar");

  // The idle alarm, an expired session, Lock, and a changed account all lock.
  alarmListeners[alarmListeners.length - 1]({ name: "forkmesh-vault-lock" });
  await new Promise((resolve) => setTimeout(resolve, 20));
  check("the idle alarm locks and clears the keyset", !browser.session.vaultKeyset && (await send({ type: "fm-vault-status" })).unlocked === false);
  await send({ type: "fm-vault-unlock", password: "alice master password" });
  browser.session.vaultExpiresAt = Date.now() - 1;
  ({ send } = boot());
  check("an expired session is not resumed", (await send({ type: "fm-vault-status" })).unlocked === false && !browser.session.vaultKeyset);
  await send({ type: "fm-vault-unlock", password: "alice master password" });
  browser.account = "bob";
  check("another account never inherits an unlocked vault", (await send({ type: "fm-vault-status" })).unlocked === false && !browser.session.vaultKeyset);
  browser.account = "alice";
  await send({ type: "fm-vault-unlock", password: "alice master password" });
  check("Lock forgets the keyset", (await send({ type: "fm-vault-lock" })).unlocked === false && Object.keys(browser.session).length === 0);

  const generated = await send({ type: "fm-vault-generate", length: 24 });
  check("the generator answers without an unlocked vault", generated.password.length === 24);

  console.log(`${checks} checks passed`);
  server.stdin.end();
})().catch((error) => { console.error(error); server.kill(); process.exit(1); });
