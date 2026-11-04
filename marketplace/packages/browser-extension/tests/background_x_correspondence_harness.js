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
  if (!ok) { console.error(`FAIL ${name} ${String(detail).slice(0, 1500)}`); server.kill(); process.exit(1); }
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

const POPUP = { id: "ext", url: "chrome-extension://ext/popup.html" };
const X_TAB = { id: "ext", url: "https://x.com/forkmesh/status/1853482133370019840", tab: { id: 4, url: "https://x.com/forkmesh/status/1853482133370019840" } };
const MAIL_TAB = { id: "ext", url: "https://mail.google.com/mail/u/0/", tab: { id: 5, url: "https://mail.google.com/mail/u/0/" } };
function message(request, sender) {
  return new Promise((resolve) => {
    if (!listener(request, sender, resolve)) setImmediate(() => resolve(undefined));
  });
}

const post = (statusId, author, mine, people, extra = {}) => ({
  statusId, threadId: statusId, kind: "post", author: { handle: author, name: "" }, mine, people,
  subject: "Shipping the mesh today", sentAt: 1_760_000_000_000,
  providerUrl: `https://x.com/${author}/status/${statusId}`, ...extra,
});
const links = (from) => fetched.slice(from).filter((call) => call.path === "/api/crm/acme/emails/link");

(async () => {
  const ada = await rpc({ actor: "alice", method: "POST", path: "/api/crm/acme/contacts",
    body: { name: "Ada Lovelace", twitter: "https://x.com/AdaLovelace" } });
  check("a known contact exists with an X profile link", ada.status === 201);

  const mine = post("1853482133370019840", "forkmesh", "forkmesh", [
    { handle: "GraceHopper", name: "Grace Hopper" },
    { handle: "adalovelace", name: "" },
    { handle: "ForkMesh", name: "" },
    { handle: "not a handle", name: "" },
  ]);
  const logged = await message({ type: "fm-track-tweet", post: mine }, X_TAB);
  check("your own post logs everyone it mentions", logged.ok && logged.tracked && logged.logged === 2
    && logged.created === 1, JSON.stringify(logged));
  const sent = links(0);
  check("one link per person, the first by the post id and the next by post/handle",
    sent.map((call) => call.body.messageId).join() === "1853482133370019840,1853482133370019840/adalovelace"
    && sent.every((call) => call.body.provider === "x" && call.body.direction === "outbound"
      && call.body.createContact === true && call.body.account === "forkmesh"
      && call.body.providerUrl === "https://x.com/forkmesh/status/1853482133370019840"),
    JSON.stringify(sent.map((call) => call.body)));

  const contacts = await rpc({ actor: "alice", method: "GET", path: "/api/crm/acme/contacts" });
  const grace = contacts.data.contacts.find((contact) => contact.twitter === "@gracehopper") || {};
  check("a person the CRM did not know becomes a contact with their X handle",
    grace.name === "Grace Hopper" && !grace.mastodon && grace.source === "x conversation"
    && contacts.data.contacts.length === 2, JSON.stringify(contacts.data.contacts));

  const pings = (await rpc({ control: "pings" })).data.pings.filter((ping) => ping.recipient === "charlie");
  check("a ping for each person logged", pings.map((ping) => ping.title).join("|")
    === "🎉 New CRM contact from X|🎉 X post tracked in the CRM", JSON.stringify(pings));

  const lookedUp = await message({ type: "fm-lookup", provider: "x", handle: "@AdaLovelace", name: "" }, X_TAB);
  check("the header looks a contact up by X handle", lookedUp.ok && lookedUp.contact?.id === ada.data.contact.id
    && lookedUp.matchedBy === "handle", JSON.stringify(lookedUp));
  const adaPost = lookedUp.emails.find((email) => email.provider === "x") || {};
  check("the known contact was matched by the X profile on its record", adaPost.handle === "adalovelace"
    && adaPost.direction === "outbound" && adaPost.threadId === "1853482133370019840"
    && lookedUp.contact.twitter === "https://x.com/AdaLovelace", JSON.stringify(lookedUp.emails));

  const before = fetched.length;
  const again = await message({ type: "fm-track-tweet", post: mine }, X_TAB);
  check("the same post opened again sends nothing", again.ok && !again.tracked && again.seen
    && fetched.length === before);

  const dm = await message({ type: "fm-track-tweet", post: post("1853482133370019999", "forkmesh", "forkmesh",
    [{ handle: "linus", name: "Linus" }], {
      kind: "dm", threadId: "12-34", providerUrl: "https://x.com/messages/12-34", subject: "Hello from ForkMesh",
    }) }, X_TAB);
  const dmLink = links(before)[0]?.body || {};
  check("a direct message you send logs the person it went to", dm.ok && dm.tracked && dm.created === 1
    && dmLink.threadId === "12-34" && dmLink.providerUrl === "https://x.com/messages/12-34", JSON.stringify({ dm, dmLink }));

  const settings = await message({ type: "fm-tweet-settings" }, POPUP);
  check("the popup shows who was logged last", settings.track && settings.last.contacts.join() === "Linus"
    && settings.last.dm === true && settings.last.error === "", JSON.stringify(settings));

  const quiet = fetched.length;
  const theirs = await message({ type: "fm-track-tweet", post: post("1853482133370020001", "someone", "forkmesh",
    [{ handle: "gracehopper", name: "" }]) }, X_TAB);
  check("someone else's post is not logged", theirs.ok && !theirs.tracked && theirs.mine === false
    && fetched.length === quiet, JSON.stringify(theirs));

  const tracked = await message({ type: "fm-track-tweet", post: post("1853482133370020002", "forkmesh", "",
    [{ handle: "gracehopper", name: "" }]) }, X_TAB);
  check("signed out of X, the tracked Marketing account still makes the post yours", tracked.ok && tracked.tracked,
    JSON.stringify(tracked));

  const forged = await message({ type: "fm-track-tweet", post: post("1853482133370020003", "forkmesh", "forkmesh",
    [{ handle: "gracehopper", name: "" }], { providerUrl: "https://evil.example/forkmesh/status/1853482133370020003" }) }, X_TAB);
  check("a permalink off X is refused", forged.ok === false && forged.error === "invalid_post", JSON.stringify(forged));

  const elsewhere = await message({ type: "fm-track-tweet", post: post("1853482133370020004", "forkmesh", "forkmesh",
    [{ handle: "gracehopper", name: "" }]) }, MAIL_TAB);
  check("only a tab on X may log X posts", elsewhere.ok === false && elsewhere.error === "forbidden_sender",
    JSON.stringify(elsewhere));

  const off = await message({ type: "fm-tweet-set-track", track: false }, X_TAB);
  check("an X page cannot switch the logging", off.ok === false && off.error === "forbidden_sender");
  await message({ type: "fm-tweet-set-track", track: false }, POPUP);
  const paused = fetched.length;
  const skipped = await message({ type: "fm-track-tweet", post: post("1853482133370020005", "forkmesh", "forkmesh",
    [{ handle: "gracehopper", name: "" }]) }, X_TAB);
  check("switched off in the popup, nothing is sent", skipped.ok && !skipped.tracked && fetched.length === paused);

  server.kill();
  console.log(`${checks} checks passed`);
})().catch((error) => { console.error(error); server.kill(); process.exit(1); });
