"use strict";
const fs = require("fs"), path = require("path"), vm = require("vm");
const { spawn } = require("child_process");
const readline = require("readline");

const EXTENSION = path.join(__dirname, "..", "content", "extension");
const server = spawn(process.env.PYTHON || "python3", [path.join(__dirname, "crm_stdio_server.py")],
  { stdio: ["pipe", "pipe", "inherit"] });
const replies = [];
readline.createInterface({ input: server.stdout }).on("line", (line) => replies.shift()(JSON.parse(line)));
const rpc = (message) => new Promise((resolve) => { replies.push(resolve); server.stdin.write(JSON.stringify(message) + "\n"); });

let checks = 0;
function check(name, ok, detail = "") {
  checks += 1;
  if (!ok) { console.error(`FAIL ${name} ${detail}`); server.kill(); process.exit(1); }
  console.log("ok   " + name);
}

const clone = (value) => JSON.parse(JSON.stringify(value));
const area = (store) => ({
  async get(keys) { return Object.fromEntries(keys.filter((key) => key in store).map((key) => [key, clone(store[key])])); },
  async set(values) { Object.assign(store, clone(values)); },
  async remove(keys) { for (const key of [].concat(keys)) delete store[key]; },
});
const local = { sessionToken: "token" }, session = {};
const fetched = [], created = [], notes = [], focused = [];
let actor = "charlie";
let listener = null;
const noop = { addListener() {} };
const chrome = {
  runtime: {
    id: "ext", getURL: (suffix) => `chrome-extension://ext/${suffix}`,
    onMessage: { addListener(callback) { listener = callback; } },
    onInstalled: noop, onStartup: noop,
  },
  permissions: { onRemoved: noop, contains: async () => false },
  scripting: { getRegisteredContentScripts: async () => [], registerContentScripts: async () => {}, unregisterContentScripts: async () => {} },
  storage: { local: area(local), session: area(session) },
  tabs: {
    async create(options) { created.push(options.url); return { id: 40 + created.length, windowId: 1 }; },
    async sendMessage(tabId, message) { notes.push({ tabId, ...clone(message) }); },
    async get(tabId) { return { id: tabId, windowId: 2 }; },
    async update(tabId, options) { focused.push({ tabId, ...options }); },
  },
  windows: { async update(windowId, options) { focused.push({ windowId, ...options }); } },
  alarms: { onAlarm: noop, async create() {}, async clear() {} },
};
const context = {
  chrome, URL, URLSearchParams, console, setTimeout, Promise, crypto: globalThis.crypto,
  importScripts() {},
  ForkMeshVaultBackground: { install: () => ({ forget: async () => {} }) },
  ForkMeshRepoBackground: { install: () => {} },
  async fetch(url, options) {
    const target = new URL(url);
    const body = options.body === undefined ? null : JSON.parse(options.body);
    fetched.push({ method: options.method, path: target.pathname, body });
    const reply = await rpc({ actor, method: options.method, path: target.pathname + target.search, body });
    return { ok: reply.status < 400, status: reply.status, json: async () => reply.data };
  },
};
context.self = context;
vm.createContext(context);
vm.runInContext(fs.readFileSync(path.join(EXTENSION, "background.js"), "utf8"), context, { filename: "background.js" });

const CRM_PAGE = { id: "ext", url: "https://app.forkmesh.com/dashboard/crm?org=acme", tab: { id: 3 } };
const MARKETING = { id: "ext", url: "https://app.forkmesh.com/dashboard/marketing?org=acme", tab: { id: 4 } };
const CONSOLE = { id: "ext", url: "https://api-console.zoho.eu/client/1000.SELF", tab: { id: 9 } };
const MAIL = { id: "ext", url: "https://mail.zoho.com/zm/", tab: { id: 7 } };
const message = (request, sender) => new Promise((resolve) => {
  if (!listener(request, sender, resolve)) setImmediate(() => resolve(undefined));
});
const CLIENT = { clientId: "1000.ABCDEFGHIJKLMNOPQRSTUVWXYZ0123", clientSecret: "0123456789abcdef0123456789abcdef0123456789" };
const BAD = "1000.0123456789abcdef0123456789abcdef.0123456789abcdef0123456789abcdef";
const BAD2 = "1000.1123456789abcdef0123456789abcdef.0123456789abcdef0123456789abcdef";
const GOOD = "1000.good56789abcdef0123456789abcdef.fedcba9876543210fedcba9876543210";
const posts = () => fetched.filter((item) => item.path === "/api/crm/acme/zoho/exchange" && item.method === "POST");

(async () => {
  for (const [sender, name] of [[MAIL, "Zoho Mail"], [MARKETING, "Marketing"], [CONSOLE, "the API Console"]]) {
    const reply = await message({ type: "fm-zoho-setup-start", organization: "acme" }, sender);
    check(`${name} cannot start the Zoho setup`, reply.error === "forbidden_sender");
  }
  let reply = await message({ type: "fm-zoho-setup-start", organization: "acme" }, CRM_PAGE);
  check("a member who cannot manage settings is refused", reply.error === "organization_admin_required" && !created.length, JSON.stringify(reply));

  actor = "alice";
  reply = await message({ type: "fm-zoho-setup-start", organization: "acme" }, CRM_PAGE);
  check("an owner's one click opens the Zoho API Console", reply.ok && created[0] === "https://api-console.zoho.com/", JSON.stringify(reply));
  check("the setup starts guiding with nothing read", reply.setup.active && reply.setup.state === "guiding"
    && Object.values(reply.setup.keys).every((value) => value === ""));
  check("the CRM tab hears about it", notes.some((note) => note.tabId === 3 && note.type === "fm-zoho-setup-update"));

  for (const [sender, name] of [[MAIL, "Zoho Mail"], [CRM_PAGE, "the CRM page"]]) {
    reply = await message({ type: "fm-zoho-setup-keys", keys: CLIENT }, sender);
    check(`${name} cannot hand over the Self Client`, reply.error === "forbidden_sender");
  }
  reply = await message({ type: "fm-zoho-setup-state" }, MAIL);
  check("Zoho Mail cannot read the setup", reply.error === "forbidden_sender");

  reply = await message({ type: "fm-zoho-setup-state" }, CONSOLE);
  check("the console tab reads the setup and its data center", reply.ok && reply.setup.organization === "acme" && reply.setup.region === "eu");
  reply = await message({ type: "fm-zoho-setup-keys", keys: { ...CLIENT, code: "not a code" } }, CONSOLE);
  check("a well-formed client is kept and a malformed code dropped", reply.setup.keys.clientId === "…0123"
    && reply.setup.keys.clientSecret === "…6789" && reply.setup.keys.code === "" && !posts().length);
  check("replies never carry the client secret", !JSON.stringify(reply).includes(CLIENT.clientSecret) && !JSON.stringify(notes).includes(CLIENT.clientSecret));
  check("the console tab hears changes", notes.some((note) => note.tabId === 9 && note.type === "fm-zoho-setup-changed"));

  reply = await message({ type: "fm-zoho-setup-keys", keys: { code: BAD } }, CONSOLE);
  check("the code completes the set and is sent once with the console's data center", posts().length === 1
    && JSON.stringify(posts()[0].body) === JSON.stringify({ ...CLIENT, code: BAD, region: "eu" }), JSON.stringify(posts()));
  check("a first refused code goes back to making a new one", reply.setup.state === "guiding" && reply.setup.rejected === 1
    && reply.setup.keys.code === "" && reply.setup.keys.clientId === "…0123", JSON.stringify(reply.setup));

  reply = await message({ type: "fm-zoho-setup-keys", keys: { code: BAD } }, CONSOLE);
  check("the same code read again is not sent again", posts().length === 1 && reply.setup.state === "guiding");

  reply = await message({ type: "fm-zoho-setup-keys", keys: { code: GOOD } }, CONSOLE);
  check("a fresh code connects Zoho Mail", reply.setup.state === "done" && reply.setup.accountId === "4242"
    && reply.setup.region === "eu", JSON.stringify(reply.setup));
  check("everything read is forgotten once saved", Object.values(reply.setup.keys).every((value) => value === "")
    && !JSON.stringify(session).includes(CLIENT.clientSecret) && !JSON.stringify(session).includes(GOOD));
  check("the CRM tab is brought back", focused.some((item) => item.tabId === 3 && item.active));
  const done = notes.filter((note) => note.tabId === 3 && note.setup?.state === "done");
  check("the CRM tab hears the account it is connected to", done.length && done.at(-1).setup.accountId === "4242");
  const saved = await rpc({ actor: "alice", method: "GET", path: "/api/crm/acme/settings", body: null });
  check("the CRM has Zoho Mail connected", saved.data.settings.zoho.connected && saved.data.settings.zoho.accountId === "4242"
    && saved.data.settings.zoho.region === "eu", JSON.stringify(saved.data.settings.zoho));
  reply = await message({ type: "fm-zoho-setup-keys", keys: { code: BAD2 } }, CONSOLE);
  check("a finished setup sends nothing more", posts().length === 2 && reply.setup.state === "done");
  reply = await message({ type: "fm-zoho-setup-back" }, CONSOLE);
  check("Back to the CRM ends a finished setup", reply.ok && !session.zohoSetup);

  reply = await message({ type: "fm-zoho-setup-start", organization: "acme" }, CRM_PAGE);
  check("a connected organization opens the console of its data center", created.at(-1) === "https://api-console.zoho.eu/");
  await message({ type: "fm-zoho-setup-keys", keys: { ...CLIENT, code: BAD } }, CONSOLE);
  reply = await message({ type: "fm-zoho-setup-keys", keys: { code: BAD2 } }, CONSOLE);
  check("a second refused code stops with Zoho's reason", reply.setup.state === "error" && reply.setup.error === "zoho_code_rejected"
    && reply.setup.detail === "invalid_code", JSON.stringify(reply.setup));
  reply = await message({ type: "fm-zoho-setup-restart" }, CONSOLE);
  check("Try again starts over with nothing kept", reply.setup.state === "guiding" && reply.setup.rejected === 0
    && Object.values(reply.setup.keys).every((value) => value === ""));
  reply = await message({ type: "fm-zoho-setup-stop" }, CONSOLE);
  check("Stop ends the setup", reply.ok && !reply.setup.active && !session.zohoSetup);
  check("the CRM tab hears it was stopped", notes.some((note) => note.tabId === 3 && note.setup?.state === "stopped"));
  reply = await message({ type: "fm-zoho-setup-keys", keys: CLIENT }, CONSOLE);
  check("values after Stop are refused", reply.error === "zoho_setup_expired");

  server.kill();
  console.log(`${checks} checks passed`);
})().catch((error) => { console.error(error); server.kill(); process.exit(1); });
