"use strict";
const fs = require("fs"), path = require("path"), vm = require("vm");
const { spawn } = require("child_process");
const readline = require("readline");

const EXTENSION = path.join(__dirname, "..", "content", "extension");
const server = spawn(process.env.PYTHON || "python3", [path.join(__dirname, "marketing_stdio_server.py")],
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

const DASHBOARD = { id: "ext", url: "https://app.forkmesh.com/dashboard/marketing?org=acme", tab: { id: 3 } };
const PORTAL = { id: "ext", url: "https://console.x.com/apps/123/keys", tab: { id: 9 } };
const X_PAGE = { id: "ext", url: "https://x.com/forkmesh", tab: { id: 7 } };
const message = (request, sender) => new Promise((resolve) => {
  if (!listener(request, sender, resolve)) setImmediate(() => resolve(undefined));
});
const KEYS = {
  apiKey: "A".repeat(20) + "apiK5", apiSecret: "S".repeat(46) + "sec1",
  accessToken: "123456789-" + "T".repeat(26) + "tok1", accessSecret: "Z".repeat(41) + "acc1",
};
const SECOND = { accessToken: "123456789-" + "U".repeat(26) + "tok2", accessSecret: "Y".repeat(41) + "acc2" };
const posts = () => fetched.filter((item) => item.path === "/api/marketing/acme/x-connection" && item.method === "POST");

(async () => {
  let reply = await message({ type: "fm-x-setup-start", organization: "acme" }, X_PAGE);
  check("a page other than Marketing cannot start the X setup", reply.error === "forbidden_sender");
  reply = await message({ type: "fm-x-setup-start", organization: "acme" }, DASHBOARD);
  check("a member who cannot manage settings is refused", reply.error === "organization_admin_required" && !created.length, JSON.stringify(reply));

  actor = "alice";
  reply = await message({ type: "fm-x-setup-start", organization: "acme" }, DASHBOARD);
  check("an owner's start opens X's developer console", reply.ok && created[0] === "https://console.x.com/", JSON.stringify(reply));
  check("the setup starts guiding with no keys", reply.setup.active && reply.setup.state === "guiding" && !reply.setup.permission
    && Object.values(reply.setup.keys).every((value) => value === ""));
  check("the Marketing tab hears about it", notes.some((note) => note.tabId === 3 && note.type === "fm-x-setup-update"));

  for (const [sender, name] of [[X_PAGE, "x.com"], [DASHBOARD, "the dashboard"]]) {
    reply = await message({ type: "fm-x-setup-keys", keys: KEYS }, sender);
    check(`${name} cannot hand over keys`, reply.error === "forbidden_sender");
  }
  reply = await message({ type: "fm-x-setup-state" }, X_PAGE);
  check("x.com cannot read the setup", reply.error === "forbidden_sender");

  reply = await message({ type: "fm-x-setup-state" }, PORTAL);
  check("the console tab reads the setup", reply.ok && reply.setup.organization === "acme");
  reply = await message({ type: "fm-x-setup-keys", keys: { apiKey: KEYS.apiKey, apiSecret: KEYS.apiSecret, accessToken: "not a token" } }, PORTAL);
  check("well-formed keys are kept and malformed ones dropped", reply.setup.keys.apiKey === "…piK5"
    && reply.setup.keys.apiSecret === "…sec1" && reply.setup.keys.accessToken === "");
  check("replies never carry a full key", !JSON.stringify(reply).includes(KEYS.apiSecret) && !JSON.stringify(notes).includes(KEYS.apiSecret));
  check("the console tab hears changes", notes.some((note) => note.tabId === 9 && note.type === "fm-x-setup-changed"));

  reply = await message({ type: "fm-x-setup-keys", keys: { accessToken: KEYS.accessToken, accessSecret: KEYS.accessSecret } }, PORTAL);
  check("four keys made before Read and write are not sent", reply.setup.state === "guiding" && !posts().length);
  reply = await message({ type: "fm-x-setup-mark", permission: true }, PORTAL);
  check("setting Read and write drops the earlier access token", reply.setup.permission
    && reply.setup.keys.accessToken === "" && reply.setup.keys.apiKey === "…piK5");

  await rpc({ control: "outbound", url: "https://api.x.com/2/users/me", status: 401, data: { title: "Unauthorized" } });
  reply = await message({ type: "fm-x-setup-keys", keys: { accessToken: KEYS.accessToken, accessSecret: KEYS.accessSecret } }, PORTAL);
  check("the fourth key after Read and write sends all four", posts().length === 1
    && JSON.stringify(posts()[0].body) === JSON.stringify(KEYS), JSON.stringify(posts()));
  check("X's refusal is reported and the access token dropped", reply.setup.state === "error"
    && reply.setup.error === "x_not_authorized" && reply.setup.keys.accessToken === "" && reply.setup.keys.apiKey === "…piK5", JSON.stringify(reply.setup));

  await rpc({ control: "outbound", url: "https://api.x.com/2/users/me", data: { data: { id: "9", username: "forkmesh", name: "ForkMesh" } } });
  reply = await message({ type: "fm-x-setup-keys", keys: SECOND }, PORTAL);
  check("a new access token connects the account", reply.setup.state === "done" && reply.setup.username === "forkmesh", JSON.stringify(reply.setup));
  check("the keys are forgotten once saved", Object.values(reply.setup.keys).every((value) => value === "")
    && !JSON.stringify(session).includes(KEYS.apiSecret));
  const done = notes.filter((note) => note.tabId === 3 && note.setup?.state === "done");
  check("Marketing hears the account it is connected to", done.length && done.at(-1).setup.username === "forkmesh");
  const saved = await rpc({ actor: "alice", method: "GET", path: "/api/marketing/acme/x-connection", body: null });
  check("Marketing has X connected", saved.data.x.connected && saved.data.x.username === "forkmesh");

  reply = await message({ type: "fm-x-setup-back" }, PORTAL);
  check("Back to Marketing focuses the Marketing tab and ends the setup", reply.ok
    && focused.some((item) => item.tabId === 3 && item.active) && !session.xSetup);

  actor = "alice";
  reply = await message({ type: "fm-x-setup-start", organization: "acme" }, DASHBOARD);
  reply = await message({ type: "fm-x-setup-stop" }, PORTAL);
  check("Stop ends the setup", reply.ok && !reply.setup.active && !session.xSetup);
  reply = await message({ type: "fm-x-setup-keys", keys: KEYS }, PORTAL);
  check("keys after Stop are refused", reply.error === "x_setup_expired");

  server.kill();
  console.log(`${checks} checks passed`);
})().catch((error) => { console.error(error); server.kill(); process.exit(1); });
