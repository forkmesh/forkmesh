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

const storage = { sessionToken: "token", account: "charlie", organization: "acme" };
const session = {};
const fetched = [];
let listener = null;
const noop = { addListener() {} };
const area = (store) => ({
  async get(keys) { return Object.fromEntries([keys].flat().filter((key) => key in store).map((key) => [key, JSON.parse(JSON.stringify(store[key]))])); },
  async set(values) { Object.assign(store, JSON.parse(JSON.stringify(values))); },
  async remove(keys) { for (const key of [keys].flat()) delete store[key]; },
});
const chrome = {
  runtime: {
    id: "ext", getURL: (suffix) => `chrome-extension://ext/${suffix}`,
    onMessage: { addListener(callback) { listener = callback; } },
    onInstalled: noop, onStartup: noop,
  },
  permissions: { onRemoved: noop, contains: async () => false },
  scripting: { getRegisteredContentScripts: async () => [], registerContentScripts: async () => {}, unregisterContentScripts: async () => {} },
  storage: { local: area(storage), session: area(session) },
  tabs: { async create() { return { id: 1 }; }, async get() { return null; }, async sendMessage() {} },
  alarms: { onAlarm: noop, async create() {}, async clear() {} },
};
const backgroundContext = {
  chrome, URL, URLSearchParams, console, Promise, crypto: globalThis.crypto, setTimeout, clearTimeout,
  importScripts() {},
  ForkMeshVaultBackground: { install: () => ({ forget: async () => {} }) },
  ForkMeshRepoBackground: { install: () => {} },
  ForkMeshForumBackground: { install: () => {} },
  async fetch(url, options) {
    const target = new URL(url);
    const body = options.body === undefined ? null : JSON.parse(options.body);
    fetched.push({ method: options.method, path: target.pathname, body });
    const reply = await rpc({ actor: "charlie", method: options.method, path: target.pathname + target.search, body });
    return { ok: reply.status < 400, status: reply.status, json: async () => reply.data };
  },
};
backgroundContext.self = backgroundContext;
vm.createContext(backgroundContext);
vm.runInContext(fs.readFileSync(path.join(EXTENSION, "background.js"), "utf8"), backgroundContext, { filename: "background.js" });

const MAIL_TAB = { id: "ext", url: "https://mail.zoho.com/zm/", tab: { id: 4 } };
function message(request, sender) {
  return new Promise((resolve) => {
    if (!listener(request, sender, resolve)) setImmediate(() => resolve(undefined));
  });
}

(async () => {
  await rpc({ actor: "alice", method: "POST", path: "/api/crm/acme/contacts",
    body: { name: "Ada Lovelace", email: "ada@engines.example" } });
  const pixel = await message({ type: "fm-track-pixel" }, MAIL_TAB);
  check("a pixel comes with a signed token and its image address",
    pixel.ok && /^[0-9a-f]{32}\.[0-9a-f]{24}$/.test(pixel.pixel.token)
      && pixel.pixel.url.endsWith(`/api/crm/-/t/o/${pixel.pixel.token}.png`), JSON.stringify(pixel));
  const sent = (token, email, messageId) => message({ type: "fm-track-sent", message: {
    provider: "zoho", messageId, mailbox: "charlie@acme.example", subject: "Engines quote", sentAt: 1_760_000_000_000,
    providerUrl: "https://mail.zoho.com/zm/#mail/folder/sent", trackToken: token,
    recipients: [{ email, name: "" }] } }, MAIL_TAB);
  const known = await sent(pixel.pixel.token, "ada@engines.example", "fm-sent-known");
  check("a send to a contact is linked under the pixel's id",
    known.tracked && known.email.id === pixel.pixel.token.split(".")[0], JSON.stringify(known));
  const other = await message({ type: "fm-track-pixel" }, MAIL_TAB);
  const unknown = await sent(other.pixel.token, "bob@elsewhere.example", "fm-sent-unknown");
  check("a send to someone the CRM does not know is still recorded for its opens",
    unknown.tracked && unknown.contact === null && unknown.email.id === other.pixel.token.split(".")[0], JSON.stringify(unknown));
  const plain = await sent("", "carol@elsewhere.example", "fm-sent-plain");
  check("a send without a pixel to someone unknown is not recorded", plain.ok && !plain.tracked, JSON.stringify(plain));
  await rpc({ control: "advance", ms: 60000 });
  await rpc({ actor: "nobody", method: "GET", path: `/api/crm/-/t/o/${pixel.pixel.token}.png` });
  await rpc({ actor: "nobody", method: "GET", path: `/api/crm/-/t/o/${other.pixel.token}.png` });
  const pings = (await rpc({ control: "pings" })).data.pings.filter((ping) => ping.recipient === "charlie").map((ping) => ping.title);
  check("each first open pings the sender", pings.includes("📬 Ada Lovelace opened your email")
    && pings.includes("📬 bob@elsewhere.example opened your email"), JSON.stringify(pings));
  storage.sentMail = { track: true, pixel: false };
  const off = await message({ type: "fm-track-pixel" }, MAIL_TAB);
  check("no pixel when the popup turns it off", off.ok && off.pixel === null, JSON.stringify(off));
  storage.sentMail = { track: false, pixel: true };
  check("no pixel when sent mail is not tracked", (await message({ type: "fm-track-pixel" }, MAIL_TAB)).pixel === null);
  console.log(`${checks} checks passed`);
  server.kill();
})().catch((error) => { console.error(error); server.kill(); process.exit(1); });
